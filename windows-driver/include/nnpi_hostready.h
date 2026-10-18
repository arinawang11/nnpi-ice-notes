/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_HOSTREADY_H
#define NNPI_HOSTREADY_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "nnpi_fifo_protocol.h"
#define NNPI_HOSTREADY_VERSION 1U
#define NNPI_HOSTREADY_SUBMIT_IOCTL 0x8000e024U
#define NNPI_HOSTREADY_OBSERVE_IOCTL 0x8000e028U
#define NNPI_HOSTREADY_CONFIRM 0x32354139U
#define NNPI_HOSTREADY_LIMIT 20U
/* Fixed action only: no address, offset, value or user-selected poll count. */
struct nnpi_hostready_request { uint32_t version, size; uint64_t generation, epoch; uint32_t confirmation, reserved; };
enum nnpi_hostready_phase { NNPI_HR_IDLE=0, NNPI_HR_WRITTEN=1, NNPI_HR_READY=2, NNPI_HR_STOPPED=3, NNPI_HR_EXHAUSTED=4 };
struct nnpi_hostready_response {
    uint32_t version, size;
    uint64_t generation, epoch;
    uint32_t phase, writes, observations, reads;
    uint32_t host_before, command_before, response_before, card_before, card_last;
    uint32_t reserved;
};
typedef char nnpi_hr_req_size[(sizeof(struct nnpi_hostready_request)==32)?1:-1];
typedef char nnpi_hr_rsp_size[(sizeof(struct nnpi_hostready_response)==64)?1:-1];
static inline int nnpi_hostready_request_valid(const struct nnpi_hostready_request *r, size_t in, size_t out) {
    return r && in==sizeof(*r) && out==sizeof(struct nnpi_hostready_response) &&
        r->version==1 && r->size==sizeof(*r) && r->generation && r->epoch &&
        r->generation!=UINT64_MAX && r->epoch!=UINT64_MAX &&
        r->confirmation==NNPI_HOSTREADY_CONFIRM && !r->reserved;
}
/* No non-target bits may be replayed (self-reset, P2P, error, unknown bits).
 * Only the not-ready baseline is eligible; already-ready host is not rewritten. */
static inline int nnpi_hostready_baseline(uint32_t host, uint32_t card, uint32_t command, uint32_t response) {
    struct nnpi_fifo_snapshot c, r;
    return host==0 && (card==0 || card==1) &&
        nnpi_fifo_decode(command,&c)==NNPI_FIFO_OK && !c.used_qwords &&
        nnpi_fifo_decode(response,&r)==NNPI_FIFO_OK && !r.used_qwords;
}
/* Caller serializes with D0/PnP and uses a monotonic 100ns clock. */
static inline int nnpi_hostready_can_observe(const struct nnpi_hostready_response *r,
    uint64_t generation, uint64_t epoch, uint64_t now, uint64_t previous) {
    return r && r->phase==NNPI_HR_WRITTEN && r->observations<NNPI_HOSTREADY_LIMIT &&
        r->generation==generation && r->epoch==epoch && now>=previous &&
        now-previous>=UINT64_C(5000000);
}
static inline int nnpi_hostready_observed(struct nnpi_hostready_response *r, uint32_t card) {
    if (!r || r->phase!=NNPI_HR_WRITTEN || r->observations>=NNPI_HOSTREADY_LIMIT) return 0;
    ++r->observations; ++r->reads; r->card_last=card;
    if (card==1) r->phase=NNPI_HR_READY;
    else if (card!=0) r->phase=NNPI_HR_STOPPED;
    else if (r->observations==NNPI_HOSTREADY_LIMIT) r->phase=NNPI_HR_EXHAUSTED;
    return 1;
}
static inline int nnpi_hostready_response_valid(const struct nnpi_hostready_response *r, size_t n) {
    if (!r || n!=sizeof(*r) || r->version!=1 || r->size!=sizeof(*r) ||
        !r->generation || !r->epoch || r->generation==UINT64_MAX || r->epoch==UINT64_MAX ||
        r->reserved || r->writes!=1 ||
        r->observations>20 || r->reads!=4+r->observations ||
        !nnpi_hostready_baseline(r->host_before,r->card_before,r->command_before,r->response_before)) return 0;
    switch (r->phase) {
    case NNPI_HR_WRITTEN: return r->observations<20 && r->card_last==0;
    case NNPI_HR_READY: return r->observations && r->card_last==1;
    case NNPI_HR_STOPPED: return r->observations && r->card_last>1;
    case NNPI_HR_EXHAUSTED: return r->observations==20 && !r->card_last;
    default: return 0;
    }
}
static inline int nnpi_hostready_reply_matches(const struct nnpi_hostready_response *r, size_t n,
    const struct nnpi_hostready_request *q, int observe) {
    return nnpi_hostready_request_valid(q,sizeof(*q),sizeof(*r)) &&
        nnpi_hostready_response_valid(r,n) && r->generation==q->generation && r->epoch==q->epoch &&
        (observe ? r->observations!=0 : (r->phase==NNPI_HR_WRITTEN && !r->observations));
}
#endif
