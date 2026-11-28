# WinArchiveFSPro — C++ native application

WinArchiveFSPro manages read-only SquashFS, DwarFS and EROFS filesystem images. The native C++23 core is shared by the command-line program, the WinUI 3 application and the WinFsp UNC background process.

## Core build and tests

CMake 3.22 or newer, a C++23 compiler, libarchive and Python 3 are required. Linux builds also require OpenSSL. Windows application builds require the Windows SDK, MSVC, WinFsp SDK and nlohmann-json.

```sh
cmake -S native/cpp -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
```

The test configuration builds independent deterministic decoder and creator implementations. It covers cache concurrency, positioned reads, file identity, image-tree navigation, extraction, command-line parsing, binary TAR streaming, compression preset arguments and projected filesystem paths.

For a Windows application build, set `ARCHIVEFS_WITH_DWARFS=ON` and provide the official DwarFS 0.15.10 CMake package. The SquashFS and EROFS libraries use the existing native decoder interfaces. The SquashFS adapter provides independent read contexts for parallel decompression.

### Additional archive formats

ZIP (including ZIP64), 7z, RAR, TAR, CPIO, CAB, ISO9660, AR and LHA are available through the same read-only browser, CLI, drive mounts and UNC namespace. TAR supports gzip, bzip2, xz, Zstandard, LZ4, LZMA, lzip and Unix compress filters; standalone compressed streams appear as a single file. Ordinary TAR entries use indexed physical offsets, including sparse extent maps. GZIP-compressed TAR uses an in-memory index of bounded decompressor snapshots to restart near the requested uncompressed offset, including concatenated GZIP members. The snapshot reservation shares the configured image cache budget with decoded pages. Other compressed TAR and solid archives reuse decoder cursors and decoded-page caches; a backward cache miss can require replaying a stream. No extracted files or intermediate TAR files are written for reads.

Archive creation remains available for SquashFS, DwarFS and EROFS. Encrypted archives requiring passwords, split/multi-volume archives and UDF-only discs are not supported. The installer offers archive-specific Explorer commands without changing existing default archive applications.

## Windows distribution

Build the native SquashFS and EROFS dependencies with `scripts/build-native.sh` (MSYS2) and `scripts/build-erofs.sh` (Linux with Docker). Install WinFsp using `scripts/install-winfsp.ps1`, then use `native/cpp/scripts/build.ps1` in an MSVC developer environment. The build produces the native DLL, `archivefs.exe`, `WinArchiveFSPro.exe`, `WinArchiveFSPro.Unc.exe`, and the required compression tools in `artifacts/dist`.

The Windows workflow builds the official DwarFS library, runs the core tests and the real-archive integration tests, builds the installer, and verifies the installed application.

## Command-line usage

```text
archivefs info IMAGE
archivefs list IMAGE [DIRECTORY]
archivefs search IMAGE QUERY [DIRECTORY]
archivefs read IMAGE ENTRY OFFSET COUNT
archivefs verify IMAGE
archivefs unpack IMAGE DESTINATION
archivefs batch-unpack OUTPUT_DIRECTORY IMAGE...
archivefs create SquashFS|DwarFS|EROFS PRESET OUTPUT SOURCE...
archivefs batch-create SquashFS|DwarFS|EROFS PRESET OUTPUT_DIRECTORY SOURCE...
archivefs pack SOURCE OUTPUT [CODEC]
archivefs mount IMAGE [DRIVE:]
archivefs eject DRIVE:
archivefs unc-start
archivefs unc-stop
archivefs unc-status
archivefs unc-path IMAGE
archivefs unc-alias IMAGE
archivefs unc-open IMAGE
archivefs serve ALIAS ROOT [IDLE_SECONDS]
archivefs doctor
```

Use `--lang en-US`, `--lang zh-CN` or `--lang system` to select the command-line language. The portable core test executable is named `archivefs-read`.

## Architecture

- `RandomAccessSource` and `ReadSession`: positioned reads, scoped source handles and identity validation; Windows opportunistic locks release transient sources when conflicting operations occur.
- `ArchiveBackend`: format-specific metadata and regular-file access, including the official DwarFS file-view interface.
- `ArchiveReader`: concurrent page caching, capacity-based eviction and request coalescing.
- `ArchiveTree`: directory indexing, Windows filename encoding, recursive search, verification and extraction.
- `ImageCreator`: eight compression presets, format-specific options, multiple sources, individual archive batches, binary TAR streaming, child-process cancellation and atomic output publication.
- `Projection` and `ImagePool`: read-only drive and UNC namespace projection, concurrent image opening, active leases, aggregate decoded-data cache reservations and idle expiration.
- `NativeMount`: WinFsp native callbacks and volume lifecycle.
- `Settings`, `MountedVolume` and the UNC agent: per-user settings, background access, notification area controls, mount management and Explorer integration.

The UNC agent exposes exact registered prefixes such as `\\sfs$\C`, `\\sfs$\UNC`, `\\sfs.localhost\C` and the configured alias. Network images can be opened through `\\sfs$\UNC\SERVER\SHARE\IMAGE`. Both host aliases use the same image pool. No extracted file tree is stored on disk to serve filesystem reads.

### Symbolic links and special objects

`archivefs readlink IMAGE ENTRY` prints the stored link target. Mounted and UNC archives resolve relative and absolute POSIX symbolic links inside their own image, using exact original filename bytes. Dangling links, cycles, paths escaping the image, and non-file special objects remain visible but cannot be opened for data access. They are not exposed as host devices or Windows reparse points. Extraction copies regular files and directories.
