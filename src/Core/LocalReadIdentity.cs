// SPDX-License-Identifier: GPL-3.0-only
namespace WinSquashfs.Core;

// Use the effective Windows security context of the hosting application.
// GUI and background processes keep the access token supplied at launch.
internal sealed class LocalReadIdentity : IDisposable
{
    internal DirectoryChangeMonitor Changes { get; } = new();
    internal T Run<T>(Func<T> action) => action();
    public void Dispose() => Changes.Dispose();
}
