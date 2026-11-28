// SPDX-License-Identifier: GPL-3.0-only
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace WinSquashfs.Core;

internal sealed class ImageHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public ImageHandle() : base(true) { }
    protected override bool ReleaseHandle() { Native.ws_close(handle); return true; }
}

internal static partial class Native
{
    private const string Dll = "winsqfs-native";
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern int ws_open([MarshalAs(UnmanagedType.LPUTF8Str)] string path, out ImageHandle image);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int SourceRead(ulong offset, IntPtr buffer, nuint length);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern int ws_open_source(ulong size, SourceRead read, out ImageHandle image);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern void ws_close(IntPtr image);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr ws_root(ImageHandle image);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr ws_child(IntPtr node);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr ws_next(IntPtr node);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr ws_name(IntPtr node);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern uint ws_type(IntPtr node);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern uint ws_mtime(IntPtr node);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern uint ws_inode(IntPtr node);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern ulong ws_size(IntPtr node);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern ulong ws_image_size(ImageHandle image);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern uint ws_block_size(ImageHandle image);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern uint ws_compression(ImageHandle image);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern int ws_read(ImageHandle image, IntPtr node, ulong offset, IntPtr buffer, uint length);
}

public sealed class Entry
{
    internal IntPtr Pointer { get; }
    public string Name { get; }
    public string WindowsName { get; internal set; }
    public Entry? Parent { get; }
    public uint Type { get; }
    public ulong Inode { get; }
    public ulong Size { get; }
    public DateTimeOffset Modified { get; }
    public bool IsDirectory => Type is 1 or 8;
    public bool IsFile => Type is 2 or 9;
    public string Kind => IsDirectory ? Localization.T("Folder") : IsFile ? Localization.T("File") : Type is 3 or 10 ? Localization.T("Symbolic link") : Localization.T("Special file");
    public string ArchivePath => BuildPath('/', false);
    public string WindowsPath => BuildPath('\\', true);
    private string BuildPath(char separator, bool windows)
    {
        var parts = new Stack<string>();
        for (Entry? node = this; node?.Parent != null; node = node.Parent)
            parts.Push(windows ? node.WindowsName : node.Name);
        return separator + string.Join(separator, parts);
    }
    internal Dictionary<string, Entry> ChildMap { get; } = new(StringComparer.OrdinalIgnoreCase);
    internal List<Entry> ChildList { get; } = new();
    public IReadOnlyList<Entry> Children { get; }
    internal Entry(ulong inode, string name, uint mode, ulong size, ulong mtime, Entry? parent)
    {
        Inode = inode; Parent = parent; Name = name; WindowsName = name; Size = size;
        Type = (mode & 0xF000u) switch { 0x4000 => 1u, 0x8000 => 2u, 0xA000 => 3u, _ => 4u };
        Modified = DateTimeOffset.FromUnixTimeSeconds((long)Math.Min(mtime,253402300799UL));
        Children = ChildList.AsReadOnly();
    }
    internal Entry(IntPtr p, Entry? parent)
    {
        Pointer = p; Parent = parent;
        Children = ChildList.AsReadOnly();
        Name = parent is null ? "" : Marshal.PtrToStringUTF8(Native.ws_name(p)) ?? throw new IOException(Localization.T("Invalid file name."));
        WindowsName = Name;
        Type = Native.ws_type(p); Inode = Native.ws_inode(p); Size = Native.ws_size(p);
        Modified = DateTimeOffset.FromUnixTimeSeconds(Native.ws_mtime(p));
    }
}

public static class WindowsNames
{
    private static readonly HashSet<string> Reserved = new(StringComparer.OrdinalIgnoreCase)
    { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
      "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9", "COM¹", "COM²", "COM³", "LPT¹", "LPT²", "LPT³" };
    public static string Encode(string name)
    {
        if (name.Length == 0) return "%0000";
        var b = new StringBuilder();
        bool device = Reserved.Contains(name.Split('.')[0].TrimEnd(' '));
        for (int i = 0; i < name.Length; i++)
        {
            char c = name[i];
            if (c < 32 || "<>:\"/\\|?*%".Contains(c) || (i == 0 && device) ||
                ((c == '.' || c == ' ') && name.AsSpan(i).TrimEnd(" .".AsSpan()).IsEmpty))
                b.Append('%').Append(((int)c).ToString("X4"));
            else b.Append(c);
        }
        string result = b.ToString();
        if (result.Length > 220)
        {
            string hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(name)))[..24];
            int end = 190;
            if (char.IsHighSurrogate(result[end - 1])) end--;
            result = result[..end] + "~" + hash;
        }
        return result;
    }
}

public sealed record ArchiveVerification(long Files, ulong Bytes);

public sealed class Archive : IDisposable
{
    private readonly ImageHandle? handle;
    private readonly ExtraArchive? extra;
    private readonly VolatileImageSource? source;
    private readonly LocalReadIdentity? ownedIdentity;
    private readonly object gate = new();
    private bool disposed;
    public string Path { get; }
    public Entry Root { get; }
    public ulong ImageSize { get; }
    public uint BlockSize { get; }
    public uint Compression { get; }
    public ImageFormat Format { get; }
    public string Codec => Format != ImageFormat.SquashFS ? Localization.T("Image-defined compression") : Compression switch { 1 => "gzip", 2 => "lzma", 3 => "lzo", 4 => "xz", 5 => "lz4", 6 => "zstd", _ => "Unknown" };

    public bool SourceValid => source?.Valid != false;
    internal bool UsesOplock => source?.UsesOplock == true;
    internal bool CanCacheIdle => source?.HasStableIdentity != false;
    internal long IdentityChecks => source?.IdentityChecks ?? 0;
    internal void ValidateSourcePath() => source?.ValidatePath();
    public event Action? SourceInvalidated
    {
        add { if (source != null) source.Invalidated += value; }
        remove { if (source != null) source.Invalidated -= value; }
    }
    internal void EnsureSourceValid() => source?.EnsureValid();

    public Archive(string path) : this(path, null) { }

    public static Archive OpenForBrowsing(string path) => new(path, new LocalReadIdentity(), true);

    internal Archive(string path, LocalReadIdentity? readIdentity, bool ownsIdentity = false)
    {
        ownedIdentity = ownsIdentity ? readIdentity : null;
        Path = UncShare.NormalizeSourcePath(path);
        try
        {
            if (readIdentity != null)
            {
                source = new VolatileImageSource(Path, readIdentity);
                source.BeginRead();
            }
            Format = ExtraArchive.Detect(Path, source);
            if (Format != ImageFormat.SquashFS)
            {
                extra = new ExtraArchive(Path, Format, source);
                ImageSize = extra.Size;
                BlockSize = extra.BlockSize;
                Root = new Entry(extra.Root,"",0x4000,0,extra.Modified,null);
                var directories = new HashSet<ulong> { Root.Inode };
                var queue = new Stack<Entry>();
                queue.Push(Root);
                while(queue.TryPop(out var parent))
                {
                    Exception? error = null;
                    ExtraArchive.Visit visit = (inode,name,length,mode,size,time) =>
                    {
                        try
                        {
                            string filename = Marshal.PtrToStringUTF8(name,checked((int)length)) ?? throw new IOException();
                            if(filename.Length == 0 || filename is "." or ".." || filename.Contains('/') || filename.Contains('\0'))
                                throw new IOException(Localization.T("Invalid file name."));
                            var child = new Entry(inode,filename,mode,size,time,parent);
                            AddChild(parent,child);
                            if(child.IsDirectory)
                            {
                                if(!directories.Add(inode)) throw new IOException(Localization.T("Invalid directory hierarchy."));
                                queue.Push(child);
                            }
                            return 0;
                        }
                        catch(Exception ex) { error=ex; return -1; }
                    };
                    try { extra.List(parent.Inode,visit); }
                    catch when(error != null) { throw error; }
                    parent.ChildList.Sort((a,b) => StringComparer.OrdinalIgnoreCase.Compare(a.WindowsName,b.WindowsName));
                }
                source?.CheckRead();
                return;
            }
            int status = source == null ? Native.ws_open(Path, out handle) :
                Native.ws_open_source(source.Size, source.Reader, out handle);
            source?.EnsureValid();
            if (status != 0) throw new IOException(Localization.T("Cannot open SquashFS image (error {0}).", status));
            ImageSize = Native.ws_image_size(handle); BlockSize = Native.ws_block_size(handle); Compression = Native.ws_compression(handle);
            Root = new Entry(Native.ws_root(handle), null);
            var pending = new Stack<Entry>();
            pending.Push(Root);
            while (pending.TryPop(out var parent))
            {
                for (IntPtr p = Native.ws_child(parent.Pointer); p != IntPtr.Zero; p = Native.ws_next(p))
                {
                    var child = new Entry(p, parent);
                    AddChild(parent, child);
                    if (child.IsDirectory) pending.Push(child);
                }
                parent.ChildList.Sort((a,b) => StringComparer.OrdinalIgnoreCase.Compare(a.WindowsName,b.WindowsName));
            }
            source?.CheckRead();
        }
        catch { handle?.Dispose(); extra?.Dispose(); source?.Dispose(); ownedIdentity?.Dispose(); throw; }
        finally { source?.EndRead(); }
    }

    private static void AddChild(Entry parent, Entry child)
    {
        string name = WindowsNames.Encode(child.Name);
        string unique = name;
        for(int suffix=2;parent.ChildMap.ContainsKey(unique);suffix++) unique=name+"~"+suffix;
        child.WindowsName=unique;
        parent.ChildMap.Add(unique,child);
        parent.ChildList.Add(child);
    }

    public Entry? Find(string path)
    {
        var node = Root;
        foreach (var component in path.Split(new[]{'\\','/'}, StringSplitOptions.RemoveEmptyEntries))
        {
            if (!node.ChildMap.TryGetValue(component, out var next)) return null;
            node = next;
        }
        return node;
    }

    public int Read(Entry entry, ulong offset, IntPtr buffer, uint length)
    {
        lock (gate)
        {
            source?.EnsureValid();
            ObjectDisposedException.ThrowIf(disposed, this);
            if (!entry.IsFile) throw new IOException(Localization.T("Not a regular file."));
            var owner = entry;
            while (owner.Parent != null) owner = owner.Parent;
            if (!ReferenceEquals(owner, Root)) throw new ArgumentException(Localization.T("Entry belongs to another image."));
            if (offset >= entry.Size || length == 0) return 0;
            length = (uint)Math.Min((ulong)length, entry.Size - offset);
            source?.BeginRead();
            try
            {
                int result = extra != null ? extra.Read(entry.Inode,offset,buffer,length) : Native.ws_read(handle!, entry.Pointer, offset, buffer, length);
                source?.EnsureValid();
                if (result < 0) throw new IOException(Localization.T("SquashFS read failed (error {0}).", result));
                source?.CheckRead();
                return result;
            }
            finally { source?.EndRead(); }
        }
    }

    public unsafe int Read(Entry entry, ulong offset, Span<byte> buffer)
    {
        fixed (byte* p = buffer) return Read(entry, offset, (IntPtr)p, (uint)buffer.Length);
    }

    private void ValidateEntry(Entry entry)
    {
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            var owner = entry;
            while (owner.Parent != null) owner = owner.Parent;
            if (!ReferenceEquals(owner, Root)) throw new ArgumentException(Localization.T("Entry belongs to another image."));
        }
    }

    public void ExportFile(Entry entry, string path, CancellationToken token = default)
    {
        ValidateEntry(entry);
        token.ThrowIfCancellationRequested();
        if (!entry.IsFile) throw new IOException(Localization.T("Select a regular file."));
        using var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        var buffer = new byte[1024 * 1024];
        try
        {
            for (ulong offset = 0; offset < entry.Size;)
            {
                token.ThrowIfCancellationRequested();
                int count = Read(entry, offset, buffer);
                if (count == 0) throw new EndOfStreamException();
                output.Write(buffer, 0, count); offset += (uint)count;
            }
            token.ThrowIfCancellationRequested();
            output.Flush();
            File.SetLastWriteTimeUtc(output.SafeFileHandle, entry.Modified.UtcDateTime);
        }
        catch { output.Dispose(); File.Delete(path); throw; }
    }

    public ArchiveVerification Verify(Action<long, ulong>? progress = null, CancellationToken token = default)
    {
        ValidateEntry(Root);
        token.ThrowIfCancellationRequested();
        var pending = new Stack<Entry>();
        pending.Push(Root);
        var buffer = new byte[1024 * 1024];
        long files = 0; ulong bytes = 0;
        var inodes = new HashSet<ulong>();
        while (pending.TryPop(out var item))
        {
            token.ThrowIfCancellationRequested();
            if (item.IsDirectory)
            {
                foreach (var child in item.Children) pending.Push(child);
                continue;
            }
            if (!item.IsFile || !inodes.Add(item.Inode)) continue;
            for (ulong offset = 0; offset < item.Size;)
            {
                token.ThrowIfCancellationRequested();
                int read;
                try { read = Read(item, offset, buffer); }
                catch (IOException ex) { throw new IOException(Localization.T("Unable to read {0} at offset {1}.", item.ArchivePath, offset), ex); }
                if (read == 0) throw new EndOfStreamException(item.ArchivePath);
                offset += (uint)read;
                bytes = checked(bytes + (uint)read);
            }
            files++;
            if (files % 32 == 0) progress?.Invoke(files, bytes);
        }
        progress?.Invoke(files, bytes);
        return new ArchiveVerification(files, bytes);
    }

    public void ExportEntry(Entry entry, string path, CancellationToken token = default, Action<string>? log = null)
    {
        ValidateEntry(entry);
        token.ThrowIfCancellationRequested();
        if (entry.IsFile) { ExportFile(entry, path, token); return; }
        if (!entry.IsDirectory) throw new IOException(Localization.T("Select a file or directory."));
        if (File.Exists(path) || Directory.Exists(path)) throw new IOException(Localization.T("Destination already exists."));
        path = System.IO.Path.GetFullPath(path);
        string staging = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(path)!, ".winsqfs-" + Guid.NewGuid().ToString("N") + ".partial");
        try
        {
            Directory.CreateDirectory(staging);
            var pending = new Stack<(Entry Entry, string Path)>();
            var directories = new List<(Entry Entry, string Path)>();
            pending.Push((entry, staging));
            while (pending.TryPop(out var item))
            {
                token.ThrowIfCancellationRequested();
                directories.Add(item);
                foreach (var child in item.Entry.Children)
                {
                    token.ThrowIfCancellationRequested();
                    string destination = System.IO.Path.Combine(item.Path, child.WindowsName);
                    if (child.IsDirectory)
                    {
                        Directory.CreateDirectory(destination);
                        pending.Push((child,destination));
                    }
                    else if (child.IsFile)
                    {
                        ExportFile(child, destination, token);
                    }
                    else log?.Invoke(Localization.T("Skipped {0} ({1})", child.ArchivePath, child.Kind));
                }
            }
            for (int i = directories.Count - 1; i >= 0; i--)
                Directory.SetLastWriteTimeUtc(directories[i].Path, directories[i].Entry.Modified.UtcDateTime);
            token.ThrowIfCancellationRequested();
            Directory.Move(staging, path);
        }
        finally { if (Directory.Exists(staging)) Directory.Delete(staging, true); }
    }

    public void Dispose()
    {
        lock (gate) { if (!disposed) { disposed = true; try { handle?.Dispose(); extra?.Dispose(); } finally { try { source?.Dispose(); } finally { ownedIdentity?.Dispose(); } } } }
    }
}
