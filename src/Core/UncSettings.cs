// SPDX-License-Identifier: GPL-3.0-only
using System.Security.Principal;
using System.Text.Json;
using Microsoft.Win32;

namespace WinSquashfs.Core;

public sealed record UncSettings(string Root, string Alias = "path", bool IncludeDrives = true,
    int IdleSeconds = 60, int MaxIdleImages = 8, bool StartAtLogin = false)
{
    private static readonly string DirectoryPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WinSquashfs");
    public static string SettingsPath => Path.Combine(DirectoryPath, "unc.json");
    public static string LogPath => Path.Combine(DirectoryPath, "unc.log");
    public static UncSettings Default => new(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));

    public static UncSettings Load()
    {
        if (!File.Exists(SettingsPath)) return Default;
        return JsonSerializer.Deserialize<UncSettings>(File.ReadAllText(SettingsPath)) ?? throw new IOException(Localization.T("Invalid UNC settings."));
    }
    public IReadOnlyList<UncShare> Shares()
    {
        if (IdleSeconds is < 0 or > 86400 || MaxIdleImages is < 0 or > 1024) throw new ArgumentException(Localization.T("Invalid cache limits."));
        var result = new List<UncShare> { new UncShare(Alias, Root).Validate() };
        if (IncludeDrives)
        {
            foreach (var drive in DriveInfo.GetDrives())
            {
                UncShare share;
                try
                {
                    // Include every accessible drive type, including mapped
                    // network drives, SUBST drives, and other file systems.
                    if (UncShare.IsProjectedPath(drive.RootDirectory.FullName)) continue;
                    share = new UncShare(drive.Name[..1].ToUpperInvariant(), drive.RootDirectory.FullName).Validate();
                }
                catch (IOException) { continue; }
                catch (UnauthorizedAccessException) { continue; }
                if (result.Any(s => string.Equals(s.Name, share.Name, StringComparison.OrdinalIgnoreCase)))
                    throw new ArgumentException(Localization.T("Choose an alias different from a drive letter."));
                result.Add(share);
            }
        }
        result.Add(UncShare.NetworkNamespace);
        return result;
    }
    public string? GetImagePath(string path)
    {
        path = UncShare.NormalizeSourcePath(path);
        if (path.StartsWith(@"\\", StringComparison.Ordinal))
            return UncShare.NetworkNamespace.ImagePath(path);
        return Shares().Select(s => s.ImagePath(path)).FirstOrDefault(p => p != null);
    }

    public void Save()
    {
        _ = Shares();
        Directory.CreateDirectory(DirectoryPath);
        string temp = SettingsPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temp, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
            File.Move(temp, SettingsPath, true);
        }
        finally { if (File.Exists(temp)) File.Delete(temp); }
        using var key = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run");
        if (StartAtLogin) key.SetValue("WinSquashfsUNC", "\"" + UncAgentControl.ExecutablePath + "\"");
        else key.DeleteValue("WinSquashfsUNC", false);
    }
}

public static class UncAgentControl
{
    private static string Name(string suffix)
    {
        using var identity = WindowsIdentity.GetCurrent();
        return @"Local\WinSquashfs.Unc." + identity.User!.Value + "." + suffix;
    }
    public static string ExecutablePath => Path.Combine(AppContext.BaseDirectory, "WinSquashfs.Unc.exe");
    public static Mutex CreateInstanceMutex() => new(false, Name("Instance"));
    public static EventWaitHandle CreateStopEvent() => new(false, EventResetMode.ManualReset, Name("Stop"));
    public static EventWaitHandle CreateReadyEvent() => new(false, EventResetMode.ManualReset, Name("Ready"));
    private static bool IsStartingOrRunning
    {
        get
        {
            if (!EventWaitHandle.TryOpenExisting(Name("Stop"), out var e)) return !InstanceStopped();
            e.Dispose(); return true;
        }
    }
    public static bool IsRunning
    {
        get
        {
            if (!EventWaitHandle.TryOpenExisting(Name("Ready"), out var e)) return false;
            using (e) return e.WaitOne(0);
        }
    }

    public static async Task StartAsync(CancellationToken token = default)
    {
        token.ThrowIfCancellationRequested();
        if (IsRunning) return;
        WinFspRuntime.Check();
        if (!File.Exists(ExecutablePath)) throw new FileNotFoundException(Localization.T("UNC background application is missing."), ExecutablePath);
        using var p = DetachedProcess.Start(ExecutablePath);
        try
        {
            for (int i = 0; i < 150; i++)
            {
                if (IsRunning) return;
                token.ThrowIfCancellationRequested();
                if (p.HasExited && !IsStartingOrRunning) break;
                await Task.Delay(100, token);
            }
            if (IsRunning) return;
            if (!p.HasExited)
            {
                p.Terminate();
                throw new IOException(Localization.T("UNC background application did not become ready."));
            }
        }
        catch (OperationCanceledException)
        {
            if (IsRunning) return;
            p.Terminate();
            throw;
        }
        string error = File.Exists(UncSettings.LogPath) ? File.ReadAllText(UncSettings.LogPath) : "";
        throw new IOException(Localization.T("UNC background application could not start. {0}", error));
    }

    private static bool InstanceStopped()
    {
        if (!Mutex.TryOpenExisting(Name("Instance"), out var mutex)) return true;
        using (mutex)
        {
            bool acquired;
            try { acquired = mutex.WaitOne(0); }
            catch (AbandonedMutexException) { acquired = true; }
            if (acquired) mutex.ReleaseMutex();
            return acquired;
        }
    }

    public static async Task StopAsync(CancellationToken token = default)
    {
        token.ThrowIfCancellationRequested();
        for (int i = 0; i < 150; i++)
        {
            if (EventWaitHandle.TryOpenExisting(Name("Stop"), out var stop))
            {
                using (stop) stop.Set();
            }
            if (InstanceStopped()) return;
            await Task.Delay(100);
        }
        throw new IOException(Localization.T("UNC background application did not stop."));
    }

    public static void Log(Exception error)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(UncSettings.LogPath)!);
            File.WriteAllText(UncSettings.LogPath, DateTimeOffset.Now.ToString("O") + "\n" + error);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
