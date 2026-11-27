// SPDX-License-Identifier: GPL-3.0-only
namespace WinSquashfs.Core;

internal sealed class ImageCache : IDisposable
{
    internal sealed class Cached
    {
        internal readonly string Path;
        internal readonly Archive Image;
        internal int References;
        internal long Expires;
        internal LinkedListNode<Cached>? IdleNode;
        internal Cached(string path, Archive image) { Path = path; Image = image; }
    }
    internal sealed class Lease : IDisposable
    {
        private ImageCache? owner;
        private readonly Cached cached;
        internal Archive Image => cached.Image;
        internal Lease(ImageCache owner, Cached cached) { this.owner = owner; this.cached = cached; }
        public void Dispose() => Interlocked.Exchange(ref owner, null)?.Release(cached);
    }

    private readonly object sync = new();
    private readonly Dictionary<string, Cached> images = new(StringComparer.OrdinalIgnoreCase);
    // All entries share the same timeout. Inactive entries are ordered by
    // release time, making expiration and the idle limit O(1) per entry.
    private readonly LinkedList<Cached> idle = new();
    private readonly long idleMilliseconds;
    private readonly int maxIdle;
    private readonly Timer timer;
    private long scheduled = long.MaxValue;
    private long expiryWakeups;
    private bool disposed;

    internal ImageCache(int idleSeconds, int maxIdleImages)
    {
        if (idleSeconds is < 0 or > 86400) throw new ArgumentOutOfRangeException(nameof(idleSeconds));
        if (maxIdleImages is < 0 or > 1024) throw new ArgumentOutOfRangeException(nameof(maxIdleImages));
        idleMilliseconds = idleSeconds * 1000L;
        maxIdle = maxIdleImages;
        timer = new Timer(_ => Expire(), null, Timeout.Infinite, Timeout.Infinite);
    }

    internal int Count { get { lock (sync) return images.Count; } }
    internal int ActiveCount { get { lock (sync) return images.Count - idle.Count; } }
    internal int OplockCount { get { lock (sync) return images.Values.Count(x => x.Image.UsesOplock); } }
    internal long IdentityChecks { get { lock (sync) return images.Values.Sum(x => x.Image.IdentityChecks); } }
    internal long ExpiryWakeups => Interlocked.Read(ref expiryWakeups);

    private void Unlink(Cached item)
    {
        if (item.IdleNode != null)
        {
            idle.Remove(item.IdleNode);
            item.IdleNode = null;
        }
    }

    private bool Current(Cached item) =>
        images.TryGetValue(item.Path, out var current) && ReferenceEquals(current, item);

    private void RemoveLocked(Cached item)
    {
        Unlink(item);
        images.Remove(item.Path);
    }

    private Lease AcquireLocked(Cached item)
    {
        Unlink(item);
        checked { item.References++; }
        ScheduleLocked();
        return new Lease(this, item);
    }

    internal Lease Acquire(string path, LocalReadIdentity identity)
    {
        path = Path.GetFullPath(path);
        Lease? existing = null;
        Cached? obsolete = null;
        lock (sync)
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            if (images.TryGetValue(path, out var item))
            {
                if (item.Image.SourceValid && (item.IdleNode == null || item.Expires > Environment.TickCount64))
                    existing = AcquireLocked(item);
                else
                {
                    RemoveLocked(item);
                    obsolete = item;
                    ScheduleLocked();
                }
            }
        }
        obsolete?.Image.Dispose();
        if (existing != null)
        {
            try
            {
                existing.Image.ValidateSourcePath();
                return existing;
            }
            catch (IOException)
            {
                existing.Dispose();
                // A replacement can already occupy the same path. Load its
                // own generation rather than reusing the invalid metadata.
            }
            catch { existing.Dispose(); throw; }
        }

        // Do not hold the cache lock while accessing the network or opening
        // and decoding image metadata.
        var image = new Archive(path, identity);
        Cached candidate = new(path, image);
        Cached? displaced = null;
        bool keep = false;
        Lease? result = null;
        try
        {
            lock (sync)
            {
                ObjectDisposedException.ThrowIf(disposed, this);
                if (images.TryGetValue(path, out var current) && current.Image.SourceValid &&
                    (current.IdleNode == null || current.Expires > Environment.TickCount64))
                    result = AcquireLocked(current);
                else
                {
                    image.SourceInvalidated += () => RemoveInvalid(candidate);
                    image.EnsureSourceValid();
                    if (images.TryGetValue(path, out displaced)) RemoveLocked(displaced);
                    candidate.References = 1;
                    images.Add(path, candidate);
                    ScheduleLocked();
                    keep = true;
                    result = new Lease(this, candidate);
                }
            }
            try
            {
                if (!keep) result.Image.ValidateSourcePath();
                return result;
            }
            catch { result.Dispose(); throw; }
        }
        finally
        {
            if (!keep) image.Dispose();
            displaced?.Image.Dispose();
        }
    }

    private void RemoveInvalid(Cached generation)
    {
        bool removed;
        lock (sync)
        {
            removed = Current(generation);
            if (removed)
            {
                RemoveLocked(generation);
                ScheduleLocked();
            }
        }
        if (removed) generation.Image.Dispose();
    }

    private void Release(Cached item)
    {
        List<Cached>? release = null;
        lock (sync)
        {
            if (disposed || !Current(item) || --item.References != 0) return;
            if (!item.Image.SourceValid || !item.Image.CanCacheIdle || idleMilliseconds == 0 || maxIdle == 0)
            {
                RemoveLocked(item);
                release = new List<Cached> { item };
            }
            else
            {
                item.Expires = Environment.TickCount64 + idleMilliseconds;
                item.IdleNode = idle.AddLast(item);
            }
            TrimLocked(ref release);
            ScheduleLocked();
        }
        DisposeItems(release);
    }

    private void TrimLocked(ref List<Cached>? release)
    {
        long now = Environment.TickCount64;
        while (idle.First is { } first &&
            (idle.Count > maxIdle || first.Value.Expires <= now))
        {
            var item = first.Value;
            RemoveLocked(item);
            (release ??= new List<Cached>()).Add(item);
        }
    }

    private void ScheduleLocked()
    {
        long next = idle.First?.Value.Expires ?? long.MaxValue;
        if (scheduled == next) return;
        scheduled = next;
        timer.Change(next == long.MaxValue ? Timeout.Infinite : (int)Math.Clamp(next - Environment.TickCount64, 1, int.MaxValue),
            Timeout.Infinite);
    }

    private void Expire()
    {
        Interlocked.Increment(ref expiryWakeups);
        List<Cached>? release = null;
        lock (sync)
        {
            if (disposed) return;
            scheduled = long.MaxValue;
            TrimLocked(ref release);
            ScheduleLocked();
        }
        DisposeItems(release);
    }

    private static void DisposeItems(IEnumerable<Cached>? release)
    {
        if (release != null)
            foreach (var item in release) item.Image.Dispose();
    }

    public void Dispose()
    {
        Cached[] remaining;
        lock (sync)
        {
            if (disposed) return;
            disposed = true;
            timer.Dispose();
            remaining = images.Values.ToArray();
            images.Clear();
            idle.Clear();
        }
        DisposeItems(remaining);
    }
}
