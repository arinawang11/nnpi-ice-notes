# NNP-I card-side module (`sphcs`)

Linux kernel module for the Intel NNP-I 1150 card (Spring Hill) x86 SoC.

- **License**: GPL-2.0-or-later. Copyright (C) 2019-2021 Intel Corporation.
  Vendored here so the community can build it for current kernels; this is
  not an Intel-supported release.
- **Purpose**: expose the card's PCIe Ethernet channel (`sphcschan0`), the
  boot/command channels, and the DMA path used by the card image.
- **Status**: builds against the vendor 5.7 tree; generic-kernel support is
  being added (see `ION_STUB` below).

## Layout

```
card-driver/
  Makefile            out-of-tree kbuild wrapper
  *.c, *.h            module sources (sphcs)
  compat/linux/       headers missing from mainline kernels
  sphcs_ion_stub.c    optional ION allocator stubs (ION_STUB=1)
  card_driver/        card-side protocol headers (card/, common/)
  build.sh            convenience wrapper
```

## Build

```
# vendor 5.7 kernel (has the ION allocator)
make KDIR=/path/to/linux-5.7

# mainline / distro kernel without the vendor ION allocator
make KDIR=/lib/modules/$(uname -r)/build ION_STUB=1
```

The result is `sphcs.ko` with a vermagic matching `KDIR`. It must be built
with the same kernel version that will run on the card.

## Load

The card image init loads it before configuring the interface:

```
insmod sphcs.ko mem_thr=25000 disable_p2p=1
```

`disable_p2p=1` is required on this platform; the ION/P2P paths are not used.

## Known issues (pre-1.0)

- C2H handle lifecycle under sustained load: ACK bounds/generation checks,
  lost-ACK recovery and TX backpressure are being fixed. Until then, heavy
  NBD traffic can exhaust the 16-slot ring and destabilize the card.
- The C2H ring depth is a host/ card ABI constant; see
  `windows-driver/include/nnpi_eth_protocol.h`.

## Provenance

The sources are the GPL-licensed Intel card driver, kept byte-identical where
possible. Changes made by this project are marked in the file history. No
Intel confidential material, firmware or signing keys are included.