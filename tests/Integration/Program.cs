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
