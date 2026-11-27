$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root '.deps\winfsp-binding'
New-Item -ItemType Directory -Force $dir | Out-Null
$files = [ordered]@{
  'FileSystemBase+Const.cs' = 'b14c50a3b474451fe111b40d374ea812ee0898c4'
  'FileSystemBase.cs' = 'f962b9af596078ba0512ea079ee920937f275fdd'
  'FileSystemHost.cs' = '3255beaa6bc8bd62727648eda689559038933adf'
  'Interop.cs' = 'a6343d879f7faae23ccb82dbcec7228da6c2f833'
  'Service.cs' = '9b2f576a814178a04eb9204422bb621286015ade'
}
foreach ($name in $files.Keys) {
  $path = Join-Path $dir $name
  $uri = 'https://raw.githubusercontent.com/winfsp/winfsp/v2.2B1/src/dotnet/' + $name
  Invoke-WebRequest $uri -OutFile $path
  $hash = (& git hash-object --no-filters $path).Trim()
  if ($LASTEXITCODE -or $hash -ne $files[$name]) { throw "WinFsp source hash mismatch: $name" }
}
$path = Join-Path $dir 'FileSystemHost.cs'
$source = [IO.File]::ReadAllText($path)
$old = 'Result = Api.FspFileSystemSetMountPointEx(_FileSystemPtr, MountPoint,'
if (!$source.Contains($old)) { throw 'WinFsp MountEx source has changed' }
$source = $source.Replace($old, 'Result = PrefixOnly ? STATUS_SUCCESS : Api.FspFileSystemSetMountPointEx(_FileSystemPtr, MountPoint,')
$old = 'public String Prefix'
if (!$source.Contains($old)) { throw 'WinFsp prefix property has changed' }
$source = $source.Replace($old, @'
public Boolean PrefixOnly { get; set; }

        public Boolean ReadOnlyVolume
        {
            get { return 0 != (_VolumeParams.Flags & VolumeParams.ReadOnlyVolume); }
            set
            {
                if (value) _VolumeParams.Flags |= VolumeParams.ReadOnlyVolume;
                else _VolumeParams.Flags &= ~VolumeParams.ReadOnlyVolume;
            }
        }

        public String Prefix
'@)
# Prefix-only mounting is meaningful only for a network filesystem.
$old = 'Result = Api.FspFileSystemCreate('
$source = $source.Replace($old, 'if (PrefixOnly && _VolumeParams.IsPrefixEmpty()) return unchecked((Int32)0xC000000D);' + [Environment]::NewLine + '            ' + $old)
# FileSystemHost is not derived from FileSystemBase; use the literal success status.
$source = $source.Replace('PrefixOnly ? STATUS_SUCCESS :', 'PrefixOnly ? 0 :')
[IO.File]::WriteAllText($path, $source, [Text.UTF8Encoding]::new($false))
