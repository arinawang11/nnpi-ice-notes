# Card image builder

Builds a bootable NNP-I card image for a **user-supplied Linux kernel**:

1. builds the card module (`card-driver/sphcs`) against the target kernel tree;
2. stages an initramfs (project init + `sphcs.ko` + optional rootfs files);
3. rebuilds the target kernel with that initramfs embedded;
4. assembles the 188,744,192-byte FAT16 card image with the kernel at
   `bzImage` and `EFI/BOOT/BOOTX64.EFI`;
5. writes a JSON manifest with the image SHA-256.

## Requirements

- Linux host (or WSL) with `make`, `mkfs.fat`, `mtools` (`mcopy`, `mmd`),
  `python3`, and the target kernel's toolchain.
- A **configured** kernel build tree (`make olddefconfig` already run) with
  `CONFIG_BLK_DEV_NBD`, `CONFIG_EXT4_FS`, and the x86_64 bzImage target.
- The initramfs needs a shell and `nbd-client` (and optionally `rawtun`)
  for the root transport; provide them via `--rootfs` or a custom `--init`.

## Usage

```
python3 image-builder/nnpi-image-builder.py \
  --kernel-tree /path/to/linux \
  --rootfs /path/to/initramfs-tools \
  --ion-stub \
  --out nnpi-card.img
```

`--ion-stub` adds `card-driver/sphcs_ion_stub.c` for kernels without the
vendor ION allocator.

## Output

- `nnpi-card.img` — exactly 188,744,192 bytes, FAT16.
- `nnpi-card.img.manifest.json` — image SHA-256, module hash, init hash,
  kernel version string.

## Allowlist

The Windows driver only accepts images whose SHA-256 is in its allowlist.
A user-built image therefore needs an authorization step before `boot-load`
will accept it. The 1.0 design uses an admin-provisioned user key that signs
`(image_sha256, metadata)`; until that lands, rebuild the driver allowlist or
use the developer mode described in `docs/release/RELEASE-PLAN.md`.