$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$dist = Join-Path $root 'artifacts\dist'
$cli = Join-Path $root 'artifacts\cli'
dotnet publish src/App/App.csproj -c Release -r win-x64 -p:Platform=x64 --self-contained true -o $dist
if ($LASTEXITCODE) { throw 'WinUI publish failed' }
dotnet publish src/Cli/Cli.csproj -c Release -r win-x64 --self-contained true -o $cli
if ($LASTEXITCODE) { throw 'CLI publish failed' }
Copy-Item "$cli\winsqfs.exe","$cli\winsqfs.dll","$cli\winsqfs.deps.json","$cli\winsqfs.runtimeconfig.json" -Destination $dist -Force
$agent = Join-Path $root 'artifacts\agent'
dotnet publish src/UncAgent/UncAgent.csproj -c Release -r win-x64 --self-contained true -o $agent
if ($LASTEXITCODE) { throw 'UNC background application publish failed' }
Copy-Item "$agent\WinSquashfs.Unc.exe","$agent\WinSquashfs.Unc.dll","$agent\WinSquashfs.Unc.deps.json","$agent\WinSquashfs.Unc.runtimeconfig.json" -Destination $dist -Force
Copy-Item 'artifacts\native\*' $dist -Recurse -Force
Copy-Item README.md,README.zh-CN.md,LICENSE,THIRD-PARTY.md -Destination $dist
if (!(Test-Path "$dist\winsqfs-native.dll")) { throw 'Missing native reader' }
