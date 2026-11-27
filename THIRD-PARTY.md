# Third-party components

- WinFsp: https://github.com/winfsp/winfsp (GPL-3.0 with upstream exceptions; MSI and source release v2.2B1). The managed binding is compiled from the pinned upstream source files; scripts/prepare-winfsp-binding.ps1 verifies the Git blob hashes and adds prefix-only mounting and a read-only-volume setting.
- libsquashfs / squashfs-tools-ng 1.3.2: https://github.com/AgentD/squashfs-tools-ng/tree/v1.3.2 (LGPL-3.0-or-later library, GPL utilities). Official Windows binaries and source dependencies: https://infraroot.at/pub/squashfs/windows/ and https://infraroot.at/pub/squashfs/.
- LZO 2.10: GPL-2.0-or-later, https://www.oberhumer.com/opensource/lzo/.
- Linux squashfs-tools: GPL-2.0-or-later, https://github.com/plougher/squashfs-tools. Windows build: https://packages.msys2.org/packages/squashfs-tools. MSYS2 packaging and source packages: https://github.com/msys2/MSYS2-packages and https://repo.msys2.org/msys/sources/.
- MSYS2 runtime and codec DLLs: exact installed versions are recorded in licenses/msys2-versions.txt. License files are copied from the MSYS2 installation and the upstream SquashFS Windows archive.
- Microsoft Windows App SDK and .NET: https://github.com/microsoft/WindowsAppSDK and https://github.com/dotnet/runtime. Their redistributable components retain their own license terms.

The source of WinSquashfs, its native adapter and build scripts are available in this repository.
