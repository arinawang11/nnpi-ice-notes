# NNP-I 1150 boot chain

*[中文版](boot-chain.zh-CN.md)*

Getting this card from a cold slot to a working SSH session involves three
separate chains:

1. PCIe enumeration and driver binding on the Windows host;
2. the card-side boot agent (called BIOS in the protocol) reading a boot
   image into card memory over DMA and starting the x86 SoC;
3. the runtime handshake, channel setup and Ethernet data plane between the
   host driver and the Linux running on the card.

There is a fourth chain, the one that loads TLC firmware and dispatches work
to DELPHI. I do not have that firmware, so it never ran. It is separate from
the first three: a booted x86 core does not mean the accelerator is usable.
The first three are covered here; the fourth is covered only as far as why it
stopped.

The state values, timeouts and message formats below come from the driver's
protocol implementation and from actual runs. I never obtained or
disassembled the card-side boot agent, so statements about its behaviour go
only as far as the protocol and the observed state transitions support.

## 1. Topology and responsibility boundaries

```
Windows host
  └─ PCIe function 45c6
       ├─ control window: command FIFO, response FIFO, doorbell state
       ├─ host-ready window
       ├─ runtime response window
       └─ SYSINFO push window
            │
            │ PCIe / DMA
            │
NNP-I card
  ├─ boot agent ("BIOS")
  │    ├─ answers SYSINFO requests
  │    └─ reads the boot image via the descriptor table
  ├─ x86 SoC, 2 cores / 4 threads
  │    └─ Linux 5.7 + Buildroot initramfs
  │         ├─ card-side network driver
  │         └─ PyTorch CPU runtime
  └─ DELPHI / TLC / MMU / IVP / ASIP
       └─ TLC firmware missing, compute chain not reached
```

The Windows driver binds only the `45c6` function. `45c4` is the ICE
interface itself on the card side and is not owned by this driver. The driver
also does not control card power; the slot provides it.

There is no arbitrary shared-memory interface between host and card. The host
pushes QWORDs into a fixed-width command FIFO and the last write triggers a
notification. The card puts replies into a response FIFO, which the host
polls and acknowledges. The low byte of the doorbell window reports how far
the card has got. Every stage below is built on that model.

The command FIFO is 16 QWORDs deep. A command is written as two 32-bit
halves, low then high, and the last write carries the notification. The
response FIFO has the same depth; its read and write pointers live in the
control word. A full drain of 16 QWORDs advances an intermediate pointer
first, then writes the final read pointer. Each poll snapshots the control
word, reads exactly the QWORDs in that snapshot, and acknowledges at the end.
A parse failure never replays an acknowledged snapshot.

## 2. Power, enumeration and driver binding

When the card is plugged in, the card-side boot agent is already running by
the time the host enumerates PCIe and sees two functions. Windows PnP matches
the hardware ID in the INF and loads the driver for `45c6`.

`NnpiDeviceAdd` does a few things:

- re-checks the PnP hardware ID rather than trusting the INF match alone;
- creates the device interface and a buffered I/O queue;
- registers PnP, power and file-cleanup callbacks;
- does not touch device registers at this point.

Actual access is established in `PrepareHardware`:

- walk the translated resource descriptors and record memory, port, interrupt
  and device-private resources; reject `MemoryLarge` outright instead of
  guessing its union layout;
- read 64 bytes of PCI configuration space through
  `BUS_INTERFACE_STANDARD.GetBusData`;
- use the BAR information from config space to validate the target windows
  instead of assuming a BAR index;
- map five windows: status, control snapshot, host-ready, runtime response
  and the SYSINFO push port;
- map a separate interrupt/status window, mask all interrupts, and use
  polling at runtime;
- prepare common-buffer DMA, query FLR support and acquire the reset
  interface;
- increment the generation. The generation identifies one `PrepareHardware`
  incarnation; the epoch is bumped on D0 exit or device invalidation. Both
  are used to reject stale userspace requests.

This stage only reads PCI config; it does not write device registers. A failed
mapping only disables the corresponding capability. `boot-submit` later
checks that host-ready, the control snapshot, SYSINFO, the runtime window and
FLR are all available, and refuses to start if one is missing.

Right after a host reboot, the card's BAR can read all `0xFFFFFFFF` for a
while, which is consistent with the card-side logic not being up yet. The
boot worker therefore waits before taking the first real baseline: up to 600
polls at 500 ms, until all four words stop reading all-ones. If they are
still all-ones after that, it fails. It does not try to rewrite the value.

## 3. Staging the image in host memory

The boot image is not in this repository. The driver only receives it,
validates it, and turns it into something the card can DMA from. There are
three steps inside `boot-load`: BEGIN, CHUNK and SEAL.

### 3.1 BEGIN

`boot-query` returns the current generation and epoch. `boot-load` passes
both; the driver verifies that the device is in D0 and that generation/epoch
match, then:

- generates a random session id;
- allocates 181 DMA common buffers for the image chunks;
- allocates one 4 KiB table buffer;
- moves the load state from EMPTY to LOADING.

The image is exactly 188,744,192 bytes: 180 chunks of 1 MiB plus a final
512-byte chunk. The chunk count is asserted to 181 at compile time.

### 3.2 CHUNK

Userspace sends the data in order. The driver requires the offset to equal
the number of bytes already written; skipping, overwriting and reordering are
rejected. Each chunk is a separate DMA mapping. The last chunk is only 512
bytes but its mapping still covers a full page, with the tail zeroed.

This step writes host memory only. No MMIO, and no address is given to the
card yet.

### 3.3 SEAL

Once every byte is present:

1. Check whether the image begins with the EFI capsule GUID. If it does,
   reject it. This happens after the length check and before hashing, so a
   capsule that lies about its size is still caught.
2. Hash the 181 chunks in order with SHA-256 and compare against the expected
   value compiled into the driver. A mismatch stops before SEALED and the
   image cannot be submitted.
3. Build one contiguous descriptor table: a 24-byte header followed by 181
   eight-byte entries. Each entry carries the DMA page-frame number and page
   count of one chunk. Both the chunks and the table are DMA common buffers
   belonging to the same device, so the card can read them directly.

The table must be one contiguous mapping, not a linked list of pages. During
SEAL the driver zeroes the padding area of every chunk, clears the whole
table and rewrites it, so the card cannot read leftover host memory.

After a successful SEAL the state is SEALED. The device still has not been
touched.

### 3.4 SUBMIT

`boot-submit` requires:

- state SEALED;
- all 181 chunks and the table still present;
- the SYSINFO DMA page ready;
- host-ready, control snapshot, SYSINFO and runtime windows mapped;
- a reset interface that supports function-level reset;
- no other boot worker running.

When everything is in place the driver allocates a work item, queues the boot
worker and returns immediately. The hardware work happens in the worker, so
userspace does not block for ten minutes.

## 4. First contact: baseline, host-ready and BIOS_READY

The worker first confirms that the session is still current, then reads four
baselines:

- the current host-ready value;
- the command FIFO control word;
- the response FIFO control word;
- the doorbell state.

The baseline is strict: host-ready must be 0, the doorbell low byte must be 0
or 1, and both FIFOs must be empty. Any mismatch, or an all-ones read, fails
before host-ready is written or any DMA is started.

Once the baseline passes, the driver writes a fixed 9 to host-ready. This is
the boot-stage driver-ready action, not an arbitrary register value. It then
polls the doorbell low byte for 1 (BIOS_READY), for up to 20 polls at 500 ms,
i.e. ten seconds. A 0 in between is normal waiting; any other value aborts.

On this card, the first doorbell read after binding returns 0 or 1, and it
moves to 1 after the 9 is written. The card-side agent is already responsive
before the driver binds.

## 5. SYSINFO: find out who is on the other side

After seeing 1, the driver requires the command FIFO to be empty and sends a
SYSINFO request. The message is three QWORDs, written as six half-writes to
the push window, with the last one carrying the notification:

- QWORD 0: opcode 31, type `0x11`, payload length 16;
- QWORD 1: address of a 4 KiB DMA page;
- QWORD 2: page length, 4096.

The card-side agent writes its information into that DMA page. The driver
polls the doorbell for a low byte of 10, again for up to 20 polls at 500 ms.
On 10 it parses the 123-byte payload and checks the version, CPU string, ICE
information, UTF-16 BIOS string, CSME and PMC fields. A parse failure aborts
the load; the image is not submitted.

The driver uses SYSINFO to confirm the card identity and firmware state.
SYSINFO success is a precondition for sending the image descriptor.

## 6. BOOT_IMAGE_READY: hand the image over

After SYSINFO, the driver marks the image and descriptor table as published.
From that moment they are owned by DMA and error paths may not free them.
Then it sends BOOT_IMAGE_READY:

- opcode 31, type `0x10`, payload length 16;
- QWORD 1 points at the first entry after the table header;
- QWORD 2 carries the descriptor byte count in the low 32 bits and the total
  image size in the high 32 bits.

The message itself is 24 bytes; the data lives in the 181 chunks described
by the table. The card-side agent reads them in order over DMA and starts the
x86 cores.

The driver then watches the doorbell:

- 3 means BOOT_STARTED, image transfer has begun;
- 4 or 8 means DRIVER_READY, the card-side runtime is up;
- 10 before 3 is still treated as waiting;
- once 3 has been seen, 10 is a protocol error;
- the limit is 1200 polls at 500 ms, i.e. ten minutes.

3 only means "transfer started", not "DMA finished". The driver keeps the
image, the table and every mapping alive for the whole window. Releasing or
reusing them only becomes possible after 4/8, or after the evidence-bearing
reset path described below.

The image contains a Linux 5.7 kernel and a Buildroot initramfs. Once the x86
cores are up, the card-side network driver configures its interface and the
host can set up a channel. The rootfs is in RAM, so every reboot needs a
fresh image load and any runtime changes are lost.

The screenshot below is from the card desktop after boot. neofetch reports
Buildroot 2019.11, kernel 5.7.0, four threads and 15,370 MiB of RAM; CPU-X
reports a core speed of 495 MHz, with 48 KiB L1D and 32 KiB L1I per core,
512 KiB L2 per core and 24 MiB shared L3. The frequency caveats are in the
[card runtime notes](card-runtime.md#6-实际运行画面).

![neofetch and CPU-X on the card](images/card-neofetch-cpux.png)

## 7. Runtime handshake

When the doorbell reaches 4 or 8, the boot worker does not exit; it enters the
runtime loop. Before that it checks:

- the command FIFO is empty;
- the doorbell low byte is 4 or 8;
- the doorbell error bits are zero.

It then sends QUERY_VERSION. This command is a single all-zero QWORD, still
through the command FIFO. The card replies with a three-QWORD version message,
opcode 2:

- protocol version 4.1.0 (`0x1020`), firmware version and channel version
  1.6.0 (`0x04c0`) in QWORD 0;
- QWORD 1: per-opcode response length table;
- QWORD 2: per-opcode command length table.

The driver accepts only opcode 2, three QWORDs, a major/minor protocol match
and a non-zero response length table. The patch level is ignored. Once the
version checks out, the driver sets the driver-ready field in host-ready to 1
(value `0x100`) while preserving the other bits. Only now does
`RuntimeCompatible` become true.

After that, every 5 ms:

- read the doorbell and check state and error bits;
- take up to 16 QWORDs from the response FIFO and acknowledge them;
- parse each message and route it to version, event, Ethernet config,
  Ethernet descriptor or SYSINFO handling;
- update the keepalive;
- if the network is up, pump TX/RX descriptors.

The keepalive is a counter field cycling 1 through 14. Zero is the initial
state and is never emitted; 15 is reserved and rejected. It is updated in
host-ready each round while preserving the other fields.

An unknown opcode, a half-received message, an all-ones FIFO read or a
doorbell error bit terminates the runtime and preserves the evidence. A
version query that has already gone out is not resent, and a FIFO snapshot
that has been acknowledged is not replayed.

## 8. Network channel

The network is not just a NIC registered in the kernel. It is a non-context
channel built on the existing IPC. The `net-begin` preconditions are:

- the boot worker is alive and the phase is DRIVER_READY;
- the runtime is compatible and healthy;
- channel version 1.6.0 and the length tables match;
- generation/epoch are current;
- a valid unicast MAC and a non-zero card IP are supplied;
- the channel id starts at 256 (0 through 255 are reserved for inference
  contexts).

The sequence is fixed and every step waits for its event:

1. `CHANNEL_OP` (opcode 22) creates the channel; wait for event 21
   (create success).
2. `CHANNEL_RB_OP` (opcode 23, H2C) publishes the host-to-card ring. What is
   published is the page-frame number of the ring chain header, not the
   address of the first data page. Wait for event 23 (ring set).
3. The same `CHANNEL_RB_OP` publishes the C2H ring; wait for event 23 again.
4. `ETH_CONFIG` (opcode 57) sends two QWORDs: the card IP and MAC. The card
   replies with opcode 39 echoing the IP. An echo of 0 means rejection.
5. READY.

Each observation window is 120 polls at 5 ms, i.e. 600 ms. A card-fatal
event (112 and above) terminates immediately.

Each direction has its own DMA page pool: sixteen 4 KiB data pages plus one
4 KiB chain page. The chain page describes the sixteen data pages in
page-block form; the card only receives the chain page's frame number, and
the data pages are reached through the chain. This lets the host manage its
pool without changing any address already published.

## 9. Data plane

Frames travel through descriptors; they are not copied into the control FIFO.

To send, the bridge hands a frame to the driver, the driver reserves a free
slot, copies the frame into the corresponding 4 KiB page and emits an H2C
descriptor (opcode 58) carrying the slot and length. When the card consumes
it, it returns a C2H descriptor (opcode 40) with the ACK flag set and length
0; only then does the driver mark the slot free. A successful send means
"queued", not "transmitted by the card" and definitely not "received by the
peer".

To receive, the card writes a frame into a C2H page and returns descriptor
40. The driver validates the slot and length, copies the frame into a
four-entry CPU queue, and sends an ACK descriptor to return the page. The
bridge claims frames through the RX claim/batch interface and writes them to
the TAP adapter. If the queue is full, the driver still ACKs and counts a
drop; it does not hold the page.

A lost completion is treated as packet loss, never as a reason to replay.
Replaying would put the same frame on the network twice, which is worse than
losing it. The driver and the bridge follow the same rule.

## 10. Teardown, failure and FLR

Normal close, PnP stop, D0 exit and device removal all use the same order:

1. stop the network and boot producers: clear the runtime-healthy flag, set
   closing, wake the workers and wait for `NetDone`;
2. set `ResetBlocked` so every old MMIO path is invalid;
3. invalidate the interrupt policy and stop touching the device;
4. if host-ready was written, no runtime fault is retained, this session has
   not already used reset, and the reset interface is available, perform one
   FLR;
5. use `FunctionLevelDeviceReset` with flags 0, no callback and NULL
   parameters; do not touch the device after it returns;
6. only if FLR returns `STATUS_SUCCESS` mark the published DMA as stopped and
   allow the image, table and network pools to be freed.

If FLR fails or is skipped, buffers already published to DMA are not freed,
and the PnP barrier keeps waiting. `NnpiBootBarrier` checks every 5 ms for
stop evidence and does not return without it. The card may still be
DMA-reading those pages; freeing them would let it write into memory that now
belongs to someone else. Buffers that were never published are not held by
this rule.

If the runtime hits a parse error, an MMIO exception or a partial publication,
the driver retains the fault and does not reset automatically. A human has to
decide whether to reload the image or power-cycle the card cleanly. A timeout
is not evidence that DMA stopped; it only means this operation failed.

## 11. What the driver does at each stage

| Stage | Driver action | On failure |
| --- | --- | --- |
| enumerate/bind | verify hardware ID, create device interface | not loaded |
| PrepareHardware | read config space, map windows, prepare DMA and FLR | capability disabled, submit refused |
| image load | 181 DMA buffers, sequential writes | stays in LOADING, BEGIN again |
| SEAL | reject capsule, SHA-256, build descriptor table | submit refused |
| SUBMIT | check every precondition, queue worker | not ready, device untouched |
| baseline | four all-ones checks, empty FIFOs, host-ready=0 | fail before DMA |
| BIOS wait | write 9, poll doorbell for 1 | 10 s timeout or protocol error |
| SYSINFO | send 3 QW, parse 123 bytes | abort, image not submitted |
| image submit | publish DMA, send descriptor, wait for 3/4/8 | retain buffers, run cleanup |
| runtime | version handshake, host-ready, keepalive, routing | terminate and retain evidence |
| network | create channel, publish rings, ETH_CONFIG | network-only failure, boot survives |
| teardown | stop producers, one FLR, wait for stop evidence | retain buffers, barrier does not return |

Network failures do not always stop boot: channel setup failures, event
timeouts and descriptor errors only mark the network failed; the boot
runtime keeps running. The network is optional, and a TAP or userspace
bridge problem should not tear down the whole card runtime. If the failure is
an MMIO exception, boot closes too, because there is no basis for trusting
later reads.

## 12. Why the compute chain is not part of this boot chain

The image loaded above is the card's x86 Linux, not TLC firmware. DELPHI
needs TLC to run first; TLC receives host commands and sends config and exec
messages to DELPHI over the on-chip network. The only host-side control
channel is a four-bit-opcode message register, so there is no way to forge an
exec command from the CPU side.

I tried configuring MMIO directly, bypassing TLC: power, MMU page tables,
geometry, credits, state messages and start. DELPHI accepts start and its
idle flag flips, but the unit-of-work scheduler dispatches nothing, all LSU
channels stay empty, the size registers never latch, and no AXI or MMU
traffic appears. I ruled out runstall, credit reset, state-message retries
and re-triggering the valid bits. The details are in `experiment-log.md`.

"Boot chain complete" means the x86 side is usable. The accelerator compute
chain is a different chain, and what it is missing is TLC firmware, not
another block of host driver code.

## 13. Minimal reproduction order

```text
nnpi-info boot-query --index 0
nnpi-info boot-load --index 0 --generation G --epoch E --image PATH
nnpi-info boot-submit --index 0 --session S --generation G --epoch E
nnpi-info runtime-info --index 0
nnpi-info net-begin --index 0 --generation G --epoch E \
          --card-ip 192.168.115.2 --mac 02:00:00:00:00:02
nnpi-net-bridge.exe --device-instance 'PCI\VEN_8086&DEV_45C6&SUBSYS_...' \
          --tap-guid '{...}' --session S --generation G --epoch E
```

`boot-load` already covers BEGIN, CHUNK and SEAL; `boot-submit` starts the
worker. `runtime-info` confirms version compatibility and whether the network
gate is open. The bridge must use the same session as the driver.

The image, firmware and card-side configuration are not in this repository.
Without an image the driver still builds, binds and answers read-only
queries, but the boot chain stops at SUBMIT.

## 14. Limits of the evidence

Values were taken from the driver's protocol headers, the state machine and
actual runs:

- image length, chunk count and expected SHA-256;
- doorbell states 0/1/3/4/8/10;
- boot timeouts of 20/20/1200 polls at 500 ms;
- the 5 ms runtime poll and keepalive range 1..14;
- network lifecycle opcodes 22/23/57 and events 21/23/39.

I did not obtain the card-side boot agent. The card-side Linux was only
observed at runtime; I did not analyse its source. Statements such as "the
card reads the table", or "the card-side network driver configures the
interface", rest on the protocol semantics, the state transitions and
observable behaviour after boot, not on a line-by-line analysis of card
code.

Register offsets, bit-field tables, vendor source, firmware, boot images and
private keys are not in this repository. What is here stops at stages and
driver responsibilities; it is not an address-level operations manual.
