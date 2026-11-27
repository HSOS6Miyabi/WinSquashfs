// SPDX-License-Identifier: GPL-3.0-only
using Microsoft.UI.Xaml;
using WinSquashfs.Core;

namespace WinSquashfs;
public partial class App : Application
{
    private MainWindow? window;

    internal static void RecordException(Exception? exception)
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
        try
        {
            Localization.Initialize();
            InitializeComponent();
        }
        catch (Exception e) { RecordException(e); throw; }
    }

    [System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "MessageBoxW", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    private static extern int MessageBox(IntPtr window, string text, string title, uint type);

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            string[] commandLine = Environment.GetCommandLineArgs().Skip(1).ToArray();
            if (commandLine is ["--eject", var drive])
            {
                try { await MountEject.RequestAsync(drive); }
                catch (Exception ex)
                {
                    RecordException(ex);
                    MessageBox(IntPtr.Zero, ex.Message, "WinSquashfs", 0x10);
                    Environment.ExitCode = 1;
                }
                Exit();
                return;
            }
            LocalizationResources.Install(Resources);
            window = new MainWindow();
            window.Activate();
            await window.ApplyCommandLineAsync(commandLine);
        }
        catch (Exception e) { RecordException(e); throw; }
    }
}
