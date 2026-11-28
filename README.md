# WinArchiveFSPro

[简体中文](README.zh-CN.md)

A native Windows application for browsing, creating, extracting, verifying and mounting SquashFS, DwarFS and EROFS images.

## Features

- WinUI 3 interface in English and Simplified Chinese, previews, recursive file search, recent images, appearance settings and drag and drop.
- Multi-file and directory selection, eight compression presets, format-specific advanced settings, and batch creation and extraction.
- Native WinFsp read-only drive mounts and UNC access, with parallel random reads and no temporary extracted file tree.
- Shared image caching, configurable per-image and aggregate decoded-data cache capacities and idle expiration.
- Explorer Open/Edit, multi-selection archive creation and eject commands.

| Format | Reader | Creator |
| --- | --- | --- |
| SquashFS | libsquashfs, parallel read contexts | mksquashfs (x64), tar2sqfs (x86/ARM64) |
| DwarFS | official C++ DwarFS 0.15.10 | mkdwarfs |
| EROFS | liberofs, single-device raw images | mkfs.erofs |

Regular files and directories are projected into Windows. Symbolic links and special files are displayed as metadata. Incompatible Windows names are encoded deterministically. Extreme compression uses format-specific high-ratio settings.

### Additional archive formats

ZIP (including ZIP64), 7z, RAR, TAR, CPIO, CAB, ISO9660, AR and LHA are available through the same read-only browser, CLI, drive mounts and UNC namespace. TAR supports gzip, bzip2, xz, Zstandard, LZ4, LZMA, lzip and Unix compress filters; standalone compressed streams appear as a single file. Ordinary TAR entries use indexed physical offsets, including sparse extent maps. GZIP-compressed TAR uses an in-memory index of bounded decompressor snapshots to restart near the requested uncompressed offset, including concatenated GZIP members. The snapshot reservation shares the configured image cache budget with decoded pages. Other compressed TAR and solid archives reuse decoder cursors and decoded-page caches; a backward cache miss can require replaying a stream. No extracted files or intermediate TAR files are written for reads.

Archive creation remains available for SquashFS, DwarFS and EROFS. Encrypted archives requiring passwords, split/multi-volume archives and UDF-only discs are not supported. The installer offers archive-specific Explorer commands without changing existing default archive applications.

## Installation

Windows 10 1809 or newer, with native x64, x86 and ARM64 packages. Choose the installer for the operating system's architecture. Mounting and UNC access require WinFsp 2.2.26323 or a compatible newer 2.x version. The installer installs or upgrades WinFsp; the portable distribution includes its MSI in prerequisites. No .NET runtime is required.

[Releases](https://github.com/HSOS6Miyabi/WinSquashfs/releases) · [Builds](https://github.com/HSOS6Miyabi/WinSquashfs/actions)

## UNC access

Start the independent UNC agent in the application. It stays running when the GUI closes, can start at login, and can be managed from its notification area icon.

    \\sfs$\C\images\linux.squashfs\etc\os-release
    \\sfs.localhost\C\images\linux.squashfs
    \\sfs$\path\archive.dwarfs
    \\sfs$\UNC\NAS\backup\archive.erofs

Both server names expose enabled drive mappings, the configured directory aliases, and network shares. New local or mapped drive letters are detected automatically. Double-click a remote image or run `archivefs unc-alias IMAGE` to register an exact host/share alias such as `\\sfs.NAS.localhost\backup`. Registered aliases are restored at agent startup; `\\sfs$\UNC` remains the direct entry point for a new host or share. Explicit image paths are recognized by content. Source reads use the hosting process's Windows credentials. Transient local sources use opportunistic locks where available, with short-lived reads and identity checks otherwise. Changed sources invalidate their cached generations. Explicit drive mounts protect the source until ejected.

## Command line and development

Run archivefs --help for commands. Use --lang zh-CN for Chinese output. See [native build instructions](native/cpp/README.md) for CMake, MSVC, vcpkg and Windows integration tests.

GPL-3.0-only. See [THIRD-PARTY.md](THIRD-PARTY.md).


### Portable devices and Android files

Start the UNC agent to browse `\\sfs$\mtp` (MTP/PTP), `\\sfs$\adb` (existing ADB connections), and `\\sfs$\adb.r` (Android files with authorized su access). Manage these read-only namespaces and the local ADB port in the Devices page. The agent never starts or restarts an ADB server. ADB uses UTF-8 filenames and 64-bit sizes and offsets; Root access requires authorization on the Android device.

Slow archive opens display a native Windows progress notification based on the furthest successfully read offset relative to the image size. Nested SquashFS, DwarFS and EROFS images are opened directly from their parent archive readers without extracting them to disk.

Mounted archives resolve symbolic links within the archive, including absolute POSIX targets. Unresolved links and special files are visible as unavailable filesystem objects. Use `archivefs readlink IMAGE ENTRY` to inspect a link target.
