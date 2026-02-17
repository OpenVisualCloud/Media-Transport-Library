# Media Transport Library (MTL) - Copilot Instructions

## ⚠️ MANDATORY AGENT WORKFLOW — READ THIS FIRST

This file and the `.github/copilot-docs/` directory serve as **persistent long-term memory** for agents working on this repository. You MUST follow these rules on every task:

### Before Starting Any Task
1. **Read this file fully** before writing or modifying any code.
2. **Consult the relevant deep-dive files** in `.github/copilot-docs/` based on the task area (see table below). If your task touches scheduling → read `threading-and-scheduler.md`. If it touches frames or buffers → read `memory-management.md`. If it touches locks or atomics → read `concurrency-and-locking.md`. And so on. When in doubt, read all of them.
3. **Do not guess** about patterns, conventions, or internal APIs. The answers are in these files. Check them before searching the codebase.

### While Working
4. **Fix inconsistencies on sight.** If you discover that anything in `copilot-instructions.md` or any file in `copilot-docs/` is outdated, incorrect, or incomplete compared to the actual codebase — **fix it immediately** as part of your current task. Do not leave known inaccuracies for later.
5. **Save new knowledge.** When you learn something non-obvious about the codebase that is not already documented here (a tricky invariant, an undocumented dependency, a subtle ordering constraint, a useful debugging technique), **write it into the correct `copilot-docs/` file** or into this file if it is cross-cutting. This is not optional.
6. **Keep it compressed.** Use terse, factual statements. Bullet points and tables over prose. No filler. Every token in these files costs context window — only store information that would save a future agent significant time.

### What to Save (and What NOT to)
7. **Save conclusions, not descriptions.** These files should teach *how the project is designed* and *why* — not repeat what's visible in the code. Do NOT list struct fields, variable names, or constants that are obvious from reading headers. DO save:
   - **Design decisions** — why a pattern exists, what problem it solves
   - **Non-obvious connections** — how subsystems depend on each other in ways not apparent from imports
   - **Invariants and ordering constraints** — things that break if violated but aren't enforced by the compiler
   - **Gotchas** — things a maintainer learns the hard way after debugging for hours
   - **Mental models** — the right way to think about a subsystem so the code makes sense
   - **Debugging techniques** — how to diagnose specific classes of problems
8. **Think like a senior maintainer writing notes for a new team member.** The reader can read code. They need to know what the code *doesn't tell them*.

### File Routing Guide
| If your task involves… | Read this file |
|------------------------|----------------|
| Threads, schedulers, tasklets, lcore, sleep | `threading-and-scheduler.md` |
| malloc, free, mempools, frames, DMA, NUMA, ASAN | `memory-management.md` |
| Locks, mutexes, spinlocks, atomics, race conditions | `concurrency-and-locking.md` |
| Pacing, PTP, TSC, rate limiter, latency, performance | `pacing-timing-performance.md` |
| Session create/destroy, pipelines, TX/RX data flow, RTCP | `session-lifecycle-dataflow.md` |
| DPDK APIs, mbufs, queues, flow rules, device init, TM | `dpdk-usage-patterns.md` |
| End-to-end flow, design patterns, config interactions | `architecture-and-design-philosophy.md` |
| Anything else / cross-cutting | This file (`copilot-instructions.md`) |

All deep-dive files live in: `.github/copilot-docs/`

---

## Overview

MTL implements SMPTE ST 2110 for high-throughput, low-latency media transport over IP. DPDK-based with HW pacing support (Intel E810). Supports uncompressed video (ST2110-20), compressed video (ST2110-22), audio (ST2110-30), and ancillary data (ST2110-40).

**DeepWiki Reference**: https://deepwiki.com/OpenVisualCloud/Media-Transport-Library

## Deep-Dive Reference Files

These files contain extended documentation on specific subsystems. Consult them when working in the corresponding areas:

| File | Contents |
|------|----------|
| [.github/copilot-docs/threading-and-scheduler.md](.github/copilot-docs/threading-and-scheduler.md) | All threads, scheduler types, tasklet loop, sleep, lcore allocation, quota |
| [.github/copilot-docs/memory-management.md](.github/copilot-docs/memory-management.md) | Allocation functions, NUMA, mempools, frame buffers, zero-copy, DMA, ASAN |
| [.github/copilot-docs/concurrency-and-locking.md](.github/copilot-docs/concurrency-and-locking.md) | Complete lock inventory, atomics, session get/put pattern, pipeline locking |
| [.github/copilot-docs/pacing-timing-performance.md](.github/copilot-docs/pacing-timing-performance.md) | ST2110-21 pacing, PTP, TSC, rate limiter, stats, performance debugging |
| [.github/copilot-docs/session-lifecycle-dataflow.md](.github/copilot-docs/session-lifecycle-dataflow.md) | Session create/destroy, frame state machines, TX/RX data paths, RTCP, redundancy |
| [.github/copilot-docs/dpdk-usage-patterns.md](.github/copilot-docs/dpdk-usage-patterns.md) | DPDK device init, mbufs, mempools, queues, flow rules, Traffic Manager, EAL |
| [.github/copilot-docs/architecture-and-design-philosophy.md](.github/copilot-docs/architecture-and-design-philosophy.md) | Two-world pattern, end-to-end data journey, resilience philosophy, config interactions |

## Architecture at a Glance

```
┌──────────────────────────────────────────────────────────────────┐
│ Application Layer                                                │
│   Sample apps (RxTxApp)  │  FFmpeg/GStreamer/OBS plugins         │
│   Python/Rust bindings   │  Custom applications                  │
├──────────────────────────────────────────────────────────────────┤
│ Pipeline API Layer (st20p_*, st22p_*, st30p_*)                   │
│   get_frame / put_frame  │  Auto format conversion / codec       │
├──────────────────────────────────────────────────────────────────┤
│ Session API Layer (st20_*, st30_*, st40_*)                       │
│   Frame/Slice/RTP modes  │  Direct packet control                │
├──────────────────────────────────────────────────────────────────┤
│ MTL Core (mtl_init/start/stop/uninit)                            │
│   Scheduler & Tasklets   │  PTP Timing  │  CNI  │  ARP/DHCP     │
├──────────────────────────────────────────────────────────────────┤
│ Data Path / Queue Management                                     │
│   Dedicated queues  │  Shared RX/TX (RSQ/TSQ)  │  Shared RSS     │
├──────────────────────────────────────────────────────────────────┤
│ Network Backend Abstraction                                      │
│   DPDK PMD  │  AF_XDP (native/dpdk)  │  Kernel Socket  │  RDMA  │
└──────────────────────────────────────────────────────────────────┘
```

## Key Files to Reference

### Public API Headers
- `include/mtl_api.h` — Core MTL API (`mtl_init`, `mtl_start`, types, flags)
- `include/st20_api.h` — ST2110-20 video session API
- `include/st30_api.h` — ST2110-30 audio session API
- `include/st40_api.h` — ST2110-40 ancillary data API
- `include/st_pipeline_api.h` — Pipeline API (st20p/st22p/st30p get/put frame)
- `include/mtl_sch_api.h` — Scheduler API (create/register tasklets)

### Core Internal
- `lib/src/mt_main.h` — Central `mtl_main_impl` struct, all subsystem types, lock definitions
- `lib/src/mt_main.c` — `mtl_init()`/`mtl_uninit()` implementation
- `lib/src/mt_sch.c` / `mt_sch.h` — Scheduler/tasklet system
- `lib/src/mt_mem.h` — Memory allocation wrappers
- `lib/src/mt_ptp.c` — PTP IEEE 1588 implementation
- `lib/src/mt_cni.c` — Control Network Interface (ARP, PTP, DHCP packet processing)
- `lib/src/mt_rtcp.c` / `mt_rtcp.h` — RTCP NACK-based retransmission

### ST2110 Session Implementation
- `lib/src/st2110/st_header.h` — All internal structs: `st_frame_trans`, pacing, slot, session limits
- `lib/src/st2110/st_fmt.c` — Pixel groups, fps, bandwidth calculation
- `lib/src/st2110/st_tx_video_session.c` — TX video: frame alloc, packet build, zero-copy
- `lib/src/st2110/st_rx_video_session.c` — RX video: slot management, packet assembly
- `lib/src/st2110/st_video_transmitter.c` — TX transmitter: ring dequeue, pacing, NIC burst
- `lib/src/st2110/st_tx_audio_session.c` / `st_rx_audio_session.c` — Audio sessions
- `lib/src/st2110/st_tx_ancillary_session.c` / `st_rx_ancillary_session.c` — Ancillary

### Pipeline Layer
- `lib/src/st2110/pipeline/st20_pipeline_tx.c` — TX pipeline: frame state machine, converter integration
- `lib/src/st2110/pipeline/st20_pipeline_rx.c` — RX pipeline: frame assembly + conversion
- `lib/src/st2110/pipeline/st22_pipeline_tx.c` / `st22_pipeline_rx.c` — Compressed video pipeline

### Data Path
- `lib/src/datapath/mt_queue.c` — Queue abstraction (get/put, burst)
- `lib/src/datapath/mt_shared_queue.c` — Shared RX/TX queue implementation
- `lib/src/datapath/mt_shared_rss.c` — Shared RSS for NICs without flow director

### Network Backends
- `lib/src/dev/mt_dev.c` — Device init, driver detection, queue setup
- `lib/src/dev/mt_af_xdp.c` — AF_XDP backend (UMEM, XSK, zero-copy)

### Samples & Tests
- `app/sample/` — Working code for each API (pipeline, ext_frame, forward)
- `tests/integration_tests/` — gtest patterns for all session types

## Naming Conventions

| Prefix | Scope | Example |
|--------|-------|---------|
| `mtl_*` | Public API | `mtl_init`, `mtl_start`, `mtl_uninit` |
| `mt_*` | Core internal | `mt_rte_zmalloc_socket`, `mt_sch_get_quota` |
| `st_*` / `st20_*` | ST2110 public + internal | `st20_tx_create`, `st_frame_trans` |
| `tv_*` / `rv_*` | TX/RX video (static/internal) | `tv_ops_check`, `rv_handle_rtp_pkt` |
| `ta_*` / `ra_*` | TX/RX audio | `ta_tasklet_handler` |
| `atx_*` / `arx_*` | TX/RX ancillary | `atx_tasklet_handler` |
| `st20p_*` / `st22p_*` | Pipeline API | `st20p_tx_get_frame` |
| `mudp_*` | Custom UDP stack | `mudp_sendto` |
| `mufd_*` | UDP file descriptor layer | `mufd_socket` |
| `*_impl` | Internal implementation struct | `st_tx_video_session_impl` |
| `*_mgr` | Manager struct | `st_tx_video_sessions_mgr` |
| `*_ctx` | Pipeline context | `st20p_tx_ctx` |

**Lifecycle naming**: `*_init()` / `*_uinit()` (note: "uinit" not "uninit"), `*_attach()` / `*_detach()`

## Critical Rules

### Error Handling
- Return negative errno: `-EINVAL`, `-ENOMEM`, `-EIO`, `-EBUSY`
- Use goto-based cleanup for multi-step init
- Always log with `__func__` and session `idx`:
  ```c
  err("%s(%d), msg\n", __func__, s->idx);
  ```

### Memory (see `lib/src/mt_mem.h` and [memory-management.md](.github/copilot-docs/memory-management.md))
- `mt_rte_zmalloc_socket(size, socket_id)` for DMA/NUMA-aware alloc
- `MT_SAFE_FREE(obj, mt_rte_free)` — sets obj=NULL after free
- Always pass `socket_id` for NUMA locality
- Never use `malloc()` for DMA-accessible buffers
- Never free frames while `sh_info.refcnt > 0` (NIC may be DMA-reading)

### Logging (see `lib/src/mt_log.h`)
- `dbg()` (debug only), `info()`, `warn()`, `err()`, `critical()`
- `*_once()` variants for single-fire logs (avoid log spam in hot path)
- Format: `err("%s(%d), what happened\n", __func__, s->idx)`

### Tasklets (CRITICAL — see [threading-and-scheduler.md](.github/copilot-docs/threading-and-scheduler.md))
- **Never block** in tasklet callbacks — runs in shared polling thread
- Use `rte_spinlock` (not mutex) for quick critical sections
- Use `rte_ring` for async thread communication
- Return `MTL_TASKLET_ALL_DONE` (0) when no work, positive when pending
- Session access: `*_session_try_get()` → work → `*_session_put()` (spinlock pattern)
- Advice: set `ops.advice_sleep_us` to guide scheduler sleep optimization

### Concurrency (see [concurrency-and-locking.md](.github/copilot-docs/concurrency-and-locking.md))
- `rte_spinlock_t` — hot path (tasklet context): session get/put, shared queue
- `pthread_mutex_t` — control plane (session create/destroy, queue alloc)
- Never mix: don't hold spinlock across blocking call; don't use mutex in tasklet
- Pipeline uses `pthread_mutex_t` because ops run in app thread, not tasklet

### Frames (`st_frame_trans` in `st_header.h`)
- `refcnt` via `rte_atomic32_t`: 0 = free
- Acquire: find frame with `refcnt==0`, then `rte_atomic32_inc()`
- Release: `rte_atomic32_dec()`
- Flags: `ST_FT_FLAG_EXT` (external), `ST_FT_FLAG_RTE_MALLOC`, `ST_FT_FLAG_GPU_MALLOC`
- Zero-copy TX: header mbuf + `rte_pktmbuf_attach_extbuf()` + `rte_pktmbuf_chain()`
- Frame free callback: `sh_info.free_cb` fires when all NIC DMA references are released

### Pacing (see [pacing-timing-performance.md](.github/copilot-docs/pacing-timing-performance.md))
- `trs` = time between packets (ns)
- `reactive` = active_lines/total_lines (e.g., 1080/1125) for blanking interval
- Modes: `ST21_TX_PACING_WAY_RL` (HW rate limit, preferred), `ST21_TX_PACING_WAY_TSC` (SW)
- Rate limiter BPS = wire_rate × reactive ratio
- Warm-up padding sent before first data packet to prime HW rate limiter

## Threading Model Summary

| Thread Type | Count | Purpose |
|-------------|-------|---------|
| Scheduler (lcore or pthread) | Up to 18 | Tasklet polling loop (builder, transmitter, receiver) |
| TSC Calibration | 1 | Boot-time TSC frequency measurement |
| Admin | 1 | Watchdog, reset detection |
| Statistics | 1 | Periodic stats dump |
| CNI RX | 1 | System packet handling (ARP, PTP, DHCP) |
| Socket TX/RX | Up to 4 each | Kernel socket backend data path |
| SRSS | 1 per instance | Shared RSS polling (NICs without flow director) |

## Session Lifecycle

```
create() → ops_check() → sch_get_quota() → mgr_attach() → init()
  ├── alloc_frames()
  ├── init_hdr()
  ├── init_pacing()
  ├── txq_get() / rxq_get()
  └── create mempools, rings

free() → mgr_detach() → uinit()
  ├── txq_put() / rxq_put()
  ├── free mempools, rings
  ├── free_frames()
  └── sch_put()
```

## Pipeline Frame State Machines

### TX Pipeline (st20p_tx)
```
FREE ──get_frame()──→ IN_USER ──put_frame()──→ READY
  ↑                                               │
  │ (frame_done)                    (converter picks up)
  │                                               ▼
IN_TRANSMITTING ◄───(newest)─── CONVERTED ◄── IN_CONVERTING
  │
  └── (late drop) → FREE
```

### RX Pipeline (st20p_rx)
```
FREE ──(transport delivers)──→ READY ──(convert)──→ IN_CONVERTING
                                 │                       │
                          (no convert needed)      (done converting)
                                 ▼                       ▼
                              IN_USER ◄────────── CONVERTED
                                 │
                          put_frame()
                                 ▼
                               FREE
```

## Validation Checklist (`*_ops_check()` functions)
- `num_port`: 1-2 (`MTL_SESSION_PORT_MAX`)
- `payload_type`: 0-127 (RFC3550), 0 = disable
- `framebuff_cnt`: 2-8 for video
- IP: not all zeros, multicast = 224.x-239.x
- Redundant ports must have different IPs
- Check required callbacks based on `type` field (frame, slice, RTP)
- `transport_fmt` must be valid ST20 format
- Pipeline: `input_fmt`/`output_fmt` must be supported by available converter plugin

## Network Backend Selection

Backend is determined by port name format in `mtl_init_params`:

| Port Name | PMD Type | Backend |
|-----------|----------|---------|
| `0000:af:00.0` | `MTL_PMD_DPDK_USER` | DPDK PMD (VFIO/UIO) |
| `af_xdp:eth0` | `MTL_PMD_NATIVE_AF_XDP` | Native AF_XDP |
| `dpdk_af_xdp:eth0` | `MTL_PMD_DPDK_AF_XDP` | DPDK AF_XDP |
| `kernel:eth0` | `MTL_PMD_KERNEL_SOCKET` | Kernel socket |
| `rdma_ud:eth0` | `MTL_PMD_RDMA_UD` | RDMA UD (experimental) |

## Queue Architecture

| Queue Type | When Used | Backend | Characteristics |
|------------|-----------|---------|-----------------|
| Dedicated | Default for video sessions | DPDK, AF_XDP, RDMA | 1:1 mapping, no contention |
| Shared TX (TSQ) | `MTL_FLAG_SHARED_TX_QUEUE` | All | Software queue sharing, `rte_spinlock` TX serialization |
| Shared RX (RSQ) | `MTL_FLAG_SHARED_RX_QUEUE` | All | Software flow matching, per-session `rte_ring` |
| Shared RSS | NICs without flow director | DPDK, AF_XDP | Hardware RSS + software dispatch by UDP port hash |
| System | Control traffic | All | ARP, PTP, management packets |

## Type Patterns

### Handles (opaque pointers)
```c
typedef struct st_tx_video_session_handle_impl* st20_tx_handle;
typedef struct mtl_main_impl* mtl_handle;
```

### Enums
- Start at 0, end with `_MAX`: `enum st_fps { ST_FPS_P59_94 = 0, ..., ST_FPS_MAX };`

### Flags (use bit macros)
```c
#define ST20_TX_FLAG_EXT_FRAME (MTL_BIT32(2))
```

### Common Struct Fields
- `int idx` — session index for logging
- `int socket_id` — NUMA socket
- `struct mtl_main_impl* impl` — parent context
- `rte_atomic32_t refcnt` — reference count
- `stat_*` prefix for statistics fields

## Wire Format Structs
```c
MTL_PACK(struct st_rfc3550_rtp_hdr { ... });  // Packed for network
struct st_rfc4175_video_hdr { ... } __attribute__((__packed__)) __rte_aligned(2);
```

## Handle Validation (at API entry)
```c
if (impl->type != MT_HANDLE_MAIN) return -EIO;  // See mt_handle_type in mt_main.h
```

## Adding New Session Type
1. Define ops struct in `include/st**_api.h`
2. Create `st_tx/rx_*_session.c/h` with `*_impl` struct
3. Add `*_ops_check()` validation function
4. Add session get/put spinlock functions (see pattern in `st_tx_video_session.h`)
5. Register tasklet in `*_sessions_sch_init()`
6. Add manager to `mtl_sch_impl` in `mt_main.h`
7. Add session count atomic in `mtl_main_impl`
8. Add manager mutex in `mtl_sch_impl`

## RTCP Retransmission
- TX: circular buffer stores recent packets; on NACK, deep-copies and retransmits
- RX: bitmap tracks received seq numbers; periodic scan detects gaps → NACK
- Custom RTCP format: type 204, name "IMTL", FCI ranges (start + follow count)
- Enable with `ST20_TX_FLAG_ENABLE_RTCP` / `ST20_RX_FLAG_ENABLE_RTCP`

## Custom UDP Stack
Three-layer architecture for high-performance UDP:
- **MUDP** (`lib/src/udp/udp_main.c`): Core socket implementation using DPDK mbufs
- **UFD** (`lib/src/udp/ufd_main.c`): File descriptor abstraction over MUDP
- **LD_PRELOAD** (`ld_preload/udp/udp_preload.c`): Transparent interception of standard socket calls
- Supports GSO, multicast, poll/select/epoll, rate limiting, multi-process

## Plugin System
- Converter plugins (`st20_converter_dev`): format conversion (YUV ↔ RGB, endian swap)
- Encoder plugins (`st22_encoder_dev`): JPEG XS, H.264, H.265 encoding
- Decoder plugins (`st22_decoder_dev`): corresponding decoders
- Device types: `ST_PLUGIN_DEVICE_CPU`, `ST_PLUGIN_DEVICE_GPU`, `ST_PLUGIN_DEVICE_FPGA`
- Loaded dynamically via `st_plugin_*` API, matched by format capability flags

## Ecosystem Integration
- **FFmpeg Plugin** (`ecosystem/ffmpeg_plugin/`): `mtl_st20p`, `mtl_st22p`, `mtl_st30p` formats
- **GStreamer Plugin** (`ecosystem/gstreamer_plugin/`): `mtl_st20p_tx/rx`, `mtl_st30p_tx/rx` elements
- **OBS Plugin** (`ecosystem/obs_plugin/`): Live streaming integration
- **Python Bindings** (`python/`): Complete API wrapper
- **Rust Bindings** (`rust/`): Safe wrappers with memory management

## Quick Reference

| Constant | Value | Location |
|----------|-------|----------|
| `MTL_SESSION_PORT_MAX` | 2 | `mtl_api.h` |
| `MTL_PORT_MAX` | 8 | `mtl_api.h` |
| `ST_MAX_NAME_LEN` | 32 | `st_header.h` |
| `MTL_PKT_MAX_RTP_BYTES` | 1352 | `mtl_api.h` |
| `ST_SESSION_MAX_BULK` | 4 | `st_header.h` |
| `MT_MAX_SCH_NUM` | 18 | `mt_main.h` |
| `MT_MAX_RL_ITEMS` | 128 | `mt_main.h` |
| `MT_DMA_MAX_SESSIONS` | 16 | `mt_main.h` |
| `MT_MAP_MAX_ITEMS` | 256 | `mt_main.h` |
| `ST_QUOTA_TX1080P_PER_SCH` | 12 | `st_header.h` |
| `ST_QUOTA_RX1080P_PER_SCH` | 12 | `st_header.h` |
| `ST_SCH_MAX_TX_VIDEO_SESSIONS` | 60 | `st_header.h` |
| `ST_SCH_MAX_RX_VIDEO_SESSIONS` | 60 | `st_header.h` |
| `ST_TX_VIDEO_SESSIONS_RING_SIZE` | 512 | `st_header.h` |
| `ST_VIDEO_BPM_SIZE` | 1260 | `st_header.h` |

## Build & Format
```bash
./build.sh              # Release build
./build.sh debug        # Debug build (-O0 -g)
./build.sh debugonly    # Debug build (lib only)
./build.sh debugasan    # Debug + ASAN
./format-coding.sh      # Format code (requires clang-format-14)
```

**IMPORTANT**: Always run `./build.sh` to verify your changes compile successfully and `./format-coding.sh` to fix code formatting before committing. CI will reject improperly formatted code.

## Commit Messages (Conventional Commits)
Format: `<Type>: <description>` — Type is **capitalized**. See `doc/coding_standard.md`.

| Type | Use |
|------|-----|
| `Feat` / `Add` | New feature |
| `Fix` | Bugfix |
| `Refactor` | Code change (no bugfix/feature) |
| `Docs` | Documentation only |
| `Test` | Tests |
| `Perf` | Performance improvement |
| `Build` | Build system/dependencies |
| `Ci` | CI configuration |
| `Style` | Formatting (no code change) |
