# Installing the NNPI Community Driver

Target: Windows Server 2025 (build 26100) or Windows 11 24H2, x64.

## Prerequisites

- Administrator rights.
- Test signing enabled (the 1.0.0 packages are test-signed):

```
bcdedit /set testsigning on
```

Reboot after enabling test signing.

## Install

1. Open an elevated PowerShell in the package directory.
2. Add and install the driver package:

```
pnputil /add-driver drivers\nnpi\nnpi_info.inf /install
```

3. Verify the device:

```
Get-PnpDevice -PresentOnly | Where-Object InstanceId -like '*VEN_8086&DEV_45C6*'
```

Expected: `NNPI PCIe Ethernet`, class `Coprocessor`, status `OK`.

4. Optional diagnostics:

```
tools\nnpi-info.exe list
```

## Uninstall

```
pnputil /remove-device "PCI\VEN_8086&DEV_45C6&SUBSYS_45CE8086&REV_00\<instance>"
pnputil /delete-driver oemNN.inf /uninstall /force
```

Replace `oemNN.inf` with the published name shown by `pnputil /enum-drivers`.

## Notes

- `pnputil /add-driver /install` does not change the class of an existing
  device node. A clean install, or removing the device node and rescanning,
  is required when migrating from a previous class.
- The driver does not auto-install; installation is always explicit.