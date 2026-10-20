/********************************************
 * Copyright (C) 2019-2021 Intel Corporation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 ********************************************/

#ifndef _SPHCS_HOSTWIN_H
#define _SPHCS_HOSTWIN_H

/*
 * Card->host "mapped window" for a CPU-Z-style hardware inventory.
 *
 * The host reads the BAR2 inbound-memory window directly over PCIe MMIO; the
 * card fills it through a debugfs write-only file. Layout inside the 64 MiB
 * inbound memory (BAR2):
 *   [0x0000 .. 0x4000)  crash dump (union nnp_inbound_mem, 16 KiB)
 *   [0x4000 .. 0x8000)  host window (struct nnp_hostwin, 16 KiB)
 *   [0x8000 .. 64 MiB)  P2P heap (unused in this build: no CARD_PLATFORM_BR)
 *
 * Constants MUST match the host-side nnpi_hostwin_abi.h. */
#define NNPI_HOSTWIN_OFFSET    0x4000U   /* 16 KiB past the crash dump */
#define NNPI_HOSTWIN_SIZE      0x4000U   /* 16 KiB window */
#define NNPI_HOSTWIN_MAGIC     0x68707348U /* 'Hsph' little-endian */
#define NNPI_HOSTWIN_DATA_MAX  (NNPI_HOSTWIN_SIZE - 8U)
#define NNPI_HOSTWIN_PAGES     (NNPI_HOSTWIN_SIZE / PAGE_SIZE)

/* The magic is written last and acts as the publish flag: once the host reads
 * a valid magic, data_size and data are already complete. */
struct nnp_hostwin {
	u32 magic;
	u32 data_size;
	u8  data[NNPI_HOSTWIN_DATA_MAX];
};

int  sphcs_hostwin_init(void);
void sphcs_hostwin_fini(void);

#endif /* _SPHCS_HOSTWIN_H */
