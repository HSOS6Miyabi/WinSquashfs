param([Parameter(Mandatory=$true)][string]$AppDirectory,[Parameter(Mandatory=$true)][string]$Harness)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$root=Join-Path $env:TEMP ('WinArchiveFSProShell-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$root\folder" -Force|Out-Null
Set-Content "$root\a.txt" 'alpha' -NoNewline
Set-Content "$root\b.txt" 'beta' -NoNewline
Set-Content "$root\folder\c.txt" 'gamma' -NoNewline
$before=@(Get-Process WinArchiveFSPro -ErrorAction SilentlyContinue|ForEach-Object Id)
$p=$null
try{
  & $Harness "$root\a.txt" "$root\b.txt" "$root\folder"
  if($LASTEXITCODE){throw "COM multiselect creation command failed: $LASTEXITCODE"}
  $desktop=[System.Windows.Automation.AutomationElement]::RootElement
  for($i=0;$i -lt 120;$i++){
    $p=Get-Process WinArchiveFSPro -ErrorAction SilentlyContinue|Where-Object {$_.Id -notin $before}|Select-Object -First 1
    if($p){
      $pc=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty,$p.Id)
      $window=$desktop.FindFirst([System.Windows.Automation.TreeScope]::Children,$pc)
      if($window){
        $lc=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,'SourceList')
        $list=$window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$lc)
        if($list){
          $ic=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::ListItem)
          $items=$list.FindAll([System.Windows.Automation.TreeScope]::Descendants,$ic)
          if($items.Count -eq 3){break}
        }
      }
    }
    Start-Sleep -Milliseconds 250
  }
  if(!$list -or $items.Count -ne 3){throw 'Explorer selection did not reach creation UI as one group'}
  $cc=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,'CreateImage')
  $create=$window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$cc)
  $create.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  $image="$root\archive.squashfs"
  for($i=0;$i -lt 120 -and !(Test-Path $image);$i++){Start-Sleep -Milliseconds 250}
  if(!(Test-Path $image)){throw 'Multiselect shell creation did not produce archive'}
  & "$AppDirectory\archivefs.exe" verify $image
  if($LASTEXITCODE){throw 'Shell-created image failed verification'}
  $a=& "$AppDirectory\archivefs.exe" read $image a.txt 0 5
  if($a -ne '616C706861'){throw 'First selected file missing'}
  $b=& "$AppDirectory\archivefs.exe" read $image b.txt 0 4
  if($b -ne '62657461'){throw 'Second selected file missing'}
  $c=& "$AppDirectory\archivefs.exe" read $image 'folder\c.txt' 0 5
  if($c -ne '67616D6D61'){throw 'Selected folder missing'}
  Write-Host 'PASS Explorer COM multiselect, one window, three inputs, one image'
}finally{
  if($p -and !$p.HasExited){$p.CloseMainWindow()|Out-Null;if(!$p.WaitForExit(10000)){$p.Kill()}}
  Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
}
