param(
    [Parameter(Mandatory=$true)][ValidateSet('clean','upgrade')][string]$Scenario,
    [Parameter(Mandatory=$true)][string]$Distribution,
    [Parameter(Mandatory=$true)][string]$Setup
)
$ErrorActionPreference='Stop'
$versions=([xml](Get-Content (Join-Path (Split-Path -Parent $PSScriptRoot) 'Directory.Build.props') -Raw)).Project.PropertyGroup
$required=[version]($versions.WinFspRuntimeVersion+'.0')
$Distribution=(Resolve-Path $Distribution).Path
$Setup=(Resolve-Path $Setup).Path
$cli=Join-Path $Distribution 'winsqfs.exe'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
function Get-Runtime {
    $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry32)
    try {
        $key=$base.OpenSubKey('SOFTWARE\WinFsp')
        if(!$key){return $null}
        try {$dir=$key.GetValue('InstallDir')}finally{$key.Dispose()}
        if(!$dir){return $null}
        $dll=Join-Path $dir 'bin\winfsp-x64.dll'
        if(!(Test-Path -LiteralPath $dll)){return $null}
        $f=[Diagnostics.FileVersionInfo]::GetVersionInfo($dll)
        [pscustomobject]@{Path=$dll;Version=[version]::new($f.FileMajorPart,$f.FileMinorPart,$f.FileBuildPart,$f.FilePrivatePart)}
    } finally {$base.Dispose()}
}
function Invoke-Msi([string[]]$Arguments) {
    $p=Start-Process msiexec.exe -ArgumentList $Arguments -Wait -PassThru
    if($p.ExitCode -ne 0){throw "MSI exited with $($p.ExitCode): $($Arguments -join ' ')"}
}
function Invoke-ExpectedFailure([string[]]$Arguments) {
    $si=[Diagnostics.ProcessStartInfo]::new($cli)
    $si.UseShellExecute=$false
    $si.CreateNoWindow=$true
    $si.RedirectStandardOutput=$true
    $si.RedirectStandardError=$true
    $si.StandardErrorEncoding=[Text.Encoding]::UTF8
    foreach($a in $Arguments){$si.ArgumentList.Add($a)}
    $p=[Diagnostics.Process]::Start($si)
    try {
        $stdout=$p.StandardOutput.ReadToEndAsync()
        $stderr=$p.StandardError.ReadToEndAsync()
        if(!$p.WaitForExit(30000)){$p.Kill($true);throw "Dependency check timed out: $Arguments"}
        $message=$stderr.GetAwaiter().GetResult()
        $null=$stdout.GetAwaiter().GetResult()
        if($p.ExitCode -ne 1 -or $message -notmatch 'WinFsp could not initialize' -or $message -match "The type initializer for 'Fsp.Interop.Api'"){
            throw "Wrong dependency error ($($p.ExitCode)): $message"
        }
        Write-Host $message
    } finally {$p.Dispose()}
}

# This job does not build the application or install an SDK. Remove any
# WinFsp product on the runner image before exercising installation.
foreach($view in @('Registry32','Registry64')){
    $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]$view)
    try {
        $uninstall=$base.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall')
        if(!$uninstall){continue}
        try {
            foreach($id in $uninstall.GetSubKeyNames()){
                if($id -notmatch '^\{[A-Fa-f0-9-]+\}$'){continue}
                $entry=$uninstall.OpenSubKey($id)
                if(!$entry){continue}
                try {$name=$entry.GetValue('DisplayName')}finally{$entry.Dispose()}
                if($name -match '^WinFsp(?:\s|$)'){Invoke-Msi @('/x',$id,'/qn','/norestart')}
            }
        } finally {$uninstall.Dispose()}
    } finally {$base.Dispose()}
}
if(Get-Runtime){throw 'Runner still has WinFsp installed'}
Write-Host 'Verified absence of registered WinFsp runtime'
if($Scenario -eq 'upgrade'){
    $old=Join-Path $env:TEMP 'winfsp-2.1.25156.msi'
    Invoke-WebRequest 'https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi' -OutFile $old
    if((Get-FileHash $old -Algorithm SHA256).Hash -ne '073A70E00F77423E34BED98B86E600DEF93393BA5822204FAC57A29324DB9F7A'){throw 'Legacy MSI hash mismatch'}
    Invoke-Msi @('/i',('"' + $old + '"'),'/qn','/norestart')
    $prior=Get-Runtime
    if(!$prior -or $prior.Version.Major -ne 2 -or $prior.Version.Minor -ne 1){throw "Legacy runtime was not installed: $prior"}
    Write-Host "Verified existing incompatible WinFsp $($prior.Version)"
}

$testRoot=Join-Path $env:TEMP ('WinSquashfsDependency-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$testRoot\source"|Out-Null
Set-Content "$testRoot\source\hello.txt" 'Dependency check' -NoNewline
$image="$testRoot\test.squashfs"
try {
    & $cli pack "$testRoot\source" $image zstd
    if($LASTEXITCODE){throw 'Packing without compatible WinFsp failed'}
    & $cli verify $image
    if($LASTEXITCODE){throw 'Reading without compatible WinFsp failed'}
    Invoke-ExpectedFailure @('doctor')
    Invoke-ExpectedFailure @('mount',$image)
    Invoke-ExpectedFailure @('unc-start')

    $lang=if($Scenario -eq 'upgrade'){'2052'}else{'1033'}
    $p=Start-Process $Setup -ArgumentList @('/S',"/LANG=$lang") -Wait -PassThru
    if($p.ExitCode -ne 0){throw "WinSquashfs setup returned $($p.ExitCode). Log: $env:TEMP\WinSquashfs-WinFsp-install.log"}
    $installed=Get-Runtime
    if(!$installed -or $installed.Version.Major -ne $required.Major -or $installed.Version -lt $required){throw "Setup did not install the required WinFsp runtime: $installed"}
    Write-Host "PASS $Scenario installed WinFsp $($installed.Version) from the product installer"
    $app=Join-Path $env:ProgramFiles 'WinSquashfs\app'
    . (Join-Path $PSScriptRoot 'icons.ps1')
    Assert-InstalledIcons -AppDirectory $app -Setup $Setup
    $installedCli=Join-Path $app 'winsqfs.exe'
    & $installedCli doctor
    if($LASTEXITCODE){throw 'Installed native library cannot initialize'}
    & (Join-Path $PSScriptRoot 'distribution.ps1') -AppDirectory $app -Version $versions.Version -ShellEject
    Write-Host "PASS installed product runtime and Explorer eject ($Scenario)"
}
finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
