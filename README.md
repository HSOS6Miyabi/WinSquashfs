# WinArchiveFSPro

[简体中文](README.zh-CN.md)

A native Windows application for browsing, creating, extracting, verifying and mounting SquashFS, DwarFS and EROFS images.

## Features

- WinUI 3 interface in English and Simplified Chinese, previews, recursive file search, recent images, appearance settings and drag and drop.
- Multi-file and directory selection, eight compression presets, format-specific advanced settings, and batch creation and extraction.
- Native WinFsp read-only drive mounts and UNC access, with parallel random reads and no temporary extracted file tree.
- Shared image caching, configurable per-image cache capacity and idle expiration.
- Explorer Open/Edit, multi-selection archive creation and eject commands.

| Format | Reader | Creator |
| --- | --- | --- |
| SquashFS | libsquashfs, parallel read contexts | mksquashfs |
| DwarFS | official C++ DwarFS 0.15.10 | mkdwarfs |
| EROFS | liberofs, single-device raw images | mkfs.erofs |

Regular files and directories are projected into Windows. Symbolic links and special files are displayed as metadata. Incompatible Windows names are encoded deterministically. Extreme compression uses format-specific high-ratio settings.

## Installation

Windows x64, Windows 10 1809 or newer. Mounting and UNC access require WinFsp 2.2.26323 or a compatible newer 2.x version. The installer installs or upgrades WinFsp; the portable distribution includes its MSI in prerequisites. No .NET runtime is required.

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
