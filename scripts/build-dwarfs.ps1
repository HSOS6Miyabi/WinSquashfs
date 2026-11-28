$ErrorActionPreference='Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
cargo build --manifest-path native/dwarfs-reader/Cargo.toml --release --target x86_64-pc-windows-msvc
if($LASTEXITCODE){throw 'DwarFS native reader build failed'}
Copy-Item native/dwarfs-reader/target/x86_64-pc-windows-msvc/release/winsqfs_dwarfs.dll artifacts/native/winsqfs-dwarfs.dll
$exe=Join-Path (Resolve-Path artifacts/native/tools) 'dwarfs-universal.exe'
Invoke-WebRequest 'https://github.com/mhx/dwarfs/releases/download/v0.12.4/dwarfs-universal-0.12.4-Windows-AMD64.exe' -OutFile $exe
if((Get-FileHash $exe -Algorithm SHA256).Hash -ne '094EAAF821DF47DAAA11899927F269F57F71843B39ECD53BA70692447CB3E65'){throw 'DwarFS binary hash mismatch'}
Invoke-WebRequest 'https://raw.githubusercontent.com/mhx/dwarfs/v0.12.4/LICENSE' -OutFile artifacts/native/licenses/dwarfs.txt
