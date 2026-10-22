# 卡端持久化：TCP NBD 实现记录

[storage-mapping.md](storage-mapping.md) 讨论的是可行性分层。这一页记录已经
落地的部分：卡上的 Linux 把主机的一个 VHDX 当作块设备使用，并且跨卡端重启
保留数据。

## 1. 结果

- 主机在 TAP 上提供 newstyle NBD，后端是 `\\.\PHYSICALDRIVE4`（16 GiB VHDX）。
- 卡端 `nbd-client 3.20` 连接 `/dev/nbd1`，挂 ext4 到 `/mnt/vhdx`，可读写。
- 同一时刻 SSH、命令邮箱、存储三条路都可用，不需要 raw 隧道。
- 冷读 64 MiB（偏移 8 GiB）用 26.46 秒，约 **2.5 MB/s**；服务器侧平均请求
  113 KiB、间隔 61 ms，瓶颈仍是 TAP 每帧一次轮转的节拍。
- RAM 根到 VHDX 的增量写回在排除静态树后单次约 **24 秒**。

## 2. 拓扑

```
主机                                          卡
192.168.115.1                                 192.168.115.2
nbd-server.py :10810  <-- net-bridge/TAP -->  nbd-client -> /dev/nbd1 -> /mnt/vhdx
\\.\PHYSICALDRIVE4                            /run/nnpi-store-dev
```

net-bridge 把 TAP 和卡的以太网数据通道接起来；SSH 和 NBD 都是这条链路上的
普通 TCP 连接。NBD 用 10810，SSH 用 22，互不干扰。

## 3. 为什么用 NBD，不用 iSCSI

- 卡端内核有 `nbd`，用户态只需要一个几百行的服务器；iSCSI 要 target 和
  initiator 两套实现。
- 协议面小：握手、28 字节请求头、16 字节应答头，错误路径可以完整覆盖。
- 现有的 iSCSI RAM LUN 继续用于主机侧跑分，两者不冲突。

## 4. 协议要点

卡端 `nbd-client 3.20` 只接受 fixed-newstyle，旧的 oldstyle 握手会被拒绝。
服务器实现见 [`windows-driver/tools/nbd-server.py`](../windows-driver/tools/nbd-server.py)。

- 握手：`NBDMAGIC` + `IHAVEOPT` + 2 字节 flags（`FIXED_NEWSTYLE | NO_ZEROES`）。
- 选项回复的 magic 是 `0x0003E889045565A9`；回成 `IHAVEOPT` 会得到
  `received invalid negotiation magic`。
- 选项号：`EXPORT_NAME=1`、`ABORT=2`、`INFO=6`、`GO=7`。
- 传输标志 `HAS_FLAGS(0x1) | SEND_FLUSH(0x4) | SEND_TRIM(0x20) = 0x25`。
  bit1 是 **READ_ONLY**，误置会把 ext4 挂成只读。
- 卡端用 `-b 4096`，内核会把相邻请求合并，实测平均 113 KiB。`CMD_WRITE`
  只写后端文件，`CMD_FLUSH` 才 `fsync` 落盘；服务器在 FLUSH 完成后才回确认。
- 裸卷没有文件长度，服务器必须用 `--size` 指定导出大小；普通文件走
  `os.path.getsize`。
- 服务器默认监听 `10810`，连接上启用 TCP keepalive；`--io-timeout` 默认 0
  （保持阻塞读），设为非 0 后可用于清理半开连接。

## 5. 卡端接入

`nnpi-tcpstore.service` 常驻一个循环：

1. 未连接时探测 `192.168.115.1:10810`；连上后不再探测（服务器单连接，
   已连接时探测必然超时）。
2. `nbd-client -n -N default -b 4096 -L ... /dev/nbd1`，`-n` 保证进程不
   daemonize，脚本 `wait` 它。systemd 在服务主进程退出时清 cgroup，否则
   会把设备进程一起杀掉。
3. 挂载 ext4，把设备写入 `/run/nnpi-store-dev`，然后
   `systemctl restart --no-block nnpi-mailbox nnpi-wd`。两个服务的 TAIL 在
   进程启动时解析一次，必须重启才会切到 nbd1。

## 6. 写回

`mint-sync.timer` 在启动 3 分钟后首跑，之后每 15 分钟执行一次
`rsync -aHX / /mnt/vhdx/`。

两个关键决定：

- **不带 `--delete`。** `/mnt/vhdx` 同时是卡侧存储，只存在于挂载点的文件
  不能被写回删掉。早先版本带 `--delete`，测试文件在下一轮写回时被删除，
  看起来像“数据丢失”，实际是同步脚本主动删的。
- **排除静态树。** 全根扫描包括 `/usr/share/gnome`、文档、man、locale、
  图标等数万文件，在 2.5 MB/s 的链路上跑不完 5 分钟周期。一旦被切链路或
  重启打断，扫描还没走到 `/usr/local`，补丁就只留在 RAM 里。排除这些目录
  和 apt/root 缓存后，单次约 24 秒。

脚本还用 `flock` 防止定时器和手工运行重叠，并把 rsync 的退出码交给
systemd。早期版本无条件 `exit 0`，写回失败在 systemd 里永远显示 success。

## 7. 命令邮箱

存储末尾 1 MiB 留作控制面，不经过 ext4：

| 块 | 方向 | 内容 |
| --- | --- | --- |
| 4194064 | 主机 → 卡 | `NNPIREQ <seq>` + 命令文本 |
| 4194065 | 卡 → 主机 | `NNPIRES <seq> start/rc/end` + 输出尾部 |

卡端每 2 秒用 `dd iflag=direct` 读控制块（失败时先 `blockdev --flushbufs`
再读），执行 `sh /tmp/cmd.sh`，结果写回响应块并 `fsync`。序号记录在
`/run/nnpi-mailbox.last`，避免重复执行。实测一次请求返回
`SEQ=... RC=0`，与存储链路共用同一个设备指针。

## 8. 操作与恢复

- **切换通道前先静默卡端**：停邮箱、写回、tcpstore，`umount /mnt/vhdx`，
  再停主机服务器。顺序反了会留下 I/O 错误和 D 状态任务。
- 主机 NBD 服务器一次只服务一个连接，端口探测只能在断开时做。
- 同一次启动内 raw → net 是单向的：rawbridge 退出后 nbd0 会话即死，
  `nbd-client 3.20` 不能重连 oldstyle，回 raw 存储需要卡端重启。
- 卡死设备的 D 状态任务会一直挂到重启；`nbd-client -d` 可以清掉已死的
  nbd0 会话，但要先确认没有挂载。

## 9. 已验证与待验证

已验证：

- 卡端写入 → `sync -f` → 卸载 → 主机离线读到一致内容。
- 邮箱经 nbd1 返回 `RC=0`。
- VHDX-only 文件在写回后仍存在；RAM 文件被镜像到挂载点；三个脚本在
  RAM 与 VHDX 两侧 sha256 一致。
- 单次写回 23.7 秒，systemd `result=success`，rsync rc=0。

待验证：

- 下一次卡端重启后，修补过的邮箱/WD 脚本、15 分钟定时器和挂载内容是否
  自动生效（本次开机残留的旧服务实例只能靠重启清除）。
- 多连接或多个导出能否把 2.5 MB/s 抬起来；先测量再改，TAP 节拍可能才是
  真正的上限。
