param([Parameter(Mandatory=$true)][string]$AppDirectory)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
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
$img="$root\ui.squashfs"
$p=$null
try {
  $p=Start-Process "$AppDirectory\WinSquashfs.exe" -ArgumentList @('--pack',('"'+"$root\src"+'"')) -PassThru
  $window=Get-AppWindow $p 'PackSource'
  Set-Value $window 'PackSource' "$root\src"
  Set-Value $window 'PackOutput' $img
  Invoke-Control $window 'CreateImage'
  for($i=0;$i -lt 120 -and !(Test-Path $img);$i++){Start-Sleep -Milliseconds 250}
  if(!(Test-Path $img)){throw 'GUI did not create image'}
  $null=Wait-Control $window 'CreateImage' $true
  $p.CloseMainWindow()|Out-Null
  if(!$p.WaitForExit(10000)){throw 'GUI did not close'}
  $p=Start-Process "$AppDirectory\WinSquashfs.exe" -ArgumentList @('--open',('"' + $img + '"')) -PassThru
  $window=Get-AppWindow $p 'FileList'
  $null=Wait-Control $window 'MountImage' $true
  $list=Wait-Control $window 'FileList'
  $itemCondition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::ListItem)
  $item=$list.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$itemCondition)
  if(!$item){throw 'Browser did not show root folder'}
  $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
  Invoke-Control $window 'OpenFolder'
  Start-Sleep -Milliseconds 500
  $path=(Wait-Control $window 'ArchivePath').GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value
  if($path -ne '\nested'){throw "Navigation failed: $path"}
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
  Invoke-Control $window 'UnmountImage'
  for($i=0;$i -lt 120;$i++){
    if(!$mounted.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$itemCondition)){break}
    Start-Sleep -Milliseconds 250
  }
  if($mounted.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$itemCondition)){throw 'Unmount failed'}
  Write-Host 'WinUI creation, navigation and mount controls passed.'
} finally {
  if($p -and !$p.HasExited){$p.CloseMainWindow()|Out-Null;if(!$p.WaitForExit(10000)){$p.Kill();$p.WaitForExit()}}
  Remove-Item $root -Recurse -Force
}
