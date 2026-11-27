// SPDX-License-Identifier: GPL-3.0-only
using WinSquashfs.Core;

static void Require(bool value,string message) { if (!value) throw new Exception(message); }
string root = Path.Combine(Path.GetTempPath(),"WinSquashfsTests-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
try
{
    var source = Path.Combine(root,"source with spaces");
    Directory.CreateDirectory(Path.Combine(source,"nested"));
    Directory.CreateDirectory(Path.Combine(source,"empty"));
    byte[] data = new byte[3 * 1024 * 1024 + 173];
    new Random(7234).NextBytes(data);
    // Exercise incompressible data, sparse blocks, compressed blocks, and fragments.
    Array.Clear(data,131072,131072);
    for (int i=262144;i<393216;i++) data[i]=42;
    File.WriteAllBytes(Path.Combine(source,"large.bin"), data);
    File.WriteAllBytes(Path.Combine(source,"zero"), Array.Empty<byte>());
    File.WriteAllText(Path.Combine(source,"nested","hello.txt"),"Hello SquashFS");
    File.WriteAllText(Path.Combine(source,"nested","日本語.txt"),"Unicode test");
    byte[] tiny = {0,7,99,1,2,3,4};
    File.WriteAllBytes(Path.Combine(source,"tiny"),tiny);

    foreach (string codec in new[] {"gzip","xz","lzo","lz4","zstd"})
    {
        string image = Path.Combine(root,codec+".squashfs");
        await SquashTools.PackAsync(source,image,new PackOptions(Codec:codec,BlockSize:131072,Threads:2,AlwaysFragments:true,Timestamp:1700000000),Console.WriteLine);
        using (var archive = new Archive(image))
        {
            Require(archive.Codec==codec,"Wrong compressor");
            Require(archive.BlockSize==131072,"Wrong block size");
            var large=archive.Find(@"\large.bin") ?? throw new Exception("Missing large file");
            Require(large.Size==(ulong)data.Length,"Wrong file size");
            Require(archive.Find(@"\empty")!.IsDirectory,"Empty directory");
            Require(archive.Find(@"\nested\日本語.txt")!.IsFile,"Unicode name");
            Require(archive.Find(@"\zero")!.Size==0,"Zero length");
            var rand = new Random(433);
            var offsets = new[] {data.Length-5,0,131071,131072,262143,262144,393216,1048576,data.Length}
                .Concat(Enumerable.Range(0,120).Select(_=>rand.Next(0,data.Length)));
            foreach(int offset in offsets)
            {
                byte[] bytes=new byte[17001];
                int n=archive.Read(large, (ulong)offset,bytes);
                Require(n==Math.Min(bytes.Length,data.Length-offset),"Wrong read length");
                Require(bytes.AsSpan(0,n).SequenceEqual(data.AsSpan(offset,n)),$"{codec}: data mismatch at {offset}");
            }
            byte[] fragment=new byte[16];
            Require(archive.Read(archive.Find("tiny")!,0,fragment)==tiny.Length && fragment.AsSpan(0,tiny.Length).SequenceEqual(tiny),"Fragment mismatch");
            Require(archive.Read(archive.Find("zero")!,0,fragment)==0,"Empty file read");
            Parallel.For(0,40,i =>
            {
                int offset=(i*17311)%data.Length;
                byte[] buffer=new byte[8192];
                int n=archive.Read(large,(ulong)offset,buffer);
                Require(n==buffer.Length && buffer.AsSpan(0,n).SequenceEqual(data.AsSpan(offset,n)),"Concurrent read mismatch");
            });
        }
        var extracted=Path.Combine(root,codec+"-extracted");
        await SquashTools.UnpackAsync(image,extracted,Console.WriteLine);
        Require(File.ReadAllBytes(Path.Combine(extracted,"large.bin")).SequenceEqual(data),"Extraction mismatch");
        Require(File.ReadAllBytes(Path.Combine(extracted,"tiny")).SequenceEqual(tiny),"Extracted fragment mismatch");

        string drive;
        using (var mount=new MountedImage(image))
        {
            drive=mount.MountPoint.TrimEnd('\\')+"\\";
            var deadline=DateTime.UtcNow.AddSeconds(15);
            while (!Directory.Exists(drive) && DateTime.UtcNow<deadline) await Task.Delay(100);
            Console.WriteLine($"Mounted {codec} at {drive}; visible volumes: {string.Join(", ", DriveInfo.GetDrives().Select(d => d.Name))}");
            if (!Directory.Exists(drive))
            {
                try { Console.WriteLine($"Root attributes: {File.GetAttributes(drive)}"); }
                catch (Exception ex) { Console.WriteLine(ex); }
                throw new IOException($"Mounted root is not accessible: {drive}");
            }
            Require(Directory.EnumerateFileSystemEntries(drive).Count()==5,"Mount enumeration mismatch");
            Require(File.ReadAllText(Path.Combine(drive,"nested","hello.txt"))=="Hello SquashFS","Mounted text read");
            using (var stream=new FileStream(Path.Combine(drive,"large.bin"),FileMode.Open,FileAccess.Read,FileShare.Read))
            {
                foreach (int pos in new[]{data.Length-7,131071,0,262144,1048576})
                {
                    stream.Seek(pos,SeekOrigin.Begin);
                    byte[] got=new byte[Math.Min(17001,data.Length-pos)];
                    stream.ReadExactly(got);
                    Require(got.SequenceEqual(data.AsSpan(pos,got.Length).ToArray()),"Mounted random read mismatch");
                }
            }
            bool blocked=false;
            try { File.WriteAllText(Path.Combine(drive,"new.txt"),"deny"); }
            catch (IOException) { blocked=true; }
            catch (UnauthorizedAccessException) { blocked=true; }
            Require(blocked,"Mount permitted writing");
            Require(!File.Exists(Path.Combine(drive,"new.txt")),"Unexpected mutation");
        }
        var until=DateTime.UtcNow.AddSeconds(10);
        while(Directory.Exists(drive) && DateTime.UtcNow<until) await Task.Delay(100);
        Require(!Directory.Exists(drive),"Unmount failed");
        var upstream=Path.Combine(root,codec+"-upstream");
        await SquashTools.RunAsync("unsquashfs",new[]{"-no-progress","-d",SquashTools.ToolPath(upstream),SquashTools.ToolPath(image)},Console.WriteLine);
        Require(File.ReadAllBytes(Path.Combine(upstream,"large.bin")).SequenceEqual(data),"Upstream extraction mismatch");
        Console.WriteLine($"PASS {codec}");
    }
    // Mount one UNC namespace containing multiple images and directories. No drive is allocated.
    var imageDir=Path.Combine(root,"unc-source");
    Directory.CreateDirectory(Path.Combine(imageDir,"subdir"));
    File.Copy(Path.Combine(root,"gzip.squashfs"),Path.Combine(imageDir,"subdir","first.squashfs"));
    File.Copy(Path.Combine(root,"zstd.squashfs"),Path.Combine(imageDir,"second.sqfs"));
    File.WriteAllText(Path.Combine(imageDir,"not-an-image.txt"),"not exposed");
    string shareName="ci-"+Guid.NewGuid().ToString("N")[..8];
    var share=new UncShare(shareName,imageDir);
    var originalDrives=DriveInfo.GetDrives().Select(d=>d.Name).OrderBy(d=>d).ToArray();
    string prefix=@"\\sfs$\"+shareName;
    Require(share.UncPath==prefix,"Unexpected UNC namespace");
    using (var unc=new UncSession(new[]{share},idleSeconds:1,maxIdleImages:2))
    {
        var until=DateTime.UtcNow.AddSeconds(15);
        while(!Directory.Exists(prefix) && DateTime.UtcNow<until) await Task.Delay(100);
        Require(Directory.Exists(prefix), "UNC share did not become available");
        Require(unc.CachedImages==0,"Images opened before use");
        Require(Directory.EnumerateDirectories(prefix).Count()==2,"UNC root enumeration");
        Require(unc.CachedImages==0,"Directory listing opened image data");
        Require(!File.Exists(Path.Combine(prefix,"not-an-image.txt")),"Ordinary host file exposed");
        string one=Path.Combine(prefix,"subdir","first.squashfs");
        string two=Path.Combine(prefix,"second.sqfs");
        Require(Directory.Exists(one),"UNC image root missing");
        Require(File.ReadAllText(Path.Combine(two,"nested","hello.txt"))=="Hello SquashFS","Second UNC image read");
        using (var stream=new FileStream(Path.Combine(one,"large.bin"),FileMode.Open,FileAccess.Read,FileShare.Read))
        {
            Require(unc.ActiveImages>=1,"UNC image reference missing");
            foreach(int pos in new[]{data.Length-13,131071,0,262144,1048576})
            {
                stream.Seek(pos,SeekOrigin.Begin);
                var got=new byte[Math.Min(17001,data.Length-pos)];
                stream.ReadExactly(got);
                Require(got.SequenceEqual(data.AsSpan(pos,got.Length).ToArray()),"UNC random read mismatch");
            }
            await Task.Delay(2500);
            Require(unc.ActiveImages>=1 && unc.CachedImages>=1,"Active UNC image evicted");
            stream.Seek(131072,SeekOrigin.Begin);
            Require(stream.ReadByte()==data[131072],"UNC handle invalid after idle expiry");
        }
        until=DateTime.UtcNow.AddSeconds(10);
        while(unc.CachedImages!=0 && DateTime.UtcNow<until) await Task.Delay(100);
        Require(unc.CachedImages==0,"Idle UNC image was not released");
        Require(File.ReadAllBytes(Path.Combine(two,"tiny")).SequenceEqual(tiny),"UNC image not reopened");
        Require(DriveInfo.GetDrives().Select(d=>d.Name).OrderBy(d=>d).SequenceEqual(originalDrives),"UNC allocated a drive letter");
        bool blocked=false;
        try { File.WriteAllText(Path.Combine(one,"new.txt"),"deny"); }
        catch(IOException) { blocked=true; }
        catch(UnauthorizedAccessException) { blocked=true; }
        Require(blocked,"UNC permitted writes");
    }
    Require(!Directory.Exists(prefix),"UNC namespace remains mounted");
    using(var archive=new Archive(Path.Combine(root,"zstd.squashfs")))
    {
        var report=archive.Verify();
        Require(report.Files==5 && report.Bytes>=(ulong)data.Length,"Verification did not read all files");
        string folder=Path.Combine(root,"selected-folder");
        archive.ExportEntry(archive.Find("nested")!,folder);
        Require(File.ReadAllText(Path.Combine(folder,"hello.txt"))=="Hello SquashFS","Selected folder export failed");
    }
    Console.WriteLine("PASS drive-free UNC namespace, on-demand cache, image verification and folder extraction");

    // A valid-looking inode that claims a short uncompressed block must be rejected.
    var corruptSource=Path.Combine(root,"corrupt-source");
    Directory.CreateDirectory(corruptSource);
    File.WriteAllBytes(Path.Combine(corruptSource,"block.bin"),Enumerable.Repeat((byte)0x5A,4096).ToArray());
    string damaged=Path.Combine(root,"short-block.squashfs");
    await SquashTools.PackAsync(corruptSource,damaged,new PackOptions(Codec:"gzip",BlockSize:4096,
        NoFragments:true,NoCompression:true,NoInodeCompression:true),null);
    byte[] raw=File.ReadAllBytes(damaged);
    int inodeStart=checked((int)System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(raw.AsSpan(64,8)));
    int directoryStart=checked((int)System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(raw.AsSpan(72,8)));
    int patched=0;
    for(int i=inodeStart+2;i+4<=directoryStart;i++)
    {
        if(System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(raw.AsSpan(i,4))==0x01001000u)
        {
            System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(raw.AsSpan(i,4),0x01000FFFu);
            patched++;
        }
    }
    Require(patched==1,"Could not construct short-block regression image");
    File.WriteAllBytes(damaged,raw);
    bool corruptRejected=false;
    try { using var image=new Archive(damaged); image.Verify(); }
    catch(IOException) { corruptRejected=true; }
    Require(corruptRejected,"Short data block was accepted");

    // Empty-file ownership and cancellation must be checked before creating files.
    using(var first=new Archive(Path.Combine(root,"gzip.squashfs")))
    using(var second=new Archive(Path.Combine(root,"zstd.squashfs")))
    {
        string foreign=Path.Combine(root,"foreign");
        bool foreignRejected=false;
        try { first.ExportFile(second.Find("zero")!,foreign); }
        catch(ArgumentException) { foreignRejected=true; }
        Require(foreignRejected && !File.Exists(foreign),"Foreign empty file was exported");
        using var cancelled=new CancellationTokenSource();
        cancelled.Cancel();
        bool canceled=false;
        try { first.ExportFile(first.Find("zero")!,foreign,cancelled.Token); }
        catch(OperationCanceledException) { canceled=true; }
        Require(canceled && !File.Exists(foreign),"Canceled empty file was created");
        string longFolder=Path.Combine(root,new string('a',230));
        first.ExportEntry(first.Find("nested")!,longFolder);
        Require(File.Exists(Path.Combine(longFolder,"hello.txt")),"Long directory extraction failed");
    }
    Console.WriteLine("PASS corrupt block rejection, cancellation, ownership and long-name extraction");

    // The retained RH oplock must yield to a conflicting open, and removal
    // invalidates live virtual handles without closing the shared namespace.
    var changing=Path.Combine(root,"changing");
    Directory.CreateDirectory(changing);
    string changingSource=Path.Combine(changing,"changing.squashfs");
    string keepSource=Path.Combine(changing,"keep.squashfs");
    File.Copy(Path.Combine(root,"gzip.squashfs"),changingSource);
    File.Copy(Path.Combine(root,"zstd.squashfs"),keepSource);
    var changingShare=new UncShare("changes-"+Guid.NewGuid().ToString("N")[..8],changing);
    using(var session=new UncSession(new[]{changingShare},60,8))
    {
        string projected=Path.Combine(changingShare.UncPath,"changing.squashfs");
        string retained=Path.Combine(changingShare.UncPath,"keep.squashfs");
        Require(File.ReadAllText(Path.Combine(retained,"nested","hello.txt"))=="Hello SquashFS","Unrelated image not accessible");
        using(var live=new FileStream(Path.Combine(projected,"large.bin"),FileMode.Open,FileAccess.Read,FileShare.Read,1))
        {
            Require(live.ReadByte()==data[0],"Initial live read failed");
            Require(session.OplockImages>=1,"Local NTFS RH oplock was not granted");
            // A conflicting open must complete while the projected file remains open.
            await Task.Run(() => { using var f=File.Open(changingSource,FileMode.Open,FileAccess.ReadWrite,FileShare.None); }).WaitAsync(TimeSpan.FromSeconds(10));
            var deadline=DateTime.UtcNow.AddSeconds(5);
            while(session.CachedImages>1 && DateTime.UtcNow<deadline) await Task.Delay(50);
            Require(session.CachedImages==1,"Oplock break did not release the image");
            bool stale=false;
            try { live.Seek(2*1024*1024,SeekOrigin.Begin); live.ReadByte(); }
            catch(IOException) { stale=true; }
            catch(UnauthorizedAccessException) { stale=true; }
            Require(stale,"Invalidated handle still returned image contents");
        }
        using(var live=new FileStream(Path.Combine(projected,"large.bin"),FileMode.Open,FileAccess.Read,FileShare.Read,1))
        {
            Require(live.ReadByte()==data[0],"Image failed to reopen after oplock break");
            await Task.Run(() => File.Delete(changingSource)).WaitAsync(TimeSpan.FromSeconds(10));
            var deadline=DateTime.UtcNow.AddSeconds(5);
            while(session.CachedImages>1 && DateTime.UtcNow<deadline) await Task.Delay(50);
            Require(!File.Exists(changingSource) && session.CachedImages==1,"Source deletion did not evict active image");
            Require(!Directory.Exists(projected),"Deleted source is still visible");
            bool stale=false;
            try { live.Seek(2*1024*1024,SeekOrigin.Begin); live.ReadByte(); }
            catch(IOException) { stale=true; }
            catch(UnauthorizedAccessException) { stale=true; }
            Require(stale,"Deleted source remained readable");
        }
        File.Copy(Path.Combine(root,"zstd.squashfs"),changingSource);
        Require(File.ReadAllBytes(Path.Combine(projected,"tiny")).SequenceEqual(tiny),"Replacement image did not open");
        string renamed=Path.Combine(changing,"renamed.sqfs");
        await Task.Run(() => File.Move(changingSource,renamed)).WaitAsync(TimeSpan.FromSeconds(10));
        Require(!Directory.Exists(projected),"Renamed source remained visible");
        Require(File.ReadAllBytes(Path.Combine(changingShare.UncPath,"renamed.sqfs","tiny")).SequenceEqual(tiny),"Renamed source failed to reopen");
        Require(File.ReadAllText(Path.Combine(retained,"nested","hello.txt"))=="Hello SquashFS","Unrelated UNC image was invalidated");
    }
    Console.WriteLine("PASS RH oplock break, deletion, rename, replacement and isolated invalidation");

    // The CI runner exposes an actual SMB share. The network source should
    // not retain file handles and must invalidate active projected files.
    string? smb=Environment.GetEnvironmentVariable("WINSQUASHFS_TEST_SMB");
    if (!string.IsNullOrEmpty(smb))
    {
        string remote=Path.Combine(smb,"network.squashfs");
        File.Copy(Path.Combine(root,"gzip.squashfs"),remote,true);
        string projection=UncShare.NetworkNamespace.ImagePath(remote) ?? throw new Exception("Network mapping failed");
        Require(projection.StartsWith(@"\\sfs$\UNC\",StringComparison.OrdinalIgnoreCase),"Network namespace mismatch");
        Require(UncShare.NetworkNamespace.ImagePath(@"\\sfs$\C\recursive.sqfs")==null,"Recursive network mapping allowed");
        using var session=new UncSession(new[]{UncShare.NetworkNamespace},60,8);
        Require(Directory.Exists(projection),"Remote image directory missing");
        using(var live=new FileStream(Path.Combine(projection,"large.bin"),FileMode.Open,FileAccess.Read,FileShare.Read,1))
        {
            live.Seek(131071,SeekOrigin.Begin);
            byte[] got=new byte[17001];
            live.ReadExactly(got);
            Require(got.SequenceEqual(data.AsSpan(131071,got.Length).ToArray()),"SMB random read mismatch");
            Require(session.OplockImages==0,"SMB source unexpectedly retained an oplock");
            // An exclusive open of the original file succeeds while the
            // projected handle is open, because idle SMB handles are closed.
            using(var exclusive=File.Open(remote,FileMode.Open,FileAccess.Read,FileShare.None)) { }
            await Task.Run(() => File.Delete(remote)).WaitAsync(TimeSpan.FromSeconds(10));
            var deadline=DateTime.UtcNow.AddSeconds(5);
            while(session.CachedImages!=0 && DateTime.UtcNow<deadline) await Task.Delay(50);
            Require(!File.Exists(remote) && session.CachedImages==0,"SMB deletion did not invalidate image");
            Require(!Directory.Exists(projection),"Deleted SMB source remained visible");
            bool stale=false;
            try { live.Seek(2*1024*1024,SeekOrigin.Begin); live.ReadByte(); }
            catch(IOException) { stale=true; }
            catch(UnauthorizedAccessException) { stale=true; }
            Require(stale,"Stale SMB handle still readable");
        }
        File.Copy(Path.Combine(root,"zstd.squashfs"),remote);
        Require(File.ReadAllBytes(Path.Combine(projection,"tiny")).SequenceEqual(tiny),"SMB replacement image did not reopen");
        File.Delete(remote);
        Console.WriteLine("PASS actual SMB source, random reads, no idle source handles and deletion");
    }

    var bad=Path.Combine(root,"bad.squashfs");
    File.WriteAllBytes(bad,new byte[96]);
    bool rejected=false;
    try { using var archive=new Archive(bad); }
    catch (IOException) { rejected=true; }
    Require(rejected,"Malformed image accepted");
    Require(WindowsNames.Encode("CON.txt")!="CON.txt","Reserved name projection");
    Require(WindowsNames.Encode("..")!="..","Dot traversal projection");
    Require(WindowsNames.Encode("x:y")!="x:y","Alternate stream projection");
    Require(WindowsNames.Encode("foo.")!="foo.","Trailing dot projection");
    Require(WindowsNames.Encode("a%003A")!=WindowsNames.Encode("a:"),"Escaping collision");
    bool invalid=false;
    try { _=new PackOptions(NoFragments:true,AlwaysFragments:true).Arguments(); }
    catch (ArgumentException) { invalid=true; }
    Require(invalid,"Conflicting flags accepted");
    // Exercise actual compressor-specific options, including Unicode paths and block sizes.
    var advancedSource=Path.Combine(root,"高级设置");
    Directory.CreateDirectory(advancedSource);
    byte[] advancedData=Enumerable.Range(0,65539).Select(i=>(byte)(i%239)).ToArray();
    File.WriteAllBytes(Path.Combine(advancedSource,"数据.bin"),advancedData);
    File.WriteAllText(Path.Combine(advancedSource,"skip.txt"),"excluded");
    var excludes=Path.Combine(root,"排除列表.txt");
    File.WriteAllText(excludes,"skip.txt\n",new System.Text.UTF8Encoding(false));
    var variants=new[]
    {
        new PackOptions(Codec:"gzip",BlockSize:4096,Threads:2,Level:1,NoFragments:true,NoDuplicates:true,NoXattrs:true,AllRoot:true,NoExports:true,NoSparse:true,GzipWindow:12,ExcludeFile:excludes),
        new PackOptions(Codec:"xz",BlockSize:131072,Threads:2,XzDictionary:"64K",XzFilters:"x86"),
        new PackOptions(Codec:"lzo",BlockSize:65536,Threads:2,Level:2,LzoAlgorithm:"lzo1x_999"),
        new PackOptions(Codec:"lz4",BlockSize:262144,Threads:2,Lz4HighCompression:true,NoFragmentCompression:true),
        new PackOptions(Codec:"zstd",BlockSize:1048576,Threads:2,Level:-3,AlwaysFragments:true,NoInodeCompression:true)
    };
    foreach(var variant in variants)
    {
        string output=Path.Combine(root,"高级-"+variant.Codec+".squashfs");
        await SquashTools.PackAsync(advancedSource,output,variant,null);
        using var image=new Archive(output);
        var entry=image.Find("数据.bin") ?? throw new IOException("Unicode file missing");
        var got=new byte[advancedData.Length];
        Require(image.BlockSize==variant.BlockSize && image.Read(entry,0,got)==got.Length && got.SequenceEqual(advancedData),"Advanced codec settings round trip: "+variant.Codec);
        if(variant.ExcludeFile!=null) Require(image.Find("skip.txt")==null,"Exclude-list file was ignored");
        image.Verify();
    }
    var junction=Path.Combine(root,"output-alias");
    var junctionProcess=System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe")
    {
        UseShellExecute=false, ArgumentList={"/c","mklink","/J",junction,advancedSource}
    }) ?? throw new IOException("Cannot start mklink");
    using(junctionProcess)
    {
        await junctionProcess.WaitForExitAsync();
        Require(junctionProcess.ExitCode==0,"Cannot create test junction");
    }
    try
    {
        bool aliasRejected=false;
        try { await SquashTools.PackAsync(advancedSource,Path.Combine(junction,"self.squashfs"),new PackOptions(),null); }
        catch(IOException ex) { aliasRejected=ex.Message.Contains("outside the source directory"); }
        Require(aliasRejected && !File.Exists(Path.Combine(advancedSource,"self.squashfs")),"Output inside source through junction was accepted");
    }
    finally { Directory.Delete(junction); }
    using(var cancelled=new CancellationTokenSource())
    {
        cancelled.Cancel();
        string output=Path.Combine(root,"cancelled.squashfs");
        bool canceled=false;
        try { await SquashTools.PackAsync(advancedSource,output,new PackOptions(),null,cancelled.Token); }
        catch(OperationCanceledException) { canceled=true; }
        Require(canceled && !File.Exists(output),"Canceled packing published an image");
        Require(!Directory.EnumerateFiles(root,".winsqfs-*.partial").Any(),"Packing left a temporary file");
    }
    Console.WriteLine("PASS advanced compressor options, Unicode paths, output containment and pack cancellation");
    var zh=System.Globalization.CultureInfo.GetCultureInfo("zh-CN");
    var en=System.Globalization.CultureInfo.GetCultureInfo("en-US");
    var tw=System.Globalization.CultureInfo.GetCultureInfo("zh-TW");
    Require(Localization.Resolve("system",zh)=="zh-CN","Simplified Chinese system language resolution");
    Require(Localization.Resolve("system",en)=="en-US","English system language resolution");
    Require(Localization.Resolve("system",tw)=="en-US","Unsupported locale fallback");
    Require(Localization.Resolve("en-US",zh)=="en-US","Explicit language preference");
    Localization.Initialize("zh-CN");
    Require(Localization.T("Folder")=="文件夹","Chinese embedded resource missing");
    Require(Localization.T("Verified {0} files, {1} bytes.",2,64)=="已校验 2 个文件，共 64 字节。","Localized format");
    bool localizedError=false;
    try { _=new PackOptions(BlockSize:123).Arguments(); }
    catch(ArgumentException ex) { localizedError=ex.Message.Contains("数据块大小"); }
    Require(localizedError,"Chinese validation error missing");
    Localization.Initialize("en-US");
    Require(Localization.T("Folder")=="Folder","English resource fallback");
    Console.WriteLine("PASS localization, language resolution and translated validation");
    Console.WriteLine("All Windows integration tests passed.");
}
finally { Directory.Delete(root,true); }
