# SPDX-License-Identifier: GPL-3.0-only
if (-not ('WinSquashfsIconTest' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

public static class WinSquashfsIconTest
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadImageW(IntPtr instance, string name, uint type, int width, int height, uint flags);

    [DllImport("user32.dll")]
    public static extern bool DestroyIcon(IntPtr icon);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryExW(string fileName, IntPtr file, uint flags);

    [DllImport("kernel32.dll")]
    private static extern bool FreeLibrary(IntPtr module);

    private delegate bool ResourceCallback(IntPtr module, IntPtr type, IntPtr name, IntPtr parameter);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern bool EnumResourceNamesW(IntPtr module, IntPtr type, ResourceCallback callback, IntPtr parameter);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);

    [DllImport("kernel32.dll")]
    private static extern uint SizeofResource(IntPtr module, IntPtr resource);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LoadResource(IntPtr module, IntPtr resource);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LockResource(IntPtr resource);

    private static byte[] ReadResource(IntPtr module, IntPtr name, int type)
    {
        IntPtr resource = FindResourceW(module, name, new IntPtr(type));
        if (resource == IntPtr.Zero) return new byte[0];
        int size = checked((int)SizeofResource(module, resource));
        IntPtr data = LockResource(LoadResource(module, resource));
        if (data == IntPtr.Zero || size == 0) return new byte[0];
        byte[] bytes = new byte[size];
        Marshal.Copy(data, bytes, 0, size);
        return bytes;
    }

    public static bool HasIcon(string executable, string icoPath)
    {
        byte[] ico = File.ReadAllBytes(icoPath);
        int count = BitConverter.ToUInt16(ico, 4);
        byte[] expected = null;
        for (int i = 0; i < count; i++)
        {
            int entry = 6 + i * 16;
            if (ico[entry] != 32 || ico[entry + 1] != 32) continue;
            int size = BitConverter.ToInt32(ico, entry + 8);
            int offset = BitConverter.ToInt32(ico, entry + 12);
            expected = new byte[size];
            Buffer.BlockCopy(ico, offset, expected, 0, size);
            break;
        }
        if (expected == null) return false;

        IntPtr module = LoadLibraryExW(executable, IntPtr.Zero, 0x22);
        if (module == IntPtr.Zero) return false;
        try
        {
            bool matched = false;
            ResourceCallback callback = (handle, type, name, parameter) =>
            {
                byte[] group = ReadResource(handle, name, 14);
                if (group.Length < 6) return true;
                int entries = BitConverter.ToUInt16(group, 4);
                for (int i = 0; i < entries && 6 + i * 14 + 14 <= group.Length; i++)
                {
                    int entry = 6 + i * 14;
                    if (group[entry] != 32 || group[entry + 1] != 32) continue;
                    int id = BitConverter.ToUInt16(group, entry + 12);
                    byte[] actual = ReadResource(handle, new IntPtr(id), 3);
                    // Compare the 32-bit bitmap pixels, independent of the icon resource IDs.
                    if (actual.Length >= 40 + 32 * 32 * 4 && expected.Length >= 40 + 32 * 32 * 4 &&
                        BitConverter.ToInt32(actual, 0) == 40 &&
                        actual.Skip(40).Take(32 * 32 * 4).SequenceEqual(expected.Skip(40).Take(32 * 32 * 4)))
                    {
                        matched = true;
                        return false;
                    }
                }
                return true;
            };
            EnumResourceNamesW(module, new IntPtr(14), callback, IntPtr.Zero);
            GC.KeepAlive(callback);
            return matched;
        }
        finally { FreeLibrary(module); }
    }
}
'@
}

function Assert-IconAssets {
    param([Parameter(Mandatory=$true)][string]$AppDirectory)
    $sizes=@(16,24,32,48,64,96,128,256)
    $source=Join-Path (Split-Path -Parent $PSScriptRoot) 'src/App/Assets'
    foreach($name in @('app-icon.ico','file-icon.ico')){
        $path=Join-Path $AppDirectory "Assets/$name"
        if(!(Test-Path -LiteralPath $path)){throw "Missing $path"}
        if((Get-FileHash -LiteralPath $path).Hash -ne (Get-FileHash -LiteralPath (Join-Path $source $name)).Hash){
            throw "Packaged icon differs from source: $name"
        }
        $data=[IO.File]::ReadAllBytes($path)
        if([BitConverter]::ToUInt16($data,0) -ne 0 -or [BitConverter]::ToUInt16($data,2) -ne 1 -or [BitConverter]::ToUInt16($data,4) -ne $sizes.Count){
            throw "Invalid icon directory: $name"
        }
        for($i=0;$i -lt $sizes.Count;$i++){
            $size=$sizes[$i]
            $entry=6+16*$i
            $dimension=$size % 256
            if($data[$entry] -ne $dimension -or $data[$entry+1] -ne $dimension -or [BitConverter]::ToUInt16($data,$entry+6) -ne 32){
                throw "Missing $($size)px 32-bit image in $name"
            }
            $icon=[WinSquashfsIconTest]::LoadImageW([IntPtr]::Zero,$path,1,$size,$size,0x10)
            if($icon -eq [IntPtr]::Zero){throw "Windows cannot load $($size)px icon from $path"}
            [void][WinSquashfsIconTest]::DestroyIcon($icon)
        }
    }
    Assert-ExecutableIcon -Path (Join-Path $AppDirectory 'WinSquashfs.exe') -IconPath (Join-Path $AppDirectory 'Assets/app-icon.ico')
    Write-Host 'PASS packaged application and file icons'
}

function Assert-ExecutableIcon {
    param([Parameter(Mandatory=$true)][string]$Path,[Parameter(Mandatory=$true)][string]$IconPath)
    if(![WinSquashfsIconTest]::HasIcon($Path,$IconPath)){
        throw "Expected embedded application icon in $Path"
    }
}

function Assert-InstalledIcons {
    param([Parameter(Mandatory=$true)][string]$AppDirectory,[Parameter(Mandatory=$true)][string]$Setup)
    $icon=Join-Path $AppDirectory 'Assets/app-icon.ico'
    Assert-ExecutableIcon -Path $Setup -IconPath $icon
    Assert-ExecutableIcon -Path (Join-Path (Split-Path -Parent $AppDirectory) 'Uninstall.exe') -IconPath $icon
    $fileIcon=Join-Path $AppDirectory 'Assets\file-icon.ico'
    $expected='"'+$fileIcon+'",0'
    $key=Get-Item 'HKLM:\Software\Classes\WinSquashfs.Image\DefaultIcon'
    if($key.GetValue('') -ne $expected){throw 'Incorrect SquashFS file association icon'}
    foreach($extension in @('.squashfs','.sqfs','.sfs')){
        $registration=Get-Item "HKLM:\Software\Classes\$extension\OpenWithProgids"
        if($registration.GetValueNames() -notcontains 'WinSquashfs.Image'){throw "Missing file association for $extension"}
    }
    Write-Host 'PASS installer, uninstaller and registered file icons'
}
