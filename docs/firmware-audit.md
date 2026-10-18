# ICE 固件载荷核查

## 目标

当寄存器注释不足以确定 credit、state 和写完成协议时，需要确认本地是否
存在可反汇编的 TLC/IVP/ASIP 固件。核查仅涉及文件读取与 CPIO 目录元数据
解析，不联网、不操作设备。

**结论：在已检查的范围内没有找到目标 `fw.bin` / `map.bin` / ELF，固件
代码分析无法开始。** 这不证明固件不存在于其他位置，也不证明自研直配
不可行；准确的阻塞是缺少可分析载荷及其匹配工具。

## 检查范围

- 卡端镜像的 `/lib/firmware`：只有 `rtl_nic` 下的 Realtek 固件。
- 卡端 `/opt/intel_nnpi`：只有 SPH 工具（`shc`、`iMunch`、`sandstone`、
  `sphcs.ko`），无 ICE 固件。
- Buildroot 目标树 `target/lib`、`target/opt`、`target/usr/lib`：无
  `cve_image*` 或 TLC/IVP/ASIP 程序。
- Buildroot 配方引用的 release artifacts 路径不存在；`external-sources`
  与 `dl` 中也没有 `ice_driver_fw_pkg_rtl_release-3.0.0.tar.gz`。
- 两个 rootfs CPIO 的目录元数据（约 2100 项）按 `cve_image`、`cve_cb_`、
  TLC/IVP/ASIP 等名称筛选，无匹配。解析只读 header 与文件名，跳过文件
  内容。该结果排除的是已知命名，不能排除改名或嵌套载荷。
- 历史镜像检查记录同样显示未发现 `cve_image*` 或生产 runtime；`sphcs`
  通信模块不等于 ICE 固件。

## 加载格式

驱动中的固件加载路径如下（文件路径相对厂商驱动树）：

- `driver/cve_firmware.c`：TLC 使用 `cve_image0`，IVP 使用 image1–3，
  ASIP 使用 image4–6，另有一个 `cve_cb_1`；每个 bin 配套一个
  `map.bin`。这个文件名映射不是实际固件清单。
- `driver/cve_fw_map_interface.h`：map 使用 packed 描述符，11 个 `u32`
  共 44 字节，字段为 version、offset_in_file、cve_addr、size_bytes、
  permissions 和 reserved。它不是 ELF 节表或符号表。
- `driver/cve_firmware.c`：`request_firmware` 分别读取 map 与 fw，把数据
  交给内存加载器；加载器不解析 ELF，也不推导 TLC 指令语义。
- 同一文件按 map 长度和描述符大小计数，把描述的文件区间复制到 DMA
  缓冲区；不超过 16 字节的 marker 节有忽略分支。这是装载布局处理，不是
  credit/state 执行实现。
- embedded CB 要求单个 section，按 `size >> TLC_COMMAND_SIZE_SHIFT`
  形成 subjob。驱动宏中 shift 为 5，即 32 字节命令单位；这不等同于原生
  Tensilica 指令宽度。embedded CB 的消费者存在，不证明对应二进制内嵌在
  驱动中。

## 缺失的输入

Buildroot 配方明确期待 `ice_driver_fw_pkg_rtl_release-3.0.0.tar.gz`。
这只是包版本线索，不是已确认适合当前卡的固件。所检查的 Linux 配置为
`CONFIG_EXTRA_FIRMWARE=""`，没有以 builtin firmware 方式内嵌目标载荷。
驱动源码的哈希或提交日期不能代替固件 manifest 与版本匹配证据。

现有 `objdump` 为 x86_64 工具链；`/usr/bin`、`/usr/local/bin` 与
Buildroot `host/bin` 中都没有匹配 Xtensa 的工具。要可信地解释扩展指令与
MMIO 访问，需要原始 ELF/映射、载荷来源与版本，以及匹配的 Tensilica
配置。通用反汇编器的勉强输出不能作为协议证据。

## 后续条件

若取得具体包或原始 ELF，先离线记录哈希与来源，核对 bin-map 范围与版本，
再分析实际控制流。未知固件不应上传或加载到卡。若没有实物，保留现有
已部署版本及其日志，不反复扫描同一批缺失目录。
