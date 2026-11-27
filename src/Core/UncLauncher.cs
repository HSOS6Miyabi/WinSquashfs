// SPDX-License-Identifier: GPL-3.0-only
using System.Diagnostics;

namespace WinSquashfs.Core;

public static class UncLauncher
{
    public static async Task<string> ResolveAsync(string path, CancellationToken token = default)
    {
        string full = UncShare.NormalizeSourcePath(path);
        var settings = UncSettings.Load();
        string unc = settings.GetImagePath(full) ?? throw new IOException(Localization.T("Image is outside the configured UNC shares. Configure a source directory or enable drive mappings."));
        await UncAgentControl.StartAsync(token);
        try
        {
            var attributes = await Task.Run(() => File.GetAttributes(unc), token);
            if ((attributes & FileAttributes.Directory) == 0)
                throw new IOException(Localization.T("Source is not a regular file."));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            throw new IOException(Localization.T("The image cannot be opened through its UNC path: {0}", unc) +
                Environment.NewLine + ex.Message, ex);
        }
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
