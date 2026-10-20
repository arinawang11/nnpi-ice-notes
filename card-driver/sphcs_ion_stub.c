/* Stubs for the vendor ION allocator symbols.
 * Build with ION_STUB=1 on kernels that do not provide the vendor ION driver.
 * The card runs with disable_p2p=1, so the P2P heap paths are not exercised.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/types.h>
#include <linux/list.h>
#include <linux/scatterlist.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ion_exp.h>

void *ion_kbuf_alloc(u64 size, u32 alignment, const char *heap_name,
                     u32 flags, struct sg_table **sg_table, void **vaddr)
{
    return NULL;
}
void ion_kbuf_free(void *buf_handle) { }
int ion_get_buf_user_data(phys_addr_t addr, u64 *user_data) { return -ENOSYS; }
void *ion_chunk_heap_setup(struct list_head *mem_regions_list, const char *heap_name) { return NULL; }
void ion_chunk_heap_remove(void *ion_heap) { }