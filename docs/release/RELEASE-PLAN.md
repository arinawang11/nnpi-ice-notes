# NNPI Community Driver — Release Plan

Status: draft for review  
Target: first official third-party release (v1.0.0)  
Date: 2026-10-20

## 1. Positioning

- Working product name: **NNPI Community Driver** (final name to be decided).
- Hardware: Intel NNP-I 1150, PCI `8086:45C6`, subsystem `45CE8086`, card kernel 5.7 vendor tree.
- Host: Windows 10/11 x64, KMDF. Validated build: Windows 11 26100 (24H2).
- License: GPL-2.0. Host driver and tools are `GPL-2.0-only`; the card module `sphcs` is
  `GPL-2.0-or-later` (Intel copyright, SPDX in every file).
- No affiliation with, sponsorship by, or endorsement from Intel. This is not an Intel product.
  The hardware is end-of-life and receives no vendor support; this project owns the stack.

## 2. Release scope (v1.0.0)

| Component | Artifact | Source | License |
|---|---|---|---|
| Host kernel driver | `nnpi_info.sys`, `nnpi_info.inf`, `nnpi_info.cat` | `pkg/driver-installer/driver-src/driver` | GPL-2.0-only |
| Diagnostics/control | `nnpi-info.exe` | `driver-src/tools/nnpi-info.cpp` | GPL-2.0-only |
| L2 bridge | `nnpi-net-bridge.exe` | `driver-src/tools/nnpi-net-bridge.cpp` | GPL-2.0-only |
| Card module | `sphcs.ko` (kernel 5.7) | `pkg/card-dma-lab/assets/nnpi/...` | GPL-2.0-or-later |
| Card image builder | initramfs + image tooling + reference init | `nnpi/storage`, `images/` | GPL-2.0 |
| Installer | `driver-installer.exe` | `pkg/driver-installer/src` | GPL-2.0 |
| Docs | README/INSTALL/SAFETY/CHANGELOG | `release/docs` | CC-BY-4.0 (text) |

Out of scope for v1.0.0: Mint root image redistribution, vendor firmware, any Intel confidential
material, TAP binaries (documented external dependency), Visual C++ runtime (documented
prerequisite or app-local per its own license).

## 3. Quality bar — release blockers

### Blocker A: card-side crash under sustained NBD load

Observed: card kernel crashes/reboots after ~7.3 GB of sequential NBD traffic; host observes
device loss (`failed_stage=0, closing=1`), `rawbridge` exits with `ERROR_NOT_READY(21)`.
Trigger chain starts with the card's 16-slot C2H ring reporting `c2h ringbuffer full`.

Working hypotheses, in order:

1. **ACK loss / handle leak**: C2H ACK frames are dropped by the same ring-full condition they
   are supposed to relieve. Each lost ACK leaks a busy handle and possibly a DMA mapping, which
   is fatal after enough traffic.
2. **Flow-control deadlock**: xmit drops frames without applying backpressure
   (`netif_stop_queue`), so TCP collapses and the card's own control traffic starves.
3. **DMA race / use-after-free** in the xmit/completion pair, surfacing only under ring pressure.

Fix direction (root cause, not ring enlargement):

- Never drop control/ACK traffic: reserve dedicated slots or a separate priority path.
- Apply real backpressure: stop the TX queue when the ring is low, wake on completion.
- Make every xmit error path unmap DMA and release the slot exactly once.
- Add a completion watchdog that detects stalled handles and reports instead of leaking.
- Prove with counters: `ring_full == 0`, `busy_handles == 0` after quiesce, zero leaks over 20 GB.

### Blocker B: host NMIs under load

Observed: bugcheck `0x80` (`NMI_HARDWARE_FAILURE`), bucket
`0x80_4F4454_GenuineIntel_NOERRREC`; WHEA `Id=1` informational MCE events correlate in time.
The NMI has no CPER record, so it cannot be attributed from the dump.

Acceptance: after Blocker A is fixed, a 24-hour soak with zero NMIs, zero WHEA fatal records,
and no `0x9F`. If NMIs persist with a stable card, treat as platform/hardware issue and document
supported-slot/power guidance; do not ship a release that bluescreens under normal load.

### Already fixed (keep as regression tests)

- `0x9F` power IRP hang: `NnpiBootBarrier`/`NnpiNetBarrier` bounded to 10 s waits and 2 s DMA
  polling, fault retained on timeout. Validated on a hung card (no bugcheck).
- INF encoding: UTF-16LE with BOM, `ClassName="NPU（NNPI）"` renders correctly.
- Image allowlist: three SHA-256 entries, driver + tool validator kept in sync.

## 4. Engineering infrastructure

### Source of truth

- Initialize a git repository at `E:\nnpi\nnpi-final-20260927\release\` (or promote the existing
  tree) with signed tags per release.
- Every binary must be reproducible from a tagged commit plus pinned toolchain.
- Keep `source-origin.json` provenance and extend it to the card module and images.

### Reproducible builds

Pin and record:

- Visual Studio Build Tools + WDK/SDK version (current: WDK 10.0.26100, MSVC 14.44).
- Card kernel tree hash (vendor 5.7), gcc 9.4, binutils, buildroot config.
- Python version for packaging scripts.
- One-command build: `release/build/build-all.ps1` producing hashes for every artifact.

### Tests

- Keep existing offline unit/harness tests (`driver-src/tests`).
- Hardware acceptance suite (scripted, idempotent):
  1. install / upgrade / rollback / uninstall;
  2. boot-load + phase-9 + net-begin;
  3. NBD read/write 1 GiB, 8 GiB, 20 GiB (raw and TCP);
  4. Mint root boot to multi-user, 1-hour run;
  5. power transitions: restart, shutdown, sleep/resume, surprise removal;
  6. Driver Verifier (all standard checks) for 30 minutes idle + 10 minutes load;
  7. 24-hour soak with counters and WHEA/NMI monitoring.
- Every test writes a machine-readable result and the exact build hash.

### Crash forensics

- Host: enable WHEA/AER logging, Driver Verifier, ETW; keep full dumps.
- Card: implement a persistent panic path before v1.0:
  - reserve a small RAM region and use `pstore`/`ramoops` if the kernel supports it;
  - otherwise a panic notifier that pushes a compact record over the command channel or to a
    dedicated DMA buffer the host can read after reset;
  - persist `/dev/kmsg` to the host continuously during stress (not only after switch_root).
- Add a stress harness that reproduces the 7.3 GB failure deterministically and records the
  last 200 card kernel messages.

## 5. Signing and distribution

- v1.0.0 ships **test-signed** unless a real code-signing identity is obtained. Test signing
  requires `bcdedit /set testsigning on`; document the security implications and rollback.
- Package layout: `nnpi-community-driver-1.0.0-win-x64.zip`
  - `driver/`, `tools/`, `card/` (module + build instructions), `docs/`, `source/` (or a
    separate `-source.tar.gz`), `SHA256SUMS`, `RELEASE-NOTES.md`, `LICENSE`, `NOTICE`.
- Sign SYS and CAT with the release certificate; publish the certificate fingerprint and the
  exact verification commands.
- Provide an uninstall path and a recovery guide (Safe Mode, test signing off, driver rollback).

## 6. Documentation set

- `README.md` — what it is, hardware, supported OS, quick start.
- `INSTALL.md` / `UNINSTALL.md` — step by step, including test signing.
- `SAFETY.md` — BSOD risk, test signing, recovery, data backup.
- `ARCHITECTURE.md` — host driver, card module, tunnel, NBD, image allowlist.
- `PROTOCOL.md` — boot ABI, net ABI, ETH ring layout, slot accounting.
- `TESTING.md` — how to run the acceptance suite and interpret counters.
- `CHANGELOG.md` — from 0.1 to 1.0, including the 0x9F fix and the card crash fix.
- `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md` (optional), `SECURITY.md`.
- `LICENSE` (GPL-2.0), `NOTICE` (Intel copyright, TAP, MSVC runtime, trademarks).
- `docs/corresponding-source.md` — how the GPL source is delivered with each binary release.

## 7. Release gates (v1.0.0 checklist)

- [ ] Blocker A fixed; 20 GB stress with zero ring-full, zero leaks, zero card reboots.
- [ ] Blocker B resolved or explicitly waived with hardware guidance; 24 h soak clean.
- [ ] Driver Verifier clean; power transitions clean; upgrade/rollback tested.
- [ ] Reproducible build from a tagged commit; all hashes match.
- [ ] GPL corresponding source complete, buildable, and shipped; NOTICE reviewed.
- [ ] Signing identity decided and applied; verification steps documented.
- [ ] Docs complete; known issues listed; safety warnings prominent.
- [ ] Release notes and checksums published; rollback instructions tested.

## 8. Decisions needed

1. Final product name and version scheme (proposal: NNPI Community Driver 1.0.0).
2. Distribution scope: host driver only, or host + card module + image builder.
3. Signing: test-signed with documentation, or obtain a real code-signing certificate.
4. Supported Windows versions and card kernel versions for v1.0.0.
5. Where the release is published (local archive, GitHub/GitLab, or both) and under what name.

---

## 9. Decisions (2026-10-20)

1. **Product**: NNPI Community Driver, first official release **1.0.0**.
2. **Device class**: **Coprocessor**. Windows has no built-in `Coprocessor` setup class, so the
   project owns a class GUID `{AD579C1F-5595-4EFA-B661-F64EAE1CDA5D}` and the INF declares
   `Class=Coprocessor`, `ClassName="Coprocessor"`. Implemented and verified on the reference
   machine (driver 0.3.0.34, oem55.inf, device class `Coprocessor`, interface enumeration OK).
   The installer must ensure the class key default value is `Coprocessor` (the ClassInstall32
   directive does this on a clean install; the installer should verify and repair it).
3. **Release bundle**: host driver + tools + card module + image builder + docs, test-signed,
   with the full documentation set.
4. **Host support**: Windows Server 2025 (build 26100) and the matching Windows 11 24H2 client
   branch. INF decoration `NTamd64.10.0...22000` covers both; document the tested builds.
5. **Card kernel**: flexible updates. Users may take a generic Linux image, run it through the
   **image builder**, and load the result. The builder compiles `sphcs` for the target kernel
   and injects it with the project init; no fixed vendor kernel is required.
6. **Release venue**: GitHub (existing project to be linked; release artifacts + source).

### Lessons recorded during the class change

- The INF `DriverVer` date is stamped from `nnpi_info.vcxproj` `<DateStamp>`, not from the INF
  source. The old fixed date (09/22/2026) caused Windows to outrank a newer version (0.3.0.33)
  in favor of an older-version package with a newer date (10/19/2026). The release build must
  keep `DateStamp` current and treat the date/version pair as part of the package identity.
- Changing the class of an installed device requires removing the device node (or a clean
  install); `pnputil /add-driver /install` alone does not re-class an existing node.
- A custom class key is created during install, but its display name must be verified and set
  (`HKLM\SYSTEM\CurrentControlSet\Control\Class\{GUID}` default value).

## 10. Image builder and allowlist (design)

### Builder inputs

- Target kernel: `bzImage` + kernel headers/`Module.symvers` (any modern x86_64 kernel with
  `CONFIG_BLK_DEV_NBD`, `CONFIG_EXT4_FS`, `CONFIG_PCI_MSI`).
- Card module source (`sphcs`, GPL-2.0-or-later) and the project init/listener binaries.
- Optional root filesystem: initramfs or a disk image to be mounted over NBD.

### Builder outputs

- `sphcs.ko` built against the target kernel (vermagic checked).
- Card boot image (FAT/bzImage layout accepted by the card) with the init that:
  loads `sphcs`, brings up `sphcschan0`, attaches the NBD root, and starts the user root.
- A manifest: kernel version, module vermagic, image SHA-256, build inputs and hashes.

### Allowlist model (to be decided before 1.0)

The current driver embeds three SHA-256 image hashes (driver + tool validator kept in sync).
Flexible user images require an update mechanism. Options:

1. **Admin-provisioned user key (recommended)**: the driver trusts a public key stored at
   install time (admin only). The builder signs `(image_sha256, metadata)` with the user's
   private key; a small CLI installs/updates the signed allowlist. The trust root is the user,
   not the project.
2. **Project-key-signed allowlist**: only the project can authorize images; flexible for the
   maintainers, not for users.
3. **Developer mode**: admin-only, explicit opt-in (registry flag + test signing), documented
   as unsafe; accepts any image hash.

For 1.0.0, implement option 1 with a documented fallback to option 3 for development.
The tool's compiled validator constant must be removed or driven by the same allowlist file.

### Status

- Device class change: **done** (0.3.0.34 dev build on the reference machine).
- Card module defect fix: pending (see ROOTCAUSE-NOTES.md).
- Builder: design only; implementation is part of the 1.0 milestone.
- GitHub repository: URL to be provided; release automation to be added there.
