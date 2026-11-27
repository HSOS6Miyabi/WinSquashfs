# WinSquashfs

A WinUI 3 desktop application for SquashFS images on Windows x64. Browse image contents, preview files, build new images, extract files or mount a read-only drive using WinFsp.

## Features

- In-process archive browser with directory navigation, filename filtering, file metadata, text and hexadecimal previews.
- WinFsp read-only drive mounting with automatic or explicit drive-letter selection and multiple concurrent mounts.
- Direct random reads from compressed images through libsquashfs. Data and fragment blocks are decompressed in memory. No temporary extraction directory is created by browsing or mounting.
- SquashFS 4.0 reading: gzip, XZ, LZO, LZ4, Zstandard and legacy LZMA where supported by the bundled decoder.
- Linux squashfs-tools Windows ports: mksquashfs, unsquashfs, sqfscat and sqfstar. Image creation uses mksquashfs; Windows-safe extraction uses the archive reader.
- Advanced image creation: compressor, compression level, block size, worker count, memory limit, timestamps, exclusions, fragments, duplicate detection, sparse detection, ownership, export table, and compressor-specific parameters.
- NSIS installation, file associations for .squashfs, .sqfs and .sfs, and Explorer commands to browse, mount, extract or create an image.

## Installation

Download the x64 installer from [Releases](https://github.com/HSOS6Miyabi/WinSquashfs/releases) or the Windows build artifact from [Actions](https://github.com/HSOS6Miyabi/WinSquashfs/actions). The installer includes the WinFsp runtime and a self-contained Windows App SDK/.NET application. Windows 10 version 1809 or newer is required. A portable distribution requires WinFsp to be installed separately for mounting.

## Usage

Open an image from the browser or Explorer. Select files for previews or extraction, double-click a folder to navigate, and choose Mount image to expose it as a read-only drive. Mounted images remain active while the application instance is open. Unmount them from the Mounted images page.

Create image accepts a source directory and a new output path. Zstandard is the default. Advanced parameters are optional. Files with Windows-incompatible names receive deterministic Windows-safe names; the browser preserves the original names for display. Linux symbolic links, sockets and devices are listed as metadata; they are omitted from the mounted filesystem and Windows-safe extraction. The original unsquashfs executable is available for Linux-oriented extraction behavior.

## Command line

    winsqfs info image.squashfs
    winsqfs list image.squashfs
    winsqfs list image.squashfs /directory
    winsqfs read image.squashfs /file.bin 65536 4096
    winsqfs mount image.squashfs X:
    winsqfs pack C:\source C:\output.squashfs zstd
    winsqfs unpack image.squashfs C:\new-directory

The read command writes hexadecimal bytes. Ctrl+C stops a command-line mount. Native tools are located in the tools subdirectory.

## Build

On Windows x64, install .NET 8, Visual Studio with Windows SDK and desktop development components, MSYS2 MINGW64 with mingw-w64-x86_64-gcc, squashfs-tools, curl and unzip, and NSIS. From the repository root:

    bash scripts/build-native.sh
    pwsh scripts/install-winfsp.ps1
    pwsh scripts/publish.ps1

The distribution is written to artifacts/dist. The Windows workflow builds and tests the distribution and publishes installer and ZIP files when a version tag is pushed.

## Architecture

A C adapter opens SquashFS images through libsquashfs 1.3.2 and connects LZO decoding where required. The managed Core builds a directory index from inode metadata, translates Windows names and handles image lifetime. WinFsp callbacks supply uncompressed offsets and buffers to the reader. WinUI uses the same Core API. The bundled mksquashfs process builds new images with user-selected parameters and publishes completed images atomically.

## License

GPL-3.0-only. See LICENSE and THIRD-PARTY.md for dependency information.
