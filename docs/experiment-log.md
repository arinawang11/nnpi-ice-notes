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
