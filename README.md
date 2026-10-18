# NNP-I 1150 Reverse Engineering Notes

Intel NNP-I 1150（Spring Hill）加速卡的逆向记录与可复刻组件。仓库包含
自研的 Windows KMDF 主机驱动、卡端运行时说明、离线实验记录，以及在卡上
x86 SoC 运行 PyTorch 的部署脚本。

> 加速器（DELPHI）路径受限于缺失的 TLC 固件 `cve_image*.bin`；卡端 x86
> 核心可运行 PyTorch 2.4.1+cpu，ResNet18 输出与参考实现逐位一致。

## 1. 可以复刻什么

| 目标 | 可行性 | 需要自备 |
| --- | --- | --- |
| 构建主机驱动与 CLI | 直接 | NNP-I 卡、Windows 11 x64、VS2022 + WDK |
| 查询设备信息 | 直接 | 已安装并签名的驱动 |
| 建立 TAP 通道并 SSH | 需要卡端镜像 | TAP-Windows6、可启动的卡端 Linux 镜像 |
| 卡端 PyTorch CPU | 需要卡端 Linux 可访问 | 镜像、CPython 3.8 运行时、wheels |
| DELPHI 直配实验 | 不可直接复刻 | 寄存器资料与自研探针（未公开） |
| 恢复原厂推理 | 不可行 | TLC/IVP/ASIP 固件（本仓库不含） |

主机侧 PCIe 功能是 `8086:45c6`；卡内 ICE 是 `8086:45c4`。两者不是同一
设备，主机驱动绑定的是前者。

## 2. 前置条件

**硬件**

- 一块 NNP-I 1150 卡，安装在有可用 PCIe 插槽的主机上；
- 主机供电与散热满足卡的要求。

**主机软件**

- Windows 11 x64；
- Visual Studio 2022（Desktop C++）+ Windows 11 SDK/WDK；
- 管理员权限；
- 可选：TAP-Windows6 适配器（OpenVPN 项目）用于网络通道。

**卡端**

- 可启动的卡端 Linux 镜像（Buildroot / Linux 5.7）；
- 或合法的原厂恢复介质。

本仓库不包含镜像、固件、厂商驱动或签名材料。没有卡端镜像时，只能完成
主机驱动构建与只读查询，无法进入 SSH 或 PyTorch 部分。

## 3. 仓库结构

```
docs/
  hardware-notes.md     平台架构、TLC 命令模型、固件缺失证据
  experiment-log.md     实验方法与否定结论
  verdict.md            逆向可行性判定
  firmware-audit.md     固件载荷核查范围与结果
  host-setup.md         Windows 驱动安装、TAP 桥接、SSH
  card-runtime.md       卡端 Buildroot/X11 运行时与持久化
  windows-migration.md  主机管理面迁移的分层方案
  troubleshooting.md    频率、MSR、X11、WSL、MMIO 故障排查
  pytorch-cpu.md        卡端 PyTorch/ONNX Runtime 说明
windows-driver/
  driver/               KMDF 驱动（GPL-2.0-only）
  include/              共享 ABI 与协议头
  tools/                nnpi-info CLI、TAP 桥接
  tests/                离线 C11/C++17 单元测试
experiments/
  pytorch-cpu/          部署与一致性校验脚本
```

## 4. 构建主机驱动

在 “x64 Native Tools Command Prompt for VS 2022” 中：

```powershell
cd windows-driver
msbuild nnpi-host.sln /p:Configuration=Release /p:Platform=x64
```

产物位于 `windows-driver\build\x64\Release\`：

```
driver\nnpi_info.sys
driver\nnpi_info.inf
tools\nnpi-info.exe
```

工程默认 `SignMode=Off`。开发环境可启用测试签名：

```powershell
bcdedit /set testsigning on
```

重启后系统进入测试签名模式，可加载自签名的内核驱动。这会降低主机的驱动
完整性保护，生产环境不要启用。正式部署应使用自己的代码签名证书。

安装：

```powershell
pnputil /add-driver .\build\x64\Release\driver\nnpi_info.inf /install
```

INF 将设备接口限制为 `SYSTEM` 与 `BUILTIN\Administrators`。安装后确认：

```powershell
pnputil /enum-devices /class System
Get-Service nnpi_info
```

`nnpi-info.exe` 提供只读查询。运行前确认设备已启动。各子命令的参数在
源码的 usage 字符串中列出，例如 `boot-query`、`net-query`、
`runtime-info`；不带子命令时执行默认信息查询。

## 5. 离线测试

测试不依赖 WDK 或硬件，可在任意有 C11/C++17 编译器的机器上运行：

```cmd
cd windows-driver
tests\run-tests.cmd
```

脚本用 MSVC `/W4 /WX` 编译并执行 26 个测试。Linux 下可等价执行：

```sh
cd windows-driver
for f in tests/test_*.c; do
    gcc -std=c11 -Wall -Wextra -Werror -I include -o /tmp/t "$f" && /tmp/t
done
```

## 6. 建立 TAP 通道

桥接工具把驱动的以太通道接到 TAP-Windows6 适配器。它需要上游的
`tap-windows.h`，本仓库不复制该头文件：

```powershell
msbuild tools\nnpi-net-bridge.vcxproj /p:Configuration=Release /p:Platform=x64 `
        /p:NnpiTapIncludeDir=C:\path\to\tap-windows6\include
```

创建 TAP 适配器（OpenVPN 安装器或 `tapctl`），记录其 GUID。桥接命令需要
设备实例 ID、TAP GUID 和会话标识：

```powershell
nnpi-net-bridge.exe `
  --device-instance 'PCI\VEN_8086&DEV_45C6&SUBSYS_...' `
  --tap-guid '{...}' `
  --session <id> --generation <n> --epoch 1
```

设备实例 ID 每台机器不同，必须从设备管理器或 `pnputil /enum-devices`
读取，不能沿用示例。会话标识来自驱动侧的 BEGIN 响应。

桥接进程持有通道期间不要重复启动。退出时它会先排空 I/O 再释放声明。

## 7. 卡端网络与 SSH

卡端镜像启动后，主机 TAP 与卡端接口应处于同一网段。典型配置：

| 侧 | 地址 |
| --- | --- |
| 主机 TAP | `192.168.115.1/24` |
| 卡端 | `192.168.115.2/24` |

地址只是常见默认值，以镜像配置为准。验证：

```powershell
ping 192.168.115.2
ssh -i <key> root@192.168.115.2 uname -a
```

若使用其他网段，桥接工具与卡端配置需同步修改。详细步骤见
[docs/host-setup.md](docs/host-setup.md)。

## 8. 卡端 PyTorch CPU

卡端无包管理器与网络源，运行时以离线包部署。准备一个 x86_64 Linux 的
CPython 3.8 独立运行时，以及匹配的 wheels：

- `torch 2.4.1+cpu`
- `torchvision`（匹配 torch 2.4.1）
- `numpy` 及依赖
- 可选：`onnxruntime 1.16.3`

在 WSL 中放置资产：

```
/root/card-py/
  py/            CPython 运行时（含 bin/python3.8）
  wheels/        torch、torchvision、numpy
  wheels3/       纯 Python 依赖
  wheels4/       onnxruntime（可选）
```

部署：

```bash
export NNPI_SSH_KEY=/path/to/card_key
export NNPI_CARD=root@192.168.115.2
experiments/pytorch-cpu/deploy.sh
```

脚本把运行时与 wheels 以 tar 流送入卡端 `/tmp/card-py`，离线解包并做导入
自检。成功时输出类似：

```
ready 2.4.1+cpu <torchvision> <numpy>
```

卡端 rootfs 是 RAM initramfs，重启后部署内容丢失，需要重新执行。详见
[docs/pytorch-cpu.md](docs/pytorch-cpu.md)。

## 9. 验证清单

| 阶段 | 检查 | 期望 |
| --- | --- | --- |
| 驱动 | `pnputil /enum-devices` | 设备已启动 |
| 驱动 | `nnpi-info.exe` | 返回设备信息 |
| 网络 | TAP 适配器 | 已启用，地址在同网段 |
| 网络 | `ping <card-ip>` | 有回应 |
| 系统 | `ssh root@<card-ip> uname -a` | 返回卡端内核版本 |
| Python | `/tmp/card-py/run.sh -c "import torch"` | 无错误 |
| 一致性 | `resnet_check.py` | 与参考实现逐位一致 |

## 10. 未包含的内容

为遵守供应商许可与安全要求，本仓库不包含：

- Intel 驱动源码、头文件或标注保密的材料；
- 寄存器偏移表、位域定义、命令二进制布局；
- 固件、设备镜像、恢复身份材料、私钥或序列号；
- 任务账本、签名包与部署凭据。

因此 `docs/experiment-log.md` 记录的是方法与结论，不提供可直接执行的
寄存器级探针。DELPHI 直配实验需要读者自有的寄存器资料。

## 11. 安全与法律

- 只在自有硬件上操作；不要对生产设备启用测试签名。
- 不要加载来源不明的固件或镜像。
- 驱动与脚本按现状提供，不构成担保；涉及 DMA、复位和镜像加载的操作
  应先确认设备状态与缓冲区所有权。
- 本项目为独立互操作研究，与 Intel 无关联。

## 12. 文档索引

| 文档 | 内容 |
| --- | --- |
| [hardware-notes](docs/hardware-notes.md) | 架构、TLC 命令模型、直配边界 |
| [experiment-log](docs/experiment-log.md) | 实验记录与否定结果 |
| [verdict](docs/verdict.md) | 可行性判定与证据边界 |
| [firmware-audit](docs/firmware-audit.md) | 固件核查范围 |
| [host-setup](docs/host-setup.md) | 驱动、TAP、SSH 配置 |
| [card-runtime](docs/card-runtime.md) | 卡端系统与图形环境 |
| [pytorch-cpu](docs/pytorch-cpu.md) | 卡端 Python 运行时 |
| [troubleshooting](docs/troubleshooting.md) | 常见问题 |
| [windows-migration](docs/windows-migration.md) | 主机迁移分层 |
