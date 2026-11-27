# WinSquashfs v0.1.2

- The default file association opens images directly in Explorer through the UNC namespace. Edit opens the graphical image browser.
- Remote source images can be opened through \\sfs$\UNC\server\share\path\image.squashfs.
- Local UNC image sources use Read-Handle oplocks when available. Network and unsupported filesystems use short-lived read handles with file identity checks.
- Removing, renaming, replacing or modifying an image invalidates its cached contents, even when virtual handles remain open. Shared asynchronous directory notifications and deadline-based cache expiration eliminate periodic source polling.
- The Mount image button uses the system accent color.

## 简体中文

- 双击已关联的镜像文件直接在资源管理器中打开 UNC 路径，编辑命令打开图形界面。
- 支持通过 \\sfs$\UNC\服务器\共享名\路径 访问网络镜像。
- 支持本地机会锁自动释放；网络文件及不支持机会锁的文件系统采用短时读取句柄和文件身份校验。
- 源文件删除、重命名、替换或修改时，自动释放对应镜像缓存；同目录共享异步变更通知，缓存按空闲期限调度，无周期性文件检查。
- 使用系统强调色高亮“挂载镜像”按钮。

Windows x64. WinFsp 2.2.26323 or a compatible newer 2.x runtime is required for mounting and UNC access. The installer and portable distribution include the runtime installer.
