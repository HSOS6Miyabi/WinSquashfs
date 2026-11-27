# WinSquashfs v0.1.0

First public release for Windows x64.

- WinUI 3 interface in English and Simplified Chinese, including language selection.
- Browse and preview SquashFS images, extract files and directories, and verify image readability.
- Mount images as read-only volumes through WinFsp with direct in-memory random access.
- Direct UNC access using `\\sfs$\C\...`, directory aliases, and configurable idle caching.
- Create SquashFS images with gzip, XZ, LZO, LZ4, or Zstandard and advanced compression settings.
- Bundled mksquashfs, unsquashfs, sqfscat, and sqfstar, and the winsqfs command-line utility.
- NSIS installation with file associations and Explorer context menus.

## Downloads

- **WinSquashfs-Setup-x64.exe**: includes the application and WinFsp installer.
- **WinSquashfs-portable-x64.zip**: extract to a local directory. Install WinFsp separately to use mounting and UNC access.
- **SHA256SUMS.txt**: SHA-256 checksums for both distributions.

Windows 10 version 1809 or later is required. Linux symbolic links and special files are displayed in the browser; Windows mounting and extraction expose ordinary files and directories. The application binaries are not Authenticode-signed.

# 简体中文

Windows x64 首个公开版本。支持中英文界面、镜像浏览与预览、文件及目录提取、镜像可读性校验、WinFsp 只读挂载、UNC 按需访问和高级压缩参数。安装程序包含 WinFsp，并注册文件关联和资源管理器右键菜单。

安装程序：WinSquashfs-Setup-x64.exe。便携版：WinSquashfs-portable-x64.zip。SHA256SUMS.txt 提供两种发布文件的 SHA-256 校验值。便携版使用挂载和 UNC 访问功能时需要单独安装 WinFsp。

最低支持 Windows 10 1809。Windows 挂载及提取功能支持普通文件和目录，浏览器可以识别 Linux 符号链接及特殊文件。应用程序二进制文件尚未进行 Authenticode 签名。
