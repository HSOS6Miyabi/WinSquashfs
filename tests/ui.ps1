param([Parameter(Mandatory=$true)][string]$AppDirectory)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
. (Join-Path $PSScriptRoot 'screenshots.ps1')
function Find-Control($parent,[string]$id) {
  $condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,$id)
  $parent.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$condition)
}
function Wait-Control($parent,[string]$id,[bool]$enabled=$false) {
  for ($i=0;$i -lt 120;$i++) {
    $element=Find-Control $parent $id
    if ($element -and (!$enabled -or $element.Current.IsEnabled)) { return $element }
    Start-Sleep -Milliseconds 250
  }
  throw "UI control not found or disabled: $id"
}
function Invoke-Control($parent,[string]$id) {
  $element=Wait-Control $parent $id $true
  $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
function Set-Value($parent,[string]$id,[string]$value) {
  $element=Wait-Control $parent $id $true
  $element.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($value)
}
function Assert-Visible($window,[string]$id) {
  $control=Wait-Control $window $id
  $r=$control.Current.BoundingRectangle
  $w=$window.Current.BoundingRectangle
  if($control.Current.IsOffscreen -or $r.IsEmpty -or $r.Top -lt $w.Top -or $r.Bottom -gt ($w.Bottom+1) -or $r.Left -lt $w.Left -or $r.Right -gt ($w.Right+1)){
    throw "Control is not completely visible: $id ($r) within ($w)"
  }
}
function Scroll-Page($window,[string]$id,[double]$percent) {
  $element=Wait-Control $window $id
  $pattern=$null
  if($element.TryGetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern,[ref]$pattern) -and $pattern.Current.VerticallyScrollable) {
    $pattern.SetScrollPercent(-1,$percent)
    Start-Sleep -Milliseconds 300
  }
}
function Get-AppWindow($process,[string]$id) {
  $desktop=[System.Windows.Automation.AutomationElement]::RootElement
  $condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty,$process.Id)
  for($i=0;$i -lt 120;$i++){
    if($process.HasExited){throw "WinUI exited: $($process.ExitCode)"}
    $window=$desktop.FindFirst([System.Windows.Automation.TreeScope]::Children,$condition)
    if($window -and (Find-Control $window $id)){return $window}
    Start-Sleep -Milliseconds 250
  }
  throw 'WinUI window was not created'
}
$root=Join-Path $env:TEMP ("WinSquashfsUi-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory "$root\src\nested" -Force | Out-Null
Set-Content "$root\src\nested\hello.txt" 'hello' -NoNewline
Set-Content (Join-Path "$root\src\nested" ('zz-' + ('long-name-' * 10) + '.txt')) 'Long filename preview' -NoNewline
$img="$root\ui.squashfs"
$p=$null
$env:WINSQUASHFS_DIAGNOSTICS=Join-Path $root "application.log"
$started=Get-Date
$uncSettings=Join-Path $env:LOCALAPPDATA 'WinSquashfs\unc.json'
$backup=$null
if(Test-Path $uncSettings){$backup=[IO.File]::ReadAllBytes($uncSettings)}
$agentStarted=$false
try {
  $p=Start-Process "$AppDirectory\WinSquashfs.exe" -ArgumentList @('--pack',('"'+"$root\src"+'"')) -WorkingDirectory $AppDirectory -PassThru
  $window=Get-AppWindow $p 'PackSource'
  Set-Value $window 'PackSource' "$root\src"
  Set-Value $window 'PackOutput' $img
  Save-WindowScreenshot $p '01-pack'
  Assert-Visible $window 'CreateImage'
  $adv=Wait-Control $window 'AdvancedParameters'
  $exp=$null
  if($adv.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern,[ref]$exp)){
    $exp.Expand()
    Scroll-Page $window 'PackScroll' 56
    Save-WindowScreenshot $p '02-advanced'
    Assert-Visible $window 'CreateImage'
    Scroll-Page $window 'PackScroll' 100
    Save-WindowScreenshot $p '02b-advanced-bottom'
    Assert-Visible $window 'CreateImage'
    $exp.Collapse()
    Scroll-Page $window 'PackScroll' 0
  }
  Invoke-Control $window 'CreateImage'
  for($i=0;$i -lt 120 -and !(Test-Path $img);$i++){Start-Sleep -Milliseconds 250}
  if(!(Test-Path $img)){throw 'GUI did not create image'}
  $null=Wait-Control $window 'CreateImage' $true
  $p.CloseMainWindow()|Out-Null
  if(!$p.WaitForExit(10000)){throw 'GUI did not close'}
  $p=Start-Process "$AppDirectory\WinSquashfs.exe" -ArgumentList @('--open',('"' + $img + '"')) -WorkingDirectory $AppDirectory -PassThru
  $window=Get-AppWindow $p 'FileList'
  $null=Wait-Control $window 'MountImage' $true
  Save-WindowScreenshot $p '03-browser'
  $list=Wait-Control $window 'FileList'
  $itemCondition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::ListItem)
  $item=$list.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$itemCondition)
  if(!$item){throw 'Browser did not show root folder'}
  $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
  Invoke-Control $window 'OpenFolder'
  Start-Sleep -Milliseconds 500
  $path=(Wait-Control $window 'ArchivePath').GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value
  if($path -ne '\nested'){throw "Navigation failed: $path"}
  $file=$list.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$itemCondition)
  if($file){$file.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()}
  Save-WindowScreenshot $p '04-preview'
  Save-WindowScreenshot $p '04b-compact-browser' 820 560
  Assert-Visible $window 'MountImage'
  Assert-Visible $window 'VerifyImage'
  Invoke-Control $window 'VerifyImage'
  $null=Wait-Control $window 'VerifyImage' $true
  Invoke-Control $window 'UpFolder'
  Invoke-Control $window 'MountImage'
  $mounted=Wait-Control $window 'MountedList'
  $item=$null
  for($i=0;$i -lt 120;$i++){
    $item=$mounted.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$itemCondition)
    if($item){break}
    Start-Sleep -Milliseconds 250
  }
  if(!$item){throw 'Mount list is empty'}
  $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
  Save-WindowScreenshot $p '05-mounts'
  Invoke-Control $window 'UnmountImage'
  for($i=0;$i -lt 120;$i++){
    if(!$mounted.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$itemCondition)){break}
    Start-Sleep -Milliseconds 250
  }
  if($mounted.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$itemCondition)){throw 'Unmount failed'}
  Write-Host 'WinUI creation, navigation and mount controls passed.'

  # Exercise the actual background process from the GUI, then close the GUI.
  $nav=Wait-Control $window 'NavUnc' $true
  $pattern=$null
  if($nav.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern,[ref]$pattern)) {
    $pattern.Select()
  } else {
    $nav.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  }
  $alias='ui-' + [guid]::NewGuid().ToString('N').Substring(0,8)
  Set-Value $window 'UncRoot' $root
  Set-Value $window 'UncAlias' $alias
  $toggle=(Wait-Control $window 'UncDrives' $true).GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
  if($toggle.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On){$toggle.Toggle()}
  $drivesBefore=[IO.DriveInfo]::GetDrives().Name | Sort-Object
  Invoke-Control $window 'StartUnc'
  $agentStarted=$true
  $null=Wait-Control $window 'StopUnc' $true
  $unc='\\sfs$\' + $alias + '\ui.squashfs'
  Scroll-Page $window 'UncScroll' 0
  Save-WindowScreenshot $p '06-unc'
  Assert-Visible $window 'CopyUnc'
  Assert-Visible $window 'OpenUnc'
  Assert-Visible $window 'ApplyUnc'
  Scroll-Page $window 'UncScroll' 100
  Save-WindowScreenshot $p '06b-unc-settings'
  Assert-Visible $window 'ApplyUnc'
  Scroll-Page $window 'UncScroll' 0
  $uncActual=(Wait-Control $window 'UncImagePath').GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value
  if($uncActual -ne $unc){throw "Unexpected UNC path: $uncActual"}
  if([IO.File]::ReadAllText("$unc\nested\hello.txt") -ne 'hello'){throw 'GUI-launched UNC read failed'}
  if(Compare-Object $drivesBefore ([IO.DriveInfo]::GetDrives().Name | Sort-Object)){throw 'UNC assigned a drive letter'}
  Invoke-Control $window 'StopUnc'
  $agentStarted=$false
  $null=Wait-Control $window 'StartUnc' $true
  if([IO.Directory]::Exists($unc)){throw 'GUI stop did not remove UNC share'}
  Invoke-Control $window 'StartUnc'
  $agentStarted=$true
  $null=Wait-Control $window 'StopUnc' $true
  $nav=Wait-Control $window 'NavTools' $true
  $pattern=$null
  if($nav.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern,[ref]$pattern)){$pattern.Select()}else{$nav.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()}
  Save-WindowScreenshot $p '07-tools'
  $p.CloseMainWindow()|Out-Null
  if(!$p.WaitForExit(10000)){throw 'GUI did not close with UNC access enabled'}
  if([IO.File]::ReadAllText("$unc\nested\hello.txt") -ne 'hello'){throw 'Closing GUI stopped UNC access'}
  $stop=Start-Process "$AppDirectory\WinSquashfs.Unc.exe" -ArgumentList '--stop' -Wait -PassThru
  if($stop.ExitCode){throw "UNC agent stop exit code $($stop.ExitCode)"}
  $agentStarted=$false
  if([IO.Directory]::Exists($unc)){throw 'Background process did not release UNC share'}
  Write-Host 'WinUI UNC configuration, start, stop and background persistence passed.'
  $p=Start-Process "$AppDirectory\WinSquashfs.exe" -ArgumentList @('--open',('"' + $img + '"')) -WorkingDirectory $AppDirectory -PassThru
  $window=Get-AppWindow $p 'FileList'
  $null=Wait-Control $window 'CloseImage' $true
  Invoke-Control $window 'CloseImage'
  for($i=0;$i -lt 120;$i++){
    if(!(Wait-Control $window 'CloseImage').Current.IsEnabled){break}
    Start-Sleep -Milliseconds 100
  }
  $f=[IO.File]::Open($img,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
  $f.Dispose()
  Save-WindowScreenshot $p '08-empty-browser'
  Write-Host 'Closing an image releases its file handle.'
} catch {
  if(Test-Path $env:WINSQUASHFS_DIAGNOSTICS){Get-Content $env:WINSQUASHFS_DIAGNOSTICS | Write-Host}
  $uncLog=Join-Path $env:LOCALAPPDATA 'WinSquashfs\unc.log'
  if(Test-Path $uncLog){Get-Content $uncLog | Write-Host}
  Get-WinEvent -FilterHashtable @{LogName="Application";StartTime=$started;Level=2} -ErrorAction SilentlyContinue | Select-Object -First 10 | ForEach-Object { Write-Host $_.Message }
  throw
} finally {
  Remove-Item Env:WINSQUASHFS_DIAGNOSTICS -ErrorAction SilentlyContinue
  if($p -and !$p.HasExited){$p.CloseMainWindow()|Out-Null;if(!$p.WaitForExit(10000)){$p.Kill();$p.WaitForExit()}}
  if($agentStarted){Start-Process "$AppDirectory\WinSquashfs.Unc.exe" -ArgumentList '--stop' -Wait}
  if($backup -ne $null){[IO.File]::WriteAllBytes($uncSettings,$backup)}
  elseif(Test-Path $uncSettings){Remove-Item $uncSettings -Force}
  Remove-Item $root -Recurse -Force
}
