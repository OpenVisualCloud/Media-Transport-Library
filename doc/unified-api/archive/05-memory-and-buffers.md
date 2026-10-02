# 05 — Memory, buffers and data paths

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5) |
| Date | 2026-09-30 |
| Requirements | R-MEM-1…9, R-OBJ-3, R-CMP-4 |
| Research | [R00 review §2–10](research/00-pr1610-design-review.md) (primary), [R04 memory](research/04-memory-buffers.md), [R10 Rivermax §7](research/10-rivermax.md), [R09 libfabric §3](research/09-libfabric.md), [R08 consumers §4](research/08-consumers-ecosystem.md) |
| Reviews | [C1](reviews/C1-realtime-feasibility.md), [C3](reviews/C3-usability-personas.md), [C5 §6](reviews/C5-adversarial-user-review.md) |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

The maintainer's review already made the central decision: **no binary
"library-owned vs user-owned" mode**. Keep several provisioning mechanisms, but expose one
buffer handle, one access-lease state machine and one submit/dequeue contract. This
document fills in the details and maps them onto today's internals.

Revision 3 answers C5 §6. The main changes are:

- a stride larger than the row is a DIRECT layout, as the engine already supports (§4.2);
- an RX unit can feed N TX sessions through a hold count (§5.4);
- `MTL_POOL_DYNAMIC` covers producers that cannot pre-attach a fixed pool (§5.5);
- import rules for alignment, access, NUMA, the region budget and lazy device mapping
  are now stated (§3);
- internal buffers and their memory cost are visible before create (§6.3);
- a TX buffer stays readable after submit (§4.5);
- the teardown order is written down (§3.7).

Items changed in revision 3 are marked **(r3)** and cite `[C5 §x.y]`. Error codes use the
`MTL_E*` names ([07 §5](07-completions-events-and-errors.md), C5-response A5), handles the
A4 types (`mtl_instance_h`, `mtl_lease_h`, …).

## 1. Four separate concepts

```text
  memory region            buffer                    lease                          submission / result
  ─────────────            ──────                    ─────                          ───────────────────
  where bytes live         immutable layout          who may touch it now           what this use means
  VA, length, backing,     planes {region, offset,   TX: FREE / APP_WRITABLE /      media time, cookie,
  NUMA, per-device IOVA    span, stride, rows}       QUEUED / IN_FLIGHT / DONE      holds, overrides → one
  maps, refcount           + optional meta area      RX: FREE / RECEIVING / READY / terminal outcome
  lifetime: app or MTL     lifetime: pool or app     APP_READING / HELD (r3)        per use
```

| Question (review §exec) | Answered by |
|---|---|
| Who allocated the storage? | region `origin`: MTL pool, `mtl_mem_alloc`, `mtl_mem_import` |
| Who owns the storage lifetime? | region owner: session pool (MTL) or the application |
| Who may access it now? | the lease state ([03 §4](03-object-model-and-lifecycle.md)) |
| How was it registered for DMA? | region mappings, per device, eager at import or lazy at attach (§3.5) |
| Is the current path direct, copied or converted? | the session's `pool.data_path` policy, the granted path in `mtl_session_get_info`, and per-unit packet counters (§6.2) |

## 2. Provisioning paths

| Path | Call | Typical user | Phase |
|---|---|---|---|
| Default pool | none: `pool.count` in the session config (0 = the default in [09 §9](09-media-modes-and-backends.md)) | P1, P3, audio/ANC | 1 |
| Explicit MTL allocation | `mtl_mem_alloc(mt, &desc, &region)` → build buffers → attach | apps that want MTL-placed memory shared by several sessions (Rivermax "one registration for many streams" `[R10 §7]`) | 4 |
| Host memory import | `mtl_mem_import(mt, &desc{va, length}, &region)` → buffers → attach | FFmpeg/GStreamer RX pools, shared-memory IPC, MXL grains, GPU pinned host memory (§3.2) | 4 |
| Library pool as a region **(r3)** | `mtl_session_get_pool_region(rx, &region)` → TX buffers over the RX pool → attach (§5.3) | RX → TX forwarding with a library RX pool `[C5 §3.4]` | 4 |
| Library-pool export | the library pool wrapped as the framework's pool (rule in [03 §4.4](03-object-model-and-lifecycle.md), recipe in [11 §6.2](11-abi-compatibility-and-migration.md)); **(r3)** `MTL_SESSION_EXPORT_POOL` forces `MTL_COMPLETE_ALL` and implies `MTL_SESSION_MT_SUBMIT`; `release_buffer` → `mtl_tx_release` only if not submitted `[C5 §7.2, §7.3]` | framework TX, no registration | 4 |
| Dynamic pool **(r3)** | `pool.source = MTL_POOL_DYNAMIC`; `mtl_tx_acquire_dynamic(s, &layout, …)` binds a lease slot to a caller-given layout inside an imported region (§5.5) | moving-cursor arenas, FFmpeg per-frame TX, GStreamer upstream pools in one imported arena `[C5 §6.3, §3.4]` | 4 (moved from 6) |
| Pre-mapped expert import | `desc.iova` supplied, validated against the map table | compatibility with today's `mtl_dma_map` users | later (Q-MEM-3) |
| DMA-BUF / device memory import | `desc.domain = MTL_MEM_DMABUF / MTL_MEM_DEVICE`, fd or device pointer | GPU, FPGA, DPU | later, COPY-only until a direct path exists (Q-MEM-8) |
| RX dynamic provide | `mtl_rx_provide` of an idle buffer | RX consumers that cannot pre-attach | later (Q-MEM-10) |

The export-pool mapping at lease level **(r3)** `[C5 §7.2]`:

- GstBufferPool `acquire_buffer` → `mtl_tx_acquire`;
- `render` → `mtl_tx_submit`, then the wrapper is marked in flight;
- `release_buffer` → `mtl_tx_release` only if the wrapper was not submitted;
- the `TX_RESULT` returns the wrapper to the pool;
- `set_active(FALSE)` → `mtl_session_stop(MTL_STOP_FLUSH)` first, so every result
  arrives.

All paths end in the same `mtl_buffer_h`, the same `mtl_lease_h` and the same runtime verbs.
The difference ends at provisioning. `[R00 §2]`

### 2.1 What v1 does for each real producer (r3)

C5 §6.3 is right that revision 2 listed producers under "import" without saying what each
one gets. "Copies" counts full-unit CPU copies made by the application or MTL before the
NIC, not the packet build of copy-only essences.

| Producer | What the unified API does for it | Copies | Phase |
|---|---|---|---|
| App writes into MTL memory (library pool, P1/P3) | `mtl_tx_acquire` → write → `mtl_tx_submit`; DIRECT for ST20 TX | 0 | 1 |
| Framework that honours the export pool (GStreamer upstream accepts `propose_allocation`; FFmpeg `get_buffer2` supplied by the app) | the export pool above; upstream writes into MTL leases | 0 | 4 |
| GStreamer upstream pool that ignores `propose_allocation` (most decoders, `videotestsrc`) | Phase 1–3: `mtl_lease_copy_in` into a library lease. Phase 4: `MTL_POOL_DYNAMIC` over the upstream pool's arena if it is page-aligned host memory and fits the region budget (§3.4), otherwise the copy | 1, or 0 in Phase 4 | 1 / 4 |
| FFmpeg decoder `AVFrame`s (`av_buffer_pool`, 64 B-aligned, grows lazily) | copy into a library lease; zero-copy only if the app installs a `get_buffer2` that carves frames from one imported page-aligned arena (then DYNAMIC) | 1, or 0 | 1 / 4 |
| FFmpeg TX `AVPacket`s for ST 2110-22 | the codestream is always copied into packets (ST22 forces `tx_no_chain`, `st_tx_video_session.c:3413-3416`); `mtl_tx_write` or a library lease | packet build only | 1–3 |
| MXL grains, fixed grain count | ATTACHED pool over the MXL domain (a tmpfs mapping, §3.2), `rx_slot_select = BY_INDEX` on RX | 0 | 4 |
| MXL flow re-created with a different grain count | `stop` → `mtl_session_reconfigure` with the new `pool_count` ([09 §7.3](09-media-modes-and-backends.md)) → detach → attach → `start`: one stop/start gap; or DYNAMIC, which needs no re-attach | 0 | 4 |
| Moving cursor over one arena (`app/sample/ext_frame/tx_st20_pipeline_ext_frame_sample.c:147-162`; the sample copies its file into `mtl_dma_mem_alloc` memory first, `:80-99`) | one region + DYNAMIC (`layout.plane[0].offset` = the cursor); or one attached buffer per arena frame (`pool.count = F ≤ max_count`, §5.2) | 0 | 4 |
| Playout server that mmaps media files | hugetlbfs file: DIRECT. Regular filesystem (page cache): import accepted as **COPY** (§3.2) | 0 / engine copy | 4 |
| Capture SDK without an allocator hook (driver-owned buffers) | ATTACHED if its buffers are stable, page-aligned host memory; otherwise a copy | 0 or 1 | 4 |
| GPU pinned host memory (`cudaHostAlloc`, `zeMemAllocHost`) | import as host memory (it is page-locked anonymous memory in VA mode); RX DIRECT into it, then `cudaMemcpyAsync` H2D is the standard GPU ingest path `[C5 §6.14]` | 0 on the NIC side | 4 |
| VA-API surfaces, CUDA device pools, DMA-BUF | a CPU mapping plus a copy by the app; a direct path waits for the device-memory domain (§10, Q-MEM-8) | 1 | later |
| RX → TX forward, library RX pool (`app/sample/fwd/rx_st20p_tx_st20p_fwd.c:161-165`) | `mtl_session_get_pool_region` + TX buffers over it + submission `hold` (§5.3, §5.4) | 0 | 4 |
| Split-forward, 1 RX → 4 TX (`app/sample/fwd/rx_st20_tx_st20_split_fwd.c:263-265`) | the same, each TX buffer a sub-rectangle layout (§4.2) | 0 (partial packet copies counted) | 4 |
| Audio, ANC, fast metadata, any producer | copied into packets by construction; `mtl_tx_write` needs no buffer at all | packet build only | 1–2 |

"Submit an unattached buffer over an imported region", which revision 2 left as a Phase 6
escape hatch, is `MTL_POOL_DYNAMIC` in Phase 4.

## 3. Memory regions

### 3.1 Descriptor

```c
struct mtl_mem_desc {
  uint32_t struct_size;
  uint32_t domain;          /* MTL_MEM_HOST (0) | MTL_MEM_HOST_HUGEPAGE;
                               later: MTL_MEM_DMABUF | MTL_MEM_DEVICE | MTL_MEM_DEVICE_SHARED (Level Zero shared USM, COPY-only) */
  void*    va;              /* import: page-aligned start (§3.3); alloc: must be NULL */
  uint64_t length;          /* import: a multiple of the backing's page size; alloc: rounded up and reported */
  uint32_t numa;            /* 0 = auto (alloc: the first port's socket; import: detected); MTL_NUMA(n) = node n (r3) */
  uint32_t access;          /* MTL_MEM_READ | MTL_MEM_WRITE; 0 = both; enforced at attach (§3.6) */
  uint64_t device_mask;     /* 0 = lazy mapping (§3.5); else the ports / DMA engines mapped at import */
  uint64_t flags;           /* MTL_MEM_MAP_REQUIRED | MTL_MEM_NUMA_REQUIRED (r3) */
  uint64_t user_cookie;
  uint64_t reserved[4];     /* must be zero */
};
int mtl_mem_alloc (mtl_instance_h mt, const struct mtl_mem_desc* d, mtl_region_h* out);
int mtl_mem_import(mtl_instance_h mt, const struct mtl_mem_desc* d, mtl_region_h* out);
int mtl_mem_map_device(mtl_region_h r, uint32_t port);    /* r3: explicit, CP; the DMA engines of that port's sessions follow */
int mtl_mem_destroy(mtl_region_h r);                      /* -MTL_EBUSY while referenced */
int mtl_mem_get_info(mtl_region_h r, struct mtl_mem_info* info);

struct mtl_mem_info {                                     /* r3 */
  uint32_t struct_size;
  uint32_t backing;         /* enum mtl_backing: EAL_HUGEPAGE (1) | HUGETLBFS | ANON | SHMEM (tmpfs, memfd) | FILE (page cache) | DEVICE (later) */
  uint64_t length;
  uint32_t page_size;       /* of the backing: 4 KiB, 2 MiB, 1 GiB */
  uint32_t numa;            /* MTL_NUMA(n) of the pages; 0 = mixed or unknown */
  uint32_t numa_mismatch;   /* bit p: port p is on another socket than the pages */
  uint32_t direct_capable;  /* 0 = COPY-only (FILE backing, PA mode without page tables, read-only mapping) */
  uint64_t mapped_devices;  /* bit p: port p; bit 32 + d: DMA engine d */
  uint32_t buffers;         /* buffers that reference the region */
  uint32_t refs;            /* in-flight references (§3.7) */
  uint64_t reserved[4];
};
```

- `numa` follows the A7 zero-default rule: 0 is "auto", `MTL_NUMA(n)` is `n + 1`. The
  revision-2 `-1` default made a literal descriptor pin the region to node 0 `[C5 §2.4]`.
- `MTL_MEM_MAP_REQUIRED` is an import-time check on the devices named in a non-zero
  `device_mask`. Whether a session *uses* the direct path is decided by its
  `pool.data_path`; there is one knob per decision `[C3 consistency table]`.

### 3.2 Import semantics

| Backing (detected, reported as `mtl_mem_info.backing`) | Direct NIC access? | What import does |
|---|---|---|
| EAL hugepage memory (`rte_malloc`, `mtl_hp_malloc`) | yes | verified via `rte_mem_virt2memseg`; IOVA from DPDK; no new mapping, no region budget (§3.4) |
| anonymous memory, including **GPU pinned host memory** (`cudaHostAlloc`, `zeMemAllocHost`) **(r3)** | yes, VA mode | `rte_extmem_register`, then `rte_dev_dma_map` per device (§3.5) |
| shmem (`/dev/shm`, tmpfs), memfd, **hugetlbfs files** | yes, VA mode | as anonymous memory. MXL domains are tmpfs mappings, so they belong here `[C3 P5-2]` |
| regular-file mappings (page cache) **(r3)** | **no** | **accepted as COPY-only** and reported `backing = FILE`, `direct_capable = 0`; `REQUIRE_DIRECT` on a pool over it fails. A long-term DMA pin of page-cache pages races truncation and writeback, so MTL never DMA-maps them `[C5 §6.14]` |
| a read-only CPU mapping (`PROT_READ`) | no | COPY-only: DPDK maps every region read-write (`VFIO_DMA_MAP_FLAG_READ` and `_WRITE`) in VFIO (DPDK 26.07 `lib/eal/linux/eal_vfio.c:1442-1443`), and the kernel's write-pin of a read-only VMA fails **[inferred]** |
| PA mode, hugepage memory | yes via a per-buffer page table (today's `st_page_info` for internal frames) | page tables built at buffer create (Q-MEM-2) |
| PA mode, other memory | no | COPY-only; reported |

Backing detection is a CP step at import: MTL finds the VMA of `[va, va + length)` in
`/proc/self/maps`. For a file-backed VMA it runs `statfs` on the mapped path:
`HUGETLBFS_MAGIC` gives HUGETLBFS, `TMPFS_MAGIC` (including memfd) gives SHMEM, and any
other file system gives FILE. A range that spans several VMAs with different backings is
`-MTL_EINVAL`, reason `MIXED_BACKING`.

Which IOVA an imported non-hugepage region gets — MTL's invented range from `0x10000`
(`mt_dma.c:21`) or IOVA = VA — is Q-MEM-12.

Why import must map every device: today `mtl_dma_map` calls `rte_dev_dma_map` on port P
only and works for port R and the DMA engine only because DPDK's VFIO default container
happens to be shared (`mt_main.c:864-865`; DPDK `pci_common.c:478-493`). A driver with its
own `dma_map` op, a device in another container, or a non-DPDK primary port would leave the
other devices unmapped. `[R04 §4.2]`

### 3.3 Alignment (r3)

`[C5 §6.5]`

- `va` and `length` must be aligned to the backing's page size: 4 KiB for anonymous, shmem
  and memfd memory, and the huge page size for hugetlbfs. Otherwise import returns
  `-MTL_EINVAL`, reason `UNALIGNED`. MTL never rounds outward, because that would DMA-map
  bytes of unrelated heap objects into the IOMMU domain. Today `mtl_dma_map` already
  rejects both cases (`mt_main.c:830-838`), and `rte_extmem_register` needs page
  alignment.
- Plane offsets, strides and spans inside a region are **byte-granular**. The extbuf
  attach, the DMA engine and the SIMD converters (unaligned loads in `st_avx512.c`) all
  accept any byte offset. The requirements report `stride_align = offset_align = 1`
  where that holds (§4.1).
- The page sizes are in `mtl_port_get_caps` (`page_size`, `hugepage_sizes`) and in
  `mtl_mem_get_info.page_size`.
- G-96 tests the rule: every misalignment class fails and maps nothing, and an aligned
  import maps exactly `[va, va + length)`.
- A `memfd` region should be sealed with `F_SEAL_SHRINK | F_SEAL_GROW` before import, so
  another process that maps it cannot truncate the pinned pages away
  ([15 §2](15-security-and-deployment.md)).
- Framework heap memory (64 B-aligned) is therefore not importable as is. The framework
  either allocates its pool from a page-aligned arena (`posix_memalign(4096)`, a
  GstAllocator with `align = 4095`, a custom `av_buffer_pool` allocator) or copies (§2.1).

### 3.4 Region budget (r3)

`[C5 §6.4]`

Each non-EAL import is one `rte_extmem_register`. Each such call takes one DPDK memseg list,
and there are only `RTE_MAX_MEMSEG_LISTS = 128` of them, shared with the EAL's own hugepage
lists (DPDK 26.07 `lib/eal/common/malloc_heap.c:1188-1197` returns `ENOSPC` when none is
free). MTL's map table holds `MT_MAP_MAX_ITEMS = 256` entries (`lib/src/mt_main.h:62`). The
MXL POC hit the first limit at about 116 regions and had to coalesce grains
(`ecosystem/MTL_with_MXL/poc/src/sender/mxl_bridge.c:213-218`).

- `mtl_port_get_caps` reports `max_regions`, and `mtl_instance_get_mem_status` (§6.3)
  reports `regions_used` and `regions_free`. Both are the lower of the two tables.
- An import beyond the budget returns `-MTL_ENOSPC`, reason `REGION_BUDGET`, never an
  opaque DPDK error.
- **Required pattern: one region per pool arena.** Import a framework pool's contiguous
  allocation once, or `mtl_mem_alloc` once for N buffers, and create N buffers over it.
  Per-buffer imports are legal but spend the budget and cost one VFIO `MAP_DMA` ioctl per
  device (ms-scale, CP only).
- EAL hugepage memory and library pools consume no budget.

### 3.5 Device mappings (r3)

`[C5 §6.11]`

- `device_mask = 0` means **lazy mapping**. Import pins and registers the memory and
  checks that it *can* be mapped (backing, access, alignment), but maps nothing. Each
  `mtl_session_attach_buffers` maps the region into the devices that session needs and
  that are not mapped yet: the ports of its legs, plus the DMA engine if DMA offload was
  granted (the DMA set is decided at session create, `st_rx_video_session.c:2572-2575`).
  This mapping is a CP step inside attach, never on the data path.
- `mtl_mem_map_device(region, port)` maps explicitly, for example to pay the ioctl cost
  before a live start or before a DYNAMIC session uses the region (§5.5).
- A non-zero `device_mask` maps eagerly at import. `MTL_MEM_MAP_REQUIRED` fails the import
  if any named device cannot map the region.
- `mtl_mem_get_info.mapped_devices` lists the current mappings. A port opened after the
  import (Phase 6 `mtl_port_open`) is mapped at its first attach; no re-import is needed.
- Unmapping happens only in `mtl_mem_destroy`, for every mapped device.

### 3.6 Access and NUMA (r3)

- **Access is enforced at attach** `[C5 §6.10]`. An RX session needs `MTL_MEM_WRITE`, a TX
  session needs `MTL_MEM_READ`; `access = 0` grants both. A mismatch returns `-MTL_EINVAL`,
  reason `ACCESS_MISMATCH`. The IOMMU mapping itself is always read-write in v1 (§3.2), so `access`
  is a contract check, not hardware protection.
- **NUMA** `[C5 §6.14]`. Import detects the node of the pages (`move_pages` query on one
  page per huge page or per 2 MiB, CP). `mtl_mem_info.numa_mismatch` has a bit for every
  port on another socket. At attach, `mtl_session_get_info` reports `numa_mismatch`
  when the pool's pages are not on the session's socket, because a NUMA mismatch roughly
  doubles DMA latency.
- With `MTL_MEM_NUMA_REQUIRED`, import fails with `-MTL_EINVAL`, reason `NUMA_MISMATCH`,
  if the pages are not on `numa` (or, when `numa = 0`, on the first port's socket). Attach
  fails the same way for a session on another socket. Without the flag, a mismatch is
  reported, not rejected.

### 3.7 Lifetime and teardown order (r3)

- The region holds a refcount: +1 per buffer that references it, and +1 per operation in
  flight on such a buffer (bind to a transport slot, DMA copy, conversion, RX hold, §5.4).
- The in-flight reference is dropped only at the *real* end of access: the engine's
  once-only completion hook (04 §4.4), `rv_put_frame` after release, DMA completion
  drained. `[R04 §7 #2]`
- `mtl_mem_destroy` returns `-MTL_EBUSY` while any reference remains. An optional
  deferred-release mode posts `REGION_RELEASED` when the last reference drops (io_uring
  resource tags `[R11 §4.5]`, Q-MEM-4).
- Mapping and unmapping never happen on the data path. `[R00 §14 #30]`

**The application's teardown order** `[C5 §6.12]`. During a deferred destroy the buffers
stay attached until `SESSION_RETIRED`, so revision 2's example order returned `-EBUSY` on
every buffer.

1. Call `mtl_session_stop` (FLUSH or DRAIN), then `mtl_session_destroy(s, 0)`.
2. Wait for `SESSION_RETIRED` on the session EQ, or on any EQ subscribed with
   `MTL_EQ_SUB_SESSION` ([07](07-completions-events-and-errors.md)). Alternatively call
   `mtl_session_destroy(s, MTL_DESTROY_FORCE)`, which revokes app leases and returns after
   retirement. Retirement includes the bounded stalled-queue reset
   ([03 §6](03-object-model-and-lifecycle.md)), so it never waits forever.
3. Call `mtl_buffer_destroy` on each buffer, then `mtl_mem_destroy` on the region.
4. **Only after `mtl_mem_destroy` returns 0** may the application unmap or free the
   memory. Freeing it earlier is undefined: the IOMMU may still hold the mapping, and a
   late DMA drain faults.

After a hung TX queue, `mtl_mem_destroy` keeps returning `-MTL_EBUSY` until the
stalled-queue reset of [04 §4.5](04-threading-and-execution.md) has run and released the
device references; it then returns 0. The rule is the one of
[03 §6.4](03-object-model-and-lifecycle.md).

A `GstBufferPool` or `GstBaseSrc` must finish steps 1–3 inside `set_active(FALSE)` or
`stop`, before the framework frees its arena. Process exit is the one case that bypasses
the order.

## 4. Buffers and layouts

```c
struct mtl_plane_desc {          /* fixed size; versions with the parent (11 §2.1 rule 5) */
  mtl_region_h region;
  uint64_t offset;               /* from region start; byte-granular (§3.3) */
  uint64_t span;                 /* bytes MTL may touch; 0 = stride*(rows-1) + row_bytes */
  uint32_t row_bytes;            /* valid bytes per row */
  uint32_t stride;               /* bytes between row starts; 0 = row_bytes (packed) */
  uint32_t rows;
  uint32_t reserved[3];          /* must be zero */
};
struct mtl_buffer_desc {
  uint32_t struct_size;
  uint32_t plane_count;          /* 1..MTL_MAX_PLANES (4) */
  struct mtl_plane_desc plane[MTL_MAX_PLANES];
  mtl_region_h meta_region;      /* optional; null = the library-owned meta area of the slot (§4.4) */
  uint64_t meta_offset;
  uint32_t meta_capacity;        /* bytes; 0 with a null meta_region */
  uint32_t reserved0;
  uint64_t user_cookie;          /* stable per-buffer identity for the app */
  uint64_t reserved[2];
};
struct mtl_plane_view {          /* fixed size */
  void*    addr;                 /* NULL when the domain has no CPU mapping (device memory); bindings: uintptr_t */
  uint32_t stride, row_bytes, rows, reserved;
};
struct mtl_buffer_view {         /* returned by acquire/dequeue (nullable out-pointer) and mtl_buffer_get_view */
  uint32_t struct_size;
  uint32_t plane_count;
  struct mtl_plane_view plane[MTL_MAX_PLANES];
  uint32_t domain;               /* MTL_MEM_* */
  uint32_t meta_bytes;           /* RX: bytes valid in the meta area */
  void*    meta;                 /* meta area CPU address */
  uint64_t device_handle;        /* domain-specific handle when addr is NULL (dma-buf fd, device pointer); 0 for host memory */
  uint64_t reserved[2];
};
int mtl_buffer_create(const struct mtl_buffer_desc* d, mtl_buffer_h* out);
int mtl_buffer_destroy(mtl_buffer_h b);        /* -MTL_EBUSY while leased, held, or attached to a session that has not retired */
int mtl_buffer_get_view(mtl_buffer_h b, struct mtl_buffer_view* v);
int mtl_buffer_hold(mtl_buffer_h b);           /* r3, DP: exclude from any-free acquire (§4.5) */
int mtl_buffer_unhold(mtl_buffer_h b);
```

- Layouts are immutable: a buffer never changes shape between uses. A different format
  needs a different buffer, or `mtl_session_reconfigure` for library pools
  ([09 §7.3](09-media-modes-and-backends.md)).
- **(r3)** A buffer belongs to at most one session's pool: attaching it to a second
  session returns `-MTL_EBUSY`. Several sessions share *bytes* by creating their own
  buffers over one region, and RX → TX sharing uses holds (§5.4). This replaces revision
  2's "one exclusive lease across sessions", which serialised split-forward `[C5 §6.2]`.
- Audio, ANC and fast-metadata buffers are one plane with `rows = 1`. The plane's
  `row_bytes` is the byte capacity, and the *valid* size of one use goes in the
  submission (`sample_count` for PCM, which is authoritative; `valid_bytes` otherwise).
- `span` is explicit so MTL can validate that no packet or DMA touches bytes outside the
  plane (review §14 #11). Today nothing checks that an ext frame lies inside a mapped
  region. RX `query_ext_frame` checks only `buf_len` (`st_rx_video_session.c:1279`) and
  assigns `addr`/`iova` unchecked (`:1285-1286`) `[R04 §6 #6, C5 §6.14]`.

### 4.1 Asking what a session needs

Frameworks must negotiate formats and pools *before* they create a session (GStreamer caps
and ALLOCATION query, FFmpeg `get_buffer2`) `[R08 §4.1]`:

```c
/* pure functions, no instance needed */
int mtl_video_layout_query(const struct mtl_video_config* v, struct mtl_buffer_requirements* out);   /* v->app_format selects the layout */
int mtl_video_format_enum(uint32_t i, struct mtl_format_pair* pair);   /* supported (transport, app) conversions */

/* dry run: validates a config, returns granted values + requirements, allocates nothing (03 §3.3) */
int mtl_video_session_query(mtl_instance_h mt, const struct mtl_session_config* sc, const struct mtl_video_config* vc,
                            uint64_t flags /* MTL_QUERY_CHECK_CAPACITY */,
                            struct mtl_session_info* info, struct mtl_buffer_requirements* req);

/* after create */
int mtl_session_get_buffer_requirements(mtl_session_h s, struct mtl_buffer_requirements* out);
```

```c
struct mtl_buffer_requirements {           /* r3 */
  uint32_t struct_size;
  uint32_t plane_count;
  struct {
    uint64_t min_span;                     /* at the natural stride */
    uint32_t row_bytes;                    /* bytes per row the session reads or writes */
    uint32_t rows;                         /* per unit: height; height/2 for an interlaced field (§4.3) */
    uint32_t stride;                       /* natural stride, used by library pools */
    uint32_t min_stride;                   /* any stride >= min_stride (== row_bytes) is accepted on every path (§4.2) */
    uint32_t stride_align;                 /* 1 = byte-granular */
    uint32_t offset_align;                 /* 1 = byte-granular plane offsets */
  } plane[MTL_MAX_PLANES];
  uint32_t direct_possible;                /* 0/1 for this layout, backend and granted pacing */
  uint32_t min_count;                      /* minimum pool size on any path */
  uint32_t min_count_direct;               /* ST20 TX: 1 + ceil(nb_tx_desc / packets_per_unit); 0 = not applicable */
  uint32_t max_count;                      /* 8 for video and cvideo until engine change E11 (§5.2) */
  int64_t  completion_latency_ns;          /* expected gap from a unit's last packet to its result on the DIRECT path */
  uint32_t domains;                        /* accepted MTL_MEM_* bitmask */
  uint32_t meta_capacity;                  /* RX meta area per unit (ANC packet table, user meta) */
  uint32_t max_user_meta_bytes;            /* TX user meta per unit (§4.4); 0 = not supported by the essence */
  uint32_t internal_buffer_count;          /* frames MTL allocates besides the pool (CONVERT/COPY paths, §6.3) */
  uint64_t internal_bytes;                 /* their total size */
  uint64_t reserved[4];
};
```

**The minimum buffer count for the direct TX path** exists because zero-copy TX frames
become reusable only when the NIC recycles their descriptors, about `nb_tx_desc` packets
later. Today `tv_pkts_capable_chain` silently falls back to copy when
`total_pkts × (frames_cnt − 1) < nb_tx_desc` (`st_tx_video_session.c:2873-2895`, with a warn
log line) `[R04 §6 #4–5]`. `nb_tx_desc` defaults to `MT_DEV_TX_DESC = 512`
(`lib/src/dev/mt_dev.h:12`), and an instance parameter can override it (`mt_dev.c:1009`).
Realistic values, answering `[C5 §6.14]`:

| Format (GPM_SL) | Packets per unit | `min_count_direct` at 512 descriptors | `completion_latency_ns` ≈ 512 × TRS |
|---|---|---|---|
| 1080p59.94 4:2:2 10-bit | 4320 | 2 | ≈ 1.9 ms |
| 1080i59.94 field | 2160 | 2 | ≈ 1.9 ms (TRS of the field) |
| 2160p59.94 4:2:2 10-bit | 17280 | 2 | ≈ 0.5 ms |
| 480i field 4:2:2 10-bit, or any unit under 512 packets | < 512 | 3 | ≈ 512 × TRS |
| cvideo, audio, ANC, fast metadata; every RX session | — | **0** (not applicable: no direct TX path, `st_tx_video_session.c:3413-3416`) | 0 |

The latency column is a model; the Phase 0 baseline spike S0 measures it, because it also
depends on the PMD's `tx_rs_thresh` and `tx_free_thresh`, not only on the ring size.
`completion_latency_ns` is what a framework adds to its pool sizing (§5.6).

Rule: if a layout satisfies every advertised requirement and capacity remains, attach and
start must not reject it later for an undisclosed reason. `[R00 §10]` A layout that does
not satisfy them fails at attach with `-MTL_EINVAL`, reason `LAYOUT_MISMATCH`.

### 4.2 Strides: DIRECT is stride-aware (r3)

Revision 2 said a sub-rectangle TX is CONVERT/COPY "until the builder learns strides". The
builder already knows them `[C5 §6.1]`:

- `st20_tx_ops.linesize` is a session parameter (`include/st20_api.h:1215-1218`, RX
  `:1561-1564`). The session takes `max(linesize, bytes_in_line)` and rejects a smaller
  non-zero value (`st_tx_video_session.c:3333-3339`, RX `st_rx_video_session.c:3343-3351`).
- The chain builder computes each packet's offset with the linesize
  (`st_tx_video_session.c:1225`, and `:1270-1272` for non-single-line packing). It
  attaches the frame at that offset as an extbuf (`:1295-1298`).
- It copies only two kinds of packet: a packet that crosses line padding in GPM or BPM
  packing (`:1274-1288`), and, in PA mode, a packet that crosses a page (`:1289-1292`).
  The copy builder does the same (`:1120`, `:1162-1175`).
- Both split-forward samples rely on this today:
  `app/sample/fwd/rx_st20_tx_st20_split_fwd.c:263-265` sets `width/2` with the full 4K
  linesize.

The r3 rule is therefore:

- **For ST20 TX and RX, any `plane[0].stride ≥ row_bytes` is a DIRECT-capable layout.** The
  stride maps onto the engine's `linesize`. `mtl_video_config` has no linesize field: the
  layout's stride carries it.
- **One stride per session.** The engine's linesize is per session, so every buffer in
  one pool has the same plane strides.
  - Library pools use the natural stride.
  - Attached pools bind the stride at the first attach. A later attach with another
    stride returns `-MTL_EINVAL`, reason `STRIDE_MISMATCH`.
  - In v1 over the pipelines, L2 passes the stride to the engine through the
    `transport_linesize` the pipelines already forward (`st20_pipeline_tx.c:451`,
    `st20_pipeline_rx.c:560`), via the slot-interface entry point M9 (§11), before start.
- **Packets copied instead of attached** are counted per unit as `pkts_copied_partial`
  (§6.2). With GPM_SL (the default packing) no packet crosses a line, so the count is 0. BPM
  (1200 B packets) crosses no line when `row_bytes` is a multiple of 1200, for example 1920
  pixels of 4:2:2 10-bit (4800 B). GPM copies about one packet per padded line.
- Conversion paths accept any stride ≥ `min_stride` too; only the converter reads it.

### 4.3 Interlaced and PsF buffer shape (r3)

`[C5 §6.9]`

- **Interlaced: one unit is one field.** The layout has `rows = height / 2`, as today
  (`st_fmt.c:611` halves the frame size for interlaced). `media_index` parity says which
  field it is (06 §4.2).
- **A woven (interleaved) frame is two field buffers over one region.** With L the frame's
  line pitch (`row_bytes` plus any padding), field 1 has `offset = base` and field 2 has
  `offset = base + L`. Both have `stride = 2 × L` and `rows = height / 2`. This is DIRECT
  under §4.2, so a GStreamer `interlace-mode=interleaved` buffer is two submissions that
  both hold the same GstBuffer, with no weave copy. Today's sink maps interleaved caps
  onto the half-height field buffers (`gst_mtl_st20p_tx.c:358-363`).
- RX works the same way: two field buffers over one frame-sized region deliver a woven
  frame directly.
- **PsF: one unit per frame**, `rows = height`, one RTP timestamp (06 §3.4).

### 4.4 The meta area (r3)

`[C5 §6.14]`

- **Attached buffers need no meta region.** TX user meta travels in the submission and is
  copied at submit (`user_meta`, `user_meta_size`, 06 §4.1). RX meta (the ANC packet
  table, received user meta) lands in a library-owned per-slot area of the lease table.
  `view.meta` points at it. The buffer's `meta_region` is only for apps that want RX meta
  written into their own memory, such as an MXL grain header.
- **Limits.** TX user meta is at most one packet per unit: today
  `pkt_udp_suggest_max_size − sizeof(st20_rfc4175_rtp_hdr)` = 1352 − 20 = **1332 B**
  (`st_tx_video_session.c:271-272`, `MTL_PKT_MAX_RTP_BYTES` at `include/mtl_api.h:89`).
  A tagged payload (below) leaves **1320 B** of user bytes. The limit is reported as
  `max_user_meta_bytes`, and it applies to video and cvideo only; other essences report 0.
  - The ST40 RX packet table is capped at `ST40_MAX_META = 20` entries today
    (`include/st40_api.h:308`). The unified API sizes it by `mtl_anc_config.max_packets`
    (0 = 255, the per-unit `anc_count` limit in 06).
  - v1 over the legacy engine grants 20, reported in `mtl_session_info`, until engine
    change E12 grows the engine's array together with E11 (§5.2).
- **Type tag.** `mtl_tx_submission.user_meta_type` and `user_meta_version` tag the user
  meta, and the RX unit returns them (`meta_type`, `meta_version`,
  [07](07-completions-events-and-errors.md)). Two applications can then check what they
  receive instead of relying on an out-of-band contract.
  - With a non-zero `user_meta_type`, the sender prefixes the payload on the wire with 12
    bytes: `{ uint32_t magic = 'MTLM'; uint32_t meta_type; uint16_t meta_version; uint16_t length }`.
  - `user_meta_type = 0` (`MTL_META_UNTAGGED`) sends the payload exactly as today, with no
    header. The zero default is therefore wire-compatible with legacy receivers, and the
    tag is opt-in.
  - A received payload without the magic, for example from a legacy sender, is delivered
    with `meta_type = 0`.
  - `meta_type` values below `0x80000000` are registered by MTL; the others are private.

### 4.5 View validity (r3)

`[C5 §6.7]`

- **TX: MTL never writes a TX buffer.** Every TX path only reads the application's buffer:
  DIRECT (the NIC), COPY (the builder) and CONVERT (the converter, which writes an
  *internal* frame).
  - After `mtl_tx_submit`, the application may keep **reading** the buffer through its
    cached view while the unit is QUEUED and IN_FLIGHT. The bytes stay the same and the
    memory stays mapped until that buffer is acquired again.
  - In `MTL_COMPLETE_ALL` / `EXCEPTIONS`, the result tells the app when the unit
    finished. In `MTL_COMPLETE_NONE` the app cannot know when the slot becomes FREE, so
    an any-free `mtl_tx_acquire` could return that buffer to another writer.
  - `mtl_buffer_hold(b)` excludes the buffer from any-free acquire and from pool reclaim
    until `mtl_buffer_unhold(b)`. `mtl_tx_acquire_buffer(s, b)` still leases it, because
    that is the app's explicit choice.
  - Holds count against the pool, so `mtl_session_get_status().blocked_on` reports
    `APP_HOLDS` when they are what blocks acquire. Display-while-sending and diff-against-
    the-last-frame patterns therefore need no copy.
- **RX: the strict rule stays.** The view is valid while the lease is APP_READING, including
  across stop, and invalid after release, because the slot returns to RECEIVING and MTL
  writes it again. Monitor-plus-forward no longer needs `mtl_rx_transfer` to invalidate the
  RX view: the app keeps its RX lease for the display and the TX sessions hold the slot
  through their own buffers (§5.4).

## 5. Pools

```c
struct mtl_pool_config {         /* fixed size; versions with mtl_session_config */
  uint32_t source;               /* MTL_POOL_LIBRARY (0) | MTL_POOL_ATTACHED | MTL_POOL_DYNAMIC (r3, Phase 4) */
  uint32_t count;                /* 0 = default (09 §9); LIBRARY: buffers MTL allocates; ATTACHED: 0 = what is attached at start;
                                    DYNAMIC: lease slots */
  uint32_t data_path;            /* MTL_PATH_ALLOW_COPY (0) | MTL_PATH_PREFER_DIRECT | MTL_PATH_REQUIRE_DIRECT */
  uint32_t rx_overflow;          /* MTL_RX_DROP_NEW (0) | MTL_RX_RECLAIM_OLDEST_READY */
  uint32_t rx_slot_select;       /* MTL_RX_SLOT_ANY_FREE (0) | MTL_RX_SLOT_BY_INDEX (slot = media_index mod count) */
  uint32_t rx_fill;              /* MTL_RX_FILL_DEFAULT (0: ZERO_MISSING for library pools, NONE for attached) | ZERO_MISSING | NONE */
  uint32_t reserved[6];          /* r3: sized by observed growth (A8) */
};
int mtl_session_attach_buffers(mtl_session_h s, const mtl_buffer_h* bufs, uint32_t n);   /* CP; maps lazily (§3.5) */
int mtl_session_detach_buffers(mtl_session_h s);   /* CREATED/STOPPED only; every slot FREE and unheld */
int mtl_session_get_pool_region(mtl_session_h s, mtl_region_h* out);   /* r3: library pools only (§5.3) */
```

- A session starts only when its pool is complete and validated (review §14 #29): a start
  with fewer attached buffers than `pool.count` fails with `-MTL_EINVAL`, reason
  `POOL_INCOMPLETE`, and a count below `min_count` with reason `POOL_TOO_SMALL`. Attach
  and detach never happen on the data path (review §14 #30).
- **(r3)** `pool.count` may change in CREATED and STOPPED through `mtl_session_reconfigure`
  ([09 §7.3](09-media-modes-and-backends.md)). Library pools are re-created (Phase 2);
  attached pools are re-validated (Phase 4), and one that no longer fits fails with
  reason `RECONFIGURE_INCOMPATIBLE` `[C5 §7.4]`.
- **(r3)** `pool.count = 0` has a stated default: video `max(min_count_direct, 3)` (3 on
  RX), cvideo, audio, ANC and fast metadata 4 ([07 §2.2](07-completions-events-and-errors.md),
  [09 §9](09-media-modes-and-backends.md)) `[C5 §3.6]`.
- Completion mode: library pools default to `MTL_COMPLETE_NONE`. ATTACHED and DYNAMIC
  pools and exported pools (`MTL_SESSION_EXPORT_POOL`,
  [03 §4.4](03-object-model-and-lifecycle.md)) force `MTL_COMPLETE_ALL`, because the result
  is how the app learns its buffer is free. One exception (r3): a **forward pool**, whose buffers all lie
  inside an RX session's pool region and are always submitted with a `hold` (§5.4), may
  choose NONE. The hold, not a result, then returns the bytes, so the forwarder needs no
  reap loop `[C5 §3.4]`.
- Mixed pools (some library, some imported) are a later capability. `[R00 §3]`

### 5.1 `BY_INDEX` sizing and overflow (r3)

`[C5 §6.8]`

With `BY_INDEX`, unit *k* can only use slot *k mod N*. The effective headroom is
therefore one slot, not N − 1. The sizing rule is:

```text
N  >  H + 1 + ceil((rx_flush_offset + reorder_skew) / unit_period)
```

- H is the maximum number of units the consumer holds at once (APP_READING or HELD).
- The 1 is the unit being received.
- The last term counts units still RECEIVING while 2022-7 skew and the flush deadline
  keep them open (06 §11).

`mtl_session_get_info` reports the granted flush offset and the tolerated skew, so an MXL
bridge can compute N.

- `RECLAIM_OLDEST_READY` + `BY_INDEX` reclaims **only the target slot**, and only if it is
  READY (not dequeued). If the target slot is APP_READING, HELD or RECEIVING, the new unit
  is dropped, counted, and reported as `units_missed_before` on the next delivery. Other
  slots are never reclaimed, because they are not where unit *k* may go.

### 5.2 Pool size limits (r3)

`ST20_FB_MAX_COUNT = 8` (`include/st20_api.h:24`) is enforced at session create: TX
`st_tx_video_session.c:4073`, RX `st_rx_video_session.c:4266`, and `ST22_FB_MAX_COUNT` at
`:4189` / `:4403`. v1 over the pipelines inherits the cap `[C5 §6.14]`. Decision
(answers Q-MEM-5):

- **Engine change E11 (Phase 2)**: make the ST20/ST22 frame arrays dynamic and lift the
  cap. The bound then becomes memory plus the 16-bit slot field of the lease encoding
  (A4). The engine code is shared, but legacy ops keep the limit, because
  `ST20_FB_MAX_COUNT` is ABI ([14 §2](14-implementation-roadmap.md)).
- Until E11 lands, `mtl_buffer_requirements.max_count` reports **8** for video and cvideo.
  A larger `pool.count` fails at query and create with `-MTL_ERANGE`, reason
  `POOL_COUNT_MAX`. It never fails at start.
- Audio, ANC and fast metadata have no engine cap today (only `framebuff_cnt ≥ 1`, for
  example `st_tx_audio_session.c:2667`). They report the lease-encoding bound.

### 5.3 A library pool is a region (r3)

`mtl_session_get_pool_region(rx, &region)` returns the region behind a session's library
pool, so other sessions can create buffers over it `[C5 §3.4, §6.2]`. The library pool is
EAL hugepage memory, so the region consumes no budget and needs no mapping. In v1 over the
pipelines, each library frame is its own allocation, so the region may consist of several
segments, one per slot. Slot *j* starts at offset `j × pool_slot_pitch` of the region
(`pool_slot_pitch` is in `mtl_session_info`), and a buffer over the region must lie
inside one slot (`-MTL_EINVAL`, reason `SPAN` otherwise). `mtl_mem_get_info` works on it; `mtl_mem_destroy` does not
(`-MTL_EBUSY`: the session owns it). The region lives until the owning session retires.

### 5.4 RX holds: one RX unit feeding N TX sessions (r3)

`[C5 §6.2]`

Split-forward needs four TX sessions to read the same RX bytes in the same frame period.
Revision 2's exclusive lease serialised them over four frame periods. The r3 mechanism is
a **hold count on the RX slot**, taken by TX submissions (the lease rules are in
[03 §4.2–§4.3](03-object-model-and-lifecycle.md)):

- `struct mtl_tx_submission.hold` is an `mtl_lease_h` of an RX session in APP_READING
  (null = no hold). A successful submit adds one reference to that RX slot. The reference
  is dropped at the TX unit's terminal outcome (DONE, FLUSHED or FAILED): the instant MTL
  stops reading.
- Validation (DP, O(planes)) checks three things:
  - the lease belongs to an RX session of the same instance, otherwise `-MTL_EBADF`;
  - it is current, otherwise `-MTL_ESTALE`;
  - every TX plane lies inside the held RX buffer's planes (same region, offset range),
    otherwise `-MTL_EINVAL`, reason `HOLD_MISMATCH`.

  A hold on the wrong slot would let RX overwrite bytes that TX is still sending.
- **The RX slot returns to FREE when the app has released it and its hold count is 0.**
  After `mtl_rx_release` with holds outstanding, the slot is **HELD**
  (`MTL_LEASE_HELD_BY_TX` in the queue gauges): the app's lease is
  stale, MTL does not write the slot, and `RECLAIM_OLDEST_READY` never takes it.
  Whichever context drops the last reference (app release, or the TX completing context)
  makes the FREE transition with a CAS. The engine release underneath is the atomic
  `rv_put_frame` decrement (`st_rx_video_session.c:222-231`), which is safe from any
  context.
- Up to 255 holds per slot; one more returns `-MTL_EBUSY`.
- Every hold must be submitted before the app's own `mtl_rx_release`. A hold that names a
  released lease gets `-MTL_ESTALE`.
- A TX stop, withdraw or flush drops each hold at the unit's FLUSHED/WITHDRAWN outcome.
- An RX session with HELD slots can be stopped. Its destroy is deferred like a destroy
  with outstanding leases, until the last hold drops.
- `mtl_rx_transfer(rx, rl, tx, &tl)` stays as sugar for the 1 → 1 case. It leases the TX
  session's attached buffer whose planes equal the RX buffer's (`-MTL_EINVAL`, reason `HOLD_MISMATCH`, if there is
  none), records an implicit hold, and releases `rl` when `tl` is submitted.

The 4K → 4 × 1080p split-forward, zero-copy and DIRECT:

```c
/* setup, CP: RX library pool of N slots; four TX sessions with ATTACHED (forward) pools */
mtl_region_h rg;  mtl_session_get_pool_region(rx, &rg);
uint32_t rx_stride = 3840 * 5 / 2;                         /* 4:2:2 10-bit: 5 bytes per 2 pixels */
for (int q = 0; q < 4; q++) {
  for (uint32_t j = 0; j < N; j++) {
    struct mtl_buffer_desc d;  mtl_buffer_desc_init(&d);
    d.plane_count        = 1;
    d.plane[0].region    = rg;
    d.plane[0].offset    = j * pool_slot_pitch              /* mtl_session_info of the RX session (§5.3) */
                         + (q / 2) * 1080 * rx_stride + (q % 2) * (1920 * 5 / 2);
    d.plane[0].row_bytes = 1920 * 5 / 2;
    d.plane[0].stride    = rx_stride;                       /* > row_bytes: still DIRECT (§4.2) */
    d.plane[0].rows      = 1080;
    mtl_buffer_create(&d, &qb[q][j]);
  }
  mtl_session_attach_buffers(tx[q], qb[q], N);
}

/* per frame: one dequeue, four submits, one release; nothing is copied or serialised */
mtl_lease_h rl;  struct mtl_rx_unit u;
if (mtl_rx_dequeue(rx, &rl, NULL, &u, sizeof(u), timeout) == 0) {
  uint32_t j = mtl_buffer_index(mtl_lease_buffer(rl));
  for (int q = 0; q < 4; q++) {
    mtl_lease_h tl;
    struct mtl_tx_submission sub;  mtl_tx_submission_init(&sub);
    sub.hold = rl;                                          /* RX slot j stays HELD until this unit is done */
    if (mtl_tx_acquire_buffer(tx[q], qb[q][j], &tl, NULL, NULL, 0) == 0 && mtl_tx_submit(tx[q], tl, &sub) != 0)
      mtl_tx_release(tx[q], tl);
  }
  mtl_rx_release(rx, rl);                                   /* slot j becomes FREE after the last of the four */
}
```

TX slot *j* of each quadrant session is always FREE by the time RX slot *j* is READY again,
because RX slot *j* could not be refilled before all four TX units were done. This is why
the forward pool may run in `MTL_COMPLETE_NONE` (§5).

### 5.5 Dynamic pools (r3, Phase 4)

`[C5 §6.3, §3.4]`

```c
int mtl_tx_acquire_dynamic(mtl_session_h s, const struct mtl_buffer_desc* layout, mtl_lease_h* lease,
                           struct mtl_buffer_view* view /* nullable */, int64_t timeout_ns);
```

- `pool.source = MTL_POOL_DYNAMIC` allocates `pool.count` lease slots and no buffers.
  Each `mtl_tx_acquire_dynamic` takes a free slot and binds it to the caller's layout for
  one use.
- The layout must lie in a region that is already imported and mapped into the session's
  devices, by `device_mask` or `mtl_mem_map_device`. Otherwise the call returns
  `-MTL_EINVAL`, reason `REGION_NOT_MAPPED`: mapping is CP work and must never happen
  inside a DP call.
- Validation is the attach-time layout check (span, stride ≥ `min_stride`, one stride per
  session, access) done in O(planes). The slot keeps the layout inline, so the call does
  no allocation and is a DP verb (WT with a timeout).
- Completion is forced to `MTL_COMPLETE_ALL`: the result, with the submission's cookie, is
  how the producer learns it may reuse or free its memory.
- The slot count is the in-flight bound. The default count is the LIBRARY default of the
  essence (09 §9), and `min_count_direct` still applies for DIRECT.
- This covers three cases with one mechanism: the moving-cursor ext frame (§2.1),
  FFmpeg per-frame TX from one imported arena, and GStreamer upstream pools whose arena
  was imported once.
- RX dynamic provide (`mtl_rx_provide`) stays later (Q-MEM-10).

### 5.6 Framework pools: the two-phase attached-pool recipe (r3)

`[C5 §6.13]`

A zero-copy sink keeps a reference on the upstream buffer until its `TX_RESULT`: the
frame's wire time plus `completion_latency_ns`. Upstream pools negotiated with
`min_buffers = 2` (`videotestsrc`, `v4l2src`) then block in their own `acquire`. Today's
sink holds its reference until `notify_frame_done` (`gst_mtl_st20p_tx.c:547-597`) and has
the same exposure. The recipe:

1. **Negotiate.** At caps time, call `mtl_video_session_query`. In the ALLOCATION query:
   - offer the export pool first (§2), which upstream fills directly;
   - otherwise set `min_buffers ≥ min_count_direct + 1` plus the buffers the sink itself
     queues;
   - add `GST_VIDEO_META` so upstream can use a padded stride, which stays DIRECT
     under §4.2.
2. **Decide per buffer in steady state.**
   - The buffer is from the export pool: submit it.
   - It is from an upstream pool whose configured `max_buffers` (0 = unbounded) is at least
     `min_count_direct + 1`, and whose memory is importable (page-aligned host memory,
     region budget left): import the arena once, then `mtl_tx_acquire_dynamic` + submit,
     holding the GstBuffer reference until the `TX_RESULT`.
   - Otherwise: copy into a library lease (`mtl_lease_copy_in`) and unref at once. This is
     the `ALLOW_COPY` fallback, for example v4l2 mmap pools with a fixed small count.
3. **Early release for COPY/CONVERT (Phase 4).**
   - When the granted path copies or converts, the session can emit an early
     `MTL_CQE_TX_SOURCE_RELEASED` entry as soon as MTL stops reading the source (§7).
   - The sink unrefs the GstBuffer there, one conversion time after submit instead of one
     completion latency.
   - DIRECT cannot release early, because the NIC reads the memory until completion.

[11 §6.2](11-abi-compatibility-and-migration.md) carries the GStreamer and FFmpeg wiring.

## 6. Data paths

### 6.1 Policy and selected path

| `pool.data_path` | Meaning |
|---|---|
| `ALLOW_COPY` (default) | MTL may copy or convert; the selected path is reported |
| `PREFER_DIRECT` | direct when possible; the fallback is reported as a *granted* value and counted |
| `REQUIRE_DIRECT` | query/create/attach/start fails with `-MTL_ENOTSUP` and a reason if the direct path is impossible: a pool below `min_count_direct` (reason `POOL_TOO_SMALL`, Q-MEM-7), a COPY-only region, or **(r3)** an `app_format` that needs conversion (reason `DIRECT_IMPOSSIBLE` for both, the latter at query and create) `[C5 §6.6]`; never copies silently |

| Selected path | TX meaning | RX meaning |
|---|---|---|
| `DIRECT` | the NIC reads the buffer (extbuf chain); packets crossing line padding or a PA-mode page are copied and counted (§4.2) | packets are placed into the application buffer with no intermediate frame: `DIRECT_CPU` (CPU copy of each payload), `DIRECT_DMA` (DMA engine, with per-packet CPU fallback), `DIRECT_HDS` (header split, later) `[R04 §3]` |
| `CONVERT` | a converter reads the buffer and writes an internal transport frame, which is sent | an internal frame is filled; a converter writes the buffer |
| `COPY` | packets are built by copying from the buffer (no-chain mode, cvideo, audio/ANC/fast metadata, AF_XDP, kernel socket, COPY-only regions) | an extra copy from an internal frame into the buffer |
| `COPY_AND_CONVERT` | copy of a converted frame | — |

### 6.2 Where the path is decided and reported (r3)

`[C5 §6.14]`

The path is not binary per unit: the RX DMA decision is per *packet*
(`st_rx_video_session.c:1804-1826`: payload over `ST_RX_VIDEO_DMA_MIN_SIZE`, ring not full,
not crossing a page, else a CPU copy), and so is the TX partial copy. The r3 reporting is:

- **`path`** in `mtl_session_get_info` and in each TX result / RX unit is the **granted
  session path**. It changes only on a reported downgrade (`PACING_CHANGED`, a DMA engine
  loss).
- **Per-unit counters** in `mtl_tx_result` and `mtl_rx_unit` (both records carry both,
  07 §1.1–§1.2):
  - `pkts_copied_partial`: TX packets copied instead of attached; RX packets placed by CPU
    on a DMA-granted path;
  - `pkts_dma`: RX packets placed by the DMA engine. On TX it stays 0: no TX session
    uses a DMA engine at `545a266a`.
  - In `MTL_COMPLETE_NONE` the per-unit records are suppressed, and the same counters
    accumulate in the session stats (`tx.pkts_copied_partial`, `rx.pkts_dma`,
    `rx.pkts_cpu`, [08](08-observability.md)).

| Decision | Today | Unified API |
|---|---|---|
| chain (zero-copy) vs no-chain TX | at create: driver capability, `MTL_FLAG_TX_NO_CHAIN`, the frame-count heuristic; a log line only | granted `path` in `mtl_session_get_info`; `REQUIRE_DIRECT` fails instead |
| cvideo always copies | silent | reported as `COPY` |
| partial copies (line padding, PA page crossing) | silent, per packet (`st_tx_video_session.c:1274-1292`) | `pkts_copied_partial` per unit; path stays `DIRECT` |
| DMA offload → CPU fallback | silent, per packet | granted `DIRECT_DMA`; `pkts_dma` vs `pkts_copied_partial` per unit |
| internal SIMD converter vs plugin converter | log at create | granted `converter = {internal, plugin name}` and `convert_context` |

### 6.3 Internal buffers and memory status (r3)

`[C5 §6.6]`

CONVERT paths allocate internal transport frames next to the pool. st20p passes its frame
count to the transport (`st20_pipeline_tx.c:455`; RX `st20_pipeline_rx.c:564`), so a
converting session holds `count` app-format frames *and* `count` transport frames.
Example: 4K 4:2:2 10-bit transport = 20.7 MB, `YUV422PLANAR10LE` = 33.2 MB, `count = 3`.
That is 162 MB per converting session against 62 MB direct; 30 sessions need 4.9 GB against
1.9 GB of hugepages.

- `mtl_buffer_requirements` and `mtl_session_info` report `internal_buffer_count` and
  `internal_bytes` for the granted path. An RX attached pool whose `app_format` differs
  from the transport format reports its internal frames the same way.
- `mtl_instance_get_mem_status` reports memory per NUMA node:

  ```c
  int mtl_instance_get_mem_status(mtl_instance_h mt, uint32_t numa, struct mtl_mem_status* st);  /* CP; one node */
  struct mtl_mem_status {
    uint32_t struct_size;
    uint32_t numa;                    /* plain node number */
    uint64_t hugepage_size;
    uint64_t hugepages_total, hugepages_free;
    uint64_t library_bytes;           /* hugepage bytes MTL holds on this node */
    uint64_t internal_bytes;          /* of which transport-internal frames (CONVERT paths) */
    uint64_t imported_bytes;          /* regions imported on this node */
    uint64_t largest_free_segment;
    uint32_t regions_used, regions_free;   /* instance-wide region budget, §3.4 */
    uint64_t reserved[4];
  };
  ```

  Together with the dry-run query, a controller can check that a session fits before it
  creates it. Capacity in queues and lcores is `mtl_port_get_capacity` (08).

### 6.4 Where conversion and copies run

Never on a tasklet (R-THR-4). In v1 over the pipelines:

| Work | Where | Reported as |
|---|---|---|
| TX internal conversion | the application thread inside `mtl_tx_submit` (today's `put_frame` behaviour) — a DPC call (04 §3) | `convert_context = CALLER` |
| TX/RX plugin conversion, cvideo codec | plugin-owned threads | `convert_context = PLUGIN` |
| RX conversion into the app buffer | the application thread inside `mtl_rx_dequeue` (DPC), or a worker | `convert_context = CALLER / WORKER` |
| RX zero-fill of missing ranges (`ZERO_MISSING`) | the consumer's `mtl_rx_dequeue` (DPC) or a worker — never the RX tasklet, where a lost leg could mean a 5 MB memset `[C1 #16]` | `caller_work_ns` |
| RX per-packet conversion (`PKT_CONVERT`) | tasklet today | not offered in the unified API v1 |

The default `app_format` equals the transport format, so a first sample gets no
conversion `[C3 P1-5]`. A worker-pool option (so `submit` stays O(1)) is Q-THR-4.

## 7. When is a buffer reusable?

Research note 04 found five different "done" points today `[R04 §2.3, §6 #3]`:

| Path | MTL stops touching memory | Signal today |
|---|---|---|
| ST20 TX zero-copy | last chain mbuf freed after NIC completion | `notify_frame_done` from the free callback |
| ST20 TX no-chain, ST22, ST30/40/41 | after the last packet is copied into mbufs | `notify_frame_done` right after build |
| st20p TX with internal conversion | when conversion finishes, inside the app's own `put_ext_frame` | synchronous `notify_frame_done` |
| st20p TX with plugin conversion | when the plugin finishes | `notify_frame_done` on the plugin thread |
| RX | when the app calls put | — |

The unified contract (review §9, R-CMP-4):

1. **One terminal outcome per submission, produced after *both* storage access and the
   transport outcome are finished.** This needs the engine's once-only completion hook and
   the transmitter's "last packet handed to the NIC" hook for copy paths (04 §4.4). The
   result carries the transport instants (`timing.observed_last_tai_ns`, or
   `timing.enqueued_first_tai_ns` plus the expected duration), and the optional
   `MTL_CQE_TX_SOURCE_RELEASED` entry the storage instant, so the app can see the cost. An RX hold
   (§5.4) is dropped at this same instant.
2. **(r3) Early `SOURCE_RELEASED` is Phase 4, opt-in, for COPY and CONVERT paths.** A
   `MTL_CQE_TX_SOURCE_RELEASED` entry with `MTL_CQE_MORE` is posted when MTL stops reading
   the source, and the terminal result follows. This is the io_uring zero-copy-send model
   of two CQEs with a "more coming" flag `[R11 §4.1]`. It answers Q-MEM-1 with (b) in
   Phase 4, because the framework pool starvation of §5.6 needs it `[C5 §6.13]`. It is
   enabled with `completion.optional_kinds |= MTL_CQE_ENABLE_SOURCE_RELEASED`, and the
   unread-results capacity grows by one per unit while enabled (07 §2.1). DIRECT sessions
   reject the option with `-MTL_ENOTSUP`.
3. The st20p two-phase release (`ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE` +
   `st20p_tx_notify_ext_frame_free`) is subsumed: the lease *is* the release.

## 8. RX specifics

- **Attached pool = today's `ext_frames[]` array.** Attaching imported buffers to an RX
  session maps onto `st20_rx_ops.ext_frames` (dedicated) with the pipeline's `derive`
  path when formats match `[R04 §7 #1]`.
  - The engine takes `ext_frames` and `linesize` at create, so L2 hands them over through
    the slot-interface entry point M9 (§11) at attach, before start.
  - The dynamic `query_ext_frame` callback is a user callback on the tasklet that also
    forces incomplete-frame delivery (`st_rx_video_session.c:4287-4291`). It is replaced
    by attached pools plus `rx_slot_select = BY_INDEX` for index-addressed rings (MXL),
    with no code on the tasklet.
- **Every provided RX buffer returns.** Today an ext frame the transport drops (slot
  replacement, DMA busy, incomplete without the flag) is silently recycled and the app
  never learns `[R04 §6 #10]`. With attached pools the buffer stays in the pool and the
  loss is counted (`units_missed_before`); with dynamic provide (later) every provided
  buffer gets a terminal result, including `DISCARDED`.
- **Missing data.** Recycled RX frames are not re-zeroed today, so lost packets show the
  previous frame's pixels `[R04 §6 #11]`, contrary to the repository rule that gaps must
  read as zeros. The result carries completeness (`pkts_expected`, `pkts_received[leg]`,
  `missing[]`); library pools zero-fill missing ranges by default (in `dequeue`, §6.4);
  attached pools default to NONE because the app owns the memory (Q-MEM-6).
- **Latest-only monitoring** `[C5 §8.4]`: `pool.count = 2`,
  `rx_overflow = MTL_RX_RECLAIM_OLDEST_READY`. A slow consumer always dequeues the newest
  complete unit; HELD and APP_READING slots are never reclaimed.

## 9. Essence specifics

| Essence | Direct TX? | Direct RX? | Buffer shape | Notes |
|---|---|---|---|---|
| video (ST20) | yes (chain mode, VA or page-tabled PA) | packet placement (CPU/DMA copy) | 1 plane transport format, or up to 4 planes app format (conversion); interlaced: one field per unit (§4.3) | **(r3)** any stride ≥ `row_bytes` is DIRECT (§4.2) |
| cvideo (ST22) | no (always copied, `st_tx_video_session.c:3413-3416`) | no | codestream buffer (one plane; capacity = `codestream_bytes`, rounded up to whole packets, 06 §5.2) or raw frame before the encoder | encoder output buffers are internal |
| audio (ST30) | no (copied into packets) | no | one plane; capacity `buffer_capacity_bytes` (0 = 10 ms); RX units of `rx_unit_samples` (0 = 10 ms in whole packets) **(r3)** `[C5 §7.7]` | imported memory is cheap and uniform `[R04 §7 #10]`; `mtl_tx_write` avoids buffers entirely |
| ANC (ST40) | no | no | UDW bytes, capacity `max_udw_bytes` (0 = 64 KiB: 255 packets × 255 words); the packet table goes in the submission (TX) and the meta area (RX, `max_packets`, §4.4) **(r3)** | |
| fast metadata (ST41) | no | no | one data item group, capacity `buffer_capacity_bytes` (0 = 64 KiB); one data item is at most 511 words (2044 B; 9-bit length, `include/st41_api.h:94`) | |

Granted capacities and unit sizes are returned in `mtl_session_info` (`unit_bytes`,
`unit_samples`, `buffer_capacity_bytes`, `max_udw_bytes`), so an RX source can answer a
latency query and set buffer durations before the first dequeue `[C5 §7.7]`. The fields and
their defaults are in [09 §1.4](09-media-modes-and-backends.md) and §9.

## 10. Device memory (later)

- The region `domain` and the view's `domain` / NULL-`addr` / `device_handle` convention
  exist from v1 so device memory can be added without an ABI break `[C3 P5-4]`.
- **(r3) GPU pinned host memory is not device memory.** `cudaHostAlloc` and
  `zeMemAllocHost` return page-locked host memory that imports as `MTL_MEM_HOST` today
  (§3.2). It is the supported GPU ingest path in v1: RX DIRECT into pinned memory, then an
  async H2D copy.
  - Test G-row (13): import `mmap(MAP_ANONYMOUS | MAP_LOCKED)` memory as a stand-in, run
    RX DIRECT into it and TX DIRECT from it, and check the per-unit counters `[C5 §6.14]`.
- Today's "GPU direct" is Level Zero *shared* USM filled by CPU `memcpy`, RX only; there is
  no DMA-BUF, CUDA or gpudev support anywhere `[R04 §5]`. In the new model it is an RX
  buffer in the `MTL_MEM_DEVICE_SHARED` domain with path `COPY`.
  - FFmpeg's `ST20P_RX_FLAG_USE_GPU_DIRECT_FRAMEBUFFERS` option
    (`ecosystem/ffmpeg_plugin/mtl_st20p_rx.c:204-220`) has no v1 port. It is listed as
    deferred to the device-memory domain in [09 §8.1](09-media-modes-and-backends.md) and
    11 §6.3.
- DMA offload combined with GPU frames is not rejected today and would DMA to IOVA 0
  (`st_rx_video_session.c:2572-2575`, **[inferred]**). The region model rejects it by
  construction, because a DMA-engine mapping for the region is required.
- Rivermax detects GPU memory from the pointer and needs header/data split for GPUDirect
  `[R10 §7]`. MTL keeps the domain explicit.

## 11. Mapping onto today's internals (v1)

```text
 mtl_region (VA,len,maps,refcnt)       mtl_buffer (planes)               transport slot
 ───────────────────────────────       ───────────────────               ──────────────
 import: extmem_register;              validated once against            TX: st20p put_ext_frame (derive)
 dma_map per device, eager or at       the session requirements              → st20_tx_set_ext_frame(idx, {addr, iova, len})
 first attach (§3.5)                   resolves addr/iova (or page       RX: ext_frames[] + linesize via M9 at attach
 refcnt++ per buffer                   table in PA mode); stride →
                                       session linesize (§4.2)
 refcnt++ at bind, and per RX hold ───────────────────────────────────────▶
 refcnt-- at the once-only completion hook (TX) / rv_put_frame (RX, last of release + holds) / DMA drain
```

Fixes the mapping depends on (all found in research, all small):

| # | Fix | Evidence |
|---|---|---|
| M1 | In `tv_frame_free_cb`: claim completion with a CAS and decrement `refcnt` / clear `addr/iova` *before* calling any completion, so a completion can re-arm the slot safely and recovery cannot double-report | `st_tx_video_session.c:116-141`; st20p sets FREE before calling the app `st20_pipeline_tx.c:264-276` `[R04 §6 #2, C1 #11]` |
| M2 | Map regions into every device the region is used with (§3.5) | `mt_main.c:864-865` |
| M3 | Fix the `mt_map_add` overlap check (misses a new range that encloses an old one) | `lib/src/mt_dma.c:32-41` `[R04 §4.3]` |
| M4 | Stop silent recycle of RX ext frames; report | `st_rx_video_session.c:977-981` |
| M5 | Reject DMA offload for regions not mapped into the DMA engine | `st_rx_video_session.c:2572-2575` |
| M6 | Validate `buf_len` for RX dedicated ext frames (IOVA 0 / `MTL_BAD_IOVA` are already rejected at `:446-450`) | `st_rx_video_session.c:439-450` |
| M7 | Guarantee a builder-held reference per frame so the extbuf count cannot reach zero mid-frame in slice mode | `[R04 Q9]`, **[unknown]** whether it can today |
| M8 | Pipeline held-slot state, once-only completion hook, flush reclaim | 04 §4.4 `[C1 #1]` |
| M9 **(r3)** | Slot-interface entry points to set the transport `linesize` and, for RX, the dedicated `ext_frames[]` after create and before start. The engine takes both at create today, and non-GPM_SL packing sizes its copy-chain mempool from the linesize | `st_tx_video_session.c:3333-3339`, `:2981-2995`; `st_rx_video_session.c:3343-3351`, `:439-450` |
| M10 **(r3)** | RX hold count and HELD state in the pipeline slot, with the last-reference CAS to FREE | §5.4; `rv_put_frame` `st_rx_video_session.c:222-231` |
| E11 **(r3)** | Dynamic ST20/ST22 frame arrays; lift `ST20_FB_MAX_COUNT` / `ST22_FB_MAX_COUNT` (engine change, reaches legacy users) | `include/st20_api.h:24`, `:29`; `st_tx_video_session.c:4073`, `st_rx_video_session.c:4266` |
| E12 **(r3)** | Grow the ST40 RX meta array beyond `ST40_MAX_META = 20` (with E11) | `include/st40_api.h:308` |

E1–E10 and E13 (fast-metadata rate) are the timing engine changes of
[06 §14](06-timing-pacing-and-sync.md); [14 §2](14-implementation-roadmap.md) says which of them
legacy users get.

## 12. What this fixes from PR #1610

| PR #1610 | This design |
|---|---|
| `MTL_BUFFER_LIBRARY_OWNED` / `USER_OWNED` session mode | provisioning choice per pool; one lease state machine |
| `buffer_post(data, size, ctx)` | buffer handle with explicit planes + submission with media time and cookie |
| `mem_register` = address lookup, no DMA map, no refcount; accepts unregistered pointers in VA mode | region import maps every device it is used with, refcounted, `-MTL_EBUSY` on destroy; alignment, access and budget checked (§3) |
| user-owned RX = copy inside the RX tasklet | copy/convert/zero-fill never on the tasklet; path reported |
| user TX backlog advances only on the next `buffer_post` | every accepted unit progresses without another call (review §14 #3) |
| zero-copy samples mmap page-cache files and unmap before destroy | regular-file mappings are detected and accepted as COPY only (§3.2); the teardown order is stated and enforced by refcounts (§3.7) |
