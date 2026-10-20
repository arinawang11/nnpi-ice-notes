/* Vendored from the NNP-I card kernel tree (GPL-2.0) so the module can build
 * against kernels that do not carry the vendor ION allocator header. */
#ifndef _ION_EXP_H
#define _ION_EXP_H

struct mem_region {
	phys_addr_t start;
	size_t size;
	struct list_head list;
};
extern void *ion_chunk_heap_setup(struct list_head *mem_regions_list,
				  const char *heap_name);
extern void ion_chunk_heap_remove(void *ion_heap);

extern int ion_get_buf_user_data(phys_addr_t addr, u64 *user_data);

extern void *ion_kbuf_alloc(u64 size,
			    u32 alignment,
			    const char *heap_name,
			    u32 flags,
			    struct sg_table **sg_table,
			    void **vaddr);
extern void ion_kbuf_free(void *buf_handle);
#endif /* _ION_EXP_H */