// SPDX-License-Identifier: GPL-3.0-only
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace WinSquashfs.Core;

public enum ImageFormat { SquashFS, DwarFS, EROFS }

internal sealed class ExtraArchive : IDisposable
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate int Visit(ulong inode, IntPtr name, nuint length, uint mode, ulong size, ulong time);

    private readonly ImageFormat format;
    private readonly SafeFileHandle? source;
    private readonly Native.SourceRead reader;
    private IntPtr handle;
    internal ulong Root { get; }
    internal uint BlockSize { get; }
    internal ulong Modified { get; }
    internal ulong Size { get; }
    internal ExtraArchive(string path, ImageFormat format, VolatileImageSource? volatileSource)
    {
        this.format = format;
        source = volatileSource == null ? File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.Read, FileOptions.RandomAccess) : null;
        try
        {
            Size = volatileSource?.Size ?? (ulong)RandomAccess.GetLength(source!);
            reader = volatileSource?.Reader ?? ReadSource;
            ulong root, time;
            uint block;
            int result = format == ImageFormat.DwarFS
                ? Dwarfs.wa_open(Size, reader, out handle, out root, out block, out time)
                : Erofs.wa_open(Size, reader, out handle, out root, out block, out time);
            if (result != 0 || handle == IntPtr.Zero)
                throw new IOException(Localization.T("Cannot open {0} image (error {1}).", format, result));
            Root = root; BlockSize = block; Modified = time;
        }
        catch { Dispose(); throw; }
    }
    private unsafe int ReadSource(ulong offset, IntPtr buffer, nuint length)
    {
        try
        {
            var remaining = new Span<byte>((void*)buffer, checked((int)length));
            while (!remaining.IsEmpty)
            {
                int n = RandomAccess.Read(source!, remaining, checked((long)offset));
                if (n == 0) return -1;
                offset += (uint)n;
                remaining = remaining[n..];
            }
            return 0;
        }
        catch { return -1; }
    }
    internal void List(ulong inode, Visit visit)
    {
        int result = format == ImageFormat.DwarFS ? Dwarfs.wa_list(handle,inode,visit) : Erofs.wa_list(handle,inode,visit);
        if(result != 0) throw new IOException(Localization.T("Cannot read {0} directory (error {1}).", format, result));
    }
    internal int Read(ulong inode, ulong offset, IntPtr buffer, uint length) =>
        format == ImageFormat.DwarFS ? Dwarfs.wa_read(handle,inode,offset,buffer,length) : Erofs.wa_read(handle,inode,offset,buffer,length);
    public void Dispose()
    {
        if(handle != IntPtr.Zero)
        {
            if(format == ImageFormat.DwarFS) Dwarfs.wa_close(handle);
            else Erofs.wa_close(handle);
            handle = IntPtr.Zero;
        }
        source?.Dispose();
        GC.KeepAlive(reader);
    }

    internal static unsafe ImageFormat Detect(string path, VolatileImageSource? source)
    {
        Span<byte> header = stackalloc byte[1088];
        header.Clear();
        int length;
        if (source == null)
        {
            using var f = File.OpenHandle(path,FileMode.Open,FileAccess.Read,FileShare.Read,FileOptions.RandomAccess);
            length = RandomAccess.Read(f,header,0);
        }
        else
        {
            length = (int)Math.Min((ulong)header.Length,source.Size);
            fixed(byte* ptr = header)
                if (source.Reader(0,(IntPtr)ptr,(nuint)length) != 0) throw new IOException(path);
        }
        if(length >= 4 && header[..4].SequenceEqual("hsqs"u8)) return ImageFormat.SquashFS;
        if(length >= 6 && header[..6].SequenceEqual("DWARFS"u8)) return ImageFormat.DwarFS;
        if(length >= 1028 && System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(header[1024..]) == 0xE0F5E1E2u)
            return ImageFormat.EROFS;
        throw new IOException(Localization.T("Unsupported or invalid read-only image."));
    }
    private static class Dwarfs
    {
        private const string Dll = "winsqfs-dwarfs";
        [DllImport(Dll, CallingConvention=CallingConvention.Cdecl)] internal static extern int wa_open(ulong size, Native.SourceRead read, out IntPtr handle, out ulong root, out uint block, out ulong time);
        [DllImport(Dll, CallingConvention=CallingConvention.Cdecl)] internal static extern void wa_close(IntPtr handle);
        [DllImport(Dll, CallingConvention=CallingConvention.Cdecl)] internal static extern int wa_list(IntPtr handle, ulong inode, Visit visit);
        [DllImport(Dll, CallingConvention=CallingConvention.Cdecl)] internal static extern int wa_read(IntPtr handle, ulong inode, ulong offset, IntPtr buffer, uint length);
    }
    private static class Erofs
    {
        private const string Dll = "winsqfs-erofs";
        [DllImport(Dll, CallingConvention=CallingConvention.Cdecl)] internal static extern int wa_open(ulong size, Native.SourceRead read, out IntPtr handle, out ulong root, out uint block, out ulong time);
        [DllImport(Dll, CallingConvention=CallingConvention.Cdecl)] internal static extern void wa_close(IntPtr handle);
        [DllImport(Dll, CallingConvention=CallingConvention.Cdecl)] internal static extern int wa_list(IntPtr handle, ulong inode, Visit visit);
        [DllImport(Dll, CallingConvention=CallingConvention.Cdecl)] internal static extern int wa_read(IntPtr handle, ulong inode, ulong offset, IntPtr buffer, uint length);
    }
}
