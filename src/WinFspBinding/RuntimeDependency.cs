// SPDX-License-Identifier: GPL-3.0-only
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace Fsp.Interop
{
    public sealed record RuntimeInfo(string DllPath, Version Version);

    // Resolve the native library from the registered WinFsp installation.
    public static class RuntimeDependency
    {
        private const uint LoadLibrarySearchDllLoadDir = 0x00000100;
        private const uint LoadLibrarySearchSystem32 = 0x00000800;
        private static readonly object Sync = new object();
        private static IntPtr module;
        private static RuntimeInfo loaded;

        public static Version MinimumVersion { get; } = ReadMinimumVersion();

        private static Version ReadMinimumVersion()
        {
            foreach (var attribute in typeof(RuntimeDependency).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>())
                if (attribute.Key == "WinFspMinimumVersion")
                    return Version.Parse(attribute.Value);
            throw new InvalidOperationException("WinFsp minimum runtime version is not configured.");
        }

        private static RuntimeInfo Inspect()
        {
            string name = RuntimeInformation.ProcessArchitecture switch
            {
                Architecture.X64 => "winfsp-x64.dll",
                Architecture.Arm64 => "winfsp-a64.dll",
                Architecture.X86 => "winfsp-x86.dll",
                _ => throw new PlatformNotSupportedException("Unsupported WinFsp process architecture.")
            };

            using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32);
            using var key = machine.OpenSubKey(@"SOFTWARE\WinFsp", false);
            var directory = key?.GetValue("InstallDir") as string;
            if (string.IsNullOrWhiteSpace(directory) || !Path.IsPathFullyQualified(directory))
                throw new DllNotFoundException("WinFsp is not registered in HKLM\\SOFTWARE\\WinFsp (32-bit registry view).");

            string path = Path.GetFullPath(Path.Combine(directory, "bin", name));
            if (!File.Exists(path))
                throw new DllNotFoundException("WinFsp native library is missing: " + path);

            var info = FileVersionInfo.GetVersionInfo(path);
            var version = new Version(info.FileMajorPart, info.FileMinorPart, info.FileBuildPart, info.FilePrivatePart);
            if (version.Major != MinimumVersion.Major || version < MinimumVersion)
                throw new TypeLoadException($"Incompatible WinFsp runtime {version}; required {MinimumVersion} or later in the same major version. DLL: {path}");

            return new RuntimeInfo(path, version);
        }

        [DllImport("kernel32.dll", EntryPoint = "LoadLibraryExW", CharSet = CharSet.Unicode,
            ExactSpelling = true, SetLastError = true)]
        private static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);

        internal static IntPtr Load()
        {
            lock (Sync)
            {
                if (module != IntPtr.Zero) return module;
                RuntimeInfo info = Inspect();
                IntPtr candidate = LoadLibraryEx(info.DllPath, IntPtr.Zero,
                    LoadLibrarySearchDllLoadDir | LoadLibrarySearchSystem32);
                if (candidate == IntPtr.Zero)
                {
                    int error = Marshal.GetLastWin32Error();
                    throw new DllNotFoundException($"Cannot load {info.DllPath}: {new Win32Exception(error).Message} (Win32 {error}).");
                }

                loaded = info;
                module = candidate;
                return module;
            }
        }

        public static RuntimeInfo Initialize()
        {
            Load();
            RuntimeHelpers.RunClassConstructor(typeof(Api).TypeHandle);
            return loaded;
        }
    }
}
