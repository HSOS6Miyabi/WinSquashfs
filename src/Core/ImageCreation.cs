// SPDX-License-Identifier: GPL-3.0-only
using System.Formats.Tar;
using System.Runtime.InteropServices;

namespace WinSquashfs.Core;

public enum CompressionPreset { Store=1, Fastest, Fast, Balanced, High, VeryHigh, Maximum, Extreme }

public sealed record CreationOptions(ImageFormat Format, CompressionPreset Preset = CompressionPreset.Balanced,
    PackOptions? Squash = null, string? Codec = null, int? Level = null, int? BlockSize = null,
    int Threads = 0, int? MemoryMiB = null, bool Fragments = true, bool Deduplicate = false,
    bool Extreme = false, uint? Timestamp = null)
{
    public string Extension => Format switch { ImageFormat.SquashFS => ".squashfs", ImageFormat.DwarFS => ".dwarfs", _ => ".erofs" };

    public static CreationOptions ForPreset(ImageFormat format, CompressionPreset preset)
    {
        int i=(int)preset-1;
        if(i<0 || i>7) throw new ArgumentOutOfRangeException(nameof(preset));
        return format switch
        {
            ImageFormat.SquashFS => new(format,preset, new PackOptions(
                Codec: new[]{"lz4","lz4","zstd","zstd","zstd","zstd","xz","xz"}[i],
                BlockSize: new[]{131072,65536,131072,131072,262144,1048576,1048576,1048576}[i],
                Level: new int?[]{null,null,1,3,9,19,null,null}[i],
                NoCompression: i==0, NoInodeCompression: i==0, NoFragmentCompression: i==0,
                AlwaysFragments: i>=6, XzDictionary: i>=6 ? "100%" : null,
                XzFilters: i==7 ? "x86,arm,armthumb,powerpc,sparc,ia64" : null),
                Extreme:i==7),
            ImageFormat.DwarFS => new(format,preset, Codec: new[]{"null","lz4","zstd","zstd","zstd","zstd","lzma","lzma"}[i],
                Level:new int?[]{null,null,1,5,12,19,8,9}[i],
                BlockSize:1 << new[]{20,20,21,22,23,24,26,30}[i],Extreme:i==7),
            ImageFormat.EROFS => new(format,preset,Codec:new[]{"none","lz4","lz4hc","lz4hc","zstd","zstd","lzma","lzma"}[i],
                Level:new int?[]{null,null,3,9,9,22,9,109}[i],
                BlockSize:new[]{4096,4096,16384,32768,65536,262144,1048576,1048576}[i],
                Fragments:i>=4,Deduplicate:i>=6,Extreme:i==7),
            _ => throw new ArgumentOutOfRangeException(nameof(format))
        };
    }

    public IReadOnlyList<string> DwarfsArguments()
    {
        if(Format!=ImageFormat.DwarFS) throw new ArgumentException();
        string codec=Codec ?? "zstd";
        if(codec is not ("null" or "lz4" or "lz4hc" or "zstd" or "lzma")) throw new ArgumentException(Localization.T("Unknown compressor."));
        int block=BlockSize ?? 4194304;
        if(block < 1024 || block > 1073741824 || (block & (block-1))!=0) throw new ArgumentException(Localization.T("Invalid block size."));
        int bits=System.Numerics.BitOperations.Log2((uint)block);
        int preset=new[]{0,1,2,4,6,7,8,9}[(int)Preset-1];
        string compression=codec;
        if(Level.HasValue && codec is "zstd" or "lzma" or "lz4hc") compression+=":level="+Level.Value;
        if(Extreme && codec=="lzma") compression+=":extreme:dict_size="+Math.Max(12,bits);
        var a=new List<string>{"--tool=mkdwarfs","-l",preset.ToString(),"-S",bits.ToString(),"-C",compression,"--no-progress"};
        // The embedded reader supports null, Zstandard, LZ4 and LZMA data.
        a.Add("--schema-compression="+(codec=="null"?"null":Extreme?"lzma:level=9:extreme":"zstd:level=16"));
        a.Add("--metadata-compression="+(Extreme ? "lzma:level=9:extreme" : codec=="null" ? "null" : "zstd"));
        if(Threads>0) a.AddRange(new[]{"-N",Threads.ToString()});
        if(MemoryMiB.HasValue && !Extreme) a.AddRange(new[]{"-L",MemoryMiB.Value+"m"});
        return a;
    }

    public IReadOnlyList<string> ErofsArguments()
    {
        if(Format!=ImageFormat.EROFS) throw new ArgumentException();
        string codec=Codec ?? "lz4hc";
        if(codec is not ("none" or "lz4" or "lz4hc" or "zstd" or "lzma" or "deflate")) throw new ArgumentException(Localization.T("Unknown compressor."));
        int cluster=BlockSize??32768;
        if(cluster<4096 || cluster>1048576 || (cluster & (cluster-1))!=0) throw new ArgumentException(Localization.T("Invalid block size."));
        var a=new List<string>{"--tar=f","-b","4096"};
        if(codec!="none")
        {
            a.Add("-z"+codec+(Level.HasValue?",level="+Level.Value:"")+(Extreme&&codec=="lzma"?",dictsize=8388608":""));
            a.AddRange(new[]{"-C",cluster.ToString()});
            if(Fragments) a.Add(Extreme?"-Eall-fragments,ztailpacking":"-Efragments,ztailpacking");
            if(Deduplicate) a.Add("-Ededupe");
        }
        // Global extent deduplication is single-threaded in erofs-utils.
        if(Deduplicate && codec!="none") a.Add("--workers=1");
        else if(Threads>0) a.Add("--workers="+Threads);
        if(Timestamp.HasValue) a.AddRange(new[]{"-T",Timestamp.Value.ToString()});
        return a;
    }
}

public static class ImageCreator
{
    public static IReadOnlyList<string> NormalizeSources(IEnumerable<string> sources)
    {
        var result=sources.Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        if(result.Length==0) throw new ArgumentException(Localization.T("Add at least one source file or folder."));
        foreach(string path in result)
            if(!File.Exists(path) && !Directory.Exists(path)) throw new FileNotFoundException(path);
        if(result.Length>1 && result.Select(Name).Distinct(StringComparer.OrdinalIgnoreCase).Count()!=result.Length)
            throw new ArgumentException(Localization.T("Selected sources have duplicate names at the image root."));
        return result;
    }

    private static string Name(string path)
    {
        string name=Path.GetFileName(Path.TrimEndingDirectorySeparator(path));
        return name.Length>0?name:Path.GetPathRoot(path)!.TrimEnd('\\','/',':');
    }

    public static async Task CreateAsync(IEnumerable<string> sources,string output,CreationOptions options,
        Action<string>? log=null,CancellationToken token=default)
    {
        var items=NormalizeSources(sources);
        output=Path.GetFullPath(output);
        if(File.Exists(output)||Directory.Exists(output)) throw new IOException(Localization.T("Output already exists."));
        string directory=Path.GetDirectoryName(output)!;
        if(!Directory.Exists(directory)) throw new DirectoryNotFoundException(directory);
        foreach(var item in items)
        {
            if(Directory.Exists(item) && WindowsPath.ContainsDirectory(item,directory))
                throw new IOException(Localization.T("Output must be outside the source directory."));
        }
        token.ThrowIfCancellationRequested();
        string temporary=Path.Combine(directory,".winsqfs-"+Guid.NewGuid().ToString("N")+".partial");
        string? stage=null;
        try
        {
            switch(options.Format)
            {
                case ImageFormat.SquashFS:
                    if(options.Extreme)
                        await CreateExtremeSquashAsync(items,temporary,options.Squash??new PackOptions(),log,token);
                    else
                        await SquashTools.PackSourcesAsync(items,temporary,options.Squash??new PackOptions(),log,token);
                    break;
                case ImageFormat.DwarFS:
                    string source;
                    if(items.Count==1 && Directory.Exists(items[0])) source=items[0];
                    else
                    {
                        stage=Path.Combine(Path.GetTempPath(),"WinSquashfs-input-"+Guid.NewGuid().ToString("N"));
                        Directory.CreateDirectory(stage);
                        log?.Invoke(Localization.T("Preparing selected sources…"));
                        foreach(string item in items)
                            await StageAsync(item,Path.Combine(stage,Name(item)),token);
                        source=stage;
                    }
                    // The writer supports blocks up to 1 GiB. A block larger
                    // than all input data cannot improve compression.
                    if(options.Extreme && options.Preset==CompressionPreset.Extreme && options.BlockSize==1073741824)
                        options=options with{BlockSize=LargestUsefulBlock(source,token)};
                    bool universal=File.Exists(Path.Combine(AppContext.BaseDirectory,"tools","dwarfs-universal.exe"));
                    var args=options.DwarfsArguments();
                    await SquashTools.RunAsync(universal?"dwarfs-universal":"mkdwarfs",(universal?args:args.Skip(1)).Concat(new[]{"-i",source,"-o",temporary}),log,token);
                    break;
                case ImageFormat.EROFS:
                    await SquashTools.RunAsync("mkfs.erofs",options.ErofsArguments().Concat(new[]{temporary,"-"}),log,token,
                        (stream,cancel)=>WriteTarAsync(items,stream,cancel));
                    break;
                default: throw new ArgumentException();
            }
            token.ThrowIfCancellationRequested();
            using(var archive=new Archive(temporary))
                if(archive.Format!=options.Format) throw new IOException(Localization.T("Unexpected output image format."));
            token.ThrowIfCancellationRequested();
            File.Move(temporary,output);
        }
        finally
        {
            if(File.Exists(temporary)) File.Delete(temporary);
            if(stage!=null && Directory.Exists(stage)) Directory.Delete(stage,true);
        }
    }

    private static int LargestUsefulBlock(string root,CancellationToken token)
    {
        const long max=1L<<30;
        long size=0;
        var pending=new Stack<string>();pending.Push(root);
        while(pending.TryPop(out var folder))
        {
            foreach(var p in Directory.EnumerateFileSystemEntries(folder))
            {
                token.ThrowIfCancellationRequested();
                var attr=File.GetAttributes(p);
                if((attr&FileAttributes.ReparsePoint)!=0) continue;
                if((attr&FileAttributes.Directory)!=0) pending.Push(p);
                else
                {
                    size+=Math.Min(max,new System.IO.FileInfo(p).Length);
                    if(size>=max)return (int)max;
                }
            }
        }
        return (int)System.Numerics.BitOperations.RoundUpToPowerOf2((ulong)Math.Max(1L<<20,size));
    }

    private static async Task CreateExtremeSquashAsync(IReadOnlyList<string> sources,string output,
        PackOptions requested,Action<string>? log,CancellationToken token)
    {
        var common=requested with{BlockSize=1048576,MemoryMiB=null,NoFragments=false,AlwaysFragments=true,
            NoCompression=false,NoInodeCompression=false,NoFragmentCompression=false,NoDuplicates=false,
            Level=null,XzDictionary=null,XzFilters=null,GzipWindow=null,LzoAlgorithm=null,Lz4HighCompression=false};
        var candidates=new[]{
            common with{Codec="xz",XzDictionary="100%",XzFilters="x86,arm,armthumb,powerpc,sparc,ia64"},
            common with{Codec="xz",XzDictionary="100%"},
            common with{Codec="zstd",Level=22},
            common with{Codec="gzip",Level=9}
        };
        long bestDisk=long.MaxValue;
        ulong bestUsed=ulong.MaxValue;
        string? winner=null;
        foreach(var option in candidates)
        {
            string trial=output+"."+Guid.NewGuid().ToString("N");
            try
            {
                token.ThrowIfCancellationRequested();
                string label=option.Codec+(option.XzFilters!=null?" + BCJ":"");
                log?.Invoke(Localization.T("Testing maximum compression with {0}…",label));
                await SquashTools.PackSourcesAsync(sources,trial,option,log,token);
                long disk=new System.IO.FileInfo(trial).Length;
                ulong used;
                using(var archive=new Archive(trial)) used=archive.ImageSize;
                if(disk<bestDisk||(disk==bestDisk&&used<bestUsed))
                {
                    File.Move(trial,output,overwrite:true);
                    bestDisk=disk;bestUsed=used;winner=label;
                }
            }
            finally{if(File.Exists(trial))File.Delete(trial);}
        }
        log?.Invoke(Localization.T("Selected smallest image: {0}, {1} bytes.",winner,bestDisk));
    }

    // Multi-source DwarFS creation uses hard links on the same filesystem,
    // falling back to copying only when Windows cannot create a hard link.
    [DllImport("kernel32.dll",EntryPoint="CreateHardLinkW",CharSet=CharSet.Unicode,SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateHardLink(string newName,string existing,IntPtr reserved);

    private static async Task StageAsync(string source,string destination,CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        FileAttributes attr=File.GetAttributes(source);
        if((attr&FileAttributes.Directory)!=0)
        {
            Directory.CreateDirectory(destination);
            var link=new DirectoryInfo(source).LinkTarget;
            if(link!=null)
            {
                Directory.Delete(destination);
                Directory.CreateSymbolicLink(destination,link);
                return;
            }
            foreach(string item in Directory.EnumerateFileSystemEntries(source))
                await StageAsync(item,Path.Combine(destination,Name(item)),token);
            Directory.SetLastWriteTimeUtc(destination,Directory.GetLastWriteTimeUtc(source));
        }
        else
        {
            var link=new System.IO.FileInfo(source).LinkTarget;
            if(link!=null) { File.CreateSymbolicLink(destination,link); return; }
            if(CreateHardLink(destination,source,IntPtr.Zero)) return;
            using var input=new FileStream(source,FileMode.Open,FileAccess.Read,FileShare.Read,1048576,FileOptions.Asynchronous|FileOptions.SequentialScan);
            using var output=new FileStream(destination,FileMode.CreateNew,FileAccess.Write,FileShare.None,1048576,FileOptions.Asynchronous);
            await input.CopyToAsync(output,1048576,token);
            await output.FlushAsync(token);
            File.SetLastWriteTimeUtc(output.SafeFileHandle,File.GetLastWriteTimeUtc(source));
        }
    }

    private static async Task WriteTarAsync(IReadOnlyList<string> sources,Stream stream,CancellationToken token)
    {
        using var writer=new TarWriter(stream,TarEntryFormat.Pax,leaveOpen:true);
        var pending=new Stack<(string Path,string Name)>();
        if(sources.Count==1 && Directory.Exists(sources[0]))
        {
            foreach(var item in Directory.EnumerateFileSystemEntries(sources[0]).Reverse())
                pending.Push((item,Name(item)));
        }
        else foreach(var item in sources.Reverse()) pending.Push((item,Name(item)));
        while(pending.TryPop(out var item))
        {
            token.ThrowIfCancellationRequested();
            FileAttributes attr=File.GetAttributes(item.Path);
            bool dir=(attr&FileAttributes.Directory)!=0;
            FileSystemInfo info=dir?new DirectoryInfo(item.Path):new System.IO.FileInfo(item.Path);
            var target=info.LinkTarget;
            if(target!=null)
            {
                var link=new PaxTarEntry(TarEntryType.SymbolicLink,item.Name){LinkName=target.Replace('\\','/'),ModificationTime=info.LastWriteTimeUtc};
                await writer.WriteEntryAsync(link,token);
            }
            else if(dir)
            {
                var entry=new PaxTarEntry(TarEntryType.Directory,item.Name){ModificationTime=info.LastWriteTimeUtc,
                    Mode=(UnixFileMode)493};
                await writer.WriteEntryAsync(entry,token);
                foreach(var child in Directory.EnumerateFileSystemEntries(item.Path).Reverse())
                    pending.Push((child,item.Name+"/"+Name(child)));
            }
            else
            {
                using var file=new FileStream(item.Path,FileMode.Open,FileAccess.Read,FileShare.Read,1048576,FileOptions.Asynchronous|FileOptions.SequentialScan);
                var entry=new PaxTarEntry(TarEntryType.RegularFile,item.Name){DataStream=file,ModificationTime=info.LastWriteTimeUtc,
                    Mode=(UnixFileMode)420};
                await writer.WriteEntryAsync(entry,token);
            }
        }
    }
}
