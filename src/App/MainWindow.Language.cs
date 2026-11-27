// SPDX-License-Identifier: GPL-3.0-only
using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using WinSquashfs.Core;

namespace WinSquashfs;

public sealed partial class MainWindow
{
    private async void SaveLanguageClicked(object sender, RoutedEventArgs args)
    {
        if (operation != null || errorDialogOpen) return;
        try
        {
            string selected = LanguageBox.SelectedIndex switch { 1 => "en-US", 2 => "zh-CN", _ => Localization.SystemLanguage };
            if (selected == Localization.Preference) return;
            Localization.SavePreference(selected);
            StatusText.Text = Localization.T("Language saved. Restart WinSquashfs to apply the change.");
            if (mounts.Count > 0)
            {
                await ShowErrorAsync(new InvalidOperationException(Localization.T("Unmount all images before restarting. Your language preference has been saved.")));
                return;
            }
            errorDialogOpen = true;
            try
            {
                var dialog = new ContentDialog
                {
                    Title = Localization.T("Restart WinSquashfs?"),
                    Content = new TextBlock { Text = Localization.T("The application will close the current window and reopen. Unsaved input will be lost."), TextWrapping = TextWrapping.Wrap, MaxWidth = 420 },
                    PrimaryButtonText = Localization.T("Restart now"),
                    CloseButtonText = Localization.T("Later"),
                    DefaultButton = ContentDialogButton.Close,
                    XamlRoot = Root.XamlRoot
                };
                if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
                string exe = Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, "WinSquashfs.exe");
                var start = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = AppContext.BaseDirectory };
                if (archive != null) { start.ArgumentList.Add("--open"); start.ArgumentList.Add(archive.Path); }
                using var process = Process.Start(start) ?? throw new IOException(Localization.T("Operation failed"));
                Close();
            }
            finally { errorDialogOpen = false; }
        }
        catch (Exception ex) { await ShowErrorAsync(ex); }
    }
}
