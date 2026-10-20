/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_BULK_H
#define NNPI_BULK_H
#include "nnpi_chan_protocol.h"
#include "nnpi_runtime_protocol.h"
#include "nnpi_boot_protocol.h"
#include "nnpi_c2h_pattern.h"
/* Card contract: bulk_contract.h, version 4.
 * One immutable run per channel; sixteen lifetime channels, never reused.
 * HOSTRES events do NOT carry session/run/generation. Therefore this channel
 * is single-use for the device lifetime, and each resource is mapped once.
 * Caller drains received frames before publishing the next operation. */
/* Version byte bits23:16 of the opcode-59 bulk frames. The card's
 * bulk_handle() compares it against its own BULK_VERSION and answers
 * BULK_BAD on any mismatch, so this must stay 4 for wire v4. */
#define NNPI_BULK_WIRE_VERSION 4u
#define NNPI_BULK_CHANNEL 257u /* first dedicated ID; Net remains 256 */
#define NNPI_BULK_CHANNEL_COUNT 16u
#define NNPI_BULK_CHANNEL_LAST 272u
#define NNPI_BULK_CONTROL_ID 1u
#define NNPI_BULK_DATA_ID 2u
/* Each single-use channel registers once. This is NOT the BOOT generation. */
#define NNPI_BULK_BINDING_GENERATION 1u
#define NNPI_BULK_BYTES (16u*1024u*1024u)
#define NNPI_BULK_BUFFER_BYTES (4u*1024u*1024u)
#define NNPI_BULK_TIMEOUT_TICKS UINT64_C(50000000) /* per phase 5s, 100ns */
#define NNPI_BULK_EXEC_TICKS UINT64_C(300000000) /* fixed 30s observation budget, not throughput prediction */
#define NNPI_BULK_HARD_TICKS UINT64_C(900000000) /* entire run incl cleanup 90s */
/* CAPS reply: low32 capability bits, high32 ring bytes. Bits 0..9 are the
 * H2C contract every run needs; bit 10 is C2H, bit 11 FAST sentinel mode,
 * bit 12 non-64KiB C2H chunks. Unknown bits stay rejected. This is a
 * negotiation: an H2C-only card legitimately returns 0x3ff. */
#define NNPI_BULK_CAP_BASE 0x3ffu
#define NNPI_BULK_CAP_C2H 0x400u
#define NNPI_BULK_CAP_FAST 0x800u
#define NNPI_BULK_CAP_C2H_VAR 0x1000u
#define NNPI_BULK_CAP_KNOWN 0x1fffu
#define NNPI_BULK_CAP_RING_BYTES 4096u
static inline int nnpi_bulk_caps_valid(uint64_t caps,uint32_t config)
{
 uint32_t low=(uint32_t)caps;
 return (caps>>32)==(uint64_t)NNPI_BULK_CAP_RING_BYTES &&
  !(low&~NNPI_BULK_CAP_KNOWN) && (low&NNPI_BULK_CAP_BASE)==NNPI_BULK_CAP_BASE &&
  (!(config&8u) || (low&NNPI_BULK_CAP_C2H)) &&
  (!(config&16u) || (low&NNPI_BULK_CAP_FAST)) &&
  (!(config&8u) || !(config&3u) || (low&NNPI_BULK_CAP_C2H_VAR));
}
enum nnpi_bulk_phase {
 NNPI_BULK_IDLE=0, NNPI_BULK_CREATE, NNPI_BULK_CAPS,
 NNPI_BULK_MAP_A, NNPI_BULK_MAP_B, NNPI_BULK_REGISTER,
 NNPI_BULK_EXEC, NNPI_BULK_SETUP, NNPI_BULK_WINDOW, NNPI_BULK_VERIFY, NNPI_BULK_STOP, NNPI_BULK_UNMAP_B,
 NNPI_BULK_UNMAP_A, NNPI_BULK_DESTROY, NNPI_BULK_HOST_VERIFY, NNPI_BULK_DONE,
 NNPI_BULK_FAULT
};
struct nnpi_bulk_state {
 uint32_t channel, channels_used, dma_released, retired_mask, late_events;
 uint64_t retired_run[NNPI_BULK_CHANNEL_COUNT];
 uint64_t session, generation, epoch, run, started, finished, deadline;
 uint32_t phase, waiting, published, mapped, completed_bytes, card_us;
 uint32_t fault, exec_status, exec_sent, completion_seen, stop_seen;
 uint32_t channel_issued, channel_destroyed;
 uint32_t fault_from_phase, destroy_sent;
 uint64_t hard_deadline, phase_started, exec_submitted, cleanup_started, cleanup_finished;
 uint64_t observed_time;
 uint64_t card_setup_ns,card_window_ns,card_verify_ns;
 uint32_t config,stats_seen;
 /* CAPS is negotiated, not assumed: the card may be H2C-only. The result
  * survives runs so a known H2C-only card never consumes a channel on a C2H
  * request. */
 uint32_t card_caps,caps_seen;
 uint32_t verified_bytes,verify_status,first_bad_offset,expected_byte,actual_byte;
 uint64_t host_verify_ticks;
 /* dma_released means driver ownership withdrawn/delete requested, not a
  * claim about final WDF physical-memory reclamation. generation above is
  * the BOOT identity; C2H pattern uses the separate wire binding generation. */
 uint64_t phase_elapsed[NNPI_BULK_DONE];
};
/* Scheduling only: all identity/health gates must additionally hold at caller. */
static inline int nnpi_bulk_active(const struct nnpi_bulk_state *s)
{
 return s && s->session && s->generation && s->epoch && s->run && !s->fault &&
  s->phase>=NNPI_BULK_CREATE && s->phase<=NNPI_BULK_HOST_VERIFY;
}
/* Zero-time event check only after real local verification progress. Caller
 * supplies the full bulk identity/health gate and parser state under CacheLock.
 * No-progress/transport rounds keep their existing positive wait. */
static inline int nnpi_bulk_verify_continue(const struct nnpi_bulk_state *s,
 int current,unsigned partial_words,uint32_t verified_before)
{
 return current && !partial_words && nnpi_bulk_active(s) &&
  s->phase==NNPI_BULK_HOST_VERIFY && (s->config&8u) &&
  !s->waiting && !s->published && !s->mapped && s->stop_seen &&
  s->channel_destroyed && !s->dma_released && s->verify_status!=2 &&
  s->verified_bytes>verified_before && s->verified_bytes<NNPI_BULK_BYTES;
}
#include "nnpi_image_identity.h"
/* Only exact reviewed offline image; caller must also attest actual BootSeal.
 * No user-provided digest or CAPS response is an image identity authority. */
static inline int nnpi_bulk_image_approved(const uint8_t digest[32], uint64_t bytes)
{ return nnpi_image_identity_matches(digest,bytes); } /* exact reviewed dev9 image only */
static inline int nnpi_bulk_tables(const struct nnpi_runtime_message *v)
{
 return nnpi_runtime_compatible_1150(v) &&
  v->channel_version==NNPI_CHAN_PROTOCOL_VERSION &&
  ((v->command_sizes_raw>>(2u*(59u-32u)))&3u)==3u &&
  ((v->response_sizes_raw>>(2u*(41u-32u)))&3u)==3u;
}
static inline int nnpi_bulk_admission(int sealed,const uint8_t digest[32],uint64_t bytes,
 const struct nnpi_runtime_message *v)
{ return sealed==1 && nnpi_bulk_image_approved(digest,bytes) && nnpi_bulk_tables(v); }
/* DONE alone is not rearm permission: actual WDF objects must be gone. */
static inline int nnpi_bulk_can_begin(const struct nnpi_bulk_state *s,uint64_t run)
{
 if(!s || !run || s->channels_used>=NNPI_BULK_CHANNEL_COUNT || run<=s->run) return 0;
 if(!s->phase) return !s->channels_used;
 return s->phase==NNPI_BULK_DONE && !s->fault && s->dma_released &&
  s->exec_sent && s->completion_seen && !s->exec_status && !s->verify_status &&
  (!(s->config&8u) || s->verified_bytes==NNPI_BULK_BYTES) &&
  s->completed_bytes==NNPI_BULK_BYTES && s->stats_seen==7 && s->card_window_ns &&
  s->stop_seen && !s->mapped && !s->published && s->destroy_sent && s->channel_destroyed;
}
static inline int nnpi_bulk_begin(struct nnpi_bulk_state *s, uint64_t session,
 uint64_t generation,uint64_t epoch,uint64_t run,uint64_t now,
 int approved,const struct nnpi_runtime_message *v)
{
 struct nnpi_bulk_state next={0};
 if (!nnpi_bulk_can_begin(s,run) || !session || !generation || !epoch || !run || !approved ||
     !nnpi_bulk_tables(v) || now>UINT64_MAX-NNPI_BULK_HARD_TICKS) return 0;
 /* Commit allocation only here, after all admission and DMA allocation succeeded.
  * Failed allocation/admission consumes no ID and publishes nothing. */
 next.channels_used=s->channels_used;next.retired_mask=s->retired_mask;
 next.late_events=s->late_events;memcpy(next.retired_run,s->retired_run,sizeof(next.retired_run));
 next.card_caps=s->card_caps;next.caps_seen=s->caps_seen;
 if(s->phase==NNPI_BULK_DONE) {
  next.retired_mask|=1u<<(s->channel-NNPI_BULK_CHANNEL);
  next.retired_run[s->channel-NNPI_BULK_CHANNEL]=s->run;
 }
 next.channel=NNPI_BULK_CHANNEL+next.channels_used++;
 *s=next;s->session=session; s->generation=generation;
 s->epoch=epoch; s->run=run; s->started=now;
 s->deadline=now+NNPI_BULK_TIMEOUT_TICKS; s->phase=NNPI_BULK_CREATE;
 s->hard_deadline=now+NNPI_BULK_HARD_TICKS; s->phase_started=now; s->observed_time=now;
 return 1;
}
static inline void nnpi_bulk_fault(struct nnpi_bulk_state *s,uint32_t reason)
{
 if (s->phase && s->phase!=NNPI_BULK_DONE) {
  if (!s->fault) { s->fault=reason; s->fault_from_phase=s->phase; }
  s->phase=NNPI_BULK_FAULT; s->waiting=0;
 }
}
static inline void nnpi_bulk_tick(struct nnpi_bulk_state *s,uint64_t now)
{
 /* After acknowledged teardown, host timing failure is not unknown DMA.
  * Stop verification, preserve scanned byte count, then safely release. */
 if(s->phase==NNPI_BULK_HOST_VERIFY && !s->published && !s->mapped &&
    s->stop_seen && s->channel_destroyed &&
    (now<s->observed_time || now>=s->deadline || now>=s->hard_deadline)) {
  s->verify_status=2;s->observed_time=now;return;
 }
 if (nnpi_bulk_active(s)) {
  if (now<s->observed_time) { nnpi_bulk_fault(s,2); return; }
  s->observed_time=now;
 }
 if (s->phase && s->phase<NNPI_BULK_DONE &&
     (now>=s->deadline || now>=s->hard_deadline))
  nnpi_bulk_fault(s,1);
}
/* Opcode24, TWO QWs. QW0: op6/channel10/resource16/unmap1/reserved31.
 * QW1: chain PFN45/reserved19. NOT opcode23 SET_RB. */
static inline int nnpi_bulk_hostres(uint16_t channel,uint16_t id,int unmap,
 uint64_t pfn,uint64_t q[3])
{
 if (!q || channel>1023 || !id || (unmap!=0 && unmap!=1) ||
     (unmap ? pfn!=0 : (!pfn || pfn>NNPI_CHAN_PFN_MASK))) return 0;
 q[0]=24u|((uint64_t)channel<<6)|((uint64_t)id<<16)|((uint64_t)unmap<<32);
 q[1]=pfn; q[2]=0; return 1;
}
static inline uint64_t nnpi_bulk_binding(uint32_t id)
{ return id|((uint64_t)NNPI_BULK_BINDING_GENERATION<<16); } /* offset zero */
static inline unsigned nnpi_bulk_next(const struct nnpi_bulk_state *s,
 uint64_t a_pfn,uint64_t b_pfn,uint64_t q[3])
{
 unsigned verb=0; uint64_t binding=0;
 if (!s || !q || s->waiting || !s->phase || s->phase>=NNPI_BULK_DONE) return 0;
 memset(q,0,3*sizeof(*q));
 switch (s->phase) {
 case NNPI_BULK_CREATE: case NNPI_BULK_DESTROY:
  return nnpi_chan_op_encode((uint16_t)s->channel,s->phase==NNPI_BULK_DESTROY,0,0,q)==NNPI_CHAN_OK ? 1u:0u;
 case NNPI_BULK_MAP_A: case NNPI_BULK_UNMAP_A:
  return nnpi_bulk_hostres((uint16_t)s->channel,NNPI_BULK_CONTROL_ID,
   s->phase==NNPI_BULK_UNMAP_A,s->phase==NNPI_BULK_MAP_A?a_pfn:0,q)?2u:0u;
 case NNPI_BULK_MAP_B: case NNPI_BULK_UNMAP_B:
  return nnpi_bulk_hostres((uint16_t)s->channel,NNPI_BULK_DATA_ID,
   s->phase==NNPI_BULK_UNMAP_B,s->phase==NNPI_BULK_MAP_B?b_pfn:0,q)?2u:0u;
 case NNPI_BULK_CAPS: verb=1; break;
 case NNPI_BULK_REGISTER: verb=2; binding=nnpi_bulk_binding(NNPI_BULK_CONTROL_ID); break;
 case NNPI_BULK_EXEC:
  if (s->exec_sent) return 0;
  verb=5; binding=nnpi_bulk_binding(NNPI_BULK_DATA_ID)|((uint64_t)s->config<<32); break;
 case NNPI_BULK_SETUP: case NNPI_BULK_WINDOW: case NNPI_BULK_VERIFY:
  verb=6u+s->phase-NNPI_BULK_SETUP; binding=nnpi_bulk_binding(NNPI_BULK_DATA_ID)|((uint64_t)s->config<<32); break;
 case NNPI_BULK_STOP: verb=4; binding=nnpi_bulk_binding(NNPI_BULK_CONTROL_ID); break;
 default: return 0;
 }
 q[0]=59u|((uint64_t)s->channel<<6)|((uint64_t)NNPI_BULK_WIRE_VERSION<<16)|
  ((uint64_t)verb<<24);
 q[1]=s->run; q[2]=binding; return 3;
}
/* Commit BEFORE MMIO. Even partial/ambiguous emission retains ownership. */
static inline void nnpi_bulk_publish(struct nnpi_bulk_state *s)
{
 if (!nnpi_bulk_active(s) || s->waiting) return;
 s->waiting=1;
 if (s->phase==NNPI_BULK_DESTROY) s->destroy_sent=1;
 if (s->phase==NNPI_BULK_CREATE) s->channel_issued=1;
 if (s->phase==NNPI_BULK_MAP_A) s->published|=1;
 if (s->phase==NNPI_BULK_MAP_B) s->published|=2;
 if (s->phase==NNPI_BULK_EXEC) s->exec_sent=1;
}
static inline void nnpi_bulk_publish_at(struct nnpi_bulk_state *s,uint64_t now)
{
 nnpi_bulk_tick(s,now);
 if (!nnpi_bulk_active(s) || s->waiting) return;
 if (s->phase==NNPI_BULK_EXEC) s->exec_submitted=now;
 nnpi_bulk_publish(s);
}
/* Only a known, successfully retired channel with a well-formed successful
 * old reply is ignorable. Unknown IDs, malformed replies and global fatal
 * messages fall through to existing fail-closed runtime routing. */
static inline int nnpi_bulk_retired_receive(struct nnpi_bulk_state *s,
 const struct nnpi_runtime_message *m)
{
 struct nnpi_chan_event e;uint32_t channel,index,verb;uint64_t binding;
 if(m->opcode==4 && m->qword_count==1) {
  nnpi_chan_event_decode(m->qwords[0],&e);
  if(e.opcode!=4 || nnpi_chan_event_card_fatal(e.event_code) || e.ctx_valid ||
     !e.obj_valid || e.event_val) return 0;
  channel=e.obj_id;
  if(e.event_code==21 || e.event_code==22) {if(e.obj_valid_2) return 0;}
  else if(e.event_code==24 || e.event_code==25) {
   if(!e.obj_valid_2 || (e.obj_id_2!=1 && e.obj_id_2!=2)) return 0;
  } else return 0;
 } else if(m->opcode==41 && m->qword_count==3) {
  if((m->qwords[0]&63)!=41 || (m->qwords[0]>>32) || ((m->qwords[0]>>16)&255)!=4) return 0;
  channel=(uint32_t)((m->qwords[0]>>6)&1023);
 } else return 0;
 if(channel<NNPI_BULK_CHANNEL || channel>NNPI_BULK_CHANNEL_LAST) return 0;
 index=channel-NNPI_BULK_CHANNEL;
 if(!(s->retired_mask&(1u<<index))) return 0;
 if(m->opcode==41) {
  if(m->qwords[1]!=s->retired_run[index]) return 0;
  verb=(uint32_t)((m->qwords[0]>>24)&255);binding=m->qwords[2];
  if(verb==1) {if(!nnpi_bulk_caps_valid(binding,0)) return 0;}
  else if(verb==2 || verb==4) {if(binding!=nnpi_bulk_binding(NNPI_BULK_CONTROL_ID)) return 0;}
  else if(verb==5) {if((uint32_t)binding!=NNPI_BULK_BYTES) return 0;}
  else if(verb<6 || verb>8) return 0;
 }
 if(s->late_events!=UINT32_MAX) ++s->late_events;
 return 1;
}
/* Returns 0 for other channels, 1 for this channel (including retained fault).
 * Unexpected/early/duplicate local events fail closed, never advance state.
 * Distinct map and unmap event codes, plus unique IDs, remove tuple ambiguity. */
static inline int nnpi_bulk_receive(struct nnpi_bulk_state *s,
 const struct nnpi_runtime_message *m,uint64_t now)
{
 struct nnpi_chan_event e;
 unsigned expected=0,failed=0,id=0,verb=0,status;
 uint64_t budget;
 uint64_t binding=0;
 /* now is the monotonic processing time after a COMPLETE frame is read.
  * Conservative boundary: processing at/after deadline is late even if some
  * QWs arrived earlier. No backdated timestamp, grace or fault resurrection. */
 nnpi_bulk_tick(s,now);
 if(nnpi_bulk_retired_receive(s,m)) return 1;
 if (m->opcode==4 && m->qword_count==1) {
  nnpi_chan_event_decode(m->qwords[0],&e);
  if (nnpi_chan_event_card_fatal(e.event_code)) {
   if(s->phase==NNPI_BULK_DONE && !s->fault) s->fault=3;
   nnpi_bulk_fault(s,3); return 0; /* Preserve original global fatal handling. */
  }
  if (!e.obj_valid || e.obj_id!=s->channel) return 0;
  if (!s->phase || s->phase==NNPI_BULK_DONE) return 0;
  if(s->phase==NNPI_BULK_FAULT) return 1;
  if (!s->waiting || e.opcode!=4 || e.ctx_valid) goto bad;
  switch(s->phase) {
  case NNPI_BULK_CREATE: expected=21; failed=70; break;
  case NNPI_BULK_DESTROY: expected=22; failed=71; break;
  case NNPI_BULK_MAP_A: expected=24; failed=75; id=NNPI_BULK_CONTROL_ID; break;
  case NNPI_BULK_MAP_B: expected=24; failed=75; id=NNPI_BULK_DATA_ID; break;
  case NNPI_BULK_UNMAP_B: expected=25; failed=76; id=NNPI_BULK_DATA_ID; break;
  case NNPI_BULK_UNMAP_A: expected=25; failed=76; id=NNPI_BULK_CONTROL_ID; break;
  default: goto bad;
  }
  if ((id && (!e.obj_valid_2 || e.obj_id_2!=id)) ||
      (!id && e.obj_valid_2) || (e.event_code!=expected && e.event_code!=failed)) goto bad;
  if (e.event_code==failed || e.event_val) goto bad;
  if (s->phase==NNPI_BULK_MAP_A) s->mapped|=1;
  if (s->phase==NNPI_BULK_MAP_B) s->mapped|=2;
  if (s->phase==NNPI_BULK_UNMAP_A) { s->mapped&=~1u; s->published&=~1u; }
  if (s->phase==NNPI_BULK_UNMAP_B) { s->mapped&=~2u; s->published&=~2u; }
  if (s->phase==NNPI_BULK_DESTROY) s->channel_destroyed=1;
 } else if (m->opcode==41 && m->qword_count==3) {
  if (((m->qwords[0]>>6)&1023u)!=s->channel) return 0;
  if (!s->phase || s->phase==NNPI_BULK_DONE) return 0;
  if(s->phase==NNPI_BULK_FAULT) return 1;
  if (!s->waiting || (m->qwords[0]&63u)!=41 || (m->qwords[0]>>40) ||
      ((m->qwords[0]>>16)&255u)!=4 || m->qwords[1]!=s->run) goto bad;
  switch(s->phase) {
  case NNPI_BULK_CAPS: verb=1; break;
  case NNPI_BULK_REGISTER: verb=2; binding=nnpi_bulk_binding(NNPI_BULK_CONTROL_ID); break;
  case NNPI_BULK_EXEC: verb=5; break;
  case NNPI_BULK_SETUP: case NNPI_BULK_WINDOW: case NNPI_BULK_VERIFY: verb=6u+s->phase-NNPI_BULK_SETUP; break;
  case NNPI_BULK_STOP: verb=4; binding=nnpi_bulk_binding(NNPI_BULK_CONTROL_ID); break;
  default: goto bad;
  }
  status=(unsigned)((m->qwords[0]>>32)&255u);
  if (((m->qwords[0]>>24)&255u)!=verb) goto bad;
  if (verb==5) {
   if (!s->exec_sent || s->completion_seen || status>7 ||
       (uint32_t)m->qwords[2]>NNPI_BULK_BYTES ||
       (!status && (uint32_t)m->qwords[2]!=NNPI_BULK_BYTES)) goto bad;
   s->completed_bytes=(uint32_t)m->qwords[2]; s->card_us=(uint32_t)(m->qwords[2]>>32);
   s->exec_status=status; s->completion_seen=1; s->finished=now;
   /* DMA_UNKNOWN does not prove retirement. No automatic STOP/unmap recovery. */
   if (status) goto bad;
  } else if (verb>=6 && verb<=8) {
   if(status || !s->completion_seen || s->exec_status) goto bad;
   if(verb==6)s->card_setup_ns=m->qwords[2];
   if(verb==7)s->card_window_ns=m->qwords[2];
   if(verb==8)s->card_verify_ns=m->qwords[2];
   s->stats_seen|=1u<<(verb-6);
  } else if (verb==1) {
   /* Negotiation: require the H2C base, take C2H only when advertised. */
   if (status || !nnpi_bulk_caps_valid(m->qwords[2],0)) goto bad;
   if ((s->config&8u) && !((uint32_t)m->qwords[2]&NNPI_BULK_CAP_C2H)) {
    /* Direction capability, not protocol corruption. Admission rejects this
     * before a channel is consumed once caps are known; this is the guard for
     * the first run after a driver reload. */
    nnpi_bulk_fault(s,3); return 1;
   }
   if (((s->config&16u) && !((uint32_t)m->qwords[2]&NNPI_BULK_CAP_FAST)) ||
       ((s->config&8u) && (s->config&3u) && !((uint32_t)m->qwords[2]&NNPI_BULK_CAP_C2H_VAR))) {
    /* FAST and variable C2H chunks are negotiated, never assumed. */
    nnpi_bulk_fault(s,3); return 1;
   }
   s->card_caps=(uint32_t)m->qwords[2]; s->caps_seen=1;
  } else if (status || m->qwords[2]!=binding) goto bad;
  if (verb==4) s->stop_seen=1;
 } else return 0;
 /* Only a fully matched actual transition earns a new phase budget. The
  * immutable hard limit includes cleanup; duplicate/error traffic earns none. */
 s->phase_elapsed[s->phase]=now-s->phase_started;
 ++s->phase; s->waiting=0; s->phase_started=now;
 budget=s->phase==NNPI_BULK_EXEC?NNPI_BULK_EXEC_TICKS:NNPI_BULK_TIMEOUT_TICKS;
 s->deadline=s->hard_deadline-now>budget ? now+budget : s->hard_deadline;
 if (s->phase==NNPI_BULK_STOP) {
  if(s->stats_seen!=7 || !s->card_window_ns) goto bad;
  s->cleanup_started=now;
 }
 if (s->phase==NNPI_BULK_DONE) s->cleanup_finished=now;
 /* EXEC follows REGISTER only with two acknowledged host mappings. */
 if (s->phase==NNPI_BULK_EXEC && s->mapped!=3) goto bad;
 return 1;
bad:
 nnpi_bulk_fault(s,2); return 1;
}
static inline int nnpi_bulk_may_release(const struct nnpi_bulk_state *s)
{
 return !s->published && !s->mapped && (!s->channel_issued ||
  ((s->phase==NNPI_BULK_DONE ||
    (s->phase==NNPI_BULK_HOST_VERIFY && (!(s->config&8u) || s->verified_bytes==NNPI_BULK_BYTES || s->verify_status==2))) &&
   s->stop_seen && s->channel_destroyed));
}
/* Called under CacheLock by the PASSIVE worker, one bounded block per turn.
 * Cleanup already completed; mismatch does not quarantine safely stopped DMA. */
/* Eight-byte compare against the position-bound word, with a byte-level
 * report only on the first mismatch. The original per-byte expected-value
 * loop was the dominant cost of phase 14 (~1 GB/s). */
static inline void nnpi_bulk_verify_span(struct nnpi_bulk_state *s,const uint8_t *data,uint32_t from,uint32_t to)
{
 uint32_t i,j;
 for(i=from;i+8u<=to;i+=8u) {
  nnpi_pattern_u64 expected=nnpi_c2h_word(s->run,NNPI_BULK_BINDING_GENERATION,i>>3);
  const uint8_t *p=data+i;
  uint64_t actual=(uint64_t)p[0]|((uint64_t)p[1]<<8)|((uint64_t)p[2]<<16)|((uint64_t)p[3]<<24)|
                  ((uint64_t)p[4]<<32)|((uint64_t)p[5]<<40)|((uint64_t)p[6]<<48)|((uint64_t)p[7]<<56);
  if(actual!=expected && !s->verify_status) {
   s->verify_status=1;
   for(j=0;j<8u;j++) {
    uint8_t want=(uint8_t)(expected>>(8u*j));
    if(p[j]!=want) {s->first_bad_offset=i+j;s->expected_byte=want;s->actual_byte=p[j];break;}
   }
  }
 }
 for(;i<to;i++) {
  uint8_t want=nnpi_c2h_byte(s->run,NNPI_BULK_BINDING_GENERATION,i);
  if(data[i]!=want && !s->verify_status) {
   s->verify_status=1;s->first_bad_offset=i;s->expected_byte=want;s->actual_byte=data[i];
  }
 }
}
static inline int nnpi_bulk_verify_step(struct nnpi_bulk_state *s,const uint8_t *data,uint32_t bytes)
{
 uint32_t i,end;
 if(s->phase!=NNPI_BULK_HOST_VERIFY || s->published || s->mapped ||
    !s->stop_seen || !s->channel_destroyed || !data || bytes!=NNPI_BULK_BYTES ||
    s->verified_bytes>NNPI_BULK_BYTES) return 0;
 if(!(s->config&8u)) return 1;
 end=s->verified_bytes+65536u; if(end>NNPI_BULK_BYTES)end=NNPI_BULK_BYTES;
 if(s->config&16u) {
  /* FAST: the card only guarantees the same first/last 64 bytes of each
   * 4KiB block, so checking more would neither add proof nor cost less. */
  uint32_t blk;
  for(blk=s->verified_bytes;blk<end;blk+=4096u) {
   uint32_t head_end=blk+64u,tail=blk+4032u,tail_end=blk+4096u;
   if(head_end>end) head_end=end;
   nnpi_bulk_verify_span(s,data,blk,head_end);
   if(tail>blk && tail<end) {
    if(tail_end>end) tail_end=end;
    nnpi_bulk_verify_span(s,data,tail,tail_end);
   }
  }
 } else {
  for(i=s->verified_bytes;i<end;) {
   nnpi_pattern_u64 v=nnpi_c2h_word(s->run,NNPI_BULK_BINDING_GENERATION,i>>3);
   nnpi_pattern_u32 h=i&7u,n=(8u-h<end-i)?8u-h:end-i,j;
   for(j=0;j<n;j++,i++) {
    uint8_t expected=(uint8_t)(v>>(8u*(h+j)));
    if(data[i]!=expected && !s->verify_status) {
     s->verify_status=1;s->first_bad_offset=i;s->expected_byte=expected;s->actual_byte=data[i];
    }
   }
  }
 }
 s->verified_bytes=end;return 1;
}
static inline uint8_t nnpi_bulk_pattern(uint64_t run,uint32_t offset)
{ return (uint8_t)(run+1u+offset); }
#endif
