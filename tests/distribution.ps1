param(
    [Parameter(Mandatory=$true)][string]$AppDirectory,
    [Parameter(Mandatory=$true)][string]$Version,
    [switch]$ShellEject
)
$ErrorActionPreference='Stop'
$AppDirectory=(Resolve-Path $AppDirectory).Path
$cli=Join-Path $AppDirectory 'winsqfs.exe'
[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false)
function Invoke-CliExpect {
    $expected=[int]$args[0]
    $arguments=@($args | Select-Object -Skip 1)
    $si=[Diagnostics.ProcessStartInfo]::new($cli)
    $si.UseShellExecute=$false
    $si.CreateNoWindow=$true
    $si.RedirectStandardOutput=$true
    $si.RedirectStandardError=$true
    $si.StandardOutputEncoding=[Text.Encoding]::UTF8
    $si.StandardErrorEncoding=[Text.Encoding]::UTF8
    foreach($argument in $arguments){$si.ArgumentList.Add([string]$argument)}
    $p=[Diagnostics.Process]::Start($si)
    try {
        $stdout=$p.StandardOutput.ReadToEndAsync()
        $stderr=$p.StandardError.ReadToEndAsync()
        if(!$p.WaitForExit(30000)){
            $p.Kill($true)
            throw "CLI timeout: $($arguments -join ' ')"
        }
        $result=$stdout.WaitAsync([TimeSpan]::FromSeconds(10)).GetAwaiter().GetResult()
        $errorText=$stderr.WaitAsync([TimeSpan]::FromSeconds(10)).GetAwaiter().GetResult()
        if($p.ExitCode -ne $expected){throw "Expected CLI exit $expected, got $($p.ExitCode): $($arguments -join ' '): $errorText"}
        $result -split '\r?\n' | Where-Object {$_ -ne ''}
    }
    finally {$p.Dispose()}
}
function Invoke-Cli { Invoke-CliExpect 0 @args }
foreach($exe in @('WinSquashfs.exe','winsqfs.exe','WinSquashfs.Unc.exe')){
    $file=Join-Path $AppDirectory $exe
    if(!(Test-Path $file)){throw "Missing $file"}
    $actual=[Diagnostics.FileVersionInfo]::GetVersionInfo($file).FileVersion
    if($actual -ne "$Version.0"){throw "Unexpected file version of $($exe): $actual"}
}
if((Invoke-Cli --version) -ne $Version){throw 'CLI version mismatch'}
$doctor=Invoke-Cli doctor
if(($doctor -join "`n") -notmatch 'WinFsp runtime 2\.'){throw "WinFsp diagnostic failed: $doctor"}
$root=Join-Path $env:TEMP ('WinSquashfsDist-'+[guid]::NewGuid().ToString('N'))
$settingsDirectory=Join-Path $env:LOCALAPPDATA 'WinSquashfs'
$settingsFile=Join-Path $settingsDirectory 'unc.json'
$languageFile=Join-Path $settingsDirectory 'language.txt'
$backup=@{}
foreach($file in @($settingsFile,$languageFile)){
    if(Test-Path $file){$backup[$file]=[IO.File]::ReadAllBytes($file)}
}
$agentStarted=$false
try {
    $source=Join-Path $root '源文件'
    New-Item -ItemType Directory -Force $source,$settingsDirectory|Out-Null
    [IO.File]::WriteAllBytes((Join-Path $source '测试.txt'),[byte[]](0,1,2,127,255))
    $image=Join-Path $root '测试镜像.squashfs'
    Write-Host "Testing Unicode image creation and CLI output"
    Invoke-Cli pack $source $image zstd|Out-Null
    $listing=Invoke-Cli list $image
    if($listing -notmatch '测试\.txt'){throw "Unicode list output is incorrect: $listing"}
    if((Invoke-Cli read $image '\测试.txt' 0 5) -ne '0001027FFF'){throw 'CLI read mismatch'}
    $null=Invoke-Cli verify $image
    $destination=Join-Path $root '提取文件'
    Invoke-Cli unpack $image $destination|Out-Null
    if([Convert]::ToHexString([IO.File]::ReadAllBytes((Join-Path $destination '测试.txt'))) -ne '0001027FFF'){throw 'CLI extraction mismatch'}
    Invoke-CliExpect 2 list $image '\' 'extra' | Out-Null
    Invoke-CliExpect 2 pack $source (Join-Path $root 'extra.squashfs') zstd 'extra' | Out-Null

    # Mount the actual packaged CLI and read through the Windows volume.
    $start=[Diagnostics.ProcessStartInfo]::new($cli)
    $start.UseShellExecute=$false
    $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true
    $start.RedirectStandardError=$true
    $start.ArgumentList.Add('mount')
    $start.ArgumentList.Add($image)
    $mount=[Diagnostics.Process]::Start($start)
    try {
        $drive=$mount.StandardOutput.ReadLineAsync().WaitAsync([TimeSpan]::FromSeconds(20)).GetAwaiter().GetResult()
        if($drive -notmatch '^[A-Za-z]:\\?$'){
            if($mount.HasExited){throw "Mount failed: $($mount.StandardError.ReadToEnd())"}
            throw "Invalid mount point: $drive"
        }
        $drive=$drive.TrimEnd('\')
        $driveRoot=$drive+'\'
        if([IO.DriveInfo]::new($driveRoot).DriveFormat -ne 'SquashFS'){throw 'Unexpected filesystem name'}
        if([Convert]::ToHexString([IO.File]::ReadAllBytes($driveRoot+'测试.txt')) -ne '0001027FFF'){throw 'Packaged mount read mismatch'}
        if($ShellEject) {
            $shell=New-Object -ComObject Shell.Application
            $computer=$shell.NameSpace(17)
            $item=$null
            for($i=0;$i -lt 30 -and !$item;$i++){
                foreach($candidate in $computer.Items()){
                    if($candidate.Path.TrimEnd('\') -eq $drive){$item=$candidate;break}
                }
                if(!$item){Start-Sleep -Milliseconds 200}
            }
            if(!$item){throw "Explorer does not show mounted drive $drive"}
            $verb=$null
            $names=@()
            foreach($candidate in $item.Verbs()){
                $name=$candidate.Name.Replace('&','').Trim()
                $names+=$name
                if($name -in @('Eject (WinSquashfs)','弹出（WinSquashfs）')){$verb=$candidate}
            }
            if(!$verb){throw "Eject menu missing on $drive; shell filesystem: $($item.ExtendedProperty('System.Volume.FileSystem')); verbs: $($names -join '; ')"}
            foreach($candidate in $computer.Items()){
                if($candidate.Path.TrimEnd('\') -eq $env:SystemDrive){
                    foreach($v in $candidate.Verbs()){
                        if($v.Name -match 'WinSquashfs'){throw 'SquashFS eject menu appears on the system drive'}
                    }
                }
            }
            Write-Host "PASS Explorer menu: $($verb.Name)"
            $verb.DoIt()
        } else {
            Invoke-Cli eject $drive | Out-Null
        }
        if(!$mount.WaitForExit(20000)){throw 'Ejected mount process did not exit'}
        if($mount.ExitCode -ne 0){throw "Ejected mount failed: $($mount.StandardError.ReadToEnd())"}
        if([IO.Directory]::Exists($driveRoot)){throw 'Eject did not release the drive'}
        Write-Host 'PASS actual drive mount, byte read and eject'
    } finally {
        if(!$mount.HasExited){$mount.Kill($true);$mount.WaitForExit()}
        $mount.Dispose()
    }

    # Run the same background process under each language and control it from the other.
    $alias='dist-'+[guid]::NewGuid().ToString('N').Substring(0,8)
    $settings=@{Root=$root;Alias=$alias;IncludeDrives=$false;IdleSeconds=0;MaxIdleImages=0;StartAtLogin=$false}
    [IO.File]::WriteAllText($settingsFile,($settings|ConvertTo-Json -Compress))
    $unc='\\sfs$\'+$alias+'\测试镜像.squashfs\测试.txt'
    foreach($language in @('zh-CN','en-US')){
        $other=if($language -eq 'zh-CN'){'en-US'}else{'zh-CN'}
        [IO.File]::WriteAllText($languageFile,$language)
        $agentStarted=$true
        Write-Host "Starting $language agent from $other CLI with redirected output"
        Invoke-Cli --lang $other unc-start|Out-Null
        Write-Host "Agent started; checking both languages and UNC data"
        if((Invoke-Cli --lang en-US unc-status) -ne 'Running'){throw 'English status did not detect running agent'}
        if((Invoke-Cli --lang zh-CN unc-status) -ne '运行中'){throw 'Chinese status did not detect running agent'}
        if([Convert]::ToHexString([IO.File]::ReadAllBytes($unc)) -ne '0001027FFF'){throw 'UNC Unicode read mismatch'}
        # Change the preference while the agent is alive, as happens when the GUI language changes.
        [IO.File]::WriteAllText($languageFile,$other)
        Write-Host "Stopping agent after language preference change"
        Invoke-Cli --lang $other unc-stop|Out-Null
        $agentStarted=$false
        if((Invoke-Cli --lang en-US unc-status) -ne 'Stopped' -or [IO.File]::Exists($unc)){throw 'Agent did not stop after language change'}
    }
    Write-Host "PASS packaged applications $Version, Unicode paths, CLI validation and cross-language UNC lifecycle"
}
finally {
    if($agentStarted){try {Invoke-Cli --lang en-US unc-stop|Out-Null}catch{Write-Warning $_.Exception.Message}}
    foreach($file in @($settingsFile,$languageFile)){
        if($backup.ContainsKey($file)){[IO.File]::WriteAllBytes($file,$backup[$file])}
        elseif(Test-Path $file){Remove-Item -LiteralPath $file -Force}
    }
    if(Test-Path $root){Remove-Item -LiteralPath $root -Recurse -Force}
}
