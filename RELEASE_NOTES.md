# WinSquashfs v0.1.4

- Source access uses the application's actual Windows access token and network credentials.
- Supports local and UNC directory aliases, mapped drives, directory junctions, symbolic links and explicitly named images with any extension.
- Network filesystems with limited identity information use available metadata and release idle image caches immediately.
- Existing opportunistic locks, asynchronous directory notifications and drive mounting remain available.

## 简体中文

- 使用程序实际运行时的 Windows 访问令牌及网络凭据读取镜像。
- 支持本地及 UNC 目录映射、映射的网络驱动器、目录联接、符号链接和任意扩展名的镜像文件。
- 对文件身份信息不完整的网络文件系统，使用可用的元数据检查源文件，并在镜像空闲时立即释放缓存。
- 保留机会锁、异步目录变更通知及盘符挂载功能。

Windows x64. WinFsp 2.2.26323 or a compatible newer 2.x runtime is required for mounting and UNC access. Both distributions include the WinFsp runtime installer.
