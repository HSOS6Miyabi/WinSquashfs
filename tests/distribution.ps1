param(
    [Parameter(Mandatory=$true)][string]$AppDirectory,
    [Parameter(Mandatory=$true)][string]$Version
)
$ErrorActionPreference='Stop'
$AppDirectory=(Resolve-Path $AppDirectory).Path
$cli=Join-Path $AppDirectory 'winsqfs.exe'
[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false)
function Invoke-Cli {
    param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Arguments)
    $result=& $cli @Arguments
    if($LASTEXITCODE -ne 0){throw "winsqfs $($Arguments -join ' ') exited with $LASTEXITCODE"}
    $result
}
foreach($exe in @('WinSquashfs.exe','winsqfs.exe','WinSquashfs.Unc.exe')){
    $file=Join-Path $AppDirectory $exe
    if(!(Test-Path $file)){throw "Missing $file"}
    $actual=[Diagnostics.FileVersionInfo]::GetVersionInfo($file).FileVersion
    if($actual -ne "$Version.0"){throw "Unexpected file version of $($exe): $actual"}
}
if((Invoke-Cli --version) -ne $Version){throw 'CLI version mismatch'}
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
    Invoke-Cli pack $source $image zstd|Out-Null
    $listing=Invoke-Cli list $image
    if($listing -notmatch '测试\.txt'){throw "Unicode list output is incorrect: $listing"}
    if((Invoke-Cli read $image '\测试.txt' 0 5) -ne '0001027FFF'){throw 'CLI read mismatch'}
    $null=Invoke-Cli verify $image
    $destination=Join-Path $root '提取文件'
    Invoke-Cli unpack $image $destination|Out-Null
    if([Convert]::ToHexString([IO.File]::ReadAllBytes((Join-Path $destination '测试.txt'))) -ne '0001027FFF'){throw 'CLI extraction mismatch'}
    & $cli list $image '\' 'extra' 2>$null | Out-Null
    if($LASTEXITCODE -ne 2){throw 'Invalid list arguments accepted'}
    & $cli pack $source (Join-Path $root 'extra.squashfs') zstd 'extra' 2>$null | Out-Null
    if($LASTEXITCODE -ne 2){throw 'Invalid pack arguments accepted'}

    # Run the same background process under each language and control it from the other.
    $alias='dist-'+[guid]::NewGuid().ToString('N').Substring(0,8)
    $settings=@{Root=$root;Alias=$alias;IncludeDrives=$false;IdleSeconds=0;MaxIdleImages=0;StartAtLogin=$false}
    [IO.File]::WriteAllText($settingsFile,($settings|ConvertTo-Json -Compress))
    $unc='\\sfs$\'+$alias+'\测试镜像.squashfs\测试.txt'
    foreach($language in @('zh-CN','en-US')){
        $other=if($language -eq 'zh-CN'){'en-US'}else{'zh-CN'}
        [IO.File]::WriteAllText($languageFile,$language)
        $agentStarted=$true
        Invoke-Cli --lang $other unc-start|Out-Null
        if((Invoke-Cli --lang en-US unc-status) -ne 'Running'){throw 'English status did not detect running agent'}
        if((Invoke-Cli --lang zh-CN unc-status) -ne '运行中'){throw 'Chinese status did not detect running agent'}
        if([Convert]::ToHexString([IO.File]::ReadAllBytes($unc)) -ne '0001027FFF'){throw 'UNC Unicode read mismatch'}
        # Change the preference while the agent is alive, as happens when the GUI language changes.
        [IO.File]::WriteAllText($languageFile,$other)
        Invoke-Cli --lang $other unc-stop|Out-Null
        $agentStarted=$false
        if((Invoke-Cli --lang en-US unc-status) -ne 'Stopped' -or [IO.File]::Exists($unc)){throw 'Agent did not stop after language change'}
    }
    Write-Host "PASS packaged applications $Version, Unicode paths, CLI validation and cross-language UNC lifecycle"
}
finally {
    if($agentStarted){& $cli --lang en-US unc-stop|Out-Null}
    foreach($file in @($settingsFile,$languageFile)){
        if($backup.ContainsKey($file)){[IO.File]::WriteAllBytes($file,$backup[$file])}
        elseif(Test-Path $file){Remove-Item -LiteralPath $file -Force}
    }
    if(Test-Path $root){Remove-Item -LiteralPath $root -Recurse -Force}
}
