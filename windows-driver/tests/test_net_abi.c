/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "../include/nnpi_net_abi.h"

int main(void)
{
    /* Wire layout is shared by kernel and CLI, never native pointer-sized. */
    assert(sizeof(struct nnpi_net_request) == 64);
    assert(sizeof(struct nnpi_net_response) == 128);
    /* Request field offsets: identity then network inputs. */
    assert(offsetof(struct nnpi_net_request, version) == 0);
    assert(offsetof(struct nnpi_net_request, session_id) == 8);
    assert(offsetof(struct nnpi_net_request, generation) == 16);
    assert(offsetof(struct nnpi_net_request, epoch) == 24);
    assert(offsetof(struct nnpi_net_request, chan_id) == 32);
    assert(offsetof(struct nnpi_net_request, card_ip) == 36);
    assert(offsetof(struct nnpi_net_request, mac) == 40);
    /* Response field offsets: identity, lifecycle, then DMA/status. */
    assert(offsetof(struct nnpi_net_response, chan_id) == 32);
    assert(offsetof(struct nnpi_net_response, phase) == 36);
    assert(offsetof(struct nnpi_net_response, chan_created) == 40);
    assert(offsetof(struct nnpi_net_response, h2c_rb_published) == 44);
    assert(offsetof(struct nnpi_net_response, c2h_rb_published) == 48);
    assert(offsetof(struct nnpi_net_response, eth_configured) == 52);
    assert(offsetof(struct nnpi_net_response, published_dma) == 56);
    assert(offsetof(struct nnpi_net_response, stopped_dma) == 60);
    assert(offsetof(struct nnpi_net_response, card_ip) == 64);
    assert(offsetof(struct nnpi_net_response, failed_stage) == 68);
    assert(offsetof(struct nnpi_net_response, ntstatus) == 72);
    assert(offsetof(struct nnpi_net_response, observations) == 76);
    assert(offsetof(struct nnpi_net_response, worker_active) == 80);
    assert(offsetof(struct nnpi_net_response, closing) == 84);
    assert(offsetof(struct nnpi_net_response, invalidated) == 88);
    assert(offsetof(struct nnpi_net_response, budget_consumed) == 92);
    assert(offsetof(struct nnpi_net_response, mac) == 96);
    assert(offsetof(struct nnpi_net_response, reserved2) == 102);
    /* IOCTL encodings follow the established device-type/method pattern. */
    assert(NNPI_NET_BEGIN_IOCTL == ((UINT32_C(0x8000) << 16) | (3U << 14) | (0x812U << 2)));
    assert(NNPI_NET_QUERY_IOCTL == ((UINT32_C(0x8000) << 16) | (1U << 14) | (0x813U << 2)));
    /* Constants. */
    assert(NNPI_NET_ABI_VERSION == 1);
    assert(NNPI_NET_CHAN_AUTO == 0);
    assert(NNPI_NET_FAIL_NONE == 0 && NNPI_NET_FAIL_DMA == 10);
    /* Phase values from nnpi_net_session.h remain stable at the ABI boundary. */
    assert(NNPI_NS_NEW == 0 && NNPI_NS_INITIAL == 1 && NNPI_NS_READY == 13);
    assert(NNPI_NS_FINISHED == 15);
    /* A zeroed BEGIN request is a valid shape (chan auto, no reserved set). */
    {
        struct nnpi_net_request req;
        memset(&req, 0, sizeof(req));
        req.version = NNPI_NET_ABI_VERSION;
        req.size = sizeof(req);
        assert(req.chan_id == NNPI_NET_CHAN_AUTO);
        assert(req.reserved == 0 && req.reserved2[0] == 0 && req.reserved2[3] == 0);
    }
    puts("PASS portable NET ABI layout, IOCTL encodings and phase constants; NOT WDK compilation or Windows execution");
    return 0;
}
