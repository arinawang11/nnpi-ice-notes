# NNP-I 1150 Reverse Engineering Notes

> TL;DR — Reverse-engineering notes for the Intel NNP-I 1150 inference
> accelerator (`8086:45c4`, Spring Hill / DELPHI). The on-die accelerator
> path is blocked on missing TLC firmware (`cve_image*.bin`); the SoC's x86
> cores run PyTorch 2.4.1+cpu bit-exactly (ResNet18 verified).

本仓库记录对一块 Intel NNP-I 1150 加速卡（PCI ID `8086:45c4`）的逆向过程：
从主机侧 PCIe/MMIO 观察到卡端 Linux 环境，再到加速器（DELPHI 推理引擎）
直配实验，最后转向 SoC 上的 x86 CPU 运行 PyTorch。

## 硬件概览

| 项目 | 内容 |
| --- | --- |
| 加速卡 | Intel NNP-I 1150（Spring Hill） |
| PCI ID | `8086:45c4`（ICE 主机接口） |
| 卡端 CPU | x86_64，2 物理核 / 4 逻辑线程，约 500 MHz，AVX-512 VNNI |
| 卡端系统 | Linux 5.7.0，Buildroot 2019.11，RAM initramfs |
| 加速引擎 | DELPHI（CNN/GEMM 数据流引擎），经 CnC 网络由 TLC 调度 |
| 主机接口 | ICE（ICEPE/ICERDY 电源门控 + 每 ICE 的 MMIO 窗口） |

## 关键结论

1. **加速器命令路径需要 TLC 固件。** 主机侧驱动把作业描述符挂到 MMIO HUB
   的命令缓冲上，由 TLC 固件取出并通过 CnC 网络向各 CBB 下发
   EXEC/CONFIG 命令。主机侧唯一可用的 `GENERATE_CONTROL_UCMD` 通道只能发送
   4-bit opcode 的控制类消息，无法生成 EXEC/CONFIG。
2. **固件不存在。** 卡端 `/lib/firmware` 只有 `rtl_nic`；主机侧驱动树、
   安装包、本地归档中均无 `cve_image0_fw.bin` / `cve_image0_map.bin` /
   `cve_image1..4_fw.bin`。这是外部资源缺口，不是逆向方法问题。
3. **纯 MMIO 直配 DELPHI 已到达上限。** 通过 ICE 窗口可以对 DELPHI 完成
   电源、MMU 页表、几何、credit、state message 的全部配置，`delphi_start`
   也被引擎接收（IDLE 指示翻转），但 UOW 调度器从未启动：LSU 十条通道
   全部空闲、大小寄存器未锁存、credit 未被消费、无 MMU/ATU 流量。
   所有可测旁路（runstall 释放、credit 重置、state 消息重发、valid 重触发、
   MS blob 推进）均被否定。
4. **SoC CPU 路径可用。** 在卡端 x86 核心上部署 PyTorch 2.4.1+cpu 与
   torchvision，ResNet18 推理结果与主机参考实现逐位一致；ONNX Runtime
   1.16.3 亦可运行。

## 仓库结构

```
docs/
  hardware-notes.md     平台架构、命令模型与逆向结论（高层）
  experiment-log.md     实验记录：方法、结果与否定的假设
  pytorch-cpu.md        卡端 PyTorch/ONNX Runtime 运行时说明
experiments/
  pytorch-cpu/          部署脚本与一致性校验脚本
```

## 未包含的内容

为遵守供应商许可与安全要求，本仓库**不包含**：

- Intel 驱动源码、头文件或任何标注 CONIDENTIAL 的材料；
- 寄存器偏移表、位域定义、命令二进制格式；
- 任何固件、设备镜像、恢复身份材料、私钥或序列号。

文档只描述方法、观察到的行为与结论，不提供可直接复现寄存器级写入的
地址/位域信息。

## 免责声明

本项目为独立互操作研究，与 Intel 无关联。所有实验在自有硬件上进行，
以不损坏设备为前提。仓库内容按现状提供，不构成任何担保。请勿将本文档
用于规避供应商许可或获取未授权固件。
