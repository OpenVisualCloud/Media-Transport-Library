# Implementation plan

| | |
|---|---|
| Status | Plan of record. Nothing is implemented. The decisions it rests on are in [decisions.md](decisions.md) (D-99…D-112 shape it) |
| Date | 2026-10-02 |
| Baseline | `main` @ `545a266a`; the headers in [sketch/include/mtl/experimental/](sketch/include/mtl/experimental/) |

The headers are normative. Every API name in this plan is a header name; names inside the
library (the core, the bindings) are indicative.

The milestones are **MS1…MS7** (§1.2, §5, §6), MS2 and MS4 in two parts each, followed by
Phase 7. The Kubernetes fixes run as a parallel track (§6.10).

## 1. Assumptions, goal and milestones

### 1.1 The assumptions

Set by the maintainer; every choice below follows from them.

| # | Assumption | Consequence |
|---|---|---|
| A1 | AI agents (Claude Opus 5.5) write the code and the tests | effort is no longer the limit; the maintainer's review is (§10) |
| A2 | something works within one month | MS1 is a usable vertical slice: ST 2110-20 frames TX and RX on the new API, a test pool, RxTxApp and acceptance on it (§5) |
| A3 | every feature of today's API stays, except the capabilities D-112 removes ([coverage.md](coverage.md)) | no other cut (D-112) |
| A4 | slice mode is required (users exist) | rows units are in MS2a (D-101) |
| A5 | the decisions of [decisions.md](decisions.md) and the accepted resolutions of the open issues hold | their end states hold; where MS1 ships an interim (no command acks; libmtl without a soname, an `MTL_LEGACY` node or hidden internals) the milestone that reaches the end state is named (D-103, D-108) |
| A6 | elegant means small | one core under every API (D-99), one library with version nodes (D-23), the export list that `sketch/check.sh` prints (D-104), no layer that exists only for the transition |

### 1.2 The milestones

| Milestone | Scope | Calendar [I] |
|---|---|---|
| **MS1** ST 2110-20 frames | the core, the video and null bindings, the API shell inside libmtl for ST20 frames TX and RX, a test pool at four tiers, RxTxApp `st20p` on the new API, the acceptance smoke set (§5) | weeks 1–4 |
| **MS2a** ST 2110-20 rows and completeness | the nightly with the legacy comparison (§6.9); the W3 contingency check (spike S1); the `tick` with command acks, discard; **rows (slice)**; RX zero fill of library pools, due time and `MTL_SESSION_RX_LATEST`; the direct open of PCI ports and `native_af_xdp:`; `mtl_log_set_sink`; the MS1 stretch tasks that did not land (§6.1) | month 2 |
| **MS2b** video memory and the st20p re-base | attach, by index, per-acquire layouts, holds, split-forward, converter plugins; then, last, st20p re-based on the core once the nightly comparison has burned in (§6.1) | month 3 |
| **MS3** timing and observability | the ST20 timing subset (INDEX, start at TAI or index, `mtl_tx_next_slot`, RX `media_index`, E1, E2, E3 reporting); events, the stats registry's per-scheduler blocks, health; the libmtl soname, the `MTL_LEGACY` node and hidden internals; debug call-class checks; the FFmpeg plugin's st20p path on the new API (§6.2) | month 4 |
| **MS4a** audio, ANC and A/V sync | audio (st30p re-based), ANC (st40p re-based), fastmeta (st41 frames), A/V sync on the epoch; any frame rate; DSCP on their bindings; RxTxApp kinds and their gtests (§6.3) | month 5 |
| **MS4b** compressed video and plugins | cvideo (st22p re-based; codec plugins on the transform state), plugin ABI v2, `mtl_convert` (§6.3) | month 6 |
| **MS5** packets and operators | packet units on every essence over today's RTP paths (one shared chunk expander, PE1–PE8), the generic `MTL_RTP` essence and ST 2022-6 (PE7); `mtl_session_update` (flows, legs, media, pool) at a boundary; RTCP sender reports on TX (their names leave `MTL_LATER`); the link monitor, capacity query, manager reconnect (§6.4) | month 7 |
| **MS6** synchronisation and the ecosystem | start arrays, ANC and fastmeta following their video, sample-accurate audio, E5–E13, the published time base with FREERUN (E9; its name leaves `MTL_LATER`); recovery on library workers; the GStreamer, OBS, Python and Rust ports and the rest of the FFmpeg plugin (§6.5) | months 8–9 |
| **MS7** freeze and hide | the external review with the named consumers; the `MTL_1.0` freeze; the hiding stages F, F+1, F+2 (D-83) (§6.6) | month 10, then the deprecation releases |
| Phase 7 | NMOS extras, SDP, IPMX timing and the RTCP MIB, encryption, PEP ([nmos-ipmx.md](nmos-ipmx.md)), declared under `MTL_LATER` until then (D-98); a Phase 7 name leaves `MTL_LATER` in the milestone that implements it (RTCP sender reports MS5, FREERUN MS6) | after MS7 |

Shared queues and created timelines are not in v1: they stay under `MTL_LATER` in the headers.
Every session runs on the epoch timeline, and `mtl_wait`, `mtl_get_wait_handle` and
`mtl_read_events` take an instance as well as a session.

The calendar assumes about three to four reviewed tasks a week (D-107). It is an estimate; the
exit criteria, not the dates, end a milestone.

### 1.3 Non-goals of MS1

Each is in a named later milestone. A function of a later milestone is declared in the installed
header but not exported until that milestone, so a call to it fails at link time; a later value of an exported call (a
flag, an enum value, an option key, a port prefix, a `when` kind, a wait mask, an update part)
returns `-MTL_ENOTSUP` with `MTL_REASON_NOT_IMPLEMENTED` until then (D-106).

- other essences (MS4a, MS4b); packet units (MS5); rows units (MS2a);
- attached or imported memory, holds, per-acquire layouts (MS2b for video);
- `mtl_session_update` (MS5), `mtl_session_discard` (MS2a);
- media mode INDEX, start at a TAI or an index, `mtl_tx_next_slot`, `MTL_SUBMIT_RTP_TS` (MS3);
  start arrays (MS6);
- events, health, shutdown report (MS3);
- command acks, RX due-time force-complete, zero fill of lost ranges in library pools (MS2a);
- the direct open of PCI ports and `native_af_xdp:` (they reach unified sessions through the
  legacy bridge in MS1), `mtl_log_set_sink` (MS2a);
- RX DMA, two RX threads, the RX timing parser, per-packet conversion and `MTL_SUBMIT_EXACT`:
  stretch task B3, else MS2a;
- the legacy pipelines re-based on the core (MS2b for st20p).

No ported feature regresses for legacy users: the legacy API is untouched in MS1 except for
bugfixes, and the legacy gate (§8.5) passes at every milestone. The built-in PTP client on a VF
keeps working with its software time base (§2.4).

## 2. The architecture

### 2.1 One core, bindings, an API shell

The picture, the parts and their rules are in [engine.md](engine.md) §1; in short:

- **The core** (`lib/src/st2110/core/`, in libmtl) is the unit session: a slot table with one
  64-bit slot word per slot (state, claim, holds, generation), an order ring of 64-byte submission
  descriptors that is the pick-up, result and reap order, the nine session states (D-08), the armed
  wait with one `mt_wake()`, the handle table (R4) and the transform state for conversion and codecs
  (D-100). It reaches its environment (allocation on a socket, the clock, the wake flush, the
  scheduler ID) through an instance-context interface, never through `struct mtl_main_impl*`.
- **A binding** per essence and direction (plus `packet` and `null`) implements the callbacks the
  engines already call (`get_next_frame`, `notify_frame_done`, `query_frame_lines_ready`,
  `query_ext_frame`, `notify_frame_ready`, `notify_slice_ready`, `notify_detected`). It runs on the
  tasklet under the session spinlock and is wait-free: CAS, release store, fence, armed-word load,
  and no syscall. The few other engine entry points it uses are listed in one engine accessor
  header, `st_engine_core.h`.
- **The API shell** (`lib/src/unified/`, compiled into libmtl, exported in the node
  `MTL_UNIFIED_EXPERIMENTAL_<rev>`) translates the typed config into engine `ops` through the option
  table, maps reasons and enforces call classes. It holds no data-path state.
- **The legacy pipelines** `st*p_*` become wrappers on the core as soon as their essence is on it
  (st20p MS2b, the others MS4a and MS4b); the legacy session API stays as the engines' interface.
- The core has the lease-table shape from MS1, so packet units, attached pools and holds add
  bindings and slot fields, never a second core. The module list is in [engine.md](engine.md) §3.

### 2.2 Frames, rows and packets in one slot model

| Unit | `progress` | Binding |
|---|---|---|
| frame TX | all rows, set at submit | `get_next_frame` takes the descriptor at the pick cursor whose slot word is QUEUED with the descriptor's generation, decides the slot and fills the engine meta from the descriptor |
| rows TX (MS2a) | raised by each re-submit | the session is created `ST20_TYPE_SLICE_LEVEL`; `query_frame_lines_ready` returns `progress` |
| frame RX | — | `query_ext_frame` hands the engine a core-allocated slot; `notify_frame_ready` publishes it in `pub_seq` order |
| rows RX (MS2a) | rows received | `notify_slice_ready` stores `progress` and wakes `mtl_rx_wait_rows` |
| packets TX (MS5) | packets in the chunk | the same FIFO; one shared chunk expander attaches the chunk's buffers and feeds the engine's RTP ring; one completion per chunk |
| packets RX (MS5) | packets copied | the chunk is formed at dequeue from the engine's RTP ring |

### 2.3 Scope map

The inventories of today's surface are the operating-mode catalogue
([legacy-internals.md](legacy-internals.md)) and the use-case inventory of every public header,
rows U-001…U-418 ([coverage.md](coverage.md)). The home of a feature's milestone is its U-row
in coverage.md; this section is the index an implementer works from, and where the two
disagree, coverage.md is right. The coverage gate D-87 holds: every legacy capability has a
unified home and a milestone, or is removed (D-112). The rows that still need a home (U-348,
U-402; OI-40, OI-42) are in [coverage.md](coverage.md).

**Today's surfaces** ([legacy-internals.md](legacy-internals.md)): five media families × two
directions × up to three units of work × two layers, plus `st20rc`:

| Today | Unified home | When |
|---|---|---|
| `st20p_tx_*`, `st20p_rx_*` (frame) | `MTL_VIDEO`, `MTL_UNIT_FRAME`, conversion by `v.app_format` | MS1 |
| `st20_tx_*`, `st20_rx_*` `ST20_TYPE_FRAME_LEVEL` (callbacks, ext frames) | the same session, push model; derive mode `v.app_format = 0`; app memory through `mtl_mem.h` | MS1 for library pools; ext frames MS2b |
| `ST20_TYPE_SLICE_LEVEL` | `MTL_UNIT_ROWS` | MS2a |
| `*_TYPE_RTP_LEVEL` (every family), ST 2022-6 through RTP level | `MTL_UNIT_PACKETS` (`mtl_packet.h`); `MTL_RTP` for ST 2022-6 and custom payloads | MS5 (D-82) |
| `st22_*`, `st22p_*` | `MTL_CVIDEO` | MS4b |
| `st30_*`, `st30p_*` | `MTL_AUDIO` | MS4a; sample-accurate submission MS6 |
| `st40_*`, `st40p_*` | `MTL_ANC` | MS4a; ANC following its video in a start array MS6 |
| `st41_*` (RX RTP level only today) | `MTL_FASTMETA`, frame RX added | MS4a |
| `st20rc_rx_*` | none: removed (D-112); two legs on one session do the same | — |
| `st22_encoder_register`, `st20_converter_register`, `st_plugin_register` | plugin ABI v2 (`mtl_plugin.h`) | MS4b; converter plugins on the transform state MS2b |
| `st_convert_api.h` (per-pair converters) | `mtl_convert()` (`mtl_format.h`); the public per-pair converters are removed (D-112) | MS4b |
| `mtl_sch_*` user schedulers and tasklets | none: removed (D-112) | — |
| `mtl_hp_*`, `mtl_dma_*`, `mtl_udma_*` | `mtl_mem.h` regions; user DMA removed (D-112) | MS2b (video); every other essence with its binding (MS4a, MS4b) |

**ST 2110-20 use cases** ([coverage.md](coverage.md)):

| Use case today | Unified home | When |
|---|---|---|
| pipeline loops `st20p_tx_get_frame`/`put_frame`, `st20p_rx_get_frame`/`put_frame` (U-180, U-200) | `mtl_tx_acquire` + `mtl_tx_submit`; `mtl_rx_dequeue` + `mtl_rx_release` | MS1 |
| session pull model `get_next_frame`, push `notify_frame_ready` + `st20_rx_put_framebuff` (U-181, U-201) | push: acquire and submit; dequeue and release from any thread, in any order | MS1 |
| TX from the caller's memory without a slot copy beforehand | `MTL_SUBMIT_SRC_PLANES` (a copy or the conversion into the slot during submit; any session but `MTL_SESSION_REQUIRE_DIRECT`) | MS1 |
| framebuffers by index (U-182, U-183, U-224) | `info.unit_bytes`, `pool_count`; `mtl_session_get_slot` (`mtl_mem.h`) | MS1; by index MS2b |
| ext frames: session per index, pipeline per frame, two-phase release, dedicated and dynamic RX (U-184…U-186, U-208, U-209) | `MTL_SESSION_POOL_ATTACHED`, `mtl_session_attach`, `mtl_tx_acquire_slot`, `MTL_SESSION_RX_BY_INDEX`; results as the release; `mtl_tx_acquire_layout`, `mtl_rx_provide` | MS2b |
| interlaced, field as unit (U-187) | `v.raster.scan = MTL_INTERLACED`; parity from the media index | MS1 (1080i ports) |
| linesize and padding (U-188) | `v.linesize` of library pools | MS2b (wave 2c) |
| split-forward of RX tiles (U-189) | `mtl_session_get_pool_region`, `mtl_session_attach`, `unit.hold`, `mtl_tx_send_slot` (ex09) | MS2b |
| user meta (U-190, U-221) | an `MTL_META_USER` record in the meta area (`mtl_meta_put`, `mtl_meta_find`) | MS1 |
| packing, transport formats incl. the non-RFC 4175 ones (U-192…U-194) | `v.packing`; `v.format`, `MTL_YUV422P10LE_NONSTD`, `MTL_V210_NONSTD` (`MTL_INFO_NON_COMPLIANT`) | MS1 (port `transport_yuv422p10le`) |
| library-paced TX on the epoch, epoch RTP (U-150, U-154) | `MTL_MEDIA_AUTO`, the default | MS1 |
| legacy default RTP from the TX cursor (U-155) | not the default (D-10) | MS1; compared in the nightly (MS2a, G-99) |
| RTP delta and TR offset (U-156) | `tx.rtp_trim_ns` (the RTP trimmed, the launch fixed); `MTL_OPT_TROFFSET_NS` | TR offset default and 0 MS1, other values MS2a; `tx.rtp_trim_ns` MS3 |
| user pacing, exact user pacing, user RTP (U-151…U-153) | `MTL_MEDIA_TAI` + `MTL_SUBMIT_NOT_BEFORE`; `MTL_SUBMIT_EXACT` + `unit.launch_tai_ns`; `MTL_SUBMIT_RTP_TS` | TAI and NOT_BEFORE MS1; EXACT B3 (else MS2a); RTP_TS and a launch in another slot than the media time MS3 (E1) |
| drop when late, late notification (U-157, U-158) | `MTL_OPT_LATE_POLICY`; results with status, reason and margin | MS1; margins MS3 (E3) |
| VSYNC (U-159) | `MTL_OPT_EPOCH_TICK`, `MTL_EVENT_EPOCH_TICK` | MS3 |
| sender type, RL tuning knobs (U-160, U-162) | `v.sender_type`; `MTL_OPT_VIDEO_START_VRX`, `_PAD_INTERVAL`, `_STATIC_PAD_P`, `_DISABLE_BULK` | MS1 |
| TR offset, TRS, VRX, scheduler index (U-161) | `mtl_session_get_info` | MS1 |
| next frame due, frame late (U-163) | `mtl_tx_next_slot`, `mtl_epoch_index_at` | MS3 |
| incomplete frames, frame status (U-202, U-203) | delivered by default (`rx.incomplete = MTL_RX_DELIVER`), `unit.status`; `MTL_OPT_RX_INCOMPLETE = MTL_RX_DISCARD` | MS1 |
| packets per leg, arrival times, bytes per frame (U-204, U-205, U-207) | `mtl_rx_get_detail` | A2b (else MS2a); the `frame_meta_*` ports in MS2a read them |
| RX RTP and media time (U-206) | `unit.rtp`, `unit.media_tai_ns`, `unit.media_index` | MS1; `media_index` MS3 |
| RX DMA offload (U-211) | `MTL_OPT_DMA`, `MTL_OPT_DMA_DEVICES`, `MTL_PATH_DIRECT_DMA` | B3 (else MS2a) |
| auto-detect (U-212) | `v.detect = MTL_DETECT_ON`, `MTL_EVENT_RX_FORMAT` | MS3 |
| timing parser in stats and per frame (U-214…U-216) | `MTL_OPT_RX_TIMING_PARSER`, keys `tp.*`, `mtl_rx_get_detail` (`timing[]`) | B3 (else MS2a) for the stats; per unit (`timing[]`) MS2a with RX detail |
| two RX threads (U-217) | `MTL_OPT_RX_THREADS`; today's engine rejects 2 with two legs or rows units | B3 (else MS2a) |
| per-packet conversion in st20p (U-220) | `MTL_OPT_RX_CONVERT_PER_PACKET`, on the RX tasklet (library code: the one exception to "no conversion on a tasklet") | B3 (else MS2a; port `digest_1080p_packet_convert_s2`) |
| `uframe_pg_callback`, header split (U-219, U-213) | none: removed (D-112) | — |
| simulated RX loss (U-223) | `mtl_debug_inject(MTL_FAULT_DROP_PKTS)`, `MTL_FAULT_DROP_RANDOM` | MS1 (`DROP_PKTS`); `DROP_RANDOM` MS2a |
| pcapng dump | `mtl_session_capture` | MS4 |

**Easily forgotten modes** ([legacy-internals.md](legacy-internals.md)):

| # | Mode | Unified home | When |
|---|---|---|---|
| 1 | slice-level TX and RX | `MTL_UNIT_ROWS`, `mtl_tx_row_deadline`, `mtl_rx_wait_rows`, `MTL_OPT_RX_ROWS_STEP`, `MTL_OPT_ROWS_LATE` | MS2a |
| 2 | RTP-level app-built packets, the only route to ST 2022-6 | `MTL_UNIT_PACKETS`, `MTL_RTP` | MS5 |
| 3 | RX per-pixel-group callback on the tasklet | none: removed (D-112); per-packet conversion covers the conversion use | — |
| 4 | st20p per-packet conversion (three output formats, DMA off) | `MTL_OPT_RX_CONVERT_PER_PACKET` | B3 (else MS2a) |
| 5 | derive pipelines (no conversion or codec) | `v.app_format = 0`; cvideo codec bypass | MS1; cvideo MS4b |
| 6 | ext frames in both TX flavours, two-phase release | attached pools, `mtl_tx_acquire_slot`, results; `mtl_tx_acquire_layout` | MS2b |
| 7 | split-forward (4K → 4 × 1080p tiles) | pool region + attach + `unit.hold` (ex09) | MS2b |
| 8 | RX dedicated and dynamic ext frames | `MTL_SESSION_POOL_ATTACHED`, `MTL_SESSION_RX_BY_INDEX`; `mtl_rx_provide` | MS2b |
| 9 | GPU VRAM RX frames | `mtl_mem_open` with `MTL_MEM_DEVICE` (`-MTL_ENOTSUP` until then) | MS6 |
| 10 | `DATA_PATH_ONLY`, queue meta (suspected NULL dereference, SP-02) | none: removed (D-112) | — |
| 11 | exact user pacing, user pacing, normal | `MTL_SUBMIT_EXACT`, `MTL_MEDIA_TAI` + `MTL_SUBMIT_NOT_BEFORE`, `MTL_MEDIA_AUTO` | video: user pacing and normal MS1, exact B3 (else MS2a); ST40 MS6 |
| 12 | epoch RTP and `rtp_timestamp_delta_us` | the default; `tx.rtp_trim_ns` | MS1; `tx.rtp_trim_ns` MS3 |
| 13 | `DISABLE_BULK`, `TSC_NARROW`, `STATIC_PAD_P`, `start_vrx`, `pad_interval` | `MTL_OPT_VIDEO_*`, `MTL_OPT_PACING` | MS1 |
| 14 | two RX threads above 40 Gb/s | `MTL_OPT_RX_THREADS` | B3 (else MS2a) |
| 15 | header split | none: removed (D-112) | — |
| 16 | field-as-frame interlace | `MTL_INTERLACED` units | MS1; packet units MS5 |
| 17 | ST40 split by packet, interlace auto-detect | `MTL_OPT_ANC_SPLIT_BY_PACKET`, `sc.anc.detect` | MS4a |
| 18 | ST30 build pacing, FIFO, RL warm-up | `MTL_OPT_AUDIO_BUILD_PACING`, `MTL_OPT_AUDIO_FIFO_MS`, `MTL_OPT_AUDIO_RL_ACCURACY_NS`, `MTL_OPT_AUDIO_RL_OFFSET_NS` | MS4a |
| 19 | st22p codec threads | `MTL_OPT_CVIDEO_THREADS` | MS4b |
| 20 | callback-driven vs polled completion | one push model: results, events, the wait handle; no app code on tasklets (D-04) | MS1 (results, wait handle); MS3 (events) |
| 21 | non-RFC 4175 transport and `CUSTOM8` formats | `MTL_*_NONSTD`, `MTL_APP_YUV422_CUSTOM8` | MS1 |
| 22 | debug knobs in public ops (`SIMULATE_PKT_LOSS`, `st40_tx_test_config`) | `mtl_debug_inject` (`DROP_PKTS`, `DROP_RANDOM`, `TX_MUTATE`) | MS1 entry point; faults per milestone (§8.3) |

**Backends** ([legacy-internals.md](legacy-internals.md), [engine.md](engine.md) §2.7;
`caps.backend`, `enum mtl_backend` in `mtl_observe.h`). The core and the video bindings sit on
the video engine, which runs on every backend, so no binding is backend-specific; the table says
which backend a unified job exercises.

| Backend | Today | Unified jobs |
|---|---|---|
| DPDK PMD (PCI BDF), VF and PF | RL (ice PF, iavf VF), TSC, TSC_NARROW, TSN (E830 PF), PTP; zero copy if the NIC has multi-seg; flow director or shared RSS; queue-hang recovery only here | MS1 through the bridge: `UnifiedSt20p` and the pytest `st20p` smoke set; MS2a: the direct open, the other ST20 ports and the nightly |
| kernel socket (`kernel:`) | TSC only, single segment, no flow steering, still needs hugepages | MS1: `UnifiedSt20p` on `kernel:lo` (§5.3); MS2a: the nightly `unified_st20p_kernel_loopback` |
| native AF_XDP (`native_af_xdp:`) | RL through `tx_maxrate` on ice, else TSC; copies every segment into UMEM | direct open from MS2a; no unified job in MS1–MS2b; the pytest `xdp` group stays legacy until the matrix gets `api: unified` beyond `st20p` |
| DPDK AF_XDP, DPDK AF_PACKET | experimental | removed (D-112, U-095) |
| null (`null:<n>`) | none | new in MS1: the U tier |
| Windows (NetUIO) | no MtlManager, no kernel socket, no AF_XDP | a compile-only CI job for the headers (G-74) |

**The rest of the inventory, by area** ([coverage.md](coverage.md)):

| Area | ST20 milestones | Later |
|---|---|---|
| instance lifecycle and init parameters | `mtl_instance_open` over `mtl_init` for `null:<n>` and `kernel:`, options the ST20 consumers set, the bridge (MS1); PCI BDFs and `native_af_xdp:` (MS2a) | runtime `mtl_port_open` (NG8; no header symbol; after MS7, on request) |
| time source and PTP | `time_source` at open, AUTO once (`CLOCK_TAI` if valid, else `SYSTEM_TAI`); built-in PTP as today through the bridge (MS1), on a direct open (MS2a) (§2.4) | time events (MS3); a lock state that clears, clock steps, PHC and FREERUN with E9 (MS6); `mtl_time_set_reference` (MS2a); AUTO re-evaluation (Phase 7) |
| schedulers, lcores, threads | `MTL_INSTANCE_TASKLET_THREAD`, `MTL_INSTANCE_TASKLET_SLEEP`, scheduler options (MS1) | lcore borrowing, user tasklets: removed (D-112) |
| port tuning, ports and backends | the port options RxTxApp and KahawaiTest set (MS1); `mtl_port_get_spec` (A2b, else MS2a) | capacity queries (MS5) |
| memory, DMA, zero copy | library pools (MS1); RX DMA through `MTL_OPT_DMA` (B3, else MS2a) | `mtl_mem.h` (MS2b for video) |
| session plumbing | flows and two legs, MAC, NUMA, `sc.ssrc` and `sc.payload_type` (one identity for both legs), `mtl_flow.dscp`, source filter (MS1) | `mtl_session_update` (MS5) |
| TX timing | video: AUTO, TAI with `NOT_BEFORE` (MS1); `EXACT` (B3, else MS2a); `RTP_TS`, INDEX, start at a TAI or an index, `mtl_tx_next_slot` (MS3) | other essences (MS4a, MS6) |
| completion, notification, blocking | results, the wait handle (MS1); events on sessions and on the instance (MS3) | shared queues: not in v1 (`MTL_LATER`) |
| ST 2110-22, -30, -40, -41 | — | -30, -40, -41 MS4a; -22 MS4b; start arrays and sample-accurate audio MS6 |
| RTP passthrough (common to every essence) | — | MS5 |
| conversion, formats, helpers | st20p conversion by `v.app_format`, `MTL_SUBMIT_SRC_PLANES` (MS1); converter plugins (MS2b) | `mtl_convert` (MS4b) |
| plugins | — | plugin ABI v2 (MS4b) |
| stats and observability | ST20 counters (A2b, else MS2a); the stats registry's per-scheduler blocks (MS3) | the `st20_rx_user_stats` fields without a key (U-402) |
| experimental and legacy remnants | — | removed (D-112) |

### 2.4 Time sources

- A bridged instance (RxTxApp, KahawaiTest) keeps the legacy time base: `MTL_FLAG_PTP_ENABLE`,
  `ptp_get_time_fn`, or the default. Unified sessions on it read that time.
- `mtl_instance_open` takes `time_source`. In MS1 `MTL_TIME_SOURCE_AUTO` chooses once, at open:
  `CLOCK_TAI` if it is valid, else `SYSTEM_TAI` (a vDSO read). A NIC PHC (one syscall per read)
  joins AUTO in MS6 with the published time base (E9), and so does FREERUN; re-evaluation while
  running is Phase 7.
- `MTL_TIME_SOURCE_PTP_BUILTIN` on a VF runs as today: the port has no timesync, so the client
  disciplines MTL's own software time base (`ptp->no_timesync`, `mt_ptp.c:1390-1393`) and never the
  VF's PHC. `MTL_REASON_CLOCK_NOT_OWNED` is only for a request to steer a PHC MTL does not own
  (OI-7). On a direct open it needs the PCI open of MS2a; in MS1 it is reached through the bridge.
- So RxTxApp `--ptp`, the NoCtx PTP cases and the nightly-pytest `ptp` group
  (`.github/workflows/nightly-pytest.yml:40`, `tests/acceptance/tests/single/ptp/`) keep passing,
  before and after the RxTxApp default switches to the unified path (MS2a, §6.9). The legacy gate
  runs the `tests/unit/ptp/` suite, which pins the software time base (§8.5).
- `ptp_get_time_fn` maps to `MTL_TIME_SOURCE_USER` + `mtl_time_set_reference` with `MTL_TIMEREF_USER_PAIR` ([migration.md](migration.md)).

## 3. Prerequisites

### 3.1 Decisions

The implementation starts from these rules ([decisions.md](decisions.md) carries their
rationale):

- **One core:** the core with bindings under every API; the legacy `st*p_*` become wrappers on it
  (D-99).
- **One library:** libmtl carries the unified API, with `lib/src/unified/` compiled in, in the
  version node `MTL_UNIFIED_EXPERIMENTAL_0_2` (the headers' revision 0.2); the version script, the
  soname and the `MTL_LEGACY` node are in [migration.md](migration.md) §7.2 (D-23, D-108).
- **Deferred wake (W2):** a completing context never makes a syscall: `mt_wake()` sets the
  session's bit in a per-scheduler pending bitmap (with a summary word on its own cache line), and
  the scheduler loop (`sch_tasklet_func`, after the handler loop) writes the eventfd of each flagged
  session once per iteration; an application thread that completes writes directly. W3 (a waker
  thread draining the same bitmap) is built only if spike S1 shows that the deferred writes harm
  pacing (D-68, D-102).
- **Export what is implemented:** a function is exported in the milestone that implements it, and
  each function's comment ends with its milestone tag, for example `(MS1)`. The installed header
  declares the whole design (all that is not under `MTL_LATER`), so a call to a function not yet
  exported fails at link time, naming the symbol; experimental headers carry no promise before
  MS7. A later value of an exported call returns `-MTL_ENOTSUP` with `MTL_REASON_NOT_IMPLEMENTED`.
  No stub generator and no header filter (D-106).
- **Tooling first:** task P0 (§5.2) is the first change of MS1.
- **Git authority:** the implementing session commits each task after its gates pass, signed off
  through the `mtl-commit` skill and without AI attribution, on a work branch; it never pushes and
  never opens a pull request; the maintainer reviews and pushes ([ms1-status.md](ms1-status.md) §0).

Two open items of this plan are settled as MS1 rules ([decisions.md](decisions.md), OI-59 and
OI-60):

- **OI-59, frame memory under the video bindings.** TX library pools use the engine's own
  framebuffers in MS1; from MS2b every TX slot is an ext frame that carries its page table (PA mode
  builds page tables only for engine-allocated frames, `st_tx_video_session.c:245-266`), so library
  and attached pools are one code path. RX uses `query_ext_frame` with core-allocated slots from
  MS1, so RX has one path from the start.
- **OI-60, where conversion runs in MS1.** In the caller (submit converts, dequeue converts) for
  the formats st20p converts internally today; with `MTL_SUBMIT_SRC_PLANES` submit reads the
  caller's planes during the call. Converter plugins join in MS2b with the transform claim and done
  (D-100).

### 3.2 Code that must land first

- **PR #1770** (open, 38 commits): of its rows MS1 needs SF-05/MF1 (an ext buffer is reported done
  before its `refcnt` is decremented, `tv_frame_free_cb`, `st_tx_video_session.c:134-135`), SF-08,
  SF-09 and SF-23. If it is not merged by the end of week 1, the MF1 hunk is taken into task E1. The
  PR also fixes the video `update_destination` that writes the source UDP port into the destination
  port (`st_tx_video_session.c:3829-3830`).
- **PR #1610's legacy st20p parity tests**, folded into main's harness
  (`tests/unit/pipeline/st20p_tx_harness.c`, `st20p_harness.c`) before any engine change of MS1, so
  they pin today's behaviour (task T1). Its eventfd and poll loop in `mt_session_event.c` and
  `mt_session_video_common.c` seed `mt_wake` and `mtl_wait`, with credit (D-27); its event ring
  does not. What else of the PR each task can take, and what not, is in
  [legacy-internals.md](legacy-internals.md) §12.1 and §12.2.

### 3.3 Spikes

| Spike | Question | Needed by |
|---|---|---|
| S0 baseline | tasklet iteration avg and max per scheduler (`MTL_FLAG_TASKLET_TIME_MEASURE`, printed by `mt_sch.c:466-468`; RxTxApp `--tasklet_time`), RL and TSC, at the ST20 loads of the test pool | MS1 week 1 (the §8.4 budgets); E1 before it merges |
| S1 waker | the cost to pacing of the deferred eventfd writes in the scheduler loop at 100 armed sessions, and their wake latency; the latency of W3 if it has to be built | MS2a (the W3 contingency check) |
| S6 idle cleanup | rate and cost of `rte_eth_tx_done_cleanup` on an idle session | MS1 task E1 |
| S8 queue stop and start | do iavf and ice release chained external mbufs on `rte_eth_dev_tx_queue_stop`/`start` | MS3 (the stalled-queue close, attached memory after recovery) |

## 4. Where the code goes

| Item | Path | Notes |
|---|---|---|
| public headers | `include/mtl/experimental/*.h`, installed to `${includedir}/mtl/experimental/` | moved from the sketch in task H1a, one copy holding the full design, each function tagged with its milestone; the header declares the whole design, and a function not yet exported fails at link time (D-106); `MTL_LATER` blocks are not installed |
| the core | `lib/src/st2110/core/` (`st_core.h`, `st_core_slot.c`, `st_core_session.c`, `st_core_wait.c`, `st_core_handle.c`, `st_core_xform.c`; names indicative) | in libmtl, because the legacy wrappers use it too (MS2b); `st_core.h` is task C0 |
| engine accessor header | `lib/src/st2110/st_engine_core.h` | every engine entry point a binding uses besides the callbacks ([engine.md](engine.md) §3) |
| bindings | `lib/src/st2110/core/bind_{video_tx,video_rx,null}.c` in MS1; the other essences and `packet` later | each implements one engine's `ops` callbacks |
| API shell | `lib/src/unified/`, compiled into libmtl; the option table `lib/src/unified/mtl_options.def` (an X-macro: key → ops field or flag, type, range) | exported in the node `MTL_UNIFIED_EXPERIMENTAL_<rev>`; pkg-config stays `mtl` (D-23) |
| libmtl ABI | `lib/meson.build:151` has no soname or version script today | H1b: a version script with only the unified node, every other symbol exported as today; MS3: the soname, `MTL_LEGACY` and `local: *` after an `nm` audit; MS7 deprecates the legacy symbols, which leave `MTL_LEGACY` from F+2 ([migration.md](migration.md) §7.2; D-83, D-108) |
| meson | `enable_unified` (default **true**, so CI catches breakage; there is no ABI promise before the freeze), `enable_debug_api` (default false; `./build.sh unit` sets it true) | in `meson_options.txt` |
| unit tests | U: `tests/unit/unified/` → `UnifiedUnitTest`, which links libmtl and runs on `null:1`. UB: they `#include` production `.c` files, so they build into `UnitTest` (`mtl_internal_dep`) next to the harness they use (`tests/unit/session/`, `tests/unit/pipeline/`) | built and run by `./build.sh unit` |
| integration tests | `tests/integration_tests/unified/` (`UnifiedSt20p`) | part of `KahawaiTest` |
| RxTxApp | `tests/tools/RxTxApp/src/unified/` | `--api legacy\|unified`, build option `default_api`, set by `build.sh` from `MTL_RXTXAPP_DEFAULT_API` (D-110) |
| doc test | `doc/unified-api/sketch/check.sh` reads the headers from `include/` after task H1a | in CI through `.github/path_filters.yml`; it prints the function counts per header, per call class and per milestone tag, which no document repeats |
| status ledger | `doc/unified-api/ms1-status.md` | §5.8 |

## 5. MS1: ST 2110-20 frames in a month

### 5.1 Scope

**In:**

- **Headers and build:** the headers in `include/mtl/experimental/`; libmtl exports the MS1
  functions in `MTL_UNIFIED_EXPERIMENTAL_<rev>` and nothing else of the unified API; later values of
  those calls return `-MTL_ENOTSUP` with `MTL_REASON_NOT_IMPLEMENTED`; at the exit the exports equal
  the functions tagged MS1, after any stretch retags (G-51).
- **Core:** handles (process-wide, never freed, generations, R4); `mtl_last_error` (written only
  by failing calls), reasons, `MTL_INIT` and `struct_size` (R3); the slot table (one 64-bit word per
  slot), the descriptor ring, the result reservation; stop against submit (exactly one wins); the
  nine-state table (ARMED only through a start `when`, which MS1 rejects); the armed wait with the
  deferred wake, one eventfd per session; sticky interrupts with a wait-target mask; deferred,
  idempotent close.
- **Instance:** `mtl_instance_open` over `mtl_init` for `kernel:` and `null:<n>` (a null-only
  instance does not call `mtl_init`: it initialises EAL itself with
  `--no-huge --no-pci --in-memory`, or accepts an initialised EAL, and runs a library thread as its
  loop); `MTL_PORTS`, `MTL_INSTANCE_TASKLET_THREAD`/`_SLEEP`, time source AUTO; the bridge
  `mtl_instance_from_legacy`, through which PCI BDFs and `native_af_xdp:` reach unified sessions;
  `mtl_close` on an instance (0, 1 or `-MTL_EIO`); re-open after close on `null:`.
- **Session:** `create`, `query`, `get_info` (with `wire_bps`), `start` (n = 1, `when` NULL or
  NOW), `stop` (DRAIN, FLUSH, arrays), `close` (1 while retiring, 0 once retired; a repeated close
  polls), `get_status` (state, `blocked_on`, legs).
- **Data path:** `mtl_tx_acquire`, `mtl_tx_submit` (with `MTL_SUBMIT_SRC_PLANES`), `mtl_release`,
  `mtl_rx_dequeue`, `mtl_reap` with the typed `mtl_tx_reap` and `mtl_tx_reap_full`, `mtl_wait`,
  `mtl_get_wait_handle`; the `used` rules of video frame units; the meta area with its terminator,
  `mtl_meta_put`, `mtl_meta_find`.
- **Results:** `MTL_SESSION_RESULTS` with the records in `seq` order; ON_TIME (`MTL_TXR_RESLOTTED`
  when AUTO moved the unit), DROPPED (the slot decision's `DUPLICATE_SLOT`, `BEHIND`, `TOO_LATE`;
  the recovery verdict), FLUSHED; cookie verbatim; reservation at acquire. In copy mode (no chain:
  kernel socket, AF_XDP, NICs without multi-segment) a result means built, not sent, until MS2a.
- **Video:** `MTL_VIDEO`, `MTL_UNIT_FRAME`, TX and RX, one or two legs (ST 2022-7, one `sc.ssrc`
  and `sc.payload_type` for both), progressive and interlaced (a field per unit), library pools
  (OI-59), `app_format` conversions in the caller (OI-60), `MTL_META_USER`, packing, sender type,
  `mtl_flow.dscp` into the TX builders' TOS, the options RxTxApp and KahawaiTest set.
- **TX timing:** the video TX binding decides the slot at `get_next_frame` with exact math: AUTO
  takes the next feasible slot (a late unit is ON_TIME with `MTL_TXR_RESLOTTED`; interlaced fields
  alternate in submission order and a reslot skips whole frames), AUTO + `NOT_BEFORE` the first slot
  whose first-packet time is ≥ t, TAI the nearest slot (`DUPLICATE_SLOT`, `BEHIND`, `TOO_LATE` as
  DROPPED); it drives the engine with `USER_PACING | RTP_TIMESTAMP_EPOCH` and `required_tai = N·T`,
  so RTP comes from the frame epoch (D-10) with today's rounding. `notify_frame_late` never fires
  for core sessions. `RTP_TS`, and a TAI unit whose `NOT_BEFORE` or `EXACT` launch lands in another
  slot than its media time, are `-MTL_ENOTSUP` until E1 (MS3). The pick-up lead is max(RL warm-up
  lead, one bulk build time + the S0 scheduler iteration bound) + any conversion stage, about 0.5 ms
  with RL and about 20 µs with TSC ([engine.md](engine.md) §3). The default `min_tx_delay_ns` 0 is
  playback; a capture producer sets `min_tx_delay_ns` = one frame period + the pick-up lead and gets
  a slot delay of one frame.
- **RX:** incomplete units delivered with their status (the default
  `rx.incomplete = MTL_RX_DELIVER`; the bytes of lost ranges are unspecified until zero fill of
  library pools lands in MS2a, and the result says so with `MTL_RX_INCOMPLETE`),
  `MTL_OPT_RX_INCOMPLETE = MTL_RX_DISCARD`; delivery in the completer's `pub_seq` order; two legs
  with the out-of-order window of `rx.skew_budget_ns`.
- **Observe:** the D-110 log lines; `mtl_stat_*`, `mtl_rx_get_detail`, `mtl_port_get_spec` and
  `mtl_instance_list_sessions` with stretch task A2b.
- **Options:** the option table: every key known, the ST20 ones implemented.
- **Test substrate:** the null binding with null loopback (an RX session receives the TX units
  whose destination IP and port it matches), the test clock (completions run synchronously inside
  `MTL_FAULT_CLOCK_ADVANCE`), `mtl_debug_inject` (`FORCE_ERROR`, `DROP_PKTS`) with
  `-Denable_debug_api=true`.

**Stretch:** A2b (stats, RX detail, port spec, session list), B3 (RX DMA, two RX threads, the
RX timing parser, per-packet conversion, `MTL_SUBMIT_EXACT`), X (E2, exact `floor` RTP). What
does not land moves to MS2a (A2b, B3) or MS3 (X, with E2).

**Out:** the non-goals of §1.3.

### 5.2 Tasks

Each task is one commit on the work branch (§5.8), ≤ 1.5 k changed lines with its tests
(mechanical moves exempt), owned by `mtl-developer` for Gates 0–4 (P0 and S0: the main session), reviewed by `mtl-reviewer`
(Gate 5), and run on VFs by `mtl-system-admin` where marked (Gate 6). Sizes are estimates [I].

| # | Task | Depends on | Size | Gate 6 | Exit |
|---|---|---|---|---|---|
| P0 | tooling, owned by the main session (detail below): `run_gtest` gains `pacing_way`, `kernel:lo` and whitelisted arguments; the agent definitions, skills and instructions follow | — | 200 | `run_gtest(gtest_filter='St20p*', pacing_way='tsc')`, `run_gtest(p_port='kernel:lo', r_port='kernel:lo', gtest_filter='St20p*')` | both runs green; each config diff approved; the session restarted |
| S0 | baseline measurement, owned by the main session (detail below): RxTxApp `--tasklet_time`, RL and TSC | — | — | — | the SCH avg and max lines per pacing class in ms1-status §4; the §8.4 MS1 budgets confirmed, or a revision asked for as a D-row |
| T1 | PR #1610's legacy st20p parity tests on main's harness: the 9 TX cases of `a693810c` and the 4 RX cases of `f23158c1` (legacy-internals.md §12.1) | — | 600 test | — | they pin today's behaviour and pass; Gate 2: each test fails with the line it pins reverted locally |
| H1a | the headers to `include/mtl/experimental/`, moved verbatim (`./format-coding.sh` leaves them unchanged), with every path that names them (detail below); the G-73 field lint; a CI job, path filters | P0 | 150 new, 3.6 k moved | — | `check.sh` green over `include/` locally; `actionlint` and `.github/scripts/ci/check-path-filters.py` pass; CI is confirmed on the maintainer's pull request |
| H1b | the API shell inside libmtl, node `MTL_UNIFIED_EXPERIMENTAL_0_2`; `mtl_last_error`; `mtl_options.def` and the reasons table, each checked against its header by a U test; the `nm` check; `UnifiedUnitTest`; the build wiring (detail below) | H1a | 700 + 200 | — | the node exports exactly the five functions below; `./build.sh unit` runs `UnifiedUnitTest` |
| C0 | the core's internal header `st_core.h`: the slot word, the descriptor ring, the armed words, the binding ops, the instance-context interface | — | 400 | — | a TU including `st_core.h` builds with `ninja -C build`; checkpoint 1: the maintainer's approval on the answer line of ms1-status §3; the commit waits for it |
| C1a | handles, the state table, interrupts, deferred and idempotent close, with U tests on a test binding | C0, H1b | 900 + 600 | — | U tests against `st_core.h` on a test binding for G-07, G-29, G-30, G-49, G-64, G-65, G-71 (the core part); the public-API forms of G-49 and G-71 complete in A2a, the `null:1` and `FORCE_ERROR` forms in C2 |
| C1b | the slot table, the descriptor ring, results, the reservation, the deferred wake with its wake-flush call in `mt_sch.c` | C0, C1a | 1.0 k + 600 | yes (legacy gate) | U tests against `st_core.h` on a test binding for G-01, G-02, G-04, G-05, G-09, G-52, G-72, G-95 (the core part; G-01's public-API form in A2a); the descriptor-ring micro-benchmark within the §8.4 DP-call budget |
| A1 | the instance: the null-only EAL rule, null port parsing (`null:1`), the bridge from legacy, errors and reasons, options; OI-2, OI-7, OI-49 | H1b, C1a | 900 + 500 | — | U: G-33, G-70 (the codes), G-73, G-77 (MS1 part: a second non-SHARED open is `-MTL_EEXIST`, `INSTANCE_MISMATCH`), G-111 (AS calls), G-112 |
| C2 | the null binding with null loopback, the test clock with synchronous completions, `DROP_PKTS`, `FORCE_ERROR` | C1b, A1 | 550 + 300 | — | U: G-92, G-93, `Rx.loopback_frame`, `Fault.force_error`; the `null:1` and `FORCE_ERROR` forms of G-49 |
| E1 | engine fixes of MS1 (§7): MF1 (unless PR #1770 landed), MF7, the TX recovery verdict, idle cleanup only in `WAIT_FRAME`, incomplete delivery always on, SF-45, SF-49, st30p and st40p user timestamps, OI-3; `st_engine_core.h` | T1; S0's data before its commit | 500 + UB | yes | the legacy gate on a VF, TSC and RL; UB for MF1, MF7, the verdict |
| B1 | the video TX binding: frames, one and two legs, interlace, the slot decision (§5.1), DSCP, in-caller conversion, `MTL_SUBMIT_SRC_PLANES`, the log lines (D-110) | C1b, E1 | 900 + 400 UB | legacy `St20*` (the builders' TOS) | UB through a harness like `tests/unit/session/st20_tx_harness.c`: G-08, G-20, `submit_carries_meta`, `slot_decision`, the log lines byte-identical |
| B2 | the video RX binding: `query_ext_frame` slots, the incomplete policy, one and two legs, the log lines | C1b, E1 | 700 + 300 UB | — | UB: `rx_hold_release`, `rx_order_pub_seq`, `rx_incomplete_status`, two legs |
| A2a | the API shell, part 2: session calls, data path, reap, wait, `get_info`, `get_status` | A1, C2, B1, B2 | 1.1 k + 700 | — | U: G-31, G-46, G-47, G-88, G-103, G-107, G-70 (dequeue), the public-API forms of G-01, G-49 and G-71; ex01, ex02, ex03 and ex05 run on `null:1` |
| CI1 | CI: `gtest.sh` baseline entries `UnifiedSt20p*` and a `kernel:lo` case; a debug + ASan unit job; `MTL_RXTXAPP_DEFAULT_API` → `-Ddefault_api`, with `--reconfigure` | H1b; I1 for the gtest entries | 200 | — | the jobs run on a pull request of the maintainer's |
| I1 | `UnifiedSt20p`: the fixture wraps the global instance (`tests.hpp:97`, after `tests.cpp:805`), the ported cases and the cross cases of §5.3, `kernel:lo`; written in week 2, committed after A2a | A2a | 1.3 k | yes | green next to legacy `St20p*` in one run, default pacing and `--pacing_way tsc` |
| R1 | RxTxApp `src/unified/` for `st20p` TX and RX, `--api`, `default_api`, SIGTERM → `mtl_interrupt`; written in weeks 2–3, committed after A2a | A2a | 1.0 k | — | local loopback JSON runs with `--api unified`; output lines identical |
| P1 | acceptance: the smoke set (§5.5) on a unified-default build and on a legacy build | R1, CI1 | — | main session runs pytest | the same pass list on both |
| A2b | stretch: stats, `mtl_rx_get_detail`, `mtl_port_get_spec`, `mtl_instance_list_sessions`; A2b retags these six functions (MS1) in the header: `mtl_stat_list`, `mtl_stat_find`, `mtl_stat_read`, `mtl_rx_get_detail`, `mtl_port_get_spec`, `mtl_instance_list_sessions` | A2a | 600 + 300 | — | U: G-42, the G-88 listing; RxTxApp's stats lines |
| B3 | stretch: RX DMA, two RX threads, the RX timing parser, per-packet conversion, `MTL_SUBMIT_EXACT` | B1, B2 | 800 + 300 UB | yes | G-26 (the binding's checks); `digest_1080p_packet_convert_s2` ported |
| X | stretch: E2, exact `floor` RTP, for core sessions only | — | 400 | yes | the oracle unit tests; legacy rounding tests unchanged |

**P0, S0, T1, H1a and H1b in detail.**

- **P0** (the main session, not `mtl-developer`). Each config diff (agent definitions, the MCP
  server, skills, instructions) needs the maintainer's approval before it is committed, and the
  Claude Code session restarts after P0 so the new MCP parameters and agent prompts load. The parts:
  - `.github/claude/agents/*.md` together with their Copilot mirrors `.github/agents/*.agent.md`,
    and `.github/instructions/mtl-system-setup.instructions.md` (the tool inventory);
  - `run_gtest` in `.github/mcp/mtl_mcp_server.py`: `pacing_way`; the whitelisted extra arguments
    `--pacing_way`, `--level`, `--rss_mode`, `--iova_mode`, `--multi_src_port`, `--p_sip`;
    `kernel:lo` ports, which need `--p_sip` (`.github/scripts/gtest.sh:376`) and a `kernel:<if>`
    branch in `_validate_bdf`;
  - `mtl-developer` may edit `tests/tools/RxTxApp/`, `.github/`, `build.sh`, `meson_options.txt`,
    `doc/unified-api/`;
  - the `mtl-build` skill item of [legacy-internals.md](legacy-internals.md) §12.1;
  - a KB routing row for the reviewer: `lib/src/st2110/core/`, `lib/src/unified/` and
    `include/mtl/experimental/` → engine.md §1–§3, §5, §7 and contract.md §1, in
    `.github/instructions/mtl-kb-routing.instructions.md` or wherever the reviewer's Gate C reads
    routing;
  - reviewer rules for MS1 core and binding code: RX gaps are unspecified until MS2a (§5.1), so
    "RX frame buffers must be zero-initialized" does not apply to them yet; per-milestone exports
    and `-MTL_ENOTSUP` for later values are required, not dead code; the prefixes `st_core_` and
    `bind_`, and `mtl_` only for exported symbols;
  - the stale claims of the repository `CLAUDE.md`: `.clang-format` is a real file, not a link to
    `.github/linters/`, and pre-commit pins mirrors-clang-format v22.1.8, not clang-format-14; the
    same claim is in `.github/skills/mtl-build/SKILL.md:30` and
    `.github/instructions/mtl-c-coding.instructions.md:97`.
- **S0** (the main session). A release `./build.sh` (not `debugonly`), then the loop config, once
  per pacing class (`auto`, then `tsc`):
  `sudo RxTxApp --config_file tests/tools/RxTxApp/script/loop_json/st20p_1v_1080p59.json --tasklet_time --pacing_way auto --test_time 600`.
  The SCH avg and max lines go to ms1-status §4. A budget revision is a D-row, so ask first.
- **T1.** If `pr1610` is missing: `git fetch origin pull/1610/head:pr1610`; the line numbers are
  pinned to `14a1f80c`, so if the PR has moved, `git branch -f pr1610 14a1f80c` (keep the fetched
  head under another name).
- **H1a.** The same commit updates every path that names the headers: `sketch/gen_api_doc.py`
  (`HDR` at `:18`, the links at `:77`), the `inc=` line of `check.sh`, the 29 `sketch/include`
  links of the documents, the README "Normative" row and sketch/README; and it removes
  `doc/unified-api/sketch/include`. The headers keep `/* clang-format off */`: formatting would
  move the milestone tags off the comment's last line (check.sh lint 5). There is no header
  filter: the installed header declares the whole design (§3.1, D-106). The G-73 field lint is a
  header lint in `check.sh`.
- **H1b.** It exports exactly the MS1 functions it implements: `mtl_last_error`,
  `mtl_reason_name`, `mtl_version_string`, `mtl_option_list`, `mtl_option_find`. The `nm` check
  compares the node with `lib/src/unified/exports.list`, which each later task extends (G-51: a
  subset of the MS1 tags until the exit); the legacy exported symbol set is unchanged (an
  `nm -D --defined-only` diff against the baseline). H1b also owns `enable_unified` in
  `meson_options.txt` and `--reconfigure` for `build_unit` at `build.sh:93`, so
  `-Denable_debug_api` applies to an existing build directory.

**What each task reads.** Give the agent of a task only these sections, plus the repository's
own rules it loads anyway (`CLAUDE.md`, `lib/CLAUDE.md` with the C rules, `tests/*/CLAUDE.md`).
Every task also reads §1 and §2 of this plan, the header it implements, its row above, and the
§8.2 rows of the G-ids in its exit. For C0, C1a and C1b there is no public header: the header is
`st_core.h` and the contract sections listed; for A1 it is the `---- Instance` section of
`mtl.h` and `mtl_legacy.h`. Section numbers are those of the documents in this
directory. Where PR #1610 has code for a task (P0, T1, C1a, C1b, B1, B2, A2a, R1, A2b, B3), the
agent also reads that task's row of [legacy-internals.md](legacy-internals.md) §12.1 and the rules
of §12.2, and opens the PR's code with `git show pr1610:<path>`: about 1.1 kLOC is worth adapting,
the rest is a checklist or a trap.

| Task | Design to read | Code to open first |
|---|---|---|
| P0 | §3.1, §5.8; README.md §1; legacy-internals.md §12.1 (row P0) | `.github/mcp/mtl_mcp_server.py` (`run_gtest`, `_validate_bdf`), `.github/scripts/gtest.sh:376`, `tests/integration_tests/tests.cpp:61-86`, `.github/claude/agents/mtl-developer.md`, `mtl-reviewer.md`, `mtl-system-admin.md`, `.github/agents/*.agent.md`, `.github/instructions/mtl-system-setup.instructions.md` |
| S0 | §3.3, §8.4; engine.md §11.1 | `tests/tools/RxTxApp/src/args.c` (`--tasklet_time`); `lib/src/mt_sch.c:454-470` |
| T1 | §3.2, §5.3; legacy-internals.md §12, §12.1 (row T1), §12.2 rule 9 | `tests/unit/pipeline/st20p_tx_harness.c`, `st20p_harness.c`; `git show pr1610:tests/unit/pipeline/st20p_tx_test.cpp` |
| H1a | §4; sketch/README.md; decisions.md D-104, D-106; §8.2 rows G-51, G-74 | `doc/unified-api/sketch/check.sh`, `sketch/gen_api_doc.py`, `include/meson.build`, `.github/path_filters.yml`, `.github/workflows/`, `.pre-commit-config.yaml`, `.github/scripts/ci/check-path-filters.py` |
| H1b | §4, §5.1; contract.md §1 (R1, R3, R7), §8, §12; migration.md §7.2; decisions.md D-23, D-106, D-108 | `lib/meson.build`, `meson_options.txt`, `build.sh` (`unit`), `tests/unit/meson.build`; `mtl_options.h`, `mtl_reasons.h` |
| C0 | engine.md §2.4, §3, §4 (the C0 bullet), §4.11, §5.1–§5.5, §7.1, §7.2; contract.md §4.1, §5.4, §6.2; decisions.md D-118, D-121, D-124, D-126 | `mtl.h`; `lib/src/mt_handle_guard.h` (the counter-pattern; not reused) |
| C1a | engine.md §5.7, §7.3, §9; contract.md §1 (R4, R6), §4, §7.4, §7.5 | the core header of C0; `lib/src/mt_handle_guard.h` (the counter-pattern; not reused) |
| C1b | engine.md §3.2, §4 (the C1b bullet), §5, §7.1, §7.2; contract.md §5, §6, §7.1–§7.3 | the core header of C0; `lib/src/mt_sch.c:155-242` |
| A1 | contract.md §1, §2, §8, §12; engine.md §4.10, §4.11; migration.md §4.2, §4.3, §6.2; decisions.md OI-2, OI-7, OI-49 | `mtl.h`, `mtl_options.h`, `mtl_reasons.h`, `mtl_legacy.h`; `lib/src/mt_main.c`, `lib/src/dev/mt_dev.c` (init), `lib/src/mt_util.c` (port-name parsing, `:964-967`) |
| C2 | engine.md §1 (null backend), §2.7; contract.md §2.1, §2.2; `mtl_debug.h` | the core header of C0; the null port parsing of A1 |
| E1 | engine.md §4, §10, §11, §12.1 (MF1, MF7, SF-45, SF-49); §7 of this plan | `st_tx_video_session.c`, `st_video_transmitter.c`, `st_rx_video_session.c`, `datapath/mt_queue.c`, `mt_sch.c`, `st30_pipeline_tx.c`, `st40_pipeline_tx.c` |
| B1 | §5.7; engine.md §3.1, §4.1–§4.3; contract.md §3, §5.2, §6, §9.9; timing.md §4.1, §4.2, §5, §6.1, §6.2; migration.md §4.4, §4.6; legacy-internals.md §12 | `st20_pipeline_tx.c` (the template to port, `tx_st20p_if_frame_late` at `:115-179`), `st_tx_video_session.c` (`get_next_frame` and `notify_frame_done` call sites), `tests/unit/session/st20_tx_harness.c` |
| B2 | §5.7; engine.md §3.1, §4.4, §4.5; contract.md §5.3, §6.5, §9.8; timing.md §11.5, §11.7; migration.md §4.5, §4.7 | `st20_pipeline_rx.c` (template), `st_rx_video_session.c` (`notify_frame_ready`, `query_ext_frame`) |
| A2a | contract.md §3.5, §4, §5, §6, §7 | the core header of C0; `examples/ex01_tx_video.c`, `ex02_rx_video.c` and the other examples of its exit |
| A2b | contract.md §11; engine.md §2.8; `mtl_observe.h` | `lib/src/mt_stat.c`; the stats calls of `tests/tools/RxTxApp/src/rx_st20p_app.c` |
| B3 | engine.md §4.5; contract.md §9.7, §9.8; timing.md §5.4, §6.1; migration.md §4.5, §4.7; legacy-internals.md §3.6 | `st_rx_video_session.c` (DMA, threads), `st_rx_timing_parser.c`, `st20_pipeline_rx.c` (per-packet conversion) |
| CI1 | §5.3, §6.9; `.github/instructions/mtl-gtest.instructions.md` | `.github/scripts/gtest.sh` (`generate_test_cases()`), `.github/workflows/`, `build.sh` |
| I1 | §5.3; `.github/instructions/mtl-gtest.instructions.md` | `tests/integration_tests/st20p_test.cpp`, `tests.hpp`, `tests.cpp` |
| R1 | §5.4; migration.md §4.4, §4.5, §11.1 | `tests/tools/RxTxApp/src/tx_st20p_app.c`, `rx_st20p_app.c`, `rxtx_app.c`, `args.c` |
| P1 | §5.5; `.github/instructions/mtl-acceptance-*.instructions.md` | `tests/acceptance/mtl_engine/rxtxapp.py` (`validate_results`) |
| X | timing.md §3.5, §4.2, §16; §8.6 of this plan | `tests/unit/session/st20_tx/rtp_timestamp_rounding_test.cpp` |

**Order and calendar** [I]:

| Week | Written | Committed and approved | Checkpoint |
|---|---|---|---|
| 1 (days 1–5) | P0 (day 1), S0, T1, H1a, H1b, C0 | P0, T1, H1a, H1b | day 5: C0 approved (checkpoint 1), H1a and H1b committed and approved |
| 2 (days 6–10) | C1a, C1b, A1, E1; I1 drafted | C0, C1a, C1b, A1 | day 10: C1a, C1b and A1 committed and approved |
| 3 (days 11–15) | C2, B1, B2, A2a, CI1; R1 drafted | E1, C2, B1, B2, A2a | day 15: first unified frame on a VF (I1's cross TX case) |
| 4 (days 16–20) | I1, R1, P1, fixes; A2b, B3, X if the gates allow | CI1, I1, R1 | day 17: feature freeze; the exit run (§5.6) on days 18–20 |

Critical path: P0 → H1a → H1b → A1 → A2a → R1 → P1, with C0 → C1a → C1b → B1/B2 → A2a beside
it.

**Gates.** Each is checked on its day by the main session against the ledger:

| Day | Gate | If missed |
|---|---|---|
| 5 | C0 approved; H1a and H1b committed and approved | stop core coding and resolve the blocker first |
| 10 | C1a, C1b and A1 committed and approved | cut B3, A2b and X; I1's non-cross cases move to MS2a |
| 15 | first unified frame on a VF (I1's cross TX case) | P1 shrinks to `kernel_lo_st20p` plus `test_fps` p29 |
| 17 | feature freeze | after it, only fixes for the exit criteria land |

### 5.3 The test pool

Four tiers. The U tier runs on every change on ordinary runners; the I and A tiers run on NIC
runners.

**U, `tests/unit/unified/` (`UnifiedUnitTest`) on `null:1` with the test clock:**

| Case | Proves |
|---|---|
| `Instance.open_close_reopen` | 200 cycles, no leak (ASan): G-112 |
| `Instance.close_timeout` | close returns within its timeout and only 0, 1 or `-MTL_EIO`, with sessions RUNNING and leases out: G-107 |
| `Api.null_handle_table` | every function with handle 0 gives `-MTL_EBADF`, every close 0: G-71 |
| `Api.struct_size_tail` | `MTL_INIT` defaults; a non-zero tail is `NONZERO_TAIL`: G-33, G-73 |
| `Api.error_codes`, `Api.not_implemented` | one meaning per code; a known but unbuilt value is `-MTL_ENOTSUP` with `NOT_IMPLEMENTED`, an unknown one `-MTL_EINVAL`: G-70, D-106 |
| `Api.option_table` | `mtl_options.def` and `mtl_options.h` list the same keys, types and ranges |
| `Api.reap_rec_size` | records smaller and larger than the library's, at pitch `rec_size`; the typed wrappers pass `sizeof(*r)`: G-72 |
| `Tx.state_call_table`, `Rx.state_call_table` | every state × every verb: G-49 |
| `Tx.roundtrip`, `Tx.exactly_once_random` | exactly one outcome per accepted submit over random submit, release, stop and close: G-01, G-31 |
| `Tx.invalid_submit_returns_slot`, `Tx.stale_foreign_lease`, `Tx.verb_on_rx_session`, `Tx.pool_count_max` | G-02, G-07, G-46, G-103 |
| `Tx.stop_drain_flush`, `Tx.stop_submit_race` | DRAIN completes, FLUSH gives `FLUSHED`; of a racing stop and submit exactly one wins: G-31 |
| `Session.interrupt_sticky`, `Session.interrupt_targets`, `Session.stop_close_wake`, `Session.close_leases_out`, `Session.close_idempotent`, `Session.close_race` | G-30, G-64, G-65, G-29 |
| `Session.names` | names copied and unique per instance (the listing with A2b): G-88 |
| `Wait.no_lost_wakeup`, `Wait.per_target` | the arming litmus test; waiters never steal: G-52, G-95 |
| `Rx.loopback_frame`, `Rx.release_any_order`, `Rx.dequeue_nothing_ready` | TX to RX on `null:1`; G-56; G-70 |
| `Results.order_cookie`, `Results.backpressure` | `seq` order, cookie verbatim; unread results block acquire with `BLOCKED_RESULTS`, nothing lost: G-09, G-47, G-04 |
| `Determinism.replay` | the suite twice on the test clock, result records equal byte for byte: G-92 |
| `Debug.inject_gated` | every MS1 fault reaches its outcome; the release-configured B job sees `-MTL_ENOTSUP`: G-93 |
| `Fault.force_error` | queued units FLUSHED and waiters `-MTL_EIO` |
| `Stats.counters` (A2b) | counters equal outcomes and never fall: G-42 |

**UB, the real engine through the bindings, without a NIC (plus T1's parity tests), in `UnitTest` next to their harness (`tests/unit/session/`, `tests/unit/pipeline/`):**
`done_once_{derive,convert,copy}`, `submit_carries_meta` (media time, user meta and `seq`
reach the engine frame: the boundary where PR #1610 lost user pacing,
[legacy-internals.md](legacy-internals.md) §12), `order_is_submit_order` (G-08),
`flush_reclaims_queued` (G-31), `no_reacquire_before_done` (G-05), `slot_decision` (AUTO,
AUTO + `NOT_BEFORE`, TAI nearest and its DROPPED reasons), `recovery_verdict`,
`rx_hold_release`, `rx_order_pub_seq`, `rx_incomplete_status`, `rtp_epoch_default` (G-20 with
today's rounding).

**I, KahawaiTest `UnifiedSt20p`, ported from `St20p` with the same case names:**
`tx_create_free_single`, `rx_create_free_single`, `tx_create_expect_fail`,
`rx_create_expect_fail`, `tx_create_expect_fail_fb_cnt`, `rx_create_expect_fail_fb_cnt`,
`digest_1080p_s1`, `digest_1080i_s2`, `digest_user_meta_s2`, `tx_put_frame_abort`; the new
cases `tx_idle_last_result` (G-03) and `close_while_streaming` (G-107 on the real engine); and
three cross-API cases, `cross_tx_unified_rx_legacy_1080p`, `cross_tx_legacy_rx_unified_1080p`
and the two-leg `cross_redundant_tx_unified_rx_legacy_1080p`. In the two cases with a legacy
receiver the legacy RX timing parser is on and asserts an RTP offset in [−1, 59] ticks and a
latency in [0, 1 ms]. The digest helper also checks the RTP: consecutive deltas equal the frame
period (1501 and 1502 alternating at 59.94), sequence numbers continuous, no packet lost. The
same filter runs on `kernel:lo` through the existing `st20p_kernel_loopback` case of
`.github/scripts/gtest.sh` and the case CI1 adds. If the day-10 gate is missed, only the cross
cases and the `_single` create cases stay in MS1.

**A, the acceptance smoke set (§5.5).**

Nothing legacy is deleted in MS1: the legacy pipelines are still what FFmpeg and GStreamer use.
The deletion rule for later is D-109.

### 5.4 RxTxApp on the new API

RxTxApp keeps its one `mtl_init` (`src/rxtx_app.c:451`) and wraps it with
`mtl_instance_from_legacy`, so the kinds that stay legacy run in the same process. MS1 ports
the JSON `"st20p"` arrays (`parse_json.c:2865` TX, `:3310` RX); MS2a ports `"video"` with
`"type": "frame"` and `"slice"`; `"rtp"` waits for packet units (MS5).

- New files `src/unified/tx_st20p_unified.c`, `rx_st20p_unified.c` and their header mirror the
  public functions of `tx_st20p_app.c` and `rx_st20p_app.c`; the dispatch is at
  `st_app_tx_st20p_sessions_init` / `st_app_rx_st20p_sessions_init` (`rxtx_app.c:534`, `:597`) and
  the stat, result and completion calls; the wrapper closes before `mtl_uninit`.
- Selection: `--api legacy|unified` (`args.c`), default from the build option `default_api`. No
  change to `tests/acceptance/mtl_engine/` in MS1 (D-110).
- **The output must not change.** The acceptance engine counts `app_rx_st20p_result(<n>), OK`
  lines (`rx_st20p_app.c:356-371`), and it greps lines that **libmtl** prints:
  `st20p_tx_create(<n>), transport fmt …, input fmt: …` (`st20_pipeline_tx.c:1175`) and the RX twin
  (`st20_pipeline_rx.c:1104`); `TX_st20p(<n>), frame get try X succ Y, put Z, drop D`
  (`st20_pipeline_tx.c:680`, parsed by `tests/acceptance/mtl_engine/application_base.py:424-426`);
  performance mode greps `TX_VIDEO_SESSION(...:app_tx_st20p_N): fps`. The video bindings print the
  same lines and the unified app names its sessions `app_tx_st20p_%d`. This is an exit criterion of
  B1, B2 and R1.
- SIGTERM: today RxTxApp handles SIGINT only (`src/rxtx_app.c:462`); R1 adds SIGTERM with
  `mtl_interrupt` in the handler (the ex11 pattern).

The JSON and CLI mapping (field details: [migration.md](migration.md) §4.4–§4.7):

| RxTxApp input | Today (st20p ops) | Unified |
|---|---|---|
| `interface[i]`, `ip`/`dip[i]`, `start_port` | `port.port[i]`, `dip_addr[i]`, `udp_port[i]` | `mtl_flow_ipv4(&sc.flows[i], ...)`; `flows[i].port` = `mtl_port_find(mt, inf->name, &p)`, then `p + 1` |
| `mcast_src_ip`, `payload_type` | `mcast_sip_addr[i]`, `payload_type` | `flows[i].source_filter`, `sc.payload_type` |
| `width`, `height`, `fps`, `interlaced` | same ops fields | `v.raster.width`, `.height`, `.fps` (`mtl_fps_rational(st_fps + 1)`), `.scan` |
| `transport_format` / `video_format`, `pg_format` | `transport_fmt` / `fmt` | `v.format` (`st20_fmt` + 1) |
| `input_format` / `output_format` | `input_fmt` / `output_fmt` | `v.app_format` (+ 1; 0 when it is the transport layout) |
| `pacing`, `packing` | `transport_pacing`, `transport_packing` | `v.sender_type`, `v.packing` (same values) |
| `tr_offset` (`"video"` only) | session default or none | `MTL_OPT_TROFFSET_NS`: absent for the session default, 0 for "none" |
| `device` | `device` | `MTL_OPT_VIDEO_CONVERT_DEVICE` |
| `enable_rtcp` | `*_FLAG_ENABLE_RTCP` | `MTL_OPT_RTX = 1` |
| `user_pacing` | `USER_PACING` + `frame->timestamp` | `sc.media_mode = MTL_MEDIA_TAI`, `unit.media_tai_ns` from `st_app_user_time()` (MS1) |
| `exact_user_pacing` | `EXACT_USER_PACING` | `MTL_SESSION_EXACT_LAUNCH` at create, `MTL_SUBMIT_EXACT` + `unit.launch_tai_ns` per unit (B3, else MS2a) |
| `user_timestamp` | `USER_TIMESTAMP` | `unit.media_tai_ns` (TAI): the RTP of the nearest slot; an RTP off the slot grid is `MTL_SUBMIT_RTP_TS` (MS3) |
| `drop_when_late` | `DROP_WHEN_LATE` | `MTL_OPT_LATE_POLICY = MTL_LATE_DROP` |
| `display`, `measure_latency`, URLs, `user_time_offset` | application side | unchanged |
| `--pacing_way` | `mtl_init_params.pacing` | `MTL_OPT_PACING` on each session (values in [migration.md](migration.md)) |
| `--ptp` | `MTL_FLAG_PTP_ENABLE` on the legacy instance | unchanged: the bridged instance keeps its time base (§2.4) |
| `--tx_ts_epoch` | `RTP_TIMESTAMP_EPOCH` | nothing: the unified default |
| `--tx_ts_delta_us` | `rtp_timestamp_delta_us` | `tx.rtp_trim_ns` (× 1000; MS3) |
| `--tx_start_vrx`, `--pad_interval`, `--tx_static_pad`, `--tx_no_bulk` | ops fields and flags | `MTL_OPT_VIDEO_START_VRX`, `MTL_OPT_VIDEO_PAD_INTERVAL`, `MTL_OPT_VIDEO_STATIC_PAD_P`, `MTL_OPT_VIDEO_DISABLE_BULK` |
| TX destination MAC | `tx_dst_mac`, `USER_P_MAC`/`USER_R_MAC` | `flows[i].dst_mac`, `MTL_FLOWF_USER_MAC` |
| `framebuff_cnt = 2` | ops | `sc.pool_count = 2` |
| SHA-256 check | `frame->user_meta` | an `MTL_META_USER` record in the meta area |
| `BLOCK_GET` | flag + block timeout | the `timeout_ns` of `mtl_tx_acquire` / `mtl_rx_dequeue` |
| `notify_event` | callback | `mtl_session_read_events` in the session thread (MS3; VSYNC as `MTL_EVENT_EPOCH_TICK`) |
| RX wire counters after the result line | `st20p_rx_get_session_stats` (`rx_st20p_app.c:379`) | `mtl_stat_get` keys (A2b, else MS2a; until then the unified app skips these diagnostic lines) |
| RX DMA, multi-thread, timing parser | RX flags | `MTL_OPT_DMA`, `MTL_OPT_RX_THREADS = 2` (one leg only), `MTL_OPT_RX_TIMING_PARSER` (B3, else MS2a) |
| `--hdr_split` | `HDR_SPLIT` | removed (D-112); the option is dropped from RxTxApp at release R+1 of the hiding |

### 5.5 The acceptance smoke set

The RxTxApp parameterisations only (`-k rxtxapp`); the FFmpeg and GStreamer variants keep
running on the legacy API as part of the legacy gate.

| Test | Why |
|---|---|
| `tests/single/st20p/test_resolutions.py::test_st20p_resolutions[Penguin_1080p-…rxtxapp…]` | carries the `smoke` marker |
| `tests/single/st20p/test_fps.py::test_st20p_fps` (p29, rxtxapp) | carries `smoke` |
| `tests/single/kernel_socket/kernel_lo_st20p/test_kernel_lo_st20p_refactored.py` (replicas 1 and 4) | needs no VF |
| `tests/single/st20p/test_integrity.py` | frame integrity |
| `tests/single/st20p/test_interlace.py` | the field path |
| `tests/single/st20p/test_multisession.py` | several sessions on one instance |

Stretch for MS1: all of `tests/single/st20p/` with the same pass list as the legacy build.
`test_drop_when_late.py` belongs to MS1 only if the drop-when-late mapping holds (B1); the `ptp`
subcase of `test_pacing_way.py` and `test_redundant.py` follow the time-source and two-leg work
of B1 and B2 and may slip to MS2a.

### 5.6 Exit criteria

1. **Build:** `./build.sh` builds libmtl with the unified API in `MTL_UNIFIED_EXPERIMENTAL_<rev>`;
   the node's exports equal the functions tagged MS1 (G-51); `check.sh` runs in CI.
2. **Unit:** `./build.sh unit` green, with the U cases and the UB cases of §5.3 and T1's parity
   tests, also in the debug + ASan job.
3. **Integration:** `UnifiedSt20p` green on E810 at mandatory level, with default pacing and with
   `--pacing_way tsc`, in the same run as legacy `St20p`; cross-API SHA-256 in both directions at
   1080p59.94 and 1080i59.94, and with two legs; the same filter on `kernel:lo`.
4. **Acceptance:** the smoke set passes on the unified-default build with the same list as the
   legacy build.
5. **Legacy gate** (§8.5): unchanged.
6. **Performance:** tasklet iteration avg and max within the §8.4 budgets against S0 (best
   effort in MS1, a gate from MS2a).
7. **Process:** every task has an `mtl-reviewer` APPROVE, its Gate 6 run where marked, and its
   row in the ledger.
8. **Examples:** ex01, ex02, ex03 and ex05 run on `null:1`.

### 5.7 Risks of MS1

| Risk | Mitigation |
|---|---|
| the core's slot layout is wrong for a later unit kind | C0 is reviewed at checkpoint 1 against rows, packets and holds (§2.2); the fields exist from C1b |
| the deferred wake costs a pinned core more than expected | S1 measures it in MS2a; W3 drains the same bitmap if it has to be built |
| the acceptance engine's log greps break | D-110 is an exit criterion of B1, B2 and R1 |
| ext frames in PA mode | MS1 uses the engine's own framebuffers for TX library pools (OI-59) |
| a stop waits up to 1 s for a unit's launch (`st_video_transmitter.c:188-191`) | accepted in MS1; the `tick` and command acks of MS2a bound it (D-103) |
| two APIs in one process: teardown order | the wrapper closes before `mtl_uninit` (SP-01); U and I tests of the bridge |
| review capacity | tasks ≤ 1.5 k lines, `mtl-reviewer` first, the WIP limits of §5.8; the gates of §5.2 cut stretch work first |

### 5.8 How MS1 runs

- **Git.** One work branch; one signed-off commit per task after its gates pass (the `mtl-commit`
  skill, no AI attribution); never push, never open a pull request; the maintainer reviews and
  pushes (§3.1). The standing permission is [ms1-status.md](ms1-status.md) §0.
- **Ledger.** `doc/unified-api/ms1-status.md` holds one row per task: task, branch, commit, gate
  evidence (test names, Gate 6 output), open questions; it exists, and the session keeps it
  current. A resumed session starts from README.md §1, the ledger and `git log --oneline -20`.
- **Task card.** Each `mtl-developer` call gets: goal, files, dependencies, the sections to read
  (§5.2's reading list, plus the §8.2 rows of its G-ids), the test names, the exit command, the size
  cap, the Gate 6 filter and the documents to update.
- **WIP limits.** The WIP limit counts tasks committed but not yet approved: at most three (week 1
  may have up to five). At most two developer tasks are in flight. A task that gets two
  `mtl-reviewer` BLOCKER rounds is split.
- **One tree.** Code tasks run one at a time in the main tree; a second one runs in a worktree
  only with its own `build_unit/`, and never runs `ninja install`.
- **Headers.** Changes to the public headers are batched once a week.
- **Stop and ask** the maintainer before a public header change, a new D-row, an engine diff above
  1 k lines, or a red legacy gate. Stop-and-ask items go to ms1-status §3, and the session
  continues with the next task that does not depend on them.

## 6. MS2–MS7

Each milestone ends with its exit criteria, the legacy gate green, and the review gates of the
repository. The detail of each item is in §2.3, §8 and the design documents; the engine items
are in [engine.md](engine.md) §11.

### 6.1 MS2: ST 2110-20 complete

**MS2a: rows, completeness and the nightly.**

- **The nightly:** the unified gtest and pytest cases with the legacy comparison (§6.9); RxTxApp's
  default switches to `unified` for the ported kinds once it is green.
- **Waking:** spike S1 on the deferred wake; W3 only if S1 shows harm to pacing.
- **Commands:** the `tick` hook in each video tasklet handler with `ctl`/`ack` (D-103): TX in
  `tvs_tasklet_handler` (the builder; the transmitter calls nothing), RX in
  `rvs_pkt_rx_tasklet_handler`; `CMD_TIMEOUT`; `mtl_session_discard`; the "last packet handed" hook
  for copy mode (`st_tx_video_session.c:2130-2134`, `:2655-2659`).
- **Rows (slice):** `MTL_UNIT_ROWS` TX (re-submit raises `progress`, `used` counts rows and 0 is
  legal) and RX (`mtl_rx_wait_rows`, `MTL_OPT_RX_ROWS_STEP`); the unit deadline is
  `mtl_tx_row_deadline(k, 0)`; an engine return code from `query_frame_lines_ready` that ends a
  frame early for TRUNCATE; interlaced rows kept; `tx.troffset_ns` beyond today's two values, with
  the cap VRX0 ≤ floor(TROFFSET / TRS) for every sender type; `MTL_OPT_ROWS_LATE`; the slice gtests
  (`st20_digest.cpp:584-695`, `st20_detect.cpp:289`) ported; RxTxApp `"video"` with `"slice"`.
  Gateway latency needs MS3's INDEX and TAI and `mtl_tx_next_slot`.
- **RX completeness:** zero fill of lost ranges in library pools (OI-26: the engine's packet
  bitmap reaches the binding; attached RX pools are never zero-filled: the unit is
  `MTL_RX_INCOMPLETE` with the loss counts of `mtl_rx_get_detail`), the RX due time and
  force-complete (E8 deadline part, G-82), `missed_before`, `MTL_SESSION_RX_LATEST`, the per-unit
  timing in `mtl_rx_get_detail`.
- **Instance:** `mtl_instance_open` on PCI BDFs (with `PTP_BUILTIN`) and on `native_af_xdp:`;
  `MTL_INSTANCE_SHARED` (U-004, G-77); `mtl_log_set_sink` over the per-scheduler log rings.
- **Helpers:** `mtl_media_ticks`, `mtl_media_tai`, `mtl_format_parse`, `mtl_time_set_reference`.
- **RX parsing fixes** (legacy bugfixes too): RTP header extensions and CSRCs on video RX (RXHDR,
  SF-68; the other essences in MS4a); the timing parser against RP 2110-25 (TPARSER, SF-81) if B3
  did not land; the third-SRD and second-SRD placement fix (SF-76). Details in engine.md §11, §12.
- **MS1 stretch:** A2b and B3 if they did not land in MS1 (X goes to MS3 with E2).
- **Tests:** waves 2a (rest) and 2b of §6.8.

**MS2b: video memory, then the st20p re-base.**

- **Video memory:** `mtl_session_attach`, `mtl_tx_acquire_slot`, `MTL_SESSION_RX_BY_INDEX` (MXL),
  `mtl_tx_acquire_layout` and `mtl_rx_provide` (GStreamer per-frame ext frames), holds and
  `mtl_tx_send_slot` (split-forward, ex09), ext frames with their page table (OI-59), the pool cap
  of 8 lifted (E11).
- **Converter plugins:** on the transform claim and done (D-100).
- **The st20p re-base on the core:** last, after the nightly comparison has burned in: `get_frame`
  = acquire seen as `st_frame`; `put_frame`/`put_ext_frame` = submit; `put_frame_abort` = release;
  `BLOCK_GET` = the core's wait (fixes SF-15, SF-16); one legacy notifier, exactly once (R2).
  Auto-detect, user meta, timing-parser meta and drop-when-late live in the video bindings. Safety
  net: the 44 `St20p` cases, the pipeline unit tests and T1's parity tests.
- **Tests:** waves 2c and 3a of §6.8.

Exit: every ST20 legacy capability of §2.3 has a unified path except packet units (MS5) and the
timing subset of MS3; the legacy st20p suites green on the core; ex04, ex06, ex09 run (ex08
needs `MTL_MEDIA_INDEX`, MS3); the `ptp` group green with the RxTxApp default switched.

### 6.2 MS3: timing and observability

- **The ST20 timing subset:** media modes INDEX and TAI with snapping (`MTL_OPT_SNAP_MODE`),
  `MTL_SUBMIT_DISCONTINUITY`, `MTL_SUBMIT_RTP_TS`, `sc.media_time_offset_ns` (in TAI mode the
  producer's latency), `tx.rtp_trim_ns`, start `MTL_AT_TAI` and `MTL_AT_INDEX` (ARMED),
  `mtl_tx_next_slot`, RX `unit.media_index`; E1 (media time and launch carried apart, so a launch
  may land in another slot), E2 (exact `floor`), E3 reporting (margins at pick-up).
- **Events, stats, health:** `mtl_read_events` with the session and instance events
  (`mtl_events.h`), the stats registry's per-scheduler blocks, `mtl_instance_get_health`,
  `mtl_instance_shutdown` with its report, the stalled-queue close after S8
  (attached memory completes only after the queue stop and start).
- **ABI hygiene:** after an `nm` audit of the users of leaked symbols: the libmtl soname, the
  `MTL_LEGACY` node for the legacy headers' functions, then `local: *` (hidden internals) as a
  separate step; call-class enforcement in debug builds (D-05); the fork rule of R8.
- **FFmpeg:** the plugin's st20p path on the new API, next to its legacy path.
- **Tests:** gtest waves 3b, 3c and 4 (NoCtx) of §6.8; ex08 and ex11 run.

### 6.3 MS4: every essence

**MS4a**: audio (st30p re-based; E6 carry buffer), ANC (st40p re-based; E7 RTP from media time,
the ST 2110-40 window and keep-alive; UDW, SF-78/SF-79: 10-bit user words and per-packet error
skipping), RX header extensions on these essences (RXHDR), fastmeta (a frame binding over the st41 session, RX frames assembled at dequeue
from the RTP ring); A/V sync on the epoch timeline; any frame rate (an engine table change;
until then a rational outside the table is `-MTL_ENOTSUP`); DSCP on these bindings; the essence
members of `mtl_session_config`; RxTxApp kinds `st30p`, `st40p`, `"ancillary"`,
`"fastmetadata"` with `"type": "frame"`; their gtests replaced per D-109; their acceptance
directories through `--api`.

**MS4b**: cvideo (st22p re-based; ST 2110-22 codec plugins on the transform state, the
`video.*` keys apply; E10: ST22 CBR and the synchronous oversize check), plugin ABI v2 with `mtl_plugin_open`, `mtl_convert`, `mtl_session_capture`
(pcapng); RxTxApp `st22p`.
Exit: G-45 (one verb sequence for every essence × direction).

### 6.4 MS5: packets and operators

Packet units on every essence over today's RTP paths: one shared chunk expander on the TX
tasklet (header mbufs plus extbuf attaches, one completion per chunk; PE1), copy RX at dequeue
from the engine's RTP ring, PE2–PE8; the generic `MTL_RTP` essence and ST 2022-6 with the packet
size from the MTU (PE7; 2022-6 cannot be sent today, its 1396–1456 B packets exceed
`MTL_PKT_MAX_RTP_BYTES` = 1352, `include/mtl_api.h:89`, SF-77) and the ST 2110-21 NL schedule ST
2022-8 §6 requires for 2022-6 (the 2022-6 part of E5 moves into MS5 with PE7). `mtl_session_update` with FLOWS, LEGS,
MEDIA and POOL at a boundary (the prepared header swap of D-103; the RTP sequence seed of OI-62),
RTCP sender reports on TX (driven by options, the library builds the Info Block; their names leave `MTL_LATER`), the link
monitor, leg admin state, `MTL_QUERY_CHECK_CAPACITY`, MtlManager reconnect. RxTxApp
`"type": "rtp"`, the 61 RTP-level gtest sites, the st41 acceptance tests and pcap replay on the
unified path. Exit: G-PKT-1…7, the update gtests ported, a 2022-6 stream accepted by a
third-party receiver.

### 6.5 MS6: synchronisation and the ecosystem

Start arrays (`mtl_session_start` over n sessions with `MTL_WHEN_ORIGIN`: media index 0 of every
started session is the start's T0), ANC and fastmeta following the first video of their start
array (its raster and slot delay), sample-accurate audio, E5, E9 (the published time base, a PHC
as time source, FREERUN, whose name leaves `MTL_LATER`), E13, clock steps (media times are kept); recovery and auto-detect
re-init on library workers; the GStreamer, OBS, Python (with the GIL test) and Rust ports, the
rest of the FFmpeg plugin, and the plugin owners' review of them. Exit: the A/V/ANC example
through one start array with exact RTP; the plugins on the new API pass the acceptance smoke
suite.

### 6.6 MS7: freeze and hide

The external review with at least three named consumers (the FFmpeg and GStreamer plugin
owners, the MXL team, the external engine team) and the private-user questionnaire, before the
header freezes; the `MTL_1.0` freeze (G-51 for `MTL_1.0`); the hiding stages F, F+1 and F+2
(D-83) with the deprecation policy of [deployment.md](deployment.md) §7. Exit: the legacy headers carry the
deprecation warning, and every in-tree consumer builds without `MTL_LEGACY_API`.

### 6.7 Phase 7

NMOS extras, the rest of `mtl_ipmx.h` (SDP, the RTCP MIB, encryption, PEP) and IPMX timing
without PTP ([nmos-ipmx.md](nmos-ipmx.md)), after MS7 (D-98); every name of it is under
`MTL_LATER` until then. Engine items that IPMX products need on any API and that have no
milestone yet (IGMPv2, the `update_destination` fixes) are bugfixes and may be pulled into an
earlier milestone when a user asks; DSCP (MS1, MS4a, MS4b), any frame rate (MS4a) and RTCP
sender reports (MS5) have theirs.

### 6.8 The ST20 gtest port waves

A port **replaces** its legacy case after the comparison window (D-109). Wave 1 and the
`St20p` part of wave 2a are MS1 (§5.3); waves 2a (rest) and 2b are MS2a, 2c and 3a MS2b; 3b,
3c and 4 are MS3.

| Wave | Milestone | Legacy cases ported | Unified feature they check |
|---|---|---|---|
| 1 | MS1 (the `_single` `St20p` cases of §5.3), MS2a (the rest) | `St20p.{tx,rx}_create_free_{single,multi,mix,max}`, `{tx,rx}_create_expect_fail`, `{tx,rx}_create_expect_fail_fb_cnt`; `St20_tx`/`St20_rx` `create_free_*`, `create_expect_fail`, `create_expect_fail_fb_cnt` | create, query, close; `POOL_COUNT_MAX` (G-103); G-32 |
| 2a | MS1 (part, §5.3), MS2a | `St20p.digest_1080p_s1`, `digest_1080i_s2`, `digest_s2`, `digest_1080p_internal_s1`, `_internal_s2`, `_no_convert_s2`, `_packet_convert_s2`, `transport_yuv422p10le`, `digest_user_meta_s2`, `digest_rtcp_s1` | the pipeline data path, conversion (per packet on the RX tasklet for `_packet_convert_s2`), user meta, RTX |
| 2b | MS2a | `St20_tx.frame_*`, `mix_*` (frame level); `St20_rx.frame_*`, `mix_*`, `digest_frame_*`, `digest20_field_*` (frame level), `after_start_*`, `frame_meta_*`, `digest_rtcp_s1`, `_s3` | derive mode (`v.app_format = 0`), 2022-7, `mtl_rx_get_detail` |
| 2c | MS2b | `St20_rx.linesize_digest_s3`, `linesize_digest_crosslines_s3` | `v.linesize` of library pools |
| 3a | MS2b | `St20p.rx_put_frame_abort` (`tx_put_frame_abort` is ported in MS1, §5.3), `redundant_stats` (stats keys, 4 ports), `digest_1080p_fail_interval`, `digest_1080p_timeout_interval` (unit status on plugin failure) | results, stats |
| 3b | MS3 | `St20s.rx_get_stats_concurrent_free_no_uaf` (read during close; the reset variant becomes a second reader, G-42); `St20_rx.detect_1080p_fps59_94_s1`, `detect_mix_frame_s3` (`MTL_EVENT_RX_FORMAT`) | stats, events |
| 3c | MS3 | `St20p.tx_no_epoch_drop`, `tx_user_pacing_no_epoch_drop`; `St20_tx.tx_user_pacing` | the timing subset; stats keys as below |
| 4 | MS3 | NoCtx (`tests/integration_tests/noctx/testcases/`): `st20p_user_pacing` (4), `st20p_ptp_epoch_recovery` (3), `st20p_interlaced_pacing` (1), `st20p_redundant` (1), `st20p_stability` (1); a new kill-and-re-open case (G-112) | `mtl_instance_open` and `mtl_instance_close` on real VFs, one process per case (`noctx/run.sh`) |

Cases with a later milestone:

| Legacy cases | Why | When |
|---|---|---|
| `St20_tx.rtp_*`, `St20_rx.rtp_*`, `digest_rtp_*`, `digest_frame_rtp_s3`, `digest_ooo_*`, `create_expect_fail_ring_sz`, `rtp_pkt_size` | packet units (the ooo cases send RTP-level packets) | MS5 |
| `digest_*slice*`, `digest_tx_slice_s3`, `detect_mix_slice_s3` | row units | MS2a |
| `St20p.*ext*`, `tx_ext_frame_manual_release_two_phase`, `St20_rx.ext_frame_*`, `dynamic_ext_frame_s3`, `linesize_digest_ext_s3`, `St20_tx.ext_frame_*`, `get_framebuffer*` | attached pools and `mtl_session_get_slot` (`mtl_mem.h`); `mtl_rx_provide` | MS2b |
| `St20_tx.update_dest_*`, `St20_rx.update_source_*` | `mtl_session_update` with `MTL_UPDATE_FLOWS` | MS5 |
| `St20_rx.uframe_*`, `detect_uframe_mix_s2`, `digest_hdr_split` | removed (D-112) | deleted with the legacy API |
| `St20_rx.pcap_dump` | `mtl_session_capture` | MS4 |
| `St20p.plugin_register_*` | converter plugins on the transform state; plugin ABI v2 (`mtl_plugin.h`) | MS2b (converters), MS4b (ABI v2) |
| `St20p.frame_is_late` | tests the legacy helper `st_frame_is_late()`; unified results carry margins instead | none |

### 6.9 The nightly (MS2a)

The comparison runs only during the window of D-109, per suite; after it the
legacy pipeline cases are deleted and the unified cases are the suite.

**gtest nightly** (`.github/scripts/gtest.sh`, `generate_test_cases()`):

- new cases next to the legacy ones, same ports and options: `unified_st2110_20_tx`
  (`UnifiedSt20_tx*`), `unified_st2110_20_rx` (`UnifiedSt20_rx*`, two shards like the legacy one),
  `unified_st2110_20p` (`UnifiedSt20p*`); in the nightly block the pacing variants
  `unified_st20p_auto_pacing_pa`, `_va`, `unified_st20p_tsc_pacing` (`UnifiedSt20p*:-*ext*`),
  `unified_st20p_kernel_loopback` (`kernel:lo`) and the unified NoCtx cases through `noctx/run.sh`;
- the baseline block (pull requests) has only `unified_st2110_20p` and the `kernel:lo` case that
  CI1 adds in MS1; the rest is nightly only;
- `nightly-gtest.yml` uploads `${TMP_FOLDER}/gtest_*.xml` with `gtest.log`; today only `gtest.log`
  is uploaded, although `gtest.sh` writes the XML.

**Comparison with the legacy run.** A compare step after `task ci:gtest-nightly` pairs each
`Unified<Suite>.<case>` with `<Suite>.<case>` from the JUnit XML of the same runner and night:

| Legacy | Unified | Verdict |
|---|---|---|
| pass | pass | OK |
| pass | fail | regression of the unified API: fails the job after burn-in |
| fail | pass or fail | legacy problem: reported, not counted against the unified API |
| — | skipped by `--level` | reported as skipped, never as passed |

Burn-in: the compare step is non-blocking (`continue-on-error`) until ten consecutive nightlies
on e810, e830 and e835 are free of "regression" rows; then it blocks. The summary also lists,
per pair, the fps and frame counts the cases print, so a pass with lower throughput is seen.
The st20p re-base of MS2b starts only after the burn-in.

**Wire comparison.** The unified defaults differ from the legacy ones by design: video RTP from
the frame epoch (D-10), and incomplete frames delivered (`rx.incomplete = MTL_RX_DELIVER`). A pcap comparison therefore
runs the legacy side with `ST20P_TX_FLAG_RTP_TIMESTAMP_EPOCH` and
`ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME`, and compares headers and payload hashes per packet;
with E2 on, RTP may differ by one tick at 1001 rates and is checked against the oracle instead
(G-99).

**pytest nightly** (`nightly-pytest.yml`): the `st20p` directory of the matrix gains an
`api: unified` variant through RxTxApp `--api` (D-110), or the reviewed `test_config.yaml` key if a side-by-side matrix is wanted; reports are combined as today,
and the same pair rule applies per test ID. The `ptp` directory runs RxTxApp `--ptp` on VFs; it
keeps passing when the RxTxApp default switches (§2.4).

Exit: two weeks of nightlies with the compare step blocking and green; the compliance gate of
§8.4 green; the RxTxApp default switched to `unified` for the ported kinds; the legacy cases and
the `ptp` group unchanged and green.

### 6.10 The Kubernetes track

The pod fixes EK1–EK19 ([engine.md](engine.md) §11; the rules D-89…D-92 and
[deployment.md](deployment.md)) are legacy bugfixes on by default, so they run as a parallel
track beside the milestones: one task in review at a time, never on a milestone's critical
path, targeted to finish by the MS3 exit. EK1, EK2, EK6 and EK7 come first: they remove a
use-after-free, CPU theft between pods and a crash loop on a valid configuration. The items that
need the unified API stay with their milestone: EK20 (the handle table) in task C1a, EK18 (the
non-blocking open) with the NoCtx ports of wave 4 and EK21 (lossless shutdown) in MS3.

## 7. Engine fixes on the critical path

Bugfixes are on by default for legacy users too; wire-visible changes are opt-in on the legacy
API and on in the unified API (D-24). This table lists the MS1 rows; the full list (E1–E13,
R1, R2, MF1–MF10, EK1–EK21, PE1–PE9) with its `path:line` and milestones is
[engine.md](engine.md) §11, and the defects are in engine.md §12.

| Fix | Kind | Task | Source |
|---|---|---|---|
| R2: the core's completion CAS gives exactly-once completion for core sessions (legacy st20p in MS2b) | fix | C1b | SF-05, SF-38, SF-39 |
| `seq` assigned at submit (the descriptor ring) | fix | C1b (G-08) | SF-44 |
| MF1: `tv_frame_free_cb` claims, decrements `refcnt` and clears the address before any completion | fix | E1, unless PR #1770 landed | SF-05 |
| MF7: the builder takes a reference on `sh_info` at frame start and drops it after the last attach | fix | E1 | — |
| R1, the TX recovery verdict: in-flight units DROPPED (`RECOVERY`) in the slot, only software-held references dropped, publication by the last PMD reference; two alternating `sh_info` per engine frame with a use generation in `fcb_opaque` | fix | E1 | SF-12, SF-41 |
| idle descriptor cleanup (`mt_txq_done_cleanup`, rate-limited, dedicated queues), only with the frame state `WAIT_FRAME` | fix | E1 (G-03, close drains) | S6 |
| RX hook never refuses a frame | fix | E1 | SF-45 |
| incomplete delivery always enabled internally | internal | E1 | — |
| `rte_thread_register` for thread-mode schedulers | fix | E1 | SF-49 |
| ST30P gets `ST30P_TX_FLAG_USER_TIMESTAMP`; ST40P honours `USER_TIMESTAMP` without `USER_PACING` (`st40_pipeline_tx.c:199`) | fix for legacy users (about 40 lines) | E1 | — |
| the legacy teardown order: `mt_sch_mrg_uinit` releases lcores before it frees active schedulers | fix | E1 | OI-3, SP-01 |
| SP-01: `mtl_uninit` self-deadlock with live sessions | ordering in the bridge (the fix in MS3) | A1 | SP-01 |
| `st_engine_core.h`: an RX frame put callable from the tasklet, the recovery verdict, per-leg arrival time (E8's arrival part) | internal | E1, used by B1 and B2 | — |
| DSCP: `mtl_flow.dscp` into the TX builders' TOS | engine change | B1 | — |
| DMA-busy drop counted | fix | B3 | SF-46 |
| E2: exact rational math, `floor` for RTP | wire-visible (±1 tick at 1001 rates; legacy opt-in) | X, else MS3 | — |

Not on the MS1 path, because the core avoids them by design: SF-13 (stats getter from a
callback), SF-15 and SF-16 (the pipeline's `BLOCK_GET` mutex and `wake_block`; the core waits on
its own wait targets, and the legacy pipelines get the fix when they are re-based), SF-14
(`update_destination` under the spinlock; MS5).

## 8. Test plan and guarantees

A behaviour without a test is not promised. §5.3 lists what MS1 tests; the comparison of §6.9
replaces legacy cases by their ports (D-109).

### 8.0 Requirements and the guarantees that close them

The text, level definitions and sources of each requirement are in
[requirements.md](requirements.md); the rule text lives in [contract.md](contract.md),
[timing.md](timing.md) or [engine.md](engine.md). This table maps each requirement to the
guarantees that close it and the milestone that closes them ("When").

| ID | Level | Requirement (label) | Guarantees | When |
|---|---|---|---|---|
| R-OBJ-1 | MUST | one opaque session type for every essence and direction | G-45 | video MS1; G-45 MS4b |
| R-OBJ-2 | MUST | direction verbs: TX acquire, submit, release; RX dequeue, release | G-46 | MS1 |
| R-OBJ-3 | MUST | a buffer is a pool slot; per-use fields in `struct mtl_unit` | G-12, G-48 | MS2b (video), MS4a and MS4b (other essences) |
| R-OBJ-4 | MUST | a 64-bit cookie returned verbatim in the result | G-47 | MS1 |
| R-OBJ-5 | MUST | typed handles validated safely; stale, foreign and null handles fail | G-07, G-29, G-71 | MS1 |
| R-OBJ-6 | SHOULD | many sessions waited on together (per-session handles, epoll) | G-52, G-95 | per-session waits MS1; instance-wide waits and events MS3; shared queues not in v1 (`MTL_LATER`) |
| R-MEM-1 | MUST | the library pool is the default | G-15 | library pools MS1; G-15 MS2b (video) |
| R-MEM-2 | MUST | application memory imported once as a region | G-10, G-11, G-96, G-101, G-102 | MS2b (video), MS4a and MS4b (other essences) |
| R-MEM-3 | MUST | a region outlives every reference | G-10, G-11 | MS2b (video), MS4a and MS4b (other essences) |
| R-MEM-4 | MUST | an import maps into every device on the path, or fails | G-16 | MS2b (video), MS4a and MS4b (other essences) |
| R-MEM-5 | MUST | data-path policy independent of allocation origin; path reported | G-13, G-14, G-84 | MS2b (video), MS4a and MS4b (other essences) |
| R-MEM-6 | MUST | fixed pools, attached before start | G-35, G-36, G-103 | G-103 MS1; the rest MS2b |
| R-MEM-7 | SHOULD | imported memory for every essence | G-15 | MS2b (video); every other essence with its binding (MS4a, MS4b) |
| R-MEM-8 | SHOULD | RX leases released in any order; holds; exhaustion policy | G-56, G-60, G-83 | G-56 MS1; holds MS2b |
| R-MEM-9 | MAY | mixed-origin pools, device memory, per-acquire layouts | — | per-acquire layouts MS2b; device memory and mixed-origin pools MS6 |
| R-TIME-1 | MUST | every time field names its clock, unit and epoch | G-17 | the ST20 fields MS1–MS3; the rest MS6 |
| R-TIME-2 | MUST | media time and launch are separate inputs | G-18, G-59 | G-59 MS1 (UB; M tier MS6); G-18 MS6 |
| R-TIME-3 | MUST | RTP = `floor(M × R) mod 2^32`, exact, one rule | G-19, G-21, G-22 | video MS3 (MS1 if task X lands); G-21, G-22 MS6 |
| R-TIME-4 | MUST | default video media time is the frame epoch | G-20 | MS1 (video), MS4a (the ST40 half) |
| R-TIME-5 | MUST | late and underrun policies with per-unit outcomes | G-26, G-28, G-61, G-66, G-85, G-86 | G-26 with EXACT (B3, else MS2a), the rest of G-26, G-28, G-85 MS3; G-61 MS4a; G-66 MS4b; G-86 MS6 |
| R-TIME-6 | MUST | time source, lock state and offset, with events | G-54, G-62 | events MS3; G-54, G-62 MS6 |
| R-TIME-7 | SHOULD | one exact epoch grid; all-or-none start arrays | G-23, G-24, G-63, G-68, G-94, G-104 | G-94 MS3; G-23, G-24, G-68, G-104 MS6 on the epoch timeline; G-63 with created timelines, not in v1 |
| R-TIME-8 | SHOULD | sample-accurate audio submission | G-21 | MS6 |
| R-TIME-9 | SHOULD | `mtl_tx_next_slot`: next slot and latest submit time | G-53 | MS3 |
| R-TIME-10 | SHOULD | RX media time, per-leg arrival, optional link offset | G-25, G-67, G-82 | G-82 MS2a; the rest MS6 |
| R-TIME-11 | MUST | progressive rows (`MTL_UNIT_ROWS`, slice mode) | G-113 | MS2a |
| R-TIME-12 | SHOULD | RX `media_index`; essences align without app arithmetic | G-76, G-105 | video G-76 MS3; the rest MS6 |
| R-TIME-13 | SHOULD | processes share the epoch grid without IPC | G-76, G-105 | MS6 (G-105) |
| R-CMP-1 | MUST | exactly one terminal outcome per accepted submission | G-01, G-02 | MS1 |
| R-CMP-2 | MUST | results are never lost | G-04, G-05 | MS1 |
| R-CMP-3 | MUST | TX statuses ON_TIME, DROPPED, FLUSHED, FAILED (LATE: Phase 7) | G-28, G-57 | the MS1 statuses MS1; G-28, G-57 MS3 |
| R-CMP-4 | MUST | "reusable" means nothing can still reach the storage | G-06, G-100 | MS2b |
| R-CMP-5 | MUST | events in a separate bounded, coalesced queue | G-41, G-58 | MS3 |
| R-CMP-6 | MUST | one error vocabulary; `mtl_last_error` | G-30, G-38, G-57, G-70 | G-30, G-70 MS1; G-57 MS3; G-38 MS4b |
| R-CMP-7 | SHOULD | a portable wait handle armed by `-MTL_EAGAIN` | G-52 | MS1 |
| R-OBS-1 | MUST | one stats schema; cumulative counters; lock-free reads | G-40, G-42 | G-42 with A2b (else MS2a); G-40 and per-scheduler stats blocks MS3 |
| R-OBS-2 | MUST | queue gauges per slot state, from one scan | G-43, G-55 | G-43 MS3; G-55 MS6 |
| R-OBS-3 | MUST | time, link, leg and scheduler status getters with events | G-41, G-54, G-91 | G-41 MS3; G-91 MS5; G-54 MS6 |
| R-OBS-4 | SHOULD | lateness histograms; windowed maxima | G-43, G-55 | MS6 |
| R-OBS-5 | SHOULD | logs carry instance, session and name; tasklets use the log ring | G-44, G-88 | G-88 MS1; G-44 MS6 |
| R-THR-1 | MUST | no public function and no application code on a tasklet | G-38 | measured MS1; P in MS4b |
| R-THR-2 | MUST | lock-free tasklet ↔ application hand-off | G-39 | measured MS1; P in MS4b |
| R-THR-3 | MUST | one call class per function, enforced in debug builds | G-50 | MS3 |
| R-THR-4 | MUST | heavy per-unit work never on a tasklet; where it runs reported | G-14, G-39 | MS1; G-14 MS2b |
| R-THR-5 | SHOULD | waking a sleeper costs a tasklet at most one signal | G-39, the waker budgets of §8.4 | MS1 (no syscall in a completing context); S1 in MS2a decides W3 |
| R-LIFE-1 | MUST | the nine session states; reversible start and stop; close anywhere | G-35, G-49, G-80 | G-49 MS1; G-80 and discard MS2a; G-35 MS2b |
| R-LIFE-2 | MUST | DRAIN and FLUSH leave every accepted unit terminal | G-31 | MS1 |
| R-LIFE-3 | MUST | interrupt and stop wake blocked callers with a distinct code | G-30, G-64 | MS1 |
| R-LIFE-4 | MUST | after retirement no application code runs, no memory touched | G-29, G-37, G-65, G-77, G-81 | G-29, G-65, G-77 (exclusive open) MS1; the shared instance MS2a; G-37 MS2b; G-81 MS3 |
| R-LIFE-5 | SHOULD | instance re-open within one process (#1341) | G-112 | MS1 (U); MS3 (I, wave 4) |
| R-CAP-1 | MUST | capability query; REQUIRE, PREFER or OFF; granted values reported | G-13, G-14, G-34 | granted values MS1; G-13, G-14 MS2b; G-34 MS4b |
| R-ABI-1 | MUST | `struct_size` inputs, opaque handles, unknown flags rejected | G-33, G-51, G-72 | MS1 |
| R-ABI-2 | MUST | a version script with an experimental node; libmtl soname | G-33, G-51, G-72 | MS1 (the experimental node); MS3 (soname, `MTL_LEGACY`, `local: *`) |
| R-ABI-3 | SHOULD | an experimental tier | G-51 | MS1 |
| R-ABI-4 | MUST | versioned instance parameters; mismatching shared open fails | G-77 | MS1 (versioned parameters); MS2a (the shared instance) |
| R-ABI-5 | MUST | C99 and C++17 with `-Wpadded -Werror`; size checks; `MTL_INIT` | G-73, G-74 | MS1 |
| R-OPS-1 | MUST | flow changes on every leg commit atomically | G-75 | MS5 |
| R-OPS-2 | MUST | resources reserved per configured leg; admin and oper state | G-91 | MS5 |
| R-OPS-3 | MUST | a stable, unique session name; sessions listable | G-88 | MS1 (the listing with A2b, else MS2a) |
| R-OPS-4 | SHOULD | remaining capacity and a dry-run create | G-89 | MS5 |
| R-OPS-5 | SHOULD | a STOPPED session reconfigured, keeping its identity | G-87 | MS5 |
| R-OPS-6 | MUST | MtlManager loss never stalls running sessions | G-98, G-110 | G-110 MS3; G-98 MS5 |
| R-OPS-7 | MUST | create and start never block on ARP or IGMP | G-90 | MS5 |
| R-TEST-1 | MUST | the null backend `null:<n>` | G-92 | MS1 |
| R-TEST-2 | MUST | the test clock | G-92 | MS1 |
| R-TEST-3 | MUST | fault injection `mtl_debug_inject`, debug builds only | G-93, §8.3 | MS1; faults per milestone |
| R-TEST-4 | SHOULD | the I tier pulls a link and resets a VF | §8.3 | MS5 (`vf_link`); MS6 (`vf_reset`) |
| R-USE-1 | SHOULD | `mtl_session_open` with a library pool and results off | G-97 | MS1 |
| R-USE-2 | SHOULD | binding helpers: stride-explicit reads, copy helpers, Python wrapper | G-72, G-97, the GIL test (§8.1) | MS6 |
| R-MIG-1 | MUST | every engine fix reaches legacy users; wire changes opt-in | G-99, G-106 | every milestone (§8.5); G-106 (E2's legacy opt-in) MS3 |
| R-PERF-1 | MUST | numeric budgets before code, confirmed by S0 | §8.4 | MS1 |
| R-SEC-1 | MUST | security properties of the new surfaces stated and tested | G-93, G-96 | G-93 MS1; G-96 MS2b |

The Kubernetes requirements K-REQ-1…20 are mapped in [deployment.md](deployment.md); the
lifecycle part MS1 and MS3 build is G-107…G-112.

### 8.1 Tiers and evidence

| Tier | Where | Needs |
|---|---|---|
| B build | CI | headers, `check.sh`, `readelf`, `nm` |
| U unit | `tests/unit/unified/` (`UnifiedUnitTest`) | nothing: the core over the null backend and the test clock, one long-lived instance per binary |
| UB unit at the engine boundary | `tests/unit/session/`, `tests/unit/pipeline/` (`UnitTest`) | the real engine driven through the bindings, without a NIC |
| I integration | `KahawaiTest` (ported cases), NoCtx | VFs, hugepages, root |
| A acceptance | `tests/acceptance/` | RxTxApp end-to-end |
| M measurement | lab with HW timestamps, EBU LIST | accuracy claims per NIC × pacing class |

A guarantee is **P** (promised: evidence exists or is funded in the named milestone; a red P
test blocks its milestone) or **BE** (best effort until the named evidence exists). A behaviour
without a test is not promised. A milestone is done only when every MUST requirement in its
scope (§8.0) has a contract test at the cheapest tier that can observe it. A red P test blocks
the exit; a BE guarantee never does, and its row says what evidence would make it P.
Why U over the null backend: UB is the most brittle tier; at `545a266a` 24 unit-test files
`#include` production `.c` files, and 60 commits touched them in 2026. So guarantees that are
properties of the core alone (G-04, G-30, G-49, G-57, G-64 and most later ones) are U over
`null:1` and need no engine internals. Properties of the engine boundary stay UB, because that
is where PR #1610 failed (its lost timing metadata, [legacy-internals.md](legacy-internals.md) §12);
UB drives the engine through the bindings, which implement callbacks the engines already have,
so harness churn follows changes of those callbacks, not every refactor.

Test isolation: `UnifiedUnitTest` opens one long-lived null-only instance, which initialises
EAL once (`--no-huge --no-pci --in-memory`; today's unit binary passes `--no-huge --no-pci`,
`tests/unit/common/ut_common.c:25-31`) or accepts the EAL a fixture already initialised; NoCtx
cases stay one process each; debug-API fault state is per object and the fixture clears it after
each case, so a case that leaves a debug fault set fails.

Bindings: when the Python wrapper lands (MS6), its suite includes a GIL
check on `null:1`: a WT call blocked in one Python thread (`mtl_rx_dequeue` with a 1 s timeout)
must not stop a second Python thread, because the binding releases the GIL around every WT
call.

### 8.2 Guarantees of MS1–MS3

The notes behind each method, and the text of the later guarantees, are in
[requirements.md](requirements.md) and [timing.md](timing.md) §16.1. "M" is the milestone whose
exit needs it.

| ID | Guarantee | Tier | M | How |
|---|---|---|---|---|
| G-01 | every accepted TX submission has exactly one terminal outcome: a result when results are on, else a counter | U, UB, I | MS1 | count outcomes against accepted submits over randomised submit, release, stop and close sequences (discard from MS2a) on `null:1`, also under the §8.3 faults |
| G-02 | a failed first submit gives no result and returns the slot to the pool, except on `-MTL_EBADF` and `-MTL_ESTALE`, which change no state (D-88) | U | MS1 | submit with each invalid-argument class: no result, and the slot is free again |
| G-03 | an accepted unit progresses without another call, the last one before idle included | I | MS1 | submit one unit, call nothing else, observe its result (I1's `tx_idle_last_result` on a VF) |
| G-04 | results are never lost: unread results make acquire return `-MTL_EAGAIN` with `blocked_on = MTL_BLOCKED_RESULTS` | U | MS1 | `MTL_SESSION_RESULTS` and no reap: acquire ends with `-MTL_EAGAIN` and `MTL_BLOCKED_RESULTS`; then every result is readable |
| G-05 | a slot is not re-acquirable before its outcome is recorded | U, UB | MS1 | race acquire against completion in a stress loop, through the video binding over the real engine |
| G-06 | "reusable" means no reader or writer remains (poisoned after the result, never on the wire) | UB, I | MS2b | poison the slot right after its result; a capture shows no poisoned packet |
| G-07 | foreign or stale handles fail with `-MTL_EBADF`, returned leases with `-MTL_ESTALE`, without changing state | U | MS1 | fuzz handle values; double submit and release; release a lease on another session |
| G-08 | transmit order is submit order | UB | MS1 | acquire A and B, submit B then A before pick-up: B is sent first |
| G-09 | results are in submission order, each with its `seq` | UB | MS1 | completions forced from another thread and out of order: results still in `seq` order |
| G-19 | RTP = `floor(M × R) mod 2^32`, exact, no drift, across the 2^32 wrap | U, UB | MS3 | the oracle method of [timing.md §16.1](timing.md) |
| G-20 | default video media time is the frame epoch | UB | MS1 | the RTP of frame N equals the epoch RTP of slot N with today's rounding (`st10_tai_to_media_clk`), with no TR offset term; `floor(N × TFRAME × 90 kHz)` from E2 (MS3, or MS1 with task X); the ST40 half (equal RTP for ST20 and ST40 of one frame) comes with MS4a |
| G-26 | invalid exact launch requests fail and never fall back | UB | with EXACT (B3, else MS2a); the checks beyond the binding's MS3 | exact launches in the past, closer than the lead and beyond the horizon: `-MTL_ERANGE` with the reason, never another pacing mode |
| G-28 | a late unit gets its policy's outcome and a result with margins | UB | MS3 | delay a submission across its deadline for each late policy and media mode |
| G-29 | close cannot race an active call or a completion into freed memory | U, UB | MS1 (ASan); TSan from MS2a | N threads in DP and WT calls while another thread closes |
| G-30 | stop and close wake blocked calls with `-MTL_ESHUTDOWN`, interrupt with `-MTL_ECANCELED`, ERROR with `-MTL_EIO` | U | MS1 | blocked acquire, dequeue and reap (event reads from MS3) return within a bound with the documented code on stop, interrupt, close and ERROR |
| G-31 | discard, DRAIN and FLUSH leave every accepted unit with a terminal outcome | U, UB | MS1 (discard MS2a) | discard, DRAIN and FLUSH with units at every stage; stop racing submit |
| G-32 | a failed create leaves no session, flow, quota or handle | U, I | MS1 | inject a failure at each create step: `capacity.*` and `port.free_*` return to baseline |
| G-33 | the library reads `min(struct_size, known)` and rejects unknown non-zero bytes (`NONZERO_TAIL`) | U | MS1 | compile against an older and a newer header copy; a non-zero tail or reserved field is `NONZERO_TAIL` |
| G-40 | reading stats never blocks or delays a tasklet | UB | MS3 | lock instrumentation: no stats read takes a lock a tasklet takes |
| G-41 | every state event has a getter that is right after an event overflow | U | MS3 | overflow a session's events, then compare every getter with the true state |
| G-42 | every counter is monotonic for the life of the session; no call resets it | U | MS1 with A2b, else MS2a | two readers with different periods over a randomised session life |
| G-43 | gauges satisfy entries − exits = gauge at quiescent points | U, UB | MS3 | exact at ≥ 10^4 quiescent points (test clock paused, application threads parked); between them `abs(entries − exits − gauge)` ≤ the number of completing contexts |
| G-46 | a TX verb on an RX session (and the reverse) is `-MTL_EINVAL` and changes nothing | U | MS1 | every TX verb on an RX session and the reverse |
| G-47 | the cookie comes back verbatim; a cookie without results is `COOKIE_WITHOUT_RESULTS` | U | MS1 | random cookies, and a cookie on a session without results |
| G-49 | every call returns the documented code in every state; a closing handle accepts only close (which polls), `mtl_session_get_status`, the release of a lease taken before the close and interrupt (a no-op), and a retired one reads `MTL_STATE_RETIRED` | U | MS1 | the state × call table, table-driven on `null:1`; ERROR reached with `MTL_FAULT_FORCE_ERROR` |
| G-50 | every function has one call class, enforced in debug builds | B, U | MS3 | header lint; the debug asserts under the U suite; a signal raised inside every CP call under `malloc` and `pthread_mutex_lock` interposers |
| G-51 | libmtl exports the unified API in `MTL_UNIFIED_EXPERIMENTAL_<rev>`: the functions tagged with milestones ≤ the current one (a subset during a milestone); from MS3 a soname, `MTL_LEGACY` and `local: *` | B | MS1, MS3, MS7 (`MTL_1.0`) | `nm -D --defined-only`, `readelf -V` and the milestone tags of the headers |
| G-52 | "drain until `-MTL_EAGAIN`, then sleep on the wait handle" never misses a wake-up (R2) | U | MS1 | a two-thread litmus test with forced interleavings of the arming protocol ([engine.md](engine.md) §7.1) |
| G-53 | submitting before the hint's deadline is ON_TIME in the hinted slot | U, UB | MS3 | submit just before `submit_deadline_tai_ns` of `mtl_tx_next_slot`: `ON_TIME` in the hinted slot |
| G-56 | RX leases released out of order or from other threads return to the pool | U | MS1 | release RX leases out of order and from other threads |
| G-57 | each status, reason and code is produced by its documented trigger | U, UB | MS3 | a table of triggers on `null:1`, through `mtl_debug_inject` where no natural trigger exists |
| G-64 | interrupt is sticky until cleared (`mtl_interrupt(o, MTL_INTR_OFF)`), never wakes another session, and with a target mask wakes only those targets | U | MS1 | the GStreamer `unlock`/`unlock_stop` sequence with acquire interrupted and the reaper not spinning; session and instance interrupts combined |
| G-65 | close with leases out returns 1; releases still work; a repeated close polls (0 or 1) and, with a timeout, waits for retirement | U | MS1 | close with leases out, close again with timeout 0 (1), release them later, close again (0) |
| G-70 | each `MTL_E*` code has one meaning; RX dequeue with nothing ready returns `-MTL_EAGAIN` in CREATED, ARMED, RUNNING and STOPPED, never `-MTL_ESHUTDOWN` | U | MS1 | each code from its triggering call and state, including dequeue on a session never started |
| G-71 | the null handle fails with `-MTL_EBADF`, except every close, which returns 0 | U | MS1 | the null handle to every function, table-driven over the headers |
| G-72 | `mtl_reap` writes `min(rec_size, native)` per record at pitch `rec_size`; the typed wrappers pass `sizeof(*r)` | U | MS1 | record sizes larger and smaller than the library's, with `MTL_INIT` hoisted out of the loop |
| G-73 | a zero-filled input struct (`MTL_INIT`) is the default configuration | B, U | MS1 | a zero-filled struct opens with the documented defaults; a lint lists every field whose zero means something else |
| G-74 | headers and examples compile (`check.sh`) | B | MS1 | `check.sh` in CI, also with `-pedantic` and clang, plus a compile-only Windows job; candidate flags `-Wconversion -Wsign-conversion -Wcast-qual` (`-Wcast-qual` catches an accessor such as `mtl_pkt_tx_table()` returning a writable table from a `const struct mtl_unit*`) |
| G-76 | RX `media_index` is the exact inverse of the TX rule (video, epoch timeline) | U, UB | MS3 | a sender on the grid at every video rate (1001 families, fields) across the 2^32 wrap, and a sender with a phase below one period, which maps to the slot it falls in; checked against the oracle |
| G-77 | closing a shared instance's reference that is not the last only drops it (returns 0); the last one shuts down; a CLOSING session stays readable through `mtl_session_get_status` | U | MS1 (exclusive open); MS2a (SHARED) | MS1: a second open without SHARED is `-MTL_EEXIST` (OI-49); close polls with a session in CLOSING. MS2a: two SHARED opens, closed in both orders |
| G-80 | every command is acked; a missing ack puts the session in ERROR with `CMD_TIMEOUT` | U, UB | MS2a | a TX unit waiting 1 s for its launch: stop is immediate and the unit `FLUSHED`; RX with no packets; with `MTL_INSTANCE_TASKLET_SLEEP`; a stalled scheduler |
| G-81 | close on a stalled queue completes (reset or quarantine) | UB (P), I (BE) | MS3 | `MTL_FAULT_TX_QUEUE_HANG`, then close: after retirement no descriptor references the session's memory |
| G-82 | an RX unit past its due time is force-completed within one scheduler iteration | UB | MS2a | feed half a frame, then stop sending |
| G-85 | TAI snapping NEAREST: collisions give `DUPLICATE_SLOT`, never a permanent drop | U, UB | MS3 | [timing.md §16.1](timing.md) |
| G-88 | names are copied, unique per instance, and listed exactly once | U | MS1 (the listing with A2b, else MS2a) | create and close churn with a concurrent `mtl_instance_list_sessions`; free the config's name buffer right after create |
| G-92 | the null backend is deterministic with the test clock | U | MS1 | completions run synchronously inside `MTL_FAULT_CLOCK_ADVANCE` on the caller's thread; run the suites twice and diff the result records byte by byte |
| G-93 | the debug API exists only with `-Denable_debug_api=true`; otherwise `-MTL_ENOTSUP` | U, B | MS1 | every fault reaches its outcome; in a release build `mtl_debug_inject` returns `-MTL_ENOTSUP` and `nm` shows no fault code |
| G-94 | a start that would strand queued units beyond the horizon fails atomically (`BEYOND_HORIZON`) | U | MS3 | [timing.md §16.1](timing.md) |
| G-95 | waiters are per target and never steal each other's wake-up | U | MS1 | one thread in `mtl_tx_acquire` and one in `mtl_tx_reap` on one session |
| G-99 | with legacy opt-in flags off, legacy wire output is identical to the baseline; on, it matches the oracle | UB, I | every engine change | per engine change, a pcap diff against the baseline inside the legacy gate |
| G-103 | `pool_count` above `max_count` fails at query and create with `-MTL_ERANGE`, `POOL_COUNT_MAX` | U | MS1 | `max_count` and `max_count + 1`, at query and at create |
| G-107 | `mtl_instance_close(mt, timeout_ns)` returns within `timeout_ns` and only 0, 1 or `-MTL_EIO` (`QUEUE_QUARANTINED`); `mtl_session_close` returns 0 or 1; only a DRAIN stop that missed its deadline returns `-MTL_ETIMEDOUT` | U, UB, I | MS1, MS3 | close with RUNNING sessions and leases out at timeouts 0, 10 ms, 1 s (with `MTL_FAULT_TX_QUEUE_HANG` from MS3); check elapsed time, return set |
| G-108 | network first: TX stops at a unit boundary and RX leaves its groups before any port stops; `mtl_shutdown_report.groups_left` counts the leaves | U, I | MS3 | `mtl_instance_shutdown` with a report on `null:1`, the step order from the trace; at I a capture shows the IGMP leave before the device stops |
| G-109 | liveness never depends on packets or time lock: `MTL_FAULT_LEG_DOWN` and `MTL_FAULT_TIME_LOST` never clear `MTL_HEALTH_LIVENESS`; readiness reports the time loss (`MTL_HEALTH_TIME_UNLOCKED`); `mtl_instance_get_health` takes no lock | U | MS3 | inject each fault while another thread polls health in a tight loop under lock instrumentation |
| G-110 | `MTL_FAULT_MANAGER_LOST` posts `MTL_EVENT_MANAGER_LOST` and sets `MTL_HEALTH_MANAGER_LOST`; running sessions continue and no call blocks (the create half is G-98, MS5) | U | MS3 | inject the fault with sessions RUNNING; results keep arriving ON_TIME; every call returns within its bound |
| G-111 | the AS calls (`mtl_instance_interrupt`, `mtl_instance_abort`) are safe at any time, during and after close, and never write a recycled descriptor; in a forked child every call but close fails with `-MTL_EBADF` (`FORKED`) | U | MS1 (the AS calls), MS3 (the fork rule) | a signal storm of the AS calls across open, close and re-open; `fork()`, then each function in the child |
| G-112 | re-open: after a close, `mtl_instance_open` works again in the same process (EAL stays initialised); after a SIGKILL mid-stream, a new process opens the same VF without manual cleanup | U, I | MS1 (U), MS3 (I, wave 4) | 200 open and close cycles on `null:1` under ASan; a NoCtx case kills its child mid-stream and re-opens the VF |
| G-113 | rows: a TX rows unit re-submitted with a growing `used` sends each row once it is ready, never beyond `used`; RX reports rows in order through `mtl_rx_wait_rows`; a complete rows unit equals the frame unit | UB, I | MS2a | the slice cases of `st20_digest.cpp` ported: rows released in steps, SHA-256 of the received frame, no packet of a row before its submit |

G-45 (the same verb sequence for every essence × direction) holds for video only until MS4b.
G-39 (no syscall, no app-holdable lock and no allocation on the tasklet side of the PMD
backend; the deferred eventfd writes of the scheduler loop run after the handler loop, outside
every tasklet; on the application side, a data call makes no syscall except the non-blocking
read that drains an armed wait handle, R6) and G-38 (no public function on a tasklet or library
loop, by a debug-build assert) are measured in MS1 and become P in MS4b. G-97 is "every example
compiles, and the ST20 ones run on `null:1`" (ex01, ex02, ex03 and ex05 from MS1; ex11 from
MS3).

Later milestones: G-10…G-16, G-35…G-37, G-60, G-78, G-83, G-84, G-96, G-100…G-102 (MS2b for
video); G-27, G-34 (MS4a, MS4b); G-75, G-87, G-89…G-91, G-98 (MS5); G-17, G-18, G-21, G-22
(with TLINE/2), G-23…G-25, G-59 at M, G-61, G-62, G-66…G-68, G-86, G-104, G-105, and G-44,
G-54, G-55, G-58 at I (MS6); G-63 with created timelines (not in v1). Two of them are defined
nowhere else in the maintained set:

- G-34 (MS4b, I, BE until the per-NIC capability job runs): every advertised capability (`caps.*`)
  passes its conformance test.
- G-54 (MS6, UB with `MTL_FAULT_TIME_LOST`): the time state leaves LOCKED on sync loss and
  `MTL_EVENT_TIME_STATE` is posted (pins SF-20).

### 8.3 Fault injection

Faults are injected with `mtl_debug_inject(obj, fault, &p)` in builds with
`-Denable_debug_api=true`, at the U and UB tiers. The I-tier steps run from
`.github/scripts/gtest.sh`. Today `script/nicctl.sh` offers `bind_kernel`, `create_vf`,
`create_kvf`, `create_tvf`, `disable_vf`, `bind_pmd` and `list`; `vf_link` (MS5) and
`vf_reset` (MS6) are new. Every G-01 and G-31 test runs under these faults.

| Fault | U/UB injection | I-tier step | Expected | Tested in |
|---|---|---|---|---|
| forced ERROR | `MTL_FAULT_FORCE_ERROR` (`p.reason`) | — | queued units FLUSHED (`SESSION_ERROR`), in flight FAILED once the device let go, waiters `-MTL_EIO` | MS1 (U) |
| TX queue hang, recovery | `MTL_FAULT_TX_QUEUE_HANG`: the transmitter's burst returns 0 until cleared | none: a real hang cannot be forced | `MTL_EVENT_RECOVERY` begin and end; in-flight units DROPPED (`RECOVERY`), never ON_TIME (the verdict is in MS1, task E1); `sh_info` untouched; close completes (G-81) | MS3 (UB); G-81 at I in MS6 |
| packet loss | `MTL_FAULT_DROP_PKTS` (pattern), `MTL_FAULT_DROP_RANDOM` (`drop_ppm`), per leg | — | `MTL_RX_INCOMPLETE`, zero fill in library pools, 2022-7 repair counted (`rx.units_used_redundancy`) | MS1 (`DROP_PKTS`, unit status); MS2a (zero fill, `DROP_RANDOM`; UB) |
| TX mutation | `MTL_FAULT_TX_MUTATE` (`MTL_TX_MUTATE_NO_MARKER`, `_SEQ_GAP`, `_BAD_PARITY`, `_PACED`) | — | the receiver counts and reports what it was fed; the legacy `st40_tx_test_config` cases | MS4a (ANC) |
| the application stops reading results | none | — | `MTL_BLOCKED_RESULTS`, `MTL_EVENT_BACKPRESSURE`, no loss | MS1 (U; the event MS3) |
| the application holds every RX slot | none | — | `rx.units_missed_pool_full`, `missed_before`, backpressure; `MTL_SESSION_RX_LATEST` reclaims unread units only | MS2a (U) |
| close mid-unit, with leases out, completions on another thread | none | — | G-01, G-65 (G-37 in MS2b) | MS1 (U, UB) |
| time step | `MTL_FAULT_TIME_STEP` (`p.step_ns`), `MTL_FAULT_CLOCK_ADVANCE` | `phc_ctl <if> adj Δ` on the lab host | media times are kept; the late policy applies; no permanent drop; `TIME_STEP` posted (G-62) from MS6 | MS3 (U); I BE, MS6 |
| time source lost | `MTL_FAULT_TIME_LOST` | stop `ptp4l` on the lab host | liveness unchanged, readiness `MTL_HEALTH_TIME_UNLOCKED` (G-109); `MTL_EVENT_TIME_STATE` to HOLDOVER (G-54) | MS3 (U, health); G-54 MS6 (UB); I BE |
| link down on one 2022-7 leg | `MTL_FAULT_LEG_DOWN` (`p.leg`); `MTL_FAULT_LEG_UP` restores | new `nicctl.sh vf_link <vf_bdf> down\|up` (PF side: `ip link set <pf> vf <n> state disable\|enable`) | `MTL_EVENT_LEG_STATE` (oper down); units flow on the other leg, results `ON_TIME` | MS3 (U, liveness only, G-109); MS5 (U, UB, I) |
| link down on the only leg | `MTL_FAULT_LEG_DOWN` | as above | `MTL_TX_DROPPED` with `LINK_DOWN`; the session stays RUNNING | MS5 (U, UB, I) |
| VF reset | `MTL_FAULT_PORT_RESET` on `MTL_OBJ_OF_PORT(mt, p)` | new `nicctl.sh vf_reset <vf_bdf>` (`/sys/bus/pci/devices/<bdf>/reset`) | `MTL_EVENT_PORT_RESET`, sessions `MTL_EVENT_RECOVERY`; gap units `MTL_TX_FAILED` with `PORT_RESET` | MS3 (UB, EK14); I BE until MS6 |
| device gone | `MTL_FAULT_PORT_RESET` with `p.unrecoverable = 1` | `nicctl.sh disable_vf` | `MTL_EVENT_PORT_REMOVED`; ERROR with `DEVICE_GONE` (or a degraded leg); `-MTL_ENODEV`; start fails until resources can be re-reserved | MS3 (U, EK14) |
| MtlManager dies | `MTL_FAULT_MANAGER_LOST` | `gtest.sh` kills MtlManager, runs creates, restarts it | `MTL_EVENT_MANAGER_LOST`, health `MTL_HEALTH_MANAGER_LOST`; running sessions continue (G-110); a create that needs an lcore gets `-MTL_EBUSY` with `MANAGER_LOST` (G-98) | MS3 (U, G-110); MS5 (G-98, U and I) |
| process killed | none | a NoCtx case kills its child with SIGKILL mid-stream, then re-opens the VF | open succeeds without manual cleanup (G-112) | MS3, wave 4 (I) |
| scheduler overload | not injectable | a load test | `MTL_EVENT_SCHED_OVERLOAD` | BE, MS6 |

### 8.4 Performance budgets

Budgets are set before any code, so that no milestone sets its own gate. S0 measures today's
baseline with `MTL_FLAG_TASKLET_TIME_MEASURE` (`include/mtl_api.h:449`) on the reference
machine and confirms or revises them in MS1 week 1; a revision is recorded in
[decisions.md](decisions.md). Today's measurement reports the average and the maximum of a
tasklet iteration (`mt_sch.c:466-468`), so the MS1 budgets are in avg and max; the maximum is
the tail that damages pacing.

| Metric | Budget | Measured by |
|---|---|---|
| tasklet iteration per scheduler, same load as legacy | avg ≤ legacy + 2 %, max ≤ legacy + 5 % | `MTL_FLAG_TASKLET_TIME_MEASURE` (RxTxApp `--tasklet_time`), I, 10 min |
| DP call cost without conversion (acquire, submit, reap, dequeue) | p50 ≤ 150 ns, p99 ≤ 1 µs | U micro-benchmark on `null:1` (the descriptor ring's gate in C1b), plus an I spot check; DPC calls reported separately |
| completion latency (last packet handed, or last mbuf freed, until the result is visible) | ≤ the reported `completion_latency_ns` (`mtl_buffer_requirements`, key `info.completion_latency_ns`) + 10 % | UB, I |
| deferred wake cost | the eventfd writes of the scheduler loop at 100 armed sessions stay inside the tasklet iteration budget above | S1, then I |
| W2 wake latency | p99 ≤ 10 µs | S1 |
| W3 wake latency, only if W3 is built | p99 ≤ unit period / 10 for units ≥ 1 ms | S1 |
| sessions per scheduler at 1080p59.94 | unchanged versus legacy | I |
| ST 2110-21 narrow compliance under stress | no regression versus legacy, for TSC and RL pacing, with a stats reader in a tight loop and 64 armed waiters (timing parser with NIC timestamps) | I; measured in MS1, gating MS2a (§6.9) |

### 8.5 The legacy gate

Every milestone and every engine change passes:

- the legacy KahawaiTest suite (mandatory level, default pacing and `--pacing_way tsc`, as
  `gtest.sh` runs it) and the acceptance smoke suite with legacy defaults;
- `./build.sh unit`, including the unit tests that pin today's wire behaviour. E1, E2 and E3
  change the code they pin; with the legacy opt-in flags off they stay green unchanged, and flag-on
  behaviour gets new tests:

  | Test | Pins |
  |---|---|
  | `tests/unit/session/st20_tx/rtp_timestamp_rounding_test.cpp` | today's RTP rounding (`st10_tai_to_media_clk`, round to nearest, ties down; `floor` is opt-in, E2) |
  | `tests/unit/session/st20_tx/epoch_test.cpp` | the epoch and onward-resync math of `calc_frame_count_since_epoch()`, the late-frame drop path |
  | `tests/unit/session/st20_tx/interlaced_field_epoch_test.cpp` | the field grid of interlaced TX: even slots first fields, odd slots second fields |
  | `tests/unit/session/st20_tx/pacing_test.cpp` | the cursor math of `tv_sync_pacing()` and the RTP of `tv_update_rtp_time_stamp()` across USER_TIMESTAMP, RTP_TIMESTAMP_EPOCH and `rtp_timestamp_delta_us` |
  | `tests/unit/session/st20_tx/rtp_gate_snap_test.cpp` | RTP quantisation and the exact-user-pacing gate |
  | `tests/unit/session/st20_tx/rl_warm_up_test.cpp` | the RL warm-up |
  | `tests/unit/session/st30_tx/pacing_test.cpp` | the ST30 RTP under USER_TIMESTAMP and USER_PACING |
  | `tests/unit/session/multi_essence_sync_test.cpp` | ST20, ST30 and ST40 resolve one USER_TIMESTAMP to one TAI instant |
  | `tests/unit/ptp/` | the built-in PTP client, including the software time base of a port without timesync (`no_timesync`) |

- a pcap diff against the pre-change baseline for wire-visible changes (G-99);
- from MS2a, when the RxTxApp default switches, the nightly-pytest `ptp` group, which runs
  built-in PTP on VFs (§2.4).

### 8.6 The timing oracle

Expected values are computed from the anchor with exact arithmetic, every packet is checked
(not only packet 0), and anchors are never inferred from the capture. The method comes from
`doc/user-pacing-timestamp-contract.md` (a separate draft, not part of this baseline), whose selection rules this design
replaces in places; the contract needs that update before an engine change alters the wire.
The comparison table, with the question behind each difference, is in [timing.md](timing.md)
§16.2.

## 9. Risks

| Risk | Milestone | Mitigation |
|---|---|---|
| the core's slot model misses a unit kind (rows, packets, holds) | MS1 checkpoint 1 | the slot word, generation, `seq`, `progress`, result and hold count are in C0 and reviewed against §2.2 |
| the bindings run on the tasklet under the session spinlock; a slow binding hurts every session on the scheduler | every milestone | wait-free rules for bindings (§2.1), the S0 budgets (§8.4), `mtl-reviewer` checks the binding diffs against the two-world rule |
| re-basing st20p changes legacy behaviour (FREE before `notify_frame_done`, `st20_pipeline_tx.c:265-286`; done-flag order differs between st20p and st30p) | MS2b, MS4a, MS4b | the re-base comes last in MS2b, after the nightly burn-in; the legacy suites and T1's parity tests stay green; a behaviour that must differ is a per-session mode of the core, listed in the commit |
| the deferred eventfd writes add jitter on isolated cores with many armed waiters | MS2a | S1; W3 drains the same bitmap if it has to be built |
| TX completion latency: on the chain path about `nb_tx_desc` packets, never while idle | MS1 | idle descriptor cleanup (task E1, S6) |
| lost wake-ups from wrong fences | MS1 | seq_cst on both sides; the litmus test G-52 |
| acceptance log greps | MS1 | D-110 as an exit criterion |
| one instance, two APIs: teardown order, SP-01, behaviour before `mtl_start()` | MS1 | the wrapper closes first; bridge tests at U and I; RxTxApp already calls `mtl_start` before it creates sessions (`rxtx_app.c:480`, `:534`); OI-2 |
| the `MTL_LEGACY` node and `local: *` break consumers of leaked internal symbols | MS3 | MS1 leaves every symbol libmtl exports today exported; an `nm` audit first, the soname and `MTL_LEGACY` next, `local: *` as its own change |
| header churn while tasks run against the headers | MS1–MS3 | header changes batched weekly, and each one only after the maintainer's yes (§5.8) |
| the comparison window lengthens the nightly | MS2a | ported cases in their own shards; the pull-request baseline grows by two cases |
| wire defaults differ between the APIs by design (epoch RTP, incomplete delivery) | MS1–MS2a | the comparison sets the equivalent legacy flags (§6.9) |
| a ported feature regresses: built-in PTP on a VF | MS1–MS2a | `PTP_BUILTIN` on a VF keeps the software time base (§2.4); `tests/unit/ptp/` in the legacy gate, the `ptp` group from MS2a |
| review load: one maintainer reviews AI-written code at a high rate | all | tasks ≤ 1.5 k lines, `mtl-reviewer` before the maintainer, the WIP limits and the gates of §5.2 and §5.8; CODEOWNERS with a second reviewer for `lib/src/st2110/core/` |
| CI capacity for NIC tiers and fault steps | MS2a on | U tier on ordinary runners; NIC jobs nightly; a serialised runner for fault steps |
| scope creep within a milestone | all | a milestone ends on its exit criteria; anything else moves to the next one |
| private users depend on behaviour not visible in-tree | MS7 | the private-user questionnaire and the external review before the freeze (MS7) |

## 10. Calendar and effort

AI agents write the code (A1), so the pace is set by review and by the hardware gates, not by
writing. The calendar of §1.2 assumes:

- three to four tasks a week through `mtl-reviewer` and the maintainer, each ≤ 1.5 k changed
  lines, within the WIP limits of §5.8;
- Gate 6 runs on one E810 host for the data-plane tasks, and on E830 and E835 for milestone exits;
- one milestone at a time on the critical path, with spikes, test work and the Kubernetes track in
  parallel.

| Milestone | Tasks [I] | Changed lines [I] | Weeks [I] |
|---|---|---|---|
| MS1 | 18, plus 3 stretch | about 11 k (5 k library) | 4 |
| MS2a | about 9 | about 7 k | 4 |
| MS2b | about 7 | about 6 k | 3–4 |
| MS3 | about 12 | about 9 k | 4 |
| MS4a | about 10 | about 8 k | 4 |
| MS4b | about 10 | about 7 k | 4 |
| MS5 | about 14 | about 10 k | 4–5 |
| MS6 | about 16 | about 12 k | 6–8 |
| MS7 | about 6 | about 4 k, then releases | 2–4, then the deprecation releases |
| Kubernetes track | about 15, beside MS1–MS3 | about 5 k | — |

What keeps the total small beyond the agents' speed: one core under every API with the
pipelines as wrappers on it and no re-base at the end (D-99), one library with version nodes
(D-23), ports that replace the legacy cases instead of twins (D-109), and exports that grow
with the code, so a later milestone adds functions to the node and never changes one (D-106).

## 11. Where the detail lives

- the architecture picture and the parts of the design: [engine.md](engine.md) §1; the engine
  change list: [engine.md](engine.md) §11;
- what each spike measures: [engine.md](engine.md) §11.1;
- the goals, non-goals and personas, and the full text and sources of each requirement:
  [requirements.md](requirements.md);
- the notes behind every guarantee's test method: [requirements.md](requirements.md),
  [timing.md](timing.md) §16.1; the oracle compared with the contract document:
  [timing.md](timing.md) §16.2;
- the row-by-row inventory, each feature's milestone, and the features reachable only through the
  session headers: [coverage.md](coverage.md); the feature × media matrix of today:
  [legacy-internals.md](legacy-internals.md);
- the function counts per header, per call class and per milestone: `sketch/check.sh`;
- the open items Phase 7 inherits from the IPMX verification: [nmos-ipmx.md](nmos-ipmx.md) §16;
- the decisions this plan rests on (D-99…D-112): [decisions.md](decisions.md);
- the state of MS1: `doc/unified-api/ms1-status.md` (§5.8).
