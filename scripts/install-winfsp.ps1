$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root 'artifacts'
New-Item -ItemType Directory -Force $dir | Out-Null
$msi = Join-Path $dir 'winfsp.msi'
Invoke-WebRequest 'https://github.com/winfsp/winfsp/releases/download/v2.2B1/winfsp-2.2.26112.msi' -OutFile $msi
if ((Get-FileHash $msi -Algorithm SHA256).Hash -ne 'F9E70EDE2344A30D377A38555E2128C0770D64DDC53C0E2AF7DFE0C605F422A8') { throw "WinFsp installer hash mismatch" }
$p = Start-Process msiexec.exe -ArgumentList @('/i',('"' + $msi + '"'),'/qn','/norestart','ADDLOCAL=ALL') -Wait -PassThru
if ($p.ExitCode -notin 0,3010) { throw "WinFsp install returned $($p.ExitCode)" }
$assembly = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'WinFsp\bin\winfsp-msil.dll'
if (!(Test-Path $assembly)) { throw "WinFsp managed assembly is missing" }

& (Join-Path $PSScriptRoot 'prepare-winfsp-binding.ps1')
