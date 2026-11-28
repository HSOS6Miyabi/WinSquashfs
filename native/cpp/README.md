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

## Windows distribution

Build the native SquashFS and EROFS dependencies with `scripts/build-native.sh` (MSYS2) and `scripts/build-erofs.sh` (Linux with Docker). Install WinFsp using `scripts/install-winfsp.ps1`, then use `native/cpp/scripts/build.ps1` in an MSVC developer environment. The build produces the native DLL, `archivefs.exe`, `WinArchiveFSPro.exe`, `WinArchiveFSPro.Unc.exe`, and the required compression tools in `artifacts/dist`.

The Windows workflow builds the official DwarFS library, runs the core tests and the real-archive integration tests, builds the installer, and verifies the installed application.

## Command-line usage

```text
archivefs info IMAGE
archivefs list IMAGE [DIRECTORY]
archivefs read IMAGE ENTRY OFFSET COUNT
archivefs verify IMAGE
archivefs unpack IMAGE DESTINATION
archivefs create SquashFS|DwarFS|EROFS PRESET OUTPUT SOURCE...
archivefs pack SOURCE OUTPUT [CODEC]
archivefs mount IMAGE [DRIVE:]
archivefs eject DRIVE:
archivefs unc-start
archivefs unc-stop
archivefs unc-status
archivefs unc-path IMAGE
archivefs unc-open IMAGE
archivefs serve ALIAS ROOT [IDLE_SECONDS]
archivefs doctor
```

Use `--lang en-US`, `--lang zh-CN` or `--lang system` to select the command-line language. The portable core test executable is named `archivefs-read`.

## Architecture

- `RandomAccessSource` and `ReadSession`: positioned reads, scoped source handles and identity validation; Windows opportunistic locks release transient sources when conflicting operations occur.
- `ArchiveBackend`: format-specific metadata and regular-file access, including the official DwarFS file-view interface.
- `ArchiveReader`: concurrent page caching, capacity-based eviction and request coalescing.
- `ArchiveTree`: directory indexing, Windows filename encoding, verification and extraction.
- `ImageCreator`: eight compression presets, format-specific options, multiple sources, binary TAR streaming, child-process cancellation and atomic output publication.
- `Projection` and `ImagePool`: read-only drive and UNC namespace projection, concurrent image opening, active leases and idle expiration.
- `NativeMount`: WinFsp native callbacks and volume lifecycle.
- `Settings`, `MountedVolume` and the UNC agent: per-user settings, background access, mount management and Explorer integration.

The UNC agent exposes exact registered prefixes such as `\\sfs$\C`, `\\sfs$\UNC`, `\\sfs.localhost\C` and the configured alias. Network images can be opened through `\\sfs$\UNC\SERVER\SHARE\IMAGE`. Both host aliases use the same image pool. No extracted file tree is stored on disk to serve filesystem reads.
