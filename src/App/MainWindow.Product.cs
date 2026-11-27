// SPDX-License-Identifier: GPL-3.0-only
using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using WinSquashfs.Core;

namespace WinSquashfs;
public sealed partial class MainWindow
{
    private void RefreshRecent()
    {
        RecentMenu.Items.Clear();
        var paths = RecentImages.Load();
        if (paths.Count == 0)
        {
            RecentMenu.Items.Add(new MenuFlyoutItem { Text = Localization.T("No recent images"), IsEnabled = false });
            return;
        }
        foreach (var path in paths)
        {
            var item = new MenuFlyoutItem { Text = path };
            item.Click += async (_, _) =>
            {
                if (!File.Exists(path))
                {
                    RecentImages.Remove(path);
                    RefreshRecent();
                    await ShowErrorAsync(new FileNotFoundException(Localization.T("Image no longer exists."), path));
                    return;
                }
                await LoadAsync(path);
            };
            RecentMenu.Items.Add(item);
        }
        RecentMenu.Items.Add(new MenuFlyoutSeparator());
        var clear = new MenuFlyoutItem { Text = Localization.T("Clear recent images") };
        clear.Click += (_, _) => { RecentImages.Clear(); RefreshRecent(); };
        RecentMenu.Items.Add(clear);
    }

    private void ImageDragOver(object sender, DragEventArgs e)
    {
        e.AcceptedOperation = operation == null && e.DataView.Contains(StandardDataFormats.StorageItems)
            ? DataPackageOperation.Copy : DataPackageOperation.None;
    }
    private async void ImageDropped(object sender, DragEventArgs e)
    {
        if (operation != null || !e.DataView.Contains(StandardDataFormats.StorageItems)) return;
        try
        {
            var items = await e.DataView.GetStorageItemsAsync();
            if (items.Count == 1 && items[0] is StorageFile file)
                await LoadAsync(file.Path);
            else StatusText.Text = Localization.T("Drop one SquashFS image.");
        }
        catch (Exception ex) { await ShowErrorAsync(ex); }
    }

    private async void VerifyClicked(object sender, RoutedEventArgs e)
    {
        var image = archive;
        if (image == null || operation != null) return;
        ArchiveVerification? report = null;
        if (await RunAsync(Localization.T("Verifying image…"), async token =>
        {
            report = await Task.Run(() => image.Verify((files, bytes) =>
                DispatcherQueue.TryEnqueue(() => StatusText.Text = Localization.T("Verified {0:N0} files · {1}", files, FormatSize(bytes))), token), token);
        }))
        {
            var result = report!;
            StatusText.Text = Localization.T("Verified {0:N0} files · {1}", result.Files, FormatSize(result.Bytes));
        }
    }

    private void LoadUncSettings()
    {
        UncSettings settings;
        try { settings = UncSettings.Load(); }
        catch (Exception ex) { settings = UncSettings.Default; StatusText.Text = Localization.T("UNC settings: {0}", ex.Message); }
        UncRootBox.Text = settings.Root;
        UncAliasBox.Text = settings.Alias;
        UncDrivesBox.IsChecked = settings.IncludeDrives;
        UncIdleBox.Value = settings.IdleSeconds;
        UncLimitBox.Value = settings.MaxIdleImages;
        UncLoginBox.IsChecked = settings.StartAtLogin;
        UpdateUncButtons();
    }
    private UncSettings ReadUncSettings() => new(UncRootBox.Text.Trim(), UncAliasBox.Text.Trim(),
        UncDrivesBox.IsChecked == true, OptionalInt(UncIdleBox) ?? 60,
        OptionalInt(UncLimitBox) ?? 8, UncLoginBox.IsChecked == true);

    private void UpdateUncButtons()
    {
        if (!ready) return;
        try
        {
            bool running = UncAgentControl.IsRunning;
            UncStatus.Text = running ? Localization.T("Running") : Localization.T("Stopped");
            bool idle = operation == null;
            StartUncButton.IsEnabled = idle && !running;
            StopUncButton.IsEnabled = idle && running;
            UncApplyButton.IsEnabled = idle;
            CopyUncButton.IsEnabled = idle && UncImagePathBox.Text.Length > 0;
            OpenUncButton.IsEnabled = idle && UncImagePathBox.Text.Length > 0;
        }
        catch (Exception ex) { UncStatus.Text = ex.Message; }
    }

    private void RefreshUncStatus()
    {
        if (!ready) return;
        try
        {
            var settings = UncSettings.Load();
            var shares = settings.Shares();
            UncShares.Text = string.Join(Environment.NewLine, shares.Select(s => s.UncPath + "  →  " + (s.IsNetworkNamespace ? Localization.T("Network shares") : s.Root)));
            UncImagePathBox.Text = uncTargetPath == null ? "" : settings.GetImagePath(uncTargetPath) ?? "";
        }
        catch (Exception ex)
        {
            UncShares.Text = ex.Message;
            UncImagePathBox.Text = "";
        }
        UpdateUncButtons();
    }
    private async void UncRootClicked(object sender, RoutedEventArgs e)
    {
        try { string? path = await PickFolderAsync(); if (path != null) UncRootBox.Text = path; }
        catch (Exception ex) { await ShowErrorAsync(ex); }
    }
    private async void StartUncClicked(object sender, RoutedEventArgs e)
    {
        await RunAsync(Localization.T("Starting direct UNC access…"), async token =>
        {
            ReadUncSettings().Save();
            await UncAgentControl.StartAsync(token);
        });
    }
    private async void StopUncClicked(object sender, RoutedEventArgs e)
    {
        await RunAsync(Localization.T("Stopping direct UNC access…"), token => UncAgentControl.StopAsync(token));
    }
    private async void UncApplyClicked(object sender, RoutedEventArgs e)
    {
        await RunAsync(Localization.T("Applying UNC settings…"), async token =>
        {
            var settings = ReadUncSettings();
            _ = settings.Shares();
            bool running = UncAgentControl.IsRunning;
            if (running) await UncAgentControl.StopAsync(token);
            settings.Save();
            if (running) await UncAgentControl.StartAsync(token);
        });
    }
    private void CopyUncClicked(object sender, RoutedEventArgs e)
    {
        try
        {
            var package = new DataPackage();
            package.SetText(UncImagePathBox.Text);
            Clipboard.SetContent(package);
            StatusText.Text = Localization.T("UNC path copied.");
        }
        catch (Exception ex) { StatusText.Text = ex.Message; }
    }
    private async void OpenUncClicked(object sender, RoutedEventArgs e)
    {
        if (uncTargetPath != null) await OpenDirectImageAsync(uncTargetPath);
    }
    private async Task OpenDirectImageAsync(string path)
    {
        await RunAsync(Localization.T("Opening image via UNC…"), async token =>
        {
            uncTargetPath = Path.GetFullPath(path);
            await UncLauncher.OpenAsync(uncTargetPath, token);
        });
    }
}
