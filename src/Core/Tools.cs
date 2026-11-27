// SPDX-License-Identifier: GPL-3.0-only
using System.Diagnostics;

namespace WinSquashfs.Core;

public sealed record PackOptions(string Codec = "zstd", int BlockSize = 131072, int Threads = 0,
    int? Level = null, bool NoFragments = false, bool NoDuplicates = false, bool NoXattrs = false,
    bool AllRoot = false, bool NoExports = false, bool NoSparse = false, bool NoCompression = false,
    bool AlwaysFragments = false, bool NoInodeCompression = false, bool NoFragmentCompression = false,
    int? MemoryMiB = null, uint? Timestamp = null, string? ExcludeFile = null, bool Lz4HighCompression = false, bool KeepDirectory = false,
    string? XzDictionary = null, string? XzFilters = null, int? GzipWindow = null, string? LzoAlgorithm = null)
{
    public IReadOnlyList<string> Arguments()
    {
        if (Codec is not ("gzip" or "lzo" or "xz" or "lz4" or "zstd")) throw new ArgumentException("Unknown compressor.");
        if (BlockSize < 4096 || BlockSize > 1048576 || (BlockSize & (BlockSize - 1)) != 0) throw new ArgumentException("Block size must be a power of two between 4 KiB and 1 MiB.");
        if (Threads < 0 || Threads > 1024) throw new ArgumentException("Invalid processor count.");
        if (NoFragments && AlwaysFragments) throw new ArgumentException("Fragment options conflict.");
        if (Level.HasValue && !(Codec == "gzip" && Level is >= 1 and <= 9 ||
                              Codec == "zstd" && (Level is >= 1 and <= 22 || Level is >= -131072 and <= -1) ||
                              Codec == "lzo" && Level is >= 1 and <= 9))
            throw new ArgumentException("Compression level is not supported by this compressor.");
        var a = new List<string> { "-noappend", "-comp", Codec, "-b", BlockSize.ToString(), "-no-progress" };
        if (Threads > 0) a.AddRange(new[] { "-processors", Threads.ToString() });
        if (Level.HasValue) a.AddRange(new[] { "-Xcompression-level", Level.Value.ToString() });
        if (NoFragments) a.Add("-no-fragments");
        if (AlwaysFragments) a.Add("-tailends");
        if (NoDuplicates) a.Add("-no-duplicates");
        if (NoXattrs) a.Add("-no-xattrs");
        if (AllRoot) a.Add("-all-root");
        if (NoExports) a.Add("-no-exports");
        if (NoSparse) a.Add("-no-sparse");
        if (NoCompression) a.Add("-noD");
        if (NoInodeCompression) a.Add("-noI");
        if (NoFragmentCompression) a.Add("-noF");
        if (MemoryMiB.HasValue)
        {
            if (MemoryMiB < 64) throw new ArgumentException("Memory limit must be at least 64 MiB.");
            a.AddRange(new[] { "-mem", MemoryMiB.Value + "M" });
        }
        if (Timestamp.HasValue) a.AddRange(new[] { "-inode-time", Timestamp.Value.ToString(), "-mkfs-time", Timestamp.Value.ToString() });
        if (Lz4HighCompression) { if (Codec != "lz4") throw new ArgumentException("LZ4 option requires LZ4."); a.Add("-Xhc"); }
        if (KeepDirectory) a.Add("-keep-as-directory");
        if (!string.IsNullOrWhiteSpace(XzDictionary))
        {
            if (Codec != "xz") throw new ArgumentException("XZ dictionary requires XZ.");
            a.AddRange(new[] { "-Xdict-size", XzDictionary });
        }
        if (!string.IsNullOrWhiteSpace(XzFilters))
        {
            if (Codec != "xz" || XzFilters.Split(',').Any(x => x is not ("x86" or "arm" or "armthumb" or "powerpc" or "sparc" or "ia64")))
                throw new ArgumentException("Invalid XZ filters.");
            a.AddRange(new[] { "-Xbcj", XzFilters });
        }
        if (GzipWindow.HasValue)
        {
            if (Codec != "gzip" || GzipWindow is < 8 or > 15) throw new ArgumentException("GZIP window must be 8–15.");
            a.AddRange(new[] { "-Xwindow-size", GzipWindow.Value.ToString() });
        }
        if (!string.IsNullOrWhiteSpace(LzoAlgorithm))
        {
            if (Codec != "lzo" || LzoAlgorithm is not ("lzo1x_1" or "lzo1x_1_11" or "lzo1x_1_12" or "lzo1x_1_15" or "lzo1x_999"))
                throw new ArgumentException("Invalid LZO algorithm.");
            if (Level.HasValue && LzoAlgorithm != "lzo1x_999") throw new ArgumentException("Compression level requires lzo1x_999.");
            a.AddRange(new[] { "-Xalgorithm", LzoAlgorithm });
        }
        if (!string.IsNullOrWhiteSpace(ExcludeFile)) a.AddRange(new[] { "-ef", SquashTools.ToolPath(ExcludeFile) });
        return a;
    }
}

public static class SquashTools
{
    public static string ToolPath(string path) => Path.GetFullPath(path).Replace('\\','/');
    public static async Task RunAsync(string tool, IEnumerable<string> args, Action<string>? log, CancellationToken token = default)
    {
        if (tool is not ("mksquashfs" or "unsquashfs" or "sqfscat")) throw new ArgumentException("Unknown tool.");
        var exe = Path.Combine(AppContext.BaseDirectory, "tools", tool + ".exe");
        var si = new ProcessStartInfo(exe) { UseShellExecute = false, RedirectStandardError = true, RedirectStandardOutput = true, CreateNoWindow = true };
        foreach (var arg in args) si.ArgumentList.Add(arg);
        using var p = new Process { StartInfo = si };
        p.OutputDataReceived += (_, e) => { if (e.Data != null) log?.Invoke(e.Data); };
        p.ErrorDataReceived += (_, e) => { if (e.Data != null) log?.Invoke(e.Data); };
        token.ThrowIfCancellationRequested();
        if (!p.Start()) throw new IOException("Cannot start " + tool);
        p.BeginOutputReadLine(); p.BeginErrorReadLine();
        using var registration = token.Register(() => { try { if (!p.HasExited) p.Kill(true); } catch (InvalidOperationException) { } catch (System.ComponentModel.Win32Exception) { } });
        await p.WaitForExitAsync();
        p.WaitForExit();
        token.ThrowIfCancellationRequested();
        if (p.ExitCode != 0) throw new IOException($"{tool} exited with code {p.ExitCode}.");
    }

    public static async Task PackAsync(string source, string output, PackOptions options, Action<string>? log, CancellationToken token = default)
    {
        source = Path.GetFullPath(source); output = Path.GetFullPath(output);
        if (!Directory.Exists(source)) throw new DirectoryNotFoundException(source);
        if (File.Exists(output) || Directory.Exists(output)) throw new IOException("Output already exists.");
        string relative = Path.GetRelativePath(source, output);
        if (!Path.IsPathRooted(relative) && relative != ".." && !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal))
            throw new IOException("Output must be outside the source directory.");
        string temporary = Path.Combine(Path.GetDirectoryName(output)!, ".winsqfs-" + Guid.NewGuid().ToString("N") + ".partial");
        try
        {
            var args = new[] { ToolPath(source), ToolPath(temporary) }.Concat(options.Arguments());
            await RunAsync("mksquashfs", args, log, token);
            token.ThrowIfCancellationRequested();
            File.Move(temporary, output);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    public static async Task UnpackAsync(string image, string destination, Action<string>? log, CancellationToken token = default)
    {
        // Windows-safe extraction of regular files; no traversal through archive links or special files.
        destination = Path.GetFullPath(destination);
        if (File.Exists(destination) || Directory.Exists(destination)) throw new IOException("Choose a new destination directory.");
        using var archive = await Task.Run(() => new Archive(image), token);
        await Task.Run(() => archive.ExportEntry(archive.Root, destination, token, log), token);
        log?.Invoke("Extracted " + destination);
    }
}
