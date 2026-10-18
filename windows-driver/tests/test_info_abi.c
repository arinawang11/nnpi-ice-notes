/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../include/nnpi_info_abi.h"
#include "../include/nnpi_boot_abi.h"
#include <stddef.h>
int main(void)
{
    struct nnpi_info_request q = {1, 16, {0, 0}};
    struct nnpi_info_response r, bad;
    unsigned int i;
    /* Wire layout is shared by kernel and CLI, never native pointer-sized. */
    assert(sizeof(struct nnpi_boot_request) == 64);
    assert(sizeof(struct nnpi_boot_response) == 192);
    assert(offsetof(struct nnpi_boot_request, chunk_bytes) == 48);
    assert(offsetof(struct nnpi_boot_response, sha256) == 160);
    assert(NNPI_BOOT_BEGIN_IOCTL == ((UINT32_C(0x8000) << 16) | (3U << 14) | (0x80dU << 2)));
    assert(NNPI_BOOT_QUERY_IOCTL == ((UINT32_C(0x8000) << 16) | (1U << 14) | (0x811U << 2)));
    assert(NNPI_BOOT_IMAGE_BYTES / NNPI_BOOT_CHUNK_MAX == 180);
    assert(NNPI_BOOT_IMAGE_BYTES % NNPI_BOOT_CHUNK_MAX == 512);
    assert(nnpi_info_request_valid(&q, 16));
    assert(!nnpi_info_request_valid(&q, 15));
    q.reserved[1] = 1; assert(!nnpi_info_request_valid(&q, 16));
    memset(&r, 0, sizeof(r));
    r.version = 1; r.size = sizeof(r); r.driver_version = 1;
    r.flags = 3; r.vendor = 0x8086; r.device = 0x45c6;
    r.identity_source = 1; r.generation = 1;
    assert(nnpi_info_response_valid(&r, sizeof(r)));
    r.resource_count = 16;
    for (i = 0; i < 16; i++) {
        r.resources[i].translated_resource_index = i;
        r.resources[i].type = 3; r.resources[i].valid = 1;
        r.resources[i].start = 0x100000000ULL + i * 4096;
        r.resources[i].length = 4096;
    }
    assert(nnpi_info_response_valid(&r, sizeof(r)));
    bad = r; bad.resource_count = 17; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    bad = r; bad.resources[0].type = 7; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    bad = r; bad.resources[0].start = UINT64_MAX; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    bad = r; bad.resources[0].length = 0; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    bad = r; bad.resources[0].translated_resource_index = 1; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    bad = r; bad.resources[0].type = 2; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    bad.resources[0].start = bad.resources[0].length = bad.resources[0].valid = 0;
    assert(nnpi_info_response_valid(&bad, sizeof(bad)));
    for (i = 0; i < 6; i++) { bad = r; bad.reserved[i] = 1; assert(!nnpi_info_response_valid(&bad, sizeof(bad))); }
    bad = r; bad.resource_count = 0; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    assert(!nnpi_info_response_valid(&r, sizeof(r) - 1));
    /* Regression from task007: preserve Memory then DevicePrivate as two
     * distinct original descriptors; private data is not an address range. */
    memset(r.resources, 0, sizeof(r.resources));
    r.resource_count = 2;
    r.resources[0].type = 3; r.resources[0].valid = 1;
    r.resources[0].start = 0x100000000ULL; r.resources[0].length = 4096;
    r.resources[1].translated_resource_index = 1; r.resources[1].type = 0x81;
    assert(nnpi_info_response_valid(&r, sizeof(r)));
    bad = r; bad.resources[1].valid = 1; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    bad = r; bad.resources[1].start = 1; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    bad = r; bad.resources[1].length = 1; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    bad = r; bad.resources[1].type = 0x82; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    bad = r; bad.resources[1].type = 10; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    bad = r; bad.resources[1].translated_resource_index = 0; assert(!nnpi_info_response_valid(&bad, sizeof(bad)));
    puts("PASS portable INFO ABI and DevicePrivate regression; NOT WDK compilation or Windows execution");
    return 0;
}
