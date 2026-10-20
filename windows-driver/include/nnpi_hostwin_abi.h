/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_HOSTWIN_ABI_H
#define NNPI_HOSTWIN_ABI_H
#include <stdint.h>
#include <stddef.h>
/* Card->host "mapped window" contract.
 *
 * The card (NNP-I Ice Lake core) runs a CPU-Z-style hardware inventory program
 * and writes its text output into the BAR2 inbound memory window. The host
 * reads that window directly through BAR2 MMIO and renders it. This ABI only
 * carries the READ side; the card writes via its own debugfs file (see the
 * sphcs hostwin module) and the two sides share these layout constants.
 *
 * Window layout inside the 64 MiB inbound memory (BAR2):
 *   [0x0000 .. 0x4000)  crash dump (union nnp_inbound_mem, 16 KiB)
 *   [0x4000 .. 0x8000)  host window (struct nnp_hostwin, 16 KiB)
 *   [0x8000 .. 64 MiB)  P2P heap (unused in the diag1150 build: no CARD_PLATFORM_BR)
 *
 * Constants MUST match the card-side sphcs hostwin module. */
#define NNPI_HOSTWIN_OFFSET 0x4000U      /* 16 KiB past crash dump */
#define NNPI_HOSTWIN_SIZE   0x4000U      /* 16 KiB window */
#define NNPI_HOSTWIN_MAGIC  0x68707348U  /* 'Hsph' little-endian, host window */
#define NNPI_HOSTWIN_MAGIC_H2C 0x48633248U /* 'H2cH' LE: host writes, card reads */
#define NNPI_HOSTWIN_DATA_MAX (NNPI_HOSTWIN_SIZE - 8U)

#define NNPI_HOSTWIN_VERSION 1U
#define NNPI_HOSTWIN_QUERY_IOCTL 0x80006080U /* 0x820, BUFFERED, READ */
#define NNPI_HOSTWIN_WRITE_IOCTL 0x8000A084U /* 0x821, BUFFERED, WRITE */

#define NNPI_HOSTWIN_OK 0U
#define NNPI_HOSTWIN_ASSOCIATION 1U
#define NNPI_HOSTWIN_RANGE 2U
#define NNPI_HOSTWIN_MAP_FAILED 3U
#define NNPI_HOSTWIN_REMOVED 4U
#define NNPI_HOSTWIN_NOT_READY 5U

/* Cache-only request: no caller-selected BAR, address or offset. */
struct nnpi_hostwin_request {
    uint32_t version, size;
    uint32_t reserved[2];
};

/* Fixed-size snapshot of the raw card window plus the fields the card stamped
 * into it. No pointers, no DMA addresses; the window is copied out verbatim. */
struct nnpi_hostwin_response {
    uint32_t version, size;
    uint64_t generation;
    uint32_t reason, data_size, magic_valid, read_bytes;
    uint8_t window[NNPI_HOSTWIN_SIZE];
};

#define NNPI_HW_ASSERT(n, e) typedef char nnpi_hw_assert_##n[(e) ? 1 : -1]
NNPI_HW_ASSERT(request, sizeof(struct nnpi_hostwin_request) == 16);
NNPI_HW_ASSERT(response, sizeof(struct nnpi_hostwin_response) == 32 + NNPI_HOSTWIN_SIZE);
#undef NNPI_HW_ASSERT

static inline int nnpi_hostwin_request_valid(const struct nnpi_hostwin_request *r, size_t n)
{
    return r && n == sizeof(*r) && r->version == NNPI_HOSTWIN_VERSION &&
        r->size == sizeof(*r) && !r->reserved[0] && !r->reserved[1];
}

static inline int nnpi_hostwin_response_valid(const struct nnpi_hostwin_response *r, size_t n)
{
    if (!r || n != sizeof(*r) || r->version != NNPI_HOSTWIN_VERSION ||
        r->size != sizeof(*r) || r->reason > NNPI_HOSTWIN_NOT_READY ||
        r->magic_valid > 1 || r->read_bytes > NNPI_HOSTWIN_SIZE)
        return 0;
    if (r->reason == NNPI_HOSTWIN_OK) {
        if (r->data_size > NNPI_HOSTWIN_DATA_MAX || r->magic_valid != 1)
            return 0;
    } else {
        if (r->data_size || r->read_bytes || r->magic_valid)
            return 0;
    }
    return 1;
}

/* Host->card write side of the same shared window. The host stamps the H2C
 * magic (last) after the payload so the card treats a matching magic as proof
 * the report is complete. data_size bounds the payload; the driver copies only
 * data[0..data_size) into the BAR2 window. No address/offset is user-selectable. */
struct nnpi_hostwin_write_request {
    uint32_t version, size;
    uint32_t data_size;
    uint32_t reserved[3];
    uint8_t data[NNPI_HOSTWIN_DATA_MAX];
};

struct nnpi_hostwin_write_response {
    uint32_t version, size;
    uint64_t generation;
    uint32_t reason, written_bytes;
    uint32_t reserved[2];
};

#define NNPI_HWW_ASSERT(n, e) typedef char nnpi_hww_assert_##n[(e) ? 1 : -1]
NNPI_HWW_ASSERT(wrequest, sizeof(struct nnpi_hostwin_write_request) == 8 + 4 + 12 + NNPI_HOSTWIN_DATA_MAX);
NNPI_HWW_ASSERT(wresponse, sizeof(struct nnpi_hostwin_write_response) == 32);
#undef NNPI_HWW_ASSERT

static inline int nnpi_hostwin_write_request_valid(const struct nnpi_hostwin_write_request *r, size_t n)
{
    return r && n == sizeof(*r) && r->version == NNPI_HOSTWIN_VERSION &&
        r->size == sizeof(*r) && r->data_size <= NNPI_HOSTWIN_DATA_MAX &&
        !r->reserved[0] && !r->reserved[1] && !r->reserved[2];
}
#endif
