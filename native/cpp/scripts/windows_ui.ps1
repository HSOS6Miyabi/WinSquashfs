# SPDX-License-Identifier: GPL-3.0-only
param([Parameter(Mandatory=$true)][string]$AppDirectory,[ValidateSet('en-US','zh-CN')][string]$Language='en-US')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
. (Join-Path $PSScriptRoot '../../../tests/screenshots.ps1')
$script:ScreenshotPrefix="native-$Language-"
$AppDirectory=(Resolve-Path $AppDirectory).Path
$root=Join-Path $env:TEMP ('ArchiveFSUI-'+[guid]::NewGuid().ToString('N'))
$settings=Join-Path $env:LOCALAPPDATA 'WinArchiveFSPro\unc.json'
$backup=if(Test-Path $settings){[IO.File]::ReadAllBytes($settings)}else{$null}
$cli=Join-Path $AppDirectory 'archivefs.exe'
$gui=Join-Path $AppDirectory 'WinArchiveFSPro.exe'
$alias='ui-'+[guid]::NewGuid().ToString('N').Substring(0,8)
$p=$null
function Find-Id($parent,[string]$id){$condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,$id);$parent.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$condition)}
function Wait-Until([scriptblock]$condition,[string]$description,[int]$seconds=40){$until=[DateTime]::UtcNow.AddSeconds($seconds);do{try{$result=&$condition;if($result){return $result}}catch{};Start-Sleep -Milliseconds 200}while([DateTime]::UtcNow -lt $until);throw "Timeout: $description"}
function Control($window,[string]$id,[bool]$enabled=$true){Wait-Until { $c=Find-Id $window $id; if($c -and (!$enabled -or $c.Current.IsEnabled)){return $c} } "control $id"}
function Window($process,[string]$id){$desktop=[System.Windows.Automation.AutomationElement]::RootElement;$cond=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty,$process.Id);Wait-Until { $w=$desktop.FindFirst([System.Windows.Automation.TreeScope]::Children,$cond);if($w -and (Find-Id $w $id)){return $w}} "window $id"}
function Invoke-Control($w,[string]$id){(Control $w $id).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()}
function Set-Value($w,[string]$id,[string]$value){(Control $w $id).GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($value)}
function Value($w,[string]$id){(Control $w $id $false).GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value}
function Navigate($w,[string]$id){$e=Control $w $id;$pat=$null;if($e.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern,[ref]$pat)){$pat.Select()}else{$e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()}}
function Choose($w,[string]$id,[string]$name){$e=Control $w $id;$e.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand();$cond=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$name);$item=Wait-Until {$i=$e.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$cond);if(!$i){$i=$w.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$cond)};if($i){return $i}} "combo option $name";$item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select();try{$e.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse()}catch{}}
function Items($w,[string]$id){$list=Control $w $id;$cond=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::ListItem);$list.FindAll([System.Windows.Automation.TreeScope]::Descendants,$cond)}
function Close-Gui {if($script:p -and !$script:p.HasExited){$script:p.CloseMainWindow()|Out-Null;if(!$script:p.WaitForExit(10000)){$script:p.Kill();throw 'GUI did not exit'}};$script:p=$null}
try{
  New-Item -ItemType Directory -Force "$root\source",(Split-Path -Parent $settings)|Out-Null
  [IO.File]::WriteAllText("$root\source\hello.txt",'native archive preview')
  [IO.File]::WriteAllText("$root\extra.txt",'second selected file')
  & $cli unc-stop;if($LASTEXITCODE){throw 'Cannot stop UNC agent'}
  [IO.File]::WriteAllText($settings,(@{Root=$root;Alias=$alias;IncludeDrives=$false;IdleSeconds=60;MaxIdleImages=8;StartAtLogin=$false;Language=$Language}|ConvertTo-Json -Compress))
  $p=Start-Process $gui -ArgumentList @('--pack',('"'+"$root\source"+'"'),('"'+"$root\extra.txt"+'"')) -PassThru
  $w=Window $p 'SourceList'
  $null=Wait-Until {(Items $w 'SourceList').Count -eq 2} 'two selected sources'
  $expected=if($Language -eq 'zh-CN'){'创建镜像'}else{'Create image'}
  if((Control $w 'CreateImage').Current.Name -ne $expected){throw ('Incorrect localized creation label: expected {0}; actual {1}' -f $expected,(Control $w 'CreateImage').Current.Name)}
  $images=@()
  foreach($format in @('SquashFS','DwarFS','EROFS')){
    Choose $w 'ImageFormat' $format
    $image=Join-Path $root ('gui.'+$format.ToLowerInvariant())
    Set-Value $w 'PackOutput' $image
    if($format -eq 'SquashFS'){
      $advanced=(Control $w 'AdvancedParameters').GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
      $advanced.Expand()
      (Control $w 'OverridePreset').GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
      Choose $w 'PackCodec' 'xz'
      Choose $w 'PackCodec' 'gzip'
      Save-WindowScreenshot $p 'create-SquashFS-advanced'
      $advanced.Collapse()
    }
    Save-WindowScreenshot $p ('create-'+$format)
    $button=Control $w 'CreateImage';if($button.Current.IsOffscreen){throw 'Creation button is offscreen'}
    Invoke-Control $w 'CreateImage'
    $null=Wait-Until {Test-Path $image} "create $format" 180
    $null=Control $w 'CreateImage'
    & $cli verify $image;if($LASTEXITCODE){throw "Invalid GUI-created $format"}
    if($format -eq 'SquashFS'){
      $info=& $cli info $image
      if($LASTEXITCODE -or $info -notmatch '^SquashFS gzip '){throw "Codec switch failed: $info"}
    }
    $images+=$image
  }
  Close-Gui
  $image=$images[0]
  $p=Start-Process $gui -ArgumentList @('--open',('"'+$image+'"')) -PassThru
  $w=Window $p 'FileList'
  $null=Control $w 'MountImage'
  foreach($id in @('CloseImage','VerifyImage','ExtractAll','OpenUnc','CopyUnc')) {
    if(!(Control $w $id $false).Current.IsEnabled){throw "Open image action disabled: $id"}
  }
  foreach($id in @('UpDirectory','ExtractSelected')) {
    if((Control $w $id $false).Current.IsEnabled){throw "Root or unselected action enabled: $id"}
  }
  $entry=Wait-Until {foreach($item in (Items $w 'FileList')){if($item.Current.Name -like 'extra.txt*'){return $item}}} 'file list entry'
  $entry.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
  $null=Wait-Until {(Value $w 'FilePreview') -like '*second selected file*'} 'file preview'
  $null=Control $w 'ExtractSelected'
  Save-WindowScreenshot $p 'browser-preview'
  if((Control $w 'FilePreview' $false).Current.IsOffscreen){throw 'Preview is offscreen at the default desktop resolution'}
  Save-WindowScreenshot $p 'browser-compact' 820 560
  foreach($id in @('MountImage','ExtractSelected','ExtractAll')){
    if((Control $w $id).Current.IsOffscreen){throw "Compact window action is offscreen: $id"}
  }
  Save-WindowScreenshot $p 'browser-restored'
  $before=@([IO.DriveInfo]::GetDrives().Name)
  Invoke-Control $w 'MountImage'
  $drive=Wait-Until {foreach($d in [IO.DriveInfo]::GetDrives()){if($d.Name -notin $before -and [IO.File]::Exists($d.Name+'extra.txt')){return $d.Name}}} 'mounted drive'
  if([IO.File]::ReadAllText($drive+'extra.txt') -ne 'second selected file'){throw 'Wrong mount data'}
  Navigate $w 'NavMount'
  $item=Wait-Until {foreach($i in (Items $w 'MountedList')){return $i}} 'mounted image list'
  $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
  Save-WindowScreenshot $p 'mounted'
  Invoke-Control $w 'UnmountImage'
  $null=Wait-Until {!([IO.Directory]::Exists($drive))} 'eject drive'
  Navigate $w 'NavUnc'
  Invoke-Control $w 'StartUnc'
  $unc='\\sfs$\'+$alias+'\'+[IO.Path]::GetFileName($image)
  $null=Wait-Until {[IO.File]::Exists($unc+'\extra.txt')} 'UNC start'
  if([IO.File]::ReadAllText($unc+'\extra.txt') -ne 'second selected file'){throw 'Wrong UNC file data'}
  Save-WindowScreenshot $p 'unc'
  foreach($id in @('UncImagePath','OpenUnc','CopyUnc','ApplyUnc','StartUnc','StopUnc')){
    if((Control $w $id $false).Current.IsOffscreen){throw "UNC action is offscreen: $id"}
  }
  Invoke-Control $w 'StopUnc'
  $null=Wait-Until {!([IO.Directory]::Exists($unc))} 'UNC stop'
  Invoke-Control $w 'StartUnc'
  $null=Wait-Until {[IO.File]::Exists($unc+'\extra.txt')} 'UNC restart'
  Navigate $w 'NavBrowse'
  Invoke-Control $w 'CloseImage'
  foreach($id in @('MountImage','CloseImage','VerifyImage','ExtractSelected','ExtractAll','OpenUnc','CopyUnc','UpDirectory')) {
    if((Control $w $id $false).Current.IsEnabled){throw "Closed image action enabled: $id"}
  }
  Close-Gui
  if([IO.File]::ReadAllText($unc+'\extra.txt') -ne 'second selected file'){throw 'GUI exit stopped UNC service'}
  & $cli unc-stop;if($LASTEXITCODE){throw 'UNC stop failed'}
  $null=Wait-Until {!([IO.Directory]::Exists($unc))} 'UNC released'
  Write-Host "PASS $Language GUI create all formats, browse, preview, mount, eject, UNC and background persistence"
}finally{
  if($p -and !$p.HasExited){$p.CloseMainWindow()|Out-Null;if(!$p.WaitForExit(5000)){$p.Kill()}}
  & $cli unc-stop
  if($null -ne $backup){[IO.File]::WriteAllBytes($settings,$backup)}elseif(Test-Path $settings){Remove-Item -LiteralPath $settings -Force}
  Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
}
