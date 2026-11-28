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
  # Verify the registered paste command using a dedicated local WebDAV fixture.
  $cli=Join-Path $AppDirectory 'archivefs.exe'
  $profilePath=Join-Path $env:LOCALAPPDATA 'WinArchiveFSPro\remote-connections.json'
  $profileBackup=if(Test-Path $profilePath){[IO.File]::ReadAllBytes($profilePath)}else{$null}
  $server=$null
  try{
    & $cli unc-stop
    if($LASTEXITCODE){throw 'Cannot stop UNC agent for shell test'}
    $portFile=Join-Path $root 'server-port.txt'
    $secret='shell-test-'+[guid]::NewGuid().ToString('N')
    $scriptPath=(Resolve-Path (Join-Path $PSScriptRoot '../native/cpp/tests/remote_webdav.py')).Path
    $server=Start-Process python -ArgumentList @(('"' + $scriptPath + '"'),'--serve',('"' + $portFile + '"'),$secret) -WindowStyle Hidden -PassThru
    for($i=0;$i -lt 100 -and !(Test-Path $portFile);$i++){Start-Sleep -Milliseconds 100}
    if(!(Test-Path $portFile)){throw 'WebDAV shell test server did not start'}
    Add-Type -AssemblyName System.Security
    $plain=[Text.Encoding]::UTF8.GetBytes((@{secret=$secret;session_token=''}|ConvertTo-Json -Compress))
    $encrypted=[Security.Cryptography.ProtectedData]::Protect($plain,$null,[Security.Cryptography.DataProtectionScope]::CurrentUser)
    $hex=([BitConverter]::ToString($encrypted)).Replace('-','').ToLowerInvariant()
    $profile=@{name='shell-test';protocol='dav';endpoint=('http://127.0.0.1:'+(Get-Content $portFile -Raw));root='/dav';username='ui';credentials=$hex}
    New-Item -ItemType Directory -Force (Split-Path -Parent $profilePath)|Out-Null
    [IO.File]::WriteAllText($profilePath,('['+($profile|ConvertTo-Json -Compress)+']'),[Text.UTF8Encoding]::new($false))
    & $cli unc-start
    if($LASTEXITCODE){throw 'Cannot start UNC agent for shell test'}
    $remote='\\$sfs\dav\shell-test'
    if([IO.File]::ReadAllText($remote+'\a.txt') -ne '0123456789'){throw 'Remote shell test source unavailable'}
    & $Harness --server-paste ($remote+'\destination') ($remote+'\a.txt')
    if($LASTEXITCODE){throw 'Remote paste COM command failed'}
    $copied=$remote+'\destination\a.txt'
    for($i=0;$i -lt 120 -and ![IO.File]::Exists($copied);$i++){Start-Sleep -Milliseconds 250}
    if([IO.File]::ReadAllText($copied) -ne '0123456789'){throw 'Remote paste content mismatch'}
    if([IO.File]::ReadAllText($remote+'\a.txt') -ne '0123456789'){throw 'Remote paste modified source'}
    Write-Host 'PASS Explorer server-side paste COM command through active UNC mount'
  }finally{
    & $cli unc-stop
    if($server -and !$server.HasExited){$server.Kill();$server.WaitForExit()}
    if($null -ne $profileBackup){[IO.File]::WriteAllBytes($profilePath,$profileBackup)}elseif(Test-Path $profilePath){Remove-Item -LiteralPath $profilePath -Force}
  }
}finally{
  if($p -and !$p.HasExited){$p.CloseMainWindow()|Out-Null;if(!$p.WaitForExit(10000)){$p.Kill()}}
  Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
}
