# WinArchiveFSPro

简体中文 | [English](README.md)

适用于 Windows 的原生只读存档文件系统管理器，支持浏览、创建、校验、提取和挂载 SquashFS、DwarFS、EROFS 镜像。

## 主要功能

- WinUI 3 中英文界面，支持文件预览、筛选、最近打开记录、拖放及多文件选择。
- 八档压缩预设及各格式的高级压缩选项。
- 通过 WinFsp 原生接口挂载只读盘符或直接访问 UNC 路径，无需将镜像内容提取到磁盘。
- 并发随机读取、共享镜像缓存及可配置的单镜像缓存容量。
- 资源管理器打开、编辑、弹出及多选创建存档命令。

| 格式 | 读取模块 | 创建工具 |
| --- | --- | --- |
| SquashFS | libsquashfs，并行读取上下文 | mksquashfs |
| DwarFS | 官方 C++ DwarFS 0.15.10 | mkdwarfs |
| EROFS | liberofs，单设备原始镜像 | mkfs.erofs |

挂载卷映射普通文件及目录，符号链接和特殊文件在浏览器中显示其类型。对 Windows 不支持的名称生成兼容名称。极限压缩采用各格式的高压缩率设置。

## 安装

支持 Windows x64，Windows 10 1809 及更新版本。挂载及 UNC 访问需要 WinFsp 2.2.26323 或兼容的更新 2.x 版本。安装程序可安装或升级 WinFsp，便携版的 prerequisites 目录包含驱动安装包。无需安装 .NET。

[版本下载](https://github.com/HSOS6Miyabi/WinSquashfs/releases) · [构建记录](https://github.com/HSOS6Miyabi/WinSquashfs/actions)

## UNC 访问

启动独立 UNC 后台进程后，可通过以下路径访问镜像：

    \\sfs$\C\images\linux.squashfs\etc\os-release
    \\sfs.localhost\C\images\linux.squashfs
    \\sfs$\path\archive.dwarfs
    \\sfs$\UNC\NAS\backup\archive.erofs

两个虚拟主机名共用已启用的盘符映射、自定义目录和网络共享入口。双击网络镜像，或执行 `archivefs unc-alias IMAGE`，即可注册 `\\sfs.NAS.localhost\backup` 形式的主机及共享名称。后台进程重新启动时恢复已注册的名称；首次访问其他主机或共享时仍可使用 `\\sfs$\UNC` 通用入口。程序使用实际运行进程的 Windows 访问令牌和网络凭据，显式路径按文件内容识别格式。本地临时访问优先使用机会锁，其他源采用短时读取句柄及文件身份检查。源镜像发生变更后，相应缓存失效。显式盘符挂载期间保护源镜像，弹出后解除保护。后台进程可独立运行并设置为登录时启动。

## 命令行与构建

执行 archivefs --help 查看命令，使用 --lang zh-CN 输出简体中文。构建及测试方法见[原生核心文档](native/cpp/README.md)。

本项目采用 GPL-3.0-only 许可证，依赖组件许可证见 [THIRD-PARTY.md](THIRD-PARTY.md)。
