$ErrorActionPreference='Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
cargo build --manifest-path native/dwarfs-reader/Cargo.toml --release --target x86_64-pc-windows-msvc
if($LASTEXITCODE){throw 'DwarFS native reader build failed'}
Copy-Item native/dwarfs-reader/target/x86_64-pc-windows-msvc/release/winsqfs_dwarfs.dll artifacts/native/winsqfs-dwarfs.dll
$zip=Join-Path $env:RUNNER_TEMP 'dwarfs-0.12.4-Windows-AMD64.7z'
Invoke-WebRequest 'https://github.com/mhx/dwarfs/releases/download/v0.12.4/dwarfs-0.12.4-Windows-AMD64.7z' -OutFile $zip
$hash=(Get-FileHash $zip -Algorithm SHA256).Hash
if($hash -ne '5574D6AEB970C4CBABD50526994159C0551B2DC3E940EDC774AC0952E1528C93'){throw "DwarFS archive hash mismatch: $hash"}
$out=Join-Path $env:RUNNER_TEMP 'dwarfs-distribution'
& 'C:\Program Files\7-Zip\7z.exe' x $zip "-o$out" -y
if($LASTEXITCODE){throw 'DwarFS extraction failed'}
$mk=Get-ChildItem $out -Recurse -Filter mkdwarfs.exe | Select-Object -First 1
if(!$mk){throw 'Missing mkdwarfs.exe'}
Copy-Item $mk.FullName artifacts/native/tools/mkdwarfs.exe -Force
Get-ChildItem $mk.DirectoryName -Filter *.dll | Copy-Item -Destination artifacts/native/tools -Force
Invoke-WebRequest 'https://raw.githubusercontent.com/mhx/dwarfs/v0.12.4/LICENSE' -OutFile artifacts/native/licenses/dwarfs.txt
