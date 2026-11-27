// SPDX-License-Identifier: GPL-3.0-only
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using WinSquashfs.Core;
using Windows.Storage.Pickers;
using Windows.System;

namespace WinSquashfs;

public sealed class BrowserRow
{
    public Entry Entry { get; }
    public string Name => Entry.Name;
    public string Kind => Entry.Kind;
    public string Glyph => Entry.IsDirectory ? "\uE8B7" : Entry.IsFile ? "\uE8A5" : "\uE71B";
    public string SizeText => Entry.IsFile ? MainWindow.FormatSize(Entry.Size) : "";
    public BrowserRow(Entry entry) { Entry = entry; }
    public override string ToString() => Name;
}

public sealed partial class MainWindow : Window
{
    private Archive? archive;
    private Entry? current;
    private readonly ObservableCollection<MountedImage> mounts = new();
    private CancellationTokenSource? operation;
    private int previewGeneration;
    private bool ready;
    private bool errorDialogOpen;
    private bool closing;
    private string? uncTargetPath;
    public MainWindow()
    {
        InitializeComponent();
        Root.Language = Localization.Language;
        LanguageBox.ItemsSource = new[] { Localization.T("Follow Windows"), "English", "简体中文" };
        LanguageBox.SelectedIndex = Localization.Preference switch { "en-US" => 1, "zh-CN" => 2, _ => 0 };
        var display = Microsoft.UI.Windowing.DisplayArea.GetFromWindowId(AppWindow.Id, Microsoft.UI.Windowing.DisplayAreaFallback.Primary);
        var area = display.WorkArea;
        double scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        if (scale <= 0) scale = 1;
        if (AppWindow.Presenter is Microsoft.UI.Windowing.OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = Math.Min(area.Width, (int)(820 * scale));
            presenter.PreferredMinimumHeight = Math.Min(area.Height, (int)(540 * scale));
        }
        AppWindow.Resize(new Windows.Graphics.SizeInt32(Math.Min(area.Width, (int)(1200 * scale)), Math.Min(area.Height, (int)(780 * scale))));
        try { SystemBackdrop = new MicaBackdrop(); } catch { }
        MountedList.ItemsSource = mounts;
        CodecBox.ItemsSource = new[] { "zstd", "gzip", "xz", "lz4", "lzo" };
        BlockBox.ItemsSource = new[] { "4 KiB", "8 KiB", "16 KiB", "32 KiB", "64 KiB", "128 KiB", "256 KiB", "512 KiB", "1024 KiB" };
        LzoAlgorithm.ItemsSource = new[] { Localization.T("Default"), "lzo1x_1", "lzo1x_1_11", "lzo1x_1_12", "lzo1x_1_15", "lzo1x_999" };
        CodecBox.SelectionChanged += (_,_) => { if (ready) LevelBox.Value = double.NaN; UpdateCodec(); };
        LzoAlgorithm.SelectionChanged += (_,_) => UpdateLzoLevel();
        NoFragments.Checked += (_,_) => AlwaysFragments.IsChecked = false;
        AlwaysFragments.Checked += (_,_) => NoFragments.IsChecked = false;
        ready = true;
        LoadUncSettings();
        RefreshRecent();
        Reset();
        RefreshDrives();
        Navigation.SelectedItem = BrowseNav;
        RefreshEntries();
        var uncTimer = DispatcherQueue.CreateTimer();
        uncTimer.Interval = TimeSpan.FromSeconds(1);
        uncTimer.Tick += (_, _) => { if (UncPage.Visibility == Visibility.Visible) UpdateUncButtons(); };
        uncTimer.Start();
        AppWindow.Closing += (_, e) =>
        {
            if (operation != null)
            {
                e.Cancel = true;
                closing = true;
                operation.Cancel();
                StatusText.Text = Localization.T("Cancelling the current operation…");
            }
        };
        Closed += (_,_) =>
        {
            uncTimer.Stop();
            ++previewGeneration;
            foreach (var mount in mounts.ToArray()) mount.Dispose();
            archive?.Dispose();
        };
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr window);

    internal async Task ApplyCommandLineAsync(string[] args)
    {
        try
        {
            if (args.Length == 0) return;
            if (args is ["--unc", var directImage])
            {
                Navigation.SelectedItem = UncNav;
                await OpenDirectImageAsync(directImage);
                return;
            }
            if (args is ["--pack", var folder])
            { SourceBox.Text = Path.GetFullPath(folder); OutputBox.Text = Path.TrimEndingDirectorySeparator(SourceBox.Text) + ".squashfs"; Navigation.SelectedItem = PackNav; return; }
            string mode = args.Length == 1 ? "--open" : args[0];
            string path = args.Length == 1 ? args[0] : args[1];
            if (mode is not ("--open" or "--mount" or "--extract") || args.Length > 2)
                throw new ArgumentException(Localization.T("Usage: WinSquashfs [--open|--mount|--extract] image or --pack directory"));
            await LoadAsync(path);
            if (archive == null) return;
            if (mode == "--mount") await MountAsync();
            if (mode == "--extract") await ExtractAllAsync();
        }
        catch (Exception e) { await ShowErrorAsync(e); }
    }

    private void NavigationChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs e)
    {
        if (!ready || e.SelectedItem is not NavigationViewItem item) return;
        BrowsePage.Visibility = (string)item.Tag == "browse" ? Visibility.Visible : Visibility.Collapsed;
        PackPage.Visibility = (string)item.Tag == "pack" ? Visibility.Visible : Visibility.Collapsed;
        MountPage.Visibility = (string)item.Tag == "mount" ? Visibility.Visible : Visibility.Collapsed;
        ToolsPage.Visibility = (string)item.Tag == "tools" ? Visibility.Visible : Visibility.Collapsed;
        UncPage.Visibility = (string)item.Tag == "unc" ? Visibility.Visible : Visibility.Collapsed;
        if ((string)item.Tag == "unc") RefreshUncStatus();
        if ((string)item.Tag == "mount") RefreshDrives();
    }

    internal static string FormatSize(ulong n)
    {
        double v = n; string[] units = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
        int i = 0; while (v >= 1024 && i < units.Length - 1) { v /= 1024; i++; }
        return i == 0 ? $"{n} B" : $"{v:0.##} {units[i]}";
    }

    private async Task ShowErrorAsync(Exception e)
    {
        App.RecordException(e);
        string message = e is TypeInitializationException ? e.GetBaseException().Message : e.Message;
        StatusText.Text = message;
        if (errorDialogOpen || closing || Root.XamlRoot == null) return;
        errorDialogOpen = true;
        try
        {
            var dialog = new ContentDialog { Title = Localization.T("Operation failed"),
                Content = new ScrollViewer { MaxHeight = 340, Content = new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap, MaxWidth = 520, IsTextSelectionEnabled = true } },
                CloseButtonText = Localization.T("Close"), XamlRoot = Root.XamlRoot };
            await dialog.ShowAsync();
        }
        finally { errorDialogOpen = false; }
    }

    private async Task<bool> RunAsync(string title, Func<CancellationToken,Task> action)
    {
        if (operation != null) return false;
        using var cts = new CancellationTokenSource();
        operation = cts; BusyRing.IsActive = true; CancelButton.IsEnabled = true;
        OpenButton.IsEnabled = RecentButton.IsEnabled = CreateButton.IsEnabled = MountButton.IsEnabled = ExtractButton.IsEnabled = ExtractAllButton.IsEnabled = VerifyButton.IsEnabled = CloseImageButton.IsEnabled = false;
        StartUncButton.IsEnabled = StopUncButton.IsEnabled = UncApplyButton.IsEnabled = CopyUncButton.IsEnabled = OpenUncButton.IsEnabled = false;
        ResetPackButton.IsEnabled = SaveLanguageButton.IsEnabled = LanguageBox.IsEnabled = false;
        UpdateMountButtons();
        StatusText.Text = title;
        try { await action(cts.Token); StatusText.Text = Localization.T("Completed"); return true; }
        catch (OperationCanceledException) { StatusText.Text = Localization.T("Cancelled"); return false; }
        catch (Exception e) { await ShowErrorAsync(e); return false; }
        finally
        {
            operation = null; BusyRing.IsActive = false; CancelButton.IsEnabled = false;
            OpenButton.IsEnabled = RecentButton.IsEnabled = CreateButton.IsEnabled = ResetPackButton.IsEnabled = SaveLanguageButton.IsEnabled = LanguageBox.IsEnabled = true;
            UpdateButtons();
            if (UncPage.Visibility == Visibility.Visible) RefreshUncStatus();
            if (closing) DispatcherQueue.TryEnqueue(() => Close());
        }
    }

    private void CancelClicked(object sender, RoutedEventArgs e) => operation?.Cancel();

    private void InitPicker(object picker)
        => WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));

    private async Task<string?> PickFolderAsync()
    {
        var picker = new FolderPicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary };
        picker.FileTypeFilter.Add("*"); InitPicker(picker);
        return (await picker.PickSingleFolderAsync())?.Path;
    }

    private async void OpenClicked(object sender, RoutedEventArgs e)
    {
        try
        {
            var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary };
            picker.FileTypeFilter.Add("*");
            InitPicker(picker);
            var file = await picker.PickSingleFileAsync();
            if (file != null) await LoadAsync(file.Path);
        }
        catch (Exception ex) { await ShowErrorAsync(ex); }
    }

    private async Task LoadAsync(string path)
    {
        await RunAsync(Localization.T("Reading image metadata…"), async token =>
        {
            var next = await Task.Run(() => new Archive(path), token);
            if (token.IsCancellationRequested) { next.Dispose(); token.ThrowIfCancellationRequested(); }
            ++previewGeneration;
            archive?.Dispose();
            archive = next;
            uncTargetPath = next.Path;
            current = next.Root;
            RecentImages.Add(next.Path);
            RefreshRecent();
            ImageSummary.Text = Localization.T("{0} · {1} · {2} · {3} blocks", Path.GetFileName(next.Path), next.Codec.ToUpperInvariant(), FormatSize(next.ImageSize), FormatSize(next.BlockSize));
            SearchBox.Text = "";
            Navigation.SelectedItem = BrowseNav;
            RefreshEntries();
        });
    }

    private void RefreshEntries()
    {
        if (!ready) return;
        PathBox.Text = current?.WindowsPath ?? "\\";
        var filter = SearchBox.Text.Trim();
        Entries.ItemsSource = current?.Children
            .Where(e => e.Name.Contains(filter, StringComparison.OrdinalIgnoreCase))
            .OrderByDescending(e => e.IsDirectory).ThenBy(e => e.Name, StringComparer.OrdinalIgnoreCase)
            .Select(e => new BrowserRow(e)).ToArray() ?? Array.Empty<BrowserRow>();
        ++previewGeneration;
        Preview.Text = "";
        Details.Text = current == null ? Localization.T("Open an image and select an entry.") : Localization.T("{0} entries in {1}", current.Children.Count, current.ArchivePath);
        BrowserEmpty.Text = current == null ? Localization.T("Open or drop a SquashFS image to begin.") : Localization.T("No matching entries.");
        BrowserEmpty.Visibility = Entries.Items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        UpdateButtons();
    }

    private void UpdateButtons()
    {
        if (!ready) return;
        var selected = (Entries.SelectedItem as BrowserRow)?.Entry;
        bool idle = operation == null;
        CloseImageButton.IsEnabled = archive != null && idle;
        UpButton.IsEnabled = current?.Parent != null;
        EnterButton.IsEnabled = selected?.IsDirectory == true;
        MountButton.IsEnabled = ExtractAllButton.IsEnabled = archive != null && idle;
        ExtractButton.IsEnabled = selected != null && (selected.IsFile || selected.IsDirectory) && idle;
        VerifyButton.IsEnabled = archive != null && idle;
        UpdateMountButtons();
    }

    private void CloseImageClicked(object sender, RoutedEventArgs e)
    {
        if (operation != null) return;
        ++previewGeneration;
        archive?.Dispose();
        archive = null;
        current = null;
        uncTargetPath = null;
        ImageSummary.Text = Localization.T("Open a SquashFS image to explore its contents.");
        SearchBox.Text = "";
        RefreshEntries();
        StatusText.Text = Localization.T("Image closed");
    }

    private void UpdateMountButtons()
    {
        if (!ready) return;
        bool idle = operation == null;
        MountHereButton.IsEnabled = archive != null && idle;
        ExplorerButton.IsEnabled = UnmountButton.IsEnabled = MountedList.SelectedItem is MountedImage && idle;
        MountEmpty.Visibility = mounts.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void MountSelectionChanged(object sender, SelectionChangedEventArgs e) => UpdateMountButtons();

    private void Go(Entry? entry)
    {
        if (entry?.IsDirectory != true) return;
        current = entry;
        SearchBox.Text = "";
        RefreshEntries();
    }

    private void UpClicked(object sender, RoutedEventArgs e) => Go(current?.Parent);
    private void EnterClicked(object sender, RoutedEventArgs e) => Go((Entries.SelectedItem as BrowserRow)?.Entry);
    private void EntryDoubleTapped(object sender, DoubleTappedRoutedEventArgs e) => Go((Entries.SelectedItem as BrowserRow)?.Entry);
    private void FilterChanged(object sender, TextChangedEventArgs e) { if (ready) RefreshEntries(); }
    private void PathKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != VirtualKey.Enter) return;
        var entry = archive?.Find(PathBox.Text);
        if (entry?.IsDirectory == true) Go(entry);
        else StatusText.Text = Localization.T("Directory not found.");
    }

    private async void EntrySelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!ready) return;
        UpdateButtons();
        int generation = ++previewGeneration;
        Preview.Text = "";
        var entry = (Entries.SelectedItem as BrowserRow)?.Entry;
        var owner = archive;
        if (entry == null || owner == null)
        {
            Details.Text = current == null ? Localization.T("Open an image and select an entry.") : Localization.T("{0} entries in {1}", current.Children.Count, current.ArchivePath);
            return;
        }
        Details.Text = Localization.T("{0}\n{1} · {2}\nModified: {3:g}\nInode: {4}", entry.Name, entry.Kind, FormatSize(entry.Size), entry.Modified.LocalDateTime, entry.Inode) +
            (entry.Name == entry.WindowsName ? "" : Localization.T("\nWindows name: {0}", entry.WindowsName));
        if (!entry.IsFile) return;
        try
        {
            string result = await Task.Run(() =>
            {
                var bytes = new byte[(int)Math.Min(entry.Size, 16384)];
                int n = owner.Read(entry,0,bytes);
                try
                {
                    var text = new UTF8Encoding(false,true).GetString(bytes,0,n);
                    if (text.All(c => !char.IsControl(c) || c is '\r' or '\n' or '\t')) return text;
                }
                catch (DecoderFallbackException) { }
                var hex = new StringBuilder();
                for (int i=0;i<n;i+=16)
                    hex.Append(i.ToString("X8")).Append("  ").AppendLine(Convert.ToHexString(bytes.AsSpan(i,Math.Min(16,n-i))));
                return hex.ToString();
            });
            if (generation == previewGeneration) Preview.Text = result;
        }
        catch (Exception ex) { if (generation == previewGeneration) Preview.Text = ex.Message; }
    }

    private async void ExtractSelectedClicked(object sender, RoutedEventArgs e)
    {
        var owner = archive;
        var entry = (Entries.SelectedItem as BrowserRow)?.Entry;
        if (owner == null || entry == null || (!entry.IsFile && !entry.IsDirectory) || operation != null) return;
        try
        {
            var folder = await PickFolderAsync();
            if (folder == null || !ReferenceEquals(owner, archive)) return;
            await RunAsync(Localization.T("Extracting selected entry…"), token => Task.Run(() => owner.ExportEntry(entry, Path.Combine(folder,entry.WindowsName), token),token));
        }
        catch (Exception ex) { await ShowErrorAsync(ex); }
    }

    private async void ExtractAllClicked(object sender, RoutedEventArgs e) => await ExtractAllAsync();
    private async Task ExtractAllAsync()
    {
        if (archive == null || operation != null) return;
        string image = archive.Path;
        try
        {
            string? folder = await PickFolderAsync();
            if (folder == null) return;
            string name = Path.GetFileNameWithoutExtension(image);
            string destination = Path.Combine(folder, name);
            for (int i=2; Directory.Exists(destination) || File.Exists(destination);i++) destination = Path.Combine(folder,$"{name} ({i})");
            await RunAsync(Localization.T("Extracting image…"), token => SquashTools.UnpackAsync(image, destination, message => DispatcherQueue.TryEnqueue(() => StatusText.Text = message), token));
        }
        catch (Exception ex) { await ShowErrorAsync(ex); }
    }

    private async void SourceClicked(object sender, RoutedEventArgs e)
    {
        try
        {
            string? path = await PickFolderAsync();
            if (path == null) return;
            SourceBox.Text = path;
            if (string.IsNullOrWhiteSpace(OutputBox.Text)) OutputBox.Text = Path.TrimEndingDirectorySeparator(path) + ".squashfs";
        }
        catch (Exception ex) { await ShowErrorAsync(ex); }
    }
    private async void OutputClicked(object sender, RoutedEventArgs e)
    {
        try
        {
            string? folder = await PickFolderAsync();
            if (folder == null) return;
            string name = string.IsNullOrWhiteSpace(OutputBox.Text) ? "image.squashfs" : Path.GetFileName(OutputBox.Text);
            OutputBox.Text = Path.Combine(folder,name);
        }
        catch (Exception ex) { await ShowErrorAsync(ex); }
    }

    private void ResetClicked(object sender, RoutedEventArgs e) { if (operation == null) Reset(); }
    private void Reset()
    {
        CodecBox.SelectedIndex = 0; BlockBox.SelectedIndex = 5;
        LevelBox.Value = MemoryBox.Value = GzipWindow.Value = double.NaN;
        ThreadsBox.Value = 0;
        TimestampBox.Text = ExcludeBox.Text = XzDict.Text = XzFilters.Text = "";
        LzoAlgorithm.SelectedIndex = 0;
        foreach (var box in new[] { NoFragments,AlwaysFragments,NoDuplicates,NoSparse,NoXattrs,AllRoot,NoExports,NoDataCompression,NoInodeCompression,NoFragmentCompression,Lz4Hc,KeepDirectory })
            box.IsChecked = false;
        UpdateCodec();
    }
    private void UpdateCodec()
    {
        if (!ready) return;
        string codec = CodecBox.SelectedItem as string ?? "zstd";
        CodecHelp.Text = codec switch
        {
            "zstd" => Localization.T("Balanced compression ratio and fast random reads. Levels 1–22 or negative fast levels."),
            "gzip" => Localization.T("Broad Linux compatibility. Levels 1–9."),
            "xz" => Localization.T("High compression ratio; larger decompression cost."),
            "lz4" => Localization.T("Prioritizes decompression speed."),
            "lzo" => Localization.T("Fast decompression with broad compatibility."),
            _ => ""
        };
        LevelBox.Minimum = codec == "zstd" ? -131072 : 1;
        LevelBox.Maximum = codec == "zstd" ? 22 : 9;
        LevelBox.IsEnabled = codec is "zstd" or "gzip" or "lzo";
        if (!LevelBox.IsEnabled) LevelBox.Value = double.NaN;
        Lz4Hc.IsEnabled = codec == "lz4"; if (!Lz4Hc.IsEnabled) Lz4Hc.IsChecked = false;
        XzDict.IsEnabled = XzFilters.IsEnabled = codec == "xz"; if (!XzDict.IsEnabled) XzDict.Text = XzFilters.Text = "";
        GzipWindow.IsEnabled = codec == "gzip"; if (!GzipWindow.IsEnabled) GzipWindow.Value = double.NaN;
        LzoAlgorithm.IsEnabled = codec == "lzo"; if (!LzoAlgorithm.IsEnabled) LzoAlgorithm.SelectedIndex = 0;
        UpdateLzoLevel();
    }

    private void UpdateLzoLevel()
    {
        if (!ready || !Equals(CodecBox.SelectedItem, "lzo")) return;
        LevelBox.IsEnabled = LzoAlgorithm.SelectedIndex <= 0 || Equals(LzoAlgorithm.SelectedItem, "lzo1x_999");
        if (!LevelBox.IsEnabled) LevelBox.Value = double.NaN;
    }

    private static int? OptionalInt(NumberBox b)
    {
        if (double.IsNaN(b.Value)) return null;
        if (b.Value != Math.Truncate(b.Value) || b.Value > int.MaxValue || b.Value < int.MinValue) throw new ArgumentException(Localization.T("{0} must be an integer.", b.Header));
        return (int)b.Value;
    }
    private async void CreateClicked(object sender, RoutedEventArgs e)
    {
        if (operation != null) return;
        try
        {
            string source = SourceBox.Text.Trim(), output = OutputBox.Text.Trim();
            if (source.Length == 0 || output.Length == 0) throw new ArgumentException(Localization.T("Choose a source directory and output image."));
            var options = new PackOptions(Codec: (string)CodecBox.SelectedItem, BlockSize: 4096 << BlockBox.SelectedIndex,
                Threads: OptionalInt(ThreadsBox) ?? 0, Level: OptionalInt(LevelBox),
                NoFragments: NoFragments.IsChecked == true, NoDuplicates: NoDuplicates.IsChecked == true,
                NoXattrs: NoXattrs.IsChecked == true, AllRoot: AllRoot.IsChecked == true, NoExports: NoExports.IsChecked == true,
                NoSparse: NoSparse.IsChecked == true, NoCompression: NoDataCompression.IsChecked == true,
                AlwaysFragments: AlwaysFragments.IsChecked == true, NoInodeCompression: NoInodeCompression.IsChecked == true,
                NoFragmentCompression: NoFragmentCompression.IsChecked == true, MemoryMiB: OptionalInt(MemoryBox),
                Timestamp: string.IsNullOrWhiteSpace(TimestampBox.Text) ? null : uint.Parse(TimestampBox.Text.Trim()),
                ExcludeFile: ExcludeBox.Text.Trim(), Lz4HighCompression: Lz4Hc.IsChecked == true, KeepDirectory: KeepDirectory.IsChecked == true,
                XzDictionary: XzDict.Text.Trim(), XzFilters: XzFilters.Text.Trim(), GzipWindow: OptionalInt(GzipWindow),
                LzoAlgorithm: LzoAlgorithm.SelectedIndex <= 0 ? null : (string)LzoAlgorithm.SelectedItem);
            OperationLog.Text = "";
            LogExpander.IsExpanded = true;
            if (await RunAsync(Localization.T("Creating SquashFS image…"), token => SquashTools.PackAsync(source,output,options, line => AppendLog(OperationLog,line), token)))
                StatusText.Text = Localization.T("Created {0}", output);
        }
        catch (Exception ex) { await ShowErrorAsync(ex); }
    }
    private void AppendLog(TextBox box, string message)
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            string text = box.Text + message + Environment.NewLine;
            box.Text = text.Length > 60000 ? text[^50000..] : text;
        });
    }

    private void RefreshDrives()
    {
        var used = DriveInfo.GetDrives().Select(d => d.Name[..2]).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var selected = DriveBox.SelectedItem as string;
        var items = new[] { Localization.T("Automatic") }.Concat(Enumerable.Range('D',23).Select(c => $"{(char)c}:").Where(s => !used.Contains(s))).ToArray();
        DriveBox.ItemsSource = items; DriveBox.SelectedItem = selected != null && items.Contains(selected) ? selected : items[0];
    }
    private async void MountClicked(object sender, RoutedEventArgs e) => await MountAsync();
    private async void MountHereClicked(object sender, RoutedEventArgs e) => await MountAsync();
    private async Task MountAsync()
    {
        if (archive == null || operation != null) { StatusText.Text = Localization.T("Open an image before mounting."); return; }
        string path = archive.Path;
        string? drive = DriveBox.SelectedItem as string; if (drive == Localization.T("Automatic")) drive = null;
        string? mountedPoint = null;
        if (await RunAsync(Localization.T("Mounting read-only volume…"), async token =>
        {
            var mounted = await Task.Run(() => new MountedImage(path, drive), token);
            if (token.IsCancellationRequested) { mounted.Dispose(); token.ThrowIfCancellationRequested(); }
            mountedPoint = mounted.MountPoint;
            mounts.Add(mounted);
            Navigation.SelectedItem = MountNav;
            MountedList.SelectedItem = mounted;
            RefreshDrives();
        })) StatusText.Text = Localization.T("Mounted {0}", mountedPoint);
    }
    private async void UnmountClicked(object sender, RoutedEventArgs e)
    {
        if (MountedList.SelectedItem is not MountedImage item || operation != null) return;
        await RunAsync(Localization.T("Unmounting volume…"), async _ => { await Task.Run(item.Dispose); mounts.Remove(item); RefreshDrives(); });
    }
    private async void ExplorerClicked(object sender, RoutedEventArgs e)
    {
        try
        {
            if (MountedList.SelectedItem is MountedImage item)
                Process.Start(new ProcessStartInfo(item.MountPoint.TrimEnd('\\') + "\\") { UseShellExecute = true });
        }
        catch (Exception ex) { await ShowErrorAsync(ex); }
    }

    private async void CheckWinFspClicked(object sender, RoutedEventArgs e)
    {
        ToolsLog.Text = "";
        await RunAsync(Localization.T("Checking WinFsp runtime…"), async token =>
        {
            ToolsLog.Text = await Task.Run(WinFspRuntime.Describe, token);
        });
    }

    private async void MkVersionClicked(object sender, RoutedEventArgs e) => await VersionAsync("mksquashfs");
    private async void UnVersionClicked(object sender, RoutedEventArgs e) => await VersionAsync("unsquashfs");
    private async Task VersionAsync(string tool)
    {
        ToolsLog.Text = "";
        await RunAsync(Localization.T("Reading tool version…"), token => SquashTools.RunAsync(tool, new[] { "-version" }, line => AppendLog(ToolsLog,line),token));
    }
    private async void ToolsFolderClicked(object sender, RoutedEventArgs e)
    {
        try { Process.Start(new ProcessStartInfo(Path.Combine(AppContext.BaseDirectory,"tools")) { UseShellExecute = true }); }
        catch (Exception ex) { await ShowErrorAsync(ex); }
    }
}
