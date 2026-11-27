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
    Console.WriteLine("All Windows integration tests passed.");
}
finally { Directory.Delete(root,true); }
