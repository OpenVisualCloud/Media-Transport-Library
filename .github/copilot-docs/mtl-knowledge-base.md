# MTL Knowledge Base — AI Agent Context

> **Purpose**: Single consolidated reference for AI agents working on MTL.
> This file replaces all prior per-topic copilot-docs files.
> Last verified against codebase: 2025-01.

---

## §1 Architecture & Design Philosophy

### What MTL Is
SMPTE ST 2110 media transport over IP. DPDK-based with HW pacing (Intel E810). Supports:
- ST2110-20 (uncompressed video)
- ST2110-22 (compressed video / JPEG-XS)
- ST2110-30 (audio)
- ST2110-40 (ancillary data)
- ST2110-41 (fast metadata)

### Two-World Pattern (Data Plane / Control Plane Split)
- **Data plane**: DPDK hugepage memory, spinlocks, lock-free rings, zero-copy, polling tasklets. Hot path — never blocks.
- **Control plane**: Heap memory, mutexes, POSIX threads. Session create/destroy, config, stats — allowed to block.

Rule: A data-plane code path must NEVER call a control-plane function (no malloc, no mutex, no log at INFO level).

### Three API Layers

| Layer | API Pattern | Who Uses It | What It Handles |
|-------|-------------|-------------|-----------------|
| **Pipeline** | `st20p_tx_get_frame()` / `put_frame()` | Most apps | Format conversion, frame lifecycle, blocking/polling, codec |
| **Session** | `st20_tx_create()` + callbacks | Advanced users | Direct control of frames/slices/RTP |
| **Transport** | Internal only | MTL core | Packet construction, pacing, NIC interaction |

Pipeline wraps session: `st20p_tx_create()` calls `st20_tx_create()` internally.

### Naming Conventions

| Prefix | Component |
|--------|-----------|
| `mt_` | Core library (non-media) |
| `st_` / `st20_` / `st22_` / `st30_` / `st40_` / `st41_` | Media session APIs |
| `st20p_` / `st22p_` / `st30p_` | Pipeline APIs |
| `tv_` | TX video session internals |
| `rv_` | RX video session internals |
| `tx_audio_session_` | TX audio internals |
| `rx_audio_session_` | RX audio internals |
| `tx_ancillary_session_` | TX ancillary internals |
| `rx_ancillary_session_` | RX ancillary internals |
| `tx_fastmetadata_` / `rx_fastmetadata_` | ST2110-41 internals |

### Video Complexity vs Audio/Ancillary/Fast-Metadata

| Aspect | Video (ST2110-20/22) | Audio/Ancillary/FMD |
|--------|---------------------|---------------------|
| Packets/frame | ~4,500 (1080p60) | 1–8 |
| Pacing | Per-packet (µs precision) | Per-frame (ms) |
| Assembly | Slot-based, bitmap, DMA | Sequential, trivial |
| Separate builder+transmitter? | Yes (ring-coupled) | No (single tasklet) |
| Shared queue friendly? | Only with flow director | Yes (low bandwidth) |

**Uniform design**: Adding a new ST2110-xx type = copy the video session pattern and simplify. Don't invent new architecture.

### End-to-End TX Data Journey
```text
App fills frame buffer (hugepage)
→ Pipeline: format convert (optional, via plugin)
→ Session: builder tasklet constructs RTP packets (header mbuf + extbuf chain to frame)
→ Builder enqueues to rte_ring
→ Transmitter tasklet dequeues, applies pacing (RL or TSC)
→ mt_txq_burst() → NIC DMA reads payload from frame buffer
→ NIC transmits on wire
→ extbuf refcnt hits 0 → frame buffer freed for reuse
```

### End-to-End RX Data Journey
```text
NIC receives multicast packet (flow rule steers to session's RX queue)
→ rte_eth_rx_burst() returns mbufs
→ RX tasklet: parse RTP header → extract tmstamp, seq_id, SRD fields
→ Find/create slot by tmstamp (rv_slot_by_tmstamp)
→ Calculate frame offset from SRD row/offset
→ Bitmap test-and-set (mt_bitmap_test_and_set) → skip if duplicate
→ memcpy (or DMA) payload into pre-allocated frame buffer
→ When recv_size >= frame_size AND dma_nb == 0 → frame complete
→ Pipeline: optional format convert → deliver to app
→ App returns frame → buffer freed for reuse
```

### Resilience Philosophy
- **Redundancy, not recovery**: ST2110-7 sends same stream on two paths; first-arriving packet wins
- **No retransmission by default**: RTCP-based retransmit is opt-in, custom format
- **Cooperative yield**: Never block, never spin-wait on NIC — save state, return, retry next iteration

### Design Decisions — Alternatives Considered

| Decision | Why This Way | Alternative Rejected |
|----------|-------------|---------------------|
| DPDK not kernel stack | Need µs-level pacing at 100Gbps | Kernel can't poll fast enough |
| Tasklets not threads | Cache locality, cooperative scheduling | Thread-per-session wastes cores |
| extbuf not memcpy TX | Zero-copy: NIC DMAs from app buffer | memcpy at 12Gbps burns CPU |
| rte_ring between builder/transmitter | Decouples timing from packet construction | Single tasklet can't meet both |
| Per-session mempool (TX) | Isolation: one session's leak can't starve others | Shared pool = cascading failures |
| Per-queue mempool (RX) | Multiple sessions share queue; pool sized to queue | Per-session = waste |
| Bitmap not seq tracking (RX) | O(1) dedup with out-of-order delivery | Last-seq useless with reordering |
| Slots not single buffer (RX) | Concurrent assembly of interleaved frames | Single buffer drops early arrivals |
| Round-robin slot eviction | Fair eviction, simple | LRU penalizes longest-waiting slot |
| Hugepages not regular pages | Fewer TLB misses at 5Gbps+ data rates | Regular pages = TLB thrashing |
| DMA threshold 1024 bytes | CPU memcpy faster below threshold | DMA setup cost dominates small copies |
| Stack mempool ops (LIFO) | Better cache reuse than FIFO ring ops | FIFO = cold cache lines |
| Spin polling not epoll/sleep | Only way to achieve <1µs pacing jitter | Any sleep mechanism adds 5-100µs |
| Hardware RL not software pacing | Sub-µs accuracy, zero CPU overhead | TSC pacing: 10-100µs jitter |

### Coding Rules
- Error paths must free in reverse allocation order
- Every `rte_malloc` must have a matching `rte_free` on the same NUMA socket
- `dbg()` / `info()` / `warn()` / `err()` for logging — never `printf`
- No C++ in library core (C99 only); tests use C++ for gtest
- All public API in `include/` — internal headers stay in `lib/src/`

---

## §2 Threading & Scheduler

### Cooperative Tasklet Model
- Tasklets are function pointers called repeatedly by a scheduler thread in a tight loop
- Tasklets **must never block** — one blocked tasklet starves all others
- Signal "I have work" (positive return) or "I'm idle" (return 0)
- Scheduler-to-tasklet: 1:many

### Two Scheduler Modes

| Mode | How | When |
|------|-----|------|
| **Lcore** (default) | DPDK lcores, CPU-pinned via `rte_eal_remote_launch()` | Deterministic latency |
| **Pthread** (`MTL_FLAG_TASKLET_THREAD`) | Regular pthreads, not pinned | Containers, non-dedicated cores |

Max schedulers: `MT_MAX_SCH_NUM = 18` (in `mt_main.h`)

### Quota System
Sessions assigned to schedulers by weight in "1080p-equivalents":
1. Search existing schedulers for capacity on right NUMA node
2. If none found, allocate new scheduler (up to 18)
3. As sessions freed, quota returned

**If adding a new session type**: define its quota correctly or schedulers get overloaded.

### Sleep/Wake Design
- Each tasklet declares `advice_sleep_us`
- Scheduler takes minimum across all tasklets
- Below 200µs (`sch_zero_sleep_threshold_us`): just yields
- Above 200µs: `pthread_cond_timedwait()` (works in both lcore and pthread modes)
- `MTL_FLAG_TASKLET_SLEEP` works in both modes

**Gotcha**: `advice_sleep_us = 0` → scheduler never sleeps → 100% CPU.

### Why Spin Polling Is Necessary

1080p60 `trs` ≈ 3.7µs between packets. Any sleep mechanism adds too much jitter:

| Mechanism | Latency | Viable? |
|-----------|---------|---------|
| `usleep()` | 50–100µs | No |
| `nanosleep()` | 50+µs | No |
| `epoll_wait()` | 5–15µs | No |
| **Spin on TSC** | **<0.1µs** | **Yes** |

Cost: 100% CPU per scheduler core. Acceptable because media machines dedicate cores.

### Builder vs Transmitter Split (TX Video Only)
Two tasklets per video session connected by `rte_ring` (`packet_ring`):
- **Builder** (`tvs_tasklet_handler`): reads frames, constructs RTP packets, enqueues to ring
- **Transmitter** (`video_trs_tasklet_handler`): dequeues, applies pacing, bursts to NIC

Ring = shock absorber. Builder can be slow; transmitter still has queued packets.

**Ring sizing**: Default 512 entries (`ST_TX_VIDEO_SESSIONS_RING_SIZE`, ≈ 1/9 of a 1080p frame's ~4500 packets). Halved if exceeds `st20_total_pkts`. Uses SP/SC (single-producer/single-consumer) mode — both tasklets run on the same lcore, so no atomics needed (~2-3ns vs ~20+ns for multi-producer).

**Bulk enqueue semantics**: `rte_ring_sp_enqueue_bulk` (all-or-nothing) not `_burst` (best-effort). The 4 packets have sequential RTP sequence numbers and pacing timestamps — they must be sent as a unit. Partial enqueue would break batch integrity.

### Cooperative tx_burst: No Retry Loop
If `rte_eth_tx_burst()` returns fewer than requested:
1. Save unsent packets to `inflight` array
2. Return to scheduler immediately (don't starve other sessions)
3. Next tasklet invocation (~0.1–1µs later): retry inflight first

### Session Migration
`MTL_FLAG_TX_VIDEO_MIGRATE`: admin thread monitors scheduler CPU. If >95% busy, moves last session to less-loaded scheduler.
- Lock ordering: **target** manager mutex first, then **source**
- Move session pointer atomically (NULL old slot, set new)

### Thread Inventory

| Thread | Purpose | Detail |
|--------|---------|--------|
| Scheduler (×18 max) | Tasklet polling | Lcore-pinned or pthread |
| TSC Calibration (×1) | Measures TSC frequency | Joins before sessions start transmitting (called lazily by `_mt_start()` and pacing init) |
| Admin (×1) | Watchdog, migration | Periodic alarm-based |
| Statistics (×1) | Stat dump | Configurable `dump_period_us` |
| CNI (×1) | ARP/PTP/DHCP/IGMP | Tasklet or thread mode (`MTL_FLAG_CNI_THREAD`) |
| Socket TX/RX (×4 each) | Kernel socket backend | Only with `kernel:` backend |
| SRSS (×1) | Shared RSS polling | Only for NICs without flow director |

---

## §3 Memory Management

### Two-World Memory Model
- **Hugepage** (DPDK): frames, mbufs, mempools, rings — via `rte_malloc` / `rte_zmalloc`
- **Heap** (libc): control structures, configs, strings — via `mt_rte_zmalloc` wrapper for tracked allocation

### NUMA Matters
All DPDK allocations take `socket_id` from `mt_socket_id(impl, port)`. Socket mismatch → 2× DMA latency. Fallback: if preferred socket has no hugepages, allocate from any socket (logs warning).

### Frame Ownership Chains

**TX**: App fills buffer → MTL attaches as extbuf → NIC DMAs → extbuf refcnt drops to 0 → buffer freed for reuse

**RX**: NIC receives → memcpy into pre-allocated frame buffer → app reads → app returns → buffer freed for reuse

### Mempool Architecture

| Scope | Pool Per | Sizing Logic | Why |
|-------|----------|-------------|-----|
| TX video | Session | `2^q-1` ≥ `nb_tx_desc + bulk` | Isolation: one session's leak can't starve others |
| TX audio/anc | Session | Same formula, smaller numbers | Same isolation |
| RX | Queue | `2^q-1` ≥ `nb_rx_desc × 1.5` | Multiple sessions share queue; pool sized to queue |

Pool sizing formula: find smallest `2^q - 1` ≥ desired count. Implemented in `mt_util.c`.

Default mempool ops: `"stack"` (LIFO) — better cache reuse than FIFO ring ops. LIFO returns the most-recently-freed mbuf (L1/L2 hot, ~3-5ns). FIFO returns the oldest freed mbuf (L3/DRAM cold, ~15-40ns). Over 260K alloc/free cycles/sec per 1080p60 stream, this saves ~2.6-9.1ms/sec. The spinlock in stack ops is uncontended (single-threaded access) — costs ~1ns.

### Zero-Copy TX Design
- Header mbufs: allocated from per-session pool, contain RTP/UDP/IP/Eth headers (~62 bytes)
- Chain mbufs: `rte_pktmbuf_attach_extbuf()` points to app's frame buffer (no copy)
  - In **IOVA VA mode** (default with `--in-memory`): chain pool `data_room = 0` — chain mbufs are pure pointer containers
  - In **IOVA PA mode** (legacy): chain pool `data_room = s->st20_pkt_len` (~1200-1260B, session-calculated) — cross-page payloads copied into mbuf
- Cross-page fallback: if payload spans hugepage boundary, memcpy into chain mbuf's data room instead of extbuf
- `rte_mbuf_ext_shared_info` callback (`tv_frame_free_cb`) decrements frame refcnt
- **Cost without zero-copy**: memcpy at 312 MB/s per 1080p60 stream, 1.2 GB/s at 4K60. A 16-stream appliance would need ~19 GB/s just for copying — most of a NUMA node's memory bandwidth
- NIC scatter-gather DMA: segment 1 = header mbuf (~62B), segment 2 = chain extbuf (~1260B). Requires `RTE_ETH_TX_OFFLOAD_MULTI_SEGS`

### RX Frame Buffers
- Allocated via `mt_rte_zmalloc_socket()` (`rte_zmalloc_socket` wrapper) — zero-initialized
- `framebuff_cnt` typically 3: one assembling, one for app, one spare
- Zero-init rationale: partial frames (packet loss) have zeroed gaps instead of garbage

### DMA Copy Engine (RX)

Decision tree for each received payload:
```text
payload_size >= 1024 (ST_RX_VIDEO_DMA_MIN_SIZE)?
  ├─ YES → DMA copy (borrow mbuf until DMA completes, inc dma_nb)
  └─ NO  → CPU memcpy (immediate, no tracking needed)
```
- DMA must not cross hugepage boundaries
- Borrowed mbufs tracked via `lender` field in `st_rx_muf_priv_data`; returned after DMA completion
- Frame completion waits for `dma_nb == 0`

### mbuf Private Data (`mt_muf_priv_data`)
**Union** (32 bytes) of TX and RX variants:

| TX fields (`st_tx_muf_priv_data`) | RX fields (`st_rx_muf_priv_data`) |
|---|---|
| `tsc_time_stamp` (8B) — pacing timestamp | `offset` — destination offset in frame |
| `ptp_time_stamp` (8B) — RTP timestamp source | `len` — payload length |
| `priv` (8B) — pointer to owning session/frame | `lender` — borrowed mbuf pointer (DMA) |
| `idx` (4B) — packet index within frame | `padding` |

Chain mbufs have `priv_size = 0` — metadata in header mbuf only.

### Why Not a Mempool for Frame Buffers?
Frame buffers are large (5.2 MB), few (3 per session), long-lived (entire session), and DMA-referenced. A mempool would waste hugepage memory on rounding/alignment/metadata. The alloc/free pattern (refcount/state machine) doesn't match pool semantics (alloc/free). Use `rte_zmalloc_socket` instead.

### RX Session-Level Frame State (Separate from Pipeline States)
Simple refcount-based lifecycle at the session layer:
- `refcnt = 0` → FREE (available for `rv_get_frame()` — linear scan for refcnt==0, atomic increment)
- `refcnt = 1` → IN USE (being filled by RX assembly, or held by app)
- `rte_atomic32_dec` → FREE (returned by app via `rv_put_frame()`)

The pipeline layer (`st20p_rx_frame_status`) adds richer states on top of this.

### Key Constants & Gotchas
- Pool names include `recovery_idx` suffix: `_HDR_0`, `_HDR_1`... (unique across recovery cycles)
- `rte_mempool_create` fails on duplicate names — recovery_idx prevents this
- ASAN: Meson option `enable_asan`, poisons freed hugepage memory
- EAL flags:
  - `--match-allocations`: Forces 1:1 allocation→hugepage mapping. Without it, DPDK may merge adjacent allocations and freeing one pool won't reclaim hugepages because they're merged with a live allocation
  - `--in-memory`: Disables shared memory files in `/var/run/dpdk/`. Avoids filesystem ops, permission issues, stale lockfiles after crash
- x86 cache line: 64 bytes (`RTE_CACHE_LINE_SIZE`)
- Default descriptors: `MT_DEV_RX_DESC = 4096/2 = 2048`, `MT_DEV_TX_DESC = 4096/8 = 512` (in `mt_dev.h`)
- RX mempool element count: `nb_rx_desc + 1024` (headroom), rounded to `2^q - 1 = 4095`
- Mempool element size cache-aligned: e.g., 1494 → 1536 (rounded to `MT_MBUF_CACHE_SIZE = 128`)

---

## §4 Concurrency & Locking

### Two-Tier Locking
- **Data plane**: `rte_spinlock_t` — never sleeps, used in tasklets
- **Control plane**: `pthread_mutex_t` — allowed to sleep, used in create/destroy

### Session Access Pattern
Three variants:
- `rx_video_session_get()` — blocking `rte_spinlock_lock`
- `rx_video_session_try_get()` — non-blocking `rte_spinlock_trylock`
- `rx_video_session_get_timeout()` — `mt_spinlock_lock_timeout`

Paired with `rx_video_session_put()` (unlock). Always get→work→put.

### Lock Ordering
- Manager mutex → session spinlock (never reverse)
- Migration: target manager mutex → source manager mutex

### Atomics Usage
- Frame refcnt (`rte_atomic32_t`) — TX extbuf lifecycle
- Session count in managers — manager needs to know when all sessions gone
- Stats counters — relaxed ordering sufficient
- RX bitmap `mt_bitmap_test_and_set()` — atomic test-and-set per packet
- RX DMA mbuf borrowing — `lender` field tracks borrowed mbufs

### Shared Queue Contention
- TX Shared Queue (TSQ): spinlock-protected, multiple sessions enqueue
- RX Shared Queue (RSQ): dispatcher thread distributes mbufs to per-session rings

### Common Race Conditions
- Create vs destroy: manager mutex prevents
- Port reset during active sessions: drain queues first
- Stats read during session teardown: get/put pattern protects

---

## §5 Pacing, Timing & Performance

### Why Pacing Exists
ST2110-21 mandates packets spread evenly across frame period. 1080p60: ~4,500 packets in ~16.7ms, ideal spacing ~3.7µs (`trs`).

### Hardware Rate Limiter (RL) — Preferred
Intel E810 Traffic Manager enforces per-flow rate limits:
- Configured with exact wire rate adjusted for blanking (`reactive` factor: active_lines/total_lines, e.g., 1080/1125)
- NIC hardware spaces packets — CPU just enqueues
- Sub-microsecond accuracy, zero CPU overhead for timing
- Max shapers: `MT_MAX_RL_ITEMS = 128` per port

### Software TSC Pacing — Fallback
When no hardware RL available:
- Read TSC → compare against `pacing.tsc_time_cursor`
- Too early → return, let scheduler poll again
- On time → transmit

### Bulk Size: Why 4
`ST_SESSION_MAX_BULK = 4` (compile-time constant in `st_header.h`). Sizes `trs_inflight[port][4]` arrays.

**RL mode**: NIC paces regardless of CPU burst size. RL transmitter sends up to 8 packets per iteration (double-call). VRX compensation is for NIC burst behavior (`max_burst_size=2048`):
```c
pacing->vrx -= 2; /* VRX compensate to rl burst */
pacing->vrx -= 2; /* leave VRX space for deviation */
```

**TSC mode**: CPU controls timing. Bulk=4 → 3 packets of VRX budget consumed:
```c
pacing->vrx -= (s->bulk - 1); /* compensate for bulk */
```

**TSC Narrow mode**: Forces `s->bulk = 1` for maximum accuracy.

| Constraint | Impact |
|-----------|--------|
| `ST_SESSION_MAX_BULK = 4` | Array sizing, compile-time |
| Ring efficiency | Amortizes ring atomics over 4 entries |
| PCIe doorbell | ~200-500ns per `tx_burst` MMIO. Bulk=4 → ~65K doorbells/sec vs 260K at bulk=1 |
| TSC VRX headroom | 3 of ~8 packets Narrow budget consumed |
| `ST20_TX_FLAG_DISABLE_BULK` | App can override to bulk=1 |

### Warm-Up Padding
Hardware RL has ramp-up delay. MTL sends padding packets (RTP padding bit set) before first frame. Default: 80% of `pkts_in_tr_offset`, capped at 128.

### PTP: The Time Reference
- MTL implements PTP slave in software (`mt_ptp.c`)
- NIC hardware timestamps PTP packets
- Every RTP packet carries PTP-derived timestamp
- Without PTP: local TSC-based pacing works but clocks drift

### Epoch Timing
Frame transmission aligned to PTP epoch boundaries. For 59.94fps: frame period ≈ 16.683ms.
- Late frame → advance to next epoch → `stat_frame_late` increments
- Fix is in the application, not MTL

### VRX (Virtual Receiver Buffer) Conformance
RX diagnostic stats:
- `vrx_min` / `vrx_max` — buffer excursion bounds
- High vrx_max → sender too bursty
- Negative vrx_min → packets arriving late

### Performance Numbers (Intuition)
- 1080p60 uncompressed ≈ 5 Gbps
- 4K60 uncompressed ≈ 12 Gbps
- One E810 100G port ≈ 8× 4K60 or 20× 1080p60 (theoretical)
- RL: 128 shapers per port
- TSC resolution ≈ 1ns, scheduler poll adds 10-100µs jitter

### Performance Debugging Mental Model

1. **NIC bottleneck?** Check `stat_tx_burst` vs `stat_tx_bytes`
2. **Scheduler overloaded?** 100% CPU, never sleeping → too many sessions
3. **Pacing broken?** RX: VRX stats. TX: `stat_epoch_mismatch`, `stat_frame_late`
4. **NUMA problem?** Socket mismatch → 2× DMA latency
5. **Ring underflow?** Transmitter sends fewer packets than expected → increase ring or reduce builder load
6. **RX losing packets?** NIC `imissed` counter → increase `nb_rx_desc`, mempool, or drain faster (continuous burst: if `rte_eth_rx_burst` returns ≥ `rx_burst_size/2`, loop immediately via `MTL_TASKLET_HAS_PENDING`)
7. **RX memcpy bottleneck?** 4K60 = 1.2GB/s memcpy per stream. Use DMA engine for payloads ≥1024 bytes

### USDT Tracepoints
Probes in `lib/src/mt_usdt_provider.d`, compiled when `MTL_HAS_USDT`. Attach with bpftrace/SystemTap.

| Provider | Key Probes | Traces |
|----------|-----------|--------|
| `sys` | `log_msg`, `tasklet_time_measure` | Logging, tasklet timing |
| `ptp` | `ptp_msg`, `ptp_result` | PTP timestamps, sync delta |
| `st20`/`st22`/`st30` | `tx_frame_next/done`, `rx_frame_available/put` | Session frame lifecycle |
| `st20p`/`st22p`/`st30p`/`st40p` | `tx_frame_get/put/drop`, `rx_frame_get/put` | Pipeline frame transitions |

Several probes are **attach-to-enable**: zero cost until tracing tool attaches.

---

## §6 Session Lifecycle & Data Flow

### Session Creation Order (TX Video)
1. Validate ops (`*_ops_check`) — reject invalid configs before allocating
2. Acquire scheduler quota — find/create scheduler with capacity
3. Attach to manager — claim slot (protected by mutex)
4. Allocate resources — frames, mempools, rings (NUMA-pinned)
5. Acquire NIC TX queue
6. Initialize pacing (RL or TSC)
7. Register tasklets (builder + transmitter)

Cheap checks first, then scarce resources (scheduler, queue), then expensive (memory).

### Session Creation Order (RX Video)
Actual sequence in `rv_attach()`:
1. `rv_init_hw()` — acquire RX queue + install flow rule + drain queue
2. `rv_init_sw()` — internally calls `rv_alloc_frames()` then `rv_init_slot()`, allocates bitmap, creates rings
3. `rv_init_mcast()` — register multicast MAC + IGMP join (2 duplicate reports)
4. `rv_init_rtcp()` — conditional, if RTCP enabled
5. `rv_init_pkt_handler()` — select packet handler based on flags/features

Tasklet is per-manager (not per-session), registered in `st_rx_video_sessions_sch_init`.

Alternative path: `rv_detector_init()` instead of `rv_init_sw()` when auto-detect mode enabled.

### RX Packet Handlers (set by `rv_init_pkt_handler`)
- `rv_handle_rtp_pkt` — standard frame mode
- `rv_handle_frame_pkt` — frame mode variant
- `rv_handle_hdr_split_pkt` — header-split mode
- `rv_handle_st22_pkt` — compressed video
- `rv_handle_detect_pkt` — auto-detection mode
- `rv_handle_detect_err` — detection error handler

### TX Destroy Order (Load-Bearing)
Dependency chain: `NIC TX descriptors → hold mbufs → hold extbuf refs → point to frame buffers`

`tv_uinit()` comment: *"must uinit hw firstly as frame use shared external buffer"*:
1. Drain ring — `mt_ring_dequeue_clean()`
2. Free ring — `rte_ring_free()`
3. Flush TX queue — `mt_txq_flush()` sends pad packets to push all session mbufs out of NIC
4. Release TX queue — `mt_txq_put()`
5. Free pad packets
6. Free inflight — `rte_pktmbuf_free_bulk(inflight)`
7. Free chain pool — `tv_mempool_free()`
8. Free header pool — `tv_mempool_free()`
9. Free frame buffers — `rte_free(frame->addr)`

**Why HW before SW (steps 1-5 before 6-9)**: After step 3, NIC holds NO references. Only then safe to free pools/frames.

### RX Destroy Order
Actual sequence in `rv_uinit()`:
1. `rv_stop_pcap_dump()`
2. `rv_uinit_mcast()` — remove multicast MAC + IGMP leave
3. `rv_uinit_rtcp()`
4. `rv_uinit_sw()` — internally calls `rv_free_frames()`, frees bitmap, destroys rings
5. `rv_uinit_hw()` — destroy flow rule + release RX queue

**Key asymmetry with TX**: NO queue drain/flush on RX destroy. RX mbufs hold copies (memcpy'd) — not references to frame buffers via extbuf.

### Frame State Machines

**TX Pipeline** (`st20p_tx_frame_status`):
```text
FREE → IN_USER (app fills) → READY → IN_CONVERTING (optional) → CONVERTED → IN_TRANSMITTING → FREE
```
If no conversion needed: READY → IN_TRANSMITTING directly.
Late frame handling: if IN_USER when epoch arrives → skip. If older READY exists alongside newer READY → drop older (newest-first).

**RX Pipeline** (`st20p_rx_frame_status`):
```text
FREE → READY (transport delivers) → IN_CONVERTING (optional) → CONVERTED → IN_USER (app reads) → FREE
```

**Session-Level TX** (`st21_tx_frame_status`):
```text
WAIT_FRAME ──(get_next_frame)──► SENDING_PKTS ──(all pkts sent)──► WAIT_FRAME
```
Note: `ST21_TX_STAT_WAIT_PKTS` exists in enum but is dead code (never assigned/checked).

### RX Packet Assembly: Slot Design

**Why slots**: Packets from different frames (different RTP timestamps) can arrive interleaved. Slots allow concurrent assembly.

- `slot_max = 1` for single-port
- `slot_max = 2` for redundant (ST2110-7) or RTCP-enabled sessions

### Slot Assignment (`rv_slot_by_tmstamp`)
```text
1. SCAN: Check active slots — if tmstamp matches, return it
2. STALE: If T < active slot's tmstamp → drop packet (old frame)
3. DMA GUARD: If slot has pending DMA (dma_nb > 0) → don't evict
4. EVICT (round-robin): Pick next free/oldest slot. If occupied → notify partial frame (CORRUPTED status)
5. CLAIM: Acquire frame buffer, set tmstamp, clear bitmap, reset recv_size
6. RETURN: Caller writes payload into slot's frame
```

### RTP Header Layout (ST2110-20)
```text
Ethernet (14B) + IPv4 (20B) + UDP (8B) + RTP (12B) + SRD (8B) = 62 bytes
```
- `seq_number` (16-bit) + `seq_number_ext` (16-bit) → 32-bit sequence: `(ext << 16) | seq`
- `tmstamp` (32-bit) — same for all packets in one frame (frame ID)
- SRD: `srd_length`, `srd_row_number`, `srd_offset`, continuation bit for multi-line packets

### Frame Offset Calculation
```text
offset = srd_row_number × bytes_per_line + (srd_offset / pixel_coverage) × pg_size
```
Example (1080p YCbCr-422 10-bit): `bytes_per_line = 1920/2 × 5 = 4800`

### Bitmap Duplicate Detection
```text
bitmap_size = frame_size / 800 / 8    (normal path: st_rx_video_session.c:3243)
           = frame_size / 1000 / 8   (detect path: st_rx_video_session.c:2697)
Floor: max(bitmap_size, height * 2 / 8)
→ 1080p: 5,184,000 / 800 / 8 = 810 bytes

pkt_idx = (seq_id - base_seq_id) & 0xFFFFFFFF  // handles 32-bit wraparound
base_seq_id estimated from first packet: seq_id - (offset / payload_length)
mt_bitmap_test_and_set(bitmap, pkt_idx) → atomic, skip if already set
```

### Frame Completion
1. `recv_size >= frame_size`
2. `dma_nb == 0` (all DMA copies finished)
3. No pending mbuf borrows

Complete → `rv_slot_full_frame()` → `ST_FRAME_STATUS_COMPLETE`. Incomplete/evicted → `ST_FRAME_STATUS_CORRUPTED`.

### Inflight Pattern (Cooperative Non-Blocking Retry)
- **Builder inflight**: ring full → save packets to `s->inflight[port][]` → return → retry next iteration
- **Transmitter inflight**: `tx_burst` sends fewer → save unsent to `s->trs_inflight[]` → retry next iteration
- Never discard (data loss + refcount leak), never block (starves other sessions)

### Recovery via `recovery_idx`
When TX queue hangs (`st20_tx_queue_fatal_error`):
1. Drain rings → release old queue → `recovery_idx++`
2. Acquire new queue → `tv_mempool_free()` → `tv_mempool_init()` with new name suffix
3. Reset state → notify app (`ST_EVENT_RECOVERY_ERROR`)
4. If recovery fails → `s->active = false` → `ST_EVENT_FATAL_ERROR`

### RTCP Retransmission
Custom format (RTCP type 204, name "IMTL"):
- RX detects bitmap gaps → NACK with (start_seq, follow_count) ranges
- TX maintains circular buffer of recent packets → deep-copies for retransmit (original mbufs may be freed)
- Standard RTCP NACKs (RFC 4585) too inefficient for thousands of packets per frame

### Redundancy: Active-Active Merging

**Video RX** — Packet-level bitmap merge:
- Two ports with independent queues/flow rules
- Same bitmap for both paths — first write wins (atomic test-and-set)
- Even 50% loss on one path recoverable if other path has missing packets
- Extra slot: `slot_max = 2`

**Audio/Ancillary RX** — Timestamp-based dedup:
- Reject packets where RTP timestamp ≤ current (`mt_seq32_greater`)
- Ancillary also checks `seq_id` via `mt_seq16_greater`
- Safety valve: after `ST_SESSION_REDUNDANT_ERROR_THRESHOLD = 20` consecutive rejects, bypass filter
- "redundant error threshold reached" in logs → investigate source

**TX Redundancy**: Clone packet with different headers for second port. Clone shares same chain payload mbuf (refcnt incremented). Frame not free until BOTH ports complete DMA.

### IGMP Multicast Group Management
- Join: 2 duplicate IGMPv3 Membership Reports (reliability — IGMP has no ACK)
- Keepalive: 10-second periodic re-join (`IGMP_JOIN_GROUP_PERIOD_S = 10`)
- Leave: IGMPv3 Leave on session destroy
- Well-known group `224.0.0.1` always joined (accept IGMP Query from routers)
- Switch without IGMP snooping → multicast floods all ports (doesn't break MTL, wastes bandwidth)

### Audio/Ancillary/Fast-Metadata Sessions
Same lifecycle pattern as video, simplified:
- No per-packet pacing (frame-level)
- No assembly complexity (1-8 packets per frame)
- Low scheduler quota
- Shared queue friendly
- ST2110-41: payload type 115, header 58 bytes, API in `st41_api.h`

### Manager Pattern
Sessions organized into managers (`st_tx_video_sessions_mgr`), one per session type per scheduler:
- Fixed-size session array (indexed by `idx`)
- Manager mutex protects attach/detach
- Tasklet handler iterates all sessions in local array

Per-scheduler (not global) → no filtering needed in tasklet hot path.

### Global Init Ordering
`mtl_init()`: DMA → queues → ARP/mcast → CNI → admin → plugins → DHCP → PTP → TSC calibration thread

`mtl_start()` blocks on TSC calibration via `mt_wait_tsc_stable()` before starting ports.

Sessions can be created between init and start (allocate queues/mempools/flow rules against stopped port).

`MTL_FLAG_DEV_AUTO_START_STOP`: init calls start internally, stop becomes no-op.

### Plugin Contract
3-phase: Registration → Session binding → Frame exchange

Registration: `dlopen` → resolve 3 symbols (`st_plugin_get_meta`, `st_plugin_create`, `st_plugin_free`) → version/magic check

Binding: search by capability bitmask (`input_fmt_caps`, `output_fmt_caps`). First match wins.

- Plugins own their threads (not called from tasklets)
- Max 8 plugins per MTL instance (`ST_MAX_DL_PLUGINS = 8`)
- Loaded during `mtl_init()` from JSON config

### Validation Checklist (`*_ops_check()`)
- `num_port`: 1-2, `payload_type`: 0-127, `framebuff_cnt`: 2-8 (video)
- IP: not all zeros, multicast = 224.x-239.x, redundant ports must differ
- Required callbacks checked based on `type` field

---

## §7 DPDK Usage Patterns

### Abstraction Layer
Queue management abstracted via `mt_queue.c` (in `lib/src/datapath/`). Functions: `mt_txq_burst()`, `mt_txq_flush()`, etc.

### Port Initialization
Static functions in `mt_dev.c`:
- `dev_config_port()` — configure device (RSS, promiscuous, multi-seg TX, checksum offload)
- `dev_start_port()` — start device, then `rte_eth_stats_reset()` (clean baseline, prevents stale counters from probe/startup contaminating session stats)
- TX offloads: `MULTI_SEGS` (scatter-gather for header+chain) + `IPV4_CKSUM` (HW checksum). RX offloads: `0` (no scatter-gather, `rx_nseg=0`)
- `rte_eth_dev_set_ptypes()`: tells NIC to only classify TIMESYNC/ARP/VLAN/QINQ/ICMP/IPv4/UDP/FRAG — reduces per-packet classification overhead. Only applied when driver reports ≥5 supported ptypes
- No promiscuous mode by default — hardware flow rules steer traffic. `MTL_FLAG_NIC_RX_PROMISCUOUS` enables it for debugging

### TX Callback Safety Net
`dev_tx_pkt_check()` registered via `rte_eth_add_tx_callback()` on every TX queue. Validates each packet:
- `pkt_len > 16` (catches zero-length mbufs)
- `pkt_len ≤ MTL_MTU_MAX_BYTES` (catches oversized)
- `nb_segs ≤ 2` (catches corrupted scatter-gather chains)

Failing packets are replaced with pad mbuf (not dropped — preserves burst count). Disabled with `MTL_FLAG_TX_NO_BURST_CHK`. Cost: ~1ns per packet (two integer comparisons).

### Flow Rules
Created in `mt_flow.c`. Two patterns:
- **Multicast**: match destination IP only (`mt_is_multicast_ip()` check)
- **Unicast**: match both source and destination IP

No explicit ASM/SSM distinction in flow code — just multicast vs unicast.

### Multicast MAC Registration
IP-to-MAC formula in `mt_mcast_ip_to_mac()` (`mt_mcast.h`): `01:00:5E` + `ip[1]&0x7f` + `ip[2]` + `ip[3]`

Two driver paths:
- `rte_eth_dev_set_mc_addr_list()` (preferred, when using kernel control)
- `rte_eth_dev_mac_addr_add()` (fallback)

Well-known group `224.0.0.1` always joined for IGMP query reception (unless kernel-ctl or user-no-multicast flags).

### Queue Architecture

| Strategy | When Used | How |
|----------|----------|-----|
| **Dedicated queue** | Default for video | One TX/RX queue per session, best isolation |
| **Flow director** | NIC supports flow rules | Multiple sessions share port, flow rules steer to per-session queues |
| **Shared RX Queue (RSQ)** | Flow director unavailable | Dispatcher distributes mbufs to per-session rings |
| **Shared TX Queue (TSQ)** | Low-bandwidth sessions | Spinlock-protected, multiple sessions enqueue |
| **RSS** | Fallback | Hash-based distribution, no per-flow control |

### TX Pad Flush
`mt_txq_flush()` → `mt_dpdk_flush_tx_queue()`: sends `mt_if_nb_tx_burst(impl, port) × 2` pad packets (for DPDK ports, `nb_tx_burst = nb_tx_desc`).

Pad destination MAC: `01:80:C2:00:00:01` (IEEE slow-protocol, switches won't forward).

Why pads instead of `tx_done_cleanup`: E810 ice driver does NOT implement `rte_eth_tx_done_cleanup` (returns `-ENOTSUP`).
Even on NICs that support it, the API only frees already-completed descriptors — if the NIC hasn't finished DMA-reading an mbuf, cleanup won't touch it.
Pads actively force the ring to cycle, guaranteeing completion regardless of NIC state.
(Note: MTL does call `rte_eth_tx_done_cleanup` in shared TX queue path as best-effort, but pad flush is the authoritative cleanup.)

### RX Burst Pattern
Default burst: 128 (runtime assignment: `s->rx_burst_size = 128`). RX packets fit in a single mbuf because `data_room = 1664` > max packet size (1494 bytes) — no multi-segment reassembly needed.

Continuous burst: if `rte_eth_rx_burst` returns ≥ `rx_burst_size / 2` (≥64), track as continuous burst. Tasklet returns `MTL_TASKLET_HAS_PENDING` when any packets received → scheduler re-polls immediately.

### Header-Split RX
Intel E810 with `ST20_RX_FLAG_HDR_SPLIT`: NIC writes payload directly into frame buffer, bypassing CPU memcpy. Requires DPDK ice driver patches.

### mbuf Lifecycle

**TX**: alloc header → fill headers → chain extbuf → enqueue ring → tx_burst → NIC DMA → mbuf freed back to pool (extbuf refcnt dec)

**RX**: NIC writes to mbuf data room → rx_burst returns mbuf → parse + memcpy/DMA payload to frame → `rte_pktmbuf_free()` returns to pool

### Traffic Manager (Rate Limiter)
Hierarchy: port → shapers (max 128, `MT_MAX_RL_ITEMS`). Each shaper = one session's rate limit.

### Multi-Process
MTL supports `--proc-type=secondary` for read-only stats access. Full multi-process TX/RX not supported.

### Backend Support

| Backend | Prefix | When To Use |
|---------|--------|-------------|
| DPDK PMD | (PCI BDF) | Production — full speed |
| AF_XDP | `native_af_xdp:` or `dpdk_af_xdp:` | When kernel driver can't unbind |
| AF_PACKET | `dpdk_af_packet:` | Testing only — very slow |
| Kernel socket | `kernel:` | Development/testing — no DPDK needed |

### Debugging
- `rte_eth_stats_get()` → check `imissed`, `ierrors`, `oerrors`
- `rte_eth_xstats_get()` → detailed per-queue stats
- `RTE_LOG_LEVEL` controls DPDK log verbosity

---

## §8 Testing

### Test Layers

| Layer | Framework | Language | What | Speed |
|-------|-----------|----------|------|-------|
| Integration (gtest) | Google Test | C++ | Internal APIs via loopback | Minutes |
| Fuzz | LLVM libFuzzer | C | RX packet parsers | Hours |
| Validation (pytest) | pytest | Python | E2E via RxTxApp/FFmpeg/GStreamer | 10s of minutes |
| Shell scripts | bash | Shell | JSON-driven loopback scenarios | Minutes each |

### Integration Tests (`tests/integration_tests/`)

Three binaries:
- `KahawaiTest` — main ST2110 tests (links MTL)
- `KahawaiUfdTest` — UDP file descriptor tests (links MTL)
- `KahawaiUplTest` — LD_PRELOAD tests (**no** MTL link, standard POSIX sockets)

Architecture: single global `mtl_init()` for entire run (`st_test_ctx()`). Sessions ephemeral per test.

Key CLI flags: `--p_port`, `--r_port`, `--log_level`, `--level` (all/mandatory), `--pacing_way`, `--rss_mode`, `--tsc`

#### Test Suites

| Suite | What |
|-------|------|
| `Main` | Init/start/stop, bandwidth calc, format utils |
| `Misc` | Version, memcpy, PTP, NUMA |
| `St20_tx`/`St20_rx` | Session-level video |
| `St20p` | Pipeline video, plugins, digest, RTCP |
| `St22_tx`/`St22_rx` | Compressed video session |
| `St22p` | Compressed pipeline, encode/decode |
| `St30_tx`/`St30_rx` | Audio session |
| `St30p` | Audio pipeline |
| `St40_tx`/`St40_rx` | Ancillary |
| `Sch` | Scheduler create/tasklet registration |
| `Dma` | DMA copy/fill sweeps |
| `Cvt` | Pixel format conversion (4349 lines!) |

#### Verification Patterns
- **Data integrity (digest)**: TX SHA256 → RX SHA256 → compare
- **FPS accuracy**: Count frames over timed window
- **Create/free stress**: `create_free_test(base, step, repeat)`
- **Expected failure**: Invalid params → expect NULL
- **Level filtering**: `MANDATORY` always runs; `ALL` needs `--level all`

#### Noctx Tests (`tests/integration_tests/noctx/`)
Tests needing isolated `mtl_init`/`mtl_uninit` per test. Run via `noctx/run.sh` (serial, 10s cooldown). Requires 4 ports.

### Fuzz Tests (`tests/fuzz/`)

| Target | File | What |
|--------|------|------|
| ST2110-20 video RX | `st20/st20_rx_frame_fuzz.c` | RFC4175 video parsing |
| ST2110-22 compressed RX | `st22/st22_rx_frame_fuzz.c` | RFC9134 compressed parsing |
| ST2110-30 audio RX | `st30/st30_rx_frame_fuzz.c` | Audio parsing |
| ST2110-40 ancillary RX | `st40/st40_rx_rtp_fuzz.c` | Ancillary parsing |
| ST40 helpers | `st40/st40_ancillary_helpers_fuzz.c` | UDW, checksum, parity |

Architecture: minimal DPDK EAL (no hugepages, no PCI) → full session context reset per input → call internal handler.

**Limitations**: single-packet fuzzing only (no multi-packet sequence bugs), no TX/control/PTP coverage.

```bash
meson setup build_fuzz -Denable_fuzzing=true -Denable_asan=true
ninja -C build_fuzz
./build_fuzz/tests/fuzz/st20_rx_frame_fuzz corpus/st20
```

### Validation Tests (`tests/validation/`)
E2E framework launching real MTL apps over SSH. Tests do NOT call MTL C API.

Config: `topology_config.yaml` (hosts, PCI BDFs) + `test_config.yaml` (session_id, paths)

Setup: `.github/scripts/setup_validation.sh` stage functions must run in the parent shell; `stage_ssh` exports `SSH_KEY` for `stage_configs`.

Setup gotcha: `pkg-config --exists libdpdk` is not enough; warm DPDK setup must also refresh/verify `ldconfig` or RxTxApp can fail with exit 127 and missing `librte_*.so.26`.

Shell gotcha: in `setup_validation.sh` (`set -o pipefail`), avoid `producer | grep -q pattern` readiness probes; early `grep -q` exit can SIGPIPE the producer and make a true match look false. Capture output once or use non-early-exit matching.

Validation methods: log parsing, FPS check, MD5 per-frame integrity, EBU LIST compliance (optional)

Markers: `@pytest.mark.smoke`, `@pytest.mark.nightly`, `@pytest.mark.dual`, `@pytest.mark.ptp`

Root required. Depends on `mfd-*` Intel-internal packages for SSH/NIC automation.

### RxTxApp (`tests/tools/RxTxApp/`)
Universal JSON-driven test vehicle. Max sessions: 180 video, 1024 audio, 180 ancillary, 180 fast-metadata (each direction).

JSON config directories: `loop_json/`, `audio_json/`, `native_af_xdp_json/`, `kernel_socket_json/`, `rss_json/`, `redundant_json/`

### Key Testing Gotchas
- DPDK can't reinit within a process → noctx pattern for isolated tests
- `cvt_test.cpp` is 4349 lines — add round-trip tests when adding new pixel formats
- Fuzz harnesses are single-packet — multi-packet protocol bugs uncovered
- `KahawaiUplTest` uses standard POSIX sockets (validates LD_PRELOAD interception)
- Test level: CI runs `mandatory`; `all` for thorough local validation
