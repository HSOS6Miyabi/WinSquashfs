# WinSquashfs

[简体中文](README.zh-CN.md) | English

A WinUI 3 desktop application for SquashFS images on Windows x64. Browse image contents, preview files, build new images, extract files or mount a read-only drive using WinFsp.

## Features

- In-process archive browser with directory navigation, filename filtering, file metadata, text and hexadecimal previews.
- Direct UNC access without assigning drive letters: configured directory aliases and local drives, image caching and login startup.
- Recent images, drag-and-drop opening, image readability verification, and selected file or directory extraction.
- WinFsp read-only drive mounting with automatic or explicit drive-letter selection and multiple concurrent mounts.
- Direct random reads from compressed images through libsquashfs. Data and fragment blocks are decompressed in memory. No temporary extraction directory is created by browsing or mounting.
- SquashFS 4.0 reading: gzip, XZ, LZO, LZ4, Zstandard and legacy LZMA where supported by the bundled decoder.
- Linux squashfs-tools Windows ports: mksquashfs, unsquashfs, sqfscat and sqfstar. Image creation uses mksquashfs; Windows-safe extraction uses the archive reader.
- Advanced image creation: compressor, compression level, block size, worker count, memory limit, timestamps, exclusions, fragments, duplicate detection, sparse detection, ownership, export table, and compressor-specific parameters.
- NSIS installation, file associations for .squashfs, .sqfs and .sfs, and Explorer commands to browse, mount, extract or create an image.

## Installation

Download the x64 installer from [Releases](https://github.com/HSOS6Miyabi/WinSquashfs/releases) or the Windows build artifact from [Actions](https://github.com/HSOS6Miyabi/WinSquashfs/actions). The installer includes the WinFsp runtime and a self-contained Windows App SDK/.NET application. Windows 10 version 1809 or newer is required. Mounting and UNC access require WinFsp 2.2.26323 (WinFsp 2026) or a later compatible 2.x version. The installer installs or upgrades the runtime when necessary. The portable distribution includes prerequisites/WinFsp.msi for offline installation. Use `winsqfs doctor` or **Tools & about → Check WinFsp runtime** to inspect the installed native runtime.

## Language

WinSquashfs supports English and Simplified Chinese. The interface follows the Windows display language by default. Select a language in **Tools & about → Interface language**, save it, and restart the application. The UNC background process is independent of the main window.

The installer supports English and Simplified Chinese and registers Explorer menu labels in the selected installer language. The CLI uses English by default to preserve script compatibility; use `winsqfs --lang zh-CN` or `winsqfs --lang system` to change the language for a command. Output from the upstream squashfs-tools binaries is preserved as originally emitted.

## Usage

Open an image from the browser or Explorer. Select files for previews or extraction, double-click a folder to navigate, and choose Mount image to expose it as a read-only drive. Mounted images remain active while the application instance is open. Unmount them from the Mounted images page or use **Eject (WinSquashfs)** in the drive's Explorer context menu (Show more options on Windows 11). The installed shell command is scoped to SquashFS volumes. `winsqfs eject X:` also works with portable installations.

Create image accepts a source directory and a new output path. Zstandard is the default. Advanced parameters are optional. Files with Windows-incompatible names receive deterministic Windows-safe names; the browser preserves the original names for display. Linux symbolic links, sockets and devices are listed as metadata; they are omitted from the mounted filesystem and Windows-safe extraction. The original unsquashfs executable is available for Linux-oriented extraction behavior.

## File associations

Double-click an associated image to open its UNC directory directly in Explorer. The **Edit with WinSquashfs** verb opens the graphical image browser. Mounting a drive letter remains available in the GUI and Explorer context menu.

## Direct UNC access

Open **UNC access** in WinSquashfs and start the background application. The default configuration exposes the current user's profile as **path** and available local drives by their letter. For example:

    C:\images\linux.squashfs
    \\sfs$\C\images\linux.squashfs
    \\sfs$\C\images\linux.squashfs\usr\bin\example

To use a custom root, set the local directory to C:\images and the share name to path, then open \\sfs$\path\linux.squashfs. The sfs$ server component is a local virtual UNC namespace chosen to reduce conflicts with DNS hostnames. The UNC paths are normal Windows paths; Explorer and other applications can read them directly. A single network filesystem per share dynamically opens images as directories. No drive letter is reserved. Ordinary host files are not exposed. The configured directory must be local and may not pass through a reparse point; reparse points encountered below it are skipped. Access is restricted to the Windows account running the background application. Run it in the ordinary interactive user session.

Network source images are accessed through the built-in UNC share. For example, \\nas\backup\rootfs.squashfs becomes \\sfs$\UNC\nas\backup\rootfs.squashfs. The background process uses the current Windows user's network credentials; use an already accessible share.

For UNC access and the image browser, local files use a read-handle opportunistic lock when supported. A conflicting operation breaks the lock and releases the source handle. SMB and filesystems without this lock use short-lived read handles and verify file identity and modification information. Asynchronous directory notifications invalidate removed, renamed, replaced or changed images, including active cached entries. Other images and the UNC shares remain available. Directory watches are shared across images in the same directory, including ancestor name changes. There is no periodic source polling. If notifications are unavailable, identity is checked on access. An idle cache schedules a single wakeup at the earliest expiration. An unavailable or unsupported file identity prevents unsafe reuse. Explicit drive-letter mounts continue to protect their source images while mounted.

The background application remains available after the GUI closes. Start-at-login is optional. Idle images are closed after the configured time (default 60 seconds); the maximum number of idle images defaults to 8. Active handles retain their image. Changing settings restarts the background application if it is running; close open UNC files before stopping or applying settings. Restart UNC access to discover newly attached drives. Only one process can own a particular sfs$ share name at a time.

The image browser can copy the current image's UNC path or open it in Explorer. Explorer's image context menu also offers Open via SquashFS UNC path. Windows 11 displays traditional shell verbs in Show more options.

## Command line

    winsqfs --version
    winsqfs doctor
    winsqfs info image.squashfs
    winsqfs list image.squashfs
    winsqfs list image.squashfs /directory
    winsqfs read image.squashfs /file.bin 65536 4096
    winsqfs mount image.squashfs X:
    winsqfs eject X:
    winsqfs verify image.squashfs
    winsqfs serve path C:\images 60
    winsqfs unc-start
    winsqfs unc-status
    winsqfs unc-path C:\images\linux.squashfs
    winsqfs unc-stop
    winsqfs pack C:\source C:\output.squashfs zstd
    winsqfs unpack image.squashfs C:\new-directory

The read command writes hexadecimal bytes. The verify command reads each distinct regular-file inode without extracting data. Ctrl+C stops a command-line mount or foreground UNC share. Native tools are located in the tools subdirectory.

## Build

On Windows x64, install .NET 8, Visual Studio with Windows SDK and desktop development components, MSYS2 MINGW64 with mingw-w64-x86_64-gcc, squashfs-tools, curl and unzip, and NSIS. From the repository root:

    bash scripts/build-native.sh
    pwsh scripts/install-winfsp.ps1
    pwsh scripts/publish.ps1

The distribution is written to artifacts/dist. The project version is defined in Directory.Build.props. Build the installer with `makensis /DPRODUCT_VERSION=<version> /DWINFSP_MIN_VERSION=<runtime-version>.0 installer/WinSquashfs.nsi`, using the versions from Directory.Build.props. After the Windows workflow completes all tests, the release job publishes a new version as a GitHub Release, including an installer, portable ZIP and SHA-256 checksums. Existing published versions are retained. Version tags must match Directory.Build.props.

## Architecture

A C adapter opens SquashFS images through libsquashfs 1.3.2 and connects LZO decoding where required. The managed Core builds a directory index from inode metadata, translates Windows names and handles image lifetime. WinFsp callbacks supply uncompressed offsets and buffers to the reader. WinUI uses the same Core API. The bundled mksquashfs process builds new images with user-selected parameters and publishes completed images atomically.

## License

GPL-3.0-only. See LICENSE and THIRD-PARTY.md for dependency information.
