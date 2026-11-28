# Third-party components

- WinFsp: https://github.com/winfsp/winfsp (GPL-3.0 with upstream exceptions; SDK and MSI v2.2, build 2.2.26323). The native C++ application uses the installed user-mode API.
- libsquashfs / squashfs-tools-ng 1.3.2: https://github.com/AgentD/squashfs-tools-ng/tree/v1.3.2 (LGPL-3.0-or-later library, GPL utilities). Official Windows binaries and source dependencies: https://infraroot.at/pub/squashfs/windows/ and https://infraroot.at/pub/squashfs/.
- LZO 2.10: GPL-2.0-or-later, https://www.oberhumer.com/opensource/lzo/.
- Linux squashfs-tools: GPL-2.0-or-later, https://github.com/plougher/squashfs-tools. Windows build: https://packages.msys2.org/packages/squashfs-tools. MSYS2 packaging and source packages: https://github.com/msys2/MSYS2-packages and https://repo.msys2.org/msys/sources/.
- MSYS2 runtime and codec DLLs: exact installed versions are recorded in licenses/msys2-versions.txt. License files are copied from the MSYS2 installation and the upstream SquashFS Windows archive.
- DwarFS 0.15.10: https://github.com/mhx/dwarfs/tree/v0.15.10 (upstream GPL-3.0 and component-specific terms). Official C++ reader and creation tools.
- erofs-utils: https://github.com/erofs/erofs-utils/tree/7e52cbffa83fbe9eb10bbf110cb12d503a048081 (GPL-2.0-or-later/MIT per source file). Native Windows builds include the upstream library and compression dependencies.
- libarchive: https://github.com/libarchive/libarchive (BSD-2-Clause and component-specific terms), used for TAR input and general archive decoding.
- nlohmann/json: https://github.com/nlohmann/json (MIT), used for settings.
- Microsoft Windows App SDK, C++/WinRT and Visual C++ redistributable libraries retain their respective license terms.
- vcpkg dependency licenses are included in licenses/vcpkg.

The source of WinArchiveFSPro, its native adapter and build scripts are available in this repository.

- libcurl: https://curl.se/libcurl/ (curl license), HTTP, FTP, TLS and AWS Signature V4.
- libssh2: https://libssh2.org/ (BSD-3-Clause), SSH transport and authentication.
- libxml2: https://gitlab.gnome.org/GNOME/libxml2 (MIT), WebDAV and S3 XML responses.
