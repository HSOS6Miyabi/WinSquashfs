// SPDX-License-Identifier: GPL-3.0-only
using Microsoft.UI.Xaml;

namespace WinSquashfs;
public partial class App : Application
{
    private MainWindow? window;
    public App() { InitializeComponent(); }
    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        window = new MainWindow();
        window.Activate();
        await window.ApplyCommandLineAsync(Environment.GetCommandLineArgs().Skip(1).ToArray());
    }
}
