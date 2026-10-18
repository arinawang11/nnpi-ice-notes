/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_RUNTIME_INFO_ABI_H
#define NNPI_RUNTIME_INFO_ABI_H
#include <stdint.h>
#include <stddef.h>
#define NNPI_RUNTIME_INFO_VERSION 1u
#define NNPI_RUNTIME_INFO_IOCTL 0x80006050u /* function 0x814, BUFFERED, READ */
#define NNPI_RUNTIME_NET_DISABLED_PAGE_LIST_UNIMPLEMENTED 1u /* r12 historical code */
#define NNPI_RUNTIME_NET_DISABLED_LIFECYCLE_UNVERIFIED 2u /* r13 historical code */
#define NNPI_RUNTIME_NET_READY 0u
#define NNPI_RUNTIME_NET_NOT_READY 3u
#define NNPI_RUNTIME_NET_CAP_MISSING 4u
#define NNPI_RUNTIME_NET_SESSION_USED 5u
#define NNPI_RUNTIME_NET_CLOSING 6u
#define NNPI_RUNTIME_NET_UNEXPECTED_CONFIG 7u
/* net_enabled reports whether a new BEGIN can currently be admitted, not link
 * readiness or session activity. Disabled historical reasons 1/2 remain valid.
 * Cached evidence only. Healthy means compatible and runtime loop operational,
 * not networking/SSH readiness. IRQ valid bits: bit0 mask, bit1 status.
 * Generation/epoch may be zero before preparation. No addresses or MMIO. */
struct nnpi_runtime_info_request { uint32_t version, size, reserved[2]; };
struct nnpi_runtime_info_response {
    uint32_t version, size;                    /* 0 */
    uint64_t generation, epoch;                /* 8,16 */
    uint32_t compatible, healthy;              /* 24,28 */
    uint32_t protocol_version, firmware_version; /* 32,36 */
    uint32_t channel_version, last_stage;       /* 40,44 */
    uint32_t last_failure_reason, last_failure_status; /* 48,52 */
    uint64_t keepalive_updates;                /* 56 */
    uint32_t irq_mask, irq_status;             /* 64,68 */
    uint32_t irq_reason, irq_valid;            /* 72,76 */
    uint32_t partial_count, net_enabled;       /* 80,84 */
    uint32_t net_disabled_reason, reserved0;   /* 88,92 */
    uint64_t unexpected_net_config_count;      /* 96 */
    uint64_t last_unexpected_net_config;       /* 104, original QW */
    uint64_t last_event;                       /* 112, original opcode4 QW */
    uint32_t reserved[2];                      /* 120 */
};
#define NNPI_RI_ASSERT(n,e) typedef char nnpi_ri_assert_##n[(e)?1:-1]
NNPI_RI_ASSERT(request,sizeof(struct nnpi_runtime_info_request)==16);
NNPI_RI_ASSERT(response,sizeof(struct nnpi_runtime_info_response)==128);
NNPI_RI_ASSERT(generation,offsetof(struct nnpi_runtime_info_response,generation)==8);
NNPI_RI_ASSERT(epoch,offsetof(struct nnpi_runtime_info_response,epoch)==16);
NNPI_RI_ASSERT(compatible,offsetof(struct nnpi_runtime_info_response,compatible)==24);
NNPI_RI_ASSERT(healthy,offsetof(struct nnpi_runtime_info_response,healthy)==28);
NNPI_RI_ASSERT(protocol,offsetof(struct nnpi_runtime_info_response,protocol_version)==32);
NNPI_RI_ASSERT(firmware,offsetof(struct nnpi_runtime_info_response,firmware_version)==36);
NNPI_RI_ASSERT(channel,offsetof(struct nnpi_runtime_info_response,channel_version)==40);
NNPI_RI_ASSERT(stage,offsetof(struct nnpi_runtime_info_response,last_stage)==44);
NNPI_RI_ASSERT(failure,offsetof(struct nnpi_runtime_info_response,last_failure_reason)==48);
NNPI_RI_ASSERT(status,offsetof(struct nnpi_runtime_info_response,last_failure_status)==52);
NNPI_RI_ASSERT(irq_mask,offsetof(struct nnpi_runtime_info_response,irq_mask)==64);
NNPI_RI_ASSERT(irq_status,offsetof(struct nnpi_runtime_info_response,irq_status)==68);
NNPI_RI_ASSERT(irq_reason,offsetof(struct nnpi_runtime_info_response,irq_reason)==72);
NNPI_RI_ASSERT(irq_valid,offsetof(struct nnpi_runtime_info_response,irq_valid)==76);
NNPI_RI_ASSERT(partial,offsetof(struct nnpi_runtime_info_response,partial_count)==80);
NNPI_RI_ASSERT(net,offsetof(struct nnpi_runtime_info_response,net_enabled)==84);
NNPI_RI_ASSERT(net_reason,offsetof(struct nnpi_runtime_info_response,net_disabled_reason)==88);
NNPI_RI_ASSERT(reserved0,offsetof(struct nnpi_runtime_info_response,reserved0)==92);
NNPI_RI_ASSERT(last_unexpected,offsetof(struct nnpi_runtime_info_response,last_unexpected_net_config)==104);
NNPI_RI_ASSERT(keepalive,offsetof(struct nnpi_runtime_info_response,keepalive_updates)==56);
NNPI_RI_ASSERT(unexpected,offsetof(struct nnpi_runtime_info_response,unexpected_net_config_count)==96);
NNPI_RI_ASSERT(event,offsetof(struct nnpi_runtime_info_response,last_event)==112);
NNPI_RI_ASSERT(reserved,offsetof(struct nnpi_runtime_info_response,reserved)==120);
#undef NNPI_RI_ASSERT
static inline int nnpi_runtime_info_request_valid(const struct nnpi_runtime_info_request *r,size_t bytes)
{
    return r && bytes==sizeof(*r) && r->version==NNPI_RUNTIME_INFO_VERSION &&
        r->size==sizeof(*r) && !r->reserved[0] && !r->reserved[1];
}
static inline int nnpi_runtime_info_buffers_valid(const struct nnpi_runtime_info_request *r,size_t in,size_t out)
{
    return nnpi_runtime_info_request_valid(r,in) && out==sizeof(struct nnpi_runtime_info_response);
}
static inline int nnpi_runtime_info_response_valid(const struct nnpi_runtime_info_response *r,size_t bytes)
{
    return r && bytes==sizeof(*r) && r->version==NNPI_RUNTIME_INFO_VERSION &&
        r->size==sizeof(*r) && r->generation!=UINT64_MAX && r->epoch!=UINT64_MAX &&
        r->compatible<=1 && r->healthy<=1 &&
        (!r->healthy || r->compatible) && r->protocol_version<=0xffffu &&
        r->firmware_version<=0xffffu && r->channel_version<=0xffffu &&
        r->irq_valid<=3 && r->partial_count<=3 && r->net_enabled<=1 &&
        (r->net_enabled ?
         (r->net_disabled_reason==NNPI_RUNTIME_NET_READY && r->compatible && r->healthy) :
         (r->net_disabled_reason==NNPI_RUNTIME_NET_DISABLED_PAGE_LIST_UNIMPLEMENTED ||
          r->net_disabled_reason==NNPI_RUNTIME_NET_DISABLED_LIFECYCLE_UNVERIFIED ||
          r->net_disabled_reason==NNPI_RUNTIME_NET_NOT_READY ||
          r->net_disabled_reason==NNPI_RUNTIME_NET_CAP_MISSING ||
          r->net_disabled_reason==NNPI_RUNTIME_NET_SESSION_USED ||
          r->net_disabled_reason==NNPI_RUNTIME_NET_CLOSING ||
          r->net_disabled_reason==NNPI_RUNTIME_NET_UNEXPECTED_CONFIG)) &&
        !r->reserved0 && !r->reserved[0] && !r->reserved[1];
}
#endif
