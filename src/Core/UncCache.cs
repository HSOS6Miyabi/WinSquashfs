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
        timer = new Timer(_ => { lock (sync) { if (!disposed) Sweep(); } }, null, 1000, 1000);
    }

    internal int Count { get { lock (sync) return images.Count; } }
    internal int ActiveCount { get { lock (sync) return images.Values.Count(x => x.References > 0); } }

    internal Lease Acquire(string path)
    {
        path = Path.GetFullPath(path);
        lock (sync)
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            Sweep();
            if (!images.TryGetValue(path, out var cached))
            {
                cached = new Cached(new Archive(path));
                images.Add(path, cached);
            }
            checked { cached.References++; }
            return new Lease(this, cached);
        }
    }
    private void Release(Cached cached)
    {
        lock (sync)
        {
            if (disposed) return;
            if (--cached.References == 0) cached.Released = Environment.TickCount64;
            Sweep();
        }
    }
    private void Sweep()
    {
        long now = Environment.TickCount64;
        var idle = images.Where(p => p.Value.References == 0).OrderBy(p => p.Value.Released).ToArray();
        int remaining = idle.Length;
        foreach (var pair in idle)
        {
            if (now - pair.Value.Released < idleMilliseconds && remaining <= maxIdle) break;
            images.Remove(pair.Key);
            pair.Value.Image.Dispose();
            remaining--;
        }
    }
    public void Dispose()
    {
        timer.Dispose();
        lock (sync)
        {
            if (disposed) return;
            disposed = true;
            foreach (var item in images.Values) item.Image.Dispose();
            images.Clear();
        }
    }
}
