# NNP-I Windows Host Driver (KMDF)

Windows 11 x64 KMDF driver and CLI for the Intel NNP-I 1150 host interface
(`PCI\VEN_8086&DEV_45C6`). This is an independent interoperability
implementation, not a vendor driver.

## Scope

The driver exposes a private device interface for:

- read-only PCI configuration and status queries;
- a boot protocol used to load a card-side image over DMA;
- a control-plane channel for Ethernet configuration between host and card.

The driver implements the control plane and the RX/TX data ABI. The user-space
bridge (`tools/nnpi-net-bridge.cpp`) connects the channel to a TAP-Windows6
adapter. Whether SSH becomes reachable still depends on the card-side image
and its network configuration; the driver alone does not provide a network
stack.

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
  nnpi-info.cpp         diagnostics CLI
  nnpi-net-bridge.cpp   TAP bridge (data plane)
tests/                  offline C11/C++17 unit tests
```

## Build

Requirements: Visual Studio 2022, Windows 11 SDK/WDK, KMDF.

```powershell
msbuild nnpi-host.sln /p:Configuration=Release /p:Platform=x64
```

The bridge needs the official `tap-windows.h` from the OpenVPN
tap-windows6 project. Point `NnpiTapIncludeDir` at the directory containing
it:

```powershell
msbuild tools\nnpi-net-bridge.vcxproj /p:Configuration=Release /p:Platform=x64 `
        /p:NnpiTapIncludeDir=C:\path\to\tap-windows6\include
```

No TAP ioctl numbers are copied into this repository; the header must come
from the upstream project.

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

The bridge has a separate mock test that needs no adapter or device:

```sh
g++ -std=c++17 -Wall -Wextra -I include -o test_bridge tools/tests/test_bridge.cpp
./test_bridge
```

It prints `BRIDGE_MOCK_TESTS_PASS` on success.

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
