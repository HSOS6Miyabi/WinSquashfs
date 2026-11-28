# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([ValidateSet('x64','arm64','x86')][string]$Architecture='x64',
      [string]$KitRoot,
      [string]$KitVersion,
      [string]$OutputDirectory,
      [string]$Altitude,
      [switch]$IncludeUnqualified)
$ErrorActionPreference='Stop'
if(!$OutputDirectory){$OutputDirectory=Join-Path $PSScriptRoot "../../artifacts/unc-filter-$Architecture"}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if($IncludeUnqualified -and !$Altitude){throw 'An allocated altitude is required to prepare an installable package'}
if($Altitude -and $Altitude -notmatch '^[0-9]{6}(\.[0-9]+)?$'){throw 'Supply the Microsoft-allocated filter altitude'}
if($Altitude -and ([decimal]::Parse($Altitude,[Globalization.CultureInfo]::InvariantCulture) -lt 360000 -or
                  [decimal]::Parse($Altitude,[Globalization.CultureInfo]::InvariantCulture) -ge 390000)){throw 'The allocated altitude must belong to FSFilter Activity Monitor'}
$cl=(Get-Command cl.exe -ErrorAction Stop).Source
$link=(Get-Command link.exe -ErrorAction Stop).Source
$roots=@()
if($KitRoot){
    $roots+= (Resolve-Path -LiteralPath $KitRoot).Path
}else{
    $kits=Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Windows Kits/10'
    if(Test-Path $kits){$roots+=$kits}
}
function Find-KitFile([string]$name,[string]$suffix){
    $items=@(foreach($root in $roots){Get-ChildItem -LiteralPath $root -Filter $name -Recurse -File -ErrorAction Stop |
        Where-Object { $_.FullName.EndsWith($suffix,[StringComparison]::OrdinalIgnoreCase) -and
            (!$KitVersion -or $_.FullName -like "*\$KitVersion\*") }})
    if(!$items){throw "WDK file not found: $suffix"}
    $items | Sort-Object FullName -Descending | Select-Object -First 1
}
$libs=(Find-KitFile ntoskrnl.lib "\km\$Architecture\ntoskrnl.lib").Directory.FullName
$KitVersion=(Get-Item $libs).Parent.Parent.Name
$km=(Find-KitFile fltKernel.h '\km\fltKernel.h').Directory.FullName
$shared=(Find-KitFile ntdef.h '\shared\ntdef.h').Directory.FullName
Write-Host "Building with WDK/SDK $KitVersion for $Architecture"
foreach($name in @('fltmgr.lib','hal.lib','BufferOverflowFastFailK.lib','libcntpr.lib')){
    if(!(Test-Path (Join-Path $libs $name))){throw "Missing WDK library: $name"}
}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$archDefine=@{x64='_AMD64_';arm64='_ARM64_';x86='_X86_'}[$Architecture]
$machine=@{x64='X64';arm64='ARM64';x86='X86'}[$Architecture]
$entry=if($Architecture -eq 'x86'){'GsDriverEntry@8'}else{'GsDriverEntry'}
$obj=Join-Path $OutputDirectory 'unc_filter.obj'
$driver=Join-Path $OutputDirectory 'ArchiveFsUncFilter.sys'
$include=@("/I$km","/I$shared")
if(Test-Path (Join-Path $km "crt")){$include+="/I$km/crt"}
$compile=@('/nologo','/c','/TC','/std:c17','/kernel','/GS','/Zl','/O2','/Oi','/W4','/WX',
    "/D$archDefine",'/DNTDDI_VERSION=0x0A000006','/D_WIN32_WINNT=0x0A00','/DPOOL_NX_OPTIN=1','/Gz',
    "/Fo$obj",(Join-Path $PSScriptRoot 'unc_filter.c'))+$include
& $cl @compile
if($LASTEXITCODE){exit $LASTEXITCODE}
& $link /nologo /driver /subsystem:native,10.0 /release /nodefaultlib /dynamicbase /nxcompat /integritycheck /opt:ref /opt:icf "/machine:$machine" "/entry:$entry" "/out:$driver" $obj (Join-Path $libs 'ntoskrnl.lib') (Join-Path $libs 'fltmgr.lib') (Join-Path $libs 'hal.lib') (Join-Path $libs 'BufferOverflowFastFailK.lib') (Join-Path $libs 'libcntpr.lib')
if($LASTEXITCODE){exit $LASTEXITCODE}
Remove-Item -LiteralPath $obj
if($Altitude){
    $arch=@{x64='amd64';arm64='arm64';x86='x86'}[$Architecture]
    $version=([xml](Get-Content (Join-Path $PSScriptRoot '../../Directory.Build.props') -Raw)).Project.PropertyGroup.Version
    $date=(Get-Date).ToUniversalTime().ToString('MM/dd/yyyy',[Globalization.CultureInfo]::InvariantCulture)
    $inf=(Get-Content (Join-Path $PSScriptRoot 'ArchiveFsUncFilter.inf.in') -Raw).
        Replace('@ARCH@',$arch).Replace('@ALTITUDE@',$Altitude).Replace('@DATE@',$date).
        Replace('@VERSION@',"$version.0").Replace('@UNQUALIFIED@',([int]$IncludeUnqualified.IsPresent).ToString())
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'ArchiveFsUncFilter.inf'),$inf,[Text.Encoding]::ASCII)
}
Write-Host "Built $driver; driver is unsigned and has not been loaded"
