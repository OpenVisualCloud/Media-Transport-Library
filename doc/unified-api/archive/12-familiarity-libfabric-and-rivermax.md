# 12 — For libfabric and Rivermax users: concept map

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5) |
| Date | 2026-09-30 |
| Goal | GO-2: recognisable to libfabric and Rivermax developers without inheriting their baggage |
| Research | [R09 libfabric](research/09-libfabric.md), [R10 Rivermax](research/10-rivermax.md); several Rivermax semantics are unknown from public sources (Q-EXT-1) |
| Names | as in the header [`sketch/include/mtl/experimental/mtl_unified.h`](r3-sketch/include/mtl/experimental/mtl_unified.h) (revision 3 vocabulary, response A6) |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

## 1. The shared shape

All three APIs converge on the same four ideas. The unified MTL API keeps them:

```text
register memory once ──▶ get a slot/buffer ──▶ fill ──▶ post/commit/submit ──▶ completion
(fi_mr_reg / rmx_register_memory / mtl_mem_import)      (fi_send / commit_chunk / mtl_tx_submit)
                                                         (fi_cq_read / poll_for_completion / mtl_cq_read)
```

What only MTL has: **time**. libfabric has no scheduled transmit, no deadline, no late
completion and no timestamps in completions; its triggers are counter thresholds only
`[R09 §0]`. Rivermax has a send time per chunk but no notion of media time, RTP
derivation, lateness reporting or A/V alignment; apps add TR_OFFSET themselves and
detect lateness themselves `[R10 §2.4]`. MTL's timing model (06) is therefore designed
from the standards, not borrowed.

Revision 3 also takes two lessons from libfabric's rough edges: `*_trywait` no longer shares
a return code with the read verbs next to it (§2), and the completion-record stride is an
explicit argument rather than a property fixed at open.

## 2. libfabric → MTL

| libfabric | MTL unified API | Same | Different, and why |
|---|---|---|---|
| `fi_getinfo(hints)` → `fi_info` (caps floor, attrs "≥ requested") | the dry run `mtl_<essence>_session_query(mt, &sc, &mc, flags, &info, &req)` → granted values + buffer requirements, before anything is allocated¹ | requested-vs-granted; zero means "library chooses" | no provider list and **no mode bits**: MTL is the only implementer, so it satisfies its own restrictions `[R09 §10]` |
| fabric / domain | instance (`mtl_instance_h`) / port | | a session can span two ports (ST 2022-7) |
| endpoint (`FI_EP_DGRAM` + multicast) | session | open → bind → enable ≈ create → bind CQ / attach → start | a session is a *stream with a clock*, not a message pipe |
| `fi_mr_regattr{iov, iface, device, dmabuf}`, `fi_mr_desc` | `mtl_mem_import{va, length, domain, device_mask, access}` → region; buffers reference `{region, offset}` | explicit registration, memory type field | close with operations in flight is **`-MTL_EBUSY`**, not undefined `[R09 §3]`; `device_mask = 0` maps lazily on first attach; `access` is enforced at attach |
| `desc == NULL` → provider registers internally | default library pool | "library handles it" path | |
| `context` pointer per operation | `user_cookie` (u64) per submission and per buffer | returned verbatim | a plain cookie; MTL never writes into app-provided scratch memory (the `FI_CONTEXT` stack-corruption trap) |
| `fi_send` / `fi_sendmsg(iov, desc, flags)` | `mtl_tx_submit(s, lease, &submission)` | post a buffer, get one completion | the submission carries **media time** and optional launch time; the lease is its own handle type |
| `FI_INJECT` | copy-on-submit essences (`mtl_tx_write` for ST30/40/41, which are copied into packets anyway) | | reported as the `COPY` path, not a flag |
| `FI_INJECT_COMPLETE` / `FI_TRANSMIT_COMPLETE` | terminal TX result = "no reader left" (buffer reusable) + observed wire times | "when may I reuse the buffer" is explicit | MTL states a default (libfabric's man pages never do `[R09 §4.1]`) and adds lateness |
| `-FI_EAGAIN` | `-MTL_EAGAIN` (value 11) | the only back-pressure signal | codes are `MTL_E*` constants with the Linux errno values, one ABI on every OS |
| `fid_cq`, `fi_cq_read`, `fi_cq_sread(timeout)` | `mtl_cq_h`, `mtl_cq_read(cq, buf, record_size, max, timeout)` | batched polling, blocking read | results are **media records** (status, RTP, TAI times with a validity mask); the record size is passed per call, so an old binary reads a newer library safely |
| `fi_cq_readerr` / `-FI_EAVAIL` | status inline in every entry | | a corrupted RX frame is data with a verdict, not an error; no second read path |
| CQ overrun is fatal (`FI_EOVERRUN`) | results are materialised by the reader from the lease table; unread results are bounded by the pool size, and acquire reports `blocked_on = RESULTS` instead of dropping | | stricter than libfabric, cheap because in-flight work is bounded |
| `FI_WAIT_FD` + `FI_GETWAIT` + `fi_trywait` | `mtl_cq_get_wait_object` + `mtl_cq_trywait` | the same race-free pattern | `fi_trywait` returns `-FI_EAGAIN` for "something is ready", next to reads where it means "nothing"; MTL's `trywait` returns **1** ready / **0** armed / **< 0** error, never `-MTL_EAGAIN` `[C5 §2.10]`; an fd or a Windows `HANDLE` |
| `FI_SELECTIVE_COMPLETION` (+ errors always reported) | completion mode `MTL_COMPLETE_EXCEPTIONS` | errors always reported | |
| `fid_eq`, `fi_eq_read`, `fi_eq_write` | `mtl_eq_h`, `mtl_eq_read(eq, ev, ev_size, max, timeout)`, `mtl_eq_post` | control-plane events separate from CQs; app can inject events | bounded, coalescing, a state getter per event type instead of "overrun is fatal"; subscription masks (`MTL_EQ_SUB_*`) give each framework element its own copy |
| `fid_cntr`, `fi_cntr_wait(threshold)` | stats counters (lock-free snapshot, cumulative) | cheap aggregate completion counts | no wait-on-counter in v1 |
| `FI_PROGRESS_AUTO` | library tasklets | | the progress threads are visible, pinned and configurable |
| `FI_PROGRESS_MANUAL` | not in v1 (Q-THR-3) | | unsafe for hardware-paced video |
| `FI_THREAD_COMPLETION` | acquire and release are MP-safe; submits need one context per session unless `MTL_SESSION_MT_SUBMIT`; one waiter per wait target | lock-free by contract | `MTL_SESSION_SINGLE_READER` elides the reaper lock |
| `FI_RM_ENABLED` | always | `-MTL_EAGAIN`, never fatal | |
| `fi_cancel` → `FI_ECANCELED` | `mtl_session_stop(MTL_STOP_FLUSH)` → `FLUSHED` results; `mtl_tx_withdraw` → `FLUSHED/WITHDRAWN`; `mtl_session_interrupt` → `-MTL_ECANCELED` for waiters | | `fi_close` silently discards in-flight ops; MTL gives every accepted unit a result |
| `fi_open_ops(name)` | key-addressed options and typed stat getters; backend extension tables later (`mtl_open_ext`) | | |
| `FI_VERSION(major, minor)` passed to `fi_getinfo` | `mtl_instance_params.api_version`, per instance; `struct_size` everywhere; `mtl_struct_known_size()` | "state what you were compiled for", scoped to the objects it creates | no process-global version call |

¹ Plus `mtl_port_get_caps`, `mtl_port_get_capacity` and `mtl_capability_request` {ANY, PREFER, REQUIRE, OFF}.

## 3. Rivermax → MTL

| Rivermax | MTL unified API | Same | Different, and why |
|---|---|---|---|
| `rmx_init`, `rmx_set_cpu_affinity` (before init) | `mtl_instance_open` / `mtl_instance_acquire_default` with `mtl_instance_params` (lcores, ports, time source) or MtlManager | configure-before-init | MTL owns scheduler threads (tasklets); Rivermax has one internal thread + app threads |
| device by local IP (`rmx_retrieve_device_iface_ipv4`) | `mtl_port_find(mt, "192.168.1.10" \| BDF \| name, &port)` | select by IP | DPDK-bound VFs may have no kernel IP; the port's configured IP is used |
| output media stream from **SDP** + `set_idx_in_sdp` | `mtl_video_session_create(mt, &sc, &vc, &s)`; an SDP *parse* helper in L4 for -20/-30/-40 is the largest single migration win (Q-MODE-6) | | MTL must not *require* SDP (audio, ANC, fastmeta, tests) |
| three memory configurations: Rivermax-managed / app-allocated / app-registered (mkey) | library pool / `mtl_mem_alloc` or `mtl_mem_import` + attached buffers / pre-mapped import (later) | same three, same runtime verbs | provider-neutral handles instead of mkeys |
| one registration shared by many streams | one region, many sessions; an RX library pool is a region too (`mtl_session_get_pool_region`) | | |
| memory block → sub-block (header/payload split) → chunk → stride | pool → buffer → planes; packet layout is MTL's | | MTL builds RTP headers and packetises; header/payload split is a later memory-domain feature |
| `get_next_chunk` → fill → `commit_chunk(time)` | `mtl_tx_acquire` → fill → `mtl_tx_submit(media time)` | acquire/commit loop on the app thread, no library callbacks | commit sends *the lease you committed* (Rivermax sends the oldest acquired chunk, whatever handle you call it on `[R10 §2.6]`) |
| chunk = N lines, only the first chunk of a frame gets a time, others 0 | progressive rows: `submit(…, ready_rows)` then `mtl_tx_publish(rows)` | chunk ≈ publish step *for timing* | a Rivermax chunk is separate memory with app-written RTP headers; an MTL publish is a row counter over one frame buffer. Apps that DMA lines from a capture card import the frame buffer (05 §2) |
| commit time 0 = "send right after the pending chunks" | `launch.mode = MTL_LAUNCH_DERIVED` with media mode AUTO | back-to-back continuation | |
| commit time = first-packet wire time; app adds TRO; UTC unless the PTP clock is used | media time → MTL derives launch (TROFFSET, read schedule) in TAI; `MTL_LAUNCH_EXACT` for Rivermax-style exact time | exact-time escape hatch exists | MTL knows 2110-21 and computes TRO itself; exact time is flagged `MTL_TX_TIMING_NON_COMPLIANT` |
| commit fails if the time is < ~600 ns ahead; apps pass 0 and `skip_chunks(0)` | late policy DROP / bounded SEND_LATE / RESLOT with a per-unit result and margin; live sources declare `media_time_offset_ns` | | MTL accepts and reports rather than failing close deadlines |
| `RMX_NO_FREE_CHUNK`, `RMX_HW_SEND_QUEUE_IS_FULL`, `RMX_BUSY` | `-MTL_EAGAIN` + `mtl_session_get_status().blocked_on` (`BUFFERS` / `RESULTS` / `RING` / `APP_LEASES`) | non-blocking back-pressure statuses | `APP_LEASES` names the leak Rivermax apps debug by hand |
| `skip_chunks(n)` to repeat or drop frames | underrun policy `REPEAT_LAST` (later); `mtl_tx_release` for an acquired, unsubmitted lease | | explicit policy instead of ring-position arithmetic |
| optional tracking: `mark_chunk_for_tracking(token)`, `poll_for_completion`, HW completion timestamp | results optional (`MTL_COMPLETE_NONE`, the default) with library pools; forced (`ALL`) with attached memory; observed times when available | token ↔ cookie; HW timestamp ↔ `observed_first_tai_ns` | with imported memory the result is how the app learns its buffer is free |
| `cancel_unsent_chunks`, then `destroy_stream` retried while `RMX_BUSY` | `mtl_session_discard_queued` (or `mtl_session_stop(MTL_STOP_FLUSH)`), release held leases, then `mtl_session_destroy`, which defers until leases return and posts `SESSION_RETIRED` instead of returning busy | | |
| RX: ring-strided packet chunks, completion moderation, no release call (valid until ring wrap) | RX frame/field units with explicit `mtl_rx_release`; `.hold` keeps an RX slot alive for TX fan-out; packet-chunk RX is reserved (NG2) | | MTL assembles frames and handles 2022-7; lifetime is explicit, not "until wrap" |
| IPO: HW places packets by sequence; app merges legs with `max_path_differential` | 2022-7 merge inside MTL; the session declares its skew budget (`rx_skew_budget_ns`) | | MTL does the merge |
| `rmx_establish_event_channel` → fd | `mtl_session_get_wait_object` / `mtl_cq_get_wait_object` / `mtl_eq_get_wait_object` | pollable object | an fd on Linux, a `HANDLE` on Windows, one struct |
| clock: system / user callback / NIC PTP (external daemon) | `mtl_instance_params.time_source`: built-in PTP, external PHC, validated `CLOCK_TAI`, SYSTEM_TAI (estimated), user feed | | MTL can run its own PTP client; everything is TAI; `mtl_time_cross_timestamp` maps other clocks |
| `rmx_stats_*` out-of-process consumer | in-process snapshot API; out-of-process reader is Q-OBS-2 | | |
| versioned exports `*_v1`, `_rmx_init(&version)`, init functions for param blocks | an experimental DSO with its own soname and version node, `api_version` per instance, an exported `*_init()` for every struct | | |

## 4. What a Rivermax sender loop becomes

```c
/* Rivermax (Dev Kit style)                        MTL unified: media_mode = INDEX */
for (;;) {                                         for (int64_t k = 0; running; k++) {
  t = t0 + k * frame_period + tro;                   mtl_lease_h l;   /* no TRO math */
  for (c = 0; c < chunks; c++) {                     int ret = mtl_tx_acquire(s, &l, &v, NULL,
    while (get_next_chunk(&h) == NO_FREE_CHUNK);                          MTL_MS(20));
    write_rtp_headers_and_payload(&h);               if (ret < 0) break;
    commit_chunk(&h, c == 0 ? t : 0);                fill_frame(&v);
  }                                                  sub.media_index = k;
  k++;                                               if (mtl_tx_submit(s, l, &sub) < 0) {
}                                                      mtl_tx_release(s, l);
                                                       break;
                                                     }
                                                   }
/* library pool: completion mode NONE by default, so nothing has to be read; set
   MTL_COMPLETE_ALL to get per-unit status, margin and observed times */
```

The MTL loop is shorter because packetisation, RTP headers, TRO and pacing are the
library's job; the Rivermax-style knobs (exact first-packet time, progressive publish)
are there for the users who need them. `v` is a `struct mtl_buffer_view` and `sub` a
`struct mtl_tx_submission`, both set up once with their `*_init()` functions; the compiled
form of this loop is [10 §3](10-api-sketch.md).
