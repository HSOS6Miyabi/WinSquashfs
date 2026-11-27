// SPDX-License-Identifier: GPL-3.0-only
namespace WinSquashfs.Core;

internal sealed class ImageCache : IDisposable
{
    internal sealed class Cached
    {
        internal readonly Archive Image;
        internal int References;
        internal long Released;
        internal Cached(Archive image) { Image = image; }
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
    private readonly long idleMilliseconds;
    private readonly int maxIdle;
    private readonly Timer timer;
    private bool disposed;

    internal ImageCache(int idleSeconds, int maxIdleImages)
    {
        if (idleSeconds is < 0 or > 86400) throw new ArgumentOutOfRangeException(nameof(idleSeconds));
        if (maxIdleImages is < 0 or > 1024) throw new ArgumentOutOfRangeException(nameof(maxIdleImages));
        idleMilliseconds = idleSeconds * 1000L;
        maxIdle = maxIdleImages;
        timer = new Timer(_ => Sweep(), null, 500, 500);
    }

    internal int Count { get { lock (sync) return images.Count; } }
    internal int ActiveCount { get { lock (sync) return images.Values.Count(x => x.References > 0); } }
    internal int OplockCount { get { lock (sync) return images.Values.Count(x => x.Image.UsesOplock); } }

    internal Lease Acquire(string path, LocalReadIdentity identity)
    {
        path = Path.GetFullPath(path);
        Sweep();
        lock (sync)
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            if (images.TryGetValue(path, out var existing) && existing.Image.SourceValid)
            {
                checked { existing.References++; }
                return new Lease(this, existing);
            }
        }

        // Opening a remote source can block on the network. Do not hold the
        // cache lock while loading its metadata or closing source handles.
        var image = new Archive(path, identity);
        Cached candidate = new(image);
        Cached? displaced = null;
        bool keep = false;
        try
        {
            lock (sync)
            {
                ObjectDisposedException.ThrowIf(disposed, this);
                if (images.TryGetValue(path, out var existing) && existing.Image.SourceValid)
                {
                    checked { existing.References++; }
                    return new Lease(this, existing);
                }
                image.SourceInvalidated += () => RemoveInvalid(path, candidate);
                image.EnsureSourceValid();
                if (images.TryGetValue(path, out displaced)) images.Remove(path);
                candidate.References = 1;
                images.Add(path, candidate);
                keep = true;
                return new Lease(this, candidate);
            }
        }
        finally
        {
            if (!keep) image.Dispose();
            displaced?.Image.Dispose();
        }
    }
    private void RemoveInvalid(string path, Cached generation)
    {
        bool removed;
        lock (sync)
        {
            removed = images.TryGetValue(path, out var current) && ReferenceEquals(current, generation);
            if (removed) images.Remove(path);
        }
        if (removed) generation.Image.Dispose();
    }
    private void Release(Cached cached)
    {
        lock (sync)
        {
            if (disposed) return;
            if (--cached.References == 0) cached.Released = Environment.TickCount64;
        }
        Sweep();
    }
    private void Sweep()
    {
        var remove = new List<Cached>();
        lock (sync)
        {
            if (disposed) return;
            foreach (var pair in images.Where(p => !p.Value.Image.SourceValid).ToArray())
            {
                images.Remove(pair.Key);
                remove.Add(pair.Value);
            }
            long now = Environment.TickCount64;
            var idle = images.Where(p => p.Value.References == 0).OrderBy(p => p.Value.Released).ToArray();
            int remaining = idle.Length;
            foreach (var pair in idle)
            {
                if (now - pair.Value.Released < idleMilliseconds && remaining <= maxIdle) break;
                images.Remove(pair.Key);
                remove.Add(pair.Value);
                remaining--;
            }
        }
        foreach (var item in remove) item.Image.Dispose();
    }
    public void Dispose()
    {
        timer.Dispose();
        Cached[] remaining;
        lock (sync)
        {
            if (disposed) return;
            disposed = true;
            remaining = images.Values.ToArray();
            images.Clear();
        }
        foreach (var item in remaining) item.Image.Dispose();
    }
}
