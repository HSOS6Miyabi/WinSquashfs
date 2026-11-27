// SPDX-License-Identifier: GPL-3.0-only
using Microsoft.Win32.SafeHandles;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace WinSquashfs.Core;

internal static class WindowsPath
{
    private const uint ShareAll = 7;
    private const uint OpenExisting = 3;
    private const uint BackupSemantics = 0x02000000;
    private const uint VolumeNameNt = 2;

    [DllImport("kernel32.dll", EntryPoint = "CreateFileW", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security,
        uint creation, uint flags, IntPtr template);

    [DllImport("kernel32.dll", EntryPoint = "GetFinalPathNameByHandleW", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandle(SafeFileHandle file, StringBuilder path, uint capacity, uint flags);

    internal static string ResolveDirectory(string path)
    {
        using var handle = CreateFile(path, 0, ShareAll, IntPtr.Zero, OpenExisting, BackupSemantics, IntPtr.Zero);
        if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
        var buffer = new StringBuilder(512);
        while (true)
        {
            uint length = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Capacity, VolumeNameNt);
            if (length == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (length < buffer.Capacity) return buffer.ToString().TrimEnd('\\');
            buffer.EnsureCapacity(checked((int)length + 1));
        }
    }

    internal static bool ContainsDirectory(string root, string directory)
    {
        string source = ResolveDirectory(root);
        string target = ResolveDirectory(directory);
        return target.Equals(source, StringComparison.OrdinalIgnoreCase) ||
            target.StartsWith(source + "\\", StringComparison.OrdinalIgnoreCase);
    }
}
