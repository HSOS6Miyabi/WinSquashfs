# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$ArtifactDirectory,
      [Parameter(Mandatory=$true)][string]$OutputDirectory,
      [string]$MakeNsis='makensis.exe')
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$source=(Resolve-Path -LiteralPath $ArtifactDirectory).Path
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output){throw 'Choose a new signing output directory'}
if($output.StartsWith($source.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Signing output must be outside the unsigned distribution directory'}
$props=([xml](Get-Content (Join-Path $root 'Directory.Build.props') -Raw)).Project.PropertyGroup
$signScript=Join-Path $PSScriptRoot 'sign-authenticode.ps1'
$nsis=(Get-Command $MakeNsis -ErrorAction Stop).Source
$architectures=@('x64','x86','arm64')
$names=@('WinArchiveFSPro.exe','archivefs.exe','WinArchiveFSPro.Unc.exe','archivefs_core.dll',
         'winsqfs-native.dll','winsqfs-erofs.dll',"winsqfs-shell-$($props.Version).dll")
foreach($arch in $architectures){
    $directory=if($arch -eq 'x64'){'dist'}else{"dist-$arch"}
    $distribution=Join-Path $source $directory
    foreach($name in $names){
        if(!(Test-Path -LiteralPath (Join-Path $distribution $name) -PathType Leaf)){throw "Missing $arch file: $name"}
    }
}
New-Item -ItemType Directory -Path $output | Out-Null
$env:ARCHIVEFS_SIGN_SCRIPT=$signScript
$manifest=@()
try{
    foreach($arch in $architectures){
        $directory=if($arch -eq 'x64'){'dist'}else{"dist-$arch"}
        $dist=Join-Path $output $directory
        Copy-Item -LiteralPath (Join-Path $source $directory) -Destination $dist -Recurse
        foreach($name in $names){
            $file=Join-Path $dist $name
            & $signScript -Path $file
            $manifest+= [ordered]@{architecture=$arch;file="$directory/$name";sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant()}
        }
        $setup=Join-Path $output "WinArchiveFSPro-Setup-$arch.exe"
        & $nsis "/DPRODUCT_VERSION=$($props.Version)" "/DWINFSP_MIN_VERSION=$($props.WinFspRuntimeVersion).0" "/DPRODUCT_ARCH=$arch" "/DDIST_DIR=$dist" "/DSETUP_OUTPUT=$setup" '/DARCHIVEFS_SIGN_INSTALLER' (Join-Path $root 'installer/WinArchiveFSPro.nsi')
        if($LASTEXITCODE -ne 0){throw "Signed installer compilation failed for $arch"}
        & $signScript -Path $setup -VerifyOnly
        $manifest+= [ordered]@{architecture=$arch;file="WinArchiveFSPro-Setup-$arch.exe";sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $setup).Hash.ToLowerInvariant()}
    }
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'signed-files.json') -Encoding UTF8
    Write-Host 'PASS all three signed application distributions and installers'
}finally{
    Remove-Item Env:\ARCHIVEFS_SIGN_SCRIPT -ErrorAction SilentlyContinue
}
