# SPDX-License-Identifier: GPL-3.0-only
param([Parameter(Mandatory=$true)][ValidateSet('x86','arm64')][string]$Architecture)
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/../../..").Path
Set-Location $root
$platform=@{x86='Win32';arm64='ARM64'}[$Architecture]
$dist=Join-Path $root "artifacts/dist-$Architecture"
$readers=Join-Path $root "artifacts/readers-$Architecture"
$version=([xml](Get-Content Directory.Build.props -Raw)).Project.PropertyGroup.Version
if(Test-Path $dist){Remove-Item $dist -Recurse -Force}
New-Item -ItemType Directory -Force $dist,"$dist/Assets","$dist/prerequisites"|Out-Null
Copy-Item "$readers/*" $dist -Recurse -Force
Copy-Item 'src/App/Assets/app-icon.ico','src/App/Assets/file-icon.ico' "$dist/Assets"
Copy-Item LICENSE,README.md,README.zh-CN.md,THIRD-PARTY.md $dist
Copy-Item artifacts/winfsp.msi "$dist/prerequisites/WinFsp.msi"
Copy-Item build/production/winsqfs-shell.dll "$dist/winsqfs-shell-$version.dll"
$core=(Resolve-Path build/production/archivefs_core.lib).Path
msbuild native/cpp/gui/WinArchiveFSPro.vcxproj /restore /m /p:Configuration=Release "/p:Platform=$platform" "/p:ArchiveFSCoreLib=$core" "/p:OutDir=$dist/" /p:WindowsAppSDKSelfContained=true
if($LASTEXITCODE){exit $LASTEXITCODE}
$expected=@{x86=0x014c;arm64=0xaa64}[$Architecture]
foreach($name in @('WinArchiveFSPro.exe','archivefs.exe','WinArchiveFSPro.Unc.exe','archivefs_core.dll',"winsqfs-shell-$version.dll",'winsqfs-native.dll','winsqfs-erofs.dll','tools/archivefs-tar2sqfs.exe','tools/mkdwarfs.exe','tools/mkfs.erofs.exe')){
    $file=Join-Path $dist $name
    $stream=[IO.File]::OpenRead($file)
    try{
        $reader=[IO.BinaryReader]::new($stream)
        if($reader.ReadUInt16() -ne 0x5a4d){throw "Invalid executable: $name"}
        $stream.Position=60
        $stream.Position=$reader.ReadUInt32()
        if($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne $expected){throw "Incorrect native architecture: $name"}
    }finally{$stream.Dispose()}
}
Write-Host "Packaged native $Architecture application, Explorer extension, decoders, creation tools, assets, and prerequisites."
