#!/usr/bin/env python3
"""Build a bootable NNP-I card image for a user-supplied kernel tree.

Linux host only (make, dosfstools, mtools, python3).
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

IMAGE_BYTES = 188744192


def run(cmd, cwd=None):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], cwd=cwd, check=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    here = Path(__file__).resolve()
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kernel-tree", required=True, help="configured kernel build tree")
    ap.add_argument("--module-dir", default=str(here.parents[1] / "card-driver"))
    ap.add_argument("--init", default=str(here.parent / "init" / "init.sh"))
    ap.add_argument("--rootfs", help="directory merged into the initramfs")
    ap.add_argument("--out", required=True, help="output image path")
    ap.add_argument("--ion-stub", action="store_true", help="build the ION symbol stubs")
    ap.add_argument("--skip-kernel-build", action="store_true",
                    help="reuse the existing bzImage (initramfs not re-embedded)")
    ap.add_argument("--workdir", help="keep intermediate files in this directory")
    args = ap.parse_args()

    kernel = Path(args.kernel_tree).resolve()
    module_src = Path(args.module_dir).resolve()
    init_src = Path(args.init).resolve()
    if not (kernel / "Makefile").exists():
        sys.exit("not a kernel tree: " + str(kernel))
    if not (kernel / ".config").exists():
        sys.exit("kernel tree is not configured (run make olddefconfig): " + str(kernel))

    work = Path(args.workdir) if args.workdir else Path(tempfile.mkdtemp(prefix="nnpi-image-"))
    work.mkdir(parents=True, exist_ok=True)

    # 1. Build the module from a private copy (keeps the source tree clean).
    module = work / "card-driver"
    if module.exists():
        shutil.rmtree(module)
    shutil.copytree(module_src, module)
    run(["make", "-C", module, "KDIR=" + str(kernel),
         "ION_STUB=" + ("1" if args.ion_stub else "0")])
    ko = module / "sphcs.ko"
    if not ko.exists():
        sys.exit("sphcs.ko was not produced")

    # 2. Stage the initramfs.
    stage = work / "initramfs"
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "lib" / "modules").mkdir(parents=True)
    shutil.copy2(init_src, stage / "init")
    os.chmod(stage / "init", 0o755)
    shutil.copy2(ko, stage / "lib" / "modules" / "sphcs.ko")
    if args.rootfs:
        shutil.copytree(Path(args.rootfs).resolve(), stage, dirs_exist_ok=True)

    # 3. Rebuild the kernel with the initramfs embedded.
    if not args.skip_kernel_build:
        run(["make", "-C", kernel, "CONFIG_INITRAMFS_SOURCE=" + str(stage), "bzImage"])
    bz = kernel / "arch" / "x86" / "boot" / "bzImage"
    if not bz.exists():
        sys.exit("bzImage not found: " + str(bz))

    # 4. Assemble the FAT16 card image (exact size required by the boot ABI).
    img = work / "nnpi-card.img"
    if img.exists():
        img.unlink()
    subprocess.run(["truncate", "-s", str(IMAGE_BYTES), str(img)], check=True)
    run(["mkfs.fat", "-F", "16", "-n", "NNPI", img])
    run(["mmd", "-i", img, "::/EFI"])
    run(["mmd", "-i", img, "::/EFI/BOOT"])
    run(["mcopy", "-i", img, bz, "::/bzImage"])
    run(["mcopy", "-i", img, bz, "::/EFI/BOOT/BOOTX64.EFI"])
    size = img.stat().st_size
    if size != IMAGE_BYTES:
        sys.exit("image size %d != %d" % (size, IMAGE_BYTES))

    out = Path(args.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(img, out)

    kernel_release = ""
    try:
        kernel_release = subprocess.check_output(
            ["make", "-s", "-C", str(kernel), "kernelrelease"], text=True).strip()
    except subprocess.CalledProcessError:
        pass
    manifest = {
        "schema": 1,
        "image": str(out),
        "image_bytes": size,
        "image_sha256": sha256(out),
        "kernel_release": kernel_release,
        "kernel_bzimage_sha256": sha256(bz),
        "module_sha256": sha256(ko),
        "init_sha256": sha256(init_src),
        "ion_stub": bool(args.ion_stub),
        "kernel_rebuilt": not args.skip_kernel_build,
    }
    manifest_path = out.with_suffix(out.suffix + ".manifest.json")
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))
    print("manifest:", manifest_path)


if __name__ == "__main__":
    main()