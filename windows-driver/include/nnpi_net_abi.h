/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_NET_ABI_H
#define NNPI_NET_ABI_H
#include <stdint.h>
#include "nnpi_net_session.h"

/* 027 minimal PCIe-Ethernet channel ABI. BEGIN establishes the non-context
 * channel (CHANNEL_OP -> H2C/C2H CHANNEL_RB_OP -> ETH_CONFIG -> READY) in the
 * driver's worker, exactly once per session; QUERY reports state and is CPU-only
 * (including outside D0). Frame TX/RX are a separate data-plane ABI, not part of
 * this lifecycle header. Requests are exact 64-byte headers; responses 128 bytes.
 * No retries after lost replies. The kernel never trusts a user-declared G/E. */
#define NNPI_NET_ABI_VERSION 1U
#define NNPI_NET_CHAN_AUTO 0U /* kernel-assigned non-context id in 256..1023 */
#define NNPI_NET_BEGIN_IOCTL 0x8000e048U /* function 0x812, ANY_ACCESS */
#define NNPI_NET_QUERY_IOCTL 0x8000604cU /* function 0x813, READ_ACCESS */

/* Failure stages, parallel to nnpi_boot_abi.h. failed_stage is the first
 * failure; ntstatus is the associated status. */
#define NNPI_NET_FAIL_NONE     0U
#define NNPI_NET_FAIL_IDENTITY 1U
#define NNPI_NET_FAIL_CHANNEL  2U
#define NNPI_NET_FAIL_RB       3U
#define NNPI_NET_FAIL_ETH      4U
#define NNPI_NET_FAIL_BUDGET   5U
#define NNPI_NET_FAIL_TIMEOUT  6U
#define NNPI_NET_FAIL_PROTOCOL 7U
#define NNPI_NET_FAIL_MMIO     8U
#define NNPI_NET_FAIL_CLOSING  9U
#define NNPI_NET_FAIL_DMA      10U

/* BEGIN: session_id=0, current query G/E, chan_id=NNPI_NET_CHAN_AUTO or an
 * explicit non-context id, card_ip = inet_pton s_addr, mac = card MAC. QUERY:
 * all fields after size zero. Other fields echo the kernel-issued session/G/E.
 * card_ip 0 is invalid (the card rejects it). */
struct nnpi_net_request {
    uint32_t version, size;
    uint64_t session_id, generation, epoch;
    uint32_t chan_id, card_ip;
    uint8_t  mac[6];
    uint16_t reserved;
    uint32_t reserved2[4];
};

/* phase uses NNPI_NS_* from nnpi_net_session.h. READY means the channel, both
 * pools and ETH_CONFIG are confirmed; it never releases DMA and is NOT SSH
 * success. card_ip is the card-confirmed echo; mac echoes the configured MAC. */
struct nnpi_net_response {
    uint32_t version, size;
    uint64_t session_id, generation, epoch;
    uint32_t chan_id, phase;
    uint32_t chan_created, h2c_rb_published;
    uint32_t c2h_rb_published, eth_configured;
    uint32_t published_dma, stopped_dma;
    uint32_t card_ip, failed_stage;
    uint32_t ntstatus, observations;
    uint32_t worker_active, closing;
    uint32_t invalidated, budget_consumed;
    uint8_t  mac[6];
    uint16_t reserved2;
    uint32_t reserved[6];
};

typedef char nnpi_net_req_size[(sizeof(struct nnpi_net_request) == 64) ? 1 : -1];
typedef char nnpi_net_resp_size[(sizeof(struct nnpi_net_response) == 128) ? 1 : -1];
#endif /* NNPI_NET_ABI_H */
