# NNP-I 1150 硬件与命令模型笔记

本文只记录高层结构与可观察行为。寄存器偏移、位域和命令二进制布局不在
本仓库范围内。

## 1. 平台组成

NNP-I 1150（代号 Spring Hill）是一块 PCIe 加速卡，板载：

- **ICE**：主机接口。主机通过 BAR0 的低区读取卡级状态，并通过
  `ICEPE`/`ICERDY` 对每个 ICE 实例做电源门控与就绪握手。每个 ICE 在
  BAR0 中有一个独立的 MMIO 窗口，加速器子系统（DELPHI、MMU HUB 等）
  只能经该窗口访问。
- **TLC**：卡上的调度控制器。固件运行在 TLC 上，负责从命令缓冲取命令、
  通过 CnC（Chip-to-Chip / 片上网络）向目标 CBB 下发配置与执行命令。
- **DELPHI**：CNN/GEMM 数据流引擎，包含 tile 阵列、IRAM/BRAM/ORAM、
  LSU 通道、credit 机制与 UOW（Unit of Work）调度器。
- **MMU / ATU**：DELPHI 的地址转换单元，页表由主机驱动发布。
- **IVP / ASIP / SLMC / DSE 等**：其他 CBB，各自有独立固件或配置面。
- **x86 子系统**：卡上另有一颗 x86 SoC（2 物理核 / 4 线程，AVX-512
  VNNI），运行 Buildroot Linux 5.7.0，rootfs 为 RAM initramfs。

## 2. 命令模型

逆向驱动源码与命令格式定义后确认：

1. 主机驱动不直接向 DELPHI 写 EXEC 命令。它把一段命令缓冲的基址和条目数
   写入 MMIO HUB，再敲 doorbell。
2. TLC 固件消费该命令缓冲，按命令类（EXEC / CONFIGURATION / CONTROL /
   BID 描述 / MID 描述 / WALK 描述 / PATCH 等）把单条命令经 CnC 发给
   目标 CBB。
3. 因此 CBB 的**配置**和**执行**都由 TLC 固件代理；主机侧寄存器
   `GENERATE_CONTROL_UCMD` 只有 4-bit opcode 空间，只能发控制类消息
   （例如 license 相关），无法构造 EXEC/CONFIG。
4. 固件本身以“嵌入命令缓冲”的形式由驱动加载：每个 section 按固定命令
   大小切分成命令条数，再走同一 doorbell 路径交给 TLC。

结论：**没有 TLC 固件，就没有厂商作业派发路径**。主机侧能做的只有
MMIO 直配 CBB。

## 3. 固件缺失的证据链

- 卡端 `/lib/firmware` 仅有 `rtl_nic` 系列，无任何 `cve_image*.bin`。
- 卡端 `/opt/intel_nnpi` 只有 SPH 工具（`shc`、`iMunch`、`sandstone`、
  `sphcs.ko`），不含固件。
- 主机侧驱动安装包、编译产物、本地归档中均无 `cve_image0_fw.bin`、
  `cve_image0_map.bin`、`cve_image1..4_fw.bin`。
- 只读探针显示 HUB 的命令缓冲基址/条目数长期为 0，doorbell 从未被提交；
  TLC 侧 host 接口（SP 寄存器、mailbox）读回全 `0xFFFFFFFF`，
  说明 TLC 未运行固件，卡端 CPU 也无法与其通信。

以上是两条独立证据：**资源不存在** 与 **通信面不可用**。

## 4. 纯 MMIO 直配的边界

在保持设备安全状态不变的前提下，实验确认了以下能力：

- ICE 电源门控可由主机完成，窗口随即变为可访问。
- DELPHI 的电源域（TNN 部分）可由主机打开，状态机在毫秒级稳定。
- DELPHI 的几何、tile 划分、缓冲区 credit、MMU 页表、AXI 通道和
  state message 均可写且回读一致。
- `delphi_start` 位能被引擎接收：DELPHI 的 IDLE 指示由 1 变 0，
  并在整个观察窗口内保持。

但以下环节始终没有发生：

- UOW 调度器未派发工作项；
- LSU 的地址与 FSM 通道保持全零，大小寄存器未被锁存；
- 已发布的 credit 未被消费；
- MMU/ATU 没有任何事务或 fault；
- 输出缓冲区未被写入。

已逐项排除的解释包括：MMU 页表/ATU 配置、时钟门控、电源域、runstall、
credit 发布与重置、state message、valid 触发方式、WORK_PARTN 的
partial/final-save 位、MS_CFG、算法/数据类型编码。结论是阻塞点位于
**TLC 固件负责的 UOW 派发环节**，其寄存器/协议在本地资料中没有任何
映射，无法安全猜测。

## 5. 读取顺序陷阱

对未实现 CBB 区域做连续整页读取，会使同一进程中后续对 ICE 窗口的读
返回全 `0xFFFFFFFF`；单次读未映射地址不会立即污染。因此所有探针必须
保持“先读已实现窗口（DELPHI/HUB），后读未实现区域（TLC）”的顺序。
该现象在文档中单独记录，避免被误判为设备掉线。

## 6. 设备安全状态

每轮实验结束后设备均恢复到初始状态：ICE 电源门控关闭、PCI 命令寄存器
回到仅 MEM 访问、无 DMA 所有权变更、无中断挂起。未进行固件刷写、
未修改恢复镜像、未更换硬件试错。
