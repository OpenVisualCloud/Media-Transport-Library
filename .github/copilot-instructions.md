# Media Transport Library (MTL) - Copilot Instructions

## Overview
MTL implements SMPTE ST 2110 for high-throughput, low-latency media over IP. DPDK-based with HW pacing support (Intel E810).

## Key Files to Reference
- **Public API**: `include/mtl_api.h`, `include/st20_api.h`, `include/st_pipeline_api.h`
- **Constants**: `lib/src/st2110/st_header.h`, `lib/src/mt_main.h`
- **Formats/Timing**: `lib/src/st2110/st_fmt.c` (pixel groups, fps, bandwidth calc)
- **Session impl**: `lib/src/st2110/st_tx_video_session.c`, `st_rx_video_session.c`
- **Samples**: `app/sample/` (working code for each API)
- **Tests**: `tests/integration_tests/` (gtest patterns)

## Naming Conventions
| Prefix | Scope | Example |
|--------|-------|---------|
| `mtl_*` | Public API | `mtl_init`, `mtl_start` |
| `mt_*` | Core internal | `mt_rte_zmalloc_socket` |
| `st_*` / `st20_*` | ST2110 | `st20_tx_create` |
| `tv_*` / `rv_*` | TX/RX video (static) | `tv_ops_check` |
| `ta_*` / `ra_*` | TX/RX audio | |
| `*_impl` | Internal struct | `st_tx_video_session_impl` |
| `*_mgr` | Manager struct | `st_tx_video_sessions_mgr` |

**Lifecycle**: `*_init()` / `*_uinit()` (note: "uinit" not "uninit"), `*_attach()` / `*_detach()`

## Critical Rules

### Error Handling
- Return negative errno: `-EINVAL`, `-ENOMEM`, `-EIO`, `-EBUSY`
- Use goto-based cleanup for multi-step init
- Always log with `__func__` and session `idx`: `err("%s(%d), msg\n", __func__, s->idx)`

### Memory (see `lib/src/mt_mem.h`)
- `mt_rte_zmalloc_socket(size, socket_id)` for DMA/NUMA-aware alloc
- `MT_SAFE_FREE(obj, mt_rte_free)` sets obj=NULL after free
- Always pass `socket_id` for NUMA locality

### Logging (see `lib/src/mt_log.h`)
- `dbg()` (debug only), `info()`, `warn()`, `err()`, `critical()`
- `*_once()` variants for single-fire logs
- Format: `err("%s(%d), what happened\n", __func__, s->idx)`

### Tasklets (CRITICAL)
- **Never block** in tasklet callbacks - runs in shared polling thread
- Use `rte_spinlock` (not mutex) for quick critical sections
- Use `rte_ring` for async thread communication
- Session access: `*_session_get()` → work → `*_session_put()` (spinlock pattern)

### Frames (`st_frame_trans` in `st_header.h`)
- `refcnt` via `rte_atomic32_t`: 0 = free
- Acquire: find frame with `refcnt==0`, then `rte_atomic32_inc()`
- Release: `rte_atomic32_dec()`
- Flags: `ST_FT_FLAG_EXT` (external), `ST_FT_FLAG_RTE_MALLOC`, `ST_FT_FLAG_GPU_MALLOC`

### Zero-Copy TX Packets
- Header mbuf from `mbuf_mempool_hdr` + payload mbuf via `rte_pktmbuf_attach_extbuf()`
- Chain with `rte_pktmbuf_chain()` - no copy of frame data

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
- `int idx` - session index for logging
- `int socket_id` - NUMA socket
- `struct mtl_main_impl* impl` - parent context
- `rte_atomic32_t refcnt` - reference count
- `stat_*` prefix for statistics fields

## Session Lifecycle
```text
create() → ops_check() → zmalloc_socket() → sch_get_quota() → mgr_attach() → attach()
free()   → mgr_detach() → detach() → uinit() → sch_put() → rte_free()
```

## Validation (see `*_ops_check()` functions)
- `num_port`: 1-2 (MTL_SESSION_PORT_MAX)
- `payload_type`: 0-127 (RFC3550), 0=disable
- `framebuff_cnt`: 2-8 for video
- IP: not all zeros, multicast=224.x-239.x
- Redundant ports must have different IPs
- Check required callbacks based on `type` field

## Pacing (see `st_tx_video_pacing` in `st_header.h`)
- `trs` = time between packets (ns)
- `reactive` = 1080/1125 for blanking interval
- Modes: `ST21_TX_PACING_WAY_RL` (HW rate limit), `ST21_TX_PACING_WAY_TSC` (SW)

## Adding New Session Type
1. Define ops in `include/st**_api.h`
2. Create `st_tx/rx_*_session.c/h` with `*_impl` struct
3. Add `*_ops_check()` validation
4. Register tasklet in `*_sessions_sch_init()`
5. Add manager to `mtl_sch_impl` in `mt_main.h`
6. Add session count atomic in `mtl_main_impl`

## Wire Format Structs
```c
MTL_PACK(struct st_rfc3550_rtp_hdr { ... });  // Packed for network
struct st_rfc4175_video_hdr { ... } __attribute__((__packed__)) __rte_aligned(2);
```

## Handle Validation (at API entry)
```c
if (impl->type != MT_HANDLE_MAIN) return -EIO;  // See mt_handle_type in mt_main.h
```

## Quick Reference
| Constant | Value | Location |
|----------|-------|----------|
| MTL_SESSION_PORT_MAX | 2 | mtl_api.h |
| ST_MAX_NAME_LEN | 32 | st_header.h |
| MTL_PKT_MAX_RTP_BYTES | 1352 | mtl_api.h |
| ST_SESSION_MAX_BULK | 4 | st_header.h |

## Build & Format
```bash
./build.sh              # Release
./build.sh debugonly    # Debug
./format-coding.sh      # Format code (requires clang-format-14)
```

**IMPORTANT**: Always run `./build.sh` to verify your changes compile successfully and `./format-coding.sh` to fix code formatting before committing. CI will reject improperly formatted code.

## Commit Messages (Conventional Commits)
Format: `<Type>: <description>` — Type is capitalized. See `doc/coding_standard.md`.

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
