// SPDX-License-Identifier: GPL-3.0-only
using System.Collections.ObjectModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage.Pickers;
using Windows.System;
using WinSquashfs.Core;

namespace WinSquashfs;

public sealed partial class MainWindow
{
    private readonly ObservableCollection<string> packSources = new();
    private int[] packBlocks = Array.Empty<int>();
    private bool settingPack;
    private CompressionPreset lastPreset = CompressionPreset.Balanced;
    private ImageFormat PackFormat => FormatBox.SelectedItem is ImageFormat format ? format : ImageFormat.SquashFS;
    private string Extension => CreationOptions.ForPreset(PackFormat,CompressionPreset.Balanced).Extension;
    private CheckBox[] BcjBoxes => new[] { BcjX86,BcjArm,BcjThumb,BcjPpc,BcjSparc,BcjIa64 };
    private string[] BcjNames => new[] { "x86","arm","armthumb","powerpc","sparc","ia64" };
    private CheckBox[] SquashBoxes => new[] { NoFragments,AlwaysFragments,NoDuplicates,NoSparse,NoXattrs,AllRoot,
        NoExports,NoDataCompression,NoInodeCompression,NoFragmentCompression,Lz4Hc,KeepDirectory };

    private void InitializeCreation()
    {
        settingPack=true;
        SourceList.ItemsSource=packSources;
        FormatBox.ItemsSource=Enum.GetValues<ImageFormat>();
        PresetBox.ItemsSource=new[]{"1 · "+Localization.T("Store"),"2 · "+Localization.T("Fastest"),
            "3 · "+Localization.T("Fast"),"4 · "+Localization.T("Balanced"),"5 · "+Localization.T("High"),
            "6 · "+Localization.T("Very high"),"7 · "+Localization.T("Maximum"),"8 · "+Localization.T("Extreme"),
            Localization.T("Custom")};
        XzDict.ItemsSource=new[]{Localization.T("Default"),"25%","50%","75%","100%"};
        LzoAlgorithm.ItemsSource=new[]{Localization.T("Default"),"lzo1x_1","lzo1x_1_11","lzo1x_1_12","lzo1x_1_15","lzo1x_999"};
        TimestampDate.Date=DateTimeOffset.Now.Date;
        TimestampTime.Time=TimeSpan.Zero;
        FormatBox.SelectedIndex=0;
        PresetBox.SelectedIndex=3;
        FormatBox.SelectionChanged+=(_,_)=> { if(!settingPack) ConfigureFormat(); };
        PresetBox.SelectionChanged+=(_,_)=> { if(!settingPack && PresetBox.SelectedIndex is >=0 and <8) ApplyPreset((CompressionPreset)(PresetBox.SelectedIndex+1)); };
        CodecBox.SelectionChanged+=(_,_)=> { if(!settingPack){ LevelBox.Value=double.NaN; UpdateCodec(); MarkCustom(); } };
        foreach(var box in new[]{BlockBox,XzDict,LzoAlgorithm}) box.SelectionChanged+=(_,_)=>{if(!settingPack){UpdateCodec();MarkCustom();}};
        foreach(var box in new[]{LevelBox,ThreadsBox,MemoryBox,GzipWindow}) box.ValueChanged+=(_,_)=>MarkCustom();
        foreach(var box in SquashBoxes.Concat(BcjBoxes).Concat(new[]{FixedTimestamp,DwarfsExtreme,ErofsFragments,ErofsDedupe}))
        { box.Checked+=(_,_)=>MarkCustom(); box.Unchecked+=(_,_)=>MarkCustom(); }
        NoFragments.Checked+=(_,_)=>AlwaysFragments.IsChecked=false;
        AlwaysFragments.Checked+=(_,_)=>NoFragments.IsChecked=false;
        packSources.CollectionChanged+=(_,_)=>UpdateSourceCount();
        UpdateSourceCount();
        settingPack=false;
        ConfigureFormat();
    }

    private void UpdateSourceCount()
    {
        SourceCount.Text=Localization.T("{0} selected sources",packSources.Count);
        SourceEmpty.Visibility=packSources.Count==0?Visibility.Visible:Visibility.Collapsed;
    }
    private void AddSources(IEnumerable<string> paths)
    {
        foreach(string path in paths.Where(p=>!string.IsNullOrWhiteSpace(p)).Select(Path.GetFullPath))
            if(!packSources.Contains(path,StringComparer.OrdinalIgnoreCase)) packSources.Add(path);
        if(packSources.Count>0 && string.IsNullOrWhiteSpace(OutputBox.Text))
        {
            string first=Path.TrimEndingDirectorySeparator(packSources[0]);
            string folder=Path.GetDirectoryName(first)??first;
            string name=packSources.Count==1?(Directory.Exists(first)?Path.GetFileName(first):Path.GetFileNameWithoutExtension(first)):"archive";
            if(string.IsNullOrWhiteSpace(name))name="archive";
            OutputBox.Text=Path.Combine(folder,name+Extension);
        }
    }
    private async void AddFilesClicked(object sender,RoutedEventArgs e)
    {
        try
        {
            var picker=new FileOpenPicker{SuggestedStartLocation=PickerLocationId.DocumentsLibrary};
            picker.FileTypeFilter.Add("*"); InitPicker(picker);
            AddSources((await picker.PickMultipleFilesAsync()).Select(f=>f.Path));
        }
        catch(Exception ex){await ShowErrorAsync(ex);}
    }
    private async void SourceClicked(object sender,RoutedEventArgs e)
    {
        try { string? path=await PickFolderAsync(); if(path!=null)AddSources(new[]{path}); }
        catch(Exception ex){await ShowErrorAsync(ex);}
    }
    private async void AddSourcePathClicked(object sender,RoutedEventArgs e)
    {
        try { AddSources(new[]{SourceBox.Text.Trim().Trim('"')}); SourceBox.Text=""; }
        catch(Exception ex){await ShowErrorAsync(ex);}
    }
    private void SourcePathKeyDown(object sender,KeyRoutedEventArgs e)
    {
        if(e.Key==VirtualKey.Enter){AddSourcePathClicked(sender,new RoutedEventArgs());e.Handled=true;}
    }
    private void RemoveSourcesClicked(object sender,RoutedEventArgs e)
    {
        foreach(string path in SourceList.SelectedItems.OfType<string>().ToArray())packSources.Remove(path);
    }
    private void ClearSourcesClicked(object sender,RoutedEventArgs e){packSources.Clear();SourceBox.Text="";}
    private async void SourcesDropped(object sender,DragEventArgs e)
    {
        try { if(operation==null && e.DataView.Contains(StandardDataFormats.StorageItems))
            AddSources((await e.DataView.GetStorageItemsAsync()).Select(i=>i.Path)); }
        catch(Exception ex){await ShowErrorAsync(ex);}
    }
    private async void OutputClicked(object sender,RoutedEventArgs e)
    {
        try
        {
            var folder=await PickFolderAsync(); if(folder==null)return;
            var name=string.IsNullOrWhiteSpace(OutputBox.Text)?"archive"+Extension:Path.GetFileName(OutputBox.Text);
            OutputBox.Text=Path.Combine(folder,name);
        }
        catch(Exception ex){await ShowErrorAsync(ex);}
    }
    private async void ExcludeClicked(object sender,RoutedEventArgs e)
    {
        try { var p=new FileOpenPicker();p.FileTypeFilter.Add("*");InitPicker(p);
            if(await p.PickSingleFileAsync() is { } f){ExcludeBox.Text=f.Path;MarkCustom();} }
        catch(Exception ex){await ShowErrorAsync(ex);}
    }
    private void ClearExcludeClicked(object sender,RoutedEventArgs e){ExcludeBox.Text="";MarkCustom();}

    private void ConfigureFormat()
    {
        settingPack=true;
        ImageFormat format=PackFormat;
        CodecBox.ItemsSource=format switch{
            ImageFormat.SquashFS=>new[]{"zstd","gzip","xz","lz4","lzo"},
            ImageFormat.DwarFS=>new[]{"null","lz4","lz4hc","zstd","lzma"},
            _=>new[]{"none","lz4","lz4hc","zstd","lzma","deflate"}};
        packBlocks=format==ImageFormat.DwarFS?Enumerable.Range(16,11).Select(i=>1<<i).ToArray():
            Enumerable.Range(12,9).Select(i=>1<<i).ToArray();
        BlockBox.ItemsSource=packBlocks.Select(b=>FormatSize((ulong)b)).ToArray();
        BlockBox.Header=Localization.T(format==ImageFormat.EROFS?"Physical cluster size":"Data block size");
        SquashAdvancedPanel.Visibility=format==ImageFormat.SquashFS?Visibility.Visible:Visibility.Collapsed;
        DwarfsAdvancedPanel.Visibility=format==ImageFormat.DwarFS?Visibility.Visible:Visibility.Collapsed;
        ErofsAdvancedPanel.Visibility=format==ImageFormat.EROFS?Visibility.Visible:Visibility.Collapsed;
        ResourceOptions.Visibility=format==ImageFormat.EROFS?Visibility.Collapsed:Visibility.Visible;
        TimestampOptions.Visibility=format==ImageFormat.DwarFS?Visibility.Collapsed:Visibility.Visible;
        FormatExtension.Text=Extension;
        if(!string.IsNullOrWhiteSpace(OutputBox.Text))
        {
            string ext=Path.GetExtension(OutputBox.Text);
            if(new[]{".squashfs",".sqfs",".sfs",".dwarfs",".erofs"}.Contains(ext,StringComparer.OrdinalIgnoreCase))
                OutputBox.Text=Path.ChangeExtension(OutputBox.Text,Extension);
        }
        settingPack=false;
        ApplyPreset(lastPreset);
    }
    private void MarkCustom()
    {
        if(settingPack||!ready)return;
        settingPack=true;PresetBox.SelectedIndex=8;
        PresetScale.Value=0;
        PresetHelp.Text=Localization.T("Custom parameters. Select a preset to restore its settings.");
        settingPack=false;
    }
    private void ResetClicked(object sender,RoutedEventArgs e){if(operation==null)Reset();}
    private void Reset()=>ApplyPreset(CompressionPreset.Balanced);
    private void ApplyPreset(CompressionPreset preset)
    {
        settingPack=true;
        try
        {
            lastPreset=preset;
            PresetBox.SelectedIndex=(int)preset-1;PresetScale.Value=(int)preset;
            var o=CreationOptions.ForPreset(PackFormat,preset);var s=o.Squash;
            CodecBox.SelectedItem=s?.Codec??o.Codec;
            BlockBox.SelectedIndex=Array.IndexOf(packBlocks,s?.BlockSize??o.BlockSize??131072);
            foreach(var box in SquashBoxes.Concat(BcjBoxes))box.IsChecked=false;
            NoDataCompression.IsChecked=s?.NoCompression??false;
            NoInodeCompression.IsChecked=s?.NoInodeCompression??false;
            NoFragmentCompression.IsChecked=s?.NoFragmentCompression??false;
            AlwaysFragments.IsChecked=s?.AlwaysFragments??false;
            XzDict.SelectedItem=s?.XzDictionary??Localization.T("Default");
            var filters=(s?.XzFilters??"").Split(',');
            for(int i=0;i<BcjBoxes.Length;i++) BcjBoxes[i].IsChecked=filters.Contains(BcjNames[i]);
            LzoAlgorithm.SelectedIndex=0;GzipWindow.Value=double.NaN;
            ThreadsBox.Value=0;MemoryBox.Value=double.NaN;
            FixedTimestamp.IsChecked=false;ExcludeBox.Text="";
            DwarfsExtreme.IsChecked=o.Extreme;
            ErofsFragments.IsChecked=o.Fragments;ErofsDedupe.IsChecked=o.Deduplicate;
            UpdateCodec();
            LevelBox.Value=s?.Level??o.Level??double.NaN;
            PresetHelp.Text=Localization.T(preset switch {
                CompressionPreset.Store=>"Store data without compression.",
                CompressionPreset.Fastest=>"Favor creation speed and low CPU use.",
                CompressionPreset.Fast=>"Fast compression with moderate savings.",
                CompressionPreset.Balanced=>"Balanced size, speed and random access.",
                CompressionPreset.High=>"Prioritize smaller images.",
                CompressionPreset.VeryHigh=>"Higher compression with additional CPU time.",
                CompressionPreset.Maximum=>"Use high-ratio compression and larger blocks.",
                _=>"Use the highest compression effort and effective format parameters. Creation may take a very long time and use substantial memory."});
        }
        finally{settingPack=false;}
    }
    private void UpdateCodec()
    {
        string codec=CodecBox.SelectedItem as string??"zstd";bool sq=PackFormat==ImageFormat.SquashFS;
        CodecHelp.Text=Localization.T(codec switch{
            "none" or "null"=>"Store data without compression.",
            "lz4" or "lzo"=>"Fast decompression with broad compatibility.",
            "xz" or "lzma"=>"High compression ratio; larger decompression cost.",
            "zstd"=>"Balanced compression ratio and fast random reads. Levels 1–22 or negative fast levels.",
            "lz4hc"=>"Higher compression with fast LZ4 decoding.",
            _=>"General-purpose compression."});
        LevelBox.Minimum=codec=="zstd"?-131072:codec=="lzma"?0:1;
        LevelBox.Maximum=codec=="zstd"?22:codec=="lz4hc"?12:codec=="lzma"&&PackFormat==ImageFormat.EROFS?109:9;
        LevelBox.IsEnabled=codec is "zstd" or "gzip" or "lzma" or "lz4hc" or "deflate" ||
            (codec=="lzo" && LzoAlgorithm.SelectedIndex is 0 or 5);
        if(!LevelBox.IsEnabled)LevelBox.Value=double.NaN;
        XzOptions.Visibility=sq&&codec=="xz"?Visibility.Visible:Visibility.Collapsed;
        GzipWindow.Visibility=sq&&codec=="gzip"?Visibility.Visible:Visibility.Collapsed;
        LzoAlgorithm.Visibility=sq&&codec=="lzo"?Visibility.Visible:Visibility.Collapsed;
        Lz4Hc.Visibility=sq&&codec=="lz4"?Visibility.Visible:Visibility.Collapsed;
        DwarfsExtreme.IsEnabled=codec=="lzma";
    }
    private uint? Timestamp()
    {
        if(FixedTimestamp.IsChecked!=true||PackFormat==ImageFormat.DwarFS)return null;
        var dt=TimestampDate.Date.Date+TimestampTime.Time;
        return checked((uint)new DateTimeOffset(dt,TimeZoneInfo.Local.GetUtcOffset(dt)).ToUnixTimeSeconds());
    }
    private async void CreateClicked(object sender,RoutedEventArgs e)
    {
        if(operation!=null)return;
        try
        {
            AddSources(new[]{SourceBox.Text.Trim().Trim('"')});SourceBox.Text="";
            var paths=packSources.ToArray();
            string output=OutputBox.Text.Trim();
            if(paths.Length==0||output.Length==0)throw new ArgumentException(Localization.T("Choose sources and an output image."));
            string codec=(string)CodecBox.SelectedItem;
            int block=packBlocks[BlockBox.SelectedIndex];int threads=OptionalInt(ThreadsBox)??0;
            int? level=OptionalInt(LevelBox),memory=OptionalInt(MemoryBox);
            var preset=lastPreset;bool extreme=PresetBox.SelectedIndex==7;
            CreationOptions options;
            if(PackFormat==ImageFormat.SquashFS)
            {
                var filters=string.Join(",",BcjBoxes.Select((box,i)=>(box,i)).Where(v=>v.box.IsChecked==true).Select(v=>BcjNames[v.i]));
                var s=new PackOptions(Codec:codec,BlockSize:block,Threads:threads,Level:level,
                    NoFragments:NoFragments.IsChecked==true,AlwaysFragments:AlwaysFragments.IsChecked==true,
                    NoDuplicates:NoDuplicates.IsChecked==true,NoSparse:NoSparse.IsChecked==true,NoXattrs:NoXattrs.IsChecked==true,
                    AllRoot:AllRoot.IsChecked==true,NoExports:NoExports.IsChecked==true,NoCompression:NoDataCompression.IsChecked==true,
                    NoInodeCompression:NoInodeCompression.IsChecked==true,NoFragmentCompression:NoFragmentCompression.IsChecked==true,
                    MemoryMiB:extreme?null:memory,Timestamp:Timestamp(),ExcludeFile:ExcludeBox.Text.Trim(),
                    Lz4HighCompression:codec=="lz4"&&Lz4Hc.IsChecked==true,KeepDirectory:KeepDirectory.IsChecked==true,
                    XzDictionary:codec=="xz"&&XzDict.SelectedIndex>0?(string)XzDict.SelectedItem:null,
                    XzFilters:codec=="xz"&&!string.IsNullOrEmpty(filters)?filters:null,GzipWindow:codec=="gzip"?OptionalInt(GzipWindow):null,
                    LzoAlgorithm:codec=="lzo"&&LzoAlgorithm.SelectedIndex>0?(string)LzoAlgorithm.SelectedItem:null);
                options=new(PackFormat,preset,Squash:s,Extreme:extreme);
            }
            else options=new(PackFormat,preset,Codec:codec,Level:level,BlockSize:block,Threads:threads,MemoryMiB:memory,
                Fragments:ErofsFragments.IsChecked==true,Deduplicate:ErofsDedupe.IsChecked==true,
                Extreme:PackFormat==ImageFormat.DwarFS?DwarfsExtreme.IsChecked==true:extreme,Timestamp:Timestamp());
            OperationLog.Text="";LogExpander.IsExpanded=true;
            if(await RunAsync(Localization.T("Creating image…"),token=>ImageCreator.CreateAsync(paths,output,options,line=>AppendLog(OperationLog,line),token)))
                StatusText.Text=Localization.T("Created {0}",output);
        }
        catch(Exception ex){await ShowErrorAsync(ex);}
    }
}
