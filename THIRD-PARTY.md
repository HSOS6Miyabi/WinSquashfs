# Third-party components

- WinFsp: https://github.com/winfsp/winfsp (GPL-3.0 with upstream exceptions; MSI and source release v2.2, build 2.2.26323). The managed binding is compiled from the pinned upstream source files; scripts/prepare-winfsp-binding.ps1 verifies the Git blob hashes and adds prefix-only mounting, a read-only-volume setting, and an explicit version-checked loader using the registered installation directory.
- libsquashfs / squashfs-tools-ng 1.3.2: https://github.com/AgentD/squashfs-tools-ng/tree/v1.3.2 (LGPL-3.0-or-later library, GPL utilities). Official Windows binaries and source dependencies: https://infraroot.at/pub/squashfs/windows/ and https://infraroot.at/pub/squashfs/.
- LZO 2.10: GPL-2.0-or-later, https://www.oberhumer.com/opensource/lzo/.
- Linux squashfs-tools: GPL-2.0-or-later, https://github.com/plougher/squashfs-tools. Windows build: https://packages.msys2.org/packages/squashfs-tools. MSYS2 packaging and source packages: https://github.com/msys2/MSYS2-packages and https://repo.msys2.org/msys/sources/.
- MSYS2 runtime and codec DLLs: exact installed versions are recorded in licenses/msys2-versions.txt. License files are copied from the MSYS2 installation and the upstream SquashFS Windows archive.
- DwarFS 0.12.4 creation tools: https://github.com/mhx/dwarfs/tree/v0.12.4 (upstream GPL-3.0 and component-specific license terms).
- dwarfs Rust crate 0.2.1: https://github.com/oxalica/dwarfs-rs (MIT/Apache-2.0). Cargo dependencies supply LZMA, LZ4, and Zstandard decoding.
- erofs-utils: https://github.com/erofs/erofs-utils/tree/7e52cbffa83fbe9eb10bbf110cb12d503a048081 (GPL-2.0-or-later/MIT per source file). Native Windows builds include the upstream library and compression dependencies.
- Microsoft Windows App SDK and .NET: https://github.com/microsoft/WindowsAppSDK and https://github.com/dotnet/runtime. Their redistributable components retain their own license terms.

The source of WinSquashfs, its native adapter and build scripts are available in this repository.
