// SPDX-License-Identifier: GPL-3.0-only
using Microsoft.UI.Xaml;

namespace WinSquashfs;
public partial class App : Application
{
    private MainWindow? window;

    private static void RecordException(Exception? exception)
    {
        if (exception == null) return;
        try
        {
            var path = Environment.GetEnvironmentVariable("WINSQUASHFS_DIAGNOSTICS");
            if (string.IsNullOrEmpty(path))
            {
                var folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WinSquashfs");
                Directory.CreateDirectory(folder);
                path = Path.Combine(folder, "application.log");
            }
            File.AppendAllText(path, DateTimeOffset.UtcNow.ToString("O") + Environment.NewLine + exception + Environment.NewLine);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    public App()
    {
        UnhandledException += (_, args) => RecordException(args.Exception);
        AppDomain.CurrentDomain.UnhandledException += (_, args) => RecordException(args.ExceptionObject as Exception);
        try { InitializeComponent(); }
        catch (Exception e) { RecordException(e); throw; }
    }

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            window = new MainWindow();
            window.Activate();
            await window.ApplyCommandLineAsync(Environment.GetCommandLineArgs().Skip(1).ToArray());
        }
        catch (Exception e) { RecordException(e); throw; }
    }
}
