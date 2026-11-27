// SPDX-License-Identifier: GPL-3.0-only
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace WinSquashfs.Core;

internal sealed class DetachedProcess : IDisposable
{
    private const uint CreateNoWindow = 0x08000000;
    private const uint WaitObject0 = 0;
    private const uint WaitTimeout = 258;
    private readonly SafeProcessHandle handle;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo
    {
        internal uint cb;
        internal IntPtr reserved, desktop, title;
        internal uint x, y, width, height, xChars, yChars, fillAttribute, flags;
        internal ushort showWindow, reserved2Size;
        internal IntPtr reserved2, stdin, stdout, stderr;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation
    {
        internal IntPtr process, thread;
        internal uint processId, threadId;
    }

    [DllImport("kernel32.dll", EntryPoint = "CreateProcessW", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateProcess(string applicationName, StringBuilder commandLine,
        IntPtr processAttributes, IntPtr threadAttributes, [MarshalAs(UnmanagedType.Bool)] bool inheritHandles,
        uint creationFlags, IntPtr environment, string currentDirectory,
        ref StartupInfo startupInfo, out ProcessInformation processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(SafeProcessHandle process, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateProcess(SafeProcessHandle process, uint exitCode);

    private DetachedProcess(SafeProcessHandle handle) => this.handle = handle;

    internal static DetachedProcess Start(string executable)
    {
        executable = Path.GetFullPath(executable);
        var info = new StartupInfo { cb = (uint)Marshal.SizeOf<StartupInfo>() };
        var command = new StringBuilder("\"" + executable + "\"");
        if (!CreateProcess(executable, command, IntPtr.Zero, IntPtr.Zero, false, CreateNoWindow,
            IntPtr.Zero, Path.GetDirectoryName(executable)!, ref info, out var process))
            throw new Win32Exception(Marshal.GetLastWin32Error());

        using var thread = new SafeWaitHandle(process.thread, true);
        return new DetachedProcess(new SafeProcessHandle(process.process, true));
    }

    private bool Wait(uint milliseconds)
    {
        return WaitForSingleObject(handle, milliseconds) switch
        {
            WaitObject0 => true,
            WaitTimeout => false,
            _ => throw new Win32Exception(Marshal.GetLastWin32Error())
        };
    }

    internal bool HasExited => Wait(0);

    internal void Terminate()
    {
        if (HasExited) return;
        if (!TerminateProcess(handle, 1))
        {
            int error = Marshal.GetLastWin32Error();
            if (!HasExited) throw new Win32Exception(error);
        }
        if (!Wait(5000)) throw new TimeoutException(Localization.T("UNC background application did not stop."));
    }

    public void Dispose() => handle.Dispose();
}
