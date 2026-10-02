# 10 — Rivermax API in depth, and how it maps to MTL

| | |
|---|---|
| Topic | NVIDIA Rivermax public C API (`rmx_*`), its Dev Kit, and how real integrators use it; mapping to MTL and to the unified API |
| Date | 2026-09-29 |
| Author | research agent (Rivermax track) |
| Status | Input for the unified-API design. Knowledge only, no implementation. |

**Sources.** Everything below was read from source code, not from the licensed Rivermax User Manual (not publicly reachable; see "Source access").

| Tag | Source | Pinned revision |
|---|---|---|
| RE | [NVIDIA/rivermax-examples](https://github.com/NVIDIA/rivermax-examples), `api_demo/` and `legacy/` | `4dca2694` (2026-06-11) |
| RDK | [NVIDIA/rivermax-dev-kit](https://github.com/NVIDIA/rivermax-dev-kit), `source/` | `ebbb89e4` (2026-04-23) |
| DS | [NVIDIA/DeepStream](https://github.com/NVIDIA/DeepStream) `src/gst-plugins/gst-nvdsudp/` (new `rmx_*` and `legacy_api/` `rmax_*`) | `0a63ef8b` |
| UE | Unreal Engine `Plugins/VirtualProduction/Rivermax/RivermaxCore`, read through the public third-party mirror [orgitcog/u9n](https://github.com/orgitcog/u9n) | `97a2175e` |
| DOCA | [DOCA Rivermax programming guide](https://docs.nvidia.com/doca/sdk/doca-rivermax/index.html) (DOCA 3.5.0 page, "last updated 2026-09-01") | web |
| WEB | [Rivermax product page](https://developer.nvidia.com/networking/rivermax), [Rivermax FAQ](https://developer.nvidia.com/networking/rivermax/faq) | web |

Short-hands used in citations: `RE:api_demo/output_media/media_send/media_send.cpp:125` means file:line at the pinned revision. RDK paths are relative to `source/`. UE paths are relative to `.../RivermaxCore/Source/RivermaxCore/Private/`.

**Source access.** `rivermax_api.h` is not vendored anywhere public. The closest thing to a public prototype list is UE's generated `RivermaxWrapper.h` (a function-pointer table of 131 `rmx_*` entry
points with full signatures), used below as the prototype reference. `docs.nvidia.com/networking/display/rivermax` returns 404 (redirects to a 404 on `networking-docs.nvidia.com`); the Rivermax User
Manual needs a developer login. The Holoscan advanced-network Rivermax manager was not found in `nvidia-holoscan/holohub` (moved or removed) and was not read. DeepStream's README pins Rivermax 1.80.x
as the current SDK. The UE mirror is an unofficial copy of Epic source; treat it as evidence of how a large integrator uses the API, not as NVIDIA documentation.

**Labels.** **[verified]** = seen in cited code/doc. **[inferred]** = my reading of verified code, not stated anywhere. **[unknown]** = not determinable from public sources.

---

## 1. API generations, object model, context

### 1.1 Two generations

| Generation | Shape | Evidence |
|---|---|---|
| Legacy `rmax_*` | `rmax_init(&cfg)`, `rmax_out_create_stream_ex`, `rmax_out_get_next_chunk(id, &payload, &hdr)`, **stream-level** `rmax_out_commit(id, time, flags)`, `rmax_in_create_stream`, `rmax_in_get_next_chunk(id, min, max, timeout, flags, &comp)`, `rmax_set_clock`, `RMAX_ERR_*` | **[verified]** DS `legacy_api/gstnvdsudpsink.c:1214-2242`, `legacy_api/gstnvdsudpsrc.c:1347-2940` |
| Current `rmx_*` | Builder style: app-allocated param structs initialised by `rmx_*_init*()`, populated with `rmx_*_set_*()`, then `rmx_*_create_stream()`; chunk *handles* (`rmx_output_media_chunk_handle`, `rmx_input_chunk_handle`) owned by the app; `RMX_*` status codes | **[verified]** all RE `api_demo/*` files; UE `RivermaxWrapper.h:12-142` |

Notable deltas: RX completion moderation moved from a per-call argument (`rmax_in_get_next_chunk(id, min, max, timeout, flags, ...)`) to a per-stream runtime setting
(`rmx_input_set_completion_moderation(id, min, max, timeout_usec)`) **[verified]** (DS legacy src `:2940` vs UE `RivermaxWrapper.h:78`). The legacy TX commit was stream-level
(`rmax_out_commit(stream_id, ...)`), which I believe is the origin of today's "commit sends the oldest acquired chunk" rule (§2.6) **[inferred]**.

### 1.2 ABI style

- Exported symbols are version-suffixed: `rmx_output_media_commit_chunk_v1`, `rmx_get_version_string_v1`, ...; init is the un-suffixed `_rmx_init(const rmx_version* policy)` **[verified]** UE
  `RivermaxWrapper.cpp` (`GetDllExport(... TEXT("rmx_..._v1"))`), `RivermaxWrapper.h:21`. So `rmx_init()` in examples is presumably a header macro that passes the compile-time
  `RMX_VERSION_MAJOR/MINOR/PATCH` as a compatibility policy **[inferred]** (RE `api_demo/output_media/media_send/media_send.cpp:48`; RDK `facade.cpp:96-97` prints `RMX_VERSION_*` next to
  `rmx_get_version_string()`).
- Many accessors are **inline in the header**, not exported: UE calls `rmx_output_media_get_chunk_strides()`, `rmx_input_get_completion_ptr()`, `rmx_input_get_packet_size()`,
  `rmx_input_get_packet_timestamp()` directly while every other call goes through its function table **[verified]** UE `Streams/RivermaxOutStream.cpp:431`, `Streams/RivermaxInputStream.cpp:318-348`. So
  handle/completion structs have a public, fixed layout even though the docs treat them as opaque **[inferred]**. UE even casts `rmx_output_chunk_completion*` to `rmx_output_chunk_completion_metadata*`
  to read `user_token` and `timestamp` (`RivermaxOutStream.cpp:1371-1372`) **[verified]**.
- Lesson for MTL: Rivermax gets ABI stability from *caller-allocated, init-function-initialised* parameter blocks plus versioned exports, not from opaque heap objects. That is directly relevant to review §4.7 / survey §4.1 (ABI fragility).

### 1.3 Library lifecycle and device selection

| Call | Semantics | Label |
|---|---|---|
| `rmx_set_cpu_affinity(mask, core_count)` + helper `rmx_mark_cpu_for_affinity(mask, cpu)` | Pins the **Rivermax internal thread**. Must be called **before** `rmx_init` | **[verified]** RDK `services/cpu/cpu_utils.cpp:28-48`, `facade.cpp:149-153`; DOCA "Optional Configurations: CPU affinity for Rivermax internal thread. Should be done before library initialization" |
| `rmx_enable_system_signal_handling()` | Before `rmx_init`; afterwards blocking/polling calls return `RMX_SIGNAL` on SIGINT etc. | **[verified]** RDK `facade.cpp:157-163`; `core/chunk/media_chunk.cpp:53-54` |
| `rmx_init()` / `rmx_cleanup()` | Process-global; the Dev Kit ref-counts a single init via `shared_ptr` | **[verified]** RDK `facade.cpp:110-139` |
| `rmx_apply_lib_param(&p)` with `rmx_init_lib_param`, `rmx_set_lib_param_name/value/forced` | String `name=value` tunables; unknown name returns `RMX_NOT_IMPLEMENTED`, bad value `RMX_INVALID_PARAM_2` | **[verified]** RDK `services/legacy_util/rt_threads.cpp:659-695`. Parameter names **[unknown]** |
| `rmx_get_version_string()`, `rmx_get_version_numbers()` | | **[verified]** UE `RivermaxWrapper.h:12-13` |
| `rmx_get_device_list`, `rmx_get_device_count/get_device`, `rmx_get_device_interface_name/ip_count/ip_address/mac_address/id/serial_number`, `rmx_free_device_list` | Enumeration | **[verified]** UE `RivermaxWrapper.h:23-32`; RDK `apps/ipmx_sender/ipmx_sender.cpp:228-265` |
| `rmx_retrieve_device_iface(&iface, &rmx_ip_addr)` / `rmx_retrieve_device_iface_ipv4(&iface, &in_addr)` | **Device selection is by local IP address**, not PCI address | **[verified]** RE `api_demo/output_media/memory_registration_media_send/memory_registration_media_send.cpp` ("Retrieve device interface"), UE `Streams/RivermaxInputStream.cpp:1149` |
| `rmx_enquire_device_capabilities(iface, &caps)`, `rmx_apply_device_config` / `rmx_revert_device_config` | Capability query and device config | **[verified]** prototypes only, UE `RivermaxWrapper.h:34-36`. Contents of `rmx_device_capabilities` **[unknown]**; DOCA says per-device "PTP clock support" is a capability |

Streams bind to a NIC by *local address*: input via `rmx_input_set_stream_nic_address(sockaddr)`, generic output via `rmx_output_gen_set_local_addr`, media output via the **source address inside the
SDP** **[verified]** RE `api_demo/input/receive/receive.cpp:56`, `api_demo/output_generic/generic_send/generic_send.cpp:113`, RDK `services/media/video_settings_calculator.cpp` (`generate_media_sdp`
puts `source_ip` into `SessionDescription` and `a=source-filter`).

### 1.4 Object model

```text
process ── rmx_init ── [internal thread, pinned by rmx_set_cpu_affinity]
   │
   ├─ clock (system | user callback | NIC PTP)               rmx_use_*_clock
   ├─ memory registrations (addr,len) x device -> mkey        rmx_register_memory
   ├─ output media stream  (SDP + mem blocks)                 rmx_output_media_*   ── chunk handle
   ├─ output generic stream (local/remote addr, rate)         rmx_output_gen_*     ── chunk handle
   ├─ input stream (NIC addr, ring capacity, sub-blocks)      rmx_input_*          ── chunk handle
   │      └─ flows (dst/src 2- or 4-tuple, flow tag) attach/detach at runtime
   ├─ event channel per stream (fd / IOCP) + notifications    rmx_establish_event_channel
   └─ stats consumer (out-of-band, by process id)             rmx_stats_*
```

Stream IDs are plain `rmx_stream_id` values **[verified]**. There is no "session" object that groups TX+RX or audio+video; A/V alignment is entirely the application's job (common start time, see §10) **[verified]** RDK `io_node/senders/media_sender_io_node.cpp:361-449` (`coordinate_start_time` via an app-side `ISynchronizer`).

---

## 2. Output media stream (the ST 2110 sender)

### 2.1 Creation is SDP-driven

```c
rmx_output_media_init(&p);
rmx_output_media_assign_mem_blocks(&p, blocks, n_blocks);
rmx_output_media_set_sdp(&p, sdp_text);              /* whole SDP text */
rmx_output_media_set_idx_in_sdp(&p, media_block_idx); /* which m= line */
rmx_output_media_set_packets_per_chunk(&p, ppc);
rmx_output_media_set_stride_size(&p, sub_block_id, stride);
rmx_output_media_set_packets_per_frame(&p, ppf);
/* optional */ rmx_output_media_set_pcp/dscp/ecn(&p, v);
/* optional */ rmx_output_media_set_source_ports(&p, ports, count);
/* optional */ rmx_output_media_set_tx_adaptive_scheduling_factor(&p, double);
rmx_output_media_create_stream(&p, &stream_id);
```

**[verified]** RE `api_demo/output_media/media_send/media_send.cpp:68-84`; RDK `core/stream/send/media_stream.cpp:53-106`; DS `gstnvdsudpsink.c:1337-1360`; UE `RivermaxWrapper.h:84-101` (prototypes for `set_source_ports`, `set_tx_adaptive_scheduling_factor`). Semantics of `tx_adaptive_scheduling_factor` **[unknown]**.

- The SDP is the *single source* of destination IP/port, source IP (hence NIC), media type, format, frame rate, and 2110-21 sender type. The Dev Kit generates it (`a=fmtp:... sampling=; width=; height=;
  exactframerate=; depth=; TCS; colorimetry; PM; SSN; TP=2110TPN|TPNL|TPW`, `a=ts-refclk`, `a=source-filter`, `a=mid`/group for multi-flow) **[verified]** RDK
  `services/media/video_settings_calculator.cpp` (`generate_media_sdp`), `services/sdp/sdp_smpte_2110_20_description.cpp:42`, `services/sdp/include/rdk/services/sdp/sdp_defs.h:198-202`.
- After creation the app can read back the resolved addresses: `rmx_output_media_init_context(&ctx, id)`, `rmx_output_media_set_context_block(&ctx, sdp_idx)`, `rmx_output_media_get_local_address / get_remote_address` **[verified]** UE `Streams/RivermaxOutStream.cpp:194-206`.
- Runtime updates exist only for QoS: `rmx_output_update_dscp`, `rmx_output_update_ecn`, and `rmx_output_get_chunk_count` **[verified]** UE `RivermaxWrapper.h:140-142`. There is no public destination-update for *media* streams (generic has `rmx_output_gen_update_remote_addr`, §3) **[verified by absence in the 131-entry table; inferred as "not supported"]**.
- Which SDP attributes Rivermax actually parses (e.g. whether `TP=` selects narrow vs wide pacing, whether `exactframerate` drives the rate) **[unknown]**. That it needs `packets_per_frame` next to the SDP suggests it derives the per-packet rate from frame period / packets per frame **[inferred]**.

### 2.2 Memory hierarchy: block → sub-block → chunk → stride

```text
stream
 └─ mem block[0..B)          rmx_output_media_mem_block, init by rmx_output_media_init_mem_blocks()
     ├─ chunk count          rmx_output_media_set_chunk_count(block, C)
     ├─ sub-block count      rmx_output_media_set_sub_block_count(block, 1 | 2)   1 = contiguous, 2 = header/data split
     ├─ packet layout        rmx_output_media_set_packet_layout(block, sb, uint16_t sizes[C*ppc])
     └─ sub-block memory     rmx_mem_region* = rmx_output_media_get_sub_block(block, sb)            {addr, length, mkey}
                             rmx_mem_multi_key_region* = rmx_output_media_get_dup_sub_block(block, sb)  {addr, length, mkey[]} (2022-7)
chunk  = packets_per_chunk consecutive strides
stride = fixed-size slot per packet per sub-block (rmx_output_media_set_stride_size(params, sb, bytes))
```

**[verified]** RE `media_send.cpp:52-63`, `hds_media_send/hds_media_send.cpp:52-69`; RDK `core/stream/send/media_stream.cpp:108-173`.

- A block is a set of whole chunks; the Dev Kit sizes one block to hold `media_units_in_mem_block` frames (default 10 for `media_sender`) and uses a single block **[verified]** RDK
  `apps/media_sender/include/rdk/apps/media_sender/media_sender.h:52`, `io_node/senders/include/rdk/io_node/senders/media_sender_io_node.h:255`. UE also defaults to a single memblock "potentially
  improving SDK performance" **[verified]** UE `Streams/RivermaxOutStream.cpp` CVar `Rivermax.Output.UseSingleMemblock`.
- Strides are aligned up to cache line in all examples **[verified]** RE `media_send.cpp:71`; RDK `services/media/video_settings_calculator.cpp:209-210`. Hardware requirements on stride/alignment **[unknown]**.
- Header/data split (HDS): sub-block 0 = app header strides (RTP header, e.g. 20 B for 2110-20 single SRD), sub-block 1 = payload strides **[verified]** RE `hds_media_send.cpp:54-87`; RDK `media_stream.cpp:80-86`.
- **Dynamic packet sizes** (for ST 2110-40): skip `set_packet_layout`, call `rmx_output_media_set_chunk_packet_count(&handle, n)` *before* `get_next_chunk`, then write per-packet sizes into `uint16_t* rmx_output_media_get_chunk_packet_sizes(&handle, sb)` **[verified]** RE `dynamic_media_send/dynamic_media_send.cpp:70-74,138-163`; RDK `core/chunk/media_chunk.cpp:41-45`.

### 2.3 Chunk size choice

- Dev Kit default: **4 video lines per chunk** (`packets_in_chunk = 4 * packets_in_line`); a custom value must divide packets-per-frame **[verified]** RDK `services/media/video_settings_calculator.cpp:193-205`.
- Dev Kit packetisation: largest pgroup-aligned line fraction ≤ `MAX_PAYLOAD_SIZE = 1440` bytes, equal packets per line (1080p 4:2:2 10-bit → 4 packets/line, 1200 B payload + 20 B RTP/SRD header, 4320
  packets/frame, 16 packets/chunk) **[verified]** RDK `services/media/video_settings_calculator.cpp:150-178`, `services/media/include/rdk/services/media/media_settings.h:81` (numbers computed by me from
  that code).
- Chunk size trades CPU work per commit against how finely the app can be "just in time" and how soon memory is reusable **[inferred]**.

### 2.4 The data-path loop and the commit time

Canonical loop (identical across RE, RDK, DS, UE):

```c
for (;;) {                                  /* per frame */
  send_time = t0 + frame_idx * frame_period;
  for (c = 0; c < chunks_per_frame; ++c) {
    do s = rmx_output_media_get_next_chunk(&h); while (s == RMX_NO_FREE_CHUNK);
    p = rmx_output_media_get_chunk_strides(&h, sb);   /* write RTP hdr + payload per stride */
    t = (c == 0) ? send_time : 0;                      /* only first chunk of frame carries time */
    do s = rmx_output_media_commit_chunk(&h, t); while (s == RMX_HW_SEND_QUEUE_IS_FULL);
  }
}
```

**[verified]** RE `media_send.cpp:117-148`; RDK `io_node/senders/media_sender_io_node.cpp:519-553`; UE `Streams/RivermaxOutStream.cpp:472-535` ("Only first chunk gets scheduled with a timestamp. Following chunks are queued after it using 0").

| Question | Answer | Label |
|---|---|---|
| Unit | nanoseconds, `uint64_t` | **[verified]** all callers |
| Clock domain | Rivermax's configured clock (§6): system clock = **UTC**; `rmx_use_ptp_clock` = NIC PHC (**TAI**). DS/rivermax_player subtract leap seconds when not on PTP | **[verified]** DS `gstnvdsudpsink.c:697-722`; RE `legacy/rivermax_player/rivermax_player.cpp:1596-1604` ("TAI" vs "UTC" comment) |
| What `time` means | Scheduled wire time of the **first packet of the committed chunk**; subsequent chunks with `0` are paced back-to-back after it at the stream rate | **[verified]** semantics of `0`: RDK `io_node/senders/include/rdk/io_node/senders/media_sender_io_node.h:51` `SEND_IMMEDIATELY_AFTER_PENDING_CHUNKS_TIMESTAMP = 0`. "First packet" and "at the stream rate" **[inferred]** |
| Senders add TRO | App passes `alignment_point + TRO` (2110-21 §6.3 defaults, `tro = mult*T_frame - 2*T_RS`) | **[verified]** RDK `services/media/video_settings_calculator.cpp:86-130`, `media_sender_io_node.cpp:476-478`; UE `RivermaxOutStream.cpp:834-894`; DS `gstnvdsudpsink.c:730-770` |
| Too-late / too-soon time | Commit fails if the time is not far enough in the future; integrators clamp: if `time <= now + 600 ns` pass `0` | **[verified]** UE `RivermaxOutStream.cpp:497-503` ("...otherwise rmax_commit will throw an error"), CVar default 600 ns; RE `rivermax_player.cpp:1594`; DS `gstnvdsudpsink.c:2525-2531`. Status code **[unknown]** |
| Recovering timing | After a late frame UE calls `rmx_output_media_skip_chunks(&h, 0)` "to reset internal states. Otherwise, scheduling time / Tro isn't respected next time" | **[verified]** UE `RivermaxOutStream.cpp:600-609` |
| Is lateness reported? | No lateness status/callback observed; apps compare `send_time` with `rmx_get_time()` themselves and log | **[verified]** RDK `media_sender_io_node.h:424-442`; DS `gstnvdsudpsink.c:2376-2380` |

### 2.5 Who builds what in the packet

- **The application writes the RTP header** (V/P/X/CC/M/PT, 16-bit seq, timestamp, SSRC) and, for 2110-20, the extended sequence number and SRD headers, into the header stride (HDS) or the start of the
  payload stride **[verified]** RE `media_send.cpp:132-136` ("Fill the chunk with RTP headers and media payload"); RE `legacy/rivermax_player/rivermax_player.cpp:879-891`; RDK
  `services/ulp_packet_buffer/writers/rtp_smpte_2110_20_packet_buffer_writer.cpp:35-90` (marker on last packet, SRD offset/length, `timestamp += ticks_per_media_unit`, `sequence++`,
  `extended_sequence_number++`).
- **Rivermax builds Ethernet/IP/UDP** from the SDP (plus optional source ports, PCP/DSCP/ECN) **[inferred]**: no example touches L2-L4 on TX, and `set_pcp/dscp/ecn/source_ports` exist on the stream.
- The **RTP timestamp is not derived from the commit time** by Rivermax; apps compute it from their own frame time **[verified]** RDK `io_node/common/rtp_video_send_stream.cpp` `calculate_send_time_ns()` sets `rtp_timestamp = time_to_rtp_timestamp(first_packet_start_time_ns, sample_rate)`; UE `RivermaxOutStream.cpp:1160-1170`.

### 2.6 FIFO commit rule and chunk handle identity

"If multiple chunks were acquired with `get_next_chunk` for the stream after the previous call to `commit_chunk`, the **oldest acquired chunk will be sent**, not the one whose method was called."
**[verified]** RDK `core/chunk/include/rdk/core/chunk/media_chunk.h:115-131`, `core/stream/send/include/rdk/core/stream/send/media_stream.h:318-330`; same warning on generic chunks
(`generic_chunk.h:138`). UE: "We can't get chunks [early] since commit will only commit the chunks returned by last call to get next chunk" and instead computes future stride addresses itself from the
ring layout (`RivermaxOutStream.cpp:630-633`) **[verified]**. The ring is strictly sequential: `get_next_chunk` always returns the next ring position; to repeat or drop frames UE uses
`rmx_output_media_skip_chunks(&h, n)` (e.g. skip `(N_buffers-1) * chunks_per_frame` to land back on the frame just sent) **[verified]** UE `RivermaxOutStream.cpp:815-821,1177-1202`.

### 2.7 Pacing

- Pacing is done by the NIC ("Hardware-level packet pacing for smooth, consistent streaming across all data flows") **[verified]** WEB product page. The app thread never paces packets; it only submits chunks ahead of time **[verified]** by the loops above.
- The NIC PTP clock ("PTP clock time handler") needs "ConnectX-6 Dx or DPU devices only" **[verified]** RE `legacy/rivermax_player/rivermax_player.cpp:2738`. Which NIC generation is needed for time-based scheduling itself, and product names like "accurate TX scheduling" / "Rivermax media timing" **[unknown]** from public sources.
- UE observes that Rivermax does not report the last chunk of a frame as complete "until after the TRoffset gap", i.e. the library fills the inter-frame gap internally **[verified comment]** UE
  `RivermaxOutStream.cpp:482`; the stats API reports "Dummy wqes" on TX queues **[verified]** RDK `services/statistics/statistics_reader.cpp` (`rmx_stats_get_tx_queue_dummy_wqes`). Together: gaps are
  realised with dummy work-queue entries **[inferred]**.

### 2.8 Teardown

`rmx_output_media_cancel_unsent_chunks(&h)` returns acquired-but-uncommitted chunks, then `rmx_output_media_destroy_stream(id)` is retried while it returns `RMX_BUSY` (in-flight chunks) **[verified]** RDK `io_node/senders/media_sender_io_node.cpp:594-613`, `core/stream/send/media_stream.cpp:252-274`; UE `RivermaxOutStream.cpp:292-305`.

### 2.9 ST 2022-7 on TX

One media stream sends both legs: SDP with one `m=` block per path (`a=mid:a`, `a=mid:b`, group), memory registered once **per NIC** (one mkey per path), sub-blocks set with
`rmx_output_media_get_dup_sub_block()` → `rmx_mem_multi_key_region{addr,length,mkey[RMX_MAX_DUP_STREAMS]}` **[verified]** RDK `io_node/senders/media_sender_io_node.cpp:203-238`,
`apps/base_memory_strategy.cpp:129-150`, `facade.cpp:88-91`; DS `gstnvdsudpsink.c:1315-1334`. How `set_idx_in_sdp` and `set_source_ports` are meant to be used with several paths (DS calls
`set_idx_in_sdp` once per path in a loop) **[unknown]**.

---

## 3. Output generic stream

- Create: `rmx_output_gen_init_stream`, `set_local_addr`, optional fixed `set_remote_addr`, `set_packets_per_chunk` (max), optional `set_max_sub_blocks`, `set_pcp/dscp/ecn`, optional **rate**
  `rmx_output_gen_init_rate(&r, bps)` + `set_rate_max_burst(&r, packets)` + `set_rate_typical_packet_size(&r, bytes)` + `rmx_output_gen_set_rate(&p, &r)` **[verified]** RE
  `api_demo/output_generic/generic_send/generic_send.cpp:110-120`; RDK `core/stream/send/generic_stream.cpp:49-88`; UE `RivermaxWrapper.h:116-131`.
- Data path: `get_next_chunk`, then **scatter-gather append**: `rmx_output_gen_append_packet_to_chunk(&h, rmx_mem_region sub_blocks[], count)` per packet (each region has its own `mkey`; memory must be
  registered by the app), optional per-chunk destination `rmx_output_gen_set_chunk_remote_addr`, then `rmx_output_gen_commit_chunk(&h, time)` **[verified]** RE `generic_send.cpp:71-160`; RDK
  `core/chunk/generic_chunk.cpp:38-54`, `io_node/senders/generic_sender_io_node.cpp:221-236`.
- Runtime: `rmx_output_gen_update_rate`, `rmx_output_gen_update_remote_addr` **[verified]** UE `RivermaxWrapper.h:130-131`.
- Payload is "UDP payload"; Rivermax adds L2-L4 **[inferred]** from the address setters. Generic is *rate*-paced (token bucket in HW) rather than media-frame-paced **[inferred]** from the rate API; commit time semantics are the same as media (0 = ASAP) **[verified]** RE `generic_send.cpp:128,158`.

## 4. TX completion tracking

| Call | Semantics | Label |
|---|---|---|
| `rmx_output_media_mark_chunk_for_tracking(&h, uint64_t token)` | After `get_next_chunk`, before `commit` | **[verified]** RDK `core/chunk/include/rdk/core/chunk/media_chunk.h:143-151` |
| `rmx_output_media_poll_for_completion(&h)` | Non-blocking; `RMX_OK` = a tracked completion was fetched, `RMX_BUSY` = none yet. May need repeated calls because untracked completions ahead are drained first | **[verified]** RDK `core/chunk/media_chunk.cpp:141-154`, `media_chunk.h:153-165` |
| `rmx_output_media_get_last_completion(&h)` → `const rmx_output_chunk_completion*`; `rmx_output_get_completion_user_token(c)`, `rmx_output_get_completion_timestamp(c)` | Token and "time of completing the chunk transmit set by HW" | **[verified]** RDK `media_chunk.cpp:156-165`, `media_chunk.h:167-176` |
| Generic equivalents `rmx_output_gen_mark_chunk_for_tracking / poll_for_completion / get_last_completion` | same | **[verified]** RDK `core/chunk/generic_chunk.cpp:98-134` |

- The completion timestamp is in the same domain as `rmx_get_time()`: the Dev Kit latency tool subtracts the app send time from it **[verified]** RDK `io_node/misc/generic_latency_io_node.cpp:342-366`; that it is a HW wire timestamp is **[verified]** by the Dev Kit doc string "set by HW".
- Completions are in order; UE asserts token order and monotonic timestamps **[verified]** UE `RivermaxOutStream.cpp:1373-1381`.
- There is no *mandatory* completion: memory reuse is implied by `get_next_chunk` succeeding (ring position free) **[inferred]**; the app learns "safe to overwrite" only by being handed that chunk again. Status codes named `RMX_OUTPUT_MEDIA_...` do not exist in any public source **[verified by absence]**.

## 5. Input stream

### 5.1 Creation and memory

```c
rmx_input_init_stream(&p, RMX_INPUT_APP_PROTOCOL_PACKET /* or RMX_INPUT_RAW_PACKET */);
rmx_input_set_stream_nic_address(&p, nic_sockaddr);
rmx_input_enable_stream_option(&p, RMX_INPUT_STREAM_CREATE_INFO_PER_PACKET);
rmx_input_set_timestamp_format(&p, RMX_INPUT_TIMESTAMP_RAW_NANO /* or _SYNCED */);
rmx_input_set_mem_capacity_in_packets(&p, n);            /* ring size in packets */
rmx_input_set_mem_sub_block_count(&p, 1 | 2);            /* 2 = header/data split */
rmx_input_set_entry_size_range(&p, sb, min, max);        /* or rmx_input_set_entry_uniform_size */
rmx_input_determine_mem_layout(&p);                       /* library computes strides/lengths */
stride = rmx_input_get_stride_size(&p, sb);
region = rmx_input_get_mem_block_buffer(&p, sb);         /* {addr,length,mkey}: leave, or fill addr (+mkey) */
cap    = rmx_input_get_mem_capacity_in_packets(&p);      /* may be rounded */
rmx_input_create_stream(&p, &id);
rmx_input_set_completion_moderation(id, min_pkts, max_pkts, timeout_usec);
rmx_input_init_flow(&f); rmx_input_set_flow_local_addr(&f, dst); rmx_input_set_flow_remote_addr(&f, src); rmx_input_set_flow_tag(&f, tag);
rmx_input_attach_flow(id, &f);                            /* runtime; detach with rmx_input_detach_flow */
```

**[verified]** RE `api_demo/input/receive/receive.cpp:53-106`, `memory_registration_receive/memory_registration_receive.cpp:101-131`, `multi_source_receive/multi_source_receive.cpp:118-127`, `drain_detached_flow_receive/drain_detached_flow_receive.cpp:210-230`; RDK `core/stream/receive/receive_stream.cpp:49-95,210-235`; UE `Streams/RivermaxInputStream.cpp:1160-1242`.

- `RMX_INPUT_APP_PROTOCOL_PACKET` delivers from the UDP payload (RTP header first); `RMX_INPUT_RAW_PACKET` delivers whole frames including network headers (UE parses the RTP header "from the raw net header") **[verified]** RDK `io_node/receivers/rtp_receiver_io_node.cpp:49`, UE `RivermaxInputStream.cpp:175,350-352`.
- `RMX_INPUT_STREAM_RTP_SMPTE_2110_20_DYNAMIC_HDS` splits 2110-20 packets at the variable RTP+SRD header boundary **[verified name]** UE `RivermaxInputStream.cpp:1179`; exact behaviour **[inferred]** from the name.
- HDS capability can be absent: UE checks `header block length <= 0` → "Header data split not supported for device" **[verified]** UE `RivermaxInputStream.cpp:1193-1197`.
- **Hardware packet placement**: `RMX_INPUT_STREAM_RTP_SEQN_PLACEMENT_ORDER` (16-bit RTP seq) / `RMX_INPUT_STREAM_RTP_EXT_SEQN_PLACEMENT_ORDER` (32-bit 2110-20 extended seq) make the NIC write each
  packet to ring slot derived from its sequence number; "The application should not expect the packet receive events (completions) to arrive in any specific order" **[verified]** DOCA
  "Hardware-Accelerated Packet Placement"; DS `gstnvdsudpsrc.c:1542-1549` ("hardware-based ST2022-7 implementation").
- One stream can carry many flows (SSM multi-source, multiple destinations); packets carry the flow tag; a detached flow's already-received packets can still be drained **[verified]** RE `multi_source_receive/README.md`, `drain_detached_flow_receive/README.md`.

### 5.2 Getting data

```c
rmx_input_init_chunk_handle(&h, id);
rmx_input_get_next_chunk(&h);                                   /* RMX_OK even with 0 packets */
const rmx_input_completion* c = rmx_input_get_chunk_completion(&h);
n    = rmx_input_get_completion_chunk_size(c);                  /* packets in this chunk */
base = rmx_input_get_completion_ptr(c, sb);                     /* first stride, per sub-block */
t0   = rmx_input_get_completion_timestamp_first(c);  t1 = rmx_input_get_completion_timestamp_last(c);
more = rmx_input_get_completion_flag(c, RMX_INPUT_COMPLETION_FLAG_MORE);
pi   = rmx_input_get_packet_info(&h, i);                        /* with CREATE_INFO_PER_PACKET */
len  = rmx_input_get_packet_size(pi, sb); tag = rmx_input_get_packet_flow_tag(pi); ts = rmx_input_get_packet_timestamp(pi);
pkt  = base + i * stride;
```

**[verified]** RE `receive.cpp:117-141`; RDK `core/chunk/receive_chunk.cpp:38-53`, `core/chunk/include/rdk/core/chunk/receive_chunk.h:44-120`, `core/stream/receive/ipo_receive_stream.cpp:337`,
`io_node/misc/generic_latency_io_node.cpp:352`; UE `RivermaxInputStream.cpp:253-348`. DOCA's equivalent event also returns "Sequence number of the first packet" **[verified]** DOCA; a public `rmx_*`
getter for it **[unknown]**.

| Question | Answer | Label |
|---|---|---|
| Blocking? | Set by `set_completion_moderation(min, max, timeout_us)`. DOCA: timeout = "μsecs that library would do busy wait (polling) for ... at least min_packets". Dev Kit: `timeout 0` busy-loops until `max`; `min=max=0` avoids waiting | **[verified]** DOCA; RDK `.../receive/receive_stream_interface.h:62-79`. The texts disagree |
| What real code does | Every example and UE use `(min=0, max=5000, timeout=0)` and then **sleep 100-300 µs** between polls; RDK receivers loop with optional sleep | **[verified]** RE `receive.cpp:84-143`; UE `RivermaxInputStream.cpp:270,1221-1224`; RDK `receiver_io_node_base.cpp:378-405`. Returns promptly with 0..max packets **[inferred]** |
| Interrupt-driven wait | Per-stream event channel: `rmx_init_event_channel(&p, id)`, `rmx_set_event_channel_handle`, `rmx_establish_event_channel` → fd (Linux, epoll) or IOCP handle (Windows); arm with `rmx_request_notification` | **[verified]** RDK `services/legacy_util/rt_threads.cpp:415-470`; used on TX when `RMX_NO_FREE_CHUNK` in RE `rivermax_player.cpp:1781-1783`. RX use **[inferred]** |
| RX buffer lifetime | No release call exists; a chunk's strides stay valid until the ring wraps. Consumers must finish (or copy) before `capacity` more packets arrive | **[inferred]** from the absence of any release API in the 131-entry table and in all consumers |
| Timestamps | `RMX_INPUT_TIMESTAMP_RAW_NANO` (NIC time in ns), `RMX_INPUT_TIMESTAMP_SYNCED` (used by latency tools against `rmx_get_time`); DOCA says the default is "raw counter" | **[verified]** names: RDK `io_node/misc/generic_latency_io_node.cpp:179`, `apps/media_probe/media_probe.cpp:168`; DOCA. "SYNCED = PTP/clock domain" **[inferred]** |
| Frame assembly | None in Rivermax. RX is packet-chunk only; receivers assemble frames themselves (UE copies payload into its frame buffer / GPU) | **[verified]** UE `RivermaxInputStream.cpp:316-360`; RDK `rtp_receiver_io_node.cpp:58-95` |

### 5.3 ST 2022-7 RX (IPO, "Inline Packet Ordering")

The Dev Kit `IPOReceiveStream` "implements redundant multi-path receive stream by incapsulating multiple streams using Inline Packet Ordering feature over the **same memory buffer**" **[verified]** RDK `core/stream/receive/include/rdk/core/stream/receive/ipo_receive_stream.h:93-97`:

1. One `rmx_input` stream **per path/NIC**, each with `RMX_INPUT_STREAM_RTP_[EXT_]SEQN_PLACEMENT_ORDER`, all pointing at the **same header/payload buffers** (one mkey per NIC) **[verified]** `ipo_receive_stream.cpp:56-79,165-198`.
2. The NIC places a packet at slot `seq % capacity`; a duplicate from the other leg lands on the same slot **[verified]** DOCA + `ipo_receive_stream.cpp:534`.
3. Software polls all legs, marks slots valid, and releases a contiguous run only after each packet has sat for `max_path_differential` (default 50 ms in the app) **[verified]** `ipo_receive_stream.cpp:356-511`, `apps/ipo_receiver/ipo_receiver.cpp:29,119`. Sender restart is detected after 100 ms idle **[verified]** `ipo_receive_stream.h` (`m_sender_restart_threshold`).

So 2022-7 merge is "HW places, SW tracks validity and latency budget"; there is no library-level 2022-7 RX object in `rmx_*` **[verified by absence]**.

## 6. Clock and time

| Mode | Calls | Semantics | Label |
|---|---|---|---|
| System (default) | none | Rivermax time = system clock, UTC | **[verified]** RE `rivermax_player.cpp:94-100,2716-2720` ("Leap sec in nano for TAI conversion to UTC") |
| User | `rmx_init_user_clock(&p)`, `rmx_set_user_clock_handler(&p, uint64_t (*)(void* ctx))`, `rmx_set_user_clock_context`, `rmx_use_user_clock(&p)` | App callback returns ns | **[verified]** RDK `services/utils/clock.cpp:31-46`; RE `rivermax_player.cpp:2721-2727` |
| NIC PTP | `rmx_init_ptp_clock(&p)`, `rmx_set_ptp_clock_device(&p, &iface)`, `rmx_use_ptp_clock(&p)`, then spin `rmx_check_clock_steady()` while `RMX_BUSY` | Library reads the NIC PHC; "Linux PTP runs on the host, locking the NIC's real-time clock" (on Windows via BlueField/DOCA Firefly); CX-6 Dx or DPU only | **[verified]** RDK `clock.cpp:48-66`; WEB FAQ; RE `rivermax_player.cpp:2738-2771` |
| Read | `rmx_get_time(RMX_TIME_PTP, &ns)` | TAI ns when PTP clock is configured | **[verified]** RDK `clock.cpp:68-72`; UE `RivermaxManager.cpp:179-184`. Other `rmx_time_type` values **[unknown]** |

Rivermax runs **no PTP servo of its own**: an external daemon (linuxptp / DOCA Firefly) disciplines the NIC clock **[verified]** FAQ. UE falls back to system clock when PTP is unsupported on the interface **[verified]** UE `RivermaxManager.cpp:417-460`. Contrast: MTL ships a built-in PTP client (`MTL_FLAG_PTP_ENABLE`) plus `ptp_get_time_fn` user clock (`include/mtl_api.h:342,667`) **[verified]**.

## 7. Memory

| Configuration | How | Label |
|---|---|---|
| Rivermax-managed | Leave sub-block `addr=NULL,len=0,mkey=RMX_MKEY_INVALID` (TX) or leave `rmx_input_get_mem_block_buffer()` untouched (RX) | **[verified]** RDK `core/stream/send/media_stream.cpp:134-141`; RE `api_demo/input/receive/receive.cpp` |
| App-allocated, Rivermax-registered | Fill `addr/length`, `mkey = RMX_MKEY_INVALID` | **[verified]** RE `memory_allocation_media_send.cpp:88-100` |
| App-allocated and registered | `rmx_init_mem_registry(&rp, &iface)`, optional `rmx_set_mem_registry_option`, `rmx_register_memory(&region, &rp)` → `region.mkey`; `rmx_deregister_memory(&region, &iface)` | **[verified]** RE `memory_registration_media_send.cpp`; UE `RivermaxWrapper.h:55-58` |

- Registration is **per device** (one mkey per NIC; 2022-7 registers the same buffer twice) **[verified]** RDK `apps/base_memory_strategy.cpp:129-150`. Motivation per NVIDIA: one large registration for many streams → one mkey → better HW cache use **[verified]** RE `memory_registration_media_send/README.md`. Registration options **[unknown]**.
- Generic output requires app-registered memory (`validate_memory_layout` rejects unregistered) **[verified]** RDK `core/stream/send/generic_stream.cpp:201-213`.
- GPUDirect: payload in CUDA VMM memory allocated with `cuMemCreate` + `gpuDirectRDMACapable = 1`, headers in host memory; "GPU Direct is supported only in header-data split mode"; "When using GPU
  allocation, Rivermax will use GPUDirect mode seamlessly" (no GPU flag in the API — Rivermax detects the pointer) **[verified]** RDK `services/legacy_util/gpu.cpp:237-290`,
  `services/settings/validator_utils.cpp:106-116`; RE `memory_allocation_media_send.cpp:51-58`. FAQ confirms HDS puts headers in host, payload in GPU **[verified]**.
- Dev Kit allocators: `Malloc`, `HugePageDefault/2MB/512MB/1GB`, `GPU`, `GPUHostPinned` **[verified]** RDK
  `services/memory_allocation/include/rdk/services/memory_allocation/memory_allocator_interface.h:46-54`. Hugepage allocations are rounded to page size; GPU alignment via `gpu_query_alignment`
  **[verified]** RDK `services/memory_allocation/huge_pages_memory_allocator.cpp`, `gpu_memory_allocator.cpp`. DOCA recommends ≥800 × 2 MB hugepages **[verified]** DOCA.
- Lifetime: registrations must outlive streams (destroy streams, then deregister) **[verified]** ordering in RE `generic_send.cpp:164-172`, `memory_registration_receive.cpp:216-228`.
- `MediaUnitBuffer` / `MediaUnitPool` are Dev Kit app-level frame containers (owned or borrowed memory, host/GPU location, mutex + condvar pool) that feed a packet writer which **copies** into chunks
  **[verified]** RDK `services/media/include/rdk/services/media/media_essence_source.h:103-111`, `media_unit_pool.h:54-64`, `io_node/senders/media_sender_io_node.cpp:337-359,498-516`. True zero-copy
  frames need the app to lay its frame buffers out *as* the mem block (UE does this: `StreamMemory.BufferAddresses[frame_index]`) **[verified]** UE `RivermaxOutStream.cpp:646`.

## 8. Errors, signals, stats

Status codes seen in public code (complete enum **[unknown]**):

| Code | Where | Transient? | Typical handling |
|---|---|---|---|
| `RMX_OK` | all | — | — |
| `RMX_NO_FREE_CHUNK` | TX `get_next_chunk`, `skip_chunks` | yes (ring full) | spin, or `rmx_request_notification` + epoll |
| `RMX_HW_SEND_QUEUE_IS_FULL` | TX `commit_chunk` | yes | spin / short sleep (DS: 10 µs) |
| `RMX_BUSY` | `destroy_stream`, `poll_for_completion`, `check_clock_steady`, `request_notification` | yes | retry / sleep 1 ms |
| `RMX_SIGNAL` | any blocking call after `rmx_enable_system_signal_handling` | terminal for the loop | exit cleanly |
| `RMX_HW_COMPLETION_ISSUE` | TX `commit_chunk` | fatal for stream | UE/rivermax_player stop the stream |
| `RMX_CHECKSUM_ISSUE` | RX (legacy generic receiver) | per-chunk | count and continue |
| `RMX_NOT_INITIALIZED`, `RMX_NOT_IMPLEMENTED`, `RMX_INVALID_PARAM_2` | init/params | fatal (config) | report |

**[verified]** RE `media_send.cpp:124-153`, `legacy/rivermax_player/rivermax_player.cpp:1580-1613`, `legacy/generic_receiver/generic_receiver.cpp:290-310`; RDK `core/chunk/media_chunk.cpp:47-154`, `rt_threads.cpp:450-470,685-692`; UE `RivermaxOutStream.cpp:510-535`; DS legacy sink `:2242-2244`. The `_2` suffix implies per-argument-position invalid-param codes **[inferred]**.

Stats: out-of-band **consumer** API — `rmx_stats_init_config`, `rmx_stats_config_set_process_id`, `rmx_stats_config_register_stats_type(RMX_STATS_SESSION_START|STOP|RUN|TX_QUEUE|RX_QUEUE|TIME)`,
`rmx_stats_create_consumer`, `rmx_stats_consumer_pop_message` → typed handles with getters (committed chunks/strides, user/free/busy chunks; TX packets/bytes/packet WQEs/dummy WQEs/free WQEs; RX
packets/bytes/used strides/CRC errors; session start W×H×fps) **[verified]** RDK `services/statistics/statistics_reader.cpp:40-270`. The process-id filter suggests a separate monitoring process can
read another process's stats **[inferred]**. No per-stream synchronous "get stats" call exists **[verified by absence]**.

## 9. Threading and progress

- Progress: the NIC paces; one Rivermax **internal thread** exists (affinity via `rmx_set_cpu_affinity` before init) **[verified]** DOCA, RDK `facade.cpp:149`. What it does (completions, clock sampling, stats) **[unknown]**.
- App data-path threads call `get_next_chunk`/`commit_chunk`/`get_next_chunk` directly; nothing is callback-driven **[verified]**.
- Every public example uses a stream (and its chunk handle) from exactly one thread; the Dev Kit gives each IO node thread N streams, pins it (`set_current_thread_affinity`) and raises it to RT priority
  (`RMAX_THREAD_PRIORITY_TIME_CRITICAL - 1`) **[verified]** RDK `io_node/senders/media_sender_io_node.cpp:616-620`, `io_node/receivers/receiver_io_node_base.cpp:247-251`. Thread-safety of concurrent
  calls on one stream **[unknown]**; different streams from different threads is the documented pattern **[verified]**.
- Required privileges (DOCA flavour): root or `cap_net_raw,cap_sys_nice` **[verified]** DOCA.

## 10. How Rivermax apps structure a 2110-20 sender and receiver

**Sender** (RDK `media_sender`, UE, DS all converge) **[verified]** RDK `io_node/senders/media_sender_io_node.cpp:451-574`, UE `RivermaxOutStream.cpp`, DS `gstnvdsudpsink.c:2335-2560`:

1. PTP clock on the TX NIC; start = `now + 1 s`, aligned up to the next frame boundary, plus TRO (2110-21 default); all streams of a process share the start (optional synchronizer across senders).
2. Ring = 1 mem block × ~10 frames (RDK) or N app frame buffers (UE).
3. Per frame: sleep until ~2 ms before the frame's send time (`SLEEP_THRESHOLD_NS = 2 ms`), then fill chunks just-in-time and commit; only chunk 0 carries the time.
4. Late: if `send_time <= now (+600 ns)` commit with 0 and log "Timeout occurred. Send time exceeded by ..."; UE additionally skips the next interval ("EnableTimingProtection") and calls `skip_chunks(0)`.
5. No new frame (UE continuous output): repeat previous by `skip_chunks` back to it.
6. Optional completion tracking of first/second-to-last chunk per frame for diagnostics.

**Receiver**: thread per N streams, pinned; `get_next_chunk` poll with 0..5000 packets and a 100-300 µs sleep; parse RTP per stride; app does frame assembly, drop detection (seq gaps), and 2022-7 (IPO) itself **[verified]** RDK `io_node/receivers/receiver_io_node_base.cpp:345-418`, `rtp_receiver_io_node.cpp:58-120`; UE `RivermaxInputStream.cpp:249-360`.

---

## 11. Mapping: Rivermax → MTL today → unified-API proposal

MTL names from `include/*.h` at this tree **[verified]**.

| Rivermax concept | MTL today | Unified-API analogue (proposal) | Where MTL can/should differ |
|---|---|---|---|
| `rmx_init` + `rmx_set_cpu_affinity` + `rmx_enable_system_signal_handling` | `mtl_init(struct mtl_init_params*)`, `mtl_start/stop/uninit`; lcores via `MtlManager`/params | `mtl_init` stays; keep "configure-before-init" block | MTL owns scheduler threads (tasklets); Rivermax only has 1 internal thread + app threads |
| Device by local IP (`rmx_retrieve_device_iface_ipv4`) | Port by PCI BDF / kernel ifname in `mtl_init_params.port[]` | Allow selecting a port by IP *or* BDF | Rivermax users think "local IP"; MTL users "BDF" |
| Output media stream from **SDP** + `idx_in_sdp` | `st20_tx_create(struct st20_tx_ops*)`, `st20p_tx_create`; no SDP parsing in `lib/` (grep: none) | Optional `…_from_sdp()` helper that fills the ops struct | MTL must not *require* SDP (audio/anc/fmd, tests) |
| `packets_per_frame`, stride, `packets_per_chunk` | `ST20_TYPE_RTP_LEVEL`: `rtp_frame_total_pkts`, `rtp_pkt_size`, `rtp_ring_size` (`include/st20_api.h:1257-1277`) | Packet-level mode keeps these names close | — |
| App writes RTP headers into strides | `ST20_TYPE_RTP_LEVEL` (`st20_tx_get_mbuf/put_mbuf`) only; frame/slice/pipeline modes build RTP in lib | Keep lib-built RTP as default; offer app-built RTP as the "Rivermax-like" mode | Big MTL advantage: lib does packetisation, SRD, seq, marker, timestamp |
| Mem block ring, chunk = N strides | Frame buffers (`framebuff_cnt`), `st20_tx_set_ext_frame`, `st20p_tx_put_ext_frame`, slice mode (`query_frame_lines_ready`) | Buffer ring; *chunk ≈ slice (lines)* | MTL frame = unit of ownership; Rivermax chunk = unit of submission |
| `get_next_chunk` / `commit_chunk(time)` | `get_next_frame` / `notify_frame_done` callbacks; pipeline `st20p_tx_get_frame` / `st20p_tx_put_frame` | `acquire` / `submit(buf, time)` with explicit buffer identity (no FIFO trap) | Keep MTL's buffer identity; review §6.5 already asks for it |
| Commit time (ns, Rivermax clock; first chunk only; 0 = ASAP) | `ST20_TX_FLAG_USER_PACING` / `USER_TIMESTAMP` / `EXACT_USER_PACING` + `st20_tx_frame_meta.timestamp`/`tfmt` (`include/st20_api.h:50-100`) | One "presentation/transmit time" field per submitted frame, TAI ns, with explicit meaning (epoch-aligned vs exact) | MTL knows media timing (TRO, epochs) itself; apps need not compute TRO |
| App detects lateness | `notify_frame_late(priv, epoch_skipped)` (`include/st20_api.h:1204`) | Keep an explicit late/skip event | MTL is better here |
| `cancel_unsent_chunks`, `skip_chunks` | none (frame drop is implicit) | `abort`/`flush` + "repeat last" policy | Consider a documented repeat-last behaviour |
| TX completion tracking (token + HW timestamp) | `notify_frame_done`; TX timestamp only in stats/RTCP | Mandatory per-buffer completion; optional HW TX time | Review §6.5: mandatory completion |
| `rmx_register_memory` → mkey (per NIC) | `mtl_dma_map/unmap`, `mtl_dma_mem_alloc`, `mtl_hp_malloc` (`include/mtl_api.h`) | Memory-region object with per-port handle | Provider-neutral handle, not mkey |
| GPUDirect implicit from pointer + HDS | none in lib | Explicit memory domain on region | Make direct/copy path observable |
| HDS TX/RX | `ST20_RX_FLAG_HDR_SPLIT` (`include/st20_api.h:226`) | Keep as capability | E810 HDS support differs |
| Output generic + HW rate | none public (datapath queues internal); `ld_preload` UDP shim | Maybe "raw UDP stream" | Out of scope for 2110? |
| Input stream + flows attach/detach | `st20_rx_create` (one flow/port), `st20_rx_update_source` | Session with source update | MTL: one flow per session |
| RX chunk of packets (`APP_PROTOCOL`/`RAW`) | `ST20_TYPE_RTP_LEVEL` RX (`st20_rx_get_mbuf/put_mbuf`) | Packet-level RX mode with explicit release | MTL has explicit release; Rivermax relies on ring wrap |
| RX frame assembly: app | Lib assembles frames/slices (`st20_rx_put_framebuff`, `st20p_rx_get_frame`), incomplete-frame flag, timing parser | Keep | Major MTL value-add |
| IPO 2022-7 (HW placement, app merge) | Lib-internal dual-port dedup (`MTL_SESSION_PORT_P/R`) | Keep in lib | MTL does the merge for the app |
| `set_completion_moderation(min,max,timeout)` + sleep | `st20p_rx_set_block_timeout`, `…_wake_block`, `mtl_sch_enable_sleep` | Common wait/timeout verb | — |
| Event channel fd + `request_notification` | none (callbacks/blocking get) | Optional pollable fd per session | Useful for epoll-based apps |
| Clock: system/user/PTP | built-in PTP (`MTL_FLAG_PTP_ENABLE`), `ptp_get_time_fn`, `mtl_ptp_read_time` | Same three modes, TAI always | MTL has its own PTP client |
| `rmx_stats_*` out-of-band consumer | `st20_tx_get_session_stats`, `mtl_get_port_stats`, `stat_dump_cb_fn` | In-process stats + optional shm | — |
| Versioned exports `_v1`, `_rmx_init(&version)` | plain symbols, struct-by-value ops | Consider versioned init / size-tagged structs | ABI lesson |

## 12. What Rivermax users will find alien in MTL, and what would ease migration

Alien today **[inferred from the comparison above]**:

1. **No SDP entry point.** Rivermax media senders start from an SDP string; MTL wants a C ops struct with IPs, ports, fmt, fps, pacing, payload type, etc.
2. **Callbacks from library threads** (`get_next_frame`, `notify_frame_done`, `notify_rtp_done`) instead of the app driving a loop. Rivermax users own the data-path thread; MTL's tasklet threads call them — with the "never block in a callback" rule.
3. **Library builds RTP and paces in software/HW rate-limit.** Rivermax users expect to write RTP headers themselves and to hand the NIC a wire time.
4. **Frame is the unit, not the chunk.** No "get stride pointer for packet i of this chunk"; slice mode is the nearest thing but is pull-based (`query_frame_lines_ready`).
5. **Ports by BDF, EAL, hugepages, VFs, MtlManager.** Rivermax needs a kernel netdev with an IP and a license; no DPDK binding.
6. **Time conventions.** Rivermax apps juggle UTC vs TAI (leap seconds) and add TRO themselves; MTL always works in TAI and computes TRO internally. Good, but must be documented as such for migrators.
7. **RX completes frames** rather than returning ring-strided packet chunks; release is explicit (`put_framebuff`) rather than implicit ring wrap.

What would make migration easy:

- An SDP helper (`sdp → ops`) and SDP generation for the receiver side (Rivermax users have NMOS/IS-05 SDPs in hand).
- A packet-level ("RTP passthrough") mode whose parameters mirror Rivermax: packets per frame, stride/packet size, packets per chunk, first-chunk time, 0 = ASAP, `NO_FREE`/`QUEUE_FULL`-style non-blocking statuses.
- Treat **chunk = slice** (N lines) in the unified buffer model, so line-based progressive submission maps 1:1 to `get_next_chunk`/`commit_chunk`.
- A single documented clock domain (TAI ns from `mtl_ptp_read_time`), with lateness surfaced as an event (MTL already has `notify_frame_late`).
- App-driven mode (no callbacks): `acquire → fill → submit(time)` on the app thread, with a pollable fd for waiting.
- Explicit memory-region registration with a "one region, many sessions" pattern, per-port handles, and a GPU/device domain.

What MTL should **not** copy **[inferred]**: the stream-level FIFO commit (identity mismatch), implicit RX lifetime by ring wrap, optional-only TX completion, "commit fails if the time is <600 ns away" (MTL should accept and report late), per-vendor mkey in the public type, and SDP as the *only* creation path.

---

## Open questions for the maintainer

1. **Should the unified API offer an SDP-based constructor?** Rivermax media streams are created from an SDP (`rmx_output_media_set_sdp` + `set_idx_in_sdp`). A `…_create_from_sdp()` helper would make
   Rivermax and NMOS users feel at home but pulls SDP parsing (and its edge cases: `TP=`, `a=mid`/DUP groups, `ts-refclk`) into `lib/`. Options: (a) helper in `lib/` for 2110-20/30/40 only; (b) separate
   `ecosystem/` or `app/` utility that fills the ops struct; (c) no SDP support.
2. **Is "app-built RTP + library pacing" a first-class mode?** Rivermax's only media TX model is "app writes RTP headers per stride, NIC paces". MTL has this as `ST20_TYPE_RTP_LEVEL` with mbufs. Options:
   (a) keep lib-built RTP as the only unified-API path and leave RTP-level in the legacy API; (b) add a packet-level mode with Rivermax-like knobs (packets per frame/chunk, stride); (c) allow header-only
   overrides (app supplies RTP header template, lib fills payload).
3. **What does a submission time mean?** Rivermax: first packet's wire time in the library clock, only on the first chunk, 0 = ASAP, rejects times <~600 ns away, and the app adds TRO. MTL: TAI
   epoch-aligned pacing, with `USER_PACING` / `EXACT_USER_PACING` variants and internal TRO. Options: (a) single field = "frame's alignment point (TAI)", lib adds TRO; (b) "exact first-packet time", app
   owns TRO (Rivermax-like); (c) both via an enum. This decides how Rivermax migrators port their time math.
4. **Late submissions: reject, clamp, or skip?** Rivermax integrators clamp to 0 and reset state with `skip_chunks(0)`; UE also skips the next interval. MTL today skips epochs and reports `notify_frame_late`. Options: (a) always send ASAP and report; (b) drop to the next epoch and report (current MTL); (c) per-session policy.
5. **Chunk = slice?** Should the unified buffer model expose sub-frame submission (N lines, like a Rivermax chunk) as the same object as a frame with a "lines ready" watermark, or as separate chunk objects? This affects progressive/low-latency TX (review §12) and how naturally `get_next_chunk/commit_chunk` code ports.
6. **Explicit buffer identity vs ring position.** Rivermax commit sends the oldest acquired chunk regardless of handle, and repeat/drop is done by `skip_chunks`. Review §6.5 already asks MTL to keep identity. Confirm we also want an explicit "repeat last frame" / "skip N" operation, since Rivermax users rely on it for continuous output.
7. **RX: provide a packet-chunk mode with implicit or explicit release?** Rivermax RX returns ring-strided packet chunks and relies on ring wrap for lifetime (no release call). Options: (a) no packet mode in the unified API (frame/slice only); (b) packet mode with explicit release (current MTL mbuf style); (c) Rivermax-like ring view with a documented "valid until N more packets" contract.
8. **Waiting model: callbacks, blocking calls, or pollable fds?** Rivermax is app-driven polling plus an optional per-stream event fd (`rmx_establish_event_channel`). MTL mixes library-thread callbacks and blocking pipeline getters. Should every unified session expose a pollable fd (epoll/IOCP-friendly), and should callbacks be optional?
9. **Device selection by IP address.** Rivermax selects NICs by local IP; MTL by BDF/ifname. Do we add IP-based port lookup to `mtl_init`/session creation, given DPDK-bound VFs may not have a kernel IP?
10. **Memory registration shape.** Rivermax: `rmx_register_memory(region, device)` → mkey per NIC, reused across many streams, with three provisioning modes. Do we expose a region object with one handle
    per port (needed for 2022-7 on two NICs) and allow many sessions to share it, as in `memory_registration_media_send`? And should a device-memory (GPU) domain be explicit, or inferred from the pointer
    as Rivermax does?
11. **Completion: mandatory, and with HW TX timestamp?** Rivermax tracking is opt-in per chunk, returns token + HW completion time, and requires polling. If MTL makes completion mandatory for ownership, should it also carry a TX timestamp (when the NIC/driver provides one), for latency diagnostics like the Dev Kit latency tool?
12. **ABI technique.** Rivermax uses caller-allocated param structs initialised by `*_init()`, setter functions, header-inline accessors, and `_v1`-suffixed exports with a versioned `_rmx_init(&version)`. Do we want setters/versioned symbols (heavier API surface, strong ABI) or size-tagged structs (lighter, familiar to MTL users)?
13. **Stats surface.** Rivermax has only an out-of-band, message-based stats consumer (per process ID, typed messages including dummy WQEs and CRC errors). Is an out-of-process stats reader (e.g. via `MtlManager` shared memory) in scope, or do we keep in-process getters only?
14. **Access to licensed Rivermax docs.** Several semantics here are **[unknown]** from public sources (full `rmx_status` enum, exact commit-time failure code, `tx_adaptive_scheduling_factor`, which SDP
    attributes drive pacing, completion-moderation semantics where DOCA and the Dev Kit disagree, thread-safety). Can someone with a Rivermax developer account confirm these before the design freezes
    Rivermax-compatible naming?
