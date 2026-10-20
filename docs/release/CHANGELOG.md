# Changelog

## Unreleased (0.3.0.34)

- Device class changed from `NNPIAccelerator` to the project-defined
  `Coprocessor` class (`{AD579C1F-5595-4EFA-B661-F64EAE1CDA5D}`).
- Power-transition fix: bounded waits in `NnpiBootBarrier` / `NnpiNetBarrier`
  prevent the 0x9F `DRIVER_POWER_STATE_FAILURE` hang when the card is
  unresponsive.
- INF is stamped from `nnpi_version.h`; the build date is controlled by
  `<DateStamp>` in the driver project (a stale date caused Windows to
  outrank a newer package).
- Synchronized the host driver sources with the current development tree
  (boot/net ABI, DMA, allowlist, tools).

## Earlier

See the repository history and `docs/experiment-log.md` for the boot-chain
and DELPHI investigations.