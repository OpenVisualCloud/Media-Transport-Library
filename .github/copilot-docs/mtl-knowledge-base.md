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
- ST2110-41 (fast metadata), as an opt-in mode of the ST2110-40 sessions

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
| `st_` / `st20_` / `st22_` / `st30_` / `st40_` | Media session APIs |
| `st20p_` / `st22p_` / `st30p_` / `st40p_` | Pipeline APIs |
| `tv_` | TX video session internals |
| `rv_` | RX video session internals |
| `tx_audio_session_` | TX audio internals |
| `rx_audio_session_` | RX audio internals |
| `tx_ancillary_session_` | TX ancillary internals |
| `rx_ancillary_session_` | RX ancillary internals |

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
- No C++ in library core (C99 plus C11 `<stdatomic.h>` atomics); tests use C++ for gtest
- All public API in `include/` — internal headers stay in `lib/src/`

---

## §2 Threading & Scheduler

### Cooperative Tasklet Model
- Tasklets are function pointers called repeatedly by a scheduler thread in a tight loop
- Tasklets **must never block** — one blocked tasklet starves all others
- Deliberate exception: `rv_dma_flush()` busy-polls at most `ST_RX_VIDEO_DMA_FLUSH_TIMEOUT_NS` per newer frame per DMA session, only while the previous frame's copies are pending, since the new frame cannot take a slot before they finish. Do not copy this pattern without such a bound
- Signal "I have work" (positive return) or "I'm idle" (return 0)
- Scheduler-to-tasklet: 1:many

### Two Scheduler Modes

| Mode | How | When |
|------|-----|------|
| **Lcore** (default) | DPDK lcores, CPU-pinned via `rte_eal_remote_launch()` | Deterministic latency |
| **Pthread** (`MTL_FLAG_TASKLET_THREAD`) | Regular pthreads, not pinned | Containers, non-dedicated cores |

Max schedulers: `MT_MAX_SCH_NUM = 18` (in `mt_main.h`)

Both modes enter `sch_tasklet_func()`, which switches a thread still on `MPOL_DEFAULT` to `MPOL_LOCAL` until the scheduler stops; any other policy is kept:
- Under `kernel.numa_balancing=1` the `task_numa_work` scan runs as task work on busy threads; without `MPOL_F_MOF` it rewrites no PTEs there and nothing migrates on fault, but the VMA walk still runs
- Mitigation only: scans on other default-policy threads still mark shared PTEs, so scheduler threads still take hinting faults; `kernel.numa_balancing=0` is the fix
- Pthread-mode schedulers inherit the policy of the thread calling `sch_start()`, usually `mtl_init()`'s `numa_bind()` `MPOL_BIND` on multi-node hosts, and keep it; plain `MPOL_BIND` has no `MPOL_F_MOF` either; EAL lcores predate that bind
- Other busy lib threads (`rv_pkt_lcore_func`, shared RSS, socket datapath) are not opted out

### Pthread-Mode Tasklets Are Unregistered Non-EAL Threads
- `MTL_FLAG_TASKLET_THREAD` schedulers come from a bare `pthread_create()` (`mt_sch.c`); `lib/` contains zero `rte_thread_register()` calls
- So `rte_lcore_id()` returns `LCORE_ID_ANY`, and each DPDK per-lcore mechanism reacts differently: `RTE_LCORE_VAR` pointers are invalid, `rte_random` falls back to one shared "unregistered" state, while `RTE_PER_LCORE` (`__thread`, e.g. `rte_errno`) still gives every thread its own copy
- For `rte_rand()` / `rte_rand_max()` / `rte_drand()` that instance is documented MT-unsafe when multiple unregistered non-EAL threads call in parallel (`rte_random.h`)
- `rte_thread_register()` is not the fix — `rte_lcore.h` warns it breaks the multi-process feature

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

### Copy Primitive
Every copy in `lib/` goes through `mt_memcpy()` (`mt_util.h`, `static inline`), never `rte_memcpy` and never a bare `memcpy` in new code. Its one line selects the backend for the whole library, so a change of copy primitive is a one-line change with no call-site churn.
Today that backend is libc `memcpy`: glibc reads the CPU at load time and picks an AVX-512, AVX2 or ERMS path, `rte_memcpy` is fixed to the path DPDK was built for, and `rte_memcpy` measured slower on the RX frame write path. `mtl_memcpy()` is the public API over the same call.
The one exception is that RX frame write path: `rv_frame_memcpy()` (`st_rx_video_session.c`) streams whole cache lines with non-temporal stores, and copies the misaligned head, the sub-line tail and short payloads through `mt_memcpy()`.
A session with a pkt lcore keeps `mt_memcpy()` for the whole copy: two threads write its frame and either may hand it over, and a fence orders only the stores of its own core. A session that migrates mid-frame needs none: the x86 unlock of its session spinlock, released after every tasklet poll, is a locked `xchg` and drains the old core's streaming stores.

### NUMA Matters
All DPDK allocations take `socket_id` from `mt_socket_id(impl, port)`. Socket mismatch → 2× DMA latency. Fallback: if preferred socket has no hugepages, allocate from any socket (logs warning).

### Frame Ownership Chains

**TX**: App fills buffer → MTL attaches as extbuf → NIC DMAs → extbuf refcnt drops to 0 → buffer freed for reuse

**RX**: NIC receives → memcpy into pre-allocated frame buffer → app reads → app returns → buffer freed for reuse

### Mempool Architecture

| Scope | Pool Per | Sizing Logic | Why |
|-------|----------|-------------|-----|
| TX video | Session | `nb_tx_desc` + builder ring + RTCP/RTP rings; deep port: builder ring + bulks + RTCP/RTP rings + ring share (`tv_mempool_n()`) | Isolation: one session's leak can't starve others |
| TX audio/anc | Session | `nb_tx_desc` + manager ring (audio 1024, anc 512); deep port: audio transmit ring threshold + 1 + ring share, anc 1 + ring share | Same isolation |
| RX | Queue | `2^q-1` ≥ `nb_rx_desc × 1.5` | Multiple sessions share queue; pool sized to queue |

Pool sizing formula: the smallest `2^q - 1` above the count, so a count of exactly `2^q - 1` doubles (2047 → 4095). Implemented in `mt_util.c`.

On a port that is not deep (Key Constants: Default descriptors) every TX pool is main's: a descriptor each and the ring, the chain pool the primary port's, a padded frame's copy pool that over `st20_total_pkts` / extra pkts. At 512 1080p video, 1 ms audio and anc get 2047.
The RTCP ring counts as `ops.rtcp.buffer_size`, which `tv_init_rtcp()` sets to `ST_TX_VIDEO_RTCP_RING_SIZE` where the app left it 0, so a recovery counts it.

A deep port's TX session pools cover what the session holds while its queue drains, plus the ring share `mt_if_tx_pool_ring_share()`: what its queues can keep, ⌈`nb_tx_desc` / descriptors a mbuf⌉ each (2 chained, 1 copy, RL audio 3 pads more on its 2 queues), and 192 mbufs another lcore's cache keeps (DPDK flushes past 1.5 × `MT_MBUF_CACHE_SIZE`).
- So a stalled queue refuses a burst before the pool runs dry; with every mbuf in the ring nothing would burst and the hang would go unseen. `St20TxPoolStallTest`, `St30TxPoolStallTest` and `St40TxPoolStallTest` run the production tasklets into a hung ring with pools of exactly that count; one mbuf less fails the alloc first (but on RL audio, whose warm-up pads keep descriptors too).
- At 512 they run main's counts, which keep slack, but a copy session's: main's count is short of what it holds, which only the `2^q - 1` rounding covers (a known bug, skipped).
- Session part: video the builder ring (512) and bulk, the transmitter's bulk (2 on RL), the RTCP retransmit and RTP rings; audio the transmit ring up to `trans_ring_thresh` (10 ms of pkts) and the pkt its transmit stage waits on; anc the pkt a refused burst leaves.
- The shared manager ring (audio 1024, anc 512) fills only behind a refused burst while its transmitter keeps up, so the allocs fail after it.
- The slack is one other lcore's cache: a deep port's queues are dedicated. A session that can migrate (`MTL_FLAG_TX_VIDEO_MIGRATE`) leaves a second cache behind: its count is the larger of main's and the deep count plus a second slack.
- A padded frame (`linesize` past the line's bytes) copies its line-crossing pkts from a pool of its own (`tv_copy_mempool_n()`): one a line the held pkts span and one more a frame they touch, plus its own slack (1023 at 2048; `CopiesCoverEveryRunOfHeldPkts`).
- A redundant pkt shares the chain mbuf: with a deep port the chain pool is sized for the port that keeps more.
- At 2048 descriptors 1080p video, 1 ms audio and anc get 2047 (`St*TxPoolSizeTest`), 1080p with RTCP 4095.

Default mempool ops: `"stack"` (LIFO) — better cache reuse than FIFO ring ops. LIFO returns the most-recently-freed mbuf (L1/L2 hot, ~3-5ns). FIFO returns the oldest freed mbuf (L3/DRAM cold, ~15-40ns). Over 260K alloc/free cycles/sec per 1080p60 stream, this saves ~2.6-9.1ms/sec. The spinlock in stack ops is uncontended (single-threaded access) — costs ~1ns.

### Zero-Copy TX Design
- Header mbufs: allocated from per-session pool, contain RTP/UDP/IP/Eth headers (~62 bytes)
- Chain mbufs: `rte_pktmbuf_attach_extbuf()` points to app's frame buffer (no copy)
  - In **IOVA VA mode** (default with `--in-memory`): chain pool `data_room = 0` — chain mbufs are pure pointer containers
  - In **IOVA PA mode** (legacy): chain pool `data_room = s->st20_pkt_len` (~1200-1260B, session-calculated) — cross-page payloads copied into mbuf
- Cross-page fallback: if payload spans hugepage boundary, memcpy into chain mbuf's data room instead of extbuf
- `rte_mbuf_ext_shared_info` callback (`tv_frame_free_cb`) copies the frame's meta for the callback, clears an ext frame's addr/iova (`st20_tx_get_framebuffer()` still returns the buffer during the callback), decrements frame refcnt, then calls `notify_frame_done` — so the app (or st20p) may re-arm and re-queue the frame from inside the callback
- **Cost without zero-copy**: memcpy at 312 MB/s per 1080p60 stream, 1.2 GB/s at 4K60. A 16-stream appliance would need ~19 GB/s just for copying — most of a NUMA node's memory bandwidth
- NIC scatter-gather DMA: segment 1 = header mbuf (~62B), segment 2 = chain extbuf (~1260B). Requires `RTE_ETH_TX_OFFLOAD_MULTI_SEGS`

### RX Frame Buffers
- Allocated via `mt_rte_zmalloc_socket()` (`rte_zmalloc_socket` wrapper) — zero-initialized
- `framebuff_cnt` typically 3: one assembling, one for app, one spare
- Zero-init covers a buffer's **first use only** — buffers are recycled, so a gap left as-is reads as the frame the buffer carried before

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
- Default descriptors (`mt_dev.h`): RX `MT_DEV_RX_DESC` 2048; TX per port: `MT_DEV_TX_DESC_DEEP` 2048 on a deep port, else `MT_DEV_TX_DESC` 512 (`MtDevTxDescTest`). An AUTO port that falls back to TSC keeps 2048, safe as it reclaims.
  - Deep (`inf->tx_deep`, `dev_tx_deep()` in `dev_config_port()`): an RL, TSN or AUTO-with-TM iavf/ice port whose cleanup frees (`MT_DEV_TX_CLEANUP_FREES`), without a shared TX queue or a user `nb_tx_desc`.
    Only a deep port reclaims, flushes bounded, skips the done cleanup, gives the port pad a ref of its own and sizes its pools from what its queue keeps; every other port keeps main's behaviour there.
  - A zero-copy (chained) video pkt takes 2 TX descriptors, so the deep TX queue holds 1024 pkts, ~3.8 ms at 1080p59.94: one lost scheduler slice (EEVDF ~3 ms). 512 holds 0.95 ms, which a shared CPU overran (`St20TxQueueStallTest`); a port that frees lazily keeps 512, as a deeper ring keeps more sent mbufs.
  - `nb_tx_desc` also sizes, so 512 → 2048 grows each on a deep port:
    - Pools: every TX session pool (video, audio, anc, fmd: its ring share, see Mempool Architecture), the per-port sys
      TX pool (`nb_tx_desc` + 1024), the shared-queue pool, and
      with `MTL_FLAG_TX_MONO_POOL` main's `nb_tx_q` × 2 × `nb_tx_desc` more mbufs per port,
      and on a deep port a cache slack for each of `MT_MAX_SCH_NUM` schedulers, as its queues
      free into the bursting scheduler's cache (`dev_tx_sys_pool_elements()`: 131071 at 17
      queues of 2048; 32767 at 512); RTCP rings, RTP rings, audio and anc sessions on top of
      that are not counted, as on main.
    - RTCP: on a deep port the retransmit ring is at least `nb_tx_desc` (`tv_rtcp_buffer_size()`), and the TX video pools count it.
      Its copy pool (`rtcp_tx_pool_n()`) holds what its queue keeps, a descriptor a copy, the 32 copies a retransmit loop makes before it bursts them, and the cache slack: 4095 at 2048, as many retransmits from a whole-ring NACK as a pool a ring larger (`RtcpCopyPoolParityTest`).
      - A PMD that frees an rs batch of 32 before the loop copies its next bulk leaves that bulk spare: one copy under `nb_tx_desc` + the slack runs dry. Off deep ports the pool is main's retransmit ring + `nb_tx_desc`.
    - Zero copy: a frame is free once the PMD frees the last mbuf pointing into it
      (`tv_frame_free_cb()` → `notify_frame_done`).
      - PMD: the iavf and ice scalar xmit (`ci_xmit_pkts()`, which MULTI_SEGS forces) cleans the
        oldest done rs batch (`tx_rs_thresh`, 32) a burst while fewer than `tx_free_thresh` (32)
        descriptors are free: a TX ring later, 1024 pkts after the frame left at 2048. Only since
        DPDK 26.07 does the cleanup free the mbufs; before, they wait for their sw_ring entry's reuse.
      - 26.07+ (`MT_DEV_TX_CLEANUP_FREES`): on a deep port `dev_start_port()` sets
        `tx_free_thresh` to `nb_tx_desc` − 2 × `tx_rs_thresh`, so a burst, a burst of none too
        (`mt_txq_reclaim()`), frees a batch while 2 are in use, and records
        `mt_if_tx_reclaim_rs_thresh()`. Every other port keeps the PMD defaults (`MtDevTxReclaimTest`).
      - Session (`tv_tx_reclaims()` → `s->reclaim.enabled`): frames of ≥ rs pkts on every
        port (2 × rs − 1 descriptors after a frame's last pkt free its batch, 2 a pkt),
        dedicated queues, no RTCP. `tv_frames_out()` asks the app for a frame only once every
        frame but the last one built is back, reclaiming every `ST_TX_VIDEO_RECLAIM_NS` while
        it waits (`tv_reclaim_poll()`, `STI_FRAME_RECLAIM_WAIT`): any 2 buffers keep zero copy. Before 26.07 it could wait for good.
      - Hang: a hung queue holds the frames too, and with nothing to send the transmitter sees no
        burst fail. A wait past `tx_hang_detect_time_thresh` (at least 2 frame times) flags the
        recovery of every leg idle as long (`tv_frames_out_hang()`); a reclaim gap past half of
        it restarts the wait, as the scheduler stalled, not the queue. The wait counts from the
        next frame's warm-up start at the earliest (`tv_next_frame_open_tsc()`:
        `tsc_time_frame_start` − `warm_pkts` × `trs`), as the waited frame's last batches leave
        only behind it, so a USER_PACING gap is no hang; a target past 1 s, which the
        transmitters send at once, counts from the wait. No per-leg signal tells a
        hung R leg from a healthy P, so both recover, and P's recovery first flushes R (TX Pad
        Flush): to the flush deadline in the tasklet, 2 ms + 2048 × 1538 B at the queue's rate,
        ~60 ms at 480p59.94 (54.4 MB/s), or 2 ms and a batch's drain, ~3 ms there, if the full
        ring refuses the mark. Where R did not drain (-EBUSY), or the flush had no sys TX mbuf
        for its mark (-ENOMEM), R's queue may still hold the frames and pool mbufs, so the
        session ends unrecoverable (`stat_unrecoverable_error`, `ST_EVENT_FATAL_ERROR`) before
        it hands a frame back or replaces a pool (`St20TxQueueRecoveryFlushFailTest`).
    - `rte_eth_tx_done_cleanup()`: in DPDK 26.07 iavf, ice and i40e walk the `nb_tx_free` entries
      from `tx_tail` and each batch their own cleanup frees, freeing again the mbufs
      `ci_tx_xmit_cleanup()` freed and left in the sw_ring: up to the free part of the ring, ~1984
      at `tx_free_thresh` = 2048 − 64. On a deep port `mt_dev_tx_done_cleanup()` never calls it.
      It runs only on TX hang recovery (`mt_txq_done_cleanup()`): video
      `st20_tx_queue_fatal_error()`, audio `st_audio_queue_fatal_error()`. There a deep port
      flushes the failed queue instead with the bounded flush alone (TX Pad Flush), so its done
      mbufs are back before the session pools go; without a sys TX mbuf for its mark it returns
      -ENOMEM rather than run the lazy loop, 2 × `nb_tx_desc` timeouts of 1-2 ms each in the
      tasklet on a hung ring. A deep port with a ring under 3 batches has no bounded flush and
      leaves the done mbufs, ≤ 63, to the queue. While it holds them `tv_mempool_free()` refuses
      to replace the session pools, so that recovery ends with the session dead
      (`stat_unrecoverable_error`) instead of the pools corrupted. Every other port, a shared
      queue's too, calls it as on main, where 26.07 iavf, ice and i40e free those mbufs twice: a
      known bug (`MtDevTxDoneCleanupNotDeepTest.FreesNoMbufTwice`, skipped).
    - Other sessions: on a deep port `tv_pkts_capable_chain()` keeps zero copy while
      `st20_total_pkts` × (frames − 1) ≥ `nb_tx_desc` / 2 (the queue at 2 descriptors a pkt), or
      the larger RTCP retransmit ring (≥ `nb_tx_desc` pkts, it holds queued pkts too, and only
      newer pkts release it), plus the 512 of the builder ring; short of it
      `tv_tasklet_frame()` refuses a frame still referenced ("refcnt not zero"). At 2048 with
      RTCP and 2 buffers 720p and 1080i copy: a user decision, the workaround is `nb_tx_desc` 512
      or 3 buffers. Every other port keeps main's bound (`nb_tx_desc` pkts, no builder ring),
      which zero-copies 480p BPM (686 pkts) on 2 buffers at 512 and meets its frames still
      referenced where the PMD frees lazily, and a 576p RTCP session too: known bugs
      (`St20TxZeroCopyLazyTest` P480Fb2Desc512, `NotDeepRtcpRingSpanningEverySpareBufferCopies`,
      skipped).
    - Time: the RL training pad trains fill the ring, and the training waits for it to drain (video ~7.5 ms at 1080p59.94, audio ~170 ms at 0.33 ms ptime), and the stop-time flush waits for each queue to drain (TX Pad Flush; on a lazy port 2 × `nb_tx_desc` pads at the queue's rate, ~0.2 s per audio RL queue at 0.33 ms ptime).
    - Recovery: `st20_tx_queue_fatal_error()` flushes the other redundant leg inside the tasklet, its ring's drain if it is healthy (~4 ms at 1080p59.94, ~8 ms with 2048 descriptors queued), so the scheduler's other sessions miss that much once, after a TX hang that is already fatal (a hung one: see Zero copy, Hang).
      - On a deep port, without a sys TX mbuf for the mark it skips that flush (`mt_txq_recovery_flush()`, -ENOMEM) rather than run the lazy loop, and the session ends unrecoverable (see Zero copy, Hang).
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

Never log under a session spinlock: tasklets `*_try_get()` it and skip the session while
it is held. Copy the counters under the lock, `put`, then log the copy (TX video:
`tv_stat_collect()` / `tv_stat_log()`).

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
- IAVF queue rates persist in the PF across processes, so `mt_dev_free()` puts every queue back to line rate; a killed RL process, a `mtl_init()` that fails in `mt_dev_create()` or a link speed iavf reports as 0 (e.g. 200G) still leave them; the next non-RL process clears them, see [iavf Per-Queue Rate Limits Outlive the Process](#iavf-per-queue-rate-limits-outlive-the-process)

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
At target − `warm_pkts`·`trs` the RL transmitter warms the NIC shaper with pads (`mt_build_pad()`, no RTP header). What follows depends on whether `tv_rl_plan_init()` planned the port: `s->rl_plan[port].armed` (`struct st_tx_video_rl_plan`, which also keeps `pad_size` and `trim_pads`; `pad_ns` and `bucket_ns` are 0 without a plan).
- A plan needs the inputs below, and the trim pads `tv_init_trim_pads()` builds only on an RL port whose NORMAL pkt is at least 128 B, so a 64 B cut leaves a frame:
  - `pad_ns` = `trs`·pi/(pi+1), the shaper time of the mean training pad at that port's own
    measured `pad_interval` pi (trained or pre-trained, `tv_set_measured_pad_interval()`), scaled
    to a NORMAL pad by bytes: training sends a frame's NORMAL and EXTRA pads (`tv_train_pad_type()`,
    EXTRA 6 bytes longer; GPM_SL its NORMAL and LINE_TAIL ones), the pre-arm NORMAL ones. Unscaled, 1080p25 BPM (share 0.99881) launched
    packet 0 13 ns a pad early on E830, 1.7 µs over a 130-pad warm-up. `tv_train_pacing()`
    keeps the whole ring full of pads for 71 frames, then sleeps until it drains (`nb_tx_desc` pads,
    ~7.5 ms at 2048 and 1080p59.94): the pre-arm and the WAIT_TARGET hold both assume an idle queue.
    Untrained rates only. A pre-train result is keyed by the rate and the training's pad mix
    (`struct mt_pacing_pad_mix`, `tv_train_mix()`: bytes, NORMAL size, pads with the remain), so a
    session of another mix at that rate trains its own; reused, its `pad_ns` came out 0.119% too
    long and packet 0 early. It is keyed by the queue's programmed rate too (`txq->bps`): a retrain
    for another mix makes `tv_init_hw()` program the rate's later queues ≥ 0.5% faster, so a
    session of the first mix trains again there. The bps retrain result stays the rate's alone,
    as audio uses it, and a new mix measuring slow on a retrained rate falls back to TSC.
    A user `ops.pad_interval` or the static table gives none;
    `pacing->pad_interval` keeps only the last port's pi.
  - The rate programmed on the queue, bps = `s->queue[i]->txq->bps`: `tv_rl_bps()`, a bps retrain result, or the cached one `tv_init_hw()` looked up. A queue without its own `txq` (shared TX queue, AF_XDP, kernel socket) has none. `st20_tx_queue_fatal_error()` gets the new queue at the old one's rate, which `pad_interval` and the plan were trained on.
  - `bucket_ns` = B/bps + aim.
    - B/bps: the shaper fills the full burst bucket B at the programmed rate. On E830 its clock runs ~11.7% ahead of the 446.43 MHz psm_clk ice computes the profile with, so training measures a faster drain; the bucket measured 2048 B/bps (80.0 µs at 720x288p50), not `pad_ns`·B/S (71.6 µs), which launched packet 0 early. Where the clocks agree (E810) the two match within ~0.3%.
    - `tv_rl_burst_size()`: iavf VF 2048 (the patched kernel ice, already required for RL); DPDK ice PF only an explicit `rl_burst_size`, as `ice_cfg_rl_burst_size()` programs it (DPDK's own default is 15 KB); any other driver, native AF_XDP included, unknown.
    - aim = `st_tx_video_rl_tokens(bps)`·`pad_ns`/S, the longest token timeslot ice can program at that rate, as the shaper drains it. `ice_sched_bw_to_rl_profile()` picks the shortest timeslot (32 << encode psm_clk cycles) that adds more than 128 B, so at most 257 B.
      Above ~1.47 GB/s even the shortest, encode 0, adds more; 257 B still bounds it up to ~2.95 GB/s, where the bound becomes the tokens of 32 cycles of the slowest psm_clk, 367 MHz.
      A timeslot passes those tokens at the true rate, which drains at least S per `pad_ns`, so this holds whatever the NIC clock (`St20TxRlModelTest.TokensCoverEveryIceTimeslot` checks it against a copy of ice's encode on all four psm_clks).
- With a plan, `video_trs_rl_pre_arm()` queues pads that a shaper with a `bucket_ns` bucket would finish at the target; the real one, an aim smaller, finishes them at target + aim. Packet 0 follows them to the NIC in the same pass, so the shaper, not the tasklet, sets its launch.
  Every drain end the pre-arm counts, its idle clamp included, is the shaper's less the aim (`St20TxRlFastClockTest.FluidShaperLaunchesOneAimPastTheTarget`).
- The pads cover `(target − now) + bucket_ns`: `ceil()` NORMAL
  pads, the last burst holding the ≤ S/64 + 8 pads that take the surplus as cuts of 64 or 8 bytes
  (cut rounded down). `st_tx_video_rl_next_burst()` in `st_tx_video_rl.h` plans each burst, pure
  code that `St20TxRlModelTest` pins, and `video_trs_rl_trim()` points its cut pads at the two
  `trim_pad` mbufs of the port, whose 64 B or 8 B cut leaves them near full. The shaper drains such
  near-full pads by their bytes (the EXTRA pads, 6 B longer, showed it, see `pad_ns`); much shorter
  ones it does not (S/2, S/4, S/8 pads cost ~0.2–0.7 µs each), so no pad is cut by more than 64 B.
- The model puts the launch at target + aim, or within one 8 B cut after it (~56 ns at 1080p25);
  never before it. It counts pkt bytes only: a shaper that charges a per-frame overhead o (FCS,
  preamble, gap: up to 24 B) makes the cuts and the NORMAL share save less, so the launch lands
  later by up to ~o/S of the cut (~0.18 µs at 1080p25), never earlier.
- **Token timeslots.** The shaper adds its tokens a timeslot apart, so it releases packet 0 at a
  tick, up to one timeslot before or after the drain end; the target's phase in the grid, random
  per run and beating with the frame period, picks the group (hardware: 2 or 3 groups a timeslot
  apart). A timeslot T ≤ aim releases packet 0 within T of target + aim, so in
  [target, target + 2·aim]: the aim makes it never early and adds at most 2·aim of lateness the
  plan cannot see (`St20TxRlFastClockTest.TimeslotGridNeverReleasesPacket0EarlyBehindTheAim`,
  `ut_trs_set_shaper_timeslot()`).
  - `tv_rl_plan_init()` keeps a plan only while aim ≤ `ST_TX_VIDEO_RL_TIMESLOT_MAX_NS` (2 µs),
    comparing aim, T's upper bound, as T itself is unknown. On E830 a ~4 µs timeslot (720p25)
    reached +10.8 µs, ones up to ~2 µs (720p50 at 2.03, 1080p25 at 1.80) stayed within
    +8.1 µs, and the hold path was as accurate at those rates (+3.6..+5.1 µs).
  - It is a rate threshold: 257 B over 2 µs, so a drain below ~128.5 MB/s holds, 8-bit 1080p25
    too. That arms 2160p59.94, 1080p59.94/50/29.97/25, 1080i59.94 and 720p59.94 (aim
    0.19–1.80 µs) and holds 720p25 (4.05 µs); SD holds on its window first. 720p50 is ~1.5%
    over the edge, so its trained `pad_ns` decides: it held on hardware, on time both ways.
  - The ~2 µs release grid once inferred on the early side came from frame-to-frame step sizes
    and is unconfirmed. Measured directly on the shaper at 720x288p50 (~25.6 MB/s programmed,
    ~8.2 µs timeslot; forced leads, credit sweeps), the launch minus the plan stayed within
    ±0.6 µs, and with the bucket at B/bps the main group sits at the hold latency. The armed
    formats measured never earlier than the hold path's minimum, +3.6 µs.
- The target is the 90 kHz tick the frame's RTP timestamp names (`tv_sync_pacing()` snaps
  epoch + `tr_offset` − `vrx`·`trs` to it), so the target sits −5.56..+5.56 µs off the unsnapped
  epoch time, by format: −2.67 µs at 1080p25, −1.34 µs at 1080p50, +4.63 µs at 1080p60, frame by
  frame at 59.94.
- Measured on E830 VF (NoCtx, NIC RX timestamps, packet 0 against that target, budget 0..+10 µs). The hold path (no plan) measured +3.6..+7.7 µs across formats; 720x288p50 alone held ±0.13 µs around +4.2 µs.

  | Format | aim | Old plan (`pad_ns`·B/S, no aim) | B/bps + aim, no timeslot gate | Now |
  |---|---|---|---|---|
  | 720x288p50 | ~9.4 µs | −12.0 / −4.8 / −1.6 µs, two groups ~8 µs apart | — | hold |
  | 1920x64p59.94 | ~13 µs | −11.9 / −7.6 / +24.5 µs, three groups | — | hold |
  | 480p59.94 | ~4.7 µs | −4.2 / −0.8 / +7.7 µs | hold (trim pads) | hold |
  | 576p50 | ~4.2 µs | −2.5 / +3.0 / +14.1 µs | — | hold |
  | 720p25 | 4.05 µs | −0.6 / +1.1 / +5.0 µs | +5.6..+9.3 µs, one frame +10.76 µs | hold |
  | 720p50 | 2.03 µs | +0.7 / +2.0 / +5.2 µs | +5.4..+8.1 µs | hold |
  | 1080p25 | 1.80 µs | — | +4.8..+7.5 µs | armed |
  | 1080p59.94 | 0.75 µs | +3.0 / +4.3 / +5.7 µs | +4.3 / +5.8 / +7.5 µs | armed |
  | 1080p59.94 ×8, VRX margin | 0.75 µs | — | pass | armed |

  Now (min / avg / max): held 480p59.94 +3.73 / +4.15 / +4.98 µs, 576p50 +3.74 / +4.16 / +5.03 µs, 720p25 +3.71 / +4.25 / +5.10 µs, 720p50 +3.61 / +4.11 / +5.07 µs; armed 720p59.94 +4.50 / +6.15 / +7.50 µs, 1080p25 +5.26 / +6.43 / +7.54 µs, 1080p59.94 +4.02 / +5.43 / +7.00 µs, 1080p60 +4.43 / +5.88 / +7.31 µs, 2160p59.94 +3.26 / +3.67 / +4.50 µs; 1080p59.94 ×8 VRX margin passes.

  The bucket alone without the aim (720p25: +1.9..+6.0 µs over 4 runs) leaves the early group a timeslot before the plan, which the model says can go early.
- Target is MTL's PTP time turned into TSC once per frame in `tv_sync_pacing()`, so on the wire the launch also carries that clock's error. It is independent of how many sessions share the scheduler only while every warm-up starts before its target and has a plan.
- The user-meta packet (`st20_frame_tx_start()`, sys queue) leaves when packet 0 is queued, up to `warm_pkts`·`trs` ahead of it, but after the previous frame's last packet; RX matches it by RTP timestamp and the timing parser skips it.
- NORMAL pads follow in bursts of up to `ST_TX_VIDEO_RL_MAX_PADS_PER_BURST` (32) copies of one mbuf (lib sets no `MBUF_FAST_FREE`, which would forbid the shared refcount). Each burst is sized from a fresh TSC and the drain end of a token bucket `bucket_ns` deep over what is queued.
- Two kinds of stall, both count `stat_trans_recalculate_warmup`:
  - Between bursts: every queued pad reached the NIC before it. If the queue drained and the bucket refilled (the drain end fell behind TSC − `bucket_ns`), the drain end resets to TSC − `bucket_ns` and the rest shrinks. While `trim_pads` still fit it, the cuts land packet 0 on the plan;
    with fewer the pre-arm stops and WAIT_TARGET holds packet 0 behind the pads already queued, which drain before the target (`St20TxRlFastClockTest.PreemptedBetweenBurstsShortOfTheTrimPadsHolds`, `IdleShaperKeepsThePlanWhileTheTrimPadsFit`).
  - Inside a burst: its doorbell rang at some time up to the return, `last_burst_succ_time_tsc` (read by `video_trs_burst_succ()`, no extra TSC read).
  - If the return is more than `ST_TX_VIDEO_RL_STALL_NS` (2 µs) past the burst's planned drain start, the doorbell may have found the shaper idle: the drain end restarts from the return (never overfills), and `video_trs_rl_pre_arm()` returns false, so WAIT_TARGET holds packet 0.
  - The doorbell time is unknown, so releasing packet 0 behind the pads would make it late (stall before the doorbell) or early (after it).
  - The 2 µs is absolute because a burst's CPU cost does not scale with the format, while a pad does (~0.97 µs at 2160p59.94, ~8.7 µs at 720p60).
  - It is expected to be above a healthy 32-pad burst (not measured on hardware yet: a healthy run should keep `stat_trans_recalculate_warmup` at ~0) and at or below most timer ticks (2–10 µs). A shorter stall before the doorbell is not detected and delays packet 0 by up to its length.
  - Behind the first burst the shaper is busy ~32 pads, so only a longer stall in a later burst holds.
  - Tests: `St20TxRlWarmUpTest.Packet0LeavesOnTargetAfterAStallInAPadBurst` and `StallTheQueuedPadsCoverKeepsThePreArm`; `ut_trs_set_burst_stall()` stalls a burst before or after its doorbell.
  - Hardware, before the hold: E830 VF, 2× 720p60 st20p, wire timestamps from an E810 capture port whose PHC follows `CLOCK_REALTIME`, bpftrace for context switches and ≥ 1 µs interrupts on the scheduler CPU.
    - The 1 ms tick inside the pre-arm call moved packet 0 by 7.4 µs median (2.0 → 9.4 µs, max 4.0 → 15.4 µs).
    - 720p60's grid repeats in 3 frames (50 ticks): on that boot one phase met the tick in 1687 of 2395 calls, the others never. 1080p59.94 drifts (1–2 of 7177).
    - Not isolated, `kworker` preemptions gave 33 single-leg shifts of 52–272 µs in 240 s, and once the next frame's pre-arm took the still-busy shaper as idle (104 µs late, no stall of its own).
    - One 139 µs stall had no context switch, traced interrupt or SMI: unexplained.
- Refused pads count as queued and retry, batched the same way, via `trs_pad_inflight_num` ahead of packet 0, which can only delay it. They retry as NORMAL pads: a refused cut pad hands its reference to the NORMAL one, so packet 0 is later, never early. Ports without a plan keep one pad per burst and per retry.
- Without a plan, `video_trs_rl_warm_up()` queues `ceil((target − now) / trs)` single-pad bursts and `ST_TX_VIDEO_RL_STATE_WAIT_TARGET` holds packet 0 until the TSC reaches target, so a late tasklet launches it late. A planned port whose target is more than `warm_pkts`·`trs` away holds too, without pads. Target already past: no pads, packet 0 goes at once.
- `tv_init_pacing()` sets `warm_pkts` to 80% of `pkts_in_tr_offset`, capped at 128, or 8 at height ≤ 576; 0 without RL, for wide pacing and for ST22.
- `stat_trans_troffset_mismatch`: target about two `trs` or more past, or more than `warm_pkts` pads (`warm_pkts`·`trs` with a plan) needed; no pads are sent.

### RL Pacing Limits
Progressive, height ≥ 1080; T = frame time. Interlaced, 720p and SD use their own `reactive`/`tr_offset` (`tv_init_pacing()`), same reasoning.
- **Frame budget.** Packets span `reactive`·T = 1080/1125·T at `trs` each. From the previous frame's last packet to the next target is `frame_idle_time` + `tr_offset` = (2 + 43)/1125·T, 0.67 ms at 59.94.
- **Warm-up window** [target − `warm_pkts`·`trs`, target], with `warm_pkts` = min(0.8·`tr_offset`/`trs`, 128).
  - Uncapped, the window is 34.4/1125·T. It opens ~10/1125·T (~150 µs at 60p) after the previous frame's last packet left the NIC.
  - That margin is what lets `video_trs_rl_pre_arm()` treat the queue as empty and the bucket as full: its drain starts at TSC − `bucket_ns`.
  - The bucket refills at the programmed rate in that idle gap, `frame_time`·(1 − `reactive`) − `warm_pkts`·`trs` (192.8 µs at 1080p59.94), so `tv_rl_plan_init()` holds a port whose B/bps is longer: only a DPDK ice PF `rl_burst_size` gets there (~63 KB at 1080p59.94); a larger one would have packet 0 land up to the shortfall late.
  - Pads queued behind an unfinished frame would make packet 0 late, so the window cannot grow past that idle gap. The 80% is margin, not a tuning knob.
- **Ring bound on a plan.** `tv_rl_plan_init()` keeps a plan only if (`warm_pkts`·`trs` + `bucket_ns`)/`pad_ns` + 1 ≤ `nb_tx_desc`/2, the aim inside `bucket_ns`. With the deep `MT_DEV_TX_DESC_DEEP` 2048, that must fit in 1023 pad drains (1024 less the `ceil()` pad), so the `warm_pkts` cap of 128 binds first; an RL port that frees lazily keeps 512, 255 drains, which the cap still fits.
- **Trim pads in the window.** The last burst keeps `trim_pads` pads, enough to cut up to a pad (a cut under 1262 B takes at most 24 of its 27: 19 coarse, 5 fine; the slack absorbs the one-pad drop to a fresh TSC). With fewer, packet 0 would launch late by what they cannot cut (seen +14..+24 µs at SD). `tv_rl_plan_init()` holds a port whose full window has at most `trim_pads` − 1 pads:
  `warm_pkts` 8 at height ≤ 576 gives ~10. Each frame, `video_trs_rl_pre_arm()` checks the pads left the same way, at its entry before any burst and after an idle shaper between bursts, and holds packet 0 when too few are left: WAIT_TARGET then sends it at the target, sooner than those pads would have (`St20TxRlFastClockTest.LateEntryShortOfTheTrimPadsHoldsPacket0ToItsTarget`).
- **Where the 128 cap binds.** Only UHD and above: 2160p is 524 → 128, a 4× shorter window. At 1080p it is 130 → 128, which makes no difference.

**Tolerance at each point** (scheduler lateness, or a tasklet stall):

| Point | With a plan | Without a plan |
|---|---|---|
| Reaching WAIT_WARMUP's pre-arm | While `trim_pads` pads still fit: packet 0 still lands at target + aim, within an 8 B cut after it. Later: no pads, WAIT_TARGET holds packet 0, so it leaves at the tasklet's poll at the target. At or past target: no pads, packet 0 late by the lateness. | ~0: WAIT_TARGET holds packet 0, so its launch is the tasklet's poll time. |
| Between pad bursts | A stall that ends before target shrinks the remaining pads (`stat_trans_recalculate_warmup`). If fewer than `trim_pads` are left, WAIT_TARGET holds packet 0, which leaves at the tasklet's poll at the target. | — |
| Inside a pad burst | Covered by the pads already queued: no change. Otherwise, up to 2 µs: packet 0 late by up to the stall. Longer: WAIT_TARGET holds packet 0, so it leaves at the later of the tasklet's poll and the drain end of the pads already in the NIC (`stat_trans_recalculate_warmup`). | — |
| Frame body | The transmitter keeps the NIC ring full, so a stall up to `nb_tx_desc`/2·`trs` (2 descriptors per chained pkt; `nb_tx_desc`·`trs` in copy mode) drains on time. Longer gives a wire gap, VRX underflow at RX, then a burst. | Same |

| Format | `trs` | `tr_offset` | `warm_pkts` | Window | Default ring body (1024 pkts) | Ring-capped window: 512 / 2048 |
|---|---|---|---|---|---|---|
| 1080p29.97 | 7.78 µs | 1.27 ms | 128 | 1.0 ms | 8.0 ms | same (cap not binding) |
| 1080p59.94 | 3.89 µs | 0.64 ms | 128 | 0.50 ms | 4.0 ms | same |
| 1080p119.88 | 1.95 µs | 0.32 ms | 128 | 0.25 ms | 2.0 ms | same |
| 2160p29.97 | 1.95 µs | 1.27 ms | 128 | 0.25 ms | 2.0 ms | 251 → 0.49 ms / 524 → 1.0 ms |
| 2160p59.94 | 0.97 µs | 0.64 ms | 128 | 0.12 ms | 1.0 ms | 251 → 0.24 ms / 524 → 0.51 ms |
| 2160p119.88 | 0.49 µs | 0.32 ms | 128 | 62 µs | 0.50 ms | 251 → 0.12 ms / 524 → 0.26 ms |

- **Longer pad trains.** Measured 2160p59.94, E830 VF TX, isolated, rx_timing parser, `pad_ns` 965 ns:

  | `warm_pkts` | `nb_tx_desc` | Latency avg | Latency min |
  |---|---|---|---|
  | 128 | 512 | 3824 ns | 3032 ns |
  | 128 | 2048 | 3848 ns | 3288 ns |
  | 256 | 2048 | 3817 ns | 3288 ns |
  | 512 | 2048 | 3700 ns | 2776 ns |

  All runs were compliant narrow, with 0 VRX failures. A 4× longer train moved packet 0 by less than one pad, so shaper drift over the train is not what limits `warm_pkts`.
- **Concurrent sessions.** Each planned session's packet 0 is set by its own queue's shaper, as long as the scheduler loop reaches its pre-arm inside the window. Every other tasklet on that scheduler adds its pass time. A pre-arm pass costs ⌈n/32⌉ `tx_burst` calls.
- **Non-isolated cores.** The tolerances above are the whole margin. A preemption, softirq or SMT sibling on a shared core can exceed them, so this is best effort (`doc/isolation.md`, `tests/tools/isolate/isolate.sh`). A larger `nb_tx_desc` raises only the frame-body row, unless the cap is ring-derived.
- **No plan, so the #1622 behavior.** This happens with:
  - a user `ops.pad_interval` or the static pad table;
  - a DPDK ice PF without the `rl_burst_size` devarg, or with one that cannot refill in the idle gap before the window;
  - drivers other than iavf and ice;
  - no programmed rate on the queue: a shared TX queue, AF_XDP, the kernel socket path;
  - a window that fails the half-ring check, for example with a small `nb_tx_desc`;
  - a window of at most `trim_pads` − 1 pads: every format of height ≤ 576;
  - an aim past `ST_TX_VIDEO_RL_TIMESLOT_MAX_NS`: a drain below ~128.5 MB/s, such as 720p25, 720p50 by a ~1.5% margin, and 8-bit 1080p25.
- **Linear.** `ST21_PACING_LINEAR` is declared in `st20_api.h`, but nothing in `lib/src` reads it on TX, so every TX schedule is gapped.
  - Linear has no idle gap, so there is no window for warm-up or pre-arm, and RL alone has no launch time to re-anchor each frame.
  - It would need closed-loop rate correction on RL, or TSN launch time (§4.3.3 in `doc/design.md`, E830 PF), which stamps every packet.

### PTP: The Time Reference
- MTL implements PTP slave in software (`mt_ptp.c`)
- NIC hardware timestamps PTP packets
- Every RTP packet carries PTP-derived timestamp
- Without PTP: local TSC-based pacing works but clocks drift

### Epoch Timing
Frame transmission aligned to PTP epoch boundaries. For 59.94fps: frame period ≈ 16.683ms.
- Epoch math is exact integer, in ST20/ST22, ST30 and ST40 TX: `st_epoch_to_tai()`,
  `st_tai_to_epoch()` and `st_tai_to_nearest_epoch()` (`st_fmt.c`, 128-bit) take the period as
  `den/mul` s: `fps_tm.den/fps_tm.mul`, or for ST30 `sample_num` samples at the sampling rate.
  - Epoch k starts at `floor(k·1e9·den/mul)` ns, so its TAI maps back to k, and the steps alternate
    between the floor and ceil of the period without drift.
  - Floor, not ceil: odd 59.94 epochs fall on a half tick, which the 90 kHz snap rounds down; a
    ceil-ed ns would round them up and break the 1501/1502 RTP alternation.
  - A `double` period is off by up to ~2e-9 ns, times ~1e11 epochs at today's TAI: the ST20 epoch
    was +67 ns off at 59.94 and +107 ns at 23.98, growing with the date; an ST30 80 µs packet
    −105 ns, and its TAI read back as the packet before it.
  - `pacing->frame_time` stays for the floating uses (`trs`, `tr_offset`, windows); vsync
    (`st_vsync_calculate()`) still divides by it.
- Late frame (next free epoch already past) → snap to the current epoch, `stat_epoch_drop` += skipped epochs, `notify_frame_late` fires (`calc_frame_count_since_epoch()`)
- Fix is in the application, not MTL
- Interlaced TX: `frame_time` is one FIELD period, so the ST 2110-21 frame grid is every
  SECOND epoch slot (even = first field). Corrected at the `tv_sync_pacing()` call site;
  `calc_frame_count_since_epoch()` stays parity-blind. Exclusions: EXACT_USER_PACING with a
  usable timestamp, and ST22 at RTP level (`s_type == MT_ST22_HANDLE_TX_VIDEO &&
  !s->st22_info`). Do **not** add 6.3.3's `T_LINE/2` — it reaches the RTP timestamp and
  breaks ST 2110-10 7.6.1.
- The 20/525, 26/625, 22/1125 TROFFSET numerators are legacy ST 2110-21:2017 fixed values; the
  2022 Table 1 TRO_DEFAULT equation is defective for the 525/625 rows (dangling `+` in the
  published PDF). Do not "re-derive" them.

### MEDIA_CLK User-Timestamp Anchoring (`st10_media_clk_to_tai`)
`ST10_TIMESTAMP_FMT_MEDIA_CLK` carries only a raw 32-bit tick count — not a full TAI
instant — so a USER_TIMESTAMP flag path must unwrap it before use:
- `uint64_t st10_media_clk_to_tai(uint64_t anchor_tai_ns, uint32_t media_ts, uint32_t sampling_rate)`
  in `include/st_api.h`, implemented in `lib/src/st2110/st_fmt.c`. Public API — apps that
  track their own RTP timestamps can estimate the real TAI instant directly, alongside
  `st10_get_tai()`/`st10_get_media_clk()`.
- Anchors on the caller's current pacing instant (video: `pacing->ptp_time_cursor`) —
  picks the 32-bit tick cycle nearest the anchor, then converts to ns in one
  `st_muldiv_u64_round_closest()` step. Rounding the anchor and the tick delta as two
  independent steps can push the result across a tick boundary at a rounding tie —
  this compounding-rounding bug is what dc8df092 fixed for st20.
- Disambiguation window: the app's MEDIA_CLK timestamp must represent a time within
  about half a 32-bit tick cycle of the session's actual current time — ~6.6 hours at
  90kHz (video/ancillary), ~12.4 hours at 48kHz (audio) — or the unwrap resolves to the
  wrong cycle.
- Shared by st20 (video), st30 (audio), and st40 (ancillary) TX, each anchoring on their
  own pacing cursor rather than reimplementing the tick-unwrap math.

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

### RX Video Frame Copy Is Cache-Bound, Not CPU-Bound

At high session density the RX video scheduler core still has idle cycles while the
NIC starts dropping — what runs out first is cache residency of the freshly DMA'd
mbufs, not CPU time. Hence `rv_handle_mbuf()` software-prefetches the next packet's
header line (and, only for the handlers that CPU-copy out of the same mbuf, the line
behind it). The hardware L2 streamer cannot do this job: each packet sits in an
independently recycled mbuf element, so successive packet-data addresses within one
burst carry no stride for the prefetcher to lock onto.

Corollaries when tuning: enlarging `nb_rx_desc` past what fits in LLC makes density
*worse*, not better — more in-flight mbufs than the last-level cache holds means the
payload is cold by the time the copy reads it (on a 2-socket Xeon 8480+, 4096 RX
descriptors × 22 1080p59.94 sessions was catastrophically worse than 1024). For the
same reason, deliberately deferring polls to amortize per-pass overhead backfires: a
deeper per-session backlog is a colder backlog. Benchmark on a quiet host — co-tenant
memory traffic moves this ceiling by a whole session, so compare variants interleaved
back-to-back, never across time.

### Performance Debugging Mental Model

1. **NIC bottleneck?** Check `stat_tx_burst` vs `stat_tx_bytes`
2. **Scheduler overloaded?** 100% CPU, never sleeping → too many sessions
3. **Pacing broken?** RX: VRX stats. TX: `stat_epoch_drop`, `stat_epoch_onward`; audio/anc/fmd also `stat_epoch_mismatch`
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
- `rv_handle_frame_pkt` — frame/slice mode (the default for `st20_is_frame_type`)
- `rv_handle_rtp_pkt` — RTP passthrough (`ST20_TYPE_RTP_LEVEL`), enqueues to `rtps_ring`
- `rv_handle_hdr_split_pkt` — header-split mode, payload lives in `mbuf->next`
- `rv_handle_st22_pkt` — compressed video
- `rv_handle_detect_pkt` — auto-detection mode
- `rv_handle_detect_err` — detection error handler

### TX Destroy Order (Load-Bearing)
Dependency chain: `NIC TX descriptors → hold mbufs → hold extbuf refs → point to frame buffers`

Actual sequence in `tv_uinit()`:
1. `st_tx_video_transmitter_state_cleanup()` — free software-held mbufs: `inflight[]`,
   `trs_inflight[]`, `trs_inflight2[]`, and one pad reference per `trs_pad_inflight_num`
2. `tv_uinit_hw()` — drain ring (`mt_ring_dequeue_clean`), free ring, flush TX queue
   (`mt_txq_flush()` pushes pad packets to force all session mbufs out of the NIC),
   release queue (`mt_txq_put()`), then free the pad mbufs
3. `tv_uinit_sw()` — free packet ring, chain + header pools (`tv_mempool_free()`),
   then frame buffers (`tv_free_frames()` → `rte_free(frame->addr)`)

**Why software mbufs first (step 1 before 2)**: `tv_uinit_hw()` frees and NULLs `s->pad[i][j]`.
The pad references retained by failed bursts are counted in `trs_pad_inflight_num` and can only
be returned while the pad pointer is still live — release them later and they leak. Pads come
from the per-interface `tx_mbuf_pool` (`mt_sys_tx_mempool()`), not a session pool, so that leak
is never reclaimed for the process lifetime and never shows up in `tv_mempool_free()`.

**Why this is not a use-after-free**: freeing a chain mbuf drops one `sh_info` reference and can
run `tv_frame_free_cb()`. `sh_info` counts *every* live chain mbuf, NIC-owned ones included
(`rte_mbuf_ext_refcnt_update(&frame_info->sh_info, 1)` per `rte_pktmbuf_attach_extbuf`), so the
callback cannot fire while any packet of that frame is still in a TX descriptor.

**Why HW before the pools and frames (step 2 before 3)**: after the flush the NIC holds no
references, so the frame buffers the extbufs pointed into can finally be freed. This is the
constraint the old *"must uinit hw firstly as frame use shared external buffer"* comment named.

`st20_tx_queue_fatal_error()` reuses step 1 for a different reason — the payload pools *are*
session-owned and shared across redundant ports, so every port must give its mbufs back before
the pools are replaced.
There `tv_mempool_free()`'s return value is load-bearing: `mt_mempool_free()` returns 0 even
when it declines to free an in-use pool, so recovery escalates to `ST_EVENT_FATAL_ERROR` rather
than dropping the session's only handle to a pool that was never freed.

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
3. DMA GUARD: If the session has copies pending (!mt_dma_empty(), lender nb_borrowed > 0)
   → rv_dma_flush(): copies go to the engine once per tasklet pass, so a burst holding
   frame N's tail and N+1's head gets here with N's copies unsubmitted. Submit them and
   poll completions for up to ST_RX_VIDEO_DMA_FLUSH_TIMEOUT_NS, which completes N if N is
   full. Still pending (tried once per newer frame; never with a pkt lcore) → drop the
   packet (dma_previous_busy_cnt). Worst case per engine: each of up to
   MT_DMA_MAX_SESSIONS sessions sharing it spends one bounded flush per new frame
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

Complete → `rv_slot_full_frame()` → `ST_FRAME_STATUS_COMPLETE`. Incomplete/evicted → `ST_FRAME_STATUS_CORRUPTED`, recycled rather than delivered unless the app set `ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` (`rv_frame_notify()`).

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
- ST2110-41 fast metadata is not a separate session type: an ST40 session opts in with
  `ST40_TX_FLAG_FAST_METADATA` / `ST40_RX_FLAG_FAST_METADATA` (`st40_api.h`). The mode swaps
  only the RTP payload format: `struct st40_fmd_rtp_hdr` (22-bit DIT, K-bit, 9-bit word length,
  no extended seq), header 58 bytes, default payload type 115, marker always 0, one zero-padded
  data item per frame from `st40_frame.data`/`data_size`. `data_size` 0 sends a zero-item
  packet (bare 12-byte RTP header), since a Data Item Length of 0 is illegal. TX create rejects
  the reserved DITs 0x300000-0x3FEFFF and payload types 1-95. Pacing, redundancy and stats are
  the ST40 ones; RTP_LEVEL packets sharing a timestamp go out back to back, so more than 4 per
  timestamp breaks the ST 2110-41 §7 network compatibility model.
  RX in this mode is RTP_LEVEL only and must leave the payload untouched — the RFC 8331 F-bit
  parse and in-place byte swap would corrupt it. `st40_rx_get_mbuf()` takes `len` from the UDP
  header, because the 54-byte zero-item frame is padded to 60 on the wire.
  Spec conflict: Annex A says segment words = DIL - 2, but §5.4 makes DIL count the words after
  the DIT/K/DIL word; the Annex A integration test follows §5.4 (DIL - 1 segment words).
- ST30 RX, like ST20: a short frame counts `stat_frames_incomplete` and is recycled, or zeroed-then-delivered under `ST30_RX_FLAG_RECEIVE_INCOMPLETE_FRAME`
- ST30 TX RL (`ST30_TX_PACING_WAY_AUTO` picks it below 0.5 ms ptime) uses two queues per port, alternating each 10 ms.
  - `tx_audio_session_profiling_rl_bps()` trains the first one by filling its whole ring (`nb_tx_desc` pkts and pads, 4 per `trs`).
  - It then sleeps until that ring drains (~170 ms at 2048 descriptors and 0.33 ms ptime, ~43 ms at 512): the session paces each chunk as if its queue were idle. Untrained rates only, so the first session per rate pays.

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
- TX offloads: `MULTI_SEGS` (scatter-gather for header+chain) + `IPV4_CKSUM` (HW checksum). RX offloads: `TIMESTAMP` only with `MTL_FLAG_ENABLE_HW_TIMESTAMP` (no scatter-gather, `rx_nseg=0`)
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
`mt_txq_flush()` → `mt_dpdk_flush_tx_queue()` / `mt_tsq_flush()` frees every mbuf queued before it:
- Lazy ports (the PMD frees an mbuf when it reuses the slot): `mt_if_nb_tx_burst(impl, port) × 2` pad packets (for DPDK ports, `nb_tx_burst = nb_tx_desc`), one burst each with a 1 ms timeout (10 ms on a shared queue).
- Deep `mt_if_tx_reclaim_rs_thresh()` ports, never a shared queue (`dev_flush_tx_eager()`): a copy of the pad from the sys
  TX pool, the mark, then 2 × rs pads, so the mark's batch is cleaned with 2 batches in use;
  in-order cleanup frees everything before it first. Then a burst of none every 10 µs until the
  mark is back: 65 descriptors, not 4096. A full ring takes the mark and pads as its batches
  drain; a send gives up once nothing went in for 2 ms + rs × 1538 B at the queue's rate (line
  rate without one), the mark's refs freed if it never went in. The whole flush gives up
  (-EBUSY) at 2 ms + `nb_tx_desc` × 1538 B at that rate; without a copy it runs the lazy loop, which teardown needs to free the pools. `mt_txq_recovery_flush()`, the other-leg flush of `st20_tx_queue_fatal_error()` in the tasklet, returns -ENOMEM instead (`MtDevTxFlushTest`).
- `tx_audio_session_sq_flush_port()` and its ancillary twin push pads through the transmitter ring instead, unchanged.

Pad destination MAC: `01:80:C2:00:00:01` (IEEE slow-protocol, switches won't forward).

The port pad (`mt_get_pad()`) is one mbuf every thread may send. On a deep port, whose bounded flush and recovery send it from the tasklets too, `mt_dev_if_init()` takes a ref of its own on it, so its refcnt never reads 1, where `rte_mbuf_refcnt_update()` sets the count instead of adding atomically.
Two schedulers flushing at once could lose a ref and free the pad from under the ring. Other ports keep main's single ref, open to that race, and `dev_tx_pkt_check()` hands it to the PMD in place of an invalid pkt without one: known bugs (`NotDeepPortPadKeepsARefOfItsOwn`, `NotDeepPadInPlaceOfAnInvalidPktKeepsItsRef`, skipped).

Why pads instead of `tx_done_cleanup`: the API only frees already-completed descriptors — if the NIC hasn't finished DMA-reading an mbuf, cleanup won't touch it.
Pads actively force the ring to cycle, guaranteeing completion regardless of NIC state.
(Note: MTL calls `rte_eth_tx_done_cleanup` only on TX hang recovery, best-effort, and never where it double frees (Pools, `rte_eth_tx_done_cleanup()`); pad flush is the authoritative cleanup.)

### RX Burst Pattern
Default burst: 128 (runtime assignment: `s->rx_burst_size = 128`). RX packets fit in a single mbuf because `data_room = 1664` > max packet size (1494 bytes) — no multi-segment reassembly needed.

Continuous burst: if `rte_eth_rx_burst` returns ≥ `rx_burst_size / 2` (≥64), track as continuous burst. Tasklet returns `MTL_TASKLET_HAS_PENDING` when any packets received → scheduler re-polls immediately.

### Header-Split RX
Intel E810 with `ST20_RX_FLAG_HDR_SPLIT`: NIC writes payload directly into frame buffer, bypassing CPU memcpy. Requires DPDK ice driver patches.

### iavf HW RX Timestamp Workaround
Only with `MTL_FLAG_ENABLE_HW_TIMESTAMP` on iavf VFs.
- **Defect (DPDK 26.07)**: iavf AVX2/AVX512 flex RX (`iavf_rxtx_vec_avx2.c`, `iavf_rxtx_vec_avx512.c`) flags every mbuf timestamp-valid without checking `IAVF_RX_FLX_DESC_TS_VALID` and seeds `rxq->phc_time` from the last mbuf, so one unstamped descriptor makes later timestamps jump by ~2^32 ns. Scalar RX (`iavf_rxtx.c`) checks the bit.
- **Workaround**: after `rte_eth_dev_adjust_nb_rx_tx_desc()`, `dev_config_port()` raises a power-of-2 `nb_rx_desc` by 32 (2048 → 2080), or lowers it by 32 at `rx_desc_lim.nb_max`. Vector RX needs a power-of-2 ring, so iavf picks "Scalar Bulk Alloc Flex" (a multiple of `rx_free_thresh` 32 keeps bulk alloc). Applies to a user-set `nb_rx_desc` too; no measurable cost at ~0.9 Mpps/queue.
- **Verify**: testpmd `show rxq info` burst mode is "Scalar Bulk Alloc Flex". On regression, strict NoCtx pacing tests fail with ~2^32 ns jumps.
- **Remove** once iavf vector RX checks TS_VALID upstream.

### iavf Close vs. Interrupt Callback Race
- **Defect (DPDK 26.07 and upstream main)**: `iavf_dev_close()` ignores the `-EAGAIN` that `rte_intr_callback_unregister()` returns while the EAL interrupt thread is inside iavf's callback. Likely when `dpdk-intr` shares a CPU with the closing thread, e.g. in a NoCtx exclusive partition.
- **Symptoms** at `mtl_uninit()`, after a passing test: SIGSEGV during `mt_dev_if_uinit`, or `EAL: PANIC in eal_intr_thread_main(): Error adding fd N epoll_ctl, Bad file descriptor` in `rte_eal_cleanup()`.
- **Fix**: `patches/dpdk/26.07/0009-net-iavf-fix-interrupt-callback-race-on-close.patch`; root cause, evidence, reproducers and upstream status in the [.md next to it](../../patches/dpdk/26.07/0009-net-iavf-fix-interrupt-callback-race-on-close.md).
- **Verify**: `UnitTest --gtest_filter='EalIntrCallback.*:DpdkIavfPatch.*'`; `DpdkIavfPatch.*` fails when the loaded iavf PMD lacks the patch.

### iavf Per-Queue Rate Limits Outlive the Process
- **Defect (ice PF)**: RL pacing puts every iavf TX queue on a shaper (`ST_DEFAULT_RL_BPS`, then each session's rate). The PF saves each queue's rate and replays it whenever a later process enables the queue, until the VF is destroyed.
- **Symptom**: a later TSC run on the same VFs is capped, e.g. 1080p59.94 at ~27 fps.
- **Fix**: `dev_if_clear_iavf_queue_rate()` commits every TX queue without a shaper, which the PF applies as no limit, for every non-RL way, the shared TX queue included. After an AUTO RL init that failed past the TM root, `dev_rl_drop_hierarchy()` first deletes the nodes it left. Best effort: a failure only warns.
  - The clear marks the RL root active, so `mt_dev_free()` then puts the queues on line rate.
  - Needs DPDK 26.07.91 (`MT_DEV_HAS_IAVF_TM_QOS_CHECK`): patch 0010 sends each unshaped queue's own ID, and 0011 makes the TM ops return `-ENOTSUP` on a VF without PF-granted QoS, where `rte_tm_capabilities_get()` probes first.
- **Verify**: `UnitTest --gtest_filter='MtDevPacingTest.*'`; on hardware `NoCtxTest.st20p_tsc_pacing_after_rl_process_full_fps` via `noctx/run.sh`.

### mbuf Lifecycle

**TX**: alloc header → fill headers → chain extbuf → enqueue ring → tx_burst → NIC DMA → mbuf freed back to pool (extbuf refcnt dec)

**RX**: NIC writes to mbuf data room → rx_burst returns mbuf → parse + memcpy/DMA payload to frame → `rte_pktmbuf_free()` returns to pool

### Traffic Manager (Rate Limiter)
Hierarchy: port → shapers (max 128, `MT_MAX_RL_ITEMS`). Each shaper = one session's rate limit.

### Multi-Process
Not supported: `dev_eal_init()` always passes `--in-memory`, so no DPDK runtime files exist for a `--proc-type=secondary` process to attach to. Independent MTL processes (one EAL each, e.g. on separate VFs) coordinate lcore use through MtlManager instead.

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
| Acceptance Tests (pytest) | pytest | Python | E2E via RxTxApp/FFmpeg/GStreamer | 10s of minutes |
| Shell scripts | bash | Shell | JSON-driven loopback scenarios | Minutes each |

### Integration Tests (`tests/integration_tests/`)

One binary:
- `KahawaiTest` — main ST2110 tests (links MTL)

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
Tests needing isolated `mtl_init`/`mtl_uninit` per test. DPDK EAL cannot
re-init within one process, so each `NoCtxTest.*` case is run in its own
`KahawaiTest` subprocess. `noctx/run.sh` enumerates cases via
`--gtest_list_tests` and loops; the MCP `run_noctx_tests` tool does the same
(accepts filters that match many cases). Requires 4 ports; `run.sh` waits 20 s
between processes, `run_pf.sh` and the MCP tools 10 s. What each case proves:
`tests/integration_tests/noctx/README.md`.

### Fuzz Tests (`tests/fuzz/`)

| Target | File | What |
|--------|------|------|
| ST2110-20 video RX | `st20/st20_rx_frame_fuzz.c` | RFC4175 video parsing |
| ST2110-22 compressed RX | `st22/st22_rx_frame_fuzz.c` | RFC9134 compressed parsing |
| ST2110-30 audio RX | `st30/st30_rx_frame_fuzz.c` | Audio parsing |
| ST2110-40 ancillary RX | `st40/st40_rx_rtp_fuzz.c` | Ancillary parsing |
| ST40 helpers | `st40/st40_ancillary_helpers_fuzz.c` | UDW, checksum, parity |
| RTCP TX | `rtcp/mt_rtcp_tx_parse_fuzz.c` | RTCP NACK parsing |

Architecture: minimal DPDK EAL (no hugepages, no PCI) → full session context reset per input → call internal handler.

**Limitations**: single-packet fuzzing only (no multi-packet sequence bugs), no TX/control/PTP coverage.

**Stub context must track the session**: a harness hand-builds `g_impl`/`g_session`, so a new mandatory field breaks it silently. Set `ops.type` explicitly (0 is FRAME_LEVEL for ST40, which reads PTP time the stub lacks) and supply any per-session buffer the create path allocates (ST30 `frame_bitmap`).

Build with clang (gcc lacks `-fsanitize=fuzzer-no-link`); `-Denable_asan=true` fails under clang (needs gcc `libasan`), use `b_sanitize`:

```bash
CC=clang CXX=clang++ meson setup build_fuzz -Denable_fuzzing=true \
  -Db_sanitize=address -Db_lundef=false -Dc_args=-Wno-error=unused-but-set-variable
ninja -C build_fuzz
./build_fuzz/tests/fuzz/st20_rx_frame_fuzz corpus/st20
```

The guide is `tests/doc/fuzz/index.rst`.
The `fuzz-tests` job of `fuzz_tests.yml` runs `.github/scripts/ci/fuzz.sh` (`task ci:fuzz -- build|run`) on `ubuntu-22.04` (clang 14) and uploads `fuzz-report`: 60 s per target on a pull request that matches the `fuzz_tests` path filter, 600 s per target on the nightly schedule of `main` and on a manual run.
The harnesses link `libmtl`, so the filter holds all of `lib/**` and `include/**`. `fuzz-tests-result` always runs and is the check to require.

### Acceptance Tests (`tests/acceptance/`)
E2E framework launching real MTL apps over SSH. Tests do NOT call MTL C API.

Config: `topology_config.yaml` (hosts, PCI BDFs) + `test_config.yaml` (session_id, paths)

Setup: `.github/scripts/setup_acceptance.sh` stage functions must run in the parent shell; `stage_ssh` exports `SSH_KEY` for `stage_configs`.

Setup gotcha: `pkg-config --exists libdpdk` is not enough; warm DPDK setup must also refresh/verify `ldconfig` or RxTxApp can fail with exit 127 and missing `librte_*.so.26`.

Shell gotcha: in `setup_acceptance.sh` (`set -o pipefail`), avoid `producer | grep -q pattern` readiness probes; early `grep -q` exit can SIGPIPE the producer and make a true match look false. Capture output once or use non-early-exit matching.

Validation methods: log parsing, FPS check, MD5 per-frame integrity, EBU LIST compliance (optional)

Markers: `@pytest.mark.smoke`, `@pytest.mark.nightly`, `@pytest.mark.dual`, `@pytest.mark.ptp`

Root required. Depends on `mfd-*` Intel-internal packages for SSH/NIC automation.

For one-shot host preparation (build, hugepages, SSH, NFS, plugin builds, configs), call `.github/scripts/acceptance_setup.sh` (interactive, or `--auto` for scripted use) or the `mtl-acceptance-setup` MCP tools directly — no dedicated agent; the interactive script prompts for NFS/PF/EBU choices at zero token cost.
For invoking pytest itself and triaging failures, use `.github/instructions/mtl-acceptance-tests.instructions.md` — it auto-attaches when working under `tests/acceptance/`.

### RxTxApp (`tests/tools/RxTxApp/`)
Universal JSON-driven test vehicle. Max sessions: 180 video, 1024 audio, 180 ancillary (fast metadata included) (each direction).

JSON config directories: `loop_json/`, `audio_json/`, `native_af_xdp_json/`, `kernel_socket_json/`, `rss_json/`, `redundant_json/`

### Key Testing Gotchas
- DPDK can't reinit within a process → noctx pattern for isolated tests
- `cvt_test.cpp` is 4349 lines — add round-trip tests when adding new pixel formats
- Fuzz harnesses are single-packet — multi-packet protocol bugs uncovered
- Test level: CI runs `mandatory`; `all` for thorough local validation
