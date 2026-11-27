// SPDX-License-Identifier: GPL-3.0-only
using System.Diagnostics;
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
        return JsonSerializer.Deserialize<UncSettings>(File.ReadAllText(SettingsPath)) ?? throw new IOException("Invalid UNC settings.");
    }
    public IReadOnlyList<UncShare> Shares()
    {
        if (IdleSeconds is < 0 or > 86400 || MaxIdleImages is < 0 or > 1024) throw new ArgumentException("Invalid cache limits.");
        var result = new List<UncShare> { new UncShare(Alias, Root).Validate() };
        if (IncludeDrives)
        {
            foreach (var drive in DriveInfo.GetDrives())
            {
                UncShare share;
                try
                {
                    if (drive.DriveType is not (DriveType.Fixed or DriveType.Removable) || !drive.IsReady ||
                        string.Equals(drive.DriveFormat, "SquashFS", StringComparison.OrdinalIgnoreCase)) continue;
                    share = new UncShare(drive.Name[..1].ToUpperInvariant(), drive.RootDirectory.FullName).Validate();
                }
                catch (IOException) { continue; }
                catch (UnauthorizedAccessException) { continue; }
                if (result.Any(s => string.Equals(s.Name, share.Name, StringComparison.OrdinalIgnoreCase)))
                    throw new ArgumentException("Choose an alias different from a drive letter.");
                result.Add(share);
            }
        }
        return result;
    }
    public string? GetImagePath(string path) => Shares().Select(s => s.ImagePath(path)).FirstOrDefault(p => p != null);

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
            if (!EventWaitHandle.TryOpenExisting(Name("Stop"), out var e)) return false;
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
        if (IsRunning) return;
        if (!File.Exists(ExecutablePath)) throw new FileNotFoundException("UNC background application is missing.", ExecutablePath);
        using var p = Process.Start(new ProcessStartInfo(ExecutablePath) { UseShellExecute = false, CreateNoWindow = true })
            ?? throw new IOException("Cannot start UNC background application.");
        for (int i = 0; i < 150; i++)
        {
            token.ThrowIfCancellationRequested();
            if (IsRunning) return;
            if (p.HasExited && !IsStartingOrRunning) break;
            await Task.Delay(100, token);
        }
        if (IsRunning) return;
        if (!p.HasExited) throw new IOException("UNC background application did not become ready.");
        string error = File.Exists(UncSettings.LogPath) ? File.ReadAllText(UncSettings.LogPath) : "";
        throw new IOException("UNC background application could not start. " + error);
    }

    public static async Task StopAsync(CancellationToken token = default)
    {
        if (EventWaitHandle.TryOpenExisting(Name("Stop"), out var stop))
        {
            using (stop) stop.Set();
            for (int i = 0; i < 150; i++)
            {
                token.ThrowIfCancellationRequested();
                if (!EventWaitHandle.TryOpenExisting(Name("Stop"), out var running)) return;
                running.Dispose();
                await Task.Delay(100, token);
            }
            throw new IOException("UNC background application did not stop.");
        }
    }

    public static void Log(Exception error)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(UncSettings.LogPath)!);
            File.WriteAllText(UncSettings.LogPath, DateTimeOffset.Now.ToString("O") + "\n" + error.Message);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
