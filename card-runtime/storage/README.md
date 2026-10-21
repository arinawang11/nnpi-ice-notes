# 卡端持久化存储（TCP NBD）

卡上的 root 是 RAM 盘。本目录把主机上的一个 VHDX（或裸卷）通过 net-bridge
上的 TCP NBD 接到卡端 `/dev/nbd1`，挂到 `/mnt/vhdx`，再用定时写回把 RAM 根
同步进去。SSH、邮箱和存储共用一条以太网通道，不需要在 raw 隧道和网络之间
来回切换。

## 文件

| 文件 | 位置 | 作用 |
| --- | --- | --- |
| `nnpi-tcpstore.sh` | `/usr/local/sbin/` | 等端口、连 nbd1、挂载、通知邮箱 |
| `nnpi-tcpstore.service` | `/etc/systemd/system/` | 常驻上面的循环 |
| `mint-sync.sh` | `/usr/local/sbin/` | RAM 根 → `/mnt/vhdx` 的增量写回 |
| `mint-sync.service` / `.timer` | `/etc/systemd/system/` | 启动后 3 分钟首跑，之后每 15 分钟 |
| `nnpi-mailbox.sh` | `/usr/local/bin/` | 用存储末尾 1 MiB 做的命令邮箱 |
| `nnpi-mailbox.service` | `/etc/systemd/system/` | 邮箱常驻 |

主机侧服务器是 [`windows-driver/tools/nbd-server.py`](../../windows-driver/tools/nbd-server.py)。

## 主机

```powershell
python windows-driver\tools\nbd-server.py `
  --bind 192.168.115.1 --port 10810 `
  --backing \\.\PHYSICALDRIVE4 --size 17179869184 --proto newstyle
```

`--proto newstyle` 需要卡端 `nbd-client >= 3.10`。裸卷没有文件长度，必须显式
给 `--size`；普通文件可以省略。

## 卡端安装

```sh
install -m 755 nnpi-tcpstore.sh mint-sync.sh /usr/local/sbin/
install -m 755 nnpi-mailbox.sh /usr/local/bin/
install -m 644 nnpi-tcpstore.service mint-sync.service mint-sync.timer \
               nnpi-mailbox.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now nnpi-tcpstore.service mint-sync.timer nnpi-mailbox.service
```

`nbd-client` 必须以 `-n` 前台运行，并作为服务主进程的子进程等待退出。
systemd 默认在服务主进程结束时清理整个 cgroup，daemonize 的 nbd-client 会被
一起杀掉，挂载中的文件系统随即报 I/O 错误。

## 运行契约

- 当前存储设备写在 `/run/nnpi-store-dev`。邮箱和写回脚本从该文件取设备，
  缺省 `/dev/nbd0`，所以从 raw 切到 TCP 后要重启这两个服务。
- 主机服务器一次只服务一个连接。**已连接时不要探测端口**：探测会超时，
  循环会跳过挂载。`nnpi-tcpstore.sh` 只在未连接时才探测。
- 切换通道前先在卡端卸载 `/mnt/vhdx` 并停掉邮箱、写回和 tcpstore，否则设备
  被抽走会留下 I/O 错误和 D 状态任务。卸载顺序：先 `umount`，再停服务。
- 写回**不带 `--delete`**：`/mnt/vhdx` 同时是卡侧存储，只存在于挂载点的文件
  不能被写回删掉。反过来，在 RAM 里删除文件不会同步删除 VHDX，需要两边都删。
- 写回排除静态镜像树和缓存（`/usr/share/{doc,man,gnome,locale,icons,fonts}`、
  `/var/cache/apt`、`/root/.cache`），单次增量在 24 秒量级；全根扫描在
  2.5 MB/s 的链路上跑不完一个周期，补丁会永远落不了盘。
- `mint-sync.sh` 用 `flock` 防止重叠，并把 rsync 的退出码交给 systemd；早期
  版本无条件 `exit 0`，失败是静默的。

## 验证

```sh
findmnt -no SOURCE,FSTYPE,OPTIONS /mnt/vhdx     # /dev/nbd1 ext4 rw
cat /run/nnpi-store-dev                          # /dev/nbd1
systemctl is-active nnpi-tcpstore mint-sync.timer nnpi-mailbox
ps -eo stat,comm | awk '$1 ~ /D/'               # 应为空
```

写一个只存在于挂载点的文件，等一轮写回：
`echo keep > /mnt/vhdx/probe.txt`，15 分钟后它应当仍在。再在 RAM 里写一个
文件，确认它被镜像到 `/mnt/vhdx` 的同一路径。
