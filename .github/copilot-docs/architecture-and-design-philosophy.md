# Architecture & Design Philosophy — Cross-Cutting Knowledge

## The "Two-World" Pattern

The most pervasive design pattern in MTL is a strict **two-world split** between data plane and control plane. It shows up everywhere:

| Domain | Data Plane | Control Plane |
|--------|-----------|---------------|
| **Locking** | `rte_spinlock_t` (never block) | `pthread_mutex_t` (can block, has condvar) |
| **Memory** | DPDK hugepages (DMA-accessible, NUMA) | Regular heap (`malloc`) |
| **Threading** | Tasklets in scheduler loop (cooperative) | App threads / admin thread (preemptive) |
| **Schedulers** | Lcore-pinned (deterministic) | Pthread mode (flexible) |
| **Queues** | Dedicated per-session (zero contention) | Shared queues (resource-efficient) |

**Why this exists**: Media transport has nanosecond timing requirements in the hot path but only millisecond requirements for setup/teardown. Mixing the two destroys performance. Every time you write code, first ask: "Am I in data plane or control plane?" — then use the right primitives.

## End-to-End Data Journey

### TX: Application Frame → Wire

```text
App thread:        get_frame() → write pixels → put_frame()
                                                    │
Pipeline layer:    state: IN_USER → READY          │
                   optional: → IN_CONVERTING → CONVERTED
                                                    │
Session layer:     Builder tasklet picks newest READY/CONVERTED frame
                   For each packet in frame:
                     Build header mbuf (62 bytes)
                     attach_extbuf → payload points into frame (zero-copy)
                     chain header + payload
                     enqueue to rte_ring
                                                    │
Transport layer:   Transmitter tasklet dequeues from ring
                   Check pacing: is it time to send?
                     HW RL: NIC drains at configured rate
                     SW TSC: compare TSC against schedule, wait if early
                   rte_eth_tx_burst() → packets to NIC
                                                    │
NIC:               DMA reads header + payload → wire
                   Completion → sh_info.free_cb → frame refcnt--
                   When refcnt == 0: frame back to FREE
```

**Key insight**: The frame data is NEVER copied in the normal TX path. It flows from the application's buffer directly to the NIC via DMA. The only thing built per-packet is the 62-byte header.

### RX: Wire → Application Frame

```text
NIC:               Receives packet → writes into RX mempool mbuf
                                                    │
Transport layer:   Receiver tasklet: rte_eth_rx_burst()
                   Parse RTP header → extract timestamp, seq_id
                   Find or create slot by timestamp
                   Calculate offset in frame from RTP line/offset fields
                   memcpy payload into frame buffer at offset
                   (or DMA copy if DMA engine available)
                   Update bitmap: mark this packet received
                   Free RX mbuf back to mempool
                                                    │
Session layer:     When bitmap is complete → notify_frame_ready()
                                                    │
Pipeline layer:    state: READY → optional IN_CONVERTING → IN_USER
                   App: get_frame() → read pixels → put_frame() → FREE
```

**Key asymmetry**: TX is zero-copy (extbuf pointing into frame), but RX must copy (packets arrive in random order and must be placed at calculated offsets). The DMA copy engine offloads this CPU cost when available.

## Resilience: Redundancy, Not Recovery

MTL has **no automatic error recovery**. No reconnect, no session restart, no NIC failover. This is intentional:

- **Link failure**: Sessions on a dead NIC simply stop. The application must detect and react.
- **Packet loss**: Handled by RTCP retransmission (video only, optional) or redundancy (dual-path)
- **NIC reset**: Admin thread sets a cooperative flag; sessions check it and pause. No automatic restart.
- **Application timeout**: `mtl_abort()` sets a flag but doesn't clean up — app must call `mtl_uninit()`.

**The design philosophy**: In broadcast environments, redundancy is the standard resilience mechanism (ST2110-7). Rather than building complex recovery logic that might do the wrong thing, MTL provides the building blocks (dual-port, RTCP, bitmap tracking) and lets the application decide the policy.

## Why Video Is Complex But Audio Is Simple

Every media type (video, audio, ancillary, fast metadata/ST2110-41) follows the **same structural skeleton**: session struct, manager, tasklet, ops_check, attach/detach, get/put. ST2110-41 fast metadata has its own API (`st41_api.h`), session types, and RTP header format (`st41_rtp_hdr`, payload type 115). But video dominates the codebase complexity because:

| Concern | Video | Audio/Ancillary |
|---------|-------|-----------------|
| **Bandwidth** | 5-12 Gbps per stream | ~1-50 Mbps |
| **Packets/frame** | ~4,500 (1080p60) | 1-8 |
| **Pacing** | Mandatory (ST2110-21), sub-µs | Frame-boundary only, relaxed |
| **Assembly** | Slot-based, bitmap, multi-frame | Single-buffer, sequential |
| **Zero-copy** | Critical for performance | Not needed (small payloads) |
| **Redundancy merge** | Packet-level bitmap merge | Timestamp-based dedup |
| **RTCP** | Full retransmission support | None |

**The design lesson**: Don't over-engineer audio/ancillary sessions by copying video's complexity. The skeleton is shared but the complexity is video-specific.

## Cooperative Everything

MTL avoids preemptive mechanisms wherever possible:

- **Tasklets**: Cooperative scheduling — no preemption, no priority, just "did work / no work"
- **Session get/put**: trylock — if contended, skip and retry next iteration (microseconds later)
- **Port reset**: Flag-based — sessions cooperatively check and pause
- **Sleep/wake**: Advisory `advice_sleep_us` — scheduler and tasklets cooperate on power management
- **Frame dropping**: Newest-first policy — if app is slow, old frames are silently dropped
- **Rate limiting**: Hardware paces; CPU just enqueues — no software timing loops needed

**Why cooperative?** Because preemptive mechanisms (mutexes, priority inversion, thread preemption) add jitter. In a system where 3.7µs packet spacing matters, a single context switch (1-10µs on Linux) can cause a timing violation. Cooperative patterns busy-poll instead, trading CPU usage for deterministic latency.

## The Pipeline-Session Duality

Understanding when the pipeline is "worth it" vs using raw sessions:

**Use Pipeline when**: You want simple frame I/O, need format conversion, want blocking `get_frame`, or are integrating with FFmpeg/GStreamer where the frame model is natural.

**Use Raw Session when**: You need per-packet control (RTP mode), want to manage frame memory yourself (ext_frame), need slice-level access for ultra-low-latency encoding, or are building a protocol bridge.

**The trade-off**: Pipeline adds one level of frame copying (if conversion needed) and one mutex per operation. For most use cases this overhead is negligible. For 4K120 or multi-stream, every copy matters.

## Configuration Interactions That Bite

- `MTL_FLAG_TASKLET_SLEEP` works in **both** lcore and pthread mode — lcores are pthreads under DPDK, so `pthread_cond_timedwait` works in both. No dependency on `MTL_FLAG_TASKLET_THREAD`
- `MTL_FLAG_SHARED_TX_QUEUE` adds a spinlock per TX burst — fine for few sessions, kills throughput at scale
- `MTL_FLAG_DEV_AUTO_START_STOP` changes the lifecycle model: `mtl_start()`/`mtl_stop()` become no-ops, device is started during `mtl_init()`
- `num_port > 1` with different NIC models forces redundant port to TSC pacing even if primary has HW RL
- `framebuff_cnt` must be ≥ 2 (double-buffer minimum) but more isn't always better — each frame consumes ~8MB (1080p) of hugepage memory

## What Makes a Good MTL Session Configuration

The "golden path" for a typical 1080p60 TX session:
1. Use pipeline API (`st20p_tx_create`) unless you need packet-level control
2. Set `framebuff_cnt = 3` (triple-buffer: one in app, one building, one transmitting)
3. Let MTL allocate frames (don't use ext_frame unless you have a reason)
4. Enable HW pacing (default on E810)
5. Use dedicated queues (don't set `MTL_FLAG_SHARED_TX_QUEUE` unless queues are scarce)
6. Match NUMA: ensure the thread calling `put_frame` runs on the same NUMA node as the NIC

Deviating from any of these requires understanding why the default exists.

## Naming Conventions

| Prefix | Scope | Example |
|--------|-------|--------|
| `mtl_*` | Public API | `mtl_init`, `mtl_start`, `mtl_uninit` |
| `mt_*` | Core internal | `mt_rte_zmalloc_socket`, `mt_sch_get_quota` |
| `st_*` / `st20_*` | ST2110 public + internal | `st20_tx_create`, `st_frame_trans` |
| `tv_*` / `rv_*` | TX/RX video (static/internal) | `tv_ops_check`, `rv_handle_rtp_pkt` |
| `ta_*` / `ra_*` | TX/RX audio | `ta_tasklet_handler` |
| `atx_*` / `arx_*` | TX/RX ancillary | `atx_tasklet_handler` |
| `tx_fastmetadata_*` / `rx_fastmetadata_*` | TX/RX fast metadata (ST41) | `tx_fastmetadata_session_build_packet` |
| `st20p_*` / `st22p_*` / `st30p_*` | Pipeline API | `st20p_tx_get_frame` |
| `mudp_*` / `mufd_*` | Custom UDP stack / FD layer | `mudp_sendto`, `mufd_socket` |
| `*_impl` / `*_mgr` / `*_ctx` | Impl struct / Manager / Pipeline context | `st_tx_video_session_impl` |

**Lifecycle gotcha**: `*_init()` / `*_uinit()` — note "uinit" NOT "uninit". Also `*_attach()` / `*_detach()`.

## Coding Rules

- Return negative errno: `-EINVAL`, `-ENOMEM`, `-EIO`, `-EBUSY`
- Use goto-based cleanup for multi-step init
- Always log with `__func__` and session `idx`: `err("%s(%d), msg\n", __func__, s->idx);`
- Logging: `dbg()`, `info()`, `warn()`, `err()`, `critical()` — use `*_once()` variants in hot path
- Memory: `mt_rte_zmalloc_socket(size, socket_id)` for DMA/NUMA-aware alloc — never raw `rte_malloc` or `malloc` for NIC-touching data
- `MT_SAFE_FREE(obj, mt_rte_free)` — sets obj=NULL after free
