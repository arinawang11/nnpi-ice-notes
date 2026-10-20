# NNP-I 1150 折腾笔记

*[English](README.md)*

这块 Intel NNP-I 1150（Spring Hill）前后折腾了一段时间，这里是把过程和
能用的东西整理出来的结果。卡是一块 PCIe 加速卡，板上带一颗小的 x86 SoC。
主机通过 ICE 接口跟它通信，推理引擎叫 DELPHI。

结论先放在前面：卡能上电，主机接口能通，加速器的配置能读，卡上的 x86
也能启动 Linux。但 DELPHI 始终没有跑起来。负责给它派发任务的 TLC 固件
（`cve_image*.bin`）在我手上的镜像、驱动包和归档里都找不到。x86 那边的
PyTorch 倒是能正常用，而且速度不算差。

## 驱动发布状态

社区驱动正在准备第一个正式版本（**NNPI Community Driver 1.0.0**，当前开发版
0.3.0.34）：

- 宿主驱动：Windows Server 2025 / Windows 11 24H2，x64，测试签名；
- 设备类别：`Coprocessor`（项目自有 class GUID）；
- 卡端 `sphcs` 模块（GPL-2.0-or-later），支持 vendor 与通用内核的树外构建；
- `image-builder/`：把用户提供的通用内核构建成卡端启动镜像。

安装、安全、发布计划与卡端根因见 `windows-driver/README.md` 和
`docs/release/`。

**硬件验证（2026-10-20）。** 用本地构建的镜像（vendor 5.7 内核 + 打补丁的
`sphcs` + Alpine initramfs；验证用的是这份代码的测试构建 0.3.0.44）在卡上
启动，通过 raw tunnel 连续读取 15 GiB NBD 数据：`rc=0`，盘尾
`NNPISTRESS DONE` 标记写入成功，卡全程停在 phase 9，没有复位或重启，宿主
没有 0x80 NMI、没有 WHEA、没有 0x9F（约 10.2 MB/s，约 24 分钟）。这越过了
之前 7.3 GB 的必崩点。1.0 剩下的阻塞项是启动超时后的 teardown 路径：600 秒
`WAIT_DRIVER` 超时后，当前的有界等待仍可能在卡还活跃时释放 DMA，已经因此
出现过 0x80 NMI。启动未决时设备应保持 fault，不要释放 DMA。

## 硬件情况

卡上有两个 PCI function。`8086:45c4` 是卡侧的 ICE 接口，`8086:45c6` 是
Windows 主机绑定的那个。每个 ICE 实例有自己的一段 MMIO，访问之前要先做
上电和 ready 握手。

卡上是一颗双核四线程的 x86 SoC，带 AVX-512 VNNI，跑的是 Buildroot 做的
Linux 5.7，根文件系统在内存里，通过 PCIe 加载镜像启动。DELPHI 就在旁边，
同一片区域里还有 MMU、DSE、IVP、ASIP 这些模块。卡起来之后，它们都能从
ICE 的窗口访问到。

## 查到的东西

主机驱动不会直接把任务命令写进 DELPHI。它的做法是把命令缓冲区的地址和
条目数写进一个 MMIO hub，然后敲一下 doorbell。TLC 固件把缓冲区取走，
再通过片上网络向目标模块发 config 和 exec 命令。主机这边能用的消息通道
只有一个带 4 位 opcode 的控制消息寄存器，所以从 CPU 侧伪造一条 exec
命令是走不通的。

没有 TLC 固件，这条路就断了。我后来试过直接配 MMIO：上电、MMU 页表、
geometry、credit、状态消息、start。引擎会接受 start，idle 标志也会翻，
但 UOW 调度器不派发任何东西。LSU 通道一直是空的，size 寄存器没有锁存，
总线上也看不到 AXI 或 MMU 流量。runstall 释放、credit 重置、状态消息
重发、重新触发 valid 位这些可能都逐一排除过了。实验过程和否定结果都在
`docs/experiment-log.md` 里。

加速器的工作就停在这里。如果手上有固件，情况会不一样；没有的话，我不认为
靠猜能把派发协议补出来。

## 启动链

这张卡从上电到能 SSH，跑起来的是三条链：主机 PCIe 枚举和驱动绑定、卡侧
启动代理通过 DMA 加载镜像并启动 x86、以及主机驱动和卡上 Linux 之间的
运行时与网络通道。DELPHI 的计算链是第四条，需要 TLC 固件，我手上没有，
所以没有走通。x86 能启动不代表加速器可用，这两件事分开看。

文中的状态值、超时和消息格式来自驱动代码和实际运行记录。卡侧启动代理
（协议里叫 BIOS）没有源码或固件，也没有反汇编，所以关于它内部行为的
描述只到协议和状态迁移能够支持的范围。

### 1. 主机和卡怎么连

```
Windows 主机
  └─ PCIe function 45c6
       ├─ 控制窗口：命令 FIFO、响应 FIFO、门铃状态
       ├─ host-ready 窗口
       ├─ 运行时响应窗口
       └─ SYSINFO 推送窗口
            │
            │ PCIe / DMA
            │
NNP-I 卡
  ├─ 启动代理（协议中称 BIOS）
  │    ├─ 响应 SYSINFO
  │    └─ 按描述符表读取启动镜像
  ├─ x86 SoC，2 核 4 线程
  │    └─ Linux 5.7 + Buildroot initramfs
  │         ├─ 卡端网络驱动
  │         └─ PyTorch CPU 运行时
  └─ DELPHI / TLC / MMU / IVP / ASIP
       └─ TLC 固件缺失
```

Windows 驱动只绑定 `45c6`。`45c4` 是卡侧 ICE 接口，不归这个驱动管。
驱动也不控制卡的上电，槽位供电由主机平台负责。

主机没有可以任意读写卡内存的接口。主机把 QWORD 推进命令 FIFO，最后一次
写带通知；卡把回复放进响应 FIFO，主机轮询取走并确认；门铃低字节表示卡
当前处于哪个阶段。后面的流程都建立在这个模型上。

命令 FIFO 深 16 个 QWORD。一条命令按低 32 位、高 32 位分两次写入，最后
一次带通知。响应 FIFO 的读写指针在控制字里，深度也是 16 个 QWORD；一次
读满 16 个时，先推进一次中间指针，再写最终读指针。每轮轮询先取控制字
快照，按快照把数据读完，最后确认。解析失败不会重放已经确认的内容。

### 2. 枚举和驱动绑定

卡上电后，卡侧启动代理已经在运行。主机完成 PCIe 枚举时能看到两个
function，Windows PnP 按 INF 里的硬件 ID 绑定 `45c6`。

`NnpiDeviceAdd` 复核 PnP 硬件 ID，建立设备接口和缓冲 I/O 队列，注册
PnP、电源和文件清理回调。这个阶段不访问设备寄存器。

`PrepareHardware` 才建立访问能力：

- 遍历 translated resource，记录内存、端口、中断和 device-private 资源；
  遇到 `MemoryLarge` 直接拒绝，不猜联合体布局；
- 通过 `BUS_INTERFACE_STANDARD.GetBusData` 读 64 字节 PCI 配置空间；
- 用配置空间里的 BAR 信息核对窗口地址，不假设 BAR 编号；
- 映射状态、控制快照、host-ready、运行时响应和 SYSINFO 推送五个窗口；
- 映射中断/状态窗口，把中断屏蔽，运行时用轮询；
- 准备 common-buffer DMA，查询 FLR 能力并取得 reset interface；
- generation 加一。generation 对应一次 `PrepareHardware` 实例；epoch 在
  D0 退出或设备失效时递增。两者一起用来拒绝过期的用户态请求。

这个阶段只读 PCI 配置，不写设备寄存器。某个窗口映射失败只关闭对应能力；
`boot-submit` 会检查 host-ready、控制快照、SYSINFO、运行时和 FLR 是否
都在，缺一个就不启动。

主机刚重启时，卡的 BAR 可能在一段时间内全读 `0xFFFFFFFF`，因为卡侧逻辑
还没起来。boot worker 在读取正式基线之前先等，最多 600 次、每次 500 ms，
四个基线都不再是全 1 才继续。等完还是全 1 就按失败处理，不去改写它。

### 3. 镜像装载

启动镜像不在仓库里。驱动负责接收、校验，并把它变成卡能 DMA 读取的形式。
`boot-load` 内部依次做 BEGIN、CHUNK、SEAL。

#### 3.1 BEGIN

`boot-query` 返回当前 generation 和 epoch。`boot-load` 带上这两个值，
驱动确认设备在 D0 且两者匹配，然后生成随机的 session id，分配 181 个
DMA common buffer 作为镜像块，再分配一个 4 KiB 表缓冲，状态从 EMPTY
进入 LOADING。

镜像长度固定为 188,744,192 字节：180 个 1 MiB 块，最后一块 512 字节。
块数在编译期断言为 181。

#### 3.2 CHUNK

用户态按顺序送数据。驱动要求 offset 严格等于已写入字节数，不允许跳块、
覆盖或乱序。每块是独立的 DMA 映射；最后一块只有 512 字节，映射仍覆盖
整页，尾部补零。

这一步只写主机内存，没有 MMIO，也没有把地址交给卡。

#### 3.3 SEAL

字节到齐后：

1. 检查镜像开头是否为 EFI capsule 的 GUID。是就拒绝。检查放在长度校验
   之后、哈希之前，capsule 谎报大小也拦得住。
2. 按顺序对 181 个块算 SHA-256，和驱动里编译好的期望值比较。不一致就
   停在 SEALED 之前，不能提交。
3. 构造一张连续描述符表：24 字节表头，后接 181 个 8 字节条目。条目里是
   每个块的 DMA 页帧号和页数。表和块都是同一设备的 DMA common buffer。

表必须是一块连续映射，不能用分页链表。SEAL 时驱动把每块的填充区清零，
再把整张表清零后重写，避免卡读到主机内存里的残留数据。

SEAL 成功后的状态是 SEALED，此时仍未访问设备。

#### 3.4 SUBMIT

`boot-submit` 要求状态为 SEALED，181 个块和表都还在，SYSINFO DMA 页
已准备好，host-ready、控制快照、SYSINFO 和运行时窗口都已映射，reset
interface 存在且支持 function-level reset，并且没有其他 boot worker 在跑。

条件满足后，驱动分配 work item、排队 boot worker，然后返回。硬件操作都在
worker 里，用户态不会阻塞十分钟。

### 4. 基线、host-ready 和 BIOS_READY

worker 先确认 session 仍是当前这个，再读四个基线：host-ready 当前值、
命令 FIFO 控制字、响应 FIFO 控制字、门铃状态。

基线要求 host-ready 为 0，门铃低字节为 0 或 1，两个 FIFO 都为空。任何
一项不符，或者读到全 1，都在写 host-ready 和启动 DMA 之前失败。

基线通过后，驱动向 host-ready 写固定的 9。这是 boot 阶段的 driver-ready
动作，不是可以随意填写的寄存器。随后轮询门铃低字节，等它变成 1
（BIOS_READY），最多 20 次、每次 500 ms，即 10 秒。中间读到 0 是正常
等待，读到其他值就中止。

这台卡上，主机第一次读门铃得到 0 或 1，写 9 之后进入 1。卡侧启动代理在
驱动绑定之前就已经能响应。

### 5. SYSINFO

看到 1 之后，驱动要求命令 FIFO 为空，然后发一条 3 QWORD 的 SYSINFO
请求，通过推送窗口做 6 次半字写，最后一次带通知：

- QWORD 0：opcode 31，类型 `0x11`，payload 长度 16；
- QWORD 1：4 KiB DMA 页地址；
- QWORD 2：页长度 4096。

卡侧启动代理把信息写进这块 DMA 页。驱动轮询门铃，等低字节变成 10，
上限同样是 20 次、每次 500 ms。看到 10 后解析 123 字节 payload，检查
版本、CPU 字符串、ICE 信息、UTF-16 BIOS 字符串、CSME 和 PMC 字段。
解析不通过就中止，不提交镜像。

SYSINFO 成功是发送镜像描述符的前置条件。

### 6. BOOT_IMAGE_READY

SYSINFO 成功后，驱动先把镜像和描述符表标记为已发布。从这一刻起它们归
DMA 所有，错误路径不能直接释放。然后发送 BOOT_IMAGE_READY：

- opcode 31，类型 `0x10`，payload 长度 16；
- QWORD 1 指向表头之后的第一个条目；
- QWORD 2 低 32 位是描述符字节数，高 32 位是镜像总字节数。

消息本身 24 字节，数据在描述符表指向的 181 个块里。卡侧启动代理按表
逐块 DMA 读取，然后启动 x86 核心。

驱动随后观察门铃：

- 3 表示 BOOT_STARTED，镜像传输开始；
- 4 或 8 表示 DRIVER_READY，卡侧运行时已启动；
- 3 之前读到 10 仍算等待；
- 读到过 3 之后再读到 10，按协议错误处理；
- 上限 1200 次、每次 500 ms，即 10 分钟。

3 只表示传输开始，不表示 DMA 完成。这段时间驱动保持镜像、表和所有映射
有效，不释放、不重用。要等到 4/8，或者走完后面带证据的复位流程，才能
谈释放。

镜像里是 Linux 5.7 内核和 Buildroot initramfs。x86 启动后卡端网络驱动
配置网卡，主机才能建立通道。rootfs 在内存里，重启后需要重新加载镜像，
运行时的修改也会丢失。

卡端 Linux 启动后的画面可以通过 x11vnc 看到。neofetch 显示 Buildroot
2019.11、内核 5.7.0、4 个线程、15,370 MiB 内存；CPU-X 显示核心频率
495 MHz，每核 48 KiB L1D、32 KiB L1I、512 KiB L2，共享 24 MiB L3。
截图和频率说明在[卡端运行时](docs/card-runtime.md#6-实际运行画面)。

![卡端 neofetch 与 CPU-X](docs/images/card-neofetch-cpux.png)

### 7. 运行时握手

门铃到 4 或 8 后，boot worker 不退出，转入运行时循环。进入前检查命令
FIFO 为空、门铃低字节为 4 或 8、门铃错误标志为 0。

然后发 QUERY_VERSION。这条命令只有一个全零 QWORD，走命令 FIFO。卡回
一条 3 QWORD 的版本消息，opcode 2：

- 协议版本 4.1.0（`0x1020`）、固件版本、通道版本 1.6.0（`0x04c0`）；
- QWORD 1 是每个通道 opcode 的响应长度表；
- QWORD 2 是对应的命令长度表。

驱动只接受 opcode 2、3 个 QWORD、major/minor 匹配、响应长度表非零的
回复；patch 不参与比较。版本通过后，驱动把 host-ready 里的 driver-ready
字段置 1（值 `0x100`），其他位保持不变。此时 `RuntimeCompatible` 为真。

之后以 5 ms 为间隔轮询：读门铃并检查错误标志，从响应 FIFO 一次最多取
16 个 QWORD 并确认，逐条解析并路由到版本、事件、以太网配置、以太网
描述符或 SYSINFO，更新 keepalive；网络建立后还要泵送 TX/RX 描述符。

keepalive 是计数字段，循环 1 到 14。0 是初值，不发；15 保留不用。每轮
更新它，其他位保持不变。

运行时遇到未知 opcode、只收到一半的消息、FIFO 读回全 1 或门铃错误标志
非零，会终止并保留现场。已经发出的版本查询不重发，已经确认的 FIFO 内容
不重放。

### 8. 网络通道

网络不是在内核里注册一个网卡就结束，它要在现有 IPC 上建一个非上下文
通道。`net-begin` 的前置条件：

- boot worker 还在，阶段是 DRIVER_READY；
- 运行时兼容且健康；
- 通道版本 1.6.0，长度表匹配；
- generation/epoch 是当前值；
- 提供合法的单播 MAC 和非零卡端 IP；
- 通道号从 256 开始，0 到 255 留给推理上下文。

建立顺序固定，每一步等对应事件：

1. `CHANNEL_OP`（opcode 22）创建通道，等事件 21（创建成功）。
2. `CHANNEL_RB_OP`（opcode 23，H2C）发布主机到卡的 ring，等事件 23。
   发布的是 ring 链头页的页帧号，不是第一块数据页的地址。
3. 同样的 `CHANNEL_RB_OP` 发布 C2H ring，再等事件 23。
4. `ETH_CONFIG`（opcode 57）发 2 个 QWORD：卡端 IP 和 MAC。卡回
   opcode 39 回显 IP，回显 0 表示拒绝。
5. 进入 READY。

每一步的观察上限是 120 次、每次 5 ms，即 600 ms。出现 112 及以上的
卡致命事件就终止。

每个方向有 16 个 4 KiB 数据页和一个 4 KiB 链页。链页以 page-block 形式
描述这 16 个数据页；卡只拿到链页的页帧号，数据页通过链页间接描述。
这样主机管理自己的页池时，不需要修改已经发布的地址。

### 9. 数据面

帧通过描述符传递，不复制进控制 FIFO。

发送时，桥接程序把一帧交给驱动，驱动找空闲 slot，把帧写进对应的 4 KiB
页，然后发 H2C 描述符（opcode 58），带上 slot 和长度。卡消费后回 C2H
描述符（opcode 40），ACK 位置位、长度为 0，驱动收到后才把 slot 标回
空闲。发送接口返回“已入队”，不代表卡已经发出，更不代表对端收到。

接收时，卡把帧写进 C2H 页并回描述符 40。驱动校验 slot 和长度，把帧
复制到深度为 4 的 CPU 队列，然后回 ACK 描述符，把页还给卡。桥接程序
通过 RX claim/batch 取走帧，写进 TAP 适配器。队列满时驱动仍然 ACK 并
计入丢包，不会一直占着页。

完成事件丢失按丢包处理，不重放。重放会让同一帧进入网络两次，比丢一帧
更难处理。驱动和桥接程序在这一点上一致。

### 10. 拆除、失败和 FLR

正常关闭、PnP 停止、D0 退出和设备移除走同一套顺序：

1. 停掉网络和 boot 两个生产者：清除运行时健康标志，置 closing，唤醒
   worker，等待 `NetDone`；
2. 置 `ResetBlocked`，让旧的 MMIO 路径失效；
3. 失效中断策略，不再访问设备；
4. 如果 host-ready 写过、没有保留运行时故障、本次 session 尚未用过复位，
   且 reset interface 可用，执行一次 FLR；
5. FLR 使用 `FunctionLevelDeviceReset`、flags 0、无回调、参数 NULL；
   调用返回后不再访问设备；
6. 只有 FLR 返回 `STATUS_SUCCESS`，才把已发布的 DMA 标记为停止，之后
   才允许释放镜像、表和网络页池。

FLR 失败或没有执行时，已经发布给 DMA 的缓冲区不会释放，
`NnpiBootBarrier` 每 5 ms 检查一次停止证据，没有证据就不返回。卡可能
还在 DMA 读这些页，提前释放会让它写到已经属于别人的内存。还没发布的
缓冲区不受这条约束。

运行时出现解析错误、MMIO 异常或部分发布时，驱动保留故障，不自动 FLR。
需要人工决定是重新加载镜像，还是先做一次干净的电源周期。超时只是本次
操作失败，不能当作 DMA 已经停止。

### 11. 各阶段驱动做什么

| 阶段 | 驱动动作 | 失败时 |
| --- | --- | --- |
| 枚举/绑定 | 复核硬件 ID，建设备接口 | 不加载 |
| PrepareHardware | 读配置空间，映射窗口，准备 DMA 和 FLR | 关闭对应能力，提交被拒 |
| 镜像装载 | 181 个 DMA 缓冲，顺序写入 | 停在 LOADING，可重新 BEGIN |
| SEAL | 拒绝 capsule，SHA-256，建描述符表 | 不允许提交 |
| SUBMIT | 检查前置条件，排队 worker | 返回 not ready，不碰设备 |
| 基线 | 全 1 检查，FIFO 为空，host-ready=0 | 在 DMA 之前失败 |
| BIOS 等待 | 写 9，轮询门铃到 1 | 10 秒超时或协议错误 |
| SYSINFO | 发 3 QW，解析 123 字节 | 中止，不提交镜像 |
| 镜像提交 | 发布 DMA，发描述符，等 3/4/8 | 保留缓冲区，进入清理 |
| 运行时 | 版本握手，host-ready，keepalive，路由 | 终止并保留现场 |
| 网络 | 建通道，发 ring，ETH_CONFIG | 只让网络失败，boot 继续 |
| 拆除 | 停生产者，单次 FLR，等停止证据 | 保留缓冲区，barrier 不返回 |

网络失败的处理范围是：通道建立失败、事件超时、描述符错误只把网络状态
置为失败，boot 运行时继续跑。网络是可选的，TAP 或用户态桥接的问题不
应该拆掉整个卡端运行时。但如果失败发生在 MMIO 访问上，boot 也会关闭，
因为此时无法保证后续读取还有意义。

### 12. 计算链为什么不在这里

上面加载的是卡上 x86 的 Linux，不是 TLC 固件。DELPHI 工作需要 TLC 先
运行，由它接收主机命令，再通过片上网络给 DELPHI 发 config 和 exec。
主机侧的控制消息只有 4 位 opcode，没有直接写 EXEC 的路径。

我试过绕过 TLC 直接配 MMIO：上电、MMU 页表、geometry、credit、状态
消息、start 都做了。DELPHI 接受 start，idle 标志翻转，但 UOW 调度器不
派发，LSU 全通道为零，size 不锁存，总线上没有 AXI 或 MMU 流量。
runstall、credit、状态消息、valid 位这些可能也逐一排除了。过程记录在
[`docs/experiment-log.md`](docs/experiment-log.md)。

所以“启动链走完”只说明 x86 侧可用。加速器计算链缺的是 TLC 固件，不是
主机驱动里再补一段代码。

### 13. 复现顺序

```text
nnpi-info boot-query --index 0
nnpi-info boot-load --index 0 --generation G --epoch E --image PATH
nnpi-info boot-submit --index 0 --session S --generation G --epoch E
nnpi-info runtime-info --index 0
nnpi-info net-begin --index 0 --generation G --epoch E \
          --card-ip 192.168.115.2 --mac 02:00:00:00:00:02
nnpi-net-bridge.exe --device-instance 'PCI\VEN_8086&DEV_45C6&SUBSYS_...' \
          --tap-guid '{...}' --session S --generation G --epoch E
```

`boot-load` 包含 BEGIN、CHUNK 和 SEAL；`boot-submit` 启动 worker。
`runtime-info` 用来确认版本兼容和网络 gate 是否打开。桥接程序要和驱动
使用同一个 session。

镜像、固件和卡端配置都不在仓库里。没有镜像时，驱动仍可编译、绑定和做
只读查询，但启动链走到 SUBMIT 就停。

### 14. 证据边界

文中的数值来自驱动的协议头、状态机和运行记录：镜像长度、块数、SHA-256
期望值、门铃状态 0/1/3/4/8/10、20/20/1200 次（每次 500 ms）的 boot
超时、5 ms 运行时轮询、keepalive 1..14、网络 opcode 22/23/57 和事件
21/23/39。

卡侧启动代理的实现没有拿到；卡端 Linux 只有运行观察，没有源码分析。
文中说“卡按表读取”“卡端网络驱动配置网卡”，依据是协议语义、状态迁移和
启动后的实际行为，不是对卡侧代码的逐行分析。

寄存器偏移、位域表、厂商源码、固件、启动镜像和私钥都没有放进仓库。
这份说明解释启动链的阶段和驱动职责，不是带地址的操作手册。

## 启动一个通用 Linux

卡侧启动代理不关心镜像里装的是什么。它按 DMA 读 181 块，校验主机写进去的
SEAL 头，然后跳到入口。厂商镜像里碰巧是 Buildroot initramfs 和 Intel 的
工具，那只是载荷，不是启动条件。

为了验证这一点，我用 Alpine minirootfs 和厂商内核树编出来的同一个 5.7.0
内核做了一个镜像。大小还是 188,744,192 字节：MBR、一个 0xEF 分区、FAT16，
`bzImage` 和 `EFI/BOOT/bootx64.efi` 是同一个文件。initramfs 里是原样的
Alpine 用户空间，加上从卡端镜像拿来的 `sphcs.ko`。它的 init 挂 proc、sys、
dev，加载模块，然后等待。

驱动同时放行两个镜像的 SHA-256，实际跑法是：

```
nnpi-info boot-load   --index 0 --generation 1 --epoch 1 --image generic-alpine.img
nnpi-info boot-submit --index 0 --session <id> --generation 1 --epoch 1
nnpi-info boot-query  --index 0
```

门铃在十秒左右从 1 走到 10、3、4，`boot-query` 报 `phase=9
driver_ready=1`。接着 `net-begin` 建好以太网通道，卡端回了 ETH_CONFIG。
在 TAP 网卡上起 `nnpi-net-bridge` 之后，`ping 192.168.115.2` 6/6 通，
TTL 64。

有两个坑花了不少时间：

- 白名单不只编进驱动，也编进了用户态工具。只重编 SYS 的话，`boot-load`
  会在本地就把新镜像拒掉，根本还没碰设备（`chunks_sent: 0`）。
- 桥接程序没起来时 ping 是不通的。TAP 网卡和地址照样在，`ipconfig`
  看着一切正常，但帧根本没到卡上。

这说明启动路径与载荷无关，普通发行版内核也能在卡上跑起来。现在还不是一个
可登录的系统：这个镜像没有 sshd 或 telnetd，而且仍然整个跑在 initramfs 里。
下一步是做磁盘根文件系统和远程登录。

## 仓库里有什么

`windows-driver/` 是我自己写的 KMDF 驱动，针对 `45c6` 这个 function，
另外还有一个小的命令行工具和一个 TAP 桥接程序。驱动负责设备信息查询、
加载卡端镜像的 boot 协议，以及以太网的配置和数据通道。里面没有厂商的
源码或二进制。

`docs/` 是笔记：架构、实验记录、固件核查、主机环境、卡端运行环境、
排错，还有主机侧迁到 Windows 需要做哪些事。[卡端访问主机磁盘的可行性](docs/storage-mapping.md)
单独写了一篇，分层说明块级服务、钉页窗口和直接访问内存这三条路各自能
走到哪。

`experiments/pytorch-cpu/` 是卡端 CPU 运行时的部署脚本。
`experiments/iscsi-ram-lun/` 是用 CrystalDiskMark 给主机存储路径跑分的
iSCSI RAM LUN。

## 存储路径跑分

CrystalDiskMark 只认普通块设备，所以把 PCIe function 藏在一个小的 iSCSI
target 后面：四个 RAM LUN、四条 Windows 会话、一个动态盘条带卷（`G:`，
`NNPI4WAY`）。CDM 9.0.3，256 MiB：

| 项目 | 单会话 | 四路条带 |
| --- | --- | --- |
| SEQ1M Q8T1 读 | 3412.69 MB/s | 4645.99 MB/s |
| SEQ1M Q8T1 写 | 3059.09 MB/s | 3771.37 MB/s |
| SEQ1M Q1T1 读 | 3184.52 MB/s | 4226.78 MB/s |
| SEQ1M Q1T1 写 | 2882.09 MB/s | 3328.51 MB/s |
| RND4K Q32T1 读 | 155.30 MB/s | 285.51 MB/s |
| RND4K Q32T1 写 | 145.72 MB/s | 245.51 MB/s |
| RND4K Q1T1 读 | 131.10 MB/s | 122.21 MB/s |
| RND4K Q1T1 写 | 116.39 MB/s | 106.13 MB/s |

顺序读写已经超过 Gen3 x4 的上限。RND4K Q32T1 接近翻倍，但瓶颈在客户端：
CDM 的随机测试只有一条线程、队列深度 32，会吃满一个核，所以扩到五路、
六路没有收益（六路 4K 读 69,789 IOPS，顺序读 4345 MiB/s）。Q1T1 不会涨，
单队列没有并行度可分。

同一块卷和脚本以后可以直接指向卡侧内存后端，只改 target 的读写路径。
协议细节和 14 个 Windows 侧的坑在 `experiments/iscsi-ram-lun/README.md`，
按时间记录在 `docs/experiment-log.md`。

## 编译驱动

需要 Visual Studio 2022 和 Windows 11 WDK，机器上得插着卡。在 x64 开发
人员命令提示符里：

```powershell
cd windows-driver
msbuild nnpi-host.sln /p:Configuration=Release /p:Platform=x64
```

工程里签名是关掉的（`SignMode=Off`）。本机测试可以开测试签名
（`bcdedit /set testsigning on`，然后重启），再用
`pnputil /add-driver ... /install` 装。这个做法会降低整机的驱动完整性
检查，所以别在日常用的机器上开。正式用的话，自己签一个证书。

INF 把访问权限限制在 SYSTEM 和 Administrators。装好之后，
`nnpi-info.exe` 提供只读查询。不带参数是最基本的设备信息，其他子命令
（`boot-query`、`net-query`、`runtime-info` 等）的用法可以直接看源码里的
usage 字符串。

## 测试

单元测试是纯 C11，不需要 WDK，也不需要插卡：

```cmd
cd windows-driver
tests\run-tests.cmd
```

一共 26 个。同样的文件在 Linux 下用 gcc 也能编：

```sh
cd windows-driver
for f in tests/test_*.c; do
    gcc -std=c11 -Wall -Wextra -Werror -I include -o /tmp/t "$f" && /tmp/t
done
```

桥接程序也有一个 mock 测试：`g++ -std=c++17 -I include -o /tmp/tb
tools/tests/test_bridge.cpp && /tmp/tb`。

## 网络和 SSH

桥接程序把驱动的以太网通道接到 TAP-Windows6 网卡上。它需要上游的
`tap-windows.h`，这个头文件没有复制进仓库：

```powershell
msbuild tools\nnpi-net-bridge.vcxproj /p:Configuration=Release /p:Platform=x64 `
        /p:NnpiTapIncludeDir=C:\path\to\tap-windows6\include
```

先建一个 TAP 网卡，记下它的 GUID，再找到卡在本机的设备实例 ID（每台
机器都不一样）。然后：

```powershell
nnpi-net-bridge.exe --device-instance 'PCI\VEN_8086&DEV_45C6&SUBSYS_...' `
  --tap-guid '{...}' --session <id> --generation <n> --epoch 1
```

session id 来自驱动的 BEGIN 响应。同一个设备不要重复起两个桥接进程；
正常退出时它会把 I/O 排空并释放占用。

卡端镜像是要自备的，仓库里没有。你需要一个能启动、并且会配置网卡的
镜像。我用的配置是主机 TAP 地址 `192.168.115.1/24`，卡端
`192.168.115.2/24`，但这只是我这边的情况。卡能响应之后，
`ssh root@192.168.115.2` 就应该能连上。更多细节在
`docs/host-setup.md`。

## 卡上的 PyTorch

卡上没有包管理器，也没法联网装东西，所以运行时是打成 tar 包传过去的。
先在 WSL 里准备一个独立的 CPython 3.8 x86_64 版本，再准备好要用的
wheel（`torch 2.4.1+cpu`、对应版本的 torchvision、numpy，可选
onnxruntime 1.16.3），放到 `/root/card-py` 下：

```
py/
wheels/
wheels3/
wheels4/        # 可选
```

然后：

```bash
export NNPI_SSH_KEY=/path/to/key
export NNPI_CARD=root@192.168.115.2
experiments/pytorch-cpu/deploy.sh
```

脚本会把所有东西流式传到 `/tmp/card-py`，离线解包，导入成功后会打印一行
版本信息。ResNet18 的结果和主机参考逐位一致。根文件系统是 initramfs，
重启会把 `/tmp/card-py` 清掉，需要重新部署一次。

## 没有放出来的东西

厂商的驱动源码和头文件、固件、启动镜像、SSH 私钥、寄存器表都没有放。
前四样要么是专有的，要么本身敏感；寄存器表是我暂时不打算公开的部分。
这也意味着光靠这个仓库没法重跑直接配 MMIO 的那些实验——笔记里写的是
试过什么、结果如何，不是一份带地址的操作步骤。

如果手上没有卡端镜像或固件，驱动的编译和只读查询仍然能用，但网络和
PyTorch 那两节的前提是你已经有一个能启动的镜像。

## 几句提醒

给内核驱动开测试签名会降低机器本身的安全性。来路不明的固件和镜像不要
加载。DMA 和复位操作要小心，动手之前先弄清楚缓冲区归谁。

代码是 GPL-2.0-only，除非文件里另有说明。这个项目和 Intel 没有关系。
请用在自己拥有的硬件上。
