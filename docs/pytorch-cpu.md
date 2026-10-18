# 在 NNP-I 卡端 CPU 上运行 PyTorch

加速器（DELPHI）路径因 TLC 固件缺失无法派发作业后，我们把卡上的 x86
子系统作为目标，部署了一个不依赖网络安装的 PyTorch CPU 运行时。

## 1. 目标环境

| 项目 | 值 |
| --- | --- |
| CPU | x86_64，2 物理核 / 4 逻辑线程，AVX-512 VNNI |
| 系统 | Linux 5.7.0，Buildroot 2019.11 |
| rootfs | RAM initramfs（重启后 `/tmp` 内容丢失） |
| 网络 | 通过 TAP 直连主机，仅 SSH 可达 |
| 包管理 | 无 pip 源、无编译工具链 |

因此方案必须是**整包拷贝 + 离线解包**：在主机 WSL 中准备好 Python 运行时
和 wheel 包，`tar` 流式传到卡端，用脚本解包到 `/tmp`。

## 2. 部署布局

```
/tmp/card-py/
  py/           独立 Python 3.8 运行时（PYTHONHOME）
  wheels/       torch / torchvision / numpy 等主依赖
  wheels3/      其余纯 Python 依赖
  wheels4/      onnxruntime（可选）
  site/         解包目标（PYTHONPATH）
  resnet18.pth  权重（可选）
  run.sh        启动器
```

`run.sh` 设置 `PYTHONHOME`、`PYTHONPATH`、`LD_LIBRARY_PATH` 与
`OMP_NUM_THREADS`（默认 2，匹配物理核数），然后 `exec` 运行时里的
`python3.8`。

## 3. 部署步骤

```bash
export NNPI_SSH_KEY=/path/to/card_key
export NNPI_CARD=root@<card-address>
experiments/pytorch-cpu/deploy.sh
```

脚本做三件事：

1. 从 WSL 资产目录把运行时、wheels、权重以 tar 流送到卡端；
2. 传送本仓库的辅助脚本；
3. 生成 `run.sh`，仅在 `site/torch` 不存在时解包，最后做一次导入自检。

重复执行是幂等的；已安装的 `site/torch` 不会被重装。

## 4. 版本与验证

| 组件 | 版本 |
| --- | --- |
| Python | 3.8 |
| PyTorch | 2.4.1+cpu |
| torchvision | 与 torch 2.4.1 匹配的 CPU 轮子 |
| ONNX Runtime | 1.16.3 |

验证脚本：

- `resnet_check.py`：固定随机种子构造输入，比对 ResNet18 输出与主机参考
  实现。结果为**逐位一致**。
- `transformer_check.py`：小型 Transformer 前向，检查 attention/FFN
  数值稳定性与输出形状。
- `ort_check.py`：加载 ONNX 模型并与 PyTorch 输出比对。

## 5. 性能与限制

- 4 个逻辑线程、约 500 MHz，CPU 推理吞吐有限；本方案用于功能验证与
  离线小模型，不替代加速器。
- AVX-512 VNNI 可被 PyTorch 的 CPU 后端使用，但受主频与内存带宽限制。
- rootfs 是 initramfs：**重启即丢失**，需要重新执行部署。
- `/tmp` 空间有限，部署前应确认剩余空间；wheels 安装完成后可删除
  `wheels*/` 释放空间。

## 6. 与加速器路径的关系

CPU 运行时与 DELPHI 直配实验互不影响：前者只使用卡端 x86 核心和系统
内存，不触碰 ICE/DELPHI 寄存器，也不改变设备电源状态。两者可以并存，
但加速器要真正执行运算仍需 TLC 固件。
