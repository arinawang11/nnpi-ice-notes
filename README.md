# NNP-I 1150 notes

*[中文版](README.zh-CN.md)*

This is what I ended up with after spending a while poking at an Intel
NNP-I 1150 (Spring Hill). The card is a PCIe accelerator with a small x86
SoC on board. The host talks to it through an interface called ICE, and the
inference engine is called DELPHI.

The short version: I can power the card up, talk to the host interface, read
the accelerator's configuration, and boot a Linux image on the x86 cores.
I could not get DELPHI to execute anything. The firmware that dispatches
work to it (TLC, `cve_image*.bin`) is not in any image, driver package or
archive I have. The x86 cores do run PyTorch, and that part works well
enough to be useful.

## The hardware

The card exposes two PCI functions. `8086:45c4` is the ICE interface on the
card side. `8086:45c6` is what the Windows host binds to. Each ICE instance
has its own MMIO window behind a power-enable and ready handshake.

On the card there is a dual-core x86 SoC (four threads, AVX-512 VNNI) running
a Buildroot Linux 5.7 with a RAM initramfs. It boots from an image loaded
over PCIe. DELPHI sits next to it, along with a few other blocks (MMU, DSE,
IVP, ASIP), all reachable through ICE windows once the card is up.

## What I found

The host driver never writes job commands to DELPHI directly. It puts a
command buffer address and entry count into an MMIO hub, then rings a
doorbell. TLC firmware picks the buffer up and sends config and exec commands
to the target blocks over the on-chip network. The only message channel
available from the host is a small control-message register with a four-bit
opcode, so there is no way to forge an exec command from the CPU side.

Without TLC firmware, that whole path is closed. I went down the direct MMIO
route for a while: power, MMU page tables, geometry, credits, state messages,
start. The engine accepts the start (its idle flag flips), but the unit-of-work
scheduler never dispatches anything. The LSU channels stay empty, the size
registers are never latched, and no AXI or MMU traffic appears. I ruled out
the obvious candidates one by one, including runstall release, credit reset,
state message retries and re-triggering the valid bits. The experiments and
the negative results are written up in `docs/experiment-log.md`.

That is where the accelerator work stopped. If you have the firmware, the
situation is different; without it, I do not think there is a safe way to
guess the dispatch protocol.

## What is in the repo

`windows-driver/` is my own KMDF driver for the `45c6` function, plus a small
CLI and a TAP bridge. It handles device info, the boot protocol used to load
an image, and the Ethernet control and data channel. It does not contain any
vendor source or binaries.

`docs/` has the notes: architecture, the experiment log, the firmware audit,
host setup, the card runtime, troubleshooting, and a write-up of what moving
the host side to Windows involves. The boot sequence is described end to end,
including what the driver does at each stage, in
[`docs/boot-chain.md`](docs/boot-chain.md).

`experiments/pytorch-cpu/` is the deployment path for the CPU runtime.

## Building the driver

You need Visual Studio 2022 with the Windows 11 WDK, and a card in the
machine. From an x64 developer prompt:

```powershell
cd windows-driver
msbuild nnpi-host.sln /p:Configuration=Release /p:Platform=x64
```

The projects are set to `SignMode=Off`. For local testing you can turn on
test signing (`bcdedit /set testsigning on`, then reboot) and install the
driver with `pnputil /add-driver ... /install`. That weakens driver integrity
checks on the machine, so I would not do it outside a test box. For anything
else, sign the package with your own certificate.

The INF limits access to SYSTEM and Administrators. Once installed,
`nnpi-info.exe` gives you the read-only queries. Run it without arguments for
the default info, or look at the usage strings in the source for the
subcommands (`boot-query`, `net-query`, `runtime-info`, and so on).

## Tests

The unit tests are plain C11 and do not need the WDK or a device:

```cmd
cd windows-driver
tests\run-tests.cmd
```

There are 26 of them. On Linux the same files compile with gcc:

```sh
cd windows-driver
for f in tests/test_*.c; do
    gcc -std=c11 -Wall -Wextra -Werror -I include -o /tmp/t "$f" && /tmp/t
done
```

The bridge has a mock test too: `g++ -std=c++17 -I include -o /tmp/tb
tools/tests/test_bridge.cpp && /tmp/tb`.

## Network and SSH

The bridge connects the driver's Ethernet channel to a TAP-Windows6 adapter.
It needs the upstream `tap-windows.h`, which is not copied here:

```powershell
msbuild tools\nnpi-net-bridge.vcxproj /p:Configuration=Release /p:Platform=x64 `
        /p:NnpiTapIncludeDir=C:\path\to\tap-windows6\include
```

Create a TAP adapter, note its GUID, and find the device instance ID for your
card (it differs per machine). Then:

```powershell
nnpi-net-bridge.exe --device-instance 'PCI\VEN_8086&DEV_45C6&SUBSYS_...' `
  --tap-guid '{...}' --session <id> --generation <n> --epoch 1
```

The session id comes from the driver's BEGIN response. Do not start the bridge
twice for the same device; it drains I/O and releases the claim on exit.

The card image is not in this repo. You need one that boots and configures its
network interface. A common setup is host TAP at `192.168.115.1/24` and the
card at `192.168.115.2/24`, but that is just the configuration I used. Once
the card answers, `ssh root@192.168.115.2` should work. More detail is in
`docs/host-setup.md`.

## PyTorch on the card

The card has no package manager and no network install path, so the runtime
is copied over as a tarball. Prepare a standalone CPython 3.8 x86_64 build
and the wheels you need (`torch 2.4.1+cpu`, the matching torchvision, numpy,
and optionally onnxruntime 1.16.3). Put them under `/root/card-py` in WSL:

```
py/
wheels/
wheels3/
wheels4/        # optional
```

Then:

```bash
export NNPI_SSH_KEY=/path/to/key
export NNPI_CARD=root@192.168.115.2
experiments/pytorch-cpu/deploy.sh
```

The script streams everything to `/tmp/card-py`, unpacks the wheels offline,
and prints a version line when the imports work. ResNet18 matches the host
reference bit for bit. The rootfs is an initramfs, so a reboot wipes
`/tmp/card-py` and you have to deploy again.

## What is not here

I left out the vendor driver source and headers, the firmware, the boot
images, the SSH keys and the register tables. The first four are either
proprietary or sensitive, and the register tables are the part of the work I
am not putting in public. That also means the direct-MMIO experiments cannot
be rerun from this repo alone; the notes describe what was tried and what
happened, not a recipe with addresses.

If you are missing the card image or the firmware, the driver build and the
read-only queries are still usable, but the network and PyTorch sections
assume you already have a bootable image.

## A few warnings

Test-signing a kernel driver lowers the security of the machine. Do not load
firmware or images from sources you do not trust. Be careful with DMA and
reset operations; make sure you know who owns the buffers before you touch
anything.

The code is GPL-2.0-only unless a file says otherwise. Nothing here is
affiliated with Intel. Use it on hardware you own.
