# Research note 04: memory, buffers, DMA and external frames (current state)

| Field | Value |
|---|---|
| Topic | Memory modes, frame lifecycles, copies, DMA mapping, header split, GPU direct |
| Revision | 1 |
| Date | 2026-09-29 |
| Baseline | `main` @ `545a266a` |
| Scope | `include/*.h`, `lib/src/mt_main.c`, `lib/src/mt_dma.c`, `lib/src/mt_mem.h`, `lib/src/mt_util.c`, `lib/src/st2110/**`, `lib/src/dev/mt_af_xdp.c`, `lib/src/datapath/mt_dp_socket.c`, `gpu_direct/` |
| Reads with | `00-pr1610-design-review.md` (maintainer review, "review §N") |

Labels: **[verified]** = read in code at the cited `path:line`; **[inferred]** = follows from verified code but not exercised at runtime; **[unknown]** = not established.
Line numbers refer to `HEAD` (`545a266a`). The checkout had uncommitted edits in several cited files (bitmap bounds, stats, RTCP parsing; none in the memory paths), so citations were remapped from the working tree to `HEAD` with a line diff.
All paths are relative to the repository root. `st_tx_video_session.c` etc. live in `lib/src/st2110/`; pipeline files in `lib/src/st2110/pipeline/`.

## 0. Summary

- There is no memory object in MTL today. There are three unrelated mechanisms: (a) library `rte_zmalloc` frames, (b) raw `{addr, iova, len}` triples (`st20_ext_frame`, `st_ext_frame`) that MTL trusts, and (c) a global map table filled by `mtl_dma_map()` which nothing on the data path consults. **[verified]**
- External memory exists only for ST20/ST22 video. ST30, ST40 and ST41 (session and pipeline) have no external-frame facility. **[verified]**
- The one real "safe to reuse" point is the TX video zero-copy path: `notify_frame_done` fires from the mbuf extbuf free callback, after the last chain mbuf is freed (i.e. after NIC DMA read and descriptor recycle). Every other path signals "done" at a different, weaker point (after the copy into mbufs, after conversion, or synchronously inside `put_ext_frame`). **[verified]**
- `mtl_dma_map()` works only in IOVA-VA mode, only issues `rte_dev_dma_map()` on `MTL_PORT_P`, invents IOVAs from `0x10000` upward, has an incomplete overlap check, and holds no references: unmapping memory that is still in a TX descriptor or DMA ring is not prevented. **[verified]**
- Whether a frame is transmitted zero-copy or copied is decided silently at session create (driver multi-seg capability, IOVA mode, frame-count heuristic, ST22 always copy). The application can only find out from logs or aggregate stats. **[verified]**
- Header split is compiled only with an out-of-tree DPDK patch that is not shipped for the pinned DPDK 26.07. "GPU direct" is Level Zero USM *shared* memory written by the CPU, not NIC-to-VRAM DMA. No DMA-BUF support exists anywhere. **[verified]**

## 1. Memory modes per media type and layer

### 1.1 Common allocation primitives

| Primitive | Implementation | Alignment | IOVA source |
|---|---|---|---|
| `mt_rte_zmalloc_socket(sz, socket)` | `rte_zmalloc_socket(name, sz, RTE_CACHE_LINE_SIZE, socket)` `lib/src/mt_mem.h:38-39` | 64 B | `rte_malloc_virt2iova` / `rte_mem_virt2iova` |
| `mtl_hp_malloc/zmalloc` | `mt_rte_(z)malloc_socket(size, mt_socket_id(impl, port))` `lib/src/mt_main.c:764-796` | 64 B | `mtl_hp_virt2iova` = `rte_malloc_virt2iova` `lib/src/mt_main.c:803-806` (start only) |
| `mtl_dma_mem_alloc` | libc `mt_zmalloc(iova_size + page_size)`, page-align, `mtl_dma_map` `lib/src/mt_main.c:929-971` | page | MTL-invented IOVA (see §4) |
| `mtl_dma_map` | map-table insert + `rte_extmem_register` + `rte_dev_dma_map(port P)` `lib/src/mt_main.c:819-879` | page (addr and size) | MTL-invented IOVA |
| GPU (`gpu_allocate_shared_buffer`) | `zeMemAllocShared(..., 16, ...)` `gpu_direct/gpu.c:213` | 16 B | none (iova left 0) |

In IOVA-VA mode `rte_malloc_virt2iova(p) == p`, so hugepage memory needs no explicit map. In IOVA-PA mode a multi-hugepage `rte_malloc` block is not guaranteed IOVA-contiguous, which is why MTL builds per-frame page tables for its own frames (`tv_frame_create_page_table` `st_tx_video_session.c:160-201`, `rv_frame_create_page_table` `st_rx_video_session.c:350-391`).
**[verified]** External frames never get a page table (only the `ST_FT_FLAG_RTE_MALLOC` branch calls it: `st_tx_video_session.c:265-266`, `st_rx_video_session.c:489-495`). **[verified]**

### 1.2 Mode table

"Stop touching" is the point after which MTL no longer reads/writes that memory. "Signal" is how the app learns it.

| # | Media / layer / dir | Mode | Allocator | Requirements & validation | MTL stops touching | Signal to app |
|---|---|---|---|---|---|---|
| 1 | ST20 session TX | internal frames | `mt_rte_zmalloc_socket(st20_fb_size)` `st_tx_video_session.c:245` | none from app | chain mode: extbuf refcnt reaches 0 = last chain mbuf freed after NIC completion `st_tx_video_session.c:116-141`; no-chain mode: after last packet copied `:2130-2134` | `notify_frame_done(idx)` from the free-cb context (see §2.1) |
| 2 | ST20 session TX | `ST20_TX_FLAG_EXT_FRAME` + `st20_tx_set_ext_frame(idx)` | app | see §6 item 6: `buf_len` `:4520`, `addr` `:4527`, `iova` non-0/BAD `:4533`, slot refcnt 0 `:4557`; in-transport check only warns `:4539-4543` | as #1; ext `addr/iova` cleared after callback `:136-139` | `notify_frame_done(idx)`; re-arm with `set_ext_frame` per use |
| 3 | ST20 session TX | RTP level (`st20_tx_get_mbuf/put_mbuf`) | MTL mempool, app writes into mbuf data room | `len` only | when mbuf freed by driver | none (mbuf ownership passes on `put_mbuf`) |
| 4 | ST22 session TX | internal codestream frames | `mt_rte_zmalloc_socket` `:245`, boxes pre-copied `:252-262` | none; **no ext mode** | after last packet copied: ST22 forces `tx_no_chain = true` `:3413-3416` | `notify_frame_done(idx)` |
| 5 | ST20 session RX | internal frames | `mt_rte_zmalloc_socket(size)` `st_rx_video_session.c:472`, zeroed once | none | when app calls `st20_rx_put_framebuff(addr)` `:4692-4715` | `notify_frame_ready(addr, meta)` hands ownership `:939`, `:978` |
| 6 | ST20 session RX | dedicated `ops.ext_frames[framebuff_cnt]` | app | `addr != NULL` `:439-444`; `iova != 0 && != BAD` `:445-450` (required even if only CPU copy is used). **`buf_len` not checked**; array length implicit | as #5 | as #5 |
| 7 | ST20 session RX | dynamic `query_ext_frame` (tasklet callback) | app, per frame | requires `ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` `:4287-4292`; only `buf_len >= fb_size` checked `:1279`; **addr and iova unchecked** | as #5 | `notify_frame_ready`; `meta->opaque` carries app ID `:1289` |
| 8 | ST20 session RX | header split (`ST20_RX_FLAG_HDR_SPLIT`) | one big per-port pool, MTL or `ext_frames[port]` `:245-313` | 1 port only `:4293-4298`; BPM packing only; `ext_frames[]` reinterpreted as per-*port* pools; `buf_len >= mbufs_total*BPM + 4096` `:287` | NIC writes payload directly; app returns via put | as #5 |
| 9 | ST20 session RX | RTP level (`st20_rx_get_mbuf`) | MTL RX mempool | none | on `st20_rx_put_mbuf` | `notify_rtp_ready` |
| 10 | ST20 session RX | GPU (`gpu_direct_framebuffer_in_vram_device_address`) | `zeMemAllocShared` `:462-468` | only built `#ifdef MTL_GPU_DIRECT_ENABLED`; iova stays 0 `:480-484` | as #5 (CPU `mt_memcpy` into USM) | as #5 |
| 11 | ST22 session RX | internal codestream | as #5 | **no ext mode** (`st22_rx_ops` has no ext fields, `include/st20_api.h:1632-1700`) | on `st22_rx_put_framebuff` | `notify_frame_ready` |
| 12 | ST30 session TX | internal | `mt_rte_zmalloc_socket(st30_frame_size)` `st_tx_audio_session.c:105` | none; **no ext** | after last packet copied into mbuf `:496`, `:530`, `:915-931` | `notify_frame_done(idx)` |
| 13 | ST30 session RX | internal | `mt_rte_zmalloc_socket` `st_rx_audio_session.c:143` | none; **no ext** | on `st30_rx_put_framebuff` `:1771` | `notify_frame_ready` |
| 14 | ST40 session TX | lib allocates `struct st40_frame` only; UDW payload is an arbitrary app pointer `st40_frame.data` `include/st40_api.h:313-318` | app (any memory) | none on `data` | after last packet built (copied) `st_tx_ancillary_session.c:1141-1147` | `notify_frame_done` |
| 15 | ST40 session RX | frame mode: per-slot `udw_buf` `st_rx_ancillary_session.c:924`; or RTP ring of mbufs `:659` | MTL | none | put framebuff / put mbuf | `notify_frame_ready` / `notify_rtp_ready` |
| 16 | ST41 session TX/RX | as ST40 (`st41_frame.data` app pointer `include/st41_api.h:126-129`) | app / MTL | none | after copy | callbacks |
| 17 | st20p TX | internal src (`st_frame_size(input_fmt)`) | `st20_pipeline_tx.c:575` | none | derive: transport done (#1); convert: after conversion | `notify_frame_done(st_frame*)` if set; slot back to FREE |
| 18 | st20p TX | `ST20P_TX_FLAG_EXT_FRAME` + `put_ext_frame` | app | derive: `addr[0]/iova[0]/size` to #2, `linesize` ignored `st20_pipeline_tx.c:958-970`; convert: `st_frame_sanity_check` `st_fmt.c:615-660` | derive: #1; plugin: after convert `:380-385`; internal: in `put_ext_frame` `:992-1000` | `notify_frame_done`; `MANUAL_RELEASE` parks slot until `notify_ext_frame_free` |
| 19 | st20p RX | internal dst | `st20_pipeline_rx.c:701` (derive: transport frame is the dst) | none | on `st20p_rx_put_frame` | `st20p_rx_get_frame` |
| 20 | st20p RX | `ext_frames` / `query_ext_frame` | app | derive: static array mapped `addr[0]/iova[0]/size` only, **opaque dropped** `:582-586`; non-derive: sanity check | on `put_frame` | `get_frame`; dropped frames never reported |
| 21 | st22p TX/RX | ext frames for raw (pre-encode / post-decode) side only | app | sanity check `st22_pipeline_tx.c:934`, `st22_pipeline_rx.c:158` | TX: held until transport done of the *encoded* frame (`st22_pipeline_tx.c:265-279`) | `notify_frame_done` / `get_frame` |
| 22 | st30p / st40p | pipeline frame = transport frame (no pipeline alloc, st40p TX has its own UDW buffer `st40_pipeline_tx.c:382`) | MTL | **no ext mode** (`include/st30_pipeline_api.h`, `include/st40_pipeline_api.h`) | after copy into mbufs | `notify_frame_done` / `get_frame` |

Pipeline-row line numbers come from a quick sweep; the ones marked with exact excerpts above (`st20_pipeline_tx.c:250-290`, `:360-390`, `:940-1005`, `st_fmt.c:615-660`, `:1283-1297`, `st20_pipeline_rx.c:576-592`) were re-read and are **[verified]**; the rest are **[verified in a quick read, spot-checked]**.

## 2. Frame lifecycle state machines as implemented

### 2.1 Session TX video (`struct st_frame_trans`, `st_header.h:137-163`)

Fields relevant to memory: `addr`, `iova`, `page_table/page_table_len` (PA only), `refcnt` (atomic, "0 means free"), `flags` (`ST_FT_FLAG_RTE_MALLOC | EXT | GPU_MALLOC`, `st_header.h:123-127`), `sh_info` (DPDK `rte_mbuf_ext_shared_info`, one per frame). **[verified]**

```text
            app sets ext (EXT only)            builder: get_next_frame(idx)
 FREE(refcnt=0) ------------------------> FREE --------------------------------> SENDING(refcnt=1)
   ^   st20_tx_set_ext_frame :4502            refcnt must be 0 :1936-1944          | per pkt:
   |                                          rte_atomic32_inc :1963               |  attach_extbuf + sh_info++ :1293-1299
   |                                                                               |  (or memcpy into mbuf :1275-1292)
   |   tv_frame_free_cb :116-141  <---- sh_info refcnt 0 (DPDK calls free_cb) -----+
   |     1. check frame refcnt==1                                                  |
   |     2. notify_frame_done (app callback)          no-chain: called directly    |
   |     3. refcnt-- (FREE)                           after last pkt built :2130   |
   |     4. EXT: addr=NULL, iova=0                                                 |
   +-------------------------------------------------------------------------------+
 Recovery/stop: tv_notify_frame_done + refcnt-- + sh_info=0 forced :4291-4301
```

- Session-level build state is `ST21_TX_STAT_WAIT_FRAME -> SENDING_PKTS -> WAIT_FRAME` (`st_tx_video_session.c:1912-1966`, `:2122-2126`). **[verified]**
- `sh_info` starts at 0 (`:237`) and is only incremented per attached packet (`:1298`, `:1709`). There is no "builder hold" reference, so `free_cb` fires whenever the count touches 0.
  **[verified]** Because E810 recycles descriptors lazily (KB §7 pad-flush), the count normally cannot touch 0 mid-frame; whether it can during a slice-mode stall while pads keep the ring moving is **[unknown]** (see Q9).
- Redundant port R packets share the P chain mbuf (`rte_mbuf_refcnt_update(pkt_chain, 1)` `:1353-1357`), so the extbuf count is per packet, not per port; `free_cb` waits for both NICs. **[verified]**
- The callback context is whoever frees the last mbuf: normally the transmitter tasklet inside `rte_eth_tx_burst`/pad flush, but also control-plane paths (`tv_transmitter_port_state_cleanup` `:3055-3070`, recovery `:4291-4301`). **[verified]**

### 2.2 Session RX video

```text
FREE(0) --rv_get_frame (scan, inc) :205-220--> ASSEMBLING(1) --complete--> notify_frame_ready(addr) --> APP(1)
                                                    |  (dynamic ext: query_ext_frame :1259-1290,     |
                                                    |   fail -> refcnt-- -> FREE, pkt dropped)        |
                                                    +--incomplete & !RECEIVE_INCOMPLETE--> put -> FREE |
APP --st20_rx_put_framebuff(addr) :4692 (lookup by addr) --> rv_put_frame refcnt-- :222-232 --> FREE <-+
notify_frame_ready < 0 on complete frame -> rv_put_frame :939-944; on incomplete frame the return is ignored :978
```

- DMA: frame completion waits for `mt_dma_empty()` (`rv_dma_dequeue` `:1507-1528`, `:1845`); a new timestamp is dropped while the previous frame still has DMA in flight (`:1202-1208`). **[verified]**
- Frame buffers are zeroed only on allocation (`rte_zmalloc`); a recycled frame keeps the previous frame's bytes in gaps. **[verified]** (KB §3 says the same.)
- Why dynamic ext requires `RECEIVE_INCOMPLETE_FRAME`: without it an incomplete frame is `rv_put_frame`'d silently (`:977-981`) and the app's queried buffer (and its `opaque`) is never handed back. **[inferred]**

### 2.3 Pipeline states (all use `_Atomic uint32_t stat` + CAS claim, no mutex)

| Pipeline | States (header) | Notes |
|---|---|---|
| st20p TX | FREE, READY, IN_CONVERTING, CONVERTED, DROPPED, IN_USER, IN_TRANSMITTING (`st20_pipeline_tx.h:11-20`) | IN_TRANSMITTING -> FREE, or -> IN_USER with `MANUAL_RELEASE` (`st20_pipeline_tx.c:250-290`) |
| st20p RX | FREE, READY, IN_CONVERTING, CONVERTED, IN_USER (`st20_pipeline_rx.h:11-18`) | PKT_CONVERT uses plain store FREE->IN_CONVERTING |
| st22p TX | FREE, IN_USER, READY, IN_ENCODING, ENCODED, DROPPED, IN_TRANSMITTING (`st22_pipeline_tx.h:11-20`) | ext src held until transport done |
| st22p RX | FREE, READY, IN_DECODING, DECODED, IN_USER (`st22_pipeline_rx.h:11-18`) | |
| st30p TX / st40p TX | FREE, IN_USER, READY, DROPPED, IN_TRANSMITTING (`st30_pipeline_tx.h:16-23`, `st40_pipeline_tx.h:20-27`) | frame is the transport frame |
| st30p RX / st40p RX | FREE, READY, IN_USER (`st30_pipeline_rx.h:11-16`, `st40_pipeline_rx.h:15-20`) | |

st20p TX with external memory, the three "done" points:

```text
put_ext_frame ──derive──> st20_tx_set_ext_frame ─> CONVERTED ─> IN_TRANSMITTING ─> [NIC done] ─> frame_done ─> notify_frame_done   (reusable: yes)
              ──internal converter──> convert_func() in caller thread ─> notify_frame_done(src) immediately       (src reusable: yes; network not done)
              ──plugin converter──> READY ─> plugin converts ─> convert_put_frame ─> notify_frame_done(src)        (src reusable: yes; network not done)
```

The pipeline sets slot state *before* the callback (`st20_pipeline_tx.c:264-276`) so the app can release from inside the callback. **[verified]**

## 3. Where copies happen

| Path | Copy | Where | Visible to app? |
|---|---|---|---|
| TX video, chain + VA mode | none (extbuf) | `st_tx_video_session.c:1293-1299` | no |
| TX video, packet crosses line padding (`linesize > bytes_in_line`) | CPU copy into `mbuf_mempool_copy_chain` | `:1275-1288` | no (per packet) |
| TX video, PA mode, payload crosses hugepage | CPU copy into chain data room (`chain_room_size = st20_pkt_len` `:2922-2924`) | `:1289-1292` | no |
| TX video, `tx_no_chain` (`MTL_FLAG_TX_NO_CHAIN`, driver lacks `TX_OFFLOAD_MULTI_SEGS`, or `total_pkts*(frames_cnt-1) < nb_tx_desc`) | whole frame copied into mbufs | decision `:3413-3421`, `:2861-2895` | info log only (`:3422-3424`) |
| TX ST22 session (all) | whole codestream copied | forced `:3413-3416`, copy `:1622` | no |
| TX ST30/ST40/ST41 | always copied into mbufs | e.g. `st_tx_audio_session.c:496`, `:530` | no |
| TX native AF_XDP | every packet (all segments) copied into a umem-backed mbuf, original freed at once | `lib/src/dev/mt_af_xdp.c:555-597` (`stat_tx_copy++`) | stats |
| TX kernel socket | single-seg only (`nb_segs>1` rejected `mt_dp_socket.c:36-38`), `sendto/sendmsg` copies into kernel | `mt_dp_socket.c:79`, `:132`, `:148`, `:166` | no |
| RX video default | CPU `mt_memcpy` mbuf -> frame | `st_rx_video_session.c:1826`, also `:2220`, `:2417` | no |
| RX video DMA offload | CBDMA/DSA copy when payload > 1024 B, ring not full, not cross-page; else CPU | `:1804-1827`, `ST_RX_VIDEO_DMA_MIN_SIZE` `st_rx_video_session.h:10` | aggregate `stat_pkts_dma` `include/st20_api.h:1763` |
| RX video header split | none when payload lands in place; CPU copy on mismatch | `:2398-2414` | `stat_pkts_copy_hdr_split` `include/st20_api.h:1770` |
| RX kernel socket | `recvfrom` into mbuf, then mbuf -> frame (2 copies) | `mt_dp_socket.c:555` | no |
| RX GPU "direct" | CPU `mt_memcpy` into USM shared memory | same as default | no |
| st20p / st22p convert | internal SIMD converter (app thread in `put_frame`/`get_frame`) or plugin thread | `st20_pipeline_tx.c:992`, `st_plugin.c:269-275` | no public "derive" query (`grep derive include/` is empty) |
| st20p RX PKT_CONVERT | per packet conversion in tasklet | `st20_pipeline_rx.c:130-164` | no |

**Conclusion:** the app cannot tell, per session or per frame, which path was selected. The selection is irreversible and happens at create time, with a heuristic (`tv_pkts_capable_chain`) that silently downgrades small formats or low `framebuff_cnt` to copy mode. **[verified]**

## 4. DMA mapping details

### 4.1 `mtl_dma_map(mt, vaddr, size)` (`lib/src/mt_main.c:819-879`)

1. Rejects non-page-aligned `vaddr` or `size` (`:830-838`). Page size is `sysconf(_SC_PAGESIZE)` (`:582-584`), i.e. 4 KiB, not the hugepage size. **[verified]**
2. Rejects any IOVA mode other than VA (`:840-843`). With no IOMMU (vfio no-iommu / PA) external mapping is impossible. **[verified]**
3. `mt_map_add` (`lib/src/mt_dma.c:14-69`) under a pthread mutex: checks overlap, then assigns `iova = max(existing iova_end, 0x10000)` (comment says "1M", value is 64 KiB, `:21`). The IOVA is **not** the VA. Table limit `MT_MAP_MAX_ITEMS = 256` (`lib/src/mt_main.h:62`). **[verified]**
4. Non-DPDK primary port (kernel, native AF_XDP): returns the invented IOVA with no mapping at all (`:858-860`). **[verified]**
5. `rte_extmem_register(vaddr, size, NULL, 0, page_size)` (`:862`), so DPDK's memseg for it has no IOVA table (`rte_mem_virt2iova` on it yields bad IOVA). **[inferred from DPDK semantics]**
6. `rte_dev_dma_map(mt_port_device(impl, MTL_PORT_P), ...)` "only map for MTL_PORT_P now" (`:869-870`). **[verified]**

`mtl_dma_unmap` requires the exact `(vaddr, size, iova)` triple (`mt_dma.c:83`), unmaps port P, unregisters extmem (`mt_main.c:881-927`). `mt_map_uinit` frees leftover items with only a warning (`mt_dma.c:111-126`) and does not unmap them. **[verified]**

### 4.2 Multi-port, multi-NIC, DMA engines

- In DPDK 26.07, `pci_dma_map` falls back to `rte_vfio_container_dma_map(RTE_VFIO_DEFAULT_CONTAINER_FD, ...)` for vfio-bound devices whose driver has no `dma_map` op
  (`drivers/bus/pci/pci_common.c:478-493`, in the DPDK 26.07 source).
  ice/iavf and the ioat/idxd dmadevs are in the default container, so one map on port P is in practice visible to port R and to the CBDMA/DSA engine used for RX offload.
  **[verified in DPDK source; per-driver op presence inferred]**
- This works by accident of VFIO container sharing. A driver with its own `dma_map` op (e.g. mlx5 registers memory per device), a device in a non-default container, or a primary port that is not DPDK-based while R is, would leave the other device unmapped. **[inferred]**
- No NUMA or device-affinity check on the mapped region. **[verified]** (absent from `mtl_dma_map`)
- The invented IOVA range is only checked against other user maps, not against DPDK's own IOVA=VA hugepage space. A collision is unlikely because hugepage VAs are high, but nothing prevents it. **[inferred]**

### 4.3 Validation holes in the map table

- The overlap check (`mt_dma.c:32-41`) tests "new start inside old" and "new end inside old", but not "new range strictly encloses old". **[verified]**
- No reference count, no owner, and no link from sessions or frames to map items. Data-path code never looks the table up. Unmapping memory that is still referenced by TX descriptors, DMA descriptors or an RX frame succeeds. The IOMMU then faults on the NIC/DMA access (possible E810 MDD event and VF reset). **[verified absence; consequence inferred]**
- Ext frames are not checked to lie inside a mapped region. Any non-zero IOVA is accepted (`st_tx_video_session.c:4533`, `st_rx_video_session.c:446`). **[verified]**

### 4.4 IOVA use by path

| Consumer | Needs IOVA? | Where computed |
|---|---|---|
| TX chain extbuf | yes, per packet `iova + offset`, or page table in PA mode | `tv_frame_get_offset_iova` `st_tx_video_session.c:143-158` |
| TX no-chain / ST22 / ST30/40/41 | no (CPU copy) | n/a |
| RX CPU copy | no, but dedicated ext frames still *require* it (`st_rx_video_session.c:446`) | n/a |
| RX DMA offload | yes, dst `iova + offset` (page table only for internal PA frames) | `rv_frame_get_offset_iova` `:332-348`, `:1810` |
| RX header split | yes, pool base `frames_iova + idx*BPM` | `:582-583` |
| Pipeline internal planes | `iova[p] = iova[p-1] + linesize*h` (assumes contiguity) | `st_fmt.c:1283-1297` |
| Pipeline transport-side `st_frame` given to plugins | not filled (iova 0) | quick read: `st20_pipeline_tx.c:494`, `st20_pipeline_rx.c:232` |

## 5. Header split, GPU direct, DMA-BUF

**Header split.** Code is fenced by `ST_HAS_DPDK_HDR_SPLIT` (`st_rx_video_session.c:548`, `lib/src/dev/mt_dev.c:1193`, `:1697`, `:1916`, `lib/src/mt_main.h:335`). That macro is defined only by `patches/dpdk/<ver>/hdr_split/0001-...-hdr-split-mbuf-callback.patch` (e.g. `patches/dpdk/26.03/hdr_split/...patch:211`).
`patches/dpdk/26.07/` (pinned in `versions.env`) has no `hdr_split/` directory, and `script/check_dpdk_patches.sh:35` says the subdirectory is applied by "other flows". **[verified]**

How it works: an ice mbuf-alloc callback points each RX mbuf's `buf_addr/buf_iova` at `frames + idx*ST_VIDEO_BPM_SIZE` in one contiguous pool (`st_rx_video_session.c:550-596`). The frame address is then resolved from the first payload (`:1256-1258`).
This implies: one port only, BPM packing only, a fixed 1:1 packet-to-slot layout, and a pool of `frames_cnt*mbufs_per_frame + (mbufs_per_frame-1)` slots. Frames are not independent buffers, so the model cannot map onto a per-buffer handle. **[verified]**

**GPU direct.** This is a separate Level Zero helper library (`gpu_direct/gpu.c`, optional meson dependency `lib/meson.build:21-23`).

- The only library hook is `ST20P_RX_FLAG_USE_GPU_DIRECT_FRAMEBUFFERS`. It sets `gpu_direct_framebuffer_in_vram_device_address` and `gpu_context` (`include/st20_api.h:1624-1625`, `st20_pipeline_rx.c:549-552`).
- The RX session then allocates frames with `gpu_allocate_shared_buffer` = `zeMemAllocShared` (`gpu_direct/gpu.c:201-219`) and CPU-copies packets into them. Frames are freed via `gpu_free_buf` in `st_frame_trans_uinit` (`lib/src/mt_util.c:900-905`).
- No NIC or DMA engine ever targets device memory, and there is no TX GPU path. **[verified]**
- `ST20_RX_FLAG_DMA_OFFLOAD` is not rejected when combined with GPU frames. `rv_init_sw` gates only on frame type, uframe and header split (`st_rx_video_session.c:2572-2575`), and GPU frames have `iova == 0`. So combining the two would DMA to IOVA `0 + offset`. **[inferred]**

**DMA-BUF / CUDA / gpudev.** A repository search for `dmabuf|dma_buf|udmabuf|rte_gpu|gpudev|cuda` over `lib include gpu_direct app ecosystem plugins` finds only FFmpeg's vendored `compat/cuda` header. There is no DMA-BUF import, no `rte_gpu`, and no CUDA. **[verified]**

## 6. Flaws and gaps relevant to the new API

The most consequential come first.

1. **No retained memory object.** `mtl_dma_map` returns a bare IOVA with no handle and no refcount, so unmap-while-in-flight is undetected (§4.3). This directly violates review §14 items 9 and 10. **[verified]**
2. **Race between the TX ext-frame callback and the app re-arming the slot.**
   - `tv_frame_free_cb` calls `notify_frame_done` first, then decrements `refcnt`, then clears `addr/iova` (`st_tx_video_session.c:134-139`).
   - The st20p pipeline has already set the slot FREE before calling the app (`st20_pipeline_tx.c:264-276`).
   - An app thread can therefore do `get_frame` + `put_ext_frame` (-> `st20_tx_set_ext_frame`) in that window. If it runs before the decrement it fails with "not free" (-EIO). If it runs after the decrement but before the clear, its new `addr` is overwritten with NULL, and the builder later attaches `NULL + offset`.
   - **[inferred from code order; not reproduced]**
3. **The "done" meaning is path dependent.**
   - TX zero-copy: after NIC completion.
   - No-chain, ST22, ST30/40/41: after copy.
   - st20p convert: after conversion, possibly on the plugin thread or synchronously in the caller's `put_ext_frame`.
   - `notify_frame_done` therefore runs in three different thread contexts. The API doc claims tasklet only (quick read: `st_pipeline_api.h:925-926`). **[verified]**
4. **Silent zero-copy downgrade.** `tx_no_chain` is chosen at create from driver capability, a user flag, or the `total_pkts*(frames_cnt-1) < nb_tx_desc` heuristic (`st_tx_video_session.c:2873-2895`). ST22 is always copied. PA mode copies cross-page packets. None of these choices is reported or rejectable. There is no `REQUIRE_DIRECT` (review §10). **[verified]**
5. **The lazy-completion latency is invisible.** In chain mode a frame becomes reusable only once the NIC ring recycles its descriptors, roughly `nb_tx_desc` packets later or at a pad flush. That is why `tv_pkts_capable_chain` demands at least 2 frames' worth of packets beyond the ring. A new API must size pools with this and publish the minimum buffer count.
   **[inferred from `:2884-2889` and KB §7]**
6. **Ext-frame validation holes.**
   - RX dedicated: `buf_len` is unchecked.
   - RX dynamic: `addr` and `iova` are unchecked, and with DMA offload `iova=0` means a DMA write to IOVA 0.
   - TX: no check that the range is mapped, no PA-mode contiguity check, and the "in transport" check is only a warning.
   - Pipeline: no alignment, span, or plane-overlap check (`st_fmt.c:615-660`).
   - Derive mode ignores `ext_frame->linesize` and uses only `addr[0]`.
   - **[verified]**
7. **Planar model limited.**
   - Transport frames are single-plane `{addr, iova, len}`.
   - Internal planar frames assume IOVA contiguity (`st_fmt.c:1294`), which is wrong in PA mode for multi-hugepage frames.
   - Plugins receive `iova=0` on the transport-side frames.
   - **[verified]**
8. **ST30/ST40/ST41 have no external-memory facility at any layer.** ST40/41 TX accept an arbitrary `data` pointer (copied, never validated). RX st40p exposes the session's `udw_buf` directly. **[verified]**
9. **Header split is effectively dead code** on the pinned DPDK. It supports one port only and has a pool layout incompatible with per-buffer handles (§5). **[verified]**
10. **RX loss of ownership.**
    - `notify_frame_ready` return is ignored on incomplete frames (`st_rx_video_session.c:978`).
    - Dynamic ext buffers are stranded when a pipeline sanity check fails after a successful query.
    - Frames dropped by the transport before delivery are never reported, so an app that handed out a buffer via `query_ext_frame` gets no terminal result.
    - Review §9 rule 4 is violated on RX. **[verified in a quick read; `:978` re-read]**
11. **Recycled RX frames are not re-zeroed**, so missing packets read as the previous frame's content. **[verified]**
12. **The DMA lender teardown does not drain.** `mt_dma_free_dev` just deactivates the lender (`lib/src/mt_dma.c:438-460`). The only wait is the tasklet's `mt_dma_empty` check (`st_rx_video_session.c:2945`). Destroying a session with borrowed mbufs, while another session keeps the shared DMA device alive, leaves copies targeting frames that `rv_free_frames` then frees. **[inferred]**
13. **`mtl_dma_map` covers port P only** and relies on the VFIO default container for everything else (§4.2). **[verified/inferred]**
14. **GPU "direct" is a misnomer.** It is CPU copy into USM, with no TX path, and the DMA-offload + GPU combination is unguarded (§5). **[verified/inferred]**
15. **Pipeline memory bugs found in passing** (quick read, not re-verified):
    - st40p TX leaks UDW buffers.
    - st40p TX has a double free on the create-failure path.
    - st40p RX `meta_num` is not clamped.
    - st20p RX PKT_CONVERT + EXT_FRAME writes through a NULL dst.
    - `st20p_rx_get_fb_addr` returns the plane array instead of `addr[0]`.
    - st22p create leaks `ctx` on failure.
    - In st22p/st30p/st40p `frame_done`, the flag ordering can suppress the next frame's callback.

## 7. Requirements for a new memory model that maps onto current internals

The goal is to keep the packet engines (`tv_build_*`, `rv_handle_*`, transmitter, DMA lender) unchanged and put the new objects above `st_frame_trans`.

```text
 mtl_memory_handle (region)          mtl_buffer_handle (immutable layout)       lease / submission
 ─────────────────────────           ───────────────────────────────────        ───────────────────
 vaddr, len, domain                  planes[{mem, offset, span, stride}]         per-use timing, opaque
 per-device IOVA map (P, R, DMA)  -> resolved per plane: addr, iova, page tbl -> bound to one st_frame_trans slot
 refcnt from buffers + in-flight     validated once vs. session layout            released by free_cb / put
```

1. **An imported buffer can map to `st20_ext_frame` / `st_frame_trans` without engine changes**, provided that:
   - the plane is a single contiguous VA span;
   - `iova` is valid on every device in the path (P, R, DMA engine);
   - `span >= st20_fb_size` using the session's transport linesize.
   The slot's `addr/iova` fields are already rebindable per frame for EXT slots. **[verified: `st_tx_video_session.c:4569-4570`, `st_rx_video_session.c:1286-1288`]**
2. **Reference ownership must move into the handle.**
   - The region refcount is incremented when a buffer is bound to a slot (`set_ext_frame` / `query_ext_frame` / `rv_get_frame`).
   - It is decremented only in `tv_frame_free_cb` after `refcnt--` (TX), after `rv_put_frame` (RX), and after DMA drain.
   - Unmap/destroy returns `-EBUSY` while the count is nonzero. `mt_map_mgr` is the natural place for this, but it needs handles, not `(vaddr, iova, size)` triples.
3. **Fix the callback ordering first.** Clear or rebind `addr/iova` and decrement `refcnt` *before* invoking the app's completion, or use the slot `refcnt` as the only gate, so that a completion can safely re-arm the slot (§6 item 2).
4. **Per-device mapping.** A region import must map into every active port device and every DMA device, or must prove they share one IOMMU domain. It must fail if any path device cannot map. It must also carry a NUMA/socket hint so frames are not silently cross-socket.
5. **IOVA stays internal.**
   - VA mode, host memory: IOVA == VA for hugepages. For non-hugepage memory, keep the invented-IOVA allocator but make it collision-safe.
   - PA mode: only hugepage memory from MTL allocation can be direct, via a page table per buffer (reuse `st_page_info`). Imported non-hugepage memory must be declared "copy only".
6. **Publish capabilities and the chosen path.**
   - Expose the `tx_no_chain` decision, ST22 copy, and cross-page copies as a queryable data path (DIRECT / COPY / CONVERT).
   - Allow `REQUIRE_DIRECT` to fail session creation when `tx_no_chain` would be selected.
   - Report the minimum buffer count needed for direct TX (the `tv_pkts_capable_chain` rule) and the required alignment (page for mapping, 64 B for internal; DMA-offload needs payload not crossing a page in PA mode).
7. **One terminal result per TX submission, and one per RX provision.**
   - TX: the zero-copy completion point (extbuf free) is the only correct "reusable" point for direct buffers. For copy/convert paths, the source can be released earlier but only as a distinct `SOURCE_REUSABLE` event (review §9).
   - RX: every buffer handed to MTL (query/provide) must come back via a terminal result, including dropped and incomplete frames. This requires the transport to stop recycling ext frames silently (`st_rx_video_session.c:977-981`, `:978`).
8. **Planar and stride.**
   - The transport engines consume one contiguous plane with a fixed `st20_linesize` (`ops->linesize`, `st_rx_video_session.c:3343-3348`). Direct binding therefore requires `stride == transport linesize`; otherwise the path is COPY or CONVERT.
   - Multi-plane application formats are only ever reached through conversion. Per-plane regions are fine there because converters use `addr[p]`/`linesize[p]` (quick read: `st_convert.c:48-62`).
9. **Header split and GPU.** Header split needs a region-level "pool" import (one region carved into packet slots), not per-frame buffers. It should be kept out of v1 or modelled as a special RX pool. GPU memory can only be COPY today (CPU into USM); a device-memory domain must advertise "no direct" until a NIC-to-VRAM path exists.
10. **ST30/ST40/ST41.** These always copy into mbufs on TX and from mbufs on RX, so imported buffers there are COPY-only by construction. Their "done" is after packetization; with RTCP retransmit that is still safe because retransmits come from mbufs, not frames. **[inferred]** The new API can support them uniformly without engine work.
11. **Kernel socket and native AF_XDP** are COPY-only on TX and RX (§3). Region import must succeed without mapping (today it returns a fake IOVA) but must report COPY.

## Open questions for the maintainer

1. **What is the reusable point for copy-path TX?** On ST22, ST30/40/41, `tx_no_chain` and st20p conversion, MTL stops reading app memory long before the network outcome.
   Should v1 deliver one terminal result at transport completion for every path (simple, uniform, longer lease), or two events (`SOURCE_REUSABLE` at copy/convert time, terminal at NIC completion)? The current code mixes both, which is the source of the three-context callback problem.
2. **Should `REQUIRE_DIRECT` be able to fail session creation?** The `tv_pkts_capable_chain` heuristic, ST22 forced copy, and PA-mode cross-page copies all downgrade silently today. Options:
   - (a) fail create;
   - (b) create but report COPY;
   - (c) make MTL auto-raise the buffer count to satisfy the direct-path minimum.
   Option (c) changes memory footprint without asking.
3. **Scope of IOVA mapping: which devices must a region map into?** Today only port P is mapped, and it works for R and DMA engines only because of VFIO default-container sharing. Should the new region import iterate over every port and DMA device explicitly (robust for non-VFIO drivers, more code), or document "single IOMMU domain required" and verify it at init?
4. **Is IOVA-PA mode in scope for imported memory?** `mtl_dma_map` rejects it, and internal frames need page tables. Options:
   - (a) imported = VA-mode only, with PA copy-only;
   - (b) support PA for hugepage imports via page tables;
   - (c) drop PA for direct paths entirely.
   This affects no-IOMMU hosts and some VMs.
5. **Keep the invented-IOVA allocator for non-hugepage memory, or require IOVA == VA?** IOVA == VA is simpler and matches DPDK VA mode, but it can collide with DPDK's own IOVA=VA hugepage mappings when the region is registered with `rte_extmem_register`. The invented range starting at `0x10000` avoids that but is unchecked against the hugepage space.
6. **Should the new API expose raw IOVA at all?** The review suggests an expert "pre-mapped" import only. Current ext-frame users (FFmpeg/GStreamer plugins and samples) pass `mtl_hp_virt2iova` or `mtl_dma_map` results. Do we keep a raw `{addr, iova}` import for compatibility, validated against the map table, or force everything through region handles?
7. **What does unmap/destroy do while in flight?** `-EBUSY` (review §14) requires the refcount described in §7 item 2. Should session destroy also be allowed to force-complete outstanding buffers with an ABORTED terminal result (today `tv_notify_frame_done` is forced on recovery, `st_tx_video_session.c:4291-4301`), and must that happen before the region can be released?
8. **RX incomplete and dropped frames with imported buffers.** Should every provided RX buffer get a terminal result, including frames the transport dropped (slot replacement, DMA busy, no pipeline slot)? That means changing `rv_frame_notify` to stop silently `rv_put_frame`-ing ext frames and removing the `RECEIVE_INCOMPLETE_FRAME` coupling.
9. **Can the extbuf count hit zero mid-frame?** `sh_info` has no builder-held reference, so a stall in slice mode while pads recycle descriptors might fire `free_cb` early. Should the new completion logic take a +1 builder reference per frame (cheap, removes the doubt), or do we have evidence it is impossible? A targeted slice-mode test on E810 would settle it.
10. **Do we keep header split in the redesign?** It is compiled out on the pinned DPDK 26.07, supports one port, and uses a pool layout that is not per-buffer. Options:
    - (a) drop it;
    - (b) keep it as an internal RX optimization with MTL-allocated pools only;
    - (c) design a region-pool import for it now.
11. **What should "GPU direct" mean in v1?** Today it is Level Zero shared USM with CPU copy, RX only. Options:
    - (a) model it as a device-memory domain that is COPY-only until a NIC-to-VRAM path exists;
    - (b) remove the flag in favour of generic import of USM pointers as host memory;
    - (c) scope real device DMA (DMA-BUF / peer-to-peer) now.
    It also needs a decision on DMA-offload + GPU (currently unguarded, would DMA to IOVA 0).
12. **Should RX frames be re-zeroed or loss-masked on recycle?** Imported buffers make this the app's problem. Should MTL guarantee "missing ranges are reported" (bitmap in the result) instead of "missing ranges are zero"? That affects the RX result struct.
13. **Should ST30/ST40/ST41 get imported memory in v1?** They are COPY-only by construction, so support is cheap and uniform. The alternative is to keep them library-pool-only to limit scope.
