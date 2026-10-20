# Root-cause notes — card-side C2H handle lifecycle

Source: the GPL-licensed card module `sphcs` (to be added under `card/` in a follow-up PR).
.

## The lifecycle as implemented

```
sphcs_net_dev_start_xmit()
  reserve handle i in s_c2h_busy_handles[0..c2h_pages)      (lines 185-195)
  map skb, start DMA xfer with completion callback          (lines 200-240)
      |
      v
sphcs_net_out_msg_dma_complete_callback()
  queue C2H MSG_DSCR (the "frame sent" notification)        (lines 128-133)
  dma_unmap_single + dev_kfree_skb_any                      (lines 141-146)
  *** does NOT clear s_c2h_busy_handles[i] ***
      |
      v
host copies the frame, sends H2C MSG_DSCR with is_ack=1
      |
      v
netmsg_op_work_handler()
  if (cmd.is_ack) s_c2h_busy_handles[cmd.skb_handle] = 0;   (lines 470-472)
```

`c2h_pages` is derived from the host-published C2H ring size:
`c2h_rb[0].size / NNP_PAGE_SIZE` (lines 571-572). With the current host pool that is 16.

## Defect 1 — out-of-bounds write on ACK

`skb_handle` is an 8-bit wire field (`NNPI_ETH_SLOT_BITS 8`, bits 28..35 of the descriptor).
The card writes `s_c2h_busy_handles[op->cmd.skb_handle]` with **no bounds check**
against `s_c2h_handles` (16). Any ACK carrying handle 16..255 writes past the 16-byte
`kcalloc` array and corrupts adjacent slab memory.

A single corrupted or buggy ACK is enough. Repeated under load, this presents as an
unexplained card panic at an arbitrary point (observed at ~7.3 GB).

Fix: validate `skb_handle < s_c2h_handles`, log/drop invalid ACKs, keep a per-slot
generation to detect duplicates.

## Defect 2 — duplicate ACK frees a reallocated slot

The ACK handler clears the slot unconditionally. If a duplicate ACK arrives after the
slot has been reused for a new frame, the new frame's busy bit is cleared while its DMA
is still in flight. The slot can then be reserved again, so two skbs share one host page:
use-after-free / cross-frame corruption.

Note: the tunnel layer already shows large duplicate counts (`dup=36062` in one rawbridge
run), so duplicates are not hypothetical at the transport level.

Fix: store a generation/epoch per slot at reservation; accept an ACK only if it matches
the current generation, then clear.

## Defect 3 — lost ACK leaks the slot forever

The handle is only released by the host ACK. If an ACK is lost (ring pressure, host bug,
corrupted frame), the slot stays busy forever. With 16 slots, 16 lost ACKs permanently
stop C2H traffic: `c2h ringbuffer full`.

There is no timeout, no quarantine, no recovery.

Fix: a completion watchdog that quarantines a slot after a bounded time, requires the
host to confirm (or an epoch bump to invalidate stale DMA), then reclaims it. At minimum,
count and surface leaked slots so the host can reset the session cleanly instead of
deadlocking.

## Related defects

- **No backpressure**: `sphcs_net_dev_start_xmit` drops the skb when the ring is full
  (`kfree_skb`, return 0) instead of `netif_stop_queue`. TCP collapses and control traffic
  starves; the drop itself is counted but not propagated.
- **DMA pool exhaustion path does not ACK the host**: in `sphcs_net_process_command`,
  if `dma_page_pool_get_free_page` fails, the function returns without sending an ACK for
  the host's H2C slot (lines 430-440). The host's TX slot leaks, the mirror image of
  Defect 3.
- **`kzalloc(GFP_NOWAIT)` failure path**: if the work item cannot be allocated, an ACK
  is dropped (fine) but a data descriptor triggers `sphcs_net_send_ack`; any failure there
  leaks the host slot.

## Fix plan for v1.0.0 (Blocker A)

1. Bounds + generation checks on every ACK (Defects 1 and 2).
2. Per-slot timeout watchdog with quarantine (Defect 3); expose leak counters via debugfs.
3. Real backpressure: `netif_stop_queue` below a free-slot watermark, wake on ACK.
4. ACK the host on every DMA-pool failure path.
5. Panic-safe forensics: persistent card log (pstore/ramoops or a host-readable reserved
   buffer) so the next crash is attributable.
6. Acceptance: 20 GB sequential NBD traffic with `ring_full == 0`, no leaked slots, no
   card reboot, no host NMI.
