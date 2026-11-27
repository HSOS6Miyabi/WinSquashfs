# SPDX-License-Identifier: GPL-3.0-only
param(
    [Parameter(Mandatory=$true)][string]$Setup,
    [Parameter(Mandatory=$true)][string]$Uninstaller
)
$ErrorActionPreference = 'Stop'

$assets = Join-Path (Split-Path -Parent $PSScriptRoot) 'installer/assets'
foreach ($image in @(
    @{Name='wizard.bmp'; Width=492; Height=942},
    @{Name='header.bmp'; Width=450; Height=171}
)) {
    $path = Join-Path $assets $image.Name
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 54 -or
        [BitConverter]::ToUInt16($bytes,0) -ne 0x4D42 -or
        [BitConverter]::ToUInt32($bytes,2) -ne $bytes.Length -or
        [BitConverter]::ToUInt32($bytes,10) -ne 54 -or
        [BitConverter]::ToUInt32($bytes,14) -ne 40 -or
        [BitConverter]::ToInt32($bytes,18) -ne $image.Width -or
        [BitConverter]::ToInt32($bytes,22) -ne $image.Height -or
        [BitConverter]::ToUInt16($bytes,26) -ne 1 -or
        [BitConverter]::ToUInt16($bytes,28) -ne 24 -or
        [BitConverter]::ToUInt32($bytes,30) -ne 0) {
        throw "Invalid high-DPI bitmap: $path"
    }
    $stride = (($image.Width * 3 + 3) -band (-bnot 3))
    if ($bytes.Length -ne (54 + $stride * $image.Height)) {
        throw "Invalid bitmap pixel data length: $path"
    }
}

if (-not ('WinSquashfsManifestTest' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class WinSquashfsManifestTest
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint SizeofResource(IntPtr module, IntPtr resource);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr LoadResource(IntPtr module, IntPtr resource);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LockResource(IntPtr resource);

    [DllImport("kernel32.dll")]
    private static extern bool FreeLibrary(IntPtr module);

    public static string Read(string path)
    {
        IntPtr module = LoadLibraryExW(path, IntPtr.Zero, 0x22);
        if (module == IntPtr.Zero)
            throw new InvalidOperationException("Cannot open executable resources: " + path);
        try
        {
            IntPtr resource = FindResourceW(module, new IntPtr(1), new IntPtr(24));
            if (resource == IntPtr.Zero)
                throw new InvalidOperationException("Application manifest is missing: " + path);
            int size = checked((int)SizeofResource(module, resource));
            IntPtr data = LockResource(LoadResource(module, resource));
            if (size == 0 || data == IntPtr.Zero)
                throw new InvalidOperationException("Cannot read application manifest: " + path);
            byte[] bytes = new byte[size];
            Marshal.Copy(data, bytes, 0, size);
            return Encoding.UTF8.GetString(bytes).TrimStart('\uFEFF').TrimEnd('\0');
        }
        finally { FreeLibrary(module); }
    }
}
'@
}

foreach ($exe in @($Setup,$Uninstaller)) {
    $path = (Resolve-Path -LiteralPath $exe).Path
    [xml]$manifest = [WinSquashfsManifestTest]::Read($path)
    $legacy = $manifest.SelectSingleNode("//*[local-name()='dpiAware' and namespace-uri()='http://schemas.microsoft.com/SMI/2005/WindowsSettings']")
    $modern = $manifest.SelectSingleNode("//*[local-name()='dpiAwareness' and namespace-uri()='http://schemas.microsoft.com/SMI/2016/WindowsSettings']")
    if (!$legacy -or $legacy.InnerText.Trim() -ne 'true') {
        throw "System DPI awareness is missing: $path"
    }
    if (!$modern -or ($modern.InnerText -replace '\s','') -ne 'PerMonitorV2,System') {
        throw "Per-monitor DPI awareness is missing: $path"
    }
}
Write-Host 'PASS high-DPI installer artwork and application manifests'
