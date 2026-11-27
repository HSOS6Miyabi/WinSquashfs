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
    public uint Inode { get; }
    public ulong Size { get; }
    public DateTimeOffset Modified { get; }
    public bool IsDirectory => Type is 1 or 8;
    public bool IsFile => Type is 2 or 9;
    public string Kind => IsDirectory ? "Folder" : IsFile ? "File" : Type is 3 or 10 ? "Symbolic link" : "Special file";
    public string ArchivePath => Parent is null ? "/" : Parent.ArchivePath.TrimEnd('/') + "/" + Name;
    public string WindowsPath => Parent is null ? "\\" : Parent.WindowsPath.TrimEnd('\\') + "\\" + WindowsName;
    internal Dictionary<string, Entry> ChildMap { get; } = new(StringComparer.OrdinalIgnoreCase);
    internal List<Entry> ChildList { get; } = new();
    public IReadOnlyList<Entry> Children => ChildList;
    internal Entry(IntPtr p, Entry? parent)
    {
        Pointer = p; Parent = parent;
        Name = parent is null ? "" : Marshal.PtrToStringUTF8(Native.ws_name(p)) ?? throw new IOException("Invalid file name.");
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
    private readonly ImageHandle handle;
    private readonly object gate = new();
    private bool disposed;
    public string Path { get; }
    public Entry Root { get; }
    public ulong ImageSize { get; }
    public uint BlockSize { get; }
    public uint Compression { get; }
    public string Codec => Compression switch { 1 => "gzip", 2 => "lzma", 3 => "lzo", 4 => "xz", 5 => "lz4", 6 => "zstd", _ => "Unknown" };

    public Archive(string path)
    {
        Path = System.IO.Path.GetFullPath(path);
        int status = Native.ws_open(Path, out handle);
        if (status != 0) { handle?.Dispose(); throw new IOException($"Cannot open SquashFS image (error {status})."); }
        try
        {
            ImageSize = Native.ws_image_size(handle); BlockSize = Native.ws_block_size(handle); Compression = Native.ws_compression(handle);
            Root = new Entry(Native.ws_root(handle), null);
            var pending = new Stack<Entry>();
            pending.Push(Root);
            while (pending.TryPop(out var parent))
            {
                for (IntPtr p = Native.ws_child(parent.Pointer); p != IntPtr.Zero; p = Native.ws_next(p))
                {
                    var child = new Entry(p, parent);
                    string name = WindowsNames.Encode(child.Name);
                    string unique = name;
                    for (int suffix = 2; parent.ChildMap.ContainsKey(unique); suffix++) unique = name + "~" + suffix;
                    child.WindowsName = unique;
                    parent.ChildMap.Add(unique, child); parent.ChildList.Add(child);
                    if (child.IsDirectory) pending.Push(child);
                }
                parent.ChildList.Sort((a,b) => StringComparer.OrdinalIgnoreCase.Compare(a.WindowsName,b.WindowsName));
            }
        }
        catch { handle.Dispose(); throw; }
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
            ObjectDisposedException.ThrowIf(disposed, this);
            if (!entry.IsFile) throw new IOException("Not a regular file.");
            var owner = entry;
            while (owner.Parent != null) owner = owner.Parent;
            if (!ReferenceEquals(owner, Root)) throw new ArgumentException("Entry belongs to another image.");
            if (offset >= entry.Size || length == 0) return 0;
            length = (uint)Math.Min((ulong)length, entry.Size - offset);
            int result = Native.ws_read(handle, entry.Pointer, offset, buffer, length);
            if (result < 0) throw new IOException($"SquashFS read failed (error {result}).");
            return result;
        }
    }

    public unsafe int Read(Entry entry, ulong offset, Span<byte> buffer)
    {
        fixed (byte* p = buffer) return Read(entry, offset, (IntPtr)p, (uint)buffer.Length);
    }

    public void ExportFile(Entry entry, string path, CancellationToken token = default)
    {
        if (!entry.IsFile) throw new IOException("Select a regular file.");
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
        }
        catch { output.Dispose(); File.Delete(path); throw; }
    }

    public ArchiveVerification Verify(Action<long, ulong>? progress = null, CancellationToken token = default)
    {
        var pending = new Stack<Entry>();
        pending.Push(Root);
        var buffer = new byte[1024 * 1024];
        long files = 0; ulong bytes = 0;
        var inodes = new HashSet<uint>();
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
                catch (IOException ex) { throw new IOException($"Unable to read {item.ArchivePath} at offset {offset}.", ex); }
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

    public void ExportEntry(Entry entry, string path, CancellationToken token = default)
    {
        if (entry.IsFile) { ExportFile(entry, path, token); return; }
        if (!entry.IsDirectory) throw new IOException("Select a file or directory.");
        if (File.Exists(path) || Directory.Exists(path)) throw new IOException("Destination already exists.");
        string staging = path + "." + Guid.NewGuid().ToString("N") + ".partial";
        try
        {
            Directory.CreateDirectory(staging);
            var pending = new Stack<(Entry Entry, string Path)>();
            pending.Push((entry, staging));
            while (pending.TryPop(out var item))
            {
                token.ThrowIfCancellationRequested();
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
                        File.SetLastWriteTimeUtc(destination, child.Modified.UtcDateTime);
                    }
                }
            }
            token.ThrowIfCancellationRequested();
            Directory.Move(staging, path);
        }
        finally { if (Directory.Exists(staging)) Directory.Delete(staging, true); }
    }

    public void Dispose()
    {
        lock (gate) { if (!disposed) { disposed = true; handle.Dispose(); } }
    }
}
