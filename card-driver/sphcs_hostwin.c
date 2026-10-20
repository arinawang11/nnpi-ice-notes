/********************************************
 * Copyright (C) 2019-2021 Intel Corporation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 ********************************************/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/vmalloc.h>
#include <asm/page.h>

#include "sphcs_hostwin.h"
#include "sph_log.h"
#include "sphcs_cs.h"
#include "nnp_inbound_mem.h"

static void __iomem *hostwin_vaddr;
static struct dentry *hostwin_dentry;

/* Write-only debugfs: the card fills the window with the inventory text the
 * user-space CPU-Z program produced. Each write replaces the previous content;
 * the host reads the result over BAR2, never through this file. */
static ssize_t hostwin_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct nnp_hostwin *hw = hostwin_vaddr;
	size_t n;

	if (!hw)
		return -ENODEV;
	if (!count)
		return 0;

	n = min(count, (size_t)NNPI_HOSTWIN_DATA_MAX);
	if (copy_from_user(hw->data, buf, n))
		return -EFAULT;

	/* Publish order: data and size first, magic last. The host treats a
	 * valid magic as proof the payload is complete. */
	hw->data_size = (u32)n;
	wmb();
	hw->magic = NNPI_HOSTWIN_MAGIC;

	return (ssize_t)count;
}

static const struct file_operations hostwin_fops = {
	.open = simple_open,
	.write = hostwin_write,
};

int sphcs_hostwin_init(void)
{
	struct page *pages[NNPI_HOSTWIN_PAGES];
	int i;

	if (!g_the_sphcs || !g_the_sphcs->inbound_mem_dma_addr)
		return -ENODEV;
	if (g_the_sphcs->inbound_mem_size <
	    NNPI_HOSTWIN_OFFSET + NNPI_HOSTWIN_SIZE) {
		sph_log_err(START_UP_LOG, "Inbound memory too small for host window\n");
		return -ENOMEM;
	}

	for (i = 0; i < NNPI_HOSTWIN_PAGES; i++)
		pages[i] = pfn_to_page(__phys_to_pfn(
			dma_to_phys(g_the_sphcs->hw_device,
				    g_the_sphcs->inbound_mem_dma_addr +
				    NNPI_HOSTWIN_OFFSET + i * PAGE_SIZE)));

	hostwin_vaddr = vm_map_ram(pages, NNPI_HOSTWIN_PAGES, -1, PAGE_KERNEL);
	if (!hostwin_vaddr) {
		sph_log_err(START_UP_LOG, "Failed to map host window region\n");
		return -ENOMEM;
	}
	memset(hostwin_vaddr, 0, NNPI_HOSTWIN_SIZE);

	if (g_the_sphcs->debugfs_dir) {
		hostwin_dentry = debugfs_create_file("hostwin", 0200,
						     g_the_sphcs->debugfs_dir,
						     NULL, &hostwin_fops);
	}

	sph_log_info(START_UP_LOG, "Host window at inbound+0x%x\n",
		     NNPI_HOSTWIN_OFFSET);
	return 0;
}

void sphcs_hostwin_fini(void)
{
	if (hostwin_dentry) {
		debugfs_remove(hostwin_dentry);
		hostwin_dentry = NULL;
	}
	if (hostwin_vaddr) {
		vm_unmap_ram(hostwin_vaddr, NNPI_HOSTWIN_PAGES);
		hostwin_vaddr = NULL;
	}
}
