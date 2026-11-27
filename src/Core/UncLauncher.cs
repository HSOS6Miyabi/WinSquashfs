// SPDX-License-Identifier: GPL-3.0-only
using System.Diagnostics;

namespace WinSquashfs.Core;

public static class UncLauncher
{
    public static async Task<string> ResolveAsync(string path, CancellationToken token = default)
    {
        string full = UncShare.NormalizeSourcePath(path);
        var settings = UncSettings.Load();
        string unc = settings.GetImagePath(full) ?? throw new IOException(Localization.T("Image is outside the configured UNC shares. Configure a local directory or enable local drives."));
        await UncAgentControl.StartAsync(token);
        if (!await Task.Run(() => Directory.Exists(unc), token))
            throw new IOException(Localization.T("The image cannot be opened through its UNC path: {0}", unc));
        return unc;
    }

    public static async Task OpenAsync(string path, CancellationToken token = default)
    {
        string unc = await ResolveAsync(path, token);
        token.ThrowIfCancellationRequested();
        using var process = Process.Start(new ProcessStartInfo("explorer.exe")
        {
            UseShellExecute = false,
            ArgumentList = { unc }
        });
    }
}
