# S8 — RTP passthrough (app-built packets) as a first-class mode of the unified API

| | |
|---|---|
| Status | Proposal for maintainer review, on top of revision 3. Nothing here is implemented |
| Date | 2026-10-01 |
| Trigger | New requirement from the project owner: keep the RTP passthrough mode (today's `*_TYPE_RTP_LEVEL`), make it coherent with the rest of the API, and stop exposing `include/st20_api.h` and the other session-level headers |
| Overturns | NG2 ([01](../01-goals-and-requirements.md)), D-25, the proposed default of Q-MODE-1, [09 §2.1](../09-media-modes-and-backends.md) "Packet level (later)" |
| Related | S9 (hiding every session-level header) and S4 (simplifying the data path and the "unit" struct). See §10 for where they touch this proposal |

Labels: **[verified]** = read in the code or a cited document at HEAD `545a266a`. **[inferred]** = reasoned from verified code, not run. **[ext]** = external source, read in this session (URL in §3). Paths are relative to the repository root; `TX` = `lib/src/st2110/st_tx_video_session.c` and `RX` = `lib/src/st2110/st_rx_video_session.c`.

## 0. Summary

**Today.** Every essence (ST20, ST22, ST30, ST40, ST41) has an RTP level: the app gets a DPDK `rte_mbuf*` as a `void*`, writes the RTP header and payload, and puts it back with a length. The library adds Ethernet/IPv4/UDP, paces, duplicates for ST 2022-7 and, on RX, de-duplicates and hands raw RTP back through a per-session ring (§1). It works, but it exposes mbufs,
runs app callbacks on the tasklet, ignores user pacing everywhere, detects frames by "the RTP timestamp changed", has a different pacing model per essence, and carries a dozen bugs (§1.7). The documented reason it exists, ST 2022-6, cannot even be sent today: a 2022-6 RTP packet is 1396 B and the library's limit is 1352 B (§1.7 #1).

**Proposal.** Packet mode becomes a **unit kind** of the unified session, `unit = MTL_UNIT_PACKETS` (today's reserved `MTL_UNIT_PACKET_CHUNK`, same value 2), available on every essence plus one new packet-only essence, **generic RTP** (`mtl_rtp_session_create`), for ST 2022-6 and custom payloads. Nothing new is invented on the data path: the same five verbs, leases,
results and call classes apply. Only the buffer shape differs:

- **TX:** `mtl_tx_acquire` leases a **chunk** of N fixed-stride packet slots (plane 0 of the ordinary buffer view; a header/payload split uses planes 0 and 1). The app writes RTP header + payload per slot and a length per slot in the packet table (the meta area). `mtl_tx_submit` hands over the chunk; the last chunk of a frame carries `MTL_SUBMIT_UNIT_END`. **One
  result per chunk.**
- **The library owns L2–L4 only.** What it may write into the RTP header is an explicit per-session mask (`MTL_PKT_SET_TIMESTAMP | SEQ | SSRC_PT | MARKER`); 0 means verbatim: the bytes from the RTP header on are on the wire unchanged, on every leg.
- **Pacing** is the essence's own wire model over the unit (ST 2110-21 for video, CBR for cvideo, ptime for audio, the -40 window for ANC and fastmeta, linear for generic RTP), anchored on the session's media time (AUTO / INDEX / TAI) or on the RTP timestamp found in the packets (`MTL_PKT_TIME_FROM_RTP`). Per-chunk launch times (Rivermax-style) and ASAP are opt-in.
- **RX:** `mtl_rx_dequeue` leases a chunk of 1..N received packets, with a packet table entry per packet (address, length, leg, extended sequence, gap count, arrival time, flags); `mtl_rx_release` returns it. The library keeps 2022-7 de-duplication by sequence number. The default path copies into the chunk in the caller (`DPC`), so NIC mbufs are held only for as long
  as the per-session ring is non-empty; a bounded zero-copy LEND path is opt-in.
- **No `rte_mbuf`, no callbacks, no app code on tasklets.** Mbufs stay internal (extbuf-attached chain mbufs on TX, ring-held mbufs on RX).
- **Names.** Revision 3's `MTL_RTP_PASSTHROUGH` ("the app chooses the RTP timestamp, the library builds the packets") is renamed **RTP timestamp override** (`MTL_RTP_TS_OVERRIDE`); "passthrough" is reserved for packet mode in prose and is used in no identifier (§4.1).

## 1. Today's RTP level, per essence

### 1.1 The common shape [verified]

| Item | ST20 / ST22 | ST30 | ST40 | ST41 |
|---|---|---|---|---|
| Enum | `ST20_TYPE_RTP_LEVEL` `include/st20_api.h:356`; `ST22_TYPE_RTP_LEVEL` `:374-375` | `ST30_TYPE_RTP_LEVEL` `include/st30_api.h:171` | `ST40_TYPE_RTP_LEVEL` `include/st40_api.h:134` | `ST41_TYPE_RTP_LEVEL` `include/st41_api.h:80` (TX only; RX has no `type`, it is RTP-only, `:268-277`) |
| TX ops | `rtp_ring_size`, `rtp_frame_total_pkts`, `rtp_pkt_size`, `notify_rtp_done` `st20_api.h:1257-1277` (ST22 `:1380-1401`) | `rtp_ring_size`, `notify_rtp_done` `st30_api.h:455-462` | same `st40_api.h:413-420` | same `st41_api.h:218-225` |
| RX ops | `rtp_ring_size`, `notify_rtp_ready` `st20_api.h:1612-1619` (ST22 `:1717-1724`) | `st30_api.h:550-557` | `st40_api.h:515-522` | `st41_api.h:271-277` |
| TX get / put | `st20_tx_get_mbuf(h, &usrptr)` `:1932`, `st20_tx_put_mbuf(h, mbuf, len)` `:1948`; ST22 `:2120`, `:2136` | `st30_api.h:729`, `:745` | `st40_api.h:695`, `:711` | `st41_api.h:430`, `:446` |
| RX get / put | `st20_rx_get_mbuf(h, &usrptr, &len)` `:2279`, `st20_rx_put_mbuf` `:2290`; ST22 `:2443`, `:2454` | `st30_api.h:902`, `:913` | `st40_api.h:766`, `:777` | `st41_api.h:501`, `:512` |

- **Ownership.** `get_mbuf` returns the mbuf as `void*` and sets `usrptr` to the RTP header. In chain mode that is the mbuf start; in no-chain mode it is offset by the 42 B Ethernet+IPv4+UDP header so the library can write it in place (TX:4651-4659; audio `st_tx_audio_session.c:2988-3002`). `put_mbuf(len)` takes RTP header + payload, checks `0 < len <=
  MTL_PKT_MAX_RTP_BYTES` (`lib/src/mt_util.h:19-24`) and does `rte_ring_sp_enqueue` (TX:4701-4711). The ring is single-producer (TX:3011), which the header does not say.
- **Size limit.** `MTL_PKT_MAX_RTP_BYTES = MTL_UDP_MAX_BYTES - 8 - 100 = 1352` (`include/mtl_api.h:82-89`).
- **RX.** `get_mbuf` returns the whole mbuf, L2–L4 included, with `usrptr = data + 42` and `len = data_len - 42` (RX:4770-4773; audio `st_rx_audio_session.c:1809-1812`, ANC `st_rx_ancillary_session.c:1727-1733`, fastmeta `st_rx_fastmetadata_session.c:1101-1108`). `put_mbuf` is `rte_pktmbuf_free` (RX:4781).
- **Callbacks on the tasklet.** `notify_rtp_ready` runs once per received packet in tasklet context (RX:1971); the ST41 header says "only non-block method can be used in this callback as it run from lcore tasklet routine" (`st41_api.h:273-276`). This is app code on a pinned tasklet, which the unified API forbids (D-04, R-THR-4).
- **Bindings.** Rust bindgen and Python SWIG wrap `st20_api.h` whole (`rust/imtl-sys/wrapper.h`, `python/swig/pymtl.i`), so `*_get_mbuf` is reachable from both, but neither safe layer uses it.

### 1.2 ST20 / ST22 TX [verified unless marked]

- **Setup.** `tv_ops_check` rejects a zero ring size, zero `rtp_frame_total_pkts`, an invalid `rtp_pkt_size` and a NULL `notify_rtp_done` (TX:4088-4105); power-of-two is never checked, it fails later in `rte_ring_create` (TX:3012-3015). ST22's check (TX:4208-4221) does not test `rtp_frame_total_pkts` at all.
- **Pacing by packet count.** `st20_total_pkts = rtp_frame_total_pkts` and `st20_pkt_size = rtp_pkt_size + 42` (TX:3182-3188), so `trs = frame_time × reactive / total_pkts` (TX:519) and the RL rate is `pkt_size × total_pkts × fps` (TX:84-91). Every packet is treated as a NORMAL packet in pad training (TX:397-399). ST22 forces `vrx = 0` (TX:581-585).
- **Frame boundary = RTP timestamp change.** A new frame starts when the app's `tmstamp` differs from the previous packet's (TX:1392, chain TX:1466); the marker bit is ignored. The library then resets the packet index, counts a frame, reads the field bit from the RFC 4175 `row_number` and calls `tv_sync_pacing(impl, s, 0, second_field)` (TX:1393-1410). The sample
  bumps `tmstamp` by one per frame only to mark the boundary (`app/sample/low_level/tx_rtp_video_sample.c:61-67`).
- **RTP timestamp.** Overwritten with the epoch/PTP-derived value unless `ST20_TX_FLAG_USER_TIMESTAMP` (TX:1411-1423). Sequence number, marker, PT and SSRC are never written; `ops.ssrc` / `ops.payload_type` are not applied (TX:1368-1440). The sample never writes SSRC, so SSRC holds stale mbuf bytes.
- **L2–L4.** No-chain: Ethernet/IPv4/UDP copied over the first 42 B, lengths and checksum fixed (TX:1383-1438). Chain: a 42 B header mbuf chained in front (TX:1505-1510).
- **2022-7.** Chain: a second header mbuf shares the app segment via `rte_mbuf_refcnt_update` (TX:1520-1556), zero-copy. No-chain: a deep copy (TX:1053-1086). RTP bytes are identical on both legs.
- **`notify_rtp_done`** fires once per dequeued bulk (up to 4 packets), *before* the packets are built or sent (TX:2261); the header says "when lib finish the sending of one rtp packet" (`st20_api.h:1272-1277`).
- **Ignored flags.** `tv_sync_pacing` always gets `required_tai = 0`, so `USER_PACING` and `EXACT_USER_PACING` are accepted and silently ignored (TX:1410, :1484; `tv_pacing_required_tai` is only called from frame tasklets, TX:1970, :2490). `rtp_timestamp_delta_us` is ignored with `USER_TIMESTAMP` (TX:1411-1412).
- **Count mismatch.** Nothing detects a frame with more or fewer packets than declared. More: the cursor runs past the frame window; fewer: tail packets can wait in the ring until the next frame's packets arrive, because a bulk dequeue needs `bulk` items [inferred from TX:2242-2249].
- **ST22.** `st22_tx_create` maps onto an ST20 RTP session with `fmt = YUV_422_10BIT` (TX:4970-4990); there is no codestream logic in RTP mode.

### 1.3 ST20 / ST22 RX [verified unless marked]

- Payload-type and SSRC filters with drop counters (RX:1879-1894).
- **Two timestamp slots only** (`ST_VIDEO_RX_REC_NUM_OFO = 2`, `lib/src/st2110/st_header.h:39`); an unknown timestamp evicts the next slot and clears its bitmap (RX:1307-1333), with no "timestamp in the past" guard.
- **2022-7 dedup** by a per-slot bitmap indexed by `seq − seq_base`, the 32-bit RFC 4175 extended sequence for ST20 and the 16-bit sequence for ST22 (RX:1904-1927); the first packet of a slot sets the base, so an earlier packet arriving later is dropped as `stat_pkts_idx_oo_bitmap` (RX:1917-1921). Bitmap capacity comes from width × height × format, not from any packet
  count (RX:3366-3369).
- **No reordering**; delivery in arrival order (RX:1931-1940).
- **Ring overflow** drops the packet and counts `stat_pkts_rtp_ring_full` (RX:1963-1967).
- **Frame-only flags are silently inert** in RTP mode: header split (RX:3331), DMA (RX:2572-2574), multi-thread RX (RX:2642-2652), auto-detect (RX:3427-3428); the timing parser is allocated but never fed (RX:3408-3417).
- The app finds frames and fields itself (timestamp, marker, F bit); the library emits no per-frame event.

### 1.4 ST30 audio [verified unless marked]

- **Per-packet pacing at ptime epochs.** The RTP tasklet (`st_tx_audio_session.c:944-1087`) syncs pacing when the cursor is 0 and resets it after every packet (`:992`, `:1071`), so each packet lands on the next ptime slot. `USER_PACING` is ignored (`required_tai = 0`, `:992`).
- **Timestamp.** Detects a change of the app's raw timestamp, then stamps the epoch-derived value (`:556-567`, `:605-618`); apps must change `tmstamp` on every packet or consecutive packets carry one timestamp.
- `framebuff_size` is not required in RTP mode, yet attach divides by it (`:2171`, `:2238`) [inferred UB, log only].
- **RX dedup is by timestamp**, not sequence: a packet passes only if its timestamp is strictly newer (`st_rx_audio_session.c:584-604`). Ring-full is counted under the misleading `stat_slot_get_frame_fail` (`:620-624`).

### 1.5 ST40 ancillary, and `st40p_rx_ops.rtp_ring_size` [verified unless marked]

- **The ST40 session API is no longer RTP-only.** RX has a FRAME level in the transport (`st_rx_ancillary_session.c:380-423`), and st40p RX uses it (`lib/src/st2110/pipeline/st40_pipeline_rx.c:173-178`, commit `d74cd1e0`). `doc/design.md:618` ("RTP passthrough mode is the only supported") is stale.
- **`st40p_rx_ops.rtp_ring_size` is dead.** It is documented "Mandatory. RTP ring queue size, must be power of 2" (`include/st40_pipeline_api.h:233-234`) but no pipeline file reads it. It survives in the GStreamer `rtp-ring-size` property with a power-of-two check (`ecosystem/gstreamer_plugin/gst_mtl_st40p_rx.c:504-512`), the st40p sample
  (`app/sample/rx_st40_pipeline_sample.c:152`) and two acceptance tests that only test that property's validation (`tests/acceptance/tests/single/gstreamer/anc_format/test_anc_format.py:1871`, `:1971`).
- **TX pacing order is wrong in RTP mode.** The TSC gate runs before the dequeue (`st_tx_ancillary_session.c:1208-1221`) and the new-frame pacing sync only after it (`:752`, `:816`), so the first packet of frame N leaves when frame N−1's epoch passes, carrying frame N's timestamp: single-packet frames go out about one frame early [inferred]. Frame mode orders it
  correctly (`:1002` then `:1018`).
- **Chain-path pointer bug.** `rfc8331 = (struct st40_rfc8331_rtp_hdr*)&udp[1]` (`:805`) points past the 42 B header mbuf, not into the app's packet, and is then byte-swapped in place (`:806-807`): wrong field stats and a write beyond the header mbuf's data [verified pointer; extent inferred].
- **Byte order differs by path.** Chain TX swaps the RFC 8331 first chunk on every packet (`:821`); no-chain swaps only at a new interlaced frame (`:739-741`); RX converts it to host order for the app (`st_rx_ancillary_session.c:435`).
- RX dedup: timestamp-or-sequence with 64-bit per-frame bitmaps (`:546-584`).

### 1.6 ST41 fast metadata [verified unless marked]

- RX is RTP-only by API (no `type`, `st41_api.h:268-277`). TX has the ST40 gate-before-sync ordering (`st_tx_fastmetadata_session.c:956`, `:974`, `:550`), `second_field` hard-coded false (`:548-550`), and no DIT, K-bit or length stamping in RTP mode (`:459-462`, `:512-515` are frame-only).
- RX: `redundant_error_cnt[]` is never incremented (`st_rx_fastmetadata_session.c:159`, `:168`), so the 20-packet recovery threshold never fires; every 2022-7 duplicate returns `-EIO` and is counted as `err_packets` (`:161`, `:228-229`); the documented `fmd_dit` / `k_bit` RX filters are not implemented; RX does not convert the ST41 header chunk byte order, unlike
  ST40.

### 1.7 Semantic gaps and bugs found

| # | Finding | Evidence | Label |
|---|---|---|---|
| 1 | **ST 2022-6 cannot be sent.** A 2022-6 packet is 12 B RTP + 8 B HBRMT header + 1376 B payload = 1396 B; `put_mbuf` rejects anything above 1352 B | `include/mtl_api.h:89`, `mt_util.h:19-24`, TX:4672-4676; 2022-6 sizes from the standard | inferred (sizes not in-tree) |
| 2 | `USER_PACING` / `EXACT_USER_PACING` silently ignored on every essence in RTP mode | TX:1410; `st_tx_audio_session.c:992`; ANC/fastmeta `sync_pacing(impl, s, 0)` | verified |
| 3 | Frame boundary only by timestamp change; marker ignored; count mismatch undetected | TX:1392; ANC `:730`, `:797` | verified |
| 4 | ST40 / ST41 TX first packet of a frame one epoch early | §1.5, §1.6 | inferred |
| 5 | ST40 chain-path out-of-bounds pointer, inconsistent RFC 8331 byte order | `st_tx_ancillary_session.c:805-807`, `:739-741`, `:821` | verified |
| 6 | `notify_rtp_done` before transmit; for ST30/40/41 before the header mbuf is allocated, so an allocation failure drops a packet already reported done | TX:2261; `st_tx_audio_session.c:1021-1040` | verified |
| 7 | RX refcount race: mbuf enqueued to the app ring *before* `refcnt_update(+1)` | RX:1962-1969 | verified order; race inferred |
| 8 | RX ring-full defeats 2022-7: the bitmap bit is set before the enqueue attempt, so the other leg's copy is dropped as a duplicate (all essences) | RX:1920 vs :1962; audio `:584-624`; ANC `:546-668` | verified order |
| 9 | RX `last_pkt_idx[]` not reset on slot reuse, so gap/reorder counting breaks after two frames | RX:1324-1329, :1931 | inferred |
| 10 | ST22 TX does not validate `rtp_frame_total_pkts` (0 divides `trs`, empties `ring_count`) | TX:4208-4221, :519, :3410 | inferred |
| 11 | TX RTCP in chain mode reads seq/timestamp at offset 42 of segment 0, which is only 42 B | `lib/src/mt_rtcp.c:41-43`, :59-61; TX:2916 | inferred |
| 12 | No-chain + redundant TX error path frees uninitialised VLA slots | TX:2302-2305 | inferred |
| 13 | RX hold budget unchecked: a slow app can hold up to `rtp_ring_size` mbufs of a queue pool sized `nb_rx_desc + 1024` (shared by every session on a shared queue) | `lib/src/dev/mt_dev.c:1353`; `MT_DEV_RX_DESC = 2048`, `dev/mt_dev.h:11` | inferred |
| 14 | ST41 RX: dedup threshold never recovers; duplicates counted as errors; documented filters missing | §1.6 | verified |
| 15 | The ST40 "rtp" fuzz target no longer reaches the RTP path (zeroed ops → FRAME_LEVEL) | `tests/fuzz/st40/st40_rx_rtp_fuzz.c:120`, `:168` | inferred |
| 16 | Stale docs: `design.md:618` (ST40 RX RTP-only), the programmer's guide's RTP section (`doc/doxygen/programmers_guide.md:93-180`), `README.md:46` claims "ST2022-6 by RTP passthrough interface" | as cited | verified |

## 2. Consumers and use cases

### 2.1 In-tree users [verified]

| Consumer | What it does with RTP level | Why not frame mode |
|---|---|---|
| `app/sample/low_level/tx_rtp_video_sample.c`, `rx_rtp_video_sample.c` | reference app-side RFC 4175 packetiser (hard-coded 1080p, 4320 × 1200 B) and a marker-counting receiver | it is the APIs own sample (`doc/design.md:332-336`) |
| RxTxApp `"type": "rtp"` for video, audio, anc, fmd (`tests/tools/RxTxApp/src/parse_json.c:566`, `:842`, `:1255`, `:1384`) | **pcap replay** (`legacy/tx_video_app.c:270-345`), even **an ST22 pcap through an ST20 RTP session** (`script/loop_json/st22p_pcap.json`); app-side (de)packetisers for RFC 4175, RFC 8331, ST41 | replay; app-owned packing; ST41 RX is RTP-only |
| gtest (`tests/integration_tests/st20/`, `st22_test.cpp`, `st30_test.cpp`, `st40_test.cpp`) | pacing/fps checks, digests, mixes; **out-of-order injection** (`st20_digest.cpp:567`, `:635`, `:695`) and **truncated frames** (`st20_meta.cpp:256`) that the frame API cannot produce | fault injection |
| unit harnesses (`tests/unit/session/st20_harness.c:426-438`, `st20_tx_harness.c`, `st40_harness.c:118-120`) | feed RTP packets into RX, run the RTP tasklet | test seams |
| acceptance `tests/single/st41/test_st41.py:81`, `:139`, `:193`, `:220` | ST41 through RxTxApp RTP | ST41 RX has no frame level |
| ecosystem (FFmpeg, GStreamer, OBS, MXL), `plugins/`, `manager/`, `ld_preload/`, Rust, Python | none (except the dead st40p `rtp-ring-size` property) | — |

Nothing in the tree implements ST 2022-6; the docs only name it as the reason the mode exists (`doc/design.md:334`).

### 2.2 Use cases the mode must keep serving

| Use case | Needs from the library | Frame mode suffices? |
|---|---|---|
| **ST 2022-6** (SDI over IP, still common in plants) and **other RTP payloads MTL does not packetise** (ST 2110-43 timed text, proprietary metadata, RFC 3640 audio, …) | L2–L4, linear pacing at a declared rate, 2022-7, verbatim RTP, ~1400 B packets | no |
| **App-side packetisers**: a hardware JPEG XS or H.26x encoder emitting RTP, an FPGA pre-packetised stream, a custom RFC 4175 packing | library pacing and 2022-7 over app-built packets; optional seq/timestamp stamping | no |
| **Gateways and proxies** that forward packets unchanged: unicast ↔ multicast, re-addressing for IS-05, 2022-7 merge to one leg or split to two | RX dedup, TX L2–L4 rewrite, packet-level latency (µs, not one frame) | partly (frame forwarding reassembles and adds ≥ 1 frame) |
| **RTP re-stamping**: repair a non-compliant source's timestamps, re-time a stream onto local PTP | read the timestamp, stamp a derived one | partly (RTP timestamp override, §4.1) |
| **Recording and replay** of pcaps, including captured ST22 streams into a frame receiver | verbatim TX with original or regular timing | no |
| **Analysers and monitors**: sequence gaps, per-leg arrival times, path differential, payload conformance | every packet, both legs if asked, HW arrival times | no |
| **Test fault injection**: out-of-order, truncated or malformed units against a frame receiver | arbitrary packet order and counts | no (only via a debug API) |

## 3. Prior art: app-built packets without raw driver buffers

| System | TX shape | RX shape | Timing | Completion | Lesson for MTL |
|---|---|---|---|---|---|
| **Rivermax media API** | chunks of fixed **strides**; header/data split as two sub-blocks; a per-chunk size table for dynamic sizes; app writes RTP (R10 §2.2, §2.5) | 0..max packets per chunk with per-packet info and moderation; **no release**, valid until wrap (§5.2) | time on a frame's first chunk, `0` = follow (§2.4) | optional, with HW time (§4) | strides + size table; HDS as planes |
| **Rivermax generic API** | scatter-gather append per packet, rate token bucket (`set_rate`, max burst) (§3) | same as media | commit time, 0 = ASAP | same | a rate-paced mode for non-media payloads |
| **Rivermax IPO** (2022-7 RX) | — | NIC places each packet at `seq mod capacity` in one shared buffer for both legs; SW releases contiguous runs after a path-differential budget (§5.3) | — | — | dedup by sequence number; budget-based release |
| **DPDK** | `rte_pktmbuf_attach_extbuf` with a `free_cb` run "once all the mbufs are detached"; pinned pools, `rte_pktmbuf_pool_create_extbuf` [ext] | buffer split (patched DPDK only, R07 Q8) | `SEND_ON_TIMESTAMP`; a past time is sent at once (R11 §7) | lazy mbuf free | per-chunk `shinfo` refcount = per-chunk completion (as TX:1295) |
| **AF_XDP** | UMEM of equal frames, "2K or 4K", addressed by offset; TX ring of `{addr, len, options}`; multi-buffer via `XDP_PKT_CONTD` [ext] | FILL ring gives frames to the kernel, RX ring returns `{addr, len}` | none | COMPLETION ring; an entry "does not guarantee successful packet transmission" [ext] | fixed-size slots + lengths are the universal zero-copy shape; reusable ≠ sent |
| **io_uring** | `SEND_ZC`: a result CQE and a second `NOTIF` CQE when the buffer is reusable (`research/11-media-io-prior-art.md` §4.1) | provided-buffer rings | — | exactly one CQE per SQE, plus NOTIF | MTL's D-18 (one terminal result after both) is the simpler choice |
| **libfabric** | `fi_sendmsg` with iov + `desc[]`; `FI_MORE` batching; `FI_INJECT` copy-on-call (`research/09-libfabric.md` §4) | `FI_MULTI_RECV`: one posted buffer receives many messages, final completion when consumed | — | context per op | a chunk is one posted buffer for many packets |
| **GStreamer** | payloaders push `GstBufferList`s; `gst_rtp_base_payload_push_list` "refreshes" SSRC, PT, seqnum and timestamp before pushing [ext] | `udpsrc` buffers | per-buffer `sync` on PTS | buffer unref | header fields owned by the layer must be stated, not silently rewritten |
| **FFmpeg** | `rtp` muxer with `pkt_size`; the `udp` protocol paces only by `bitrate` / `burst_bits` ("constant bitrate if the input has enough packets") [ext] | `udp` circular buffer | none per packet | none | rate pacing is the minimum a packet sender needs |
| **PipeWire `module-rtp-sink`** (AES67) | packets of `rtp.ptime` / `rtp.framecount` within `net.mtu`; `sess.ts-offset` (default random), `sess.ts-refclk` [ext] | `module-rtp-source` | PTP-driven graph | — | audio packet mode = ptime cadence and an RTP offset |
| **DeckLink, AJA** | no packet API (SDI-centric; DeckLink IP is frame level, `research/11-media-io-prior-art.md` §1.6) | — | — | — | — |

**Shapes that work.** (1) Fixed-stride slots in registered memory with a per-packet length table (Rivermax, AF_XDP). (2) Header and payload as separate planes for GPU or FPGA payloads (Rivermax HDS). (3) One time per chunk, with "follow on" for the rest (Rivermax). (4) A completion per chunk, never per packet, meaning "storage reusable" (AF_XDP, io_uring NOTIF, MTL
D-18). (5) RX chunks of 0..max packets with moderation (Rivermax), plus an explicit release (MTL). (6) A stated rule for which header fields the layer writes (GStreamer does it silently — avoid).

## 4. Design

### 4.1 Names

| Concept | Identifier | Prose |
|---|---|---|
| App-built packets (today's `*_TYPE_RTP_LEVEL`) | `MTL_UNIT_PACKETS` (renames the reserved `MTL_UNIT_PACKET_CHUNK`, value 2 unchanged) | "packet mode", "RTP passthrough" |
| One lease of packet slots | — | "chunk" |
| Packet-only essence for ST 2022-6 and custom payloads | `MTL_ESSENCE_RTP`, `mtl_rtp_session_create`, `struct mtl_rtp_config` | "generic RTP essence" |
| Revision 3's "the app chooses the RTP timestamp, the library builds the packets" | `enum mtl_rtp_ts_mode { MTL_RTP_TS_DERIVED, MTL_RTP_TS_OVERRIDE }`, `timing.rtp_ts_mode`, `mtl_tx_submission.rtp_ts_override`, `MTL_SUB_RTP_TS`, `MTL_REASON_RTP_TS_OVERRIDE_AUTO` (207) | "RTP timestamp override" |
| What the library writes into an app-built RTP header | `MTL_PKT_SET_*` | "stamping" |

Rule: no identifier contains "passthrough". `rtp_ts_mode` applies to library-built packets; a packet-mode session with `rtp_ts_mode = OVERRIDE` is `-MTL_EINVAL` (stamping covers it, §4.5). The rename touches `mtl_unified.h:465`, `:1484-1487`, `:1512`, `:2543`, `:2555`, 06 §5.4, 07 §5.4 row 207, 09 §1.2 and §9, 10 §10, 11 §6 and `sketch/examples/ex10_processor.c`.

### 4.2 The model: a chunk is the unit

Everything in 03, 04 and 07 holds with "unit" read as "chunk":

- **TX:** `FREE → APP_WRITABLE` (acquire) `→ QUEUED` (submit) `→ IN_FLIGHT` (picked up) `→ DONE` (the last mbuf referencing the chunk is freed). Exactly one terminal outcome per submitted chunk; results in submission order; `mtl_tx_release` returns an unsubmitted chunk; `mtl_tx_withdraw` pulls a queued one.
- **RX:** `FREE → APP_READING` at dequeue (the chunk is formed *at dequeue*, §4.8, so packet sessions skip RECEIVING/READY) `→ FREE` at release, or `HELD` while a TX submission holds it (05 §5.4).
- **Session-level "unit"** for timing purposes is still the frame/field (video, cvideo, anc, fastmeta, generic RTP) or the packet (audio): a unit spans one or more chunks, ended by `MTL_SUBMIT_UNIT_END`. Admission, late policy and the RTP derivation are per unit; results are per chunk and say which unit they belong to (`media_index`).
- **One unit kind per session, fixed at create.** Mixing frames and packets in one session is rejected: the lease table slot shape, the pacing state and RTP ownership all differ, and two producers of one RTP stream would collide on sequence numbers. A packet session may join a group and share a timeline with frame sessions (§4.10).

### 4.3 Configuration

`sc.unit = MTL_UNIT_PACKETS` plus a `struct mtl_packet_config` block on `sc.next` (D-46: an optional feature group is a `next` block; this makes packet mode the first user of the chain, which needs the typed chain header of 11 §2.4: `struct_size`, `kind`, `next`). The media config is the ordinary one (`mtl_video_config`, …), because the essence still defines the wire
model, the SDP values and the RX checks. For generic RTP, `mtl_rtp_config` carries the clock rate, unit rate and profile. Fields and zero meanings are in §5.

Validation at create (`-MTL_EINVAL` with a reason unless stated): slot size ≤ the port MTU minus L2–L4; the packets per unit and the rate fit the pacing class (an RL rate is `packets_per_unit × slot_bytes × unit_rate`, as today's TX:84-91); `rx_ring_packets` fits the RX queue pool headroom (`-MTL_ENOSPC`, finding #13); essence rules of §4.7.

### 4.4 TX chunks and how they reach the NIC

- **Shape.** A chunk is a buffer whose plane 0 has `rows = packets_per_chunk`, `row_bytes = slot_bytes`, `stride = slot stride` (64 B aligned). The meta area holds the **TX packet table**, one `struct mtl_pkt_tx` (8 B: `len`, `hdr_len`) per slot. With `layout = SPLIT`, plane 0 holds header slots (RTP header + payload headers, e.g. 20 B for RFC 4175 single SRD) and
  plane 1 the payload slots, so payloads can live in another region (GPU pinned host memory in v1, device memory later, 05 §10).
- **Fill.** The app writes the RTP packet into slot i and sets `pkt[i].len`; `len = 0` skips a slot; `submission.pkt_count` (0 = all) says how many leading slots are used.
- **Library pools and imported memory.** A library pool is one region (05 §5.3). Attached pools over imported regions work as for frames (Phase 4), so an FPGA or encoder can write packets straight into registered memory.
- **PMD DIRECT path (chain).** Per packet the tasklet takes one header mbuf (L2–L4, 42 B) and one mbuf attached to the slot with `rte_pktmbuf_attach_extbuf`, sharing one `rte_mbuf_ext_shared_info` per chunk whose refcount is `packets × legs`; its `free_cb` is the chunk's once-only completion hook (the M1 CAS of 05 §11). This is today's frame chain mode (TX:1295,
  :1706) applied to app slots. SPLIT adds the header slot as a copied 20–64 B tail of the header mbuf and attaches the payload slot, the same two segments as frame chain mode.
- **COPY path** (no multi-segment NIC, AF_XDP, kernel socket): the tasklet copies each slot into a single-segment mbuf, as today's no-chain path does, and the chunk is DONE after the copy (the "last packet handed" hook of 04 §4.4). The path is reported per session and per result (`path`), as D-17 requires.
- **2022-7.** One extra header mbuf per packet per leg, chained to the same attached mbuf (refcount +1), rewriting only L2–L4 (`s_hdr[leg]`): the RTP bytes are identical on both legs by construction (ST 2022-7 requires it). COPY path: one copy per leg. Per-leg counts in `pkts_sent[leg]`; a disabled or down leg is skipped and counted (D-59).
- **The app never sees an mbuf**, and mbuf pools are sized by the library from `pool.count × packets_per_chunk × legs`.

### 4.5 RTP header ownership: stamping

`mtl_packet_config.set_fields` lists the fields the library writes; every other byte from the RTP header on is sent as the app wrote it.

| Bit | The library writes | Use |
|---|---|---|
| none (0, default) | nothing: **verbatim** | forwarders, pcap replay, 2022-6 gateways, analysers' test senders |
| `MTL_PKT_SET_TIMESTAMP` | `floor(M × R) mod 2^32` from the unit's media time (R-TIME-3, D-09); audio: per packet from the sample index | app packetisers that do not own a clock; re-stamping |
| `MTL_PKT_SET_SEQ` | 16-bit sequence, and the RFC 4175 extended sequence for video, continuous across units and restarts | packetisers; injected packets |
| `MTL_PKT_SET_SSRC_PT` | SSRC and PT from `flows[]` | today's sample leaves SSRC as stale bytes (§1.2) |
| `MTL_PKT_SET_MARKER` | M = 1 on the last packet of a unit, 0 elsewhere | packetisers |

- Stamping is done where L2–L4 are written (on the tasklet; a few stores per packet), because under AUTO the slot, and therefore the RTP, is only known at pick-up. A stamped field is written before the leg duplication, so it is equal on both legs.
- `MTL_PKT_TX_VALIDATE` checks without rewriting: V = 2, PT = flow PT, `len ≤ slot_bytes`, constant timestamp within a unit, marker only on the last packet, and (with SUBMIT unit time) the app's timestamp against the derived one within ±1 unit. Violations are counted per reason and flagged in the chunk result, never silently fixed.
- Legacy mapping: today's default (timestamp rewritten) = `SET_TIMESTAMP`; `USER_TIMESTAMP` = verbatim.

### 4.6 TX timing and pacing

**Unit time** (`mtl_packet_config.unit_time`) gives each unit its media time M:

- `MTL_PKT_TIME_SUBMIT` (0): from the submission of the unit's first chunk, under the session media mode exactly as for frames: AUTO (next slot), INDEX (`media_index`), TAI (`media_tai_ns`), with the late policy, snapping and horizon of 06. Later chunks of the unit carry no media fields (`-MTL_EINVAL` if they do).
- `MTL_PKT_TIME_FROM_RTP` (1): M is the instant near now whose RTP is the first packet's timestamp, on the session timeline (the exact inverse of R-TIME-3, unambiguous within half a wrap: 6.6 h at 90 kHz, 44 s at 48 kHz). With `source_kind = GATEWAY` the slot is `M + min_tx_delay`: this is revision 3's processor recipe (06 §10.8) applied per unit by the library, so a
  forwarder keeps the input's timestamps *and* a fixed RTP-to-launch offset (the C5 §5.5 toggle cannot happen). Requires a timestamp the library can read: `layout = CONTIG` or the header plane.

**Pacing** (`mtl_packet_config.pacing`) spreads the unit's packets:

| Mode | Packets leave | Classes | Compliance |
|---|---|---|---|
| `MTL_PKT_PACE_UNIT` (0) | per the essence's wire model over the unit's slot (§4.7): packet j at `TVD + TRO + j × TRS` for video with TRS from `packets_per_unit` | HW_RATE, HW_LAUNCH, SW | compliant when the app sends the declared count |
| `MTL_PKT_PACE_LAUNCH` | the first packet of a chunk at `submission.launch` (NOT_BEFORE / EXACT); a chunk with `launch.mode = DERIVED` follows the previous one at the session rate (Rivermax's "0 = follow") | HW_LAUNCH, SW | the app's responsibility; EXACT is flagged `NON_COMPLIANT` |
| `MTL_PKT_PACE_ASAP` | as fast as the queue accepts | any | non-compliant, reported |

- **Mid-unit underrun.** A packet whose chunk is submitted after its due time leaves immediately, bounded by the essence's burst limit, and is counted; the chunk result is LATE with `max_packet_lateness_ns`. The grid is kept: the next unit still starts on its own slot.
- **Whole-unit late policy.** If the unit's first chunk is admitted late, DROP latches the unit: its remaining chunks complete as `DROPPED / TOO_LATE` until `UNIT_END`. RESLOT and bounded SEND_LATE behave as for frames.
- **Underrun of whole units** defaults to SKIP for every essence in packet mode. EMPTY_ANC and KEEPALIVE need a library-built packet inside the app's stream, so they are accepted only with `SET_TIMESTAMP | SET_SEQ | SET_SSRC_PT`.
- `USER_PACING`'s replacement is real here: TAI or INDEX media mode with UNIT pacing, or LAUNCH pacing. Both fix finding #2 for packet users.

### 4.7 Frame boundaries and essence specifics

Boundaries are **explicit**: the chunk that ends a unit is submitted with `MTL_SUBMIT_UNIT_END`. The marker bit and timestamp changes are only checked under VALIDATE. The library counts packets per unit **at submit** (an O(1) add on the app thread): a chunk that would exceed `packets_per_unit` is `-MTL_EINVAL` (`PKT_COUNT`) and the lease stays the app's; a unit that
ends short completes with the flag `MTL_TX_TIMING_PKT_SHORT` and the pacing grid is kept.

| Essence | Pacing unit | `packets_per_unit` | Essence rules in packet mode |
|---|---|---|---|
| video (ST20) | frame or field | 0 = the library packetiser's count for the video config and `packing` (reported in `mtl_session_info`) | ST 2110-21 sender type from `mtl_video_config`; **field parity from the unit's media index**, not the app's F bit (TX:1399-1404; an app that never sets it runs at half rate, `doc/design.md:501`); VALIDATE checks the F bit |
| cvideo (ST22) | codestream frame/field | **required**; CBR (`rate_mode` 0) needs exactly this many packets per unit, `UNIT_END` with fewer is `-MTL_EINVAL`; VBR_MAX allows fewer | RFC 9134 header fields are the app's; RL is downgraded to SW as today (`research/07-modes-matrix.md` §2.2), and reported |
| audio (ST30/31) | **one packet** (one ptime slot) | unused | `samples_per_packet` and format give the expected length (VALIDATE); a chunk is consecutive packets, its media time is the first sample of the first packet, later packets +S each; a chunk off the packet grid is a DISCONTINUITY (D-41); `UNIT_END` is ignored |
| anc (ST40) | frame/field | 0 = no limit up to `pool` capacity | the unit's packets are sent in the ST 2110-40 window of the anc timing model, after the pacing sync (fixes finding #4); **bytes are wire order in both directions** (no RFC 8331 swapping by the library, fixes the ST40 TX/RX inconsistency, finding #5) |
| fastmeta (ST41) | frame/field or free-running rate | as anc | DIT/K-bit/length are the app's; VALIDATE checks the DIT against `data_item_type`; RX filters `MTL_FLOW_MATCH_FMD_DIT/K` apply |
| **generic RTP** (new) | frame/field at `unit_rate`; none with LAUNCH/ASAP | required with UNIT pacing | `clock_rate` required (2022-6: 27 MHz, from the `SMPTE2022-6/27000000` SDP convention [inferred]); `profile` LINEAR (default, equal spacing over the unit, as 2022-6 senders do) or GAPPED (ST 2110-21-like); `encoding` reported for SDP; RX dedup by 16-bit sequence |

ST 2022-6 at 1080i59.94 is about 4500 packets of 1396 B per frame [inferred from the standard's 1376 B payload and the 1.485 Gb/s rate], so generic RTP raises the per-session packet limit from 1352 B to the port's MTU − 28 (engine fix PE7).

### 4.8 RX

- **Ingest (tasklet).** The existing per-essence packet handlers keep their filters (PT, SSRC, flow filters), then **2022-7 dedup by sequence number** for every essence: the 32-bit extended sequence for video, the 16-bit sequence elsewhere, in a sliding bitmap window sized from `rx_skew_budget_ns × packet rate` (today audio dedups by timestamp and ANC/fastmeta by
  timestamp-or-sequence; one rule is simpler and handles reorder). The mbuf is then enqueued **with its reference taken first** (fixes #7) on a per-session ring of `rx_ring_packets`; only after a successful enqueue is the dedup bit set (fixes #8: a ring-full drop on one leg can still be filled by the other). Ring-full drops are counted as `MTL_RX_REJECT_RING_FULL`. No
  callback runs; if a waiter is armed and the ring crosses `rx_min_packets` or receives a marker packet, the armed-waiter protocol wakes it (04 §5.1).
- **Chunk formation at dequeue.** `mtl_rx_dequeue` takes up to `packets_per_chunk` packets (at least `rx_min_packets`, or whatever arrived within `rx_max_wait_ns`, Rivermax's moderation triple). With `MTL_PKT_RX_UNIT_ALIGNED` a chunk never spans two units (it ends at a marker or a timestamp change), which suits app-side frame assemblers.
- **Paths** (D-17's `pool.data_path`): **COPY** (`ALLOW_COPY`, default) copies each packet into the chunk's slots in the caller (DPC) and frees the mbufs in bulk, so NIC mbufs are held only while the ring is non-empty; SPLIT copies the header and payload into planes 0 and 1 (software header split: a GPU ingest app gets contiguous payloads in pinned memory for one H2D
  copy). **LEND** (`REQUIRE_DIRECT` / `PREFER_DIRECT`) points the packet table at the mbuf data and frees the mbufs at release; lent packets count against `rx_ring_packets`, and with PREFER a chunk falls back to COPY when the budget is exhausted (counted). Hardware header split stays later (NG6).
- **What is delivered.** The RX packet table (`struct mtl_pkt_rx`, 40 B per packet): `data` (the RTP header, or the Ethernet header with `MTL_PKT_RX_INCLUDE_L2` for analysers and recorders), `len`, `payload`/`payload_len` (SPLIT), `leg` (the leg whose copy was delivered), `seq` (extended for video), `gap` (missing sequence numbers before this packet), `flags`
  (`GAP_BEFORE`, `UNIT_START`, `MARKER`, `REDUNDANT`, `ARRIVAL_VALID`, `HW_ARRIVAL`) and `arrival_tai_ns`. Packets are in arrival order; reordering is not done (Q-PKT-6).
- **The chunk record** is the ordinary `mtl_rx_unit`: `pkts_expected` = packets in the chunk, `pkts_received[leg]`, `pkts_recovered` (delivered from leg 1 only), `valid_bytes`, `missing[]` as sequence ranges, and `timing` with `arrival_first/last` and, when the first packet's timestamp maps onto the session's RX timeline, `media_index` (06 §11). Unit-level views
  (frames) are the app's.
- **Analysers.** `MTL_PKT_RX_NO_DEDUP` delivers both legs' copies (the second flagged `REDUNDANT`), so path differential can be measured; HW arrival times through `caps.hw_timestamps = REQUIRE`.
- **RTCP NACK** is not offered in packet mode (it is non-standard and video-only, Q-MODE-8).

### 4.9 Results, stats, events

- **TX result per chunk** (`mtl_tx_result`): status and reason as for frames (`TOO_LATE`, `LINK_DOWN`, …, plus `PKT_COUNT` and `PKT_INVALID`), `pkts_sent[leg]`, `timing.media_index` / `media_tai_ns` of the unit, `timing.rtp` of the chunk's first packet on the wire, scheduled/enqueued/observed first-packet times, `max_packet_lateness_ns`, and timing flags `UNIT_START`,
  `UNIT_END`, `PKT_SHORT`. Completion NONE stays the default for library pools, so the common case reads no results.
- **Stats** (one schema, D-20): packets and bytes per leg, units, chunks, stamping and validation violations by reason, mid-unit late packets, RX ring-full, lend-to-copy fallbacks, dedup drops, sequence gaps.
- **Events:** the ordinary set (state, legs, flow state, recovery). `RX_SIGNAL` uses packet arrival.

### 4.10 Interaction with the rest of the API

- **Groups and timelines.** A packet session with SUBMIT unit time and UNIT pacing can be a group member: app-built ANC packets with `SET_TIMESTAMP` beside a library video session get the video's RTP and the D-67 window, the same as library ANC.
- **Holds and transfer (Phase 4).** `mtl_rx_transfer(rx, lease, tx, &tx_lease)` between packet sessions leases the TX chunk over the RX chunk's packets (COPY: its slots; LEND: its mbufs, which the TX side chains behind new L2–L4 header mbufs with a refcount, zero copies end-to-end), with an implicit hold until the TX result. The app may edit headers in place before
  submit. That is the zero-copy 1:1 gateway; held lent packets count against the RX budget.
- **Flow updates** (`mtl_session_update_flows`) change L2–L4 at a unit boundary (IS-05), never the RTP bytes.
- **Reconfigure in STOPPED** may change the packet config, not the unit kind.

### 4.11 Threading and call classes

| Call | Class | From a busy-loop thread (04 §3.3) |
|---|---|---|
| `mtl_tx_acquire`, `mtl_tx_release`, `mtl_tx_withdraw` | DP (WT with a timeout) | inline-safe with timeout 0 |
| `mtl_tx_submit` on a packet session | DP: O(1) count and flag checks; slot contents are read on the tasklet | inline-safe |
| `mtl_rx_dequeue`, COPY path | **DPC** (copies up to N packets in the caller) | `-MTL_EDEADLK`: use LEND from busy loops |
| `mtl_rx_dequeue`, LEND path | DP | inline-safe with timeout 0 |
| `mtl_rx_release` | DP (frees lent mbufs in bulk) | inline-safe |

Nothing on the tasklet waits for the app: an empty chunk queue is idle, a full RX ring drops and counts.

### 4.12 What packet mode deliberately does not do

Per-packet launch times (pcap inter-packet fidelity: later, Q-PKT-4); non-RTP raw UDP (Q-PKT-3); RX reordering; RTCP; hardware header split; app code on tasklets (the inline hooks of 04 §6 remain the only, opt-in, exception).

## 5. Header additions

Compact; it follows `mtl_unified.h` conventions C1–C11, the zero-default rule and fixed-size records. Everything is "since 0.2" (the revision that adds packet mode).

```c
/* ---- Packet mode: app-built RTP packets (S8; since 0.2) ---------------------- */
enum mtl_unit_kind { MTL_UNIT_FRAME = 0, MTL_UNIT_ROWS = 1, MTL_UNIT_PACKETS = 2 /* was PACKET_CHUNK */ };
enum mtl_essence { /* ... */ MTL_ESSENCE_RTP = 6 /* generic RTP: ST 2022-6, custom payloads; packets only */ };
enum mtl_struct_kind { /* ... */ MTL_STRUCT_PACKET_CONFIG = 44, MTL_STRUCT_RTP_CONFIG = 45 };

/* Renamed from mtl_rtp_mode / MTL_RTP_PASSTHROUGH: library-built packets only. */
enum mtl_rtp_ts_mode { MTL_RTP_TS_DERIVED = 0, MTL_RTP_TS_OVERRIDE = 1 };
#define MTL_SUB_RTP_TS 0x1u            /* submission.valid: rtp_ts_override is set (was MTL_SUB_RTP) */
/* MTL_REASON_RTP_TS_OVERRIDE_AUTO = 207 (was MTL_REASON_PASSTHROUGH_AUTO) */

enum mtl_pkt_pacing { MTL_PKT_PACE_UNIT = 0, MTL_PKT_PACE_LAUNCH = 1, MTL_PKT_PACE_ASAP = 2 };
enum mtl_pkt_unit_time { MTL_PKT_TIME_SUBMIT = 0, MTL_PKT_TIME_FROM_RTP = 1 };
enum mtl_pkt_layout { MTL_PKT_LAYOUT_CONTIG = 0, MTL_PKT_LAYOUT_SPLIT = 1 /* plane 0 headers, plane 1 payloads */ };

/* mtl_packet_config.set_fields: RTP header fields the library writes on TX; 0 = verbatim */
#define MTL_PKT_SET_TIMESTAMP 0x1u /* from the unit's media time (R-TIME-3) */
#define MTL_PKT_SET_SEQ 0x2u       /* 16-bit, plus the RFC 4175 extended sequence for video */
#define MTL_PKT_SET_SSRC_PT 0x4u   /* from flows[] */
#define MTL_PKT_SET_MARKER 0x8u    /* M on the last packet of a unit only */
/* mtl_packet_config.packet_flags */
#define MTL_PKT_TX_VALIDATE 0x1u     /* check, count and flag; never rewrite */
#define MTL_PKT_RX_INCLUDE_L2 0x2u   /* RX data starts at the Ethernet header */
#define MTL_PKT_RX_NO_DEDUP 0x4u     /* deliver both legs' copies, the second flagged REDUNDANT */
#define MTL_PKT_RX_UNIT_ALIGNED 0x8u /* a chunk never spans two units */

/* A next-chain block (11 §2.4); required when unit = MTL_UNIT_PACKETS. 128 B. */
struct mtl_packet_config {
  uint32_t struct_size;
  uint32_t kind;              /* MTL_STRUCT_PACKET_CONFIG */
  const void* next;           /* NULL */
  uint32_t packets_per_chunk; /* slots per lease; 0 = by essence (32 video/cvideo/rtp, 1 ms of audio, 8 anc/fastmeta) */
  uint32_t slot_bytes;        /* max RTP packet, header included; 0 = port MTU - 28 */
  uint32_t header_slot_bytes; /* SPLIT: header plane slot; 0 = 64 */
  uint32_t layout;            /* enum mtl_pkt_layout */
  uint32_t packets_per_unit;  /* UNIT pacing; 0 = derived (video) or unlimited (anc, fastmeta); required for cvideo and rtp */
  uint32_t pacing;            /* enum mtl_pkt_pacing */
  uint32_t unit_time;         /* enum mtl_pkt_unit_time */
  uint32_t rx_ring_packets;   /* RX: NIC mbufs the session may queue or lend; 0 = 512 */
  uint32_t rx_min_packets;    /* RX: a waiting dequeue returns at this many; 0 = 1 */
  uint32_t reserved0;
  int64_t rx_max_wait_ns;     /* RX: or after this long with >= 1 packet; 0 = one ptime (audio), else 1 ms */
  uint64_t set_fields;        /* MTL_PKT_SET_* */
  uint64_t packet_flags;      /* MTL_PKT_TX_*, MTL_PKT_RX_* */
  uint64_t reserved[6];
};
MTL_API_DP void mtl_packet_config_init(struct mtl_packet_config* p);

enum mtl_rtp_profile { MTL_RTP_PROFILE_LINEAR = 0, MTL_RTP_PROFILE_GAPPED = 1 };
/* Generic RTP essence. 120 B. */
struct mtl_rtp_config {
  uint32_t struct_size;
  uint32_t clock_rate;           /* RTP clock, Hz; required */
  struct mtl_rational unit_rate; /* frames per second; required for UNIT pacing */
  uint32_t interlaced;           /* enum mtl_scan: unit = field */
  uint32_t profile;              /* enum mtl_rtp_profile */
  uint64_t bitrate_bps;          /* LAUNCH/ASAP follow-on rate; 0 = packets_per_unit x slot_bytes x unit_rate */
  char encoding[32];             /* SDP rtpmap name, e.g. "SMPTE2022-6"; reported in mtl_session_info */
  uint64_t reserved[6];
};
MTL_API_DP void mtl_rtp_config_init(struct mtl_rtp_config* p);
MTL_API_CP int mtl_rtp_session_create(mtl_instance_h mt, const struct mtl_session_config* sc,
                                      const struct mtl_rtp_config* rc, mtl_session_h* out);
MTL_API_CP int mtl_rtp_session_query(mtl_instance_h mt, const struct mtl_session_config* sc,
                                     const struct mtl_rtp_config* rc, uint64_t flags,
                                     struct mtl_session_info* info, struct mtl_buffer_requirements* req);

/* TX packet table in the meta area: one entry per slot. 8 B. */
struct mtl_pkt_tx {
  uint16_t len;     /* RTP bytes in the slot (CONTIG) or payload bytes (SPLIT); 0 = skip */
  uint16_t hdr_len; /* SPLIT: bytes in the header slot; else 0 */
  uint32_t reserved;
};
/* RX packet table in the meta area: one entry per delivered packet. 40 B. */
#define MTL_PKT_F_GAP_BEFORE 0x1u
#define MTL_PKT_F_UNIT_START 0x2u /* timestamp differs from the previous delivered packet's */
#define MTL_PKT_F_MARKER 0x4u
#define MTL_PKT_F_REDUNDANT 0x8u  /* NO_DEDUP: a later copy */
#define MTL_PKT_F_ARRIVAL_VALID 0x10u
#define MTL_PKT_F_HW_ARRIVAL 0x20u
struct mtl_pkt_rx {
  MTL_ADDR(uint8_t) data;    /* RTP header (Ethernet with INCLUDE_L2) */
  MTL_ADDR(uint8_t) payload; /* SPLIT: payload slot; else NULL */
  uint16_t len;              /* bytes at data */
  uint16_t payload_len;      /* SPLIT */
  uint8_t leg;
  uint8_t flags;             /* MTL_PKT_F_* */
  uint16_t gap;              /* missing sequence numbers before this packet, saturating */
  uint32_t seq;              /* extended for video, else 16-bit */
  uint32_t reserved;
  int64_t arrival_tai_ns;    /* valid with MTL_PKT_F_ARRIVAL_VALID */
};

/* mtl_tx_submission: one uint64_t of reserved[4] becomes */
uint32_t pkt_count;              /* packet mode: leading slots used; 0 = all */
uint32_t reserved1;
#define MTL_SUBMIT_UNIT_END 0x2u /* packet mode: last chunk of its unit (not audio) */
/* mtl_tx_timing.flags */
#define MTL_TX_TIMING_UNIT_START 0x40u
#define MTL_TX_TIMING_UNIT_END 0x80u
#define MTL_TX_TIMING_PKT_SHORT 0x100u /* the unit ended with fewer than packets_per_unit */
/* enum mtl_tx_reason: MTL_TX_REASON_PKT_COUNT = 23, MTL_TX_REASON_PKT_INVALID = 24 */
/* enum mtl_rx_reject: MTL_RX_REJECT_RING_FULL = 13 */
/* mtl_session_info gains (from its reserved tail): packets_per_unit, packets_per_chunk, slot_stride,
   pkt_payload_bytes and pkts_per_line of the library packetiser layout (video), rx path */

/* Inline helpers (exported twins for bindings) */
static inline uint8_t* mtl_pkt_slot(const struct mtl_buffer_view* v, uint32_t plane, uint32_t i) {
  return (uint8_t*)v->plane[plane].addr + (size_t)i * v->plane[plane].stride;
}
static inline struct mtl_pkt_tx* mtl_pkt_tx_table(const struct mtl_buffer_view* v) {
  return (struct mtl_pkt_tx*)v->meta;
}
static inline const struct mtl_pkt_rx* mtl_pkt_rx_table(const struct mtl_buffer_view* v) {
  return (const struct mtl_pkt_rx*)v->meta;
}
```

Size checks to add: `mtl_packet_config` 128, `mtl_rtp_config` 120, `mtl_pkt_tx` 8, `mtl_pkt_rx` 40; `mtl_tx_submission` keeps its size. The meta capacity of a packet chunk is `packets_per_chunk × sizeof(record)`, reported in `mtl_buffer_requirements.meta_capacity`.

## 6. Examples

### 6.1 TX: app-built RFC 4175 packets, library pacing, 2022-7

An app-side packetiser (for example a hardware block that emits RFC 4175 packets). The library stamps the timestamp and SSRC/PT, the app owns sequence numbers, SRD headers and the marker.

```c
/* ex13_tx_packets.c — app-built ST 2110-20 packets on two legs (S8 §6.1). */
#include "ex_common.h"

/* the app's packetiser: writes packet j of the frame into dst, returns its length */
uint16_t packetise_rfc4175(uint8_t* dst, uint32_t frame, uint32_t j, uint32_t* seq);

int tx_packets(mtl_instance_h mt) {
  struct mtl_session_config sc;
  mtl_session_config_init(&sc);
  sc.direction = MTL_DIR_TX;
  sc.unit = MTL_UNIT_PACKETS;
  int ret = mtl_flow_parse("239.168.85.20:20000,pt=112", &sc.flows[0]);
  if (ret == 0) ret = mtl_flow_parse("239.168.86.20:20000,pt=112", &sc.flows[1]); /* 2022-7 */
  if (ret < 0) return ex_fail("flow", ret);

  struct mtl_packet_config pc;
  mtl_packet_config_init(&pc);
  pc.set_fields = MTL_PKT_SET_TIMESTAMP | MTL_PKT_SET_SSRC_PT;
  pc.packets_per_unit = 4320; /* 1080p 4:2:2 10-bit, 1200 B payloads */
  pc.slot_bytes = 1200 + 20;  /* RTP + one SRD */
  sc.next = &pc;              /* UNIT pacing (ST 2110-21 narrow), SUBMIT unit time, AUTO media mode */

  struct mtl_video_config vc;
  mtl_video_config_init(&vc);
  vc.width = 1920;
  vc.height = 1080;
  vc.fps = mtl_fps_rational(MTL_FPS_59_94);
  vc.transport_format = MTL_VIDEO_YUV422_10BIT;

  mtl_session_h s;
  ret = mtl_video_session_create(mt, &sc, &vc, &s);
  if (ret < 0) return ex_fail("create", ret);
  ret = mtl_session_start(s, NULL);
  if (ret < 0) goto out;

  uint32_t seq = 0, frame = 0, j = 0;
  while (g_running) {
    mtl_lease_h lease;
    struct mtl_buffer_view v;
    mtl_buffer_view_init(&v);
    ret = mtl_tx_acquire(s, &lease, &v, NULL, MTL_MS(100));
    if (ret == -MTL_ETIMEDOUT) continue;
    if (ret < 0) break;
    struct mtl_pkt_tx* t = mtl_pkt_tx_table(&v);
    uint32_t n = 0;
    for (; n < v.plane[0].rows && j < pc.packets_per_unit; n++, j++)
      t[n].len = packetise_rfc4175(mtl_pkt_slot(&v, 0, n), frame, j, &seq);
    struct mtl_tx_submission sub;
    mtl_tx_submission_init(&sub);
    sub.pkt_count = n;
    if (j == pc.packets_per_unit) { /* this chunk ends the frame */
      sub.flags = MTL_SUBMIT_UNIT_END;
      j = 0;
      frame++;
    }
    ret = mtl_tx_submit(s, lease, &sub); /* the library duplicates per leg, L2-L4 only */
    if (ret < 0) {
      mtl_tx_release(s, lease);
      break;
    }
  }
  ret = mtl_session_stop(s, MTL_STOP_DRAIN, MTL_SEC(1));
out:
  mtl_session_destroy(s, 0);
  return ret < 0 ? ex_fail("tx_packets", ret) : 0;
}
```

### 6.2 RX: raw RTP analysis and verbatim forwarding

An analyser that watches sequence gaps and per-leg arrival on a 2022-7 video stream and forwards every packet unchanged to a unicast destination, keeping the input's timestamps with a fixed delay.

```c
/* ex14_rx_packets.c — dedup, analyse, forward verbatim (S8 §6.2). rx: a video RX session with
   unit = MTL_UNIT_PACKETS (two legs); tx: a generic RTP TX session (mtl_rtp_session_create) with
   unit = MTL_UNIT_PACKETS, set_fields = 0 (verbatim), unit_time = MTL_PKT_TIME_FROM_RTP,
   pacing = MTL_PKT_PACE_UNIT, timing.source_kind = MTL_SOURCE_GATEWAY, timing.min_tx_delay_ns = 2 ms,
   clock_rate 90000 and the input's unit rate. */
#include "ex_common.h"
#include <string.h>

void note_gap(uint32_t seq, uint16_t missing);
void note_arrival(uint8_t leg, int64_t tai_ns);

int analyse_and_forward(mtl_session_h rx, mtl_session_h tx) {
  int ret = 0;
  while (g_running) {
    mtl_lease_h in;
    struct mtl_buffer_view v;
    struct mtl_rx_unit u;
    mtl_buffer_view_init(&v);
    ret = mtl_rx_dequeue(rx, &in, &v, &u, sizeof(u), MTL_MS(10)); /* 1..N packets, COPY path */
    if (ret == -MTL_ETIMEDOUT) continue;
    if (ret < 0) break;
    const struct mtl_pkt_rx* p = mtl_pkt_rx_table(&v);

    mtl_lease_h out;
    struct mtl_buffer_view w;
    mtl_buffer_view_init(&w);
    ret = mtl_tx_acquire(tx, &out, &w, NULL, 0);
    struct mtl_pkt_tx* t = ret == 0 ? mtl_pkt_tx_table(&w) : NULL;
    uint32_t n = 0;
    for (uint32_t i = 0; i < u.pkts_expected; i++) {
      if (p[i].flags & MTL_PKT_F_GAP_BEFORE) note_gap(p[i].seq, p[i].gap);
      if (p[i].flags & MTL_PKT_F_ARRIVAL_VALID) note_arrival(p[i].leg, p[i].arrival_tai_ns);
      if (t && n < w.plane[0].rows) { /* verbatim copy; mtl_rx_transfer avoids it (S8 §4.10) */
        memcpy(mtl_pkt_slot(&w, 0, n), p[i].data, p[i].len);
        t[n++].len = p[i].len;
      }
    }
    if (t) {
      struct mtl_tx_submission sub;
      mtl_tx_submission_init(&sub);
      sub.pkt_count = n;
      if (u.pkts_expected && (p[u.pkts_expected - 1].flags & MTL_PKT_F_MARKER))
        sub.flags = MTL_SUBMIT_UNIT_END; /* rx has MTL_PKT_RX_UNIT_ALIGNED: the marker ends a chunk */
      if (mtl_tx_submit(tx, out, &sub) < 0) mtl_tx_release(tx, out);
    } /* else: TX back-pressure, the chunk is not forwarded and the TX stats show it */
    ret = mtl_rx_release(rx, in);
    if (ret < 0) break;
  }
  return ret < 0 && ret != -MTL_ESHUTDOWN ? ex_fail("analyse", ret) : 0;
}
```

## 7. Cost on the data path

All per-packet numbers are operation counts [inferred]; nanoseconds need the spike SP-PKT (§8.5).

| Step | Today (RTP level) | Packet mode |
|---|---|---|
| TX app thread, per packet | `get_mbuf` (one mempool get), `put_mbuf` (one SP ring enqueue) | none: slot write + one 2-byte length; per chunk one acquire CAS and one submit |
| TX tasklet, per packet | ring dequeue (per bulk of 4), one header mbuf, L2–L4 write, timestamp read + store, `notify_rtp_done` call per bulk (app code) | one header mbuf + one attached mbuf (bulk alloc), L2–L4 write, optional stamp stores; per chunk one descriptor dequeue and one `shinfo` init; no app call |
| TX 2022-7, per packet | +1 header mbuf, refcount +1 (chain) or a full copy (no-chain) | same |
| TX completion | none per chunk (per-packet mbuf frees) | one `free_cb` per chunk at refcount 0 (CAS + release store + fence, 04 §4.1) |
| RX tasklet, per packet | dedup, ring enqueue, refcount +1, `notify_rtp_ready` (app code) per packet | dedup, refcount +1, ring enqueue; a wake only when armed and a threshold is crossed (once per burst at most) |
| RX app thread, per packet | ring dequeue, `put_mbuf` (free) | COPY: one memcpy (≈ 1.2 KB, already paid today by frame-mode RX on the tasklet) + bulk free; LEND: table fill only |
| Memory | `rtp_ring_size` mbufs + data rooms | TX: `pool.count × packets_per_chunk × stride` in one region (1080p, 32 slots × 1280 B × 270 chunks ≈ 11 MB for two frames) plus header/attach mbufs; RX: `rx_ring_packets` NIC mbufs + COPY chunks |

The new path removes every per-packet ring operation and app callback, moves RX copies off the pinned tasklet, and adds one attached mbuf per TX packet (today's app mbuf plays that role, so the mbuf count per packet is unchanged).

## 8. Effect on the plan

### 8.1 Documents

| Where | Change |
|---|---|
| [01](../01-goals-and-requirements.md) §4 NG2 | **Deleted.** New GO-10: every RTP-level use case of §2.2 has a unified equivalent. New R-PKT-1 (packet units on every essence + generic RTP), R-PKT-2 (only L2–L4 and declared stamp fields; verbatim by default), R-PKT-3 (one result per chunk), R-PKT-4 (no mbufs, no app code on tasklets), R-PKT-5 (bounded RX mbuf hold) |
| [09](../09-media-modes-and-backends.md) | §2.1 rewritten from this document (normative); §2 table: "packet mode (phase 2P)" per essence plus a generic RTP row; §3 rows for packet mode, stamping, LAUNCH/ASAP, RX LEND; §8 "reserved" → "v1 (phase 2P)"; §9 defaults for every new field; §1 `unit` comment |
| [06](../06-timing-pacing-and-sync.md) §5.4 | renamed "RTP timestamp override"; a new §5.6 "Packet units" (unit time, pacing modes, mid-unit underrun, FROM_RTP inverse) |
| [05](../05-memory-and-buffers.md) §4, §9, §11 | packet chunk shape (plane = slots, meta = packet table); M-rows for the extbuf chunk mapping and RX LEND |
| [07](../07-completions-events-and-errors.md) | new TX reasons, RX reject slot, renamed reason 207; §6 identities count chunks |
| [03](../03-object-model-and-lifecycle.md) §4.2 | packet RX skips RECEIVING/READY |
| [04](../04-threading-and-execution.md) §3.3, §10 | the COPY/LEND class split of §4.11 |
| [10](../10-api-sketch.md), `sketch/` | §5 additions, `ex13_tx_packets.c`, `ex14_rx_packets.c`; `check.sh` covers them |
| [11](../11-abi-compatibility-and-migration.md) §2.4, §6 | the typed `next` chain header (first user); the legacy mapping table of §8.4 |
| [12](../12-familiarity-libfabric-and-rivermax.md) | the "packet-chunk RX is reserved (NG2)" row becomes the Rivermax mapping of §3 |
| [13](../13-guarantees-and-tests.md) | §8.3 |
| [14](../14-implementation-roadmap.md) | §8.2 |
| `DECISIONS.md` | D-25 → **Superseded by D-71**; new D-71 (packet units first-class, §4.2–4.8), D-72 (the `rtp_ts_mode` rename and the no-"passthrough" naming rule), D-73 (generic RTP essence), D-74 (verbatim by default, explicit stamping) |
| `OPEN-QUESTIONS.md` Q-MODE-1 | **Answered by the owner** (2026-10-01): packet mode inside the unified session, every essence, legacy session headers to be hidden; replaced by Q-PKT-1…9 (§9) |
| repository docs (side findings) | `doc/design.md:618` stale; `README.md:46` claims a 2022-6 route that cannot send 2022-6 packets; the programmer's guide RTP section; `st40p_rx_ops.rtp_ring_size` deprecation |

### 8.2 Roadmap

Revision 3 put packet mode in Phase 6 (`14-implementation-roadmap.md:296`). Hiding the session headers needs it before the ABI freeze, and it reuses Phase 1–2 machinery (lease table, legs, flows), so it becomes **phase 2P**, schedulable right after Phase 2, in three steps:

| Step | Scope | Exit |
|---|---|---|
| 2P-a (after Phase 2) | TX/RX packet units for video, anc, fastmeta and generic RTP; AUTO media mode; UNIT and ASAP pacing; chain TX and COPY RX; stamping and VALIDATE; 2022-7; PE1–PE8 | RxTxApp `"type": "rtp"` and the two low-level samples ported; ST41 acceptance tests pass on the unified path; the guarantees of §8.3 at the U tier |
| 2P-b (with Phase 3) | INDEX/TAI and FROM_RTP unit time, LAUNCH pacing, audio and cvideo packet units, groups | pcap replay test (§8.3) at the I tier; a 2022-6 stream accepted by a third-party receiver or analyser |
| 2P-c (with Phase 4) | RX LEND, `mtl_rx_transfer` / holds between packet sessions, SPLIT layouts, imported regions | zero-copy 1:1 gateway with path counters showing no copies |

Effort: roughly 4–7 EM on C5's yardstick [inferred]: most of the work is the chunk ingest in five TX engines and one RX ring discipline, not new pacing. Deprecation of `*_TYPE_RTP_LEVEL` and the `*_get_mbuf` family follows the 14 §7 policy once 2P-a ships.

### 8.3 Guarantees and tests

| New guarantee | Test | Tier |
|---|---|---|
| G-PKT-1 verbatim: with `set_fields = 0` every byte from the RTP header on reaches the wire unchanged, on every leg | null-backend loopback TX → RX with `INCLUDE_L2` off, byte compare; I tier: capture compare | U, I |
| G-PKT-2 stamping writes exactly the declared fields, identical on both legs | loopback, field-by-field | U |
| G-PKT-3 one result per chunk; accepted = published + suppressed (07 §6) under withdraw, stop(FLUSH), leg down, destroy | identity check with fault injection | U |
| G-PKT-4 count rules: over-count rejected at submit, short units flagged, the grid kept | loopback with scripted counts | U |
| G-PKT-5 RX never holds more than `rx_ring_packets` NIC mbufs; ring-full on one leg is filled by the other | `mtl_debug_inject(DROP_PKTS)` per leg + a stalled reader | U, I |
| G-PKT-6 dedup by sequence: one delivery per sequence number, `NO_DEDUP` delivers both, `gap` exact | loopback with injected loss and reorder | U |
| G-PKT-7 UNIT pacing passes the ST 2110-21 narrow check for video when the declared count is sent; FROM_RTP keeps a fixed RTP-to-launch offset | timing oracle (13 §7), EBU LIST | I |
| G-PKT-8 call classes: COPY dequeue from a busy loop is `-MTL_EDEADLK`; LEND and TX verbs are inline-safe | busy-loop harness | U |

- **Null backend.** Packet sessions are supported from 2P-a: a TX chunk completes at its scheduled instant, and a loopback RX session on the same port receives the packets with synthetic arrival times. This makes packet mode the cheapest way to build U-tier tests of the RX frame engines too (the out-of-order and truncated-frame cases of
  `tests/integration_tests/st20/st20_digest.cpp:567` and `st20_meta.cpp:256` move to the U tier).
- **pcap replay test.** RxTxApp (unified path) replays a reference ST 2110-20 pcap and the existing ST22 pcap (`tests/tools/RxTxApp/script/loop_json/st22p_pcap.json`) through packet sessions with verbatim headers and FROM_RTP unit time into frame RX sessions; the digest must match the capture and the TX timing must pass the oracle.
- The ST40 RTP fuzz target is re-pointed at the packet-mode RX path (finding #15).

### 8.4 Legacy mapping (for 11 §6 and the deprecation notes)

| Legacy | Unified |
|---|---|
| `*_TYPE_RTP_LEVEL` | `sc.unit = MTL_UNIT_PACKETS` + `mtl_packet_config` |
| TX `rtp_ring_size` | `pool.count × packets_per_chunk` |
| RX `rtp_ring_size` | `rx_ring_packets` |
| `rtp_frame_total_pkts` | `packets_per_unit` |
| `rtp_pkt_size` | `slot_bytes` |
| `notify_rtp_done` / `notify_rtp_ready` (tasklet callbacks) | results, `mtl_session_get_wait_object`, `dequeue` with a timeout |
| `*_tx_get_mbuf(&usrptr)` / `*_tx_put_mbuf(mbuf, len)` | `mtl_tx_acquire` → slots; `mtl_pkt_tx.len` + `mtl_tx_submit` per chunk |
| `*_rx_get_mbuf(&usrptr, &len)` / `*_rx_put_mbuf` | `mtl_rx_dequeue` → `mtl_pkt_rx[]`; `mtl_rx_release` |
| default timestamp rewrite / `USER_TIMESTAMP` | `MTL_PKT_SET_TIMESTAMP` / verbatim |
| `USER_PACING`, `EXACT_USER_PACING` (ignored today) | INDEX/TAI media mode with UNIT pacing, or LAUNCH pacing |
| `RTP_TIMESTAMP_EPOCH`, `rtp_timestamp_delta_us` | derived RTP is epoch-based (D-10); `media_time_offset_ns` |
| RTP-level `ENABLE_RTCP` | not offered (Q-MODE-8) |
| `st40p_rx_ops.rtp_ring_size` | removed: dead since `d74cd1e0` |
| frame boundary by timestamp change | `MTL_SUBMIT_UNIT_END` |

### 8.5 Engine fixes and spike (engines first, D-24: bugfixes reach legacy users)

| # | Fix | Evidence |
|---|---|---|
| PE1 | chunk ingest in the five TX engines (ring of chunk descriptors; extbuf attach per slot; per-chunk `shinfo` completion) | TX:1295, :1706 (the frame chain pattern) |
| PE2 | explicit unit end; per-unit packet accounting; field parity from the unit index | TX:1392-1410 |
| PE3 | media time and launch into the RTP-mode pacing sync (shared with E1) | TX:1410; `st_tx_audio_session.c:992` |
| PE4 | ST40/ST41 RTP TX: sync before the gate | `st_tx_ancillary_session.c:1208-1221`, `:752`, `:816`; `st_tx_fastmetadata_session.c:956` |
| PE5 | ST40 chain pointer and byte-order paths | `st_tx_ancillary_session.c:805-807`, `:739-741` |
| PE6 | RX: reference before enqueue, dedup bit after enqueue, `last_pkt_idx` reset, a past-timestamp guard; the same ordering in ST30/40/41 | RX:1962-1969, :1920, :1324-1329 |
| PE7 | per-session packet size limit from the MTU (2022-6 needs 1396 B) | `include/mtl_api.h:89`, `mt_util.h:19-24` |
| PE8 | ST41 RX: dedup threshold counter, duplicates not errors, DIT/K filters | `st_rx_fastmetadata_session.c:159-168`, `:228-229` |
| PE9 | legacy-only: validate power-of-two ring sizes and ST22 `rtp_frame_total_pkts`; fix the `notify_rtp_done` doc; TX RTCP chain offset | TX:3012-3015, :4208-4221; `mt_rtcp.c:41-43` |

**Spike SP-PKT** (Phase 0 list): per-packet tasklet cost of extbuf attach + header mbuf vs today's RTP level at 1080p59.94 and 2160p59.94, two legs; RX COPY cost on the app thread; chunk size sweep (8–128) against pacing jitter.

## 9. Open questions

| ID | Question | Options | Recommendation |
|---|---|---|---|
| Q-PKT-1 | Default stamping | (a) verbatim (0); (b) timestamp derived (today's default) | (a): it is what "passthrough" means, and legacy behaviour maps to one bit |
| Q-PKT-2 | RX chunk formation | (a) at dequeue by count/timeout (moderation); (b) per unit by the library | (a), with `UNIT_ALIGNED` for assemblers |
| Q-PKT-3 | Generic essence scope | (a) RTP only; (b) also raw UDP payloads (no sequence, no dedup) | (a) in 2P; (b) only on demand |
| Q-PKT-4 | Per-packet launch offsets (pcap inter-packet fidelity, test shaping) | (a) later via `mtl_pkt_tx.reserved`; (b) never | (a); SW pacing only |
| Q-PKT-5 | RX default path | (a) COPY in the caller; (b) LEND | (a): bounded NIC hold by construction |
| Q-PKT-6 | RX reordering window | (a) none, arrival order with `gap`; (b) optional window | (a) |
| Q-PKT-7 | ST 2022-6 validation | (a) generic RTP only, app writes HBRMT headers; (b) a 2022-6 helper for HBRMT header fields | (a) plus an L4 helper later if asked; needs a 2022-6 receiver or analyser for the 2P-b exit |
| Q-PKT-8 | Test fault injection (OOO, truncated units) | (a) through public packet mode; (b) only through `mtl_debug_inject` | (a) for tests; (b) stays for mutations of library-built streams |
| Q-PKT-9 | Phase | (a) 2P after Phase 2 (§8.2); (b) Phase 6 as in revision 3 | (a): hiding the session headers depends on it |

## 10. Consistency with S4 and S9

- **S9 (hide session headers).** Packet mode is the replacement that lets `*_TYPE_RTP_LEVEL`, `*_get_mbuf`, `*_put_mbuf`, `rtp_ring_size` and the RTP callbacks leave the public surface; §8.4 is the mapping S9's deprecation list can cite. Out of scope here and left to S9: `DATA_PATH_ONLY` and `st20_rx_get_queue_meta` (Q-MODE-5), the pcapng dump, slice mode, and the ST
  2110 header structs (`st20_rfc4175_rtp_hdr`, `st40_rfc8331_rtp_hdr`, …) that packet-mode apps need to *parse* packets. Those structs are wire formats, not session API, and could move to a small `mtl/wire/` header with no functions.
- **S4 (simplified data path, "unit" struct).** This proposal uses revision 3's buffer view, meta area, submission and `mtl_rx_unit` unchanged except for the fields in §5. If S4 merges them into one unit descriptor, the packet table becomes a typed member of that descriptor (`pkts`, `pkt_count`) instead of living in the meta area, and `MTL_SUBMIT_UNIT_END` becomes a
  flag of the descriptor. The semantics are unaffected: a chunk is a unit, one result per chunk, explicit release, the library owns L2–L4 and the declared stamp fields.
