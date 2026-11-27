# WinSquashfs

简体中文 | [English](README.md)

WinSquashfs 是适用于 Windows x64 的 SquashFS 镜像管理器，采用 WinUI 3 和 WinFsp，可直接浏览镜像、预览文件、提取内容、创建镜像及挂载只读卷。

## 主要功能

- 无需挂载即可浏览镜像中的文件和目录，筛选文件，查看属性、文本及十六进制预览。
- 通过 WinFsp 挂载只读卷，支持自动选择盘符或指定盘符。
- 通过 UNC 路径直接访问镜像，按需在内存中解压数据块，不占用盘符。
- 支持 GZIP、XZ、LZO、LZ4 和 Zstandard 压缩算法，可设置数据块大小、压缩级别、处理器数量、内存上限及各类高级参数。
- 集成 mksquashfs、unsquashfs、sqfscat 和 sqfstar 的 Windows 移植版本。
- 支持镜像校验、最近打开记录、拖放打开、文件和目录提取。
- 安装程序注册文件关联及资源管理器右键菜单。

## 安装及语言设置

从 [Releases](https://github.com/HSOS6Miyabi/WinSquashfs/releases) 或 [Actions](https://github.com/HSOS6Miyabi/WinSquashfs/actions) 下载 Windows x64 安装程序或便携版。安装程序集成 WinFsp；便携版的挂载与 UNC 功能需要系统中已安装 WinFsp。

界面默认跟随 Windows 显示语言，可在“工具与关于 → 界面语言”选择简体中文或英文，保存后重新启动程序。安装程序支持简体中文和英文，右键菜单使用安装时选择的语言。

## UNC 访问

在“UNC 访问”页面启动后台进程。启用本地磁盘映射后，可以直接通过以下路径访问镜像中的文件：

    C:\images\linux.squashfs
    \\sfs$\C\images\linux.squashfs
    \\sfs$\C\images\linux.squashfs\etc\os-release

也可将自定义目录映射为共享名称，例如将 C:\images 映射为 path，然后使用 \\sfs$\path\linux.squashfs 访问镜像。

关闭主窗口后，后台进程仍可提供访问。可以设置登录时自动启动、空闲超时及空闲镜像数量上限。应用共享设置将重新启动后台进程，请先关闭正在使用的 UNC 文件。

## 命令行

    winsqfs --version
    winsqfs --lang zh-CN info image.squashfs
    winsqfs --lang zh-CN list image.squashfs
    winsqfs --lang zh-CN verify image.squashfs
    winsqfs mount image.squashfs X:
    winsqfs serve path C:\images 60
    winsqfs unc-start
    winsqfs unc-status
    winsqfs unc-path C:\images\linux.squashfs
    winsqfs unc-stop
    winsqfs pack C:\source C:\output.squashfs zstd
    winsqfs unpack image.squashfs C:\new-directory

命令行默认使用英文，可通过 --lang zh-CN、--lang en-US 或 --lang system 指定本次命令的语言。原版 squashfs-tools 工具输出保留原始语言。按 Ctrl+C 停止命令行挂载或前台 UNC 服务。

## 兼容性

Windows 文件系统仅映射普通文件和目录。Linux 符号链接和特殊文件在浏览器中显示其类型；提取到 Windows 时跳过此类文件。对于 Windows 不支持的文件名，程序会生成兼容的名称。镜像校验检查文件能否读取及数据块是否有效。

## 构建与许可证

参见 [英文 README](README.md) 中的构建说明。Windows CI 会运行文件系统、压缩算法、安装和中英文界面自动化测试，并上传窗口截图。项目版本号统一在 Directory.Build.props 中设置。Windows CI 完成所有测试后，发布尚未公开的版本，生成安装程序、便携版 ZIP 和 SHA-256 校验文件。

本项目采用 GPL-3.0-only 许可证，依赖组件的许可证见 [THIRD-PARTY.md](THIRD-PARTY.md)。
