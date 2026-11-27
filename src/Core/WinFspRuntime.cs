// SPDX-License-Identifier: GPL-3.0-only
using Fsp.Interop;

namespace WinSquashfs.Core;

public static class WinFspRuntime
{
    public static RuntimeInfo Check()
    {
        try
        {
            return RuntimeDependency.Initialize();
        }
        catch (Exception ex) when (ex is DllNotFoundException or TypeLoadException or TypeInitializationException
            or EntryPointNotFoundException or BadImageFormatException or IOException or UnauthorizedAccessException
            or System.ComponentModel.Win32Exception or System.Security.SecurityException)
        {
            throw new IOException(Localization.T(
                "WinFsp could not initialize. Install or update WinFsp to {0} or later in the same major version using the WinSquashfs installer or prerequisites\\WinFsp.msi, then restart the application.\n\nDetails: {1}",
                RuntimeDependency.MinimumVersion.ToString(3), ex.GetBaseException().Message), ex);
        }
    }

    public static string Describe()
    {
        var info = Check();
        return Localization.T("WinFsp runtime {0}\n{1}", info.Version.ToString(3), info.DllPath);
    }
}
