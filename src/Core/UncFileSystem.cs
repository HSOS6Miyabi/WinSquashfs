// SPDX-License-Identifier: GPL-3.0-only
using System.Security.AccessControl;
using System.Security.Principal;
using System.Security.Cryptography;
using System.Text;
using Fsp;
using FileInfo = Fsp.Interop.FileInfo;
using VolumeInfo = Fsp.Interop.VolumeInfo;

namespace WinSquashfs.Core;

public sealed record UncShare(string Name, string Root)
{
    public const string ServerName = "sfs$";
    public string Prefix => "\\" + ServerName + "\\" + Name;
    public string UncPath => "\\" + Prefix;

    public UncShare Validate()
    {
        if (string.IsNullOrWhiteSpace(Name) || Name.Length > 64 ||
            !Name.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_'))
            throw new ArgumentException(Localization.T("Share name must contain 1–64 ASCII letters, digits, underscores or hyphens."));
        if (!Path.IsPathFullyQualified(Root) || Root.StartsWith(@"\\", StringComparison.Ordinal))
            throw new ArgumentException(Localization.T("Choose an absolute local directory as the share root."));
        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(Root));
        var drive = new DriveInfo(Path.GetPathRoot(root)!);
        if (drive.DriveType is not (DriveType.Fixed or DriveType.Removable) ||
            string.Equals(drive.DriveFormat, "SquashFS", StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException(Localization.T("Share roots must be on a local physical drive."));
        if (!Directory.Exists(root)) throw new DirectoryNotFoundException(root);
        for (DirectoryInfo? dir = new(root); dir != null; dir = dir.Parent)
            if ((dir.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new ArgumentException(Localization.T("The share root cannot traverse a reparse point."));
        return new UncShare(Name, root);
    }
    public string? ImagePath(string image)
    {
        var relative = Path.GetRelativePath(Root, Path.GetFullPath(image));
        if (relative == "." || Path.IsPathRooted(relative) || relative == ".." ||
            relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal)) return null;
        if (!UncFileSystem.IsImageName(relative)) return null;
        return UncPath + "\\" + relative;
    }
}

internal sealed class UncFileSystem : FileSystemBase
{
    private const int WriteProtected = unchecked((int)0xC00000A2);
    private const int NameNotFound = unchecked((int)0xC0000034);
    private const int NotDirectory = unchecked((int)0xC0000103);
    private const int IsDirectory = unchecked((int)0xC00000BA);
    private const int Corrupt = unchecked((int)0xC0000102);
    private readonly UncShare share;
    private readonly ImageCache cache;
    private readonly byte[] security;
    private readonly LocalReadIdentity readIdentity;
    private static readonly StringComparer Compare = StringComparer.OrdinalIgnoreCase;

    private sealed class Node : IDisposable
    {
        internal readonly string Name;
        internal readonly string PhysicalPath;
        internal readonly Entry? Entry;
        internal readonly ImageCache.Lease? Lease;
        internal bool Directory => Entry == null || Entry.IsDirectory;
        internal Node(string name, string physicalPath, Entry? entry = null, ImageCache.Lease? lease = null)
        { Name = name; PhysicalPath = physicalPath; Entry = entry; Lease = lease; }
        public void Dispose() => Lease?.Dispose();
    }
    private sealed record Row(string Name, FileInfo Info);

    internal UncFileSystem(UncShare share, ImageCache cache, LocalReadIdentity readIdentity)
    {
        this.share = share;
        this.cache = cache;
        this.readIdentity = readIdentity;
        using var identity = WindowsIdentity.GetCurrent();
        string sid = identity.User?.Value ?? throw new UnauthorizedAccessException(Localization.T("Cannot identify the current account."));
        var sd = new RawSecurityDescriptor($"O:{sid}G:{sid}D:P(A;;0x1200A9;;;{sid})");
        security = new byte[sd.BinaryLength];
        sd.GetBinaryForm(security, 0);
    }

    internal static bool IsImageName(string name) => Path.GetExtension(name).ToLowerInvariant() is ".squashfs" or ".sqfs" or ".sfs";
    private static bool SafeComponent(string part) =>
        part.Length is > 0 and <= 255 && part != "." && part != ".." &&
        !part.EndsWith('.') && !part.EndsWith(' ') && part.IndexOfAny(Path.GetInvalidFileNameChars()) < 0 &&
        part.IndexOf(':') < 0;

    private Node? Resolve(string name) => readIdentity.Run(() => ResolveCore(name));

    private static bool IsPlainPath(string path)
    {
        for (string? p = path; p != null; p = Path.GetDirectoryName(p))
        {
            var attr = File.GetAttributes(p);
            if ((attr & FileAttributes.ReparsePoint) != 0) return false;
        }
        return true;
    }

    private Node? ResolveCore(string name)
    {
        if (!IsPlainPath(share.Root)) return null;
        var parts = name.Split('\\', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Any(p => !SafeComponent(p))) return null;
        string physical = share.Root;
        string normalized = "";
        for (int i = 0; i < parts.Length; i++)
        {
            physical = Path.Combine(physical, parts[i]);
            FileAttributes attributes;
            try { attributes = File.GetAttributes(physical); }
            catch (FileNotFoundException) { return null; }
            catch (DirectoryNotFoundException) { return null; }
            if ((attributes & FileAttributes.ReparsePoint) != 0) return null;
            normalized += "\\" + parts[i];
            if ((attributes & FileAttributes.Directory) != 0) continue;
            if (!IsImageName(parts[i])) return null;
            var lease = cache.Acquire(physical);
            try
            {
                var entry = lease.Image.Find(string.Join("\\", parts.Skip(i + 1)));
                if (entry == null || (!entry.IsDirectory && !entry.IsFile))
                { lease.Dispose(); return null; }
                return new Node(normalized + (entry.Parent == null ? "" : entry.WindowsPath), physical, entry, lease);
            }
            catch { lease.Dispose(); throw; }
        }
        return new Node(normalized.Length == 0 ? "\\" : normalized, physical);
    }

    private static ulong Index(string path, uint inode = 0)
    {
        byte[] hash = SHA256.HashData(Encoding.Unicode.GetBytes(path.ToUpperInvariant() + "\0" + inode));
        return BitConverter.ToUInt64(hash) | 1UL;
    }
    private static FileInfo DirectoryInfoFor(string physical)
    {
        ulong time = (ulong)File.GetLastWriteTimeUtc(physical).ToFileTimeUtc();
        return new FileInfo { FileAttributes = 0x11, CreationTime = time, LastAccessTime = time,
            LastWriteTime = time, ChangeTime = time, IndexNumber = Index(physical) };
    }
    private static FileInfo ArchiveInfo(string path, Entry entry)
    {
        ulong time = (ulong)entry.Modified.UtcDateTime.ToFileTimeUtc();
        return new FileInfo
        {
            FileAttributes = entry.IsDirectory ? 0x11u : 0x21u,
            FileSize = entry.Size, AllocationSize = (entry.Size + 4095) / 4096 * 4096,
            CreationTime = time, LastAccessTime = time, LastWriteTime = time, ChangeTime = time,
            IndexNumber = Index(path, entry.Parent == null ? 0 : entry.Inode)
        };
    }
    private FileInfo Info(Node node) => node.Entry == null ? readIdentity.Run(() => DirectoryInfoFor(node.PhysicalPath)) : ArchiveInfo(node.PhysicalPath, node.Entry);

    public override int Init(object host)
    {
        var h = (FileSystemHost)host;
        h.Prefix = share.Prefix;
        h.PrefixOnly = true;
        h.ReadOnlyVolume = true;
        h.SectorSize = 4096; h.SectorsPerAllocationUnit = 1; h.MaxComponentLength = 255;
        h.FileInfoTimeout = 0; h.CasePreservedNames = true; h.CaseSensitiveSearch = false;
        h.UnicodeOnDisk = true; h.PersistentAcls = true; h.FileSystemName = "SquashFS";
        h.VolumeCreationTime = (ulong)DateTime.UtcNow.ToFileTimeUtc();
        h.VolumeSerialNumber = (uint)Index(share.Root);
        return STATUS_SUCCESS;
    }

    public override int ExceptionHandler(Exception ex) => ex switch
    {
        UnauthorizedAccessException => STATUS_ACCESS_DENIED,
        FileNotFoundException or DirectoryNotFoundException => NameNotFound,
        IOException => Corrupt,
        ArgumentException => STATUS_INVALID_PARAMETER,
        _ => STATUS_UNEXPECTED_IO_ERROR
    };

    public override int GetVolumeInfo(out VolumeInfo info)
    {
        info = default;
        try { info.TotalSize = (ulong)new DriveInfo(Path.GetPathRoot(share.Root)!).TotalSize; }
        catch { info.TotalSize = 0; }
        info.FreeSize = 0; info.SetVolumeLabel("SquashFS"); return STATUS_SUCCESS;
    }
    public override int GetSecurityByName(string name, out uint attributes, ref byte[] descriptor)
    {
        attributes = 0;
        using var node = Resolve(name);
        if (node == null) return NameNotFound;
        attributes = Info(node).FileAttributes;
        if (descriptor != null) descriptor = (byte[])security.Clone();
        return STATUS_SUCCESS;
    }
    public override int Open(string name, uint options, uint access, out object node, out object desc,
        out FileInfo info, out string normalized)
    {
        node = desc = null!; info = default; normalized = null!;
        if ((access & 0x000D0116u) != 0) return WriteProtected;
        var resolved = Resolve(name);
        if (resolved == null) return NameNotFound;
        bool opened = false;
        try
        {
            if ((options & 1) != 0 && !resolved.Directory) return NotDirectory;
            if ((options & 0x40) != 0 && resolved.Directory) return IsDirectory;
            info = Info(resolved); normalized = resolved.Name;
            node = resolved; opened = true; return STATUS_SUCCESS;
        }
        finally { if (!opened) resolved.Dispose(); }
    }
    public override void Close(object node, object desc) => (node as Node)?.Dispose();
    public override int GetFileInfo(object node, object desc, out FileInfo info)
    { info = Info((Node)node); return STATUS_SUCCESS; }
    public override int GetSecurity(object node, object desc, ref byte[] descriptor)
    { descriptor = (byte[])security.Clone(); return STATUS_SUCCESS; }

    public override int Read(object node, object desc, IntPtr buffer, ulong offset, uint length, out uint transferred)
    {
        transferred = 0;
        var n = (Node)node;
        if (n.Entry?.IsFile != true || n.Lease == null) return IsDirectory;
        if (length == 0) return STATUS_SUCCESS;
        if (offset >= n.Entry.Size) return STATUS_END_OF_FILE;
        transferred = (uint)n.Lease.Image.Read(n.Entry, offset, buffer, length);
        return STATUS_SUCCESS;
    }

    private IReadOnlyList<Row> List(Node dir) => readIdentity.Run(() => ListCore(dir).ToArray());

    private IEnumerable<Row> ListCore(Node dir)
    {
        if (dir.Entry == null && !IsPlainPath(dir.PhysicalPath)) throw new UnauthorizedAccessException(Localization.T("Directory is a reparse point."));
        if (dir.Name != "\\")
        {
            yield return new Row(".", Info(dir));
            if (dir.Entry?.Parent != null)
                yield return new Row("..", ArchiveInfo(dir.PhysicalPath, dir.Entry.Parent));
            else
                yield return new Row("..", DirectoryInfoFor(Path.GetDirectoryName(dir.PhysicalPath) ?? share.Root));
        }
        if (dir.Entry != null)
        {
            foreach (var child in dir.Entry.Children)
                if (child.IsDirectory || child.IsFile)
                    yield return new Row(child.WindowsName, ArchiveInfo(dir.PhysicalPath, child));
            yield break;
        }
        foreach (var child in new DirectoryInfo(dir.PhysicalPath).EnumerateFileSystemInfos())
        {
            FileAttributes attr;
            try { attr = child.Attributes; }
            catch (IOException) { continue; }
            if ((attr & FileAttributes.ReparsePoint) != 0 || !SafeComponent(child.Name)) continue;
            if ((attr & FileAttributes.Directory) == 0 && !IsImageName(child.Name)) continue;
            yield return new Row(child.Name, DirectoryInfoFor(child.FullName));
        }
    }
    public override bool ReadDirectoryEntry(object node, object desc, string pattern, string marker,
        ref object context, out string name, out FileInfo info)
    {
        name = null!; info = default;
        var n = (Node)node;
        if (!n.Directory) return false;
        if (context is not IEnumerator<Row> iterator)
        {
            iterator = List(n).Where(r => marker == null || Compare.Compare(r.Name, marker) > 0)
                .OrderBy(r => r.Name, Compare).ToList().GetEnumerator();
            context = iterator;
        }
        if (!iterator.MoveNext()) { iterator.Dispose(); return false; }
        name = iterator.Current.Name; info = iterator.Current.Info; return true;
    }
    public override int GetDirInfoByName(object node, object desc, string name, out string normalized, out FileInfo info)
    {
        normalized = null!; info = default;
        if (!((Node)node).Directory) return NotDirectory;
        var row = List((Node)node).FirstOrDefault(r => Compare.Equals(r.Name, name));
        if (row == null) return NameNotFound;
        normalized = row.Name; info = row.Info; return STATUS_SUCCESS;
    }
    public override int Create(string name, uint options, uint access, uint attrs, byte[] sd, ulong allocation,
        out object node, out object desc, out FileInfo info, out string normalized)
    { node = desc = null!; info = default; normalized = null!; return WriteProtected; }
    public override int Write(object node, object desc, IntPtr buffer, ulong offset, uint length, bool end, bool constrained,
        out uint transferred, out FileInfo info)
    { transferred = 0; info = default; return WriteProtected; }
    public override int Overwrite(object node, object desc, uint attrs, bool replace, ulong size, out FileInfo info)
    { info = default; return WriteProtected; }
    public override int SetBasicInfo(object node, object desc, uint attrs, ulong creation, ulong access, ulong write, ulong change, out FileInfo info)
    { info = default; return WriteProtected; }
    public override int SetFileSize(object node, object desc, ulong size, bool allocation, out FileInfo info)
    { info = default; return WriteProtected; }
    public override int CanDelete(object node, object desc, string name) => WriteProtected;
    public override int Rename(object node, object desc, string name, string newName, bool replace) => WriteProtected;
    public override int SetSecurity(object node, object desc, AccessControlSections sections, byte[] sd) => WriteProtected;
    public override int SetVolumeLabel(string label, out VolumeInfo info) { info = default; return WriteProtected; }
}

public sealed class UncSession : IDisposable
{
    private readonly ImageCache cache;
    private readonly LocalReadIdentity readIdentity;
    private readonly List<FileSystemHost> hosts = new();
    private int disposed;
    public IReadOnlyList<UncShare> Shares { get; }
    public int CachedImages => cache.Count;
    public int ActiveImages => cache.ActiveCount;

    public UncSession(IEnumerable<UncShare> shares, int idleSeconds = 60, int maxIdleImages = 8)
    {
        Shares = shares.Select(s => s.Validate()).ToArray();
        if (Shares.Count == 0 || Shares.Select(s => s.Name).Distinct(StringComparer.OrdinalIgnoreCase).Count() != Shares.Count)
            throw new ArgumentException(Localization.T("At least one share is required; names must be unique."));
        cache = new ImageCache(idleSeconds, maxIdleImages);
        try { readIdentity = new LocalReadIdentity(); }
        catch { cache.Dispose(); throw; }
        try
        {
            foreach (var share in Shares)
            {
                var host = new FileSystemHost(new UncFileSystem(share, cache, readIdentity));
                hosts.Add(host);
                int status = host.Mount(null);
                if (status < 0) throw new IOException(Localization.T("Cannot start {0} (NTSTATUS 0x{1:X8}).", share.UncPath, status));
            }
        }
        catch { Dispose(); throw; }
    }
    public void Dispose()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        try
        {
            foreach (var host in hosts) host.Dispose();
        }
        finally
        {
            hosts.Clear();
            cache.Dispose();
            readIdentity.Dispose();
        }
    }
}
