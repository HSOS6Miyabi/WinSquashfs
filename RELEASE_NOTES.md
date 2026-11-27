# WinSquashfs v0.1.1

Windows x64 maintenance release.

- Requires WinFsp 2026 (2.2.26323) or a later compatible 2.x runtime. The installer upgrades older installations and includes the official WinFsp MSI.
- Loads the native WinFsp DLL from its registered installation directory, checks its version, and reports detailed initialization failures.
- Adds a WinFsp runtime diagnostic in the GUI and the winsqfs doctor command.
- The portable distribution includes prerequisites/WinFsp.msi for offline driver installation.

## Downloads

- **WinSquashfs-Setup-x64.exe**: includes the application and WinFsp installer.
- **WinSquashfs-portable-x64.zip**: includes the application and the prerequisite MSI.
- **SHA256SUMS.txt**: SHA-256 checksums for both distributions.

Windows 10 version 1809 or later is required. The application binaries are not Authenticode-signed.

# 简体中文

Windows x64 维护版本。安装程序会检查 WinFsp 运行时版本，并在需要时安装或升级至 WinFsp 2026（2.2.26323）。便携版内含 prerequisites\WinFsp.msi，可离线安装。

程序从 WinFsp 的注册安装目录加载匹配的原生 DLL，并在运行时初始化失败时显示详细信息。可通过“工具与关于 → 检查 WinFsp 运行时”或 winsqfs doctor 查看安装情况。

发布文件包括安装程序、便携版 ZIP 和 SHA-256 校验文件。最低支持 Windows 10 1809，应用程序二进制文件尚未进行 Authenticode 签名。
