/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_SNAPSHOT_ABI_H
#define NNPI_SNAPSHOT_ABI_H
#include <stdint.h>
#include <stddef.h>
#include "nnpi_fifo_protocol.h"
#define NNPI_SNAPSHOT_VERSION 1U
#define NNPI_SNAPSHOT_IOCTL 0x8000600cU /* 0x803, BUFFERED, FILE_READ_ACCESS */
#define NNPI_SNAPSHOT_NONATOMIC 0x01U
#define NNPI_SNAPSHOT_DOORBELL_MAY_NOTIFY 0x02U
#define NNPI_SNAPSHOT_READONLY_NOCACHE_NX 0x04U
#define NNPI_SNAPSHOT_ATTRIBUTES 0x07U
#define NNPI_SNAPSHOT_COMMAND_VALID 0x01U
#define NNPI_SNAPSHOT_RESPONSE_VALID 0x02U
#define NNPI_SNAPSHOT_FIFO_VALID 0x03U
struct nnpi_snapshot_request { uint32_t version, size, reserved[2]; };
struct nnpi_snapshot_response {
    uint32_t version, size;
    uint64_t generation;
    uint32_t attributes, read_count, raw_valid, fifo_valid;
    uint32_t command_control, response_control, card_doorbell;
    uint32_t boot_state, error_code;
    uint32_t command_used, command_free, response_used, response_free;
    uint32_t reserved[3];
};
#define NNPI_SNAPSHOT_ASSERT(n, e) typedef char nnpi_snapshot_assert_##n[(e) ? 1 : -1]
NNPI_SNAPSHOT_ASSERT(request, sizeof(struct nnpi_snapshot_request) == 16);
NNPI_SNAPSHOT_ASSERT(response, sizeof(struct nnpi_snapshot_response) == 80);
NNPI_SNAPSHOT_ASSERT(generation, offsetof(struct nnpi_snapshot_response, generation) == 8);
NNPI_SNAPSHOT_ASSERT(raw, offsetof(struct nnpi_snapshot_response, command_control) == 32);
NNPI_SNAPSHOT_ASSERT(reserved, offsetof(struct nnpi_snapshot_response, reserved) == 68);
#undef NNPI_SNAPSHOT_ASSERT
static inline int nnpi_snapshot_request_valid(const struct nnpi_snapshot_request *r, size_t bytes)
{
    return r && bytes == sizeof(*r) && r->version == NNPI_SNAPSHOT_VERSION &&
        r->size == sizeof(*r) && !r->reserved[0] && !r->reserved[1];
}
static inline int nnpi_snapshot_buffers_valid(const struct nnpi_snapshot_request *r,
                                              size_t input_bytes, size_t output_bytes)
{
    return nnpi_snapshot_request_valid(r, input_bytes) &&
        output_bytes == sizeof(struct nnpi_snapshot_response);
}
/* Malformed FIFO is valid transport data, NOT a normal FIFO. Invalid derived
 * counts must be zero on the wire and rendered null by the CLI. */
static inline int nnpi_snapshot_response_valid(const struct nnpi_snapshot_response *r, size_t bytes)
{
    struct nnpi_fifo_snapshot f;
    uint32_t flags = 0, i;
    if (!r || bytes != sizeof(*r) || r->version != NNPI_SNAPSHOT_VERSION ||
        r->size != sizeof(*r) || !r->generation ||
        r->attributes != NNPI_SNAPSHOT_ATTRIBUTES || r->read_count != 3 || r->raw_valid != 7 ||
        r->boot_state != (r->card_doorbell & 0xffU) ||
        r->error_code != ((r->card_doorbell >> 12) & 0xfU)) return 0;
    if (nnpi_fifo_decode(r->command_control, &f) == NNPI_FIFO_OK) {
        flags |= NNPI_SNAPSHOT_COMMAND_VALID;
        if (r->command_used != f.used_qwords || r->command_free != f.free_qwords) return 0;
    } else if (r->command_used || r->command_free) return 0;
    if (nnpi_fifo_decode(r->response_control, &f) == NNPI_FIFO_OK) {
        flags |= NNPI_SNAPSHOT_RESPONSE_VALID;
        if (r->response_used != f.used_qwords || r->response_free != f.free_qwords) return 0;
    } else if (r->response_used || r->response_free) return 0;
    if (r->fifo_valid != flags) return 0;
    for (i = 0; i < 3; ++i) if (r->reserved[i]) return 0;
    return 1;
}
#endif
