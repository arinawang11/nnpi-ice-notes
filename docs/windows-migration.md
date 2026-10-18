# 主机功能迁移到 Windows

卡端运行 Linux，主机侧最初也在 Linux 下管理。把管理面迁移到 Windows
需要分层处理，不能只替换编译器。

## 现有材料

在 `nnpi-host-upgrade/src`、`nnpi-drv-5.15` 与 GEMV 实验目录中，没有
现成的 `.inf` / `.sys` / `.vcxproj` / `.sln`，也没有 `_WIN32` 或
`DeviceIoControl` 后端。Linux 内核模块不能直接在 Windows 加载；
`pthread`、文件描述符、`ioctl`、`mmap` 也不是可以原样移植的设备后端。

## 分层建议

1. **固定 GEMV 任务**：数学定义、二进制格式和参考校验可以最先迁移，
   不依赖设备驱动。现有 Python 工具使用 `dir_fd`、`O_NOFOLLOW`、
   `O_DIRECTORY` 等 Linux 语义，需要为 Windows 提供等价的文件后端，
   同时保留拒绝重解析点、路径重定向和禁止覆盖的检查。
2. **Windows PCI 驱动**：需要独立的生命周期、BAR 资源、中断、DMA 和
   控制接口。Linux 的 ioctl 数值不能当作 Windows ABI。本仓库的
   `windows-driver/` 是这一层的独立实现。
3. **ring/IPC 用户库**：协议格式可以复用；设备句柄、线程、异步收发和
   内存映射需要按平台拆分。只替换编译器不够。
4. **SSH/网络桥接**：Linux 侧依赖 TAP 与 PCI 通道。Windows 侧需要自己的
   数据通路和虚拟网卡/桥接方案。从另一台 Windows 机器经 Linux 主机管理，
   不等于原生 Windows PCI 支持。
5. **卡端算子**：保持卡端 Linux 路径，与主机迁移分层推进。CPU 装载和
   页表编码通过，不等于算术成功。

## 范围边界

优先恢复 Linux 现场、固定任务工具的平台边界与主机传输协议；暂不实施
卡内 Windows 系统替换。Windows 驱动开发前需要明确目标 Windows 版本、
WDK/签名与调试环境。不在 Linux 环境中声称 Windows 编译或部署通过；
具体实现应单独制定可验收范围。
