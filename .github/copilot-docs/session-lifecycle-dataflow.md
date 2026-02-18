# Session Lifecycle & Data Flow — Design Knowledge

## The Layered Abstraction: Pipeline → Session → Transport

MTL has three API layers, and understanding which to use (and why all three exist) is essential:

| Layer | API Pattern | Who Uses It | What It Handles For You |
|-------|-------------|-------------|------------------------|
| **Pipeline** | `st20p_tx_get_frame()` / `put_frame()` | Most applications | Format conversion, frame lifecycle, blocking/polling, codec integration |
| **Session** | `st20_tx_create()` + callbacks | Advanced users | Direct control of frames/slices/RTP, no automatic conversion |
| **Transport** | Internal only | MTL core | Packet construction, pacing, NIC interaction |

**Why three layers?** Because different users need different levels of control. A GStreamer plugin just wants "give me a buffer, I'll fill it, take it back" (pipeline). A professional broadcast appliance wants to control exactly which RTP packets go where (session with RTP mode). And the transport layer is never exposed — it's an implementation detail.

**The pipeline wraps a session**: `st20p_tx_create()` internally calls `st20_tx_create()`. The pipeline adds frame management, optional format conversion (via plugin), and a simpler get/put API. If you read pipeline code and wonder where the packets are built — it's in the session layer underneath.

## Session Creation: What Actually Happens

Session creation is a multi-step resource acquisition. Understanding the order matters because cleanup must reverse it exactly:

1. **Validate ops** (`*_ops_check`) — reject invalid configs early, before allocating anything
2. **Acquire scheduler quota** — find/create a scheduler with capacity for this session's weight
3. **Attach to manager** — claim a slot in the per-scheduler session array (protected by manager mutex)
4. **Allocate resources** — frames, mempools, rings, mbufs (all NUMA-pinned to the NIC's socket)
5. **Acquire NIC queue** — dedicated or shared, depending on flags
6. **Install flow rule** (RX only) — steer matching packets to this session's queue
7. **Initialize pacing** (TX only) — set up rate limiter or TSC timing
8. **Register tasklets** — builder/transmitter (TX) or receiver (RX)

**Why this order?** Cheap checks first (validation), then scarce resources (scheduler, queue), then expensive resources (memory). If step 5 fails (no queues available), you haven't wasted hugepage allocations.

**Cleanup reversal**: `*_uinit()` and `*_detach()` undo these steps in reverse. The most common bug in session cleanup is freeing resources in the wrong order — e.g., freeing the mempool while mbufs from it are still queued in a ring.

## The Manager Pattern

Sessions don't just float freely — they're organized into **managers** (`st_tx_video_sessions_mgr`), one per session type per scheduler. The manager:
- Owns the session array (fixed-size, indexed by `idx`)
- Owns the manager mutex (protects array during attach/detach)
- Provides the tasklet handler that iterates over all sessions in the array

**Why per-scheduler, not global?** Because the tasklet polling the sessions runs on a specific scheduler. If sessions were in a global pool, the tasklet would need to filter by scheduler — wasted cycles. One manager per scheduler means the tasklet just iterates its local array.

## Frame State Machines

### TX Pipeline Frame States
```text
FREE → (app gets frame) → IN_USER → (app puts frame) → READY
  ↑                                                        │
  │                                          (converter picks up)
  │                                                        ▼
  └── (frame_done) ── IN_TRANSMITTING ← (session grabs) ← CONVERTED / READY
```

**Key insight**: The converter is optional. If input format == transport format, frames go directly from READY to IN_TRANSMITTING. The state machine handles both paths uniformly.

**Late frame handling**: If a frame is still IN_USER when its epoch arrives, it's skipped. If it's READY but a newer frame is also READY, the older one is dropped (newest-first policy). This means the app can be slow occasionally — MTL just shows the latest frame.

### Session-Level TX Frame States (distinct from pipeline states)
All four media types share the same 2-state cycle (enums in `st_header.h`):

```text
WAIT_FRAME ──(get_next_frame succeeds)──► SENDING_PKTS
    ▲                                          │
    └───────(pkt_idx >= total_pkts)────────────┘
```

- `WAIT_FRAME`: Builder polls for next frame from app/pipeline
- `SENDING_PKTS`: Builder constructs and enqueues packets; pacing waits (video) happen via TSC comparisons *within* this state, not a separate state

**Note**: `ST21_TX_STAT_WAIT_PKTS` exists in the video enum but is dead code — never assigned or checked.

### RX Pipeline Frame States
```text
FREE → (transport delivers) → READY → (convert if needed) → IN_USER → (app returns) → FREE
```

**Simpler than TX** because the receiver doesn't make timing decisions — it just assembles, optionally converts, and hands off.

## RX Packet Assembly: The Slot Design

RX video assembly is hard because:
- Packets can arrive out of order
- Multiple frames can be in-flight simultaneously (one being assembled, previous one being consumed by app)
- Packets from different frames (different RTP timestamps) must be sorted correctly

MTL uses **slots** (typically 2-3 per session):
- Each slot represents a frame being assembled
- A slot is identified by the RTP timestamp of its first packet
- Packets arriving with an unknown timestamp claim a free slot
- When a slot is complete (all expected packets received), the frame is ready

**Why not just a single buffer?** Because network reordering means packets from frame N+1 can arrive before the last packet of frame N. With a single buffer, you'd have to drop frame N+1's early packets. With slots, both frames can be assembled simultaneously.

**The bitmap tracker**: Each slot has a bitmap of which packets have been received. This enables:
- Detecting completion (all bits set)
- Detecting gaps (for RTCP NACK requests)
- Knowing exactly which data has been written (for integrity checking)

**Header-split optimization**: On Intel E810 with `ST20_RX_FLAG_HDR_SPLIT`, the NIC writes payload data directly into the frame buffer, bypassing the CPU memcpy step entirely. See `dpdk-usage-patterns.md` for details.

## RTCP Retransmission Design

Standard RTCP (RFC 3550) doesn't define a good mechanism for selective retransmission of ST2110 video. MTL uses a **custom RTCP format** (type 204, name "IMTL"):
- RX detects gaps in the bitmap → sends NACK listing missing sequence number ranges
- TX maintains a circular buffer of recently sent packets → deep-copies requested packets and retransmits
- FCI format: (start_seq, follow_count) ranges — compact for burst losses

**Why deep-copy for retransmit?** Because the original mbufs may have been freed or their backing frame may have been recycled. The circular buffer holds independent copies.

**Why custom format?** Standard RTCP NACKs (RFC 4585) encode individual sequence numbers, which is inefficient for ST2110's thousands-of-packets-per-frame. The range encoding is much more compact.

## Redundancy: Active-Active Merging

ST2110-7 redundancy sends the **same stream on two separate network paths**. The receiver gets both and uses the first-arriving copy of each packet. But the merge strategy differs fundamentally between media types:

### Video RX: Packet-Level Bitmap Merge
- Two ports in `ops.port[]`, each with its own IP/queue/flow rule
- The RX assembly slot uses the same bitmap for both paths — **first write wins** (atomic compare-and-swap on per-packet slot)
- Per-port stats (`pkts_recv_per_port[P]`, `pkts_recv_per_port[R]`) track which path contributed what
- Even if one path drops 50% of packets, as long as the other has the missing ones, the frame assembles perfectly
- Extra slot allocated for redundancy: `slot_max = 2` vs `1` for single-port

### Audio/Ancillary RX: Timestamp-Based Dedup (Completely Different)
- No bitmap, no slot system — audio packets are sequential, not random-access
- Packets are rejected if their **RTP timestamp is not greater** than the session's current timestamp (`mt_seq32_greater`)
- This means: whichever path delivers first advances the timestamp, and the duplicate from the other path is silently dropped as "old"
- **Safety valve**: If timestamp-based dedup keeps rejecting packets for 20 consecutive tries (`ST_SESSION_REDUNDANT_ERROR_THRESHOLD`), the filter is bypassed. This prevents permanent lockout from timestamp wraparound or clock discontinuities
- When you see "redundant error threshold reached" in logs — investigate the source, the safety valve is masking a real problem

### TX Redundancy: Simple Duplication
- Builder constructs one packet, then clones it with different Ethernet/IP headers for the second port
- The clone **shares the same chain payload mbuf** (refcnt incremented) — both ports DMA from the same frame data
- **Gotcha**: This doubles the effective DMA reference lifetime. The frame isn't free until BOTH ports complete DMA
- If ports have different NIC models, the redundant port may be forced to TSC pacing even if the primary has HW rate limiting

## Multicast Group Management

Sessions automatically join/leave IGMP multicast groups during attach/detach. This is handled transparently:
- `st_tx_join_multicast()` called during session init
- `st_rx_join_multicast()` sends IGMP join for the session's multicast destination IP
- On destroy, IGMP leave is sent
- The NIC's multicast filter is updated via `rte_eth_dev_mac_addr_add()` / `mcast_addr_add()`

**Gotcha**: If the switch doesn't support IGMP snooping, multicast traffic floods all ports. This doesn't break MTL but wastes bandwidth. Ensure IGMP snooping is enabled on your switch.

## Audio, Ancillary, and Fast Metadata: Same Pattern, Simpler

Audio (ST2110-30), ancillary (ST2110-40), and fast metadata (ST2110-41) sessions follow the exact same lifecycle and manager pattern as video, but:
- **No pacing complexity** — audio packets are small and sent at frame boundaries (1ms or 125µs)
- **No assembly complexity** — audio frames are typically 1 packet per frame, ancillary even simpler
- **Low quota** — many audio/ancillary/fast-metadata sessions fit on one scheduler
- **Shared queue friendly** — their low bandwidth makes shared queues efficient
- **ST2110-41 specifics** — fast metadata has payload type 115, its own RTP header (`st41_rtp_hdr`/`st41_fmd_hdr`, 58 bytes), and API in `st41_api.h`. Naming prefix: `tx_fastmetadata_*`/`rx_fastmetadata_*`

**The design is deliberately uniform**: Adding a new media type means copying the video session pattern and simplifying. Don't invent a new architecture for new ST2110-xx types.

## Global Initialization Ordering

The `mtl_init()` → `mtl_start()` → `mtl_stop()` → `mtl_uninit()` lifecycle has ordering constraints that aren't obvious:

**`mtl_init()` does**: DMA init → queue subsystem → ARP/multicast tables → CNI → admin thread → plugin loading → DHCP → PTP → launches TSC calibration thread

**`mtl_start()` blocks on TSC calibration** before starting NIC ports. This is critical: pacing requires accurate TSC frequency. Starting the NIC before TSC is calibrated produces incorrect packet timing.

**Sessions can be created between init and start**: They allocate queues, mempools, and flow rules against a stopped port. Traffic only flows after `mtl_start()`. This works on E810 but may fail on NICs that require an active port for flow rule installation.

**`MTL_FLAG_DEV_AUTO_START_STOP` changes everything**: When set, `mtl_init()` calls start internally, and `mtl_stop()` becomes a no-op. This is the simpler model — use it unless you need explicit start/stop control.

**Uninit is the exact mirror of init** — reversed order. The most common bug in custom shutdown sequences is destroying subsystems out of order.

## Plugin Contract

(See also `architecture-and-design-philosophy.md` for the pipeline-session duality and when to use plugins.)

Plugins (converters, encoders, decoders) follow a strict 3-phase contract:

1. **Registration**: `dlopen` → resolve 3 symbols (`st_plugin_get_meta`, `st_plugin_create`, `st_plugin_free`) → version/magic check → create
2. **Session binding**: Pipeline searches registered devices by **capability bitmask** (`input_fmt_caps`, `output_fmt_caps` are OR'd `MTL_BIT64(fmt)` values). First match wins — no priority system.
3. **Frame exchange**: Plugin's processing thread calls `get_frame()` → process → `put_frame()`. MTL wakes the plugin via `notify_frame_available` callback.

**Key design decisions**:
- Plugins own their own threads — they're NOT called from tasklets. They use `pthread_mutex_t`, not spinlock.
- First-fit matching means if two plugins support the same format, registration order determines which is used.
- Plugins are loaded during `mtl_init()` from JSON config file paths — they must be available at library init time, not later.
- Max 8 plugins per MTL instance.

## Validation Checklist (`*_ops_check()`)
- `num_port`: 1-2, `payload_type`: 0-127, `framebuff_cnt`: 2-8 for video
- IP: not all zeros, multicast = 224.x-239.x, redundant ports must differ
- Check required callbacks based on `type` field (frame, slice, RTP)
- Pipeline: `input_fmt`/`output_fmt` must be supported by available converter

