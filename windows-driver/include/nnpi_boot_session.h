/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_BOOT_SESSION_H
#define NNPI_BOOT_SESSION_H
#include "nnpi_boot_protocol.h"
#include "nnpi_fifo_protocol.h"
#include "nnpi_hostready.h"
#include "nnpi_sysinfo_policy.h"

/* Production-shared POLICY ONLY, not a hardware boot implementation or ABI.
 * No allocation, DMA, MMIO, waits, reset invocation or simulated completion.
 * Owner serializes all calls with PnP/D0, supplies fresh reads with strictly
 * increasing read serials, and obtains DMA addresses from this device's DMA API.
 * Initialize once per independent operation object; never reinitialize a live
 * object to replenish budgets. session_id is owner-issued, globally unique;
 * G/E must describe the newly validated hardware incarnation, not cached 025.
 * A token/read serial correlates evidence; it cannot itself prove freshness.
 * reserve_cleanup means the owner has reserved the independent DMA lifetime,
 * provider and teardown barrier BEFORE publication. Cancellation, timeout,
 * ready, epoch loss and reset failure NEVER prove that published DMA stopped.
 */
#define NNPI_BS_NEW 0U
#define NNPI_BS_INITIAL 1U
#define NNPI_BS_WAIT_BIOS 2U
#define NNPI_BS_BIOS_READY 3U
#define NNPI_BS_SYSINFO_TX 4U
#define NNPI_BS_WAIT_SYSINFO 5U
#define NNPI_BS_SYSINFO_READY 6U
#define NNPI_BS_IMAGE_TX 7U
#define NNPI_BS_WAIT_DRIVER 8U
#define NNPI_BS_DRIVER_READY 9U
#define NNPI_BS_CLEANUP 10U
#define NNPI_BS_FINISHED 11U
#define NNPI_BS_SYSINFO_DMA 1U
#define NNPI_BS_IMAGE_DMA 2U
#define NNPI_BS_TABLE_DMA 4U
#define NNPI_BS_ALL_DMA 7U
/* Shared boot observation contract (not an ABI or a wall-clock guarantee).
 * The supervisor covers all four waits plus its independent reporting margin. */
#define NNPI_BS_POLL_MS 500U
#define NNPI_BS_READY_LIMIT 600U
#define NNPI_BS_BIOS_LIMIT 20U
#define NNPI_BS_SYSINFO_LIMIT 20U
#define NNPI_BS_DRIVER_LIMIT 1200U
#define NNPI_BS_SUPERVISOR_MARGIN_MS 60000U
#define NNPI_BS_RESET_RETURN_ONLY 1U

/* Accept the waiting phase or its ready target so the owner and policy use
 * exactly the same bound. Unknown/non-wait phases have no observation budget. */
static inline uint32_t nnpi_boot_session_wait_limit(uint32_t phase)
{
    switch (phase) {
    case NNPI_BS_WAIT_BIOS: case NNPI_BS_BIOS_READY: return NNPI_BS_BIOS_LIMIT;
    case NNPI_BS_WAIT_SYSINFO: case NNPI_BS_SYSINFO_READY: return NNPI_BS_SYSINFO_LIMIT;
    case NNPI_BS_WAIT_DRIVER: case NNPI_BS_DRIVER_READY: return NNPI_BS_DRIVER_LIMIT;
    default: return 0;
    }
}

struct nnpi_boot_session_key { uint64_t session_id, generation, epoch; };
struct nnpi_boot_session {
    struct nnpi_boot_session_key key;
    uint64_t last_read;
    uint32_t phase, hostready_used, sysinfo_used, image_used;
    uint32_t cleanup_reserved, published_dma, stopped_dma, invalidated;
    uint32_t observations, boot_started, driver_ready, payload_valid;
    uint32_t reset_used, reset_returned;
    uint32_t timed_out; /* Internal terminal reason; never a DMA-stop proof. */
};

static inline void nnpi_boot_session_init(struct nnpi_boot_session *s)
{
    if (s) memset(s,0,sizeof(*s));
}
static inline int nnpi_boot_session_key_valid(const struct nnpi_boot_session_key *k)
{
    return k && k->session_id && k->session_id!=UINT64_MAX &&
        k->generation && k->generation!=UINT64_MAX && k->epoch && k->epoch!=UINT64_MAX;
}
static inline int nnpi_boot_session_matches(const struct nnpi_boot_session *s,
                                           const struct nnpi_boot_session_key *k)
{
    return s && nnpi_boot_session_key_valid(k) &&
        s->key.session_id==k->session_id && s->key.generation==k->generation &&
        s->key.epoch==k->epoch;
}
static inline int nnpi_boot_session_begin(struct nnpi_boot_session *s,
    const struct nnpi_boot_session_key *k,uint64_t read_serial,
    uint32_t host,uint32_t card,uint32_t command,uint32_t response)
{
    if (!s || s->phase!=NNPI_BS_NEW || !nnpi_boot_session_key_valid(k) ||
        !read_serial || read_serial==UINT64_MAX ||
        !nnpi_hostready_baseline(host,card,command,response)) return 0;
    s->key=*k; s->last_read=read_serial; s->phase=NNPI_BS_INITIAL;
    return 1;
}
/* Abort is an ownership-independent terminal latch. Also used on timeout,
 * partial write, cancellation, power/removal notification and unknown state.
 * It does not reset budgets or release any published mapping. */
static inline void nnpi_boot_session_abort(struct nnpi_boot_session *s)
{
    if (!s || s->phase==NNPI_BS_NEW || s->phase==NNPI_BS_FINISHED) return;
    s->invalidated=1; s->phase=NNPI_BS_CLEANUP;
}
static inline int nnpi_boot_session_reserve_cleanup(struct nnpi_boot_session *s,
    const struct nnpi_boot_session_key *k)
{
    if (!nnpi_boot_session_matches(s,k) || s->phase!=NNPI_BS_INITIAL ||
        s->cleanup_reserved) return 0;
    s->cleanup_reserved=1; return 1;
}
/* Claim BEFORE the single fixed host-ready write; never borrow/reset 025A's
 * ledger. Caller writes only the fixed HOST_READY action, no arbitrary value.
 * Even initial card=1 needs a NEW BIOS_READY read after this claim. */
static inline int nnpi_boot_session_hostready(struct nnpi_boot_session *s,
    const struct nnpi_boot_session_key *k)
{
    if (!nnpi_boot_session_matches(s,k) || s->phase!=NNPI_BS_INITIAL ||
        !s->cleanup_reserved || s->hostready_used || s->invalidated) return 0;
    s->hostready_used=1; s->phase=NNPI_BS_WAIT_BIOS; return 1;
}
/* Doorbell boot field is bits 7:0; errors are bits 15:12. Other documented
 * fields are not replayed or interpreted. Initial baseline is stricter (0/1).
 * Known waiting states may be observed, never reissued as commands. */
static inline int nnpi_boot_session_observe(struct nnpi_boot_session *s,
    const struct nnpi_boot_session_key *k,uint64_t read_serial,uint32_t card,
    const uint8_t *payload,size_t payload_bytes)
{
    uint32_t state;
    if (!nnpi_boot_session_matches(s,k) || s->invalidated ||
        !read_serial || read_serial==UINT64_MAX || read_serial<=s->last_read ||
        (s->phase!=NNPI_BS_WAIT_BIOS && s->phase!=NNPI_BS_WAIT_SYSINFO &&
         s->phase!=NNPI_BS_WAIT_DRIVER)) return 0;
    s->last_read=read_serial; ++s->observations;
    if (card==UINT32_MAX || (card&0xf000U)) {
        nnpi_boot_session_abort(s); return 0;
    }
    state=card&0xffU;
    if (s->phase==NNPI_BS_WAIT_BIOS) {
        if (state==1) { s->phase=NNPI_BS_BIOS_READY; s->observations=0; return 1; }
        if (state!=0) { nnpi_boot_session_abort(s); return 0; }
    } else if (s->phase==NNPI_BS_WAIT_SYSINFO) {
        if (state==10) {
            if (nnpi_sysinfo_parse(payload,payload_bytes)!=NNPI_SYSINFO_PARSE_OK) {
                nnpi_boot_session_abort(s); return 0;
            }
            s->payload_valid=1; s->phase=NNPI_BS_SYSINFO_READY;
            s->observations=0; return 1;
        }
        if (state!=1) { nnpi_boot_session_abort(s); return 0; }
    } else {
        if (state==4 || state==8) {
            s->driver_ready=1; s->phase=NNPI_BS_DRIVER_READY;
            return 1; /* Deliberately no ownership transition. */
        }
        if (state==3) s->boot_started=1;
        else if (state!=10 || s->boot_started) { nnpi_boot_session_abort(s); return 0; }
    }
    /* Ready and real protocol errors above take precedence at the boundary. */
    if (s->observations>=nnpi_boot_session_wait_limit(s->phase)) {
        s->timed_out=1; nnpi_boot_session_abort(s);
    }
    return 0;
}
/* These functions publish ownership and consume the one-shot budget BEFORE
 * returning the fixed three-QWORD plan. A plan is NOT proof of a send. Owner
 * must execute it once under its hardware lock, low then high, final notify,
 * then call sent() with the actual result. Partial/uncertain sends must abort.
 * Invalid plan inputs terminate this operation, with output left unchanged.
 * No arbitrary register/port/address callback is exposed by this policy. */
static inline int nnpi_boot_session_sysinfo(struct nnpi_boot_session *s,
    const struct nnpi_boot_session_key *k,uint64_t read_serial,
    uint32_t card,uint32_t command,uint64_t dma_page,
    struct nnpi_fifo_command_plan *out)
{
    struct nnpi_fifo_command_plan plan;
    if (!nnpi_boot_session_matches(s,k) || s->phase!=NNPI_BS_BIOS_READY ||
        s->sysinfo_used || s->invalidated || !s->cleanup_reserved) return 0;
    if (!out || read_serial<=s->last_read || read_serial==UINT64_MAX ||
        !nnpi_sysinfo_plan(card,command,dma_page,&plan)) {
        nnpi_boot_session_abort(s); return 0;
    }
    s->last_read=read_serial; s->sysinfo_used=1;
    s->published_dma|=NNPI_BS_SYSINFO_DMA; s->phase=NNPI_BS_SYSINFO_TX;
    *out=plan; return 1;
}
/* Owner has already built/validated a single DMA-contiguous descriptor array
 * with nnpi_boot_block(page_block=0), matching image length/order and complete
 * rounded mappings. This policy checks wire representability, not residency. */
static inline int nnpi_boot_session_image(struct nnpi_boot_session *s,
    const struct nnpi_boot_session_key *k,uint64_t read_serial,
    uint32_t card,uint32_t command,uint64_t dma_table,uint64_t mapped_bytes,
    uint64_t entries,uint64_t image_bytes,struct nnpi_fifo_command_plan *out)
{
    uint8_t wire[NNPI_BOOT_MESSAGE_SIZE];
    struct nnpi_fifo_snapshot fifo;
    struct nnpi_fifo_command_plan plan;
    if (!nnpi_boot_session_matches(s,k) || s->phase!=NNPI_BS_SYSINFO_READY ||
        s->image_used || s->invalidated || !s->payload_valid || !s->cleanup_reserved) return 0;
    if (!out || read_serial<=s->last_read || read_serial==UINT64_MAX ||
        card==UINT32_MAX || (card&0xf0ffU)!=10U ||
        nnpi_fifo_decode(command,&fifo)!=NNPI_FIFO_OK || fifo.used_qwords ||
        nnpi_boot_image(wire,sizeof(wire),dma_table,mapped_bytes,entries,image_bytes)!=NNPI_BOOT_OK ||
        nnpi_fifo_plan_command(command,wire,sizeof(wire),&plan)!=NNPI_FIFO_OK) {
        nnpi_boot_session_abort(s); return 0;
    }
    s->last_read=read_serial; s->image_used=1;
    s->published_dma|=NNPI_BS_IMAGE_DMA|NNPI_BS_TABLE_DMA; s->phase=NNPI_BS_IMAGE_TX;
    *out=plan; return 1;
}
static inline int nnpi_boot_session_sent(struct nnpi_boot_session *s,
    const struct nnpi_boot_session_key *k,uint32_t halves_completed)
{
    if (!nnpi_boot_session_matches(s,k) || s->invalidated ||
        (s->phase!=NNPI_BS_SYSINFO_TX && s->phase!=NNPI_BS_IMAGE_TX)) return 0;
    if (halves_completed!=6) { nnpi_boot_session_abort(s); return 0; }
    s->phase=s->phase==NNPI_BS_SYSINFO_TX ? NNPI_BS_WAIT_SYSINFO : NNPI_BS_WAIT_DRIVER;
    s->observations=0; return 1;
}
/* Single associated FunctionLevelDeviceReset(flags=0, parameters=NULL).
 * Claim before calling provider. Owner holds teardown/code/provider lifetime
 * through actual return, with no callback registered. Reset invalidates MMIO
 * even on success; no boot retry or new hardware access is authorized here. */
static inline int nnpi_boot_session_reset_begin(struct nnpi_boot_session *s,
    const struct nnpi_boot_session_key *k)
{
    if (!nnpi_boot_session_matches(s,k) || !s->cleanup_reserved ||
        !s->hostready_used || s->reset_used || s->phase==NNPI_BS_FINISHED) return 0;
    nnpi_boot_session_abort(s); s->reset_used=1; return 1;
}
/* k identifies the RESET OPERATION'S captured owner, not the current caller.
 * r must be the response from the reset claimed above, never a cached 024/025
 * response. The platform adapter must carry this key through its reset worker;
 * a matching G/E alone cannot distinguish sessions or establish DMA stop. */
static inline int nnpi_boot_session_stopped(struct nnpi_boot_session *s,
    const struct nnpi_boot_session_key *k,uint32_t mode,uint32_t flags,
    const struct nnpi_reset_response *r)
{
    if (!nnpi_boot_session_matches(s,k) || !s->reset_used || s->reset_returned ||
        s->phase!=NNPI_BS_CLEANUP || mode!=NNPI_BS_RESET_RETURN_ONLY || flags ||
        !nnpi_reset_response_valid(r,sizeof(*r)) || r->operation_id!=1 ||
        r->generation!=s->key.generation || r->epoch!=s->key.epoch ||
        r->call_returned!=1) return 0;
    s->reset_returned=1; /* Any actual associated return exhausts reset. */
    if (r->callback_seen || r->callback_count || r->completed_100ns ||
        r->completion_status!=NNPI_RESET_PENDING || r->call_status!=0 ||
        r->state!=NNPI_RESET_COMPLETE) return 0;
    s->stopped_dma=s->published_dma; s->phase=NNPI_BS_FINISHED; return 1;
}
/* Each resource is evaluated independently, even in failure/cancel paths.
 * Unpublished resources may be freed; published ones require associated stop.
 * DriverReady/CardReady is intentionally NOT sufficient for this 026 policy. */
static inline int nnpi_boot_session_may_release(const struct nnpi_boot_session *s,
                                               uint32_t resources)
{
    return s && resources && !(resources&~NNPI_BS_ALL_DMA) &&
        !(s->published_dma & resources & ~s->stopped_dma);
}
#endif
