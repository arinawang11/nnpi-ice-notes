# 用 iSCSI RAM LUN 给主机链路跑分

本目录用 Windows 自带的 iSCSI Initiator 挂一个用户态 target，得到普通块
设备，让 CrystalDiskMark（CDM）能直接对主机侧存储路径打分。后端现在是
主机内存，换成卡侧通道时盘符和脚本都不用改。

这里只放自己写的 target、部署脚本和 CDM 自动化。不含厂商源码、固件或
镜像。

## 文件

| 文件 | 作用 |
| --- | --- |
| `iscsi-target.cs` | 纯 C# iSCSI target，无外部依赖，RAM LUN，512 MiB/路 |
| `setup/diskpart-4way.txt` | 把四个 LUN 转动态盘并建成一条 2 GiB 条带卷 |
| `cdm/run-test.ps1` | 点 CDM 的测试按钮并等待 DiskSpd 子进程 |
| `cdm/results.ps1` | 按控件 ID 读 CDM 的四项成绩 |
| `cdm/dump.ps1` | 枚举 CDM 子控件，用于确认控件 ID 没变 |

## 前置条件

- Windows 11 / Server，Microsoft iSCSI Initiator 服务在运行；
- CDM 9.0.3 便携版，默认放在 `%TEMP%\cdm-9.0.3`，路径按需改脚本；
- target 用 .NET Framework 的 `csc.exe` 编译，不需要 Visual Studio。

## 用法

编译（先停掉正在运行的 target，否则 exe 被占用）：

```cmd
csc /nologo /target:exe /out:iscsi-target.exe iscsi-target.cs
```

单路：

```cmd
iscsi-target.exe 3260 512 iqn.2026-10.local.nnpitest:ram9
```

多路，每路绑一个回环地址：

```cmd
set NNPI_ISCSI_BIND=127.0.0.1
iscsi-target.exe 3260 512 iqn.2026-10.local.nnpitest:ram9
set NNPI_ISCSI_BIND=127.0.0.2
iscsi-target.exe 3260 512 iqn.2026-10.local.nnpitest:ram11
```

| 环境变量 | 作用 |
| --- | --- |
| `NNPI_ISCSI_BIND` | 监听地址，同时决定 SendTargets 的回连地址 |
| `NNPI_ISCSI_MAXCONN` | MC/S 上限，默认 1 |
| `NNPI_ISCSI_NAMES` | 一个进程顺带应答的额外目标名 |
| `NNPI_ISCSI_STATS` | 每 2 万 PDU 打一行统计 |
| `NNPI_ISCSI_VERBOSE` | 逐 PDU 日志，只用于调试 |

主机侧：

```cmd
iscsicli QAddTargetPortal 127.0.0.1
iscsicli QLoginTarget iqn.2026-10.local.nnpitest:ram9
```

新盘默认 offline + read-only，顺序不能反：

```powershell
Set-Disk -Number 9 -IsReadOnly $false
Set-Disk -Number 9 -IsOffline $false
```

四路条带用 `setup/diskpart-4way.txt`，盘号按本机实际改。动态盘由 LDM/dmio
管理，不会出现在 `Get-Disk` 里，用 `diskpart` 看。

## 架构

### 单连接不够

最初是一个 target、一条会话。队列深度提上去后，瓶颈落在 target 的单线程
收包循环。试过 MC/S（同一会话的第二条连接）：Windows 确实会把命令分到两条
连接（实测 c4=1.94M、c5=0.60M 条），但总吞吐从 148.91/133.70 掉到
142.46/127.28 MB/s。iScsiPrt 对同一会话是串行的，第二条连接只增加调度开销。
所以 `MaxConnections` 默认 1，代码保留，用 `NNPI_ISCSI_MAXCONN=2` 可做对照。

多会话（不同 IQN、不同进程）才是真并行：两路并发 4K QD32 聚合 54,920
IOPS / 214.5 MiB/s，单路 36,491 / 142.5，1.50 倍。

### 把并行度变成一个盘符

CDM 只测一个卷，所以用动态磁盘把多个 iSCSI 盘条带化，见
`setup/diskpart-4way.txt`。Storage Spaces 开销更高，没采用。

四路是终点：扩到六路后 4K 读 69,789 IOPS、顺序读 4345 MiB/s，与四路一致。

## 协议与 Windows 侧的坑

1. SCSI Response 与 R2T 的 byte 1 bit 0 是常量 1。只置 F 位不置常量位，
   iScsiPrt 判非法并 RST（事件 23）。
2. Data-In 只有 S=1 的 PDU 推进连接级 StatSN，中间 PDU 不能递增。
3. Data-In 的 Buffer Offset 必须是本 PDU 数据在整条传输中的偏移；恒填 0
   时大于 64 KiB 的读会被当成重叠数据拒绝。
4. `MaxRecvDataSegmentLength` 由 initiator 定为 64 KiB；`MaxBurstLength`
   256 KiB 不能放大。`InitialR2T`/`ImmediateData` 的协商结果是逻辑或，
   `FirstBurstLength=64 KiB`。`MaxOutstandingR2T=16`，回成 1 等于放弃流水线。
5. 会话身份不能用 ISID。Windows 会跨无关会话复用同一个 ISID
   （`40 00 01 37 00 00`）。规则是 TSIH≠0 且存在才是 MC/S 附加连接，
   TSIH=0 一律新建会话。
6. 一次登录是同一连接上的两条 Login PDU（csg=0 安全协商、csg=1 运行
   协商）。只有第一条 PDU 可以建/入会话，否则一次登录会变成两个会话。
7. Text/SendTargets 响应里不能带 `TargetPortalGroupTag`，否则 Windows 报
   “参数无效的密钥格式”，整个发现失败；登录响应里则必须有。
8. 多个目标名时，每个 `TargetName` 后必须紧跟自己的 `TargetAddress`。
9. 每块盘的 VPD 0x80/0x83 必须不同，否则第二块盘被判成同一设备，直接
   只读/离线。这里按登录的 TargetName 后缀派生。
10. `iscsicli QAddTargetPortal` 不接受端口参数，多路只能各绑一个回环地址、
    共用 3260。
11. `LogoutTarget` 常报“设备正在使用”，设备离线也压不下去，只能换新 IQN。

## 性能

CDM 9.0.3，256 MiB，G: 四路条带，RAM 后端：

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

DiskSpd 交叉验证（4K QD32，`-Sh` 无缓存）：写 59,249 IOPS / 231.44 MiB/s，
读 69,742 IOPS / 272.43 MiB/s；1M 顺序 QD8 读 4329、写 3629 MiB/s。

顺序读写已经超过 Gen3 x4 的 3.94 GB/s。RND4K Q32T1 接近翻倍；Q1T1 没涨
甚至略降，因为单队列没有并行度可分，条带只多一层定位开销。

## RND4K 的极限在哪

CDM 的 `RND4K Q32T1` 固定单线程、队列深度 32。压测时采样 10 秒：

```
DiskSpd64     9.67 s CPU   ≈ 97% 单核
iscsi-target  3.1 s × 4    ≈ 各 32%
```

客户端一根线程先吃满，加盘无效。同样的卷改线程数：T1 69,737 IOPS、
T2 96,010、T4 114,983。同机本地 NVMe 用同样参数是 103,298 IOPS，所以
iSCSI 路径每 IO 多约 4.6 µs，这部分在 iScsiPrt → 回环 TCP 的内核路径上，
target 侧（digest 关闭、ImmediateData、R2T 窗口、单 PDU 一次 send）已经
没有可省的了。

## 本轮试过但回退的做法

1 MiB 连接级接收缓冲 + 响应攒批发送：RND4K 掉到 119/122 MB/s。Windows
回环上 initiator 基本一条命令一段 TCP，批攒不起来；而每次 Receive 用
1 MiB 缓冲区，内核校验并锁定用户区间的成本比省下的 syscall 高一个数量级。
散射发送对单 PDU 同理。

## 限制

- RAM LUN 随 target 重启清零，卷要重建。
- 每路一个 512 MiB 内存缓冲，四路共 2 GiB。
- 动态盘由 LDM/dmio 管理，`Get-Disk` 看不到。
- 下一步是把后端换成卡侧 MEM 通道：卡端 MEM_READ/MEM_WRITE verb、主机侧
  持久化通道和 IOCTL。那部分要新镜像/驱动/签名，单独授权。
