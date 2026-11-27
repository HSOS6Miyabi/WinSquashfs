# SPDX-License-Identifier: GPL-3.0-only
param([Parameter(Mandatory=$true)][string]$AppDirectory)
$ErrorActionPreference='Stop'
$AppDirectory=(Resolve-Path $AppDirectory).Path
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes

$prog='HKLM:\Software\Classes\WinSquashfs.Image\shell'
$open=(Get-Item "$prog\open\command").GetValue('')
$edit=(Get-Item "$prog\edit\command").GetValue('')
if((Get-Item $prog).GetValue('') -ne 'open' -or $open -ne ('"'+$AppDirectory+'\WinSquashfs.exe" --unc "%1"') -or
    $edit -ne ('"'+$AppDirectory+'\WinSquashfs.exe" --open "%1"')){throw "Incorrect shell association: $open / $edit"}
$cli=Join-Path $AppDirectory 'winsqfs.exe'
$root=Join-Path $env:TEMP ('WinSquashfsAssociation-'+[guid]::NewGuid().ToString('N'))
$shareName='WinSquashfsAssoc-'+[guid]::NewGuid().ToString('N').Substring(0,8)
$settings=Join-Path $env:LOCALAPPDATA 'WinSquashfs\unc.json'
$oldSettings=if(Test-Path $settings){[IO.File]::ReadAllBytes($settings)}else{$null}
$alias='assoc-'+[guid]::NewGuid().ToString('N').Substring(0,8)
$server=$null
$gui=$null
$opened=@()
$shell=New-Object -ComObject Shell.Application
function Find-ExplorerPath([string]$target){
    foreach($window in $shell.Windows()){
        try {
            if($window.Document.Folder.Self.Path.TrimEnd('\') -eq $target.TrimEnd('\')){return $window}
        } catch {}
    }
    return $null
}
function Open-AssociatedImage([string]$path,[string]$projected){
    $shell.ShellExecute($path,'','','open',1)
    $window=$null
    for($i=0;$i -lt 120 -and !$window;$i++){
        $window=Find-ExplorerPath $projected
        if(!$window){Start-Sleep -Milliseconds 250}
    }
    if(!$window){throw "Explorer did not navigate to $projected"}
    if([IO.File]::ReadAllText($projected+'\payload.txt') -ne 'association test'){throw 'Associated image read failed'}
    Write-Host "PASS default open: $projected"
    return $window
}
try {
    New-Item -ItemType Directory -Force "$root\source",(Split-Path -Parent $settings)|Out-Null
    [IO.File]::WriteAllText("$root\source\payload.txt",'association test')
    $image="$root\local image.squashfs"
    & $cli unc-stop
    if($LASTEXITCODE){throw 'Cannot stop prior UNC agent'}
    $configuration=@{Root=$root;Alias=$alias;IncludeDrives=$false;IdleSeconds=60;MaxIdleImages=8;StartAtLogin=$false}
    [IO.File]::WriteAllText($settings,($configuration|ConvertTo-Json -Compress))
    & $cli pack "$root\source" $image zstd
    if($LASTEXITCODE){throw 'Cannot create association test image'}
    $projected='\\sfs$\'+$alias+'\local image.squashfs'
    $opened+=Open-AssociatedImage $image $projected

    $account=[Security.Principal.WindowsIdentity]::GetCurrent().Name
    New-SmbShare -Name $shareName -Path $root -FullAccess $account -CachingMode None | Out-Null
    $server=$shareName
    Copy-Item -LiteralPath $image -Destination "$root\远程镜像.sqfs"
    $remote='\\localhost\'+$shareName+'\远程镜像.sqfs'
    $remoteProjected='\\sfs$\UNC\localhost\'+$shareName+'\远程镜像.sqfs'
    $opened+=Open-AssociatedImage $remote $remoteProjected

    $existing=@(Get-Process WinSquashfs -ErrorAction SilentlyContinue|ForEach-Object Id)
    $shell.ShellExecute($image,'','','edit',1)
    $rootElement=[System.Windows.Automation.AutomationElement]::RootElement
    $mountCondition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,'MountImage')
    $ready=$false
    for($i=0;$i -lt 120 -and !$ready;$i++){
        foreach($process in @(Get-Process WinSquashfs -ErrorAction SilentlyContinue|Where-Object {$_.Id -notin $existing})){
            $condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty,$process.Id)
            $window=$rootElement.FindFirst([System.Windows.Automation.TreeScope]::Children,$condition)
            if($window){
                $mount=$window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$mountCondition)
                if($mount -and $mount.Current.IsEnabled){$gui=$process;$ready=$true;break}
            }
        }
        if(!$ready){Start-Sleep -Milliseconds 250}
    }
    if(!$ready){throw 'Edit verb did not open a populated WinSquashfs browser'}
    Write-Host 'PASS edit opens the graphical image browser'

    # The source is open in the GUI and the UNC namespace. Removing it must
    # invalidate both views and leave the independent network source usable.
    [IO.File]::Delete($image)
    if([IO.File]::Exists($image)){throw 'Associated local image remains locked'}
    for($i=0;$i -lt 40;$i++){
        if(!$mount.Current.IsEnabled -and ![IO.Directory]::Exists($projected)){break}
        Start-Sleep -Milliseconds 250
    }
    if($mount.Current.IsEnabled -or [IO.Directory]::Exists($projected)){throw 'Deleted image was not removed from both views'}
    if([IO.File]::ReadAllText($remoteProjected+'\payload.txt') -ne 'association test'){throw 'Deleting local image invalidated network image'}
    [IO.File]::Delete($remote)
    if([IO.File]::Exists($remote)){throw 'Associated remote image remains locked'}
    for($i=0;$i -lt 40 -and [IO.Directory]::Exists($remoteProjected);$i++){Start-Sleep -Milliseconds 250}
    if([IO.Directory]::Exists($remoteProjected)){throw 'Deleted remote source remains visible'}
    Write-Host 'PASS installed open/edit verbs, local and remote Explorer navigation and source deletion'
}
finally {
    foreach($w in $opened){try {$w.Quit()}catch{}}
    if($gui -and !$gui.HasExited){
        $gui.CloseMainWindow()|Out-Null
        if(!$gui.WaitForExit(10000)){$gui.Kill();$gui.WaitForExit()}
    }
    & $cli unc-stop
    if($LASTEXITCODE){throw 'UNC agent failed to stop during cleanup'}
    if($null -ne $oldSettings){[IO.File]::WriteAllBytes($settings,$oldSettings)}
    elseif(Test-Path $settings){Remove-Item -LiteralPath $settings -Force}
    if($server){Remove-SmbShare -Name $server -Force -Confirm:$false}
    if(Test-Path $root){Remove-Item -LiteralPath $root -Recurse -Force}
}
