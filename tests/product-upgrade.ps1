# SPDX-License-Identifier: GPL-3.0-only
param([Parameter(Mandatory=$true)][string]$Setup)
$ErrorActionPreference='Stop'
$Setup=(Resolve-Path $Setup).Path
$root=Join-Path $env:TEMP ('ArchiveFSUpgrade-'+[guid]::NewGuid().ToString('N'))
$oldSettings=Join-Path $env:LOCALAPPDATA 'WinSquashfs'
$newSettings=Join-Path $env:LOCALAPPDATA 'WinArchiveFSPro'
$legacy=Join-Path $root 'WinSquashfs-Setup-x64.exe'
$runKey='HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
New-Item -ItemType Directory -Force "$root\source",$oldSettings|Out-Null
try {
    Invoke-WebRequest 'https://github.com/HSOS6Miyabi/WinSquashfs/releases/download/v0.1.4/WinSquashfs-Setup-x64.exe' -OutFile $legacy
    if((Get-FileHash $legacy -Algorithm SHA256).Hash -ne '3a678785dd5e25b253d06456fb4c7d207b419feb4349dab5ad44767d6fad563c'){throw 'Previous installer hash mismatch'}
    $p=Start-Process $legacy -ArgumentList @('/S','/LANG=1033') -Wait -PassThru
    if($p.ExitCode){throw "Previous installer exit $($p.ExitCode)"}
    $old=(Get-ItemProperty 'HKLM:\Software\WinSquashfs').InstallDir
    if(!(Test-Path "$old\app\WinSquashfs.exe")){throw 'Previous product was not installed'}
    [IO.File]::WriteAllText("$oldSettings\unc.json",(@{Root=$root;Alias='upgrade-test';IncludeDrives=$false;IdleSeconds=47;MaxIdleImages=3;StartAtLogin=$true}|ConvertTo-Json -Compress))
    [IO.File]::WriteAllText("$oldSettings\language.txt",'zh-CN')
    $image=Join-Path $root 'migrated.squashfs'
    [IO.File]::WriteAllText("$root\source\hello.txt",'legacy product upgrade')
    [IO.File]::WriteAllText("$oldSettings\recent.json",(@($image)|ConvertTo-Json -AsArray -Compress))
    New-Item $runKey -Force|Out-Null
    Set-ItemProperty $runKey WinSquashfsUNC ('"'+$old+'\app\WinSquashfs.Unc.exe"')
    $p=Start-Process $Setup -ArgumentList @('/S','/LANG=1033') -Wait -PassThru
    if($p.ExitCode){throw "Upgrade installer exit $($p.ExitCode)"}
    $new=(Get-ItemProperty 'HKLM:\Software\WinArchiveFSPro').InstallDir
    $app=Join-Path $new 'app'
    if(Test-Path 'HKLM:\Software\WinSquashfs'){throw 'Previous product registration remains'}
    if(Test-Path 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\WinSquashfs'){throw 'Previous uninstall entry remains'}
    $cfg=Get-Content "$newSettings\unc.json" -Raw|ConvertFrom-Json
    if($cfg.Root -ne $root -or $cfg.Alias -ne 'upgrade-test' -or $cfg.IncludeDrives -or $cfg.IdleSeconds -ne 47 -or $cfg.MaxIdleImages -ne 3 -or !$cfg.StartAtLogin -or $cfg.Language -ne 'zh-CN'){throw 'UNC settings migration mismatch'}
    if(@(Get-Content "$newSettings\recent.json" -Raw|ConvertFrom-Json) -notcontains $image){throw 'Recent images were not migrated'}
    $login=Get-ItemProperty $runKey
    if($login.WinSquashfsUNC -or $login.WinArchiveFSProUNC -ne ('"'+$app+'\WinArchiveFSPro.Unc.exe"')){throw 'Startup command was not migrated'}
    $command=(Get-Item 'HKLM:\Software\Classes\WinSquashfs.Image\shell\open\command').GetValue('')
    if($command -ne ('"'+$app+'\WinArchiveFSPro.exe" --unc "%1"')){throw 'File association does not point to the new program'}
    & "$app\archivefs.exe" pack "$root\source" $image zstd
    if($LASTEXITCODE){throw 'Cannot create image after upgrade'}
    & "$app\archivefs.exe" unc-start
    if($LASTEXITCODE){throw 'Cannot start upgraded service'}
    if([IO.File]::ReadAllText('\\sfs$\upgrade-test\migrated.squashfs\hello.txt') -ne 'legacy product upgrade'){throw 'Migrated UNC share did not work'}
    & "$app\archivefs.exe" unc-stop
    if($LASTEXITCODE){throw 'Cannot stop upgraded service'}
    $p=Start-Process "$new\Uninstall.exe" -ArgumentList '/S' -Wait -PassThru
    if($p.ExitCode){throw "Uninstall exit $($p.ExitCode)"}
    if(Test-Path 'HKLM:\Software\WinArchiveFSPro'){throw 'New product registration remains after uninstall'}
    if((Get-ItemProperty $runKey).WinArchiveFSProUNC){throw 'New startup command remains after uninstall'}
    Write-Host 'PASS upgrade from v0.1.4, preferences, recent images, login startup, associations, UNC access and uninstall'
} finally {
    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
}
