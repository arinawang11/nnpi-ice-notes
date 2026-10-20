# Safety and recovery

This driver talks to PCIe hardware and loads kernel code. A defect can crash
the host (bugcheck) or leave the device unresponsive.

## Test signing

Release 1.0.0 packages are test-signed. Test signing lowers the platform's
driver trust bar and must be treated as a development configuration:

```
bcdedit /set testsigning on     # enable
bcdedit /set testsigning off    # disable when no longer needed
```

## Before testing

- Back up important data.
- Keep a recovery path: another administrator account, Safe Mode access, and
  the ability to disable test signing.
- Do not run sustained storage or network workloads on a machine you cannot
  afford to reboot.

## If the host bugchecks

1. Let the machine restart.
2. Disable test signing and remove the driver package if the crash repeats.
3. Collect `C:\Windows\MEMORY.DMP` and the WER report before overwriting them.
4. Report the bugcheck code, the driver version and the workload.

## Known limitations (pre-1.0)

- Sustained high-throughput NBD traffic can destabilize the card; the card
  module's C2H handle lifecycle is being fixed before 1.0.0.
- The device class is a project-defined `Coprocessor` class; it is not a
  Microsoft NPU/DirectML class and makes no compute-accelerator claims.