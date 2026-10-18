/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_SYSINFO_POLICY_H
#define NNPI_SYSINFO_POLICY_H
#include "nnpi_sysinfo_abi.h"
#include "nnpi_reset_policy.h"
#include "nnpi_boot_protocol.h"
#include "nnpi_fifo_protocol.h"
#include <string.h>
static inline void nnpi_sysinfo_init(struct nnpi_sysinfo_response *s)
{
    memset(s,0,sizeof(*s)); s->version=NNPI_SYSINFO_VERSION; s->size=(uint32_t)sizeof(*s);
    s->reset_mode=NNPI_SYSINFO_RESET_RETURN_ONLY;
    nnpi_reset_init(&s->reset);
}
/* BIOS packed layout from ipc_protocol.h nnp_c2h_system_info: version@0,
 * CPU@7(12), ICE@19(6), UTF16 BIOS@25(66), CSME@91(24), PMC@115(8).
 * No unaligned native loads, no C-string traversal beyond the fixed field.
 * v1 is the only supported schema. This validates payload, NOT firmware identity
 * and NOT DMA termination. No READY-only free is authorized by this policy. */
static inline uint32_t nnpi_sysinfo_parse(const uint8_t *p,size_t n)
{
    size_t i;
    if (!p || n!=NNPI_SYSINFO_PAYLOAD_BYTES) return NNPI_SYSINFO_PARSE_LENGTH;
    if (p[0]!=1) return NNPI_SYSINFO_PARSE_VERSION;
    for (i=0;i<32;++i)
        if (p[25+2*i]<0x20 || p[25+2*i]>0x7e || p[26+2*i]) return NNPI_SYSINFO_PARSE_STRING;
    if (p[89] || p[90]) return NNPI_SYSINFO_PARSE_STRING;
    if (p[41]!='.' || p[49]!='.' || p[59]!='.' || p[67]!='.') return NNPI_SYSINFO_PARSE_STRING;
    return NNPI_SYSINFO_PARSE_OK;
}
static inline int nnpi_sysinfo_plan(uint32_t card,uint32_t command,uint64_t dma,
                                   struct nnpi_fifo_command_plan *plan)
{
    uint8_t wire[NNPI_BOOT_MESSAGE_SIZE];
    struct nnpi_fifo_snapshot fifo;
    if (card==UINT32_MAX || (card&0xf0ffU)!=1U ||
        nnpi_fifo_decode(command,&fifo)!=NNPI_FIFO_OK || fifo.used_qwords ||
        nnpi_boot_sysinfo(wire,sizeof(wire),dma)!=NNPI_BOOT_OK) return 0;
    return nnpi_fifo_plan_command(command,wire,sizeof(wire),plan)==NNPI_FIFO_OK;
}
static inline int nnpi_sysinfo_publish(struct nnpi_sysinfo_response *s,uint64_t gen,uint64_t epoch)
{
    if (s->budget_consumed || s->phase!=NNPI_SYSINFO_IDLE || !gen || !epoch ||
        gen==UINT64_MAX || epoch==UINT64_MAX || !s->cleanup_reserved || !s->page_retained) return 0;
    s->budget_consumed=1; s->publication_id=1; s->generation=gen; s->epoch=epoch;
    s->phase=NNPI_SYSINFO_OBSERVING; s->ownership=NNPI_SYSINFO_PUBLISHED;
    return 1;
}
typedef int (*nnpi_sysinfo_write_half_fn)(void *,uint32_t,uint32_t);
/* Production and fault injection share exact six-half ordering. Count BEFORE
 * attempting a write; a failing accessor may already have posted its value. */
static inline int nnpi_sysinfo_emit(struct nnpi_sysinfo_response *s,
                                   const struct nnpi_fifo_command_plan *plan,
                                   void *context,nnpi_sysinfo_write_half_fn write_half)
{
    uint32_t i;
    if (!s || !plan || !write_half || s->ownership!=NNPI_SYSINFO_PUBLISHED ||
        s->writes_attempted || plan->count!=3) return 0;
    for (i=0;i<6;++i) {
        ++s->writes_attempted;
        if (!write_half(context,i,(uint32_t)(plan->writes[i/2].value>>((i%2)*32)))) {
            s->phase=NNPI_SYSINFO_CLEANUP; return 0;
        }
    }
    return 1;
}
static inline int nnpi_sysinfo_observe(struct nnpi_sysinfo_response *s,uint32_t card)
{
    if (s->phase!=NNPI_SYSINFO_OBSERVING || s->epoch_invalidated || s->observations>=20) return 0;
    ++s->observations; s->card_last=card;
    if (card!=UINT32_MAX && (card&0xf0ffU)==10U) {
        s->ready_seen=1; s->ownership=NNPI_SYSINFO_PROTOCOL_COMPLETE; return 1;
    }
    return 0;
}
static inline void nnpi_sysinfo_payload(struct nnpi_sysinfo_response *s,const uint8_t *p)
{
    if (!s->ready_seen || s->phase!=NNPI_SYSINFO_OBSERVING || s->epoch_invalidated) return;
    memcpy(s->payload,p,sizeof(s->payload));
    s->parse_result=nnpi_sysinfo_parse(s->payload,sizeof(s->payload));
    s->payload_valid=s->parse_result==NNPI_SYSINFO_PARSE_OK;
}
static inline void nnpi_sysinfo_invalidate(struct nnpi_sysinfo_response *s)
{
    if (s->budget_consumed) s->epoch_invalidated=1;
}
/* 025 explicitly selects NULL reset parameters, not the 024 callback mode.
 * WDK DEVICE_RESET_HANDLER: optional ResetParameters; STATUS_SUCCESS means
 * requested operation succeeds. No guarantee of bounded/synchronous return is
 * assumed. PENDING and even other nonnegative statuses are NOT stop evidence. */
static inline void nnpi_sysinfo_reset_returned(struct nnpi_reset_response *r,uint32_t status,uint64_t now)
{
    r->call_returned=1; r->call_status=status; r->returned_100ns=now;
    r->state=status==0 ? NNPI_RESET_COMPLETE : NNPI_RESET_FAILED;
    /* callback fields remain zero/PENDING: no callback was registered. */
}
/* A reset success is an ownership boundary even when epoch invalidated. It
 * NEVER revalidates MMIO. The owner must supply the associated reset only. */
static inline int nnpi_sysinfo_stopped(struct nnpi_sysinfo_response *s,
                                     uint64_t publication_id,const struct nnpi_reset_response *r)
{
    if (!s->budget_consumed || publication_id!=s->publication_id ||
        r->operation_id!=1 || r->generation!=s->generation || r->epoch!=s->epoch ||
        s->reset_mode!=NNPI_SYSINFO_RESET_RETURN_ONLY || !r->call_returned ||
        r->callback_seen || r->callback_count || r->call_status!=0 ||
        r->state!=NNPI_RESET_COMPLETE) return 0;
    s->reset=*r; s->ownership=NNPI_SYSINFO_STOPPED_BY_RESET; s->phase=NNPI_SYSINFO_FINISHED;
    return 1;
}
static inline int nnpi_sysinfo_may_release(const struct nnpi_sysinfo_response *s)
{
    return s->ownership==NNPI_SYSINFO_UNPUBLISHED || s->ownership==NNPI_SYSINFO_STOPPED_BY_RESET;
}
#endif
