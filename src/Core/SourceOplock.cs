// SPDX-License-Identifier: GPL-3.0-only
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace WinSquashfs.Core;

// FSCTL_REQUEST_OPLOCK uses an OVERLAPPED whose storage must remain alive
// until completion, including cancellation. The low bit of hEvent suppresses
// IOCP delivery because this request is managed independently of RandomAccess.
internal sealed class SourceOplock : IDisposable
{
    private const uint RequestOplock = 0x00090240;
    private const int ErrorIoPending = 997;
    private readonly SafeFileHandle handle;
    private readonly EventWaitHandle signal = new(false, EventResetMode.ManualReset);
    private readonly IntPtr input = Marshal.AllocHGlobal(12);
    private readonly IntPtr output = Marshal.AllocHGlobal(24);
    private readonly IntPtr overlapped = Marshal.AllocHGlobal(Marshal.SizeOf<Overlapped>());
    private RegisteredWaitHandle? wait;
    private bool pending;
    private int disposed;

    [StructLayout(LayoutKind.Sequential)]
    private struct Overlapped
    {
        internal IntPtr Internal, InternalHigh;
        internal uint Offset, OffsetHigh;
        internal IntPtr Event;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle file, uint code, IntPtr input, uint inputSize,
        IntPtr output, uint outputSize, out uint returned, IntPtr overlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CancelIoEx(SafeFileHandle file, IntPtr overlapped);

    private SourceOplock(SafeFileHandle handle) { this.handle = handle; }

    internal static SourceOplock? TryRequest(SafeFileHandle handle)
    {
        var request = new SourceOplock(handle);
        try
        {
            Marshal.WriteInt16(request.input, 0, 1);
            Marshal.WriteInt16(request.input, 2, 12);
            Marshal.WriteInt32(request.input, 4, 3); // OPLOCK_LEVEL_CACHE_READ | OPLOCK_LEVEL_CACHE_HANDLE
            Marshal.WriteInt32(request.input, 8, 1); // REQUEST_OPLOCK_INPUT_FLAG_REQUEST
            Marshal.StructureToPtr(new Overlapped
            {
                Event = new IntPtr(request.signal.SafeWaitHandle.DangerousGetHandle().ToInt64() | 1)
            }, request.overlapped, false);
            bool completed = DeviceIoControl(handle, RequestOplock, request.input, 12, request.output, 24,
                out _, request.overlapped);
            request.pending = !completed && Marshal.GetLastWin32Error() == ErrorIoPending;
            if (request.pending) return request;
            request.Dispose();
            return null;
        }
        catch { request.Dispose(); throw; }
    }

    internal bool Broken => Volatile.Read(ref disposed) != 0 || signal.WaitOne(0);

    internal void Monitor(Action onBreak) =>
        wait = ThreadPool.RegisterWaitForSingleObject(signal, (_, timedOut) =>
        {
            if (!timedOut && Volatile.Read(ref disposed) == 0) onBreak();
        }, null, Timeout.Infinite, true);

    public void Dispose()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        wait?.Unregister(null);
        if (pending)
        {
            // Cancellation is asynchronous. Do not release the OVERLAPPED or
            // buffers before the kernel has finished the original request.
            if (!signal.WaitOne(0)) CancelIoEx(handle, overlapped);
            signal.WaitOne();
        }
        Marshal.FreeHGlobal(overlapped);
        Marshal.FreeHGlobal(input);
        Marshal.FreeHGlobal(output);
        signal.Dispose();
    }
}
