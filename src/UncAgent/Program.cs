// SPDX-License-Identifier: GPL-3.0-only
using WinSquashfs.Core;

try
{
    Localization.Initialize();
    if (args is ["--stop"])
    {
        UncAgentControl.StopAsync().GetAwaiter().GetResult();
        return 0;
    }
    using var mutex = UncAgentControl.CreateInstanceMutex();
    bool owned;
    try { owned = mutex.WaitOne(0); }
    catch (AbandonedMutexException) { owned = true; }
    if (!owned) return 0;
    try
    {
        using var stop = UncAgentControl.CreateStopEvent();
        using var ready = UncAgentControl.CreateReadyEvent();
        var settings = UncSettings.Load();
        using var session = new UncSession(settings.Shares(), settings.IdleSeconds, settings.MaxIdleImages);
        if (File.Exists(UncSettings.LogPath)) File.Delete(UncSettings.LogPath);
        ready.Set();
        stop.WaitOne();
        ready.Reset();
    }
    finally { mutex.ReleaseMutex(); }
    return 0;
}
catch (Exception ex) { UncAgentControl.Log(ex); return 1; }
