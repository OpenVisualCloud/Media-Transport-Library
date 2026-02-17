# Memory Management — Design Knowledge

## The Two-World Model

MTL operates in two memory worlds simultaneously:
1. **DPDK hugepage memory** — physically contiguous, DMA-accessible, NUMA-pinned. Used for anything the NIC will touch: mbufs, frame buffers, mempools, rings.
2. **Regular heap memory** — for control structures, configuration, strings, non-DMA metadata.

**The rule**: If data flows to/from the NIC, it must be in hugepage memory. If it's purely CPU-side bookkeeping, use the heap.

All `mt_rte_*` functions wrap DPDK's `rte_*` allocators and add ASAN poisoning/unpoisoning for debug builds. Never call `rte_malloc` directly — always use the `mt_` wrappers.

## Why NUMA Matters

Every MTL allocation that touches the data path takes a `socket_id` parameter. This is the NUMA socket where the memory should be physically allocated. The NIC is attached to a specific NUMA node — allocating buffers on the wrong node means every DMA operation crosses the inter-socket bus, which can halve throughput.

**How to get socket_id**: `mt_socket_id(impl, port)` returns the NUMA node for a given port. Always pass this through — never hardcode 0.

**The fallback**: If a NUMA-local allocation fails (not enough hugepage memory on that node), the code falls back to NUMA 0. This works but with degraded performance. The fallback is intentional — better to run slower than to fail entirely.

## Frame Buffer Ownership Model

The hardest memory question in MTL: **who owns a frame buffer and when?**

### TX Path Ownership Chain
```
App writes frame → App owns
put_frame()      → MTL owns
Builder reads    → MTL owns, building packets
NIC DMA reads    → NIC owns (sh_info.refcnt > 0)
DMA complete     → sh_info.free_cb fires → MTL owns again → refcnt drops to 0 → FREE
```

**Critical invariant**: You CANNOT free or reuse a frame while `sh_info.refcnt > 0`. The NIC may still be DMA-reading from it. This is why frame recycling checks refcnt, not just a "done" flag.

### RX Path Ownership Chain
```
NIC writes mbufs → NIC/DMA owns
Packet assembly  → MTL owns (copying into frame via memcpy or DMA)
Frame complete   → notify_frame_ready() → App owns
put_frame()      → MTL owns → mark FREE
```

## The Zero-Copy Design

For TX, MTL avoids copying frame data into packet buffers. Instead:
1. Build a **header-only mbuf** (Ethernet + IP + UDP + RTP + video header, ~62 bytes)
2. Create a **payload mbuf** using `rte_pktmbuf_attach_extbuf()` pointing into the frame buffer
3. Chain them with `rte_pktmbuf_chain()`

The NIC scatter-gathers both pieces. The frame data never moves — only pointers. This is WHY frames must be in hugepage memory and why refcnt tracking exists.

**The cross-page problem**: `attach_extbuf` works with a single contiguous memory region. If a packet's payload spans two non-contiguous pages (possible with `MTL_FLAG_IOVA_MAP_BY_ATOMIC_SIZE`), the code falls back to `rte_memcpy` into a regular mbuf. This fallback is in `tv_build_rtp()` — look for the `ST_FT_FLAG_CHAIN_EXTBUF` check.

## Mempools: Why So Many?

Each TX video session creates its own mbuf mempool. Why not share?

1. **NUMA locality** — session's mempool is on the same NUMA node as its NIC
2. **Contention-free allocation** — no cross-core atomic contention on the mempool
3. **Sizing** — each session knows exactly how many mbufs it needs (ring_size × packets_per_frame)
4. **Isolation** — one session exhausting mbufs can't starve others

RX sessions similarly have per-session mempools for packet reception.

## External Frames (`EXT_FRAME`)

Applications can provide their own pre-allocated buffers (e.g., GPU memory, mmap'd files) instead of letting MTL allocate frames. This is the `ext_frame` pattern:
- App calls `st20_tx_get_ext_frame()` providing a buffer address + iova + size
- MTL maps it for DMA and uses it directly — zero memcpy in/out
- App is responsible for buffer lifetime until frame is returned

**Gotcha**: External frame memory must still be DMA-accessible. For GPU memory, the IOVA must be valid for the NIC. This is where `mtl_dma_map()`/`mtl_dma_unmap()` come in.

## The DMA Copy Engine

For RX, reassembling packets into frames can be CPU-intensive (lots of small memcpy). MTL can offload this to Intel IOAT/DSA DMA engines:
- Configured via `mtl_init_params.dma_dev_port`
- Limited to 16 sessions per DMA device
- Only used for video RX (audio/ancillary are too small to benefit)
- Falls back to CPU memcpy if DMA device is unavailable or overloaded

## ASAN Integration

Debug builds (`debugasan`) instrument all `mt_rte_*` allocations:
- `rte_malloc` regions are ASAN-tracked via `ASAN_UNPOISON_MEMORY_REGION`
- After `rte_free`, regions are poisoned via `ASAN_POISON_MEMORY_REGION`
- This catches use-after-free and buffer overruns in hugepage memory, which ASAN normally can't see

## Key Gotchas

1. **Double-free via refcnt race**: If two paths decrement refcnt simultaneously and both see 0, the frame gets freed twice. The atomic decrement-and-test pattern prevents this.
2. **NUMA mismatch is silent**: Nothing warns you if you allocate on the wrong NUMA node. It just runs ~50% slower. Check `socket_id` plumbing carefully.
3. **Mempool exhaustion deadlock**: If a session's mempool runs dry (all mbufs in-flight), the builder stalls. The ring between builder and transmitter provides buffering, but the mempool must be sized for worst-case in-flight packets.
4. **Hugepage fragmentation**: Unlike heap memory, hugepage memory can't be defragmented. Long-running processes with many create/destroy cycles may eventually fail to allocate even when total free memory is sufficient.
