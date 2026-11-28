# WinArchiveFSPro

简体中文 | [English](README.md)

适用于 Windows 的原生只读存档文件系统管理器，支持浏览、创建、校验、提取和挂载 SquashFS、DwarFS、EROFS 镜像。

## 主要功能

- WinUI 3 中英文界面，支持文件预览、递归搜索、最近打开记录、外观切换、拖放及多文件选择。
- 八档压缩预设、各格式的高级压缩选项、批量创建及提取。
- 通过 WinFsp 原生接口挂载只读盘符或直接访问 UNC 路径，无需将镜像内容提取到磁盘。
- 并发随机读取、共享镜像缓存、可配置的单镜像及解压数据缓存总容量。
- 资源管理器打开、编辑、弹出及多选创建存档命令。

| 格式 | 读取模块 | 创建工具 |
| --- | --- | --- |
| SquashFS | libsquashfs，并行读取上下文 | mksquashfs（x64）、tar2sqfs（x86/ARM64） |
| DwarFS | 官方 C++ DwarFS 0.15.10 | mkdwarfs |
| EROFS | liberofs，单设备原始镜像 | mkfs.erofs |

挂载卷映射普通文件及目录，符号链接和特殊文件在浏览器中显示其类型。对 Windows 不支持的名称生成兼容名称。极限压缩采用各格式的高压缩率设置。

### 通用存档格式

支持 ZIP（含 ZIP64）、7z、RAR、TAR、CPIO、CAB、ISO9660、AR 和 LHA 的浏览、随机读取、提取、盘符挂载与 UNC 访问。TAR 支持 gzip、bzip2、xz、Zstandard、LZ4、LZMA、lzip 及 Unix compress 外层压缩；独立压缩流显示为一个文件。

普通 TAR 顺序解析元数据并记录数据偏移量，稀疏文件按数据区间读取。TAR.GZ 在内存中保存有限数量的解压状态检查点，支持串联 GZIP 成员，并能从接近目标位置的检查点继续解压。检查点与解压数据分页缓存共用配置的镜像缓存容量。TAR.XZ 利用 XZ 自带的块索引直接定位独立压缩块，小块使用有限容量的缓存，大块采用流式解压。采用可定位帧格式的 TAR.ZST 根据文件末尾的帧索引直接访问独立压缩帧，并校验存档中提供的帧校验和。其他压缩 TAR 和固实压缩存档通过复用解压状态及分页缓存提高重复读取效率，反向读取未缓存的数据时可能需要重新解码。读取过程不生成本地解压文件或中间 TAR 文件。

创建功能适用于 SquashFS、DwarFS 和 EROFS。暂不支持需要密码的加密存档、分卷存档以及仅包含 UDF 文件系统的光盘镜像。安装程序添加资源管理器专用命令，并保留常见存档格式原有的默认应用。

## 安装

支持 Windows 10 1809 及更新版本，提供 x64、x86 和 ARM64 原生版本。请根据操作系统的架构选择安装包。挂载及 UNC 访问需要 WinFsp 2.2.26323 或兼容的更新 2.x 版本。安装程序可安装或升级 WinFsp，便携版的 prerequisites 目录包含驱动安装包。无需安装 .NET。

[版本下载](https://github.com/HSOS6Miyabi/WinSquashfs/releases) · [构建记录](https://github.com/HSOS6Miyabi/WinSquashfs/actions)

## UNC 访问

启动独立 UNC 后台进程后，可通过以下路径访问镜像：

    \\sfs$\C\images\linux.squashfs\etc\os-release
    \\sfs.localhost\C\images\linux.squashfs
    \\sfs$\path\archive.dwarfs
    \\sfs$\UNC\NAS\backup\archive.erofs

两个虚拟主机名共用已启用的盘符映射、多个自定义目录和网络共享入口，并自动检测新增的本地及映射盘符。双击网络镜像，或执行 `archivefs unc-alias IMAGE`，即可注册 `\\sfs.NAS.localhost\backup` 形式的主机及共享名称。后台进程重新启动时恢复已注册的名称；首次访问其他主机或共享时仍可使用 `\\sfs$\UNC` 通用入口。程序使用实际运行进程的 Windows 访问令牌和网络凭据，显式路径按文件内容识别格式。本地临时访问优先使用机会锁，其他源采用短时读取句柄及文件身份检查。源镜像发生变更后，相应缓存失效。显式盘符挂载期间保护源镜像，弹出后解除保护。后台进程可独立运行并设置为登录时启动，也可通过通知区域图标打开主窗口或停止 UNC 访问。

## 命令行与构建

执行 archivefs --help 查看命令，使用 --lang zh-CN 输出简体中文。构建及测试方法见[原生核心文档](native/cpp/README.md)。

本项目采用 GPL-3.0-only 许可证，依赖组件许可证见 [THIRD-PARTY.md](THIRD-PARTY.md)。


### 便携设备与 Android 文件访问

启动 UNC 后台程序后，可直接使用以下目录：

- `\\sfs$\mtp`：枚举 Windows Portable Devices（MTP/PTP）设备。
- `\\sfs$\adb`：枚举已连接至本地 ADB Server 的 Android 设备。
- `\\sfs$\adb.r`：使用设备已授权的 su 权限访问 Android 文件。

在设备管理页面可启用或关闭各目录、指定 ADB 端口、刷新设备列表并打开资源管理器。ADB 客户端直接连接现有服务（默认 127.0.0.1:5037），不会启动或重启服务。支持 UTF-8 文件名、64 位文件大小和偏移量。Root 模式需要设备已有可用的 su，并完成设备上的授权。设备目录为只读。

慢速镜像打开时，后台程序显示 Windows 进度通知；百分比表示成功读取的最远文件偏移量相对于镜像大小的比例。支持通过外层镜像直接打开内层 SquashFS、DwarFS、EROFS 镜像，无需解压到本地临时文件。

挂载镜像时，符号链接在镜像内部解析，包括 POSIX 绝对路径。无有效目标的链接及特殊文件仍在目录中显示，但不能读取其数据流。可通过 `archivefs readlink IMAGE ENTRY` 查看符号链接目标。


### 远程连接

启动 UNC 后台程序，在“远程连接”页面保存连接。各协议目录为：

    \\$sfs\dav
    \\$sfs\s3
    \\$sfs\ftp
    \\$sfs\ftps
    \\$sfs\sftp
    \\$sfs\scp

通过 \\$sfs\root 浏览统一目录；安装经过签名的可选 UNC 过滤驱动后，可直接访问 \\$sfs\。原有 sfs$ 和 sfs.localhost 主机名继续可用。

WebDAV 支持 HTTP Range 读取以及服务器声明的 SabreDAV PATCH 部分更新扩展。服务器特有的部分 PUT 需要手动启用。FTP 和 FTPS 使用 MLSD 列目录、REST 定位读取及 APPE 追加写入。S3 使用 Signature V4、路径形式的存储桶地址、ListObjectsV2、Range 读取、PUT 和 CopyObject。S3 对不小于 64 MiB 的对象采用分段上传，独占复制及大对象复制采用服务器端分段复制，并在完成时检查目标对象的写入条件。目录重命名先复制全部对象，再检查源对象版本并按条件删除原对象；此操作不具备原子性。

SFTP 使用原生偏移量读写，服务器本机复制优先使用协商得到的 copy-data 扩展，必要时使用可配置的 SSH 命令。SCP 通过 SSH 命令进行偏移量读写，通过 SCP 上传文件，并可启用服务器已授权的非交互式 sudo。SSH 命令根目录可与 SFTP 可见根目录分别配置。

每个连接可配置只读模式、超时、读取缓存容量、元数据缓存有效期和写入缓存容量。自动模式优先直接写入；写回模式在本地保存完整文件，在刷新时上传；直接模式拒绝需要缓存的操作。写回缓存的数据及恢复记录在返回写入成功前刷新至本地存储。进程重启或上传响应异常时，可重新读取完整的待上传文件，使用 GUI 的“重试待上传文件”或 archivefs remote-retry PROTOCOL CONNECTION /path 显式重试。恢复时核对连接身份和源文件版本；若此前可能已成功上传，则比较服务器与缓存的完整内容。使用 archivefs remote-discard PROTOCOL CONNECTION /path --confirm 可显式放弃完整的待上传数据。GUI 的“在服务器上复制”、archivefs remote-copy 命令和资源管理器的“在服务器上粘贴副本”菜单项均通过正在运行的 UNC 后台进程执行，共用文件状态及缓存。资源管理器命令适用于同一已保存连接中的文件，不覆盖现有目标文件。相关命令的路径使用挂载目录中显示的 Windows 名称，以斜杠分隔。Windows 特有的文件属性和 ACL 修改存在限制；远程卷显示的容量为虚拟值，不代表服务器剩余配额。

密码及令牌通过 Windows DPAPI 加密保存。SSH 需要核对可信的 SHA256 主机指纹。TLS 校验证书和主机名，可指定私有 CA 证书文件。请在不可信网络中使用 HTTPS 或 FTPS。
