# PyTorch CPU on NNP-I — 部署与校验

本目录包含把 PyTorch 2.4.1+cpu 部署到 NNP-I 卡端 x86 SoC 的脚本，以及
一致性校验脚本。背景见 [../../docs/pytorch-cpu.md](../../docs/pytorch-cpu.md)。

## 文件

| 文件 | 作用 |
| --- | --- |
| `deploy.sh` | 从 WSL 资产目录打包并部署运行时、wheels、权重到卡端 `/tmp/card-py` |
| `install_wheels.py` | 在卡端把 wheel 解包到指定 `site` 目录（无需 pip） |
| `resnet_check.py` | ResNet18 前向，与参考实现逐位比对 |
| `transformer_check.py` | 小型 Transformer 前向数值检查 |
| `ort_check.py` | ONNX Runtime 与 PyTorch 输出比对 |

## 前置条件

主机侧：

- 可用的 SSH 私钥（通过环境变量传入，不要写进脚本）；
- WSL 发行版（默认 `NNPI-Build`），其中 `/root/card-py` 已备好
  `py/`、`wheels/`、`wheels3/`、`resnet18.pth`；
- 可选：`wheels4/`、`resnet18.onnx`、`resnet_input.pt` 用于 ONNX 路径。

卡端：

- SSH 可达，root 登录；
- `/tmp` 有足够空间容纳运行时与 wheels。

## 用法

```bash
export NNPI_SSH_KEY=/path/to/card_key
export NNPI_CARD=root@<card-address>          # 可选
export NNPI_WSL_DISTRO=NNPI-Build             # 可选
export NNPI_WSL_ASSETS=/root/card-py          # 可选
./deploy.sh
```

成功时最后一行形如：

```
ready 2.4.1+cpu <torchvision> <numpy>
```

之后在卡端执行校验：

```bash
/tmp/card-py/run.sh /tmp/card-py/resnet_check.py
/tmp/card-py/run.sh /tmp/card-py/transformer_check.py
/tmp/card-py/run.sh /tmp/card-py/ort_check.py      # 需要 wheels4 + ONNX 资产
```

## 注意

- rootfs 为 initramfs，重启后 `/tmp/card-py` 消失，需重新部署。
- 脚本不修改系统目录、不安装到全局 `site-packages`、不需要网络。
- 若卡端地址或密钥路径不同，一律用环境变量覆盖，不要改脚本内的默认值。
