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
    private readonly List<IDisposable> watches = new();
    private bool completeWatch;
    private int watchLost, initialized;
    private bool HasFullWatch => completeWatch && Volatile.Read(ref watchLost) == 0;
    private long identityChecks;
    private int invalid, disposed;
    internal Native.SourceRead Reader { get; }
    internal ulong Size => checked((ulong)original.Length);
    internal bool Valid => Volatile.Read(ref invalid) == 0 && Volatile.Read(ref disposed) == 0;
    internal bool UsesOplock { get; }
    internal bool HasStableIdentity => original.HasIdentity;
    internal long IdentityChecks => Interlocked.Read(ref identityChecks);
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

    [StructLayout(LayoutKind.Sequential)]
    private struct IdInformation
    {
        internal ulong Volume, Low, High;
    }
    [DllImport("kernel32.dll", EntryPoint = "GetFileInformationByHandleEx", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetId(SafeFileHandle handle, int kind, out IdInformation info, uint length);
    [DllImport("kernel32.dll", EntryPoint = "CreateFileW", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security,
        uint creation, uint flags, IntPtr template);

    private readonly record struct Stamp(ulong LegacyVolume, ulong LegacyId, ulong ExtendedVolume,
        ulong IdLow, ulong IdHigh, long Length, ulong Creation, ulong Write, long Change)
    {
        private bool HasExtended => IdLow != 0 || IdHigh != 0;
        internal bool HasIdentity => HasExtended || LegacyId != 0;

        internal bool Matches(Stamp other)
        {
            if (Length != other.Length) return false;
            if (HasExtended && other.HasExtended)
            {
                if (ExtendedVolume != other.ExtendedVolume || IdLow != other.IdLow || IdHigh != other.IdHigh)
                    return false;
            }
            else if (LegacyId != 0 && other.LegacyId != 0 &&
                (LegacyId != other.LegacyId || LegacyVolume != other.LegacyVolume)) return false;
            return (Creation == 0 || other.Creation == 0 || Creation == other.Creation) &&
                (Write == 0 || other.Write == 0 || Write == other.Write) &&
                (Change == 0 || other.Change == 0 || Change == other.Change);
        }
    }

    private static ulong Join(uint high, uint low) => ((ulong)high << 32) | low;

    [DllImport("kernel32.dll", EntryPoint = "GetFinalPathNameByHandleW", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandle(SafeFileHandle handle, System.Text.StringBuilder path, uint length, uint flags);

    private static string? ResolvedPath(SafeFileHandle handle)
    {
        var buffer = new System.Text.StringBuilder(512);
        uint length = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Capacity, 0);
        if (length == 0) return null;
        if (length >= buffer.Capacity)
        {
            buffer.EnsureCapacity(checked((int)length + 1));
            length = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Capacity, 0);
        }
        if (length == 0 || length >= buffer.Capacity) return null;
        try { return UncShare.NormalizeSourcePath(buffer.ToString()); }
        catch (Exception ex) when (ex is ArgumentException or IOException or NotSupportedException) { return null; }
    }

    private Stamp Snapshot(SafeFileHandle handle)
    {
        Interlocked.Increment(ref identityChecks);
        bool hasInfo = GetFileInformationByHandle(handle, out var info);
        bool hasBasic = GetBasic(handle, 0, out var basic, (uint)Marshal.SizeOf<BasicInformation>());
        bool hasStandard = GetStandard(handle, 1, out var standard, (uint)Marshal.SizeOf<StandardInformation>());
        if ((hasInfo && (info.Attributes & (uint)FileAttributes.Directory) != 0) ||
            (hasBasic && (basic.Attributes & (uint)FileAttributes.Directory) != 0) ||
            (hasStandard && standard.Directory != 0))
            throw new IOException(Localization.T("Source is not a regular file."));
        if (hasStandard && standard.DeletePending != 0) throw new FileNotFoundException();

        // Network servers may return only partial identity information.
        // File length and available timestamps are sufficient for a best-effort
        // generation check; missing IDs do not prevent reading an image.
        if (!GetId(handle, 18, out var extended, (uint)Marshal.SizeOf<IdInformation>()))
            extended = default;
        long length = hasStandard ? standard.Length : RandomAccess.GetLength(handle);
        ulong creation = hasBasic ? unchecked((ulong)basic.Creation) :
            hasInfo ? Join(info.CreationHigh, info.CreationLow) : 0;
        ulong write = hasBasic ? unchecked((ulong)basic.Write) :
            hasInfo ? Join(info.WriteHigh, info.WriteLow) : 0;
        return new Stamp(hasInfo ? info.VolumeSerial : 0, hasInfo ? Join(info.IndexHigh, info.IndexLow) : 0,
            extended.Volume, extended.Low, extended.High, length, creation, write, hasBasic ? basic.Change : 0);
    }

    private SafeFileHandle Open() => identity.Run(() =>
    {
        return File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete,
            FileOptions.Asynchronous | FileOptions.RandomAccess);
    });

    private SafeFileHandle OpenProbe() => identity.Run(() =>
    {
        string nativePath = path.StartsWith(@"\\?\", StringComparison.Ordinal) ? path :
            path.StartsWith(@"\\", StringComparison.Ordinal) ? @"\\?\UNC\" + path[2..] : @"\\?\" + path;
        // Metadata-only access with all sharing flags does not introduce a
        // conflicting data handle while another program edits the source.
        var handle = CreateFile(nativePath, 0x80, 7, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (handle.IsInvalid)
        {
            int error = Marshal.GetLastWin32Error();
            handle.Dispose();
            throw new IOException(path, new System.ComponentModel.Win32Exception(error));
        }
        return handle;
    });

    internal VolatileImageSource(string path, LocalReadIdentity identity)
    {
        this.path = path;
        this.identity = identity;
        Reader = ReadNative;
        SafeFileHandle? initial = null;
        try
        {
            lock (gate)
            {
                // Subscribe before capturing identity, so concurrent changes
                // during initialization also invalidate the generation.
                completeWatch = SubscribeChanges(path);
                initial = Open();
                string? resolved = ResolvedPath(initial);
                if (resolved != null)
                {
                    if (UncShare.IsProjectedPath(resolved))
                        throw new IOException(Localization.T("The source cannot be another WinSquashfs volume."));
                    if (!string.Equals(path, resolved, StringComparison.OrdinalIgnoreCase))
                        completeWatch &= SubscribeChanges(resolved);
                }
                string effectivePath = resolved ?? path;
                if (!effectivePath.StartsWith(@"\\", StringComparison.Ordinal))
                    oplock = SourceOplock.TryRequest(initial);

                original = Snapshot(initial);
                if (oplock != null)
                {
                    retained = initial;
                    initial = null;
                    UsesOplock = true;
                    oplock.Monitor(Invalidate);
                }

                EnsureValid();
                Volatile.Write(ref initialized, 1);
            }
        }
        catch { Dispose(); throw; }
        finally { initial?.Dispose(); }
    }

    private bool SubscribeChanges(string watchedPath)
    {
        bool complete = true;
        // Share watches among images in the same directory. Ancestor watches
        // detect directory renames/deletions; only name changes are relevant.
        string? item = watchedPath;
        bool ancestor = false;
        while (item != null && Path.GetDirectoryName(item) is string parent &&
            !string.IsNullOrEmpty(Path.GetFileName(item)))
        {
            try
            {
                var subscription = identity.Run(() =>
                    identity.Changes.Subscribe(parent, Path.GetFileName(item), Invalidate, MonitoringLost, ancestor));
                watches.Add(subscription);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException or PlatformNotSupportedException)
            {
                complete = false;
            }
            item = parent;
            ancestor = true;
        }
        return complete;
    }

    private void MonitoringLost()
    {
        Volatile.Write(ref watchLost, 1);
        if (Volatile.Read(ref initialized) != 0 && Valid)
            ThreadPool.QueueUserWorkItem(_ =>
            {
                if (!Valid) return;
                try { ValidatePath(); } catch (Exception) { /* The source has been invalidated. */ }
            });
    }

    private Stamp ProbeSnapshot()
    {
        try { using var handle = OpenProbe(); return Snapshot(handle); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or NotSupportedException)
        {
            // A server may reject metadata-only handles but permit file reads.
            using var handle = Open();
            return Snapshot(handle);
        }
    }

    // Only callers accessing an existing image trigger this check. A healthy
    // local oplock plus complete notifications needs no additional disk I/O.
    internal void ValidatePath()
    {
        try
        {
            lock (gate)
            {
                EnsureValid();
                if (oplock != null && !oplock.Broken && HasFullWatch && HasStableIdentity) return;
                if (!original.Matches(ProbeSnapshot()) || (oplock != null && oplock.Broken))
                    throw new IOException(Localization.T("The source image was removed, replaced or modified."));
                EnsureValid();
            }
        }
        catch { Invalidate(); throw; }
    }

    internal void EnsureValid()
    {
        if (!Valid) throw new FileNotFoundException(Localization.T("The source image was removed, replaced or modified."), path);
    }

    private void CheckLocked(SafeFileHandle handle)
    {
        EnsureValid();
        if (oplock != null ? oplock.Broken : !original.Matches(Snapshot(handle)))
            throw new IOException(Localization.T("The source image was removed, replaced or modified."));
    }

    internal void BeginRead()
    {
        try
        {
            lock (gate)
            {
                EnsureValid();
                if (active != null) throw new InvalidOperationException(Localization.T("A source read is already active."));
                if (retained != null && !HasFullWatch) ValidatePath();
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
                if (active == null) throw new IOException(Localization.T("Source handle is closed."));
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
            foreach (var watch in watches) watch.Dispose();
            watches.Clear();
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        Interlocked.Exchange(ref invalid, 1);
        CloseSource();
    }
}
