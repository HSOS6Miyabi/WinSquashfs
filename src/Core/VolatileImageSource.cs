// SPDX-License-Identifier: GPL-3.0-only
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace WinSquashfs.Core;

// Source lifetime for the on-demand UNC filesystem. An RH oplock retains a
// local read handle only while the filesystem grants it. Otherwise every
// logical read uses a short-lived handle and checks the original file identity.
internal sealed class VolatileImageSource : IDisposable
{
    private readonly string path;
    private readonly LocalReadIdentity identity;
    private readonly object gate = new();
    private readonly Stamp original;
    private SafeFileHandle? retained, active;
    private SourceOplock? oplock;
    private FileSystemWatcher? watcher;
    private Timer? timer;
    private int invalid, disposed, probing;
    internal Native.SourceRead Reader { get; }
    internal ulong Size => checked((ulong)original.Length);
    internal bool Valid => Volatile.Read(ref invalid) == 0 && Volatile.Read(ref disposed) == 0;
    internal bool UsesOplock { get; }
    internal event Action? Invalidated;

    [StructLayout(LayoutKind.Sequential)]
    private struct HandleInformation
    {
        internal uint Attributes, CreationLow, CreationHigh, AccessLow, AccessHigh, WriteLow, WriteHigh,
            VolumeSerial, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct BasicInformation
    {
        internal long Creation, Access, Write, Change;
        internal uint Attributes;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct StandardInformation
    {
        internal long Allocation, Length;
        internal uint Links;
        internal byte DeletePending, Directory;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle handle, out HandleInformation info);
    [DllImport("kernel32.dll", EntryPoint = "GetFileInformationByHandleEx", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetBasic(SafeFileHandle handle, int kind, out BasicInformation info, uint length);
    [DllImport("kernel32.dll", EntryPoint = "GetFileInformationByHandleEx", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetStandard(SafeFileHandle handle, int kind, out StandardInformation info, uint length);

    private readonly record struct Stamp(uint Volume, ulong FileId, long Length, ulong Creation, ulong Write, long Change);

    private static ulong Join(uint high, uint low) => ((ulong)high << 32) | low;

    private static Stamp Snapshot(SafeFileHandle handle)
    {
        if (!GetFileInformationByHandle(handle, out var info))
            throw new IOException("Cannot query source file identity.", new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()));
        if ((info.Attributes & ((uint)FileAttributes.Directory | (uint)FileAttributes.ReparsePoint)) != 0)
            throw new IOException("Source is not a regular file.");
        if (GetStandard(handle, 1, out var standard, (uint)Marshal.SizeOf<StandardInformation>()) &&
            (standard.DeletePending != 0 || standard.Directory != 0))
            throw new FileNotFoundException();
        long change = GetBasic(handle, 0, out var basic, (uint)Marshal.SizeOf<BasicInformation>()) ? basic.Change : 0;
        return new Stamp(info.VolumeSerial, Join(info.IndexHigh, info.IndexLow), checked((long)Join(info.SizeHigh, info.SizeLow)),
            Join(info.CreationHigh, info.CreationLow), Join(info.WriteHigh, info.WriteLow), change);
    }

    private SafeFileHandle Open() => identity.Run(() =>
    {
        if (!UncFileSystem.IsPlainPath(path)) throw new UnauthorizedAccessException(path);
        return File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete,
            FileOptions.Asynchronous | FileOptions.RandomAccess);
    });

    internal VolatileImageSource(string path, LocalReadIdentity identity)
    {
        this.path = path;
        this.identity = identity;
        Reader = ReadNative;
        SafeFileHandle? initial = null;
        try
        {
            initial = Open();
            // The Windows SMB redirector does not support application-issued
            // FSCTL_REQUEST_OPLOCK; remote sources use short-lived read handles.
            if (!path.StartsWith(@"\\", StringComparison.Ordinal) &&
                new DriveInfo(Path.GetPathRoot(path)!).DriveType != DriveType.Network)
                oplock = SourceOplock.TryRequest(initial);
            original = Snapshot(initial);
            if (oplock != null)
            {
                retained = initial;
                initial = null;
                UsesOplock = true;
                oplock.Monitor(Invalidate);
            }
            else
            {
                // No permanent source handle on filesystems without RH oplocks.
                if (original.FileId == 0) throw new IOException(Localization.T("The source filesystem did not provide a stable file identifier."));
            }

            try
            {
                watcher = identity.Run(() =>
                {
                    var w = new FileSystemWatcher(Path.GetDirectoryName(path)!, Path.GetFileName(path));
                    try
                    {
                        w.NotifyFilter = NotifyFilters.FileName | NotifyFilters.LastWrite | NotifyFilters.Size | NotifyFilters.CreationTime;
                        w.Deleted += (_, _) => Invalidate();
                        w.Renamed += (_, _) => Invalidate();
                        w.Created += (_, _) => Invalidate();
                        w.Changed += (_, _) => Invalidate();
                        w.Error += (_, _) => Invalidate();
                        w.EnableRaisingEvents = true;
                        return w;
                    }
                    catch { w.Dispose(); throw; }
                });
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or PlatformNotSupportedException)
            {
                // Identity checks continue even when directory notifications are unavailable.
            }
            timer = new Timer(_ => Probe(), null, 500, 500);
            EnsureValid();
        }
        catch { Dispose(); throw; }
        finally { initial?.Dispose(); }
    }

    internal void EnsureValid()
    {
        if (!Valid) throw new FileNotFoundException(Localization.T("The source image was removed, replaced or modified."), path);
    }

    private void CheckLocked(SafeFileHandle handle)
    {
        EnsureValid();
        if ((oplock != null && oplock.Broken) || Snapshot(handle) != original)
            throw new IOException(Localization.T("The source image was removed, replaced or modified."));
    }

    internal void BeginRead()
    {
        try
        {
            lock (gate)
            {
                EnsureValid();
                if (active != null) throw new InvalidOperationException("A source read is already active.");
                active = retained ?? Open();
                CheckLocked(active);
            }
        }
        catch { Invalidate(); throw; }
    }

    internal void CheckRead()
    {
        try
        {
            lock (gate)
            {
                EnsureValid();
                if (active == null) throw new IOException("Source handle is closed.");
                CheckLocked(active);
            }
        }
        catch { Invalidate(); throw; }
    }

    internal void EndRead()
    {
        lock (gate)
        {
            if (active != retained) active?.Dispose();
            active = null;
        }
    }

    private unsafe int ReadNative(ulong offset, IntPtr buffer, nuint size)
    {
        try
        {
            lock (gate)
            {
                EnsureValid();
                if (active == null || size > int.MaxValue || offset > Size || (ulong)size > Size - offset)
                    return -8; // SQFS_ERROR_OUT_OF_BOUNDS
                Span<byte> destination = new((void*)buffer, checked((int)size));
                long position = checked((long)offset);
                while (!destination.IsEmpty)
                {
                    EnsureValid();
                    int read = RandomAccess.Read(active, destination, position);
                    if (read == 0) throw new EndOfStreamException();
                    destination = destination[read..];
                    position += read;
                }
                EnsureValid();
                return 0;
            }
        }
        catch (Exception) { Invalidate(); return -2; }
    }

    private void Probe()
    {
        if (!Valid || Interlocked.Exchange(ref probing, 1) != 0) return;
        try
        {
            // Verify the path as well as the open file: deletion, ancestor
            // renames and replacement are detected even if a notification is lost.
            using var handle = Open();
            if (Snapshot(handle) != original) Invalidate();
        }
        catch (Exception) { Invalidate(); }
        finally { Volatile.Write(ref probing, 0); }
    }

    private void Invalidate()
    {
        if (Interlocked.Exchange(ref invalid, 1) != 0) return;
        CloseSource();
        // Never call the image cache or the archive while holding the source lock.
        var callback = Invalidated;
        if (callback != null) ThreadPool.QueueUserWorkItem(_ => callback());
    }

    private void CloseSource()
    {
        lock (gate)
        {
            oplock?.Dispose();
            oplock = null;
            if (active != retained) active?.Dispose();
            active = null;
            retained?.Dispose();
            retained = null;
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        Interlocked.Exchange(ref invalid, 1);
        timer?.Dispose();
        watcher?.Dispose();
        CloseSource();
    }
}
