# NNP-I 1150 折腾笔记

*[English](README.md)*

这块 Intel NNP-I 1150（Spring Hill）前后折腾了一段时间，这里是把过程和
能用的东西整理出来的结果。卡是一块 PCIe 加速卡，板上带一颗小的 x86 SoC。
主机通过 ICE 接口跟它通信，推理引擎叫 DELPHI。

结论先放在前面：卡能上电，主机接口能通，加速器的配置能读，卡上的 x86
也能启动 Linux。但 DELPHI 始终没有跑起来。负责给它派发任务的 TLC 固件
（`cve_image*.bin`）在我手上的镜像、驱动包和归档里都找不到。x86 那边的
PyTorch 倒是能正常用，而且速度不算差。

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

## 仓库里有什么

`windows-driver/` 是我自己写的 KMDF 驱动，针对 `45c6` 这个 function，
另外还有一个小的命令行工具和一个 TAP 桥接程序。驱动负责设备信息查询、
加载卡端镜像的 boot 协议，以及以太网的配置和数据通道。里面没有厂商的
源码或二进制。

`docs/` 是笔记：架构、实验记录、固件核查、主机环境、卡端运行环境、
排错，还有主机侧迁到 Windows 需要做哪些事。

`experiments/pytorch-cpu/` 是卡端 CPU 运行时的部署脚本。

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
