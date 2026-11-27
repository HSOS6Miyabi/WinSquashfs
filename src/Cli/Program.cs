// SPDX-License-Identifier: GPL-3.0-only
using WinSquashfs.Core;

try
{
    switch (args)
    {
        case ["info", var image]:
            using (var archive = new Archive(image))
                Console.WriteLine($"{archive.Codec} {archive.BlockSize} {archive.ImageSize}");
            break;
        case ["list", var image, ..]:
            using (var archive = new Archive(image))
            {
                var root = args.Length == 3 ? archive.Find(args[2]) : archive.Root;
                if (root is null || !root.IsDirectory) throw new IOException("Directory not found.");
                foreach (var e in root.Children) Console.WriteLine($"{e.Kind}\t{e.Size}\t{e.Name}");
            }
            break;
        case ["read", var image, var entry, var offset, var count]:
            using (var archive = new Archive(image))
            {
                var e = archive.Find(entry) ?? throw new FileNotFoundException(entry);
                var size = int.Parse(count);
                if (size < 0 || size > 16 * 1024 * 1024) throw new ArgumentException("Count must be between 0 and 16 MiB.");
                byte[] buffer = new byte[size];
                int n = archive.Read(e, ulong.Parse(offset), buffer);
                Console.WriteLine(Convert.ToHexString(buffer.AsSpan(0,n)));
            }
            break;
        case ["mount", var image, ..]:
            using (var mount = new MountedImage(image, args.Length == 3 ? args[2] : null))
            {
                Console.WriteLine(mount.MountPoint);
                using var quit = new ManualResetEventSlim();
                Console.CancelKeyPress += (_, e) => { e.Cancel = true; quit.Set(); };
                quit.Wait();
            }
            break;
        case ["pack", var source, var output, ..]:
            await SquashTools.PackAsync(source, output, new PackOptions(Codec: args.Length == 4 ? args[3] : "zstd"), Console.WriteLine);
            break;
        case ["unpack", var image, var output]:
            await SquashTools.UnpackAsync(image, output, Console.WriteLine);
            break;
        default:
            Console.Error.WriteLine("winsqfs info|list|read|mount|pack|unpack ...");
            return 2;
    }
    return 0;
}
catch (Exception e) { Console.Error.WriteLine(e.Message); return 1; }
