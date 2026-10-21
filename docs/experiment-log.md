# 实验记录

按时间顺序记录方法与结果，重点保留**否定结论**，避免后来者重复同样的
弯路。寄存器名称用于说明变量，不附带偏移或位域。

## 1. 环境建立

- 通过 PCI 配置空间启用 MEM 访问，使用 `devmem` 读取 BAR0/BAR2。
- 确认 ICE 未上电时窗口读回 `0xFFFFFFFF`；按 `ICEPE |= 1<<(ice+4)`、
  轮询 `ICERDY` 的协议上电后窗口可访问。
- 每轮结束写回 `ICEPE=0` 并把 PCI command 恢复为 `0x0000`。

## 2. MMU 与页表

- 逐字段核对 L1 索引/移位、PDE/PTE 位、L2 shift、ATU 基址与步长，
  与驱动 `lin_mm_mmu.c` 完全一致。
- 修复一处 harness 缺陷：32KB 页的 page-size 编码在扩展 VA 模式下应为 0，
  原值非法；该缺陷不解释零事务（计数器与 fault 全零说明请求未到 ATU）。
- 结论：页表本身不是阻塞点。

## 3. DELPHI 直配

### 3.1 配置写入

- 生成器输出 35 条配置写（几何、tile、buffer、credit、OFM 等），
  逐条回读一致；按实测复位快照恢复亦一致。写入对 CPU 立即可见。
- 发现 `*_CREDIT_GRANNULARITY` 无复位默认值、无权威换算公式，
  厂商固件必然编程但本地无来源，因此不猜写。

### 3.2 电源与 runstall

- 确认 TNN 电源门控位可由 CPU 写入，状态机在 1 ms 内稳定，无错误、
  无中断；此前 START 零活动的根因是电源域关闭叠加核心 runstall。
- 打开电源并释放 runstall 后，START 仍然零活动。

### 3.3 credit 与 state message

- 发布 weights/IFM/output credit 并回读确认；无任何增量。
- 写 AXI enable 绕过 credit 等待；无增量。
- 在 START 前后分别重发 state message、重置 credit；结果与基线逐项一致。

### 3.4 START 与 valid 触发

- 多种 START 组合（不同 WORK_PARTN、部分/final save 位）均被引擎接收：
  IDLE 指示翻转并持续，但 LSU 通道保持全零。
- START 后写三通道 `addr1_valid`（含 0→1 重触发）不产生任何 AXI/MMU
  流量；LSU 调试状态寄存器全程冻结。
- 结论：阻塞点在 DELPHI 内部的配置/状态机交接，不在 MMU、credit 或
  触发方式。

### 3.5 固定状态快照

START 后整页差分只有 8~12 个寄存器变化，且每次运行数值完全相同：
`CORE_STATUS_8/11/12/13/14` 等为固定值，不是随机噪声。字段定义在本地
资料中缺失，无法解码；该快照可作为“引擎已进入但未派发”的判据。

## 4. TLC 探测

- 只读探针显示 TLC 与 TLC_HI 区域读回全 `0xFFFFFFFF`；HUB 的命令缓冲
  基址/条目数/doorbell 全为 0。
- 复核地址映射：TLC 实际映射在 BAR0 低区，ICE 窗口内不可达；
  DELPHI 只在 ICE 窗口内，BAR0 低区不可达。此前把两者放在同一窗口的
  假设被否定。
- TLC 低区只有少量只读状态字；host 接口的 SP 寄存器与 mailbox 全为
  `0xFFFFFFFF`，写后仍读回原值或 0，确认不可通信。
- 卡端 CPU 无法通过 SP/mailbox 与 TLC 对话，与“无固件”构成两条独立
  证据。

## 5. 读取顺序陷阱

- 先连续整页读取未实现的 TLC 区域，再读 DELPHI，会得到全 `0xFFFFFFFF`，
  看似设备掉线。
- 单次读未映射地址不会立即污染；连续整页读会。
- 修正：探针顺序固定为“先 DELPHI/HUB，后 TLC”，并在每次关键读前后
  读取 HUB 健康寄存器做差分。

## 6. 结论与转向

- 加速器运算路径的唯一缺口是 TLC 固件（`cve_image0_fw.bin`，可能还需要
  `cve_image1..4_fw.bin`）。该资源在卡端与主机侧均不存在，属于外部资源
  缺口。
- 在不获取固件的前提下，纯 MMIO 直配的上限是：引擎离开 IDLE，但 UOW
  永不派发。
- 因此转向卡上 x86 SoC：部署 PyTorch CPU 运行时，获得可用的推理能力。
  见 [pytorch-cpu.md](pytorch-cpu.md)。

## 7. 未解决项

- `CREDIT_GRANNULARITY` 与 `ENTRY_SIZE` 的换算关系；
- final-save 公式从 FP16 到 FP32 的外推；
- 64B block 内 FP32 的排列；
- 首个 UOW 的 WORK_PARTN 位语义；
- TLC 固件的 UOW 派发协议（本地无任何映射）。

以上项目在获得权威资料（固件或供应商文档）之前不做猜测性写入。

## 8. 主机侧存储路径跑分（2026-10-19）

目标不是测内存，而是先把“CrystalDiskMark 能对加速器存储路径打分”这件事
做成可复用的装置。后端先放主机 RAM；等卡端 MEM 通道可用，只换后端，
盘符、卷和 CDM 自动化都不动。

拓扑：用户态 C# iSCSI target（RAM LUN，512 MiB/路）→ Windows 原生
iSCSI Initiator → 四块盘转动态盘 → 2 GiB 条带卷 `G:`（`NNPI4WAY`）。
四路各绑一个回环地址（`127.0.0.1`..`.4`），IQN 分别用 `ram9`、`ram11`、
`ram12`、`ram13`，共用 3260。CDM 9.0.3，测试规模 256 MiB。

CDM 实测（MB/s）：

| 项目 | 单会话 | 四路条带 |
| --- | --- | --- |
| SEQ1M Q8T1 读 | 3412.69 | 4645.99 |
| SEQ1M Q8T1 写 | 3059.09 | 3771.37 |
| SEQ1M Q1T1 读 | 3184.52 | 4226.78 |
| SEQ1M Q1T1 写 | 2882.09 | 3328.51 |
| RND4K Q32T1 读 | 155.30 | 285.51 |
| RND4K Q32T1 写 | 145.72 | 245.51 |
| RND4K Q1T1 读 | 131.10 | 122.21 |
| RND4K Q1T1 写 | 116.39 | 106.13 |

DiskSpd 交叉验证（4K QD32，`-Sh` 无缓存）：写 59,249 IOPS /
231.44 MiB/s，读 69,742 IOPS / 272.43 MiB/s；1M 顺序 QD8 读 4329、
写 3629 MiB/s。顺序读写已经超过 Gen3 x4 的 3.94 GB/s。

瓶颈不在 target。四路 RND4K QD32 压测采样 10 秒：DiskSpd 约 97% 单核，
四个 target 各约 32%。CDM 的 RND4K 固定单线程、队列深度 32，先把客户端
吃满，再加盘没有收益；扩到六路后 4K 读 69,789 IOPS、顺序读 4345 MiB/s，
与四路一致。Q1T1 反而略降，因为单队列没有并行度可分，条带只多一层定位
开销。

协议与 Windows 侧的 14 个坑、target 的热路径优化和回退方案都记在
[experiments/iscsi-ram-lun/README.md](../experiments/iscsi-ram-lun/README.md)，
这里不重复。目录里同时保留了可以直接点 CDM 的 `run-test.ps1`、
`results.ps1` 和四路 `diskpart-4way.txt`。

当前 `127.0.0.5`/`.6` 上还留着两个已注销失败的会话（`ram14`/`ram15`），
设备已被 `LogoutTarget` 拒绝，不影响 `G:`；保留进程只为避开重连风暴。
换卡侧后端需要新的卡端 MEM_READ/MEM_WRITE verb、主机侧通道和 IOCTL，
按轮次单独授权；可行性分层见 [storage-mapping.md](storage-mapping.md)。

## 9. 卡端启动通用 Linux（2026-10-19）

之前对卡端镜像的判断是：它就是一个普通的 Linux 载荷，启动代理只认 181
块 DMA 和 SEAL 头。这一轮把判断变成了实测。

镜像用 Alpine minirootfs 加厂商内核树编译的 5.7.0 内核：188,744,192
字节，MBR + 单个 0xEF 分区，FAT16，`bzImage` 与
`EFI/BOOT/bootx64.efi` 内容相同。initramfs 里保留 Alpine 的 busybox
用户空间和 `sphcs.ko`，init 只做挂载、`insmod` 和等待。

驱动 0.3.0.21 的允许列表同时保留厂商镜像和这个通用镜像的 SHA-256。
`boot-load` 送完 181 块，`boot-submit` 后门铃从 1 到 10、3、4，
十秒内 `phase=9 driver_ready=1`。`net-begin --card-ip 192.168.115.2
--mac 02:4e:4e:50:01:02` 建好通道，主机收到 ETH_CONFIG 回显；
`nnpi-net-bridge` 接上 TAP 后 ping 6/6，TTL 64。

两处必须一起改，否则会误判：

1. `nnpi_image_identity.h` 同时被驱动和 `nnpi-info` 使用。只重编 SYS
   时，`nnpi-info boot-load` 会在本地校验就拒绝新镜像，返回
   `chunks_sent: 0`，设备状态一点没动。
2. 只发 `net-begin` 不够。TAP 网卡和 `192.168.115.1` 地址一直都在，
   但没有桥接进程，ARP 出不了主机。表现为 ping 100% 丢失，而
   `net-query` 一侧 `chan_created=1`、`eth_configured=1` 全部正常。

这个镜像没有 sshd/telnetd，根文件系统仍在 initramfs 里。要拿到可登录的
系统，下一步得做磁盘根和远程登录，这也正好是驱动需要重构的部分：
卡端要能读主机磁盘或 VHDX。

## 10. 卡端持久化：TCP NBD 与写回修正（2026-10-22）

卡端 root 在 RAM，之前用 raw 隧道挂 nbd0，切到网络后 SSH 和存储互斥。
这一轮把存储挪到 net-bridge 上：主机 `nbd-server.py --proto newstyle`
监听 10810，后端 `\\.\PHYSICALDRIVE4`（16 GiB VHDX），卡端 nbd-client 3.20
以 `-n` 连 `/dev/nbd1`，挂到 `/mnt/vhdx`。SSH、命令邮箱和存储同时可用。

协议和运维细节见 [storage-tcp-nbd.md](storage-tcp-nbd.md)，脚本见
`card-runtime/storage/`。

实测：64 MiB 冷读 26.46 秒（约 2.5 MB/s）；邮箱经同一设备返回 RC=0；
RAM 根的增量写回 23.7 秒。

这一轮还定位了一个“数据丢失”误报。写回脚本原来是
`rsync -aHX --delete / /mnt/vhdx/`：VHDX 同时是卡侧存储，`--delete` 会删掉
只存在于挂载点的文件；而全根扫描（含 /usr/share/gnome 等数万文件）在
2.5 MB/s 下超过 5 分钟周期，常在走到 /usr/local 前被切链路打断，打补丁的
脚本只留在 RAM。去掉 `--delete`、排除静态树和缓存、加 flock、传播 rsync
退出码、周期改为 3 min/15 min 后，单次 24 秒；VHDX-only 文件存活、RAM
文件被镜像，三个脚本在两侧的 sha256 一致。

待验证：下一次卡端重启后，新脚本和 15 分钟定时器是否自动生效。
