// SPDX-License-Identifier: GPL-3.0-only
using Microsoft.Win32.SafeHandles;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Principal;

namespace WinSquashfs.Core;

// A process launched with a split UAC administrator token must not expose its
// elevated file access through a UNC volume readable by the ordinary user.
internal sealed class LocalReadIdentity : IDisposable
{
    private readonly SafeAccessTokenHandle? limited;
    private const int TokenElevationType = 18;
    private const int TokenLinkedToken = 19;
    private const int TokenElevationTypeFull = 2;

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetTokenInformation(SafeAccessTokenHandle token, int type, IntPtr information,
        int length, out int returnedLength);

    internal LocalReadIdentity()
    {
        using var identity = WindowsIdentity.GetCurrent();
        IntPtr buffer = Marshal.AllocHGlobal(IntPtr.Size);
        try
        {
            if (!GetTokenInformation(identity.AccessToken, TokenElevationType, buffer, sizeof(int), out _))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            if (Marshal.ReadInt32(buffer) != TokenElevationTypeFull) return;
            if (!GetTokenInformation(identity.AccessToken, TokenLinkedToken, buffer, IntPtr.Size, out _))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            limited = new SafeAccessTokenHandle(Marshal.ReadIntPtr(buffer));
            if (limited.IsInvalid) throw new UnauthorizedAccessException("Cannot obtain the ordinary user access token.");
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }
    internal T Run<T>(Func<T> action) => limited == null ? action() : WindowsIdentity.RunImpersonated(limited, action);
    public void Dispose() => limited?.Dispose();
}
