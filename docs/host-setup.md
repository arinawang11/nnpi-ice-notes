# Windows 主机侧配置

本页描述主机驱动安装、TAP 桥接与 SSH 访问的通用步骤。设备实例 ID、
GUID 和地址在不同机器上会变化，命令中的占位符需按实际值替换。

## 1. 驱动包

主机驱动为 KMDF 包，硬件 ID 为 `PCI\VEN_8086&DEV_45C6`（主机侧 PCIe
功能），与卡内 `45C4` 设备不同。包内包含：

```
nnpi_info.inf
nnpi_info.sys
nnpi_info.cat
```

INF 将设备访问 ACL 限制为 `SYSTEM` 与 `BUILTIN\Administrators`，且不提供
自动安装入口。测试签名或未签名驱动需要开启测试签名模式并重启，这会降低
主机的驱动加载安全性，建议只在专用测试机上使用。

安装（管理员 PowerShell）：

```powershell
pnputil /add-driver .\nnpi_info.inf /install
pnputil /enum-devices /class System
```

确认设备状态为 `Started`，再检查服务：

```powershell
Get-Service nnpi_info
```

## 2. TAP 桥接

卡端 Linux 通过 PCIe 上的虚拟以太网通道与主机通信，主机侧以一个 TAP
适配器呈现。需要确认：

- TAP 适配器的 GUID（安装时生成，写入桥接配置）；
- 主机侧 TAP 地址与卡端地址处于同一网段；
- 卡端地址通常为 `192.168.115.2/24`，主机侧取同网段的另一地址。

查看适配器：

```powershell
Get-NetAdapter | Where-Object InterfaceDescription -like '*TAP*'
```

桥接工具以设备实例 ID 与 TAP GUID 为参数，两者都必须来自本机，不能沿用
其他机器的值。桥接进程退出后网络通道消失，SSH 随之中断。

## 3. SSH

卡端 SSH 仅监听 TAP 地址。使用专用的 ed25519 密钥，并在客户端显式禁用
代理与配置文件，避免本机其他设置干扰：

```bash
ssh -F /dev/null \
    -i "$NNPI_SSH_KEY" \
    -o BatchMode=yes \
    -o IdentitiesOnly=yes \
    -o IdentityAgent=none \
    -o StrictHostKeyChecking=yes \
    -o UserKnownHostsFile="$HOME/.ssh/known_hosts" \
    root@"$NNPI_CARD"
```

首次连接前将卡端主机密钥写入 `known_hosts`。卡端为 RAM initramfs，
重启后主机密钥可能重新生成，届时需要更新对应条目。

## 4. 启动顺序

主机驱动与卡端镜像的加载顺序会影响结果：

1. 主机启动并完成驱动绑定；
2. 桥接进程建立 TAP 与 PCIe 通道；
3. 卡端镜像启动，网络就绪；
4. SSH 可达。

若卡端在主机重启前已经启动过一次，重新加载镜像可能停在中间状态。需要
让卡端完成一次干净的电源周期后再加载，而不是在原状态上重复提交。

## 5. 验证清单

| 检查 | 期望 |
| --- | --- |
| `pnputil /enum-devices` | 设备存在且已启动 |
| TAP 适配器 | 已启用，地址在同网段 |
| `ping <card-ip>` | 有回应 |
| `ssh root@<card-ip> uname -a` | 返回卡端内核版本 |
| 卡端 `ip addr` | TAP 地址与主机同网段 |

以上任一失败时，先确认桥接进程仍在运行，再检查主机防火墙是否拦截了
TAP 网段。
