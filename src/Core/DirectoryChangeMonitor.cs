// SPDX-License-Identifier: GPL-3.0-only
namespace WinSquashfs.Core;

// One asynchronous directory notification request per watched directory and
// security context. Changes are dispatched only to interested source names.
internal sealed class DirectoryChangeMonitor : IDisposable
{
    private readonly object gate = new();
    private readonly Dictionary<string, DirectoryWatch> directories = new(StringComparer.OrdinalIgnoreCase);
    private bool disposed;

    private sealed class DirectoryWatch
    {
        internal readonly FileSystemWatcher Watcher;
        internal readonly Dictionary<string, HashSet<Subscription>> Subscribers = new(StringComparer.OrdinalIgnoreCase);
        internal DirectoryWatch(FileSystemWatcher watcher) => Watcher = watcher;
    }

    private sealed class Subscription : IDisposable
    {
        private DirectoryChangeMonitor? owner;
        internal readonly DirectoryWatch Directory;
        internal readonly string Name;
        internal readonly Action Changed;
        internal readonly Action MonitoringLost;
        internal readonly bool DirectoryNameOnly;
        internal Subscription(DirectoryChangeMonitor owner, DirectoryWatch directory, string name, Action changed, Action monitoringLost, bool directoryNameOnly)
        { this.owner = owner; Directory = directory; Name = name; Changed = changed; MonitoringLost = monitoringLost; DirectoryNameOnly = directoryNameOnly; }
        internal void Notify()
        {
            if (Volatile.Read(ref owner) != null) Changed();
        }
        internal void NotifyLost()
        {
            if (Volatile.Read(ref owner) != null) MonitoringLost();
        }
        public void Dispose() => Interlocked.Exchange(ref owner, null)?.Remove(this);
    }

    internal int Count { get { lock (gate) return directories.Count; } }

    internal IDisposable Subscribe(string directory, string name, Action changed, Action monitoringLost, bool directoryNameOnly)
    {
        directory = Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory));
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            if (!directories.TryGetValue(directory, out var entry))
            {
                var watcher = new FileSystemWatcher(directory)
                {
                    NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite |
                        NotifyFilters.Size | NotifyFilters.CreationTime
                };
                entry = new DirectoryWatch(watcher);
                var captured = entry;
                watcher.Created += (_, e) => Dispatch(captured, e.Name, false);
                watcher.Deleted += (_, e) => Dispatch(captured, e.Name, false);
                watcher.Changed += (_, e) => Dispatch(captured, e.Name, true);
                watcher.Renamed += (_, e) =>
                {
                    Dispatch(captured, e.OldName, false);
                    Dispatch(captured, e.Name, false);
                };
                watcher.Error += (_, _) => Fail(captured);
                try { watcher.EnableRaisingEvents = true; }
                catch { watcher.Dispose(); throw; }
                directories.Add(directory, entry);
            }

            var subscription = new Subscription(this, entry, name, changed, monitoringLost, directoryNameOnly);
            if (!entry.Subscribers.TryGetValue(name, out var subscribers))
                entry.Subscribers.Add(name, subscribers = new HashSet<Subscription>());
            subscribers.Add(subscription);
            return subscription;
        }
    }

    private void Dispatch(DirectoryWatch entry, string? name, bool contentOnly)
    {
        if (name == null) { Fail(entry); return; }
        Subscription[] subscribers;
        lock (gate)
        {
            if (disposed || !entry.Subscribers.TryGetValue(name, out var matching)) return;
            subscribers = matching.Where(s => !contentOnly || !s.DirectoryNameOnly).ToArray();
        }
        foreach (var subscriber in subscribers) subscriber.Notify();
    }

    private void Fail(DirectoryWatch entry)
    {
        Subscription[] subscribers;
        lock (gate)
        {
            if (disposed) return;
            subscribers = entry.Subscribers.Values.SelectMany(x => x).ToArray();
            entry.Subscribers.Clear();
            if (directories.TryGetValue(entry.Watcher.Path, out var current) && ReferenceEquals(current, entry))
                directories.Remove(entry.Watcher.Path);
        }
        // A missing notification capability does not mean the image is gone.
        // Revalidate once and fall back to checks on actual accesses.
        entry.Watcher.Dispose();
        foreach (var subscriber in subscribers) subscriber.NotifyLost();
    }

    private void Remove(Subscription subscription)
    {
        FileSystemWatcher? release = null;
        lock (gate)
        {
            var entry = subscription.Directory;
            if (entry.Subscribers.TryGetValue(subscription.Name, out var subscribers))
            {
                subscribers.Remove(subscription);
                if (subscribers.Count == 0) entry.Subscribers.Remove(subscription.Name);
            }
            if (entry.Subscribers.Count == 0 &&
                directories.TryGetValue(entry.Watcher.Path, out var current) && ReferenceEquals(current, entry))
            {
                directories.Remove(entry.Watcher.Path);
                release = entry.Watcher;
            }
        }
        release?.Dispose();
    }

    public void Dispose()
    {
        DirectoryWatch[] release;
        lock (gate)
        {
            if (disposed) return;
            disposed = true;
            release = directories.Values.ToArray();
            directories.Clear();
            foreach (var entry in release) entry.Subscribers.Clear();
        }
        foreach (var entry in release) entry.Watcher.Dispose();
    }
}
