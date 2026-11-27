$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root 'artifacts'
New-Item -ItemType Directory -Force $dir | Out-Null
$msi = Join-Path $dir 'winfsp.msi'
$versions=([xml](Get-Content (Join-Path $root 'Directory.Build.props') -Raw)).Project.PropertyGroup
$url="https://github.com/winfsp/winfsp/releases/download/$($versions.WinFspRelease)/winfsp-$($versions.WinFspRuntimeVersion).msi"
Invoke-WebRequest $url -OutFile $msi
if ((Get-FileHash $msi -Algorithm SHA256).Hash -ne $versions.WinFspMsiSha256) { throw "WinFsp installer hash mismatch" }
$p = Start-Process msiexec.exe -ArgumentList @('/i',('"' + $msi + '"'),'/qn','/norestart','ADDLOCAL=ALL') -Wait -PassThru
if ($p.ExitCode -notin 0,3010) { throw "WinFsp install returned $($p.ExitCode)" }
$machine=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry32)
try {
    $key=$machine.OpenSubKey('SOFTWARE\WinFsp')
    if(!$key){throw 'WinFsp installation is not registered'}
    try {$installDir=$key.GetValue('InstallDir')}finally{$key.Dispose()}
} finally {$machine.Dispose()}
$dll=Join-Path $installDir 'bin\winfsp-x64.dll'
$info=[Diagnostics.FileVersionInfo]::GetVersionInfo($dll)
$installed=[version]::new($info.FileMajorPart,$info.FileMinorPart,$info.FileBuildPart,$info.FilePrivatePart)
$required=[version]($versions.WinFspRuntimeVersion + '.0')
if($installed.Major -ne $required.Major -or $installed -lt $required){throw "WinFsp $installed is incompatible; required $required"}
Write-Host "WinFsp runtime: $installed at $dll"

& (Join-Path $PSScriptRoot 'prepare-winfsp-binding.ps1')
