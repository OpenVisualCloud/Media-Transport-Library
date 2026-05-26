---
name: 'MTL C Coding Rules'
description: 'Mandatory coding rules for MTL C/C++ source. Covers naming, memory, locking, tasklet constraints, data-plane separation, and error handling.'
applyTo: '**/*.c,**/*.h'
---

# MTL C Coding Rules

These rules are **mandatory** for all C source in the MTL codebase. For deep architecture context, see the [knowledge base](../copilot-docs/mtl-knowledge-base.md).

## Language

- Library core (`lib/`): **C99 only**. No C++ constructs.
- Tests (`tests/`): C++ allowed (gtest framework).

## Naming Conventions

| Prefix | Scope | Example |
|--------|-------|---------|
| `mt_` | Core library internals (non-media) | `mt_socket_id()`, `mt_rte_zmalloc()` |
| `mtl_` | Public core API | `mtl_start()`, `mtl_stop()` |
| `st_` | ST2110 base abstractions | `st_frame_fmt_name()` |
| `st20_`/`st22_`/`st30_`/`st40_`/`st41_` | Media session APIs | `st20_tx_create()` |
| `st20p_`/`st22p_`/`st30p_` | Pipeline APIs | `st20p_tx_create()` |
| `tv_`/`rv_` | TX/RX video session internals | `tv_frame_free()` |
| `tx_audio_session_`/`rx_audio_session_` | Audio internals | |
| `tx_ancillary_session_`/`rx_ancillary_session_` | Ancillary internals | |
| `tx_fastmetadata_`/`rx_fastmetadata_` | ST2110-41 internals | |

## Data-Plane vs Control-Plane Separation (Two-World Rule)

**Data-plane code must NEVER call control-plane functions:**
- No `malloc`/`free` — use pre-allocated DPDK hugepage memory
- No `pthread_mutex` — use `rte_spinlock_t` (never sleeps)
- No INFO-level logging — only DEBUG/trace in hot paths
- No `sleep`, no blocking I/O
- Use lock-free rings, zero-copy, polling tasklets

**Control-plane** (session create/destroy, config) may use heap memory, mutexes, POSIX threads.

## Tasklet Constraints

- **Tasklets must never block** — one blocked tasklet starves all others in that scheduler
- Return positive value for "has work", 0 for "idle"
- No malloc, no mutex, no sleep inside tasklet handlers

## Error Handling

- Return 0 for success, negative for error
- Free resources in **reverse allocation order** on failure
- Reject invalid session config **before** allocating scarce resources (queues, mempools)

## Memory Management

- DPDK allocations: `mt_rte_zmalloc()` / `mt_rte_free()` — always specify `socket_id` via `mt_socket_id(impl, port)`
- Frame buffers: `rte_zmalloc_socket()`, not from mempool
- RX frame buffers must be **zero-initialized** (partial frames have zeroed gaps, not garbage)
- NUMA socket mismatch causes 2× DMA latency
- DMA copies must not cross hugepage boundaries
- Mempool names must include `recovery_idx` suffix for uniqueness across recovery cycles

## Locking Rules

- Data-plane: `rte_spinlock_t` (never sleeps)
- Control-plane: `pthread_mutex_t` (may sleep)
- **Lock ordering: manager mutex → session spinlock** (never reverse)
- **Session migration lock ordering: target manager mutex → source manager mutex**
- RX bitmap: use `mt_bitmap_test_and_set()` (atomic, per-packet dedup)

## TX Data Path

- Ring semantics: use `rte_ring_sp_enqueue_bulk()` (all-or-nothing), not `_burst()` — partial enqueue breaks RTP sequence/pacing integrity
- If `rte_eth_tx_burst()` returns fewer than requested: save unsent to `inflight[]`, return immediately, retry next iteration (never discard, never block)
- TX chain mbufs: use `rte_pktmbuf_attach_extbuf()` for zero-copy from app frame buffer
- Cross-page payloads: if payload spans hugepage boundary, memcpy into chain mbuf's data room

## RX Data Path

- DMA threshold: payloads ≥1024 bytes use DMA copy; smaller use CPU memcpy
- Frame completion requires: `recv_size >= frame_size` AND `dma_nb == 0` AND no pending mbuf borrows
- RX slot eviction: if evicting occupied slot, notify app as CORRUPTED frame
- IGMP: send 2 duplicate reports per join (no ACK in IGMP)

## Logging

- Use `dbg()` / `info()` / `warn()` / `err()`
- **Never use `printf`**

## Headers

- Public API: `include/` directory
- Internal headers: `lib/src/` directory

## Formatting

- `clang-format-14` enforced by CI
- Run `./format-coding.sh` before committing
