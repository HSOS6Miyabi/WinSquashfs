// SPDX-License-Identifier: GPL-3.0-only
using System.Security.AccessControl;
using System.Security.Principal;
using Fsp;
using FileInfo = Fsp.Interop.FileInfo;
using VolumeInfo = Fsp.Interop.VolumeInfo;

namespace WinSquashfs.Core;

internal sealed class ReadOnlyFileSystem : FileSystemBase
{
    private readonly Archive archive;
    private readonly byte[] security;
    private const int WriteProtected = unchecked((int)0xC00000A2);
    private const int NameNotFound = unchecked((int)0xC0000034);
    private const int NotDirectory = unchecked((int)0xC0000103);
    private const int IsDirectory = unchecked((int)0xC00000BA);
    private const int NotSupported = unchecked((int)0xC00000BB);

    public ReadOnlyFileSystem(Archive archive)
    {
        this.archive = archive;
        using var identity = WindowsIdentity.GetCurrent();
        string sid = identity.User?.Value ?? throw new UnauthorizedAccessException(Localization.T("Cannot identify current Windows account."));
        var sd = new RawSecurityDescriptor($"O:{sid}G:{sid}D:P(A;;0x1200A9;;;{sid})");
        security = new byte[sd.BinaryLength];
        sd.GetBinaryForm(security, 0);
    }
    private static FileInfo Info(Entry e)
    {
        ulong time = (ulong)e.Modified.UtcDateTime.ToFileTimeUtc();
        return new FileInfo
        {
            FileAttributes = e.IsDirectory ? 0x11u : 0x21u,
            FileSize = e.Size, AllocationSize = (e.Size + 4095) / 4096 * 4096,
            CreationTime = time, LastAccessTime = time, LastWriteTime = time, ChangeTime = time,
            IndexNumber = e.Inode, HardLinks = 0
        };
    }
    public override int Init(object host)
    {
        var h = (FileSystemHost)host;
        h.SectorSize = 4096; h.SectorsPerAllocationUnit = 1; h.MaxComponentLength = 255;
        h.FileInfoTimeout = 1000; h.CaseSensitiveSearch = false; h.CasePreservedNames = true;
        h.UnicodeOnDisk = true; h.PersistentAcls = true; h.FileSystemName = "SquashFS"; h.ReadOnlyVolume = true;
        h.VolumeCreationTime = (ulong)archive.Root.Modified.UtcDateTime.ToFileTimeUtc();
        h.VolumeSerialNumber = unchecked((uint)archive.ImageSize ^ archive.Root.Inode);
        return STATUS_SUCCESS;
    }
    public override int GetVolumeInfo(out VolumeInfo v)
    {
        v = default; v.TotalSize = archive.ImageSize; v.FreeSize = 0;
        var label = Path.GetFileNameWithoutExtension(archive.Path);
        v.SetVolumeLabel(label.Length > 24 ? label[..24] : label);
        return STATUS_SUCCESS;
    }
    public override int GetSecurityByName(string name, out uint attrs, ref byte[] descriptor)
    {
        attrs = 0;
        var e = archive.Find(name);
        if (e is null || (!e.IsDirectory && !e.IsFile)) return NameNotFound;
        attrs = Info(e).FileAttributes;
        if (descriptor != null) descriptor = (byte[])security.Clone();
        return STATUS_SUCCESS;
    }
    public override int Open(string name, uint options, uint access, out object node, out object desc,
                             out FileInfo info, out string normalized)
    {
        node = desc = null!; info = default; normalized = null!;
        // FILE_WRITE_DATA, APPEND_DATA, WRITE_EA, WRITE_ATTRIBUTES, DELETE, WRITE_DAC, WRITE_OWNER
        if ((access & 0x000D0116u) != 0) return WriteProtected;
        var e = archive.Find(name);
        if (e is null) return NameNotFound;
        if (!e.IsDirectory && !e.IsFile) return NotSupported;
        if ((options & 1) != 0 && !e.IsDirectory) return NotDirectory;
        if ((options & 0x40) != 0 && e.IsDirectory) return IsDirectory;
        node = e; info = Info(e); normalized = e.WindowsPath;
        return STATUS_SUCCESS;
    }
    public override int GetFileInfo(object node, object desc, out FileInfo info)
    { info = Info((Entry)node); return STATUS_SUCCESS; }
    public override int GetSecurity(object node, object desc, ref byte[] descriptor)
    { descriptor = (byte[])security.Clone(); return STATUS_SUCCESS; }
    public override int Read(object node, object desc, IntPtr buffer, ulong offset, uint length, out uint transferred)
    {
        transferred = 0;
        var e = (Entry)node;
        if (!e.IsFile) return IsDirectory;
        if (length == 0) return STATUS_SUCCESS;
        if (offset >= e.Size) return STATUS_END_OF_FILE;
        try { transferred = (uint)archive.Read(e, offset, buffer, length); return STATUS_SUCCESS; }
        catch (IOException) { return STATUS_UNEXPECTED_IO_ERROR; }
    }
    public override bool ReadDirectoryEntry(object node, object desc, string pattern, string marker,
                                    ref object context, out string name, out FileInfo info)
    {
        name = null!; info = default;
        var dir = (Entry)node;
        if (!dir.IsDirectory) return false;
        if (context is not IEnumerator<(string Name, Entry Entry)> it)
        {
            IEnumerable<(string Name, Entry Entry)> List()
            {
                if (dir.Parent != null) { yield return (".", dir); yield return ("..", dir.Parent); }
                foreach (var child in dir.Children)
                    if (child.IsDirectory || child.IsFile) yield return (child.WindowsName, child);
            }
            it = List().Where(x => marker == null || StringComparer.OrdinalIgnoreCase.Compare(x.Name, marker) > 0).GetEnumerator();
            context = it;
        }
        if (!it.MoveNext()) { it.Dispose(); return false; }
        name = it.Current.Name; info = Info(it.Current.Entry); return true;
    }
    public override int GetDirInfoByName(object node, object desc, string name, out string normalized, out FileInfo info)
    {
        normalized = null!; info = default;
        var dir = (Entry)node;
        Entry? e = name == "." ? dir : name == ".." ? dir.Parent : dir.ChildMap.GetValueOrDefault(name);
        if (e is null || (!e.IsFile && !e.IsDirectory)) return NameNotFound;
        normalized = name is "." or ".." ? name : e.WindowsName; info = Info(e); return STATUS_SUCCESS;
    }
    public override int Create(string name, uint options, uint access, uint attrs, byte[] sd, ulong allocation,
                               out object node, out object desc, out FileInfo info, out string normalized)
    { node = desc = null!; info = default; normalized = null!; return WriteProtected; }
    public override int Write(object node, object desc, IntPtr buffer, ulong offset, uint length,
                              bool end, bool constrained, out uint transferred, out FileInfo info)
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

public sealed class MountedImage : IDisposable
{
    private readonly Archive archive;
    private readonly FileSystemHost host;
    private bool disposed;
    public string MountPoint { get; }
    public string ImagePath => archive.Path;

    public MountedImage(string path, string? drive = null)
    {
        if (drive != null && (drive.Length != 2 || !char.IsAsciiLetter(drive[0]) || drive[1] != ':'))
            throw new ArgumentException(Localization.T("Use a drive letter such as X:."));
        archive = new Archive(path);
        FileSystemHost? h = null;
        try
        {
            h = new FileSystemHost(new ReadOnlyFileSystem(archive));
            int result = h.Mount(drive);
            if (result < 0) throw new IOException(Localization.T("WinFsp mount failed (NTSTATUS 0x{0:X8}).", result));
            MountPoint = h.MountPoint();
            host = h;
        }
        catch { h?.Dispose(); archive.Dispose(); throw; }
    }
    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        try { host.Dispose(); } finally { archive.Dispose(); }
    }
}
