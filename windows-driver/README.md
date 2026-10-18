# NNP-I Windows Host Driver (KMDF)

Windows 11 x64 KMDF driver and CLI for the Intel NNP-I 1150 host interface
(`PCI\VEN_8086&DEV_45C6`). This is an independent interoperability
implementation, not a vendor driver.

## Scope

The driver exposes a private device interface for:

- read-only PCI configuration and status queries;
- a boot protocol used to load a card-side image over DMA;
- a control-plane channel for Ethernet configuration between host and card.

The data plane (frame TX/RX), IP configuration, SSH tunnelling and inference
are outside this implementation. `READY` from the net control channel means
the channel and page pools were acknowledged by the card, not that a network
stack is operational.

## Layout

```
nnpi-host.sln
driver/
  driver.c            KMDF entry, PnP/power, IOCTL dispatch
  nnpi_hw.c/.h        BAR mapping, PCI config access, device identity
  nnpi_dma.c/.h       common-buffer DMA
  nnpi_image.c/.h     image layout and validation
  nnpi_irq.c/.h       interrupt handling
  nnpi_net_dma.c/.h   channel page pools
  nnpi_reset.c/.h     function-level reset path
  nnpi_info.inf       INF (SYSTEM/admin ACL, no auto-install)
  nnpi_info.vcxproj
include/              shared ABI and protocol headers
tools/
  nnpi-info.cpp       diagnostics CLI
tests/                offline C11/C++17 unit tests
```

## Build

Requirements: Visual Studio 2022, Windows 11 SDK/WDK, KMDF.

```powershell
msbuild nnpi-host.sln /p:Configuration=Release /p:Platform=x64
```

The projects have signing disabled (`SignMode=Off`). A test certificate and
`pnputil /add-driver` are required for installation; both are environment
specific and intentionally not automated here.

## Tests

The `tests/` directory contains protocol and policy unit tests that compile as
plain C11 or C++17 with no WDK and no hardware access:

```cmd
tests\run-tests.cmd
```

Run from an x64 Visual Studio developer prompt. The script compiles each test
with `/W4 /WX` and executes it; any failure aborts.

## Behaviour and safety

- The INF grants access to `SYSTEM` and `BUILTIN\Administrators` only.
- No device write occurs during `Prepare`/power-up beyond what the PCI bus
  driver performs; MMIO reads happen only on explicit status requests.
- Reset, boot and DMA operations are explicit commands, never automatic.
- The driver does not install itself, change boot settings or modify the card
  image on disk.

## License

Source files are `SPDX-License-Identifier: GPL-2.0-only`. The vendor's original
headers and firmware are not included and remain under their own terms.

This code is provided as-is, for research and interoperability on hardware you
own. Installing an unsigned or test-signed kernel driver has security
implications; use a dedicated test machine.
