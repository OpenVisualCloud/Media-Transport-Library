# C3 — Usability review: role-playing the personas

| | |
|---|---|
| Lens | Developer experience. Each persona in [01 §2](../01-goals-and-requirements.md) writes its core loop against [10-api-sketch.md](../10-api-sketch.md) and the design docs; this review records where they got stuck |
| Date | 2026-09-29 |
| Reviewed | README, 00, 01, 03, 04, 05, 06, 07, 09, 10, 11, 12, OPEN-QUESTIONS, DECISIONS; research 08, 09, 10 (11 for context) |
| Personas played | P1 first sample · P2 FFmpeg libavdevice and GStreamer plugin maintainer · P3 playout (A/V/ANC, 2022-7) · P4 low-latency live (line mode) · P5 zero-copy GPU/MXL bridge · P6 Rivermax migrant · P7 libfabric developer · Python and Rust binding authors |
| Verdict | The shape is right and much better than today. It is not yet usable as written. Three defaults cause silent stalls or silently wrong output, two zero-copy paths the motivation depends on cannot be written in v1, and the examples in 10 use about a dozen structs and fields that no other document defines |

Severity: **blocker**: the persona cannot write a correct loop, or the obvious loop fails silently. **major**: it can be written, but only with a workaround, new boilerplate, or a trap most users will hit. **minor**: naming, consistency, or documentation.

Finding IDs are `<persona>-<n>`. "OQ" marks a proposed new open question.

---

## P1 — first TX/RX sample

### P1-1 — The recommended completion default stalls the minimal loop silently (blocker)

- **Where:** [07 §2](../07-completions-events-and-errors.md) credit rule; Q-CMP-1 recommendation (a) "default ALL"; [10 §2](../10-api-sketch.md) line `sc.completion.mode = MTL_COMPLETE_NONE`; [12 §4](../12-familiarity-libfabric-and-rivermax.md) MTL loop.
- **Issue:** the minimal TX example only works because it sets a non-default mode. Delete that one line (or copy the 12 §4 loop, which never reads a CQ) and this happens:
  - capacity is the pool size (3), so after three frames finish, `mtl_tx_acquire` has no credit;
  - it waits for its timeout and returns `-ETIMEDOUT`, and the example's `if (ret == -ETIMEDOUT) continue;` spins forever with nothing on screen;
  - the `BACKPRESSURE` event goes to the instance EQ, which a P1 app never reads.

  This is exactly the "session dies in silence" pattern that bobi.studio patched around (R08 §3.3), now caused by the default.
- **Suggested change:**
  - Take Q-CMP-1 option (c): `NONE` by default for `MTL_POOL_LIBRARY`, `ALL` forced for `MTL_POOL_ATTACHED`, where the result is the only way to learn a buffer is free and 07 §2.1 already forbids `NONE`.
  - Make acquire tell the app *why* it is blocked. Either use a distinct code for "no CQ credit" versus "no free slot", or add `blocked_on` to `mtl_session_get_status`.
  - Have the admin thread (not the tasklet) log one WARNING when a session has been credit-blocked for more than 1 s.
  - Fix 12 §4 so it either reads results or sets `NONE`.
- **OQ:** Q-CMP-1a — should `acquire` return a different code for credit starvation than for slot starvation? (Rivermax separates `RMX_NO_FREE_CHUNK` from `RMX_HW_SEND_QUEUE_IS_FULL`; see P6-3.)

### P1-2 — The "10-line loop" starts after the hardest part (major)

- **Where:** 01 §2 P1 "10-line TX/RX loop; library allocates everything"; 10 §2 starts from an existing `mt`; Q-ARCH-2 (reuse `mtl_init`); [02 §3](../02-architecture.md) says `mtl_instance_create`, [03 §7](../03-object-model-and-lifecycle.md) and [11 §4](../11-abi-compatibility-and-migration.md) say `mtl_init`.
- **Issue:** a first sample still has to fill today's `mtl_init_params`: `port[][64]` strings, `sip_addr`, `num_ports`, lcores, flags and PMD types. That is the part samples copy wrongly (R08 B4, #687, #1179). The documents also disagree on which instance call exists.
- **Suggested change:**
  - Pick one instance entry point and use it in every document.
  - Add `mtl_instance_open_simple(const char* port_spec, mtl_handle* out)`, where `port_spec` is `"0000:af:01.0=192.168.1.10,0000:af:01.1=192.168.2.10"`, or honour an `MTL_PORTS` environment variable.
  - Show the instance call inside the P1 example so the line count is honest.

### P1-3 — A compound-literal flow erases every default `*_init()` set (major)

- **Where:** 10 §2 `sc.flows[0] = (struct mtl_flow){ .struct_size = …, .port = 0, .ip = …, .udp_port = 20000, .payload_type = 112 }`; [09 §1.1](../09-media-modes-and-backends.md) `struct mtl_flow`.
- **Issue:** assigning a compound literal zero-fills every field it does not name, so any defaults `mtl_session_config_init` put into `flows[]` are gone:
  - `ttl = 0`: a multicast packet with TTL 0 never leaves the host;
  - `ip_family = 0`, `udp_src_port = 0`, `ssrc = 0`, `dscp = 0`, `dst_mac = 00:…` and `flow_flags = 0` also lose their defaults.

  The idiom the design itself uses therefore defeats its own defaults mechanism.
- **Suggested change:**
  - In every sub-struct an application is likely to build as a literal (flow, submission, start params, mem desc, plane desc), make zero mean "library default", and document it on each field: `ttl 0 → 64`, `ip_family 0 → IPv4`, `udp_src_port 0 → udp_port`, `ssrc 0 → random on TX / no check on RX`.
  - Scope R-TIME-1's "validity never by zero" to *time values*, where it belongs.
  - Alternatively, provide `mtl_flow_init()` and never show literal assignment in examples.

### P1-4 — `struct_size` ceremony, and a zero `struct_size` is silently accepted (major)

- **Where:** 10 §2 (three `.struct_size = sizeof(struct …)` literals in an eight-line loop); [11 §2.1](../11-abi-compatibility-and-migration.md) rule 2 "the library reads `min(struct_size, sizeof(known))`; fields beyond the caller's size take defaults"; [06 §10.2](../06-timing-pacing-and-sync.md) start-params literal without `struct_size`.
- **Issue:**
  - A forgotten `struct_size` is 0, so under rule 2 the library reads nothing and uses all defaults. A flow is ignored; a submission with a media index becomes AUTO. There is no error.
  - The design's own 06 §10.2 example makes this mistake.
  - Zero-initialised structs from SWIG `new_*()` and Rust `Default::default()` hit it every time.
  - The boilerplate itself is large for P1: `&(struct mtl_tx_submission){ .struct_size = sizeof(struct mtl_tx_submission) }` just to say "defaults".
- **Suggested change:**
  - Return `-EINVAL` when `struct_size` is 0 or smaller than the v1 size.
  - Accept `NULL` for the submission (AUTO, no cookie) and for the start params (START_NOW).
  - Ship initialiser macros, for example `#define MTL_TX_SUBMISSION(...) ((struct mtl_tx_submission){ .struct_size = sizeof(struct mtl_tx_submission), __VA_ARGS__ })`, and the same for `MTL_START`, `MTL_FLOW`, `MTL_MEM_DESC` and `MTL_BUFFER_DESC`.

### P1-5 — `submit` and `dequeue` are classed DP but convert pixels in v1 (major)

- **Where:** [04 §3](../04-threading-and-execution.md) DP class ("lock-free, O(1) … bounded time"); [05 §6.3](../05-memory-and-buffers.md) (TX conversion "inside `mtl_tx_submit`", RX conversion "inside `mtl_rx_dequeue`"); 10 §2 (`transport_format = YUV422_10BIT`, `app_format = YUV422_PLANAR10LE`).
- **Issue:**
  - The P1 example asks for a conversion, so `mtl_tx_submit` takes milliseconds of AVX work. That contradicts its call class and the 04 §10 table, and a P4 developer reading "DP" will put it in a latency-critical loop.
  - The example then renders only `v.plane[0]` of a three-plane format, which draws a wrong picture.
- **Suggested change:**
  - Default `app_format` to the transport format, so P1 gets no conversion.
  - Add a class annotation `MTL_API_DP_CONVERT` ("DP unless the granted `convert_context == CALLER`; then O(frame)") and report the per-call cost in stats.
  - Fix the example to use a one-plane format, or to loop over planes.

### P1-6 — Timeout units and sentinels (minor)

- **Where:** 10 §2 (`100 * 1000 * 1000`, `1000 * 1000 * 1000`); 10 §1 comment "timeout > 0 → WT".
- **Issue:**
  - `int` arithmetic overflows at 3 s: `5 * 1000 * 1000 * 1000` is undefined behaviour before it ever becomes `int64_t`.
  - What a negative timeout means is not defined. libfabric users expect negative to mean infinite (R09 §5.1).
- **Suggested change:** add `MTL_MS(x)` / `MTL_SEC(x)` returning `int64_t`, `MTL_TIMEOUT_INFINITE (-1)`, and `0` = non-blocking. State all three in 04 §3.

### P1-7 — The RX default shows the previous frame's pixels on packet loss (minor)

- **Where:** 05 §5 `rx_fill` lists `MTL_RX_FILL_NONE` first (the implied default); the repository convention is "RX frame buffers must be zero-initialized — gaps from lost packets must read as zeros".
- **Issue:** a P1 viewer shows ghosting on loss, and nothing tells the developer why.
- **Suggested change:** default `ZERO_MISSING` for library pools; make `NONE` the opt-in for apps that read the loss map.

### P1-8 — No minimal RX example, and no "P1 surface" (minor)

- **Where:** 10 §5 is framework-shaped (hand-off across threads); nothing shows a ten-line RX that releases inside the loop.
- **Issue:** P1 can see about 16 policy enums (pool source, data path, `rx_overflow`, `rx_fill`, completion mode, media mode twice, late, underrun, launch, four capability tri-states, wake mode, `progressive_late`). All have defaults, but nothing says which ones a first sample may ignore.
- **Suggested change:** add a minimal RX example. In 10, list the "P1 surface": the roughly 12 calls and 8 fields a first sample needs. Put the rest of the header under an "advanced" section.

---

## P2 — FFmpeg libavdevice and GStreamer plugin maintainer

### P2-1 — Cancel cannot implement `unlock` / `unlock_stop` (blocker)

- **Where:** 04 §5.4; 03 §3.4; 10 §8; Q-LIFE-6; 11 §5 GStreamer row ("`unlock()` via `cancel_waiters`").
- **Issue:** `GstBaseSrc::unlock` must make the current *and every later* blocking call return until `unlock_stop`. The design says only that cancel "sets a cancel flag and wakes every waiter". Both readings fail:
  - **If cancel is edge-triggered:** `unlock` can land just before `create()` enters `mtl_rx_dequeue`, which then sleeps for its whole timeout. That is today's GStreamer bug (R08 §1.2) in a new form. The 10 §8 SIGINT handler has the same race between `while (!stop)` and `acquire`.
  - **If cancel is sticky:** no call clears it, so after the first flush every wait returns `-ECANCELED` forever.
  - **Scope is also wrong.** Cancel wakes "the session's CQ/EQ", but the default EQ belongs to the instance, so cancelling element A wakes element B's event thread.
- **Suggested change:**
  - Make cancel sticky, and add `mtl_session_resume_waiters(s)` for `unlock_stop`. While the flag is set, every WT call on the session returns `-ECANCELED` immediately.
  - Add per-waitable `mtl_cq_cancel_waiters(cq)` and `mtl_eq_cancel_waiters(eq)`, and keep the session call as a convenience that touches only the session's private objects.
  - Keep AS-safety for the set operation only.
- **OQ:** Q-LIFE-6a — is cancel sticky, and what clears it?

### P2-2 — An attached buffer cannot be submitted by name (blocker for zero-copy TX)

- **Where:** 10 §4 ("runtime is identical to §2"); 03 §4.1 (`acquire` picks a FREE slot); 05 §2.
- **Issue:**
  - With an imported framework pool, the sink receives *one specific* GstBuffer or AVFrame that maps to one specific `mtl_buffer_h`, and must send *that* buffer.
  - `mtl_tx_acquire` returns whichever slot MTL chooses, so the only way to send the frame is to copy it into the returned buffer. The import path in 10 §4 therefore gives no zero-copy for a framework pool.
  - What remains is export: wrap the library pool as a GstBufferPool.
- **Suggested change:** add `mtl_tx_acquire_buffer(s, buf, timeout_ns, &view, &hint)`. It leases a named attached buffer, returns `-EBUSY` if that buffer is not FREE, and behaves like `acquire` otherwise. The lease table is already indexed by slot, so the cost is small. (P5-3 builds on the same call.)

### P2-3 — FFmpeg TX zero-copy is still impossible in v1 (major)

- **Where:** 11 §5 FFmpeg row ("zero-copy via region import"); Q-MEM-10 (dynamic buffers later).
- **Issue:** libavdevice muxers receive AVPackets whose data is allocated with `av_malloc` per packet, or AVFrames from arbitrary pools via `write_uncoded_frame`. Neither can be pre-attached before start. Only RX zero-copy is achievable, and it is clean: `dequeue` → `av_buffer_create(…, free_cb = mtl_rx_release)`.
- **Suggested change:** correct 11 §5. Then either:
  - bring a narrow Q-MEM-10 into v1: "create a buffer on the fly over an already-imported region and submit it unattached", which covers FFmpeg frame pools allocated from one imported arena; or
  - state plainly that FFmpeg TX copies in v1.

### P2-4 — `destroy` returns `-EBUSY` while downstream still holds RX buffers (major)

- **Where:** 03 §6.3; Q-LIFE-4 recommendation (a).
- **Issue:** `GstBaseSrc::stop` and FFmpeg `read_close` must return, but queues, encoders and `last-sample` can still hold dequeued buffers. With option (a) the element cannot finish stopping; with `FORCE`, library-pool memory is freed under downstream (use after free). Every plugin will write its own session refcount plus deferred destroy, which is new boilerplate (call it B13).
- **Suggested change:** add option (c), deferred destroy, and make it the default:
  - `destroy` stops the session, retires the handle for all calls except `mtl_rx_release` and `mtl_tx_abort`, and frees the pool when the last lease returns;
  - a `SESSION_RETIRED` event (or a waitable) reports the final free.

  This is how GstBufferPool and AVBufferPool behave.
- **OQ:** add option (c) to Q-LIFE-4.

### P2-5 — No fd signals "acquire would succeed", and eventfd draining is unspecified (major)

- **Where:** 04 §5.3; 07 §2.1; 10 §3.
- **Issue:**
  - Wait fds belong to CQs. In `NONE` mode — the mode P1-1 recommends for library pools and the natural one for a GstBufferPool export — there are no entries, so nothing wakes an epoll loop, GstPoll, asyncio or a Rust `AsyncFd` when a TX slot frees.
  - Nothing says who drains the eventfd. If the app must `read()` it, 10 §3 busy-loops on a level-triggered epoll. If `mtl_cq_read` or `trywait` drains it (libfabric's semantics), the documents do not say so.
- **Suggested change:**
  - Add `mtl_session_get_wait_fd(s, MTL_WAIT_ACQUIRE | MTL_WAIT_DEQUEUE | MTL_WAIT_RESULTS, &fd)` plus `mtl_session_trywait(s, mask)`, or define that the session's CQ fd also fires on "a slot became FREE".
  - State that `read` and `trywait` drain the counter.

### P2-6 — The instance singleton (B5) is not absorbed (major)

- **Where:** R08 §2 B5 and §4.1; Q-ARCH-2; Q-LIFE-2; 03 §7; 07 §3.4 (one default EQ per instance).
- **Issue:**
  - The unified API keeps `mtl_init` with ports fixed at init. Two elements with different ports still need the memcmp-compatibility singleton that FFmpeg and GStreamer carry today.
  - Because the default EQ is per instance, two elements that both read it steal each other's events.
  - P2's must-have row in 01 §2 cannot be met without this.
- **Suggested change:**
  - Add `mtl_instance_acquire_default(&params, &mt)`: refcounted, it merges port lists, or opens ports later with `mtl_port_open(mt, "0000:af:01.1")`. Release with `mtl_instance_release`.
  - Route session events to a private per-session EQ by default, with the instance EQ carrying only instance and port events.
  - Move Q-LIFE-2 (b) to **B** priority.

### P2-7 — Caps negotiation before create is only partial (major)

- **Where:** 01 §2 P2 "caps query before create"; 05 §4.1 (`mtl_video_layout_query`, `get_buffer_requirements` only after create); 09 §6.1; 10 §4 (`assert(N >= req.min_count_direct)` *after* creating with `pool.count = N`).
- **Issue:**
  - `get_caps` / `negotiate` / `decide_allocation` need, *before* a session exists: the supported `(transport_format, app_format)` pairs, whether a direct path is possible for a layout, and `min_count_direct`.
  - Today's route is create → query → destroy → re-create, and create may do ARP and TM work (Q-LIFE-9).
- **Suggested change:**
  - Add a dry run, `mtl_video_session_query(mt, &sc, &vc, &info, &req)`. It validates the config, returns granted values and buffer requirements, and allocates nothing. It is also the `fi_getinfo` analogue P7 expects (P7-1).
  - Add `mtl_video_format_enum(i, &pair)` for conversion pairs.
  - Allow `pool.count` to change in CREATED.

### P2-8 — Framework timestamps hit hard `-EINVAL`, and there is no flush (major)

- **Where:** 06 §4.1 rules (strictly increasing media time, `-EINVAL` otherwise; TAI "snapped to the nearest unit").
- **Issue:**
  - GStreamer PTS plus base time, and FFmpeg `av_rescale_q` output, jitter. Two frames that snap to the same unit make `submit` fail with `-EINVAL`, which a sink turns into `GST_FLOW_ERROR` and the pipeline dies.
  - After a seek or `FLUSH_STOP`, PTS goes backwards. There is no `flush` call, only stop(ABORT) + start, which may release NIC resources (Q-LIFE-1).
  - The GStreamer pipeline clock is usually monotonic, but the API gives TAI with no conversion.
- **Suggested change:**
  - Accept a duplicate slot after snapping and give it a `DROPPED` / `DUPLICATE_SLOT` result, or apply RESLOT when that policy is chosen. Keep `-EINVAL` for genuinely backwards times without DISCONTINUITY.
  - Add `mtl_session_flush(s)`: QUEUED TX becomes FLUSHED and READY RX is discarded; the session stays RUNNING; the next submission is an implicit DISCONTINUITY.
  - Add `mtl_time_convert(mt, &t, MTL_CLOCK_MONOTONIC | MTL_CLOCK_REALTIME, &out)`. The `clock` tag already exists in `struct mtl_time`.

### P2-9 — A/V in separate elements or muxers cannot share a timeline (major)

- **Where:** 06 §10; 10 §6.
- **Issue:** FFmpeg `-f mtl_st20p` and `-f mtl_st30p` are separate AVFormatContexts, and GStreamer has separate sinks that reach PLAYING at different moments.
  - Timelines and groups are handles an owner passes around; nothing can look one up by name.
  - Group start needs every member added first.
  - On the default epoch timeline, `index = pkt.pts` (starting at 0) means 1970, so every unit is DROPPED as TOO_LATE.

  The maintainer's A/V question gets a clean answer only for a single-owner playout server (P3), not for the framework plugins that motivated it.
- **Suggested change:**
  - Add named, refcounted timelines: `mtl_timeline_open(mt, "prog1", &cfg, &tl)`, where the first opener creates it.
  - Document the framework pattern: "sessions on one named timeline start independently with `AT_MEDIA_INDEX`; no group needed".
  - Keep groups for owners of all sessions.

### P2-10 — Smaller P2 items (minor)

- **FFmpeg interrupt claim.** 04 §5.4 says FFmpeg's interrupt callback "needs exactly" `cancel_waiters`. In fact `AVIOInterruptCB` is *polled* by the demuxer (`ff_check_interrupt`); the fit is a short timeout plus a check. `AVFMT_FLAG_NONBLOCK` → `dequeue(0)` → `-EAGAIN` → `AVERROR(EAGAIN)` works as designed. Correct the claim.
- **Thread-id assertion.** The debug assertion in Q-THR-8 / 04 §4 ("record the thread ID on first use") breaks a legitimate GstBufferPool export: `acquire_buffer` runs in the upstream thread and `submit` in the sink's streaming thread.
  - State the contract as "externally serialised".
  - Have the assertion detect *overlap* (an owner flag set with CAS for the duration of the call), not a different thread.
- **Export-pool drops.** Upstream may drop an acquired export-pool buffer (QoS, flush). The pool's `release_buffer` must call `mtl_tx_abort`. Document this mapping in 11 §5.
- **RX pool size.** RX pool count is fixed at create, but downstream states `min_buffers` in the allocation query, and the pool is capped at 8 (Q-MEM-5). Adopt Q-MEM-5 (b) and allow count changes in CREATED.
- **RX_FORMAT renegotiation.** Document the sequence (stop → detach → re-create buffers → attach → start) and whether flows and stats survive it.
- **Latency query.** The `latency range {min,max}` in `mtl_session_get_info` (08 §3) answers LATENCY queries well. Keep it, and add the RX `delivered − media` expectation to it.

---

## P3 — Playout server: A/V/ANC from files with ST 2022-7

### P3-1 — The headline A/V example anchors its timeline in the past (blocker)

- **Where:** 06 §3.1 (`MTL_ANCHOR_NEXT_GRID`, `grid = 0/0` "derive from member rates"), §3.3, §10.3; 10 §6.
- **Issue:** 10 §6 creates the timeline *before* any session exists, so three things go wrong:
  - `grid = 0/0` cannot be derived from member rates that are not known yet;
  - `tai_ns` is left at 0, and it is documented as the NEXT_GRID lower bound, so T0 can be the epoch and index 0 is 1970 — every unit is DROPPED;
  - even with `tai_ns = now`, creating three sessions (ARP, RL training, flow rules; often seconds) puts T0 in the past before `mtl_group_start`, and "first timeline instant ≥ now + lead" contradicts `START_AT_MEDIA_INDEX, .media_index = 0`.
- **Suggested change:** add `MTL_ANCHOR_AT_START` and make it the default for created timelines. T0 is fixed by the first group or session start as the next common-grid point ≥ now + max member lead + preroll, so index 0 is always the first unit. Report T0 through `chosen` and `mtl_timeline_get_anchor`. Keep `AT_TAI` / `NEXT_GRID` for NMOS-scheduled starts.

### P3-2 — ANC has no way to submit or receive its packet table (major)

- **Where:** 05 §9 ("the meta table lives in the submission/result"); 06 §4.1 `struct mtl_tx_submission`; 07 §1.2 `struct mtl_rx_unit`; 09 §2 ("UDW bytes + packet table").
- **Issue:** neither struct has DID/SDID/line/horizontal offset/C-bit/stream fields, a pointer, or a count. The ANC branch of the maintainer's A/V/ANC question cannot be written.
- **Suggested change:**
  - Define `struct mtl_anc_packet { did, sdid, line, hoffset, c, stream, udw_offset, udw_count }`.
  - On TX, add `const struct mtl_anc_packet* anc; uint32_t anc_count;` to the submission, copied at submit.
  - On RX, put the table in the buffer's meta area and its count in the unit.

### P3-3 — Audio capacity and re-framing (B11) are only half-absorbed (major)

- **Where:** 06 §8; 10 §6 audio branch; `mtl_audio_config` defined nowhere.
- **Issue:**
  - **Capacity:** nothing shown sets the byte capacity of a library audio buffer. `memcpy(v.plane[0].addr, pkt.data, pkt.size)` overflows when a demuxed packet exceeds it, and splitting by hand is the re-framing B11 set out to remove.
  - **Redundant fields:** both `valid_bytes` and `sample_count` are set, plus `MTL_SUB_SAMPLES`; which one wins on a mismatch is undefined.
  - **Partial packets:** because a partial packet carries over, submission *n*'s buffer and result are held until *n+1* arrives, and the DRAIN behaviour for the tail (pad with silence or drop) is unspecified.
- **Suggested change:**
  - Add `mtl_tx_write(s, const void* data, size_t bytes, const struct mtl_media_time* m, int64_t timeout_ns)` for ST30/40/41. These essences copy anyway; this is the `FI_INJECT` analogue that 12 §2 already names. The library acquires, splits and submits.
  - Make `sample_count` authoritative for PCM and derive bytes from it.
  - Define the tail: pad with silence at DRAIN, or `MTL_SUBMIT_FLUSH_PARTIAL`.

### P3-4 — Media mode lives in two places, and the late-policy default depends on the per-unit one (major)

- **Where:** 06 §7.2 ("DROP default when media time is INDEX/TAI; RESLOT default for AUTO"); 09 §1 timing config "media mode default"; 06 §4.1 per-submission `media.mode`; 10 §6 sets both.
- **Issue:** a session that receives both AUTO and INDEX submissions has no defined late policy, and nothing says which mode wins when the session says INDEX and a submission omits `MTL_SUB_MEDIA`.
- **Suggested change:** make media mode a session property only (AUTO | INDEX | TAI). The submission carries only the value (`index` or `tai_ns`). Derive the late-policy default from the session mode at create and report it as granted. A mismatch is `-EINVAL`.

### P3-5 — Forgetting the `valid` bit silently sends AUTO timestamps (major)

- **Where:** 06 §4.1 `uint64_t valid`; 10 §6 and §7 (`.valid = MTL_SUB_MEDIA` *and* `.media = { MTL_MEDIA_INDEX, … }`).
- **Issue:** the mode is stated twice. If the bit is forgotten, the library ignores `.media` and stamps AUTO. The RTP is then silently wrong for the one persona the timing design exists for.
- **Suggested change:**
  - Remove `valid` bits for fields that carry their own mode enum (`media`, `launch`). Mode 0 = AUTO / DERIVED is a named default, not a "magic zero".
  - Keep `valid` only for plain numbers such as `rtp_override`.
  - Reject any non-zero field whose `valid` bit is clear with `-EINVAL`.

### P3-6 — Smaller P3 items (minor)

- **`leg_count` versus `flows[]`.** Forgetting `leg_count = 2` silently sends one leg. Derive the count from `flows[i].struct_size != 0`, or reject a populated `flows[1]` when `leg_count == 1`.
- **Preroll.** Preroll depth is 8 buffers (133 ms at 59.94) until Q-MEM-5. Submissions before start on an unanchored timeline cannot be checked against the horizon; state that those checks are deferred to start.
- **Playlist edits.** There is no per-unit cancel for QUEUED units (libfabric has `fi_cancel`). Add `mtl_tx_cancel(s, buf)`, which turns a QUEUED unit into a `FLUSHED` result.
- **`mtl_group_start` arity.** It has three arguments in 10 §1 and §6 but two in 06 §10.2.

---

## P4 — Low-latency live contribution (line mode)

### P4-1 — The progressive example never publishes FINAL (major)

- **Where:** 10 §7: `for (rows = 128; rows <= 1080; rows += 64) … mtl_tx_publish(s, b, rows, rows == 1080 ? MTL_PUBLISH_FINAL : 0)`.
- **Issue:** 1080 is not a multiple of 64. The last iteration publishes 1024 and the loop exits, so FINAL is never sent and the unit hits `progressive_late` (STALL / PAD / TRUNCATE).
- **Suggested change:** fix the loop (`rows = MIN(rows + 64, height)`). Make `publish(ready_rows == unit rows)` imply FINAL and drop the flag, so there is only one way to finish a unit.

### P4-2 — Progressive mode has no create-time switch, and the v1 adapter cannot provide it (major)

- **Where:** 06 §9; 09 §2 (progressive "maps to today's slice mode"); 02 §2.2 (ST20 TX wraps `st20p`).
- **Issue:** today slice mode is a session *type* chosen at create (`ST20_TYPE_SLICE_LEVEL`), and `st20p` has none. A per-submission `MTL_SUBMIT_PROGRESSIVE` cannot switch it on, and the planned adapter cannot implement it.
- **Suggested change:** add a config field (`timing.progressive` or `unit = MTL_UNIT_ROWS`) and report it as granted. The submit flag then only asserts it. Record in Q-ARCH-1 that P4 in v1 needs a session-layer adapter for ST20.

### P4-3 — Per-row deadlines are not exposed (major)

- **Where:** 06 §7.6 slot hint (one `submit_deadline` per unit); 06 §9.
- **Issue:** a line-mode producer needs "rows [0, r) must be published by t(r)". Computing that needs TRS, VRX and packets per row from `get_info`, which is B7-style arithmetic again.
- **Suggested change:** add an L4 helper `mtl_session_row_deadline(s, const struct mtl_slot_hint*, uint32_t row, struct mtl_time* t)` that uses the same exact math as the tasklet.

### P4-4 — RX progressive has two wait mechanisms and no defined final state (major)

- **Where:** 06 §9 RX; 07 §1.3 `MTL_CQE_RX_PROGRESS`; 10 §1 `mtl_rx_wait_progress`.
- **Issue:**
  - There are two ways to learn about new rows: a per-buffer WT call, and CQ progress entries.
  - When a unit is final, and what its terminal status (COMPLETE / INCOMPLETE) is on the progress path, is not defined; "the app releases after FINAL" names no field.
  - epoll users can only use the CQ variant.
- **Suggested change:** use one mechanism, RX_PROGRESS CQ entries, with `MTL_CQE_FINAL` in `hdr.flags` and the terminal status on the final entry. `mtl_rx_wait_progress` becomes a convenience over it.

### P4-5 — Smaller P4 items (minor)

- **INDEX in 10 §7.** Example 10 §7 takes an INDEX from the hint. That makes DROP the default, so a unit that is one row late vanishes. For live sources AUTO + RESLOT is the better default, and the result still reports `media_index` and `slots_skipped_before`. Say so in the example.
- **Waker latency.** The W3 waker adds 20–50 µs per wake, several line times at 1080p60. Document that P4 should busy-poll (W0) on its own core, or use `MTL_SESSION_WAKE_DIRECT`.
- **Slot delay.** `tx_slot_delay` defaults to 0, which suits playout. A live-capture preset (`L = 1`) would save P4 a tuning step.

---

## P5 — Zero-copy GPU pipeline and MXL bridge

### P5-1 — RX cannot place unit *k* into grain *k* (blocker for the MXL bridge)

- **Where:** 05 §8 ("pre-attached pools cover the same use without code on the tasklet"); 03 §4.2; R08 §1.2 MXL.
- **Issue:** an MXL flow is a ring where grain `k mod N` holds media index `k`, and the POC picked the grain in `query_ext_frame` from the frame's timestamp. With a pre-attached pool MTL fills any FREE slot, so unit *k* lands in an arbitrary grain. The bridge must then copy, or rename grains, which MXL readers cannot follow. The claim in 05 §8 does not hold for the design's own P5 example.
- **Suggested change:** add a pool option `rx_slot_select = MTL_RX_SLOT_BY_INDEX` (slot = `media_index mod count`; if the app holds that slot, `rx_overflow` applies). It is deterministic and runs no code on the tasklet. The alternative is `mtl_rx_provide(s, buf, media_index)` in v1.

### P5-2 — Shared memory is treated like page-cache files (major)

- **Where:** 05 §3.2, last row ("File-backed `mmap` of page-cache pages … reject for direct").
- **Issue:** MXL domains are tmpfs `/dev/shm` mappings, and memfd and hugetlbfs are the usual IPC surfaces. As written, the row covers shmem, which VFIO can pin long-term. It would push P5's main target onto COPY.
- **Suggested change:** split the row. shmem, memfd and hugetlbfs are mappable (pin + `dma_map`). Mappings of regular files are COPY-only. Report the result in `mtl_mem_get_info`.

### P5-3 — Forwarding one buffer between two sessions is undefined (major)

- **Where:** 03 §1 (buffer destroy blocked "while attached"); 05 §2.
- **Issue:** a split-forward (RX session A → TX session B, zero-copy) needs either an RX lease that can be submitted on another session, or one buffer attached to two sessions with a single global lease. The documents define neither.
- **Suggested change:** allow a buffer to be attached to several sessions, with its lease exclusive across all of them. Add `mtl_rx_transfer(s_rx, buf, s_tx)`, which turns an APP_READING RX lease into an APP_WRITABLE TX lease without passing through FREE. It pairs with P2-2.
- **OQ:** Q-MEM-11 — cross-session buffers and lease transfer in v1?

### P5-4 — Buffer views for device memory (minor)

- **Where:** 10 §1 `mtl_buffer_get_view` ("CPU pointers per plane"); 05 §10.
- **Issue:** device and DMA-BUF domains are reserved, but the view has no way to say "no CPU address".
- **Suggested change:** reserve `view.plane[i].addr = NULL`, plus `domain` and a device handle, in the v1 view struct now.

---

## P6 — Rivermax migrant

### P6-1 — Reading completions becomes mandatory, and 12 §4 stalls (major)

- **Where:** 12 §3 row "results are mandatory because they carry buffer ownership"; 12 §4 MTL loop; R10 §4 ("There is no *mandatory* completion").
- **Issue:** Rivermax apps learn a chunk is reusable because `get_next_chunk` hands it back, and they track completions only optionally. A migrant who copies 12 §4 stalls after `pool.count` frames (P1-1). 12's claim "same acquire/commit loop" holds only after the migrant adds a CQ reader.
- **Suggested change:** fix 12 §4. In 12 §3, state "optional (`NONE`) with library pools; mandatory with imported memory".

### P6-2 — `-EAGAIN` hides which resource ran out (minor)

- **Where:** 12 §3 (`RMX_NO_FREE_CHUNK`, `RMX_HW_SEND_QUEUE_IS_FULL`, `RMX_BUSY` → `-EAGAIN` "+ counters").
- **Issue:** migrants debug by status, not by stats. MTL folds three resources (slot, credit, ring) into one code.
- **Suggested change:** add a `blocked_on` field (see P1-1), or an optional out-parameter on `acquire`.

### P6-3 — Familiarity claims in 12 that need a qualifier (minor)

- **"Chunk ≈ publish step".** This holds for *timing* only. A Rivermax chunk is separate memory with RTP headers written by the app; an MTL publish is a row counter over one frame buffer. Say so, and point apps that DMA lines from a capture card at "import the frame buffer" (05 §2).
- **Commit time 0.** "Commit time 0 = send right after the pending chunks" corresponds to `LAUNCH_DERIVED` with AUTO media, not to EXACT. Add this row to the table.
- **Teardown.** `cancel_unsent_chunks` then destroy-retry-on-`RMX_BUSY` maps well to abort-each-lease then destroy. But with Q-LIFE-4 (a) it becomes "retry until not `-EBUSY`", so P2-4's deferred destroy helps migrants too.
- **SDP.** SDP is the Rivermax entry point (R10 §12, top of the list), and Q-MODE-6 defers it to values only. A parse helper in L4 (not in `lib/` core) for -20/-30/-40 is the largest single migration win. Consider it for v1.

---

## P7 — libfabric-familiar developer

### P7-1 — There is no `fi_getinfo`-style dry run (major)

- **Where:** 12 §2 first row (`mtl_port_get_caps` + `mtl_capability_request` ≈ `fi_getinfo`).
- **Issue:** `fi_getinfo(hints)` returns what the app *would* get before anything is opened. MTL reports granted values only after create or start, and create is heavy. The analogy is weaker than the table claims.
- **Suggested change:** add the dry-run query from P2-7, and make it the documented `fi_getinfo` counterpart.

### P7-2 — CQ entry format and size negotiation contradict each other (major)

- **Where:** 07 §1 ("entry size is fixed per CQ at creation … `struct_size` negotiation"); 10 §1 `mtl_cq_read(cq, void* entries, uint32_t max, uint32_t entry_size)`; private CQs are created implicitly; `struct mtl_cq_entry_hdr` has no size field; 10 §3 reads `struct mtl_tx_result[]` from a CQ that may also hold `TX_SOURCE_RELEASED` entries, or `RX_UNIT` entries if shared.
- **Issue:**
  - For an implicit private CQ, who fixes the entry size, and when? It cannot be fixed at creation, because the app never creates it.
  - What happens when `entry_size` differs from the CQ's fixed size is undefined.
  - A shared CQ that mixes kinds needs one container type.

  libfabric solves this with prefix-compatible `FI_CQ_FORMAT_*` structs chosen at creation (R09 §5.1).
- **Suggested change:**
  - Define `union mtl_cq_entry` with a documented fixed size (for example 256 B) and `hdr.kind` + `hdr.size`.
  - Make the signature `mtl_cq_read(cq, union mtl_cq_entry* e, uint32_t max)`.
  - Keep the typed per-session convenience calls (`mtl_tx_reap`, `mtl_rx_dequeue`) for single-kind use.
  - Use `hdr.size` for growth across versions.

### P7-3 — Smaller P7 items (minor)

- **Timeout position.** `acquire` and `dequeue` take the timeout second; `reap`, `cq_wait`, `stop` and `group_stop` take it last; and 07 §2.2 writes `mtl_rx_dequeue(s, &buf, &unit, timeout)`. Put it last everywhere, matching `fi_cq_sread`.
- **Two ways to bind a CQ.** Config `completion` "CQ/EQ binding" (09 §1) and `mtl_session_bind_cq` (10) do the same job, and 07 §2 also calls it `mtl_cq_bind`. libfabric has one verb (`fi_ep_bind`). Keep only the bind call; the config carries the mode.
- **`FI_THREAD_COMPLETION` wording.** It serialises per CQ; MTL serialises submits per session and reads per CQ. Word the 12 §2 row that way so readers do not assume one thread per shared CQ is required for submits too.
- **Two cookies.** A TX result's `hdr.user_cookie` comes from the submission. When the submission leaves it at 0, say whether the buffer's `user_cookie` is used instead. libfabric has one context per operation; MTL has two per TX unit.

---

## Python and Rust binding authors

### B-1 — Nested growable structs break the ABI rule and SWIG ergonomics (major)

- **Where:** 11 §2.1 rule 4 ("No public struct embeds another *growable* struct by value except as its last member"); 09 §1 `mtl_session_config` embeds `flows[MTL_MAX_LEGS]`, `pool`, `timing`, `completion` and `caps`, each with its own `struct_size`, none of them last.
- **Issue:**
  - The config violates the design's own ABI rule: growing `mtl_flow` moves every later field.
  - SWIG exposes nested arrays of structs awkwardly, so the Python-only setter helpers R08 criticised (`mtl_para_*_set`) come back.
- **Suggested change:**
  - Freeze the size of nested sub-structs. Give each a reserved tail (for example `uint64_t reserved[4]` that must be zero) and no `struct_size` of its own, so it versions with the parent.
  - Or move flows out of the config with `mtl_session_config_add_flow(&sc, &flow)`.
  - Add `mtl_flow_parse("239.168.85.20:20000", &flow)`. Strings bind trivially and map directly to GStreamer properties and FFmpeg AVOptions (R08 Q11 (c)).

### B-2 — Zero-initialised structs from bindings hit the silent `struct_size = 0` path (major)

- **Where:** 11 §2.1 rule 2 (see P1-4).
- **Issue:** Rust `Default::default()` and SWIG `new_*()` zero-fill. Every such struct is read as "caller knows no fields".
- **Suggested change:** reject `struct_size == 0` (P1-4). Document that bindings must call the exported `*_init()` functions, and keep those as real exported symbols (11 §3 already requires this for `static inline`).

### B-3 — Smaller binding items (minor)

- **Untyped CQ reads.** `mtl_cq_read(void*, …)` needs a hand-written typemap in every binding. P7-2's union fixes that.
- **Enum-typed fields.** `enum mtl_req` fields in `struct mtl_capability_request` (09 §6.2) violate rule 5 (`uint32_t` fields) and make bindgen's enum sizing platform-dependent.
- **Rust fit.** RAII fits well: `TxLease` (Drop → abort), `RxLease: Send` (Drop → release), with the handle generation making misuse after destroy an error instead of undefined behaviour. The view's pointer lifetime must be written down: valid while the lease is APP_WRITABLE or APP_READING, including across stop (03 §3.2 rule 2), and invalid after submit or release.
- **Python fit.** Asyncio (`loop.add_reader(fd)` + `trywait`) works only once P2-5 defines fd draining and acquire readiness. The SWIG interface must release the GIL around every WT call (11 §3 says so; add a test).

---

## Consistency check: would the examples in 10 compile?

| Symbol or field used in 10 | Where used | Defined in | Problem |
|---|---|---|---|
| `struct mtl_buffer_view` (`plane[].addr`, `.stride`) | §2, §4, §5, §6, §7 | nowhere (05 §4 says "CPU addresses per plane") | undefined |
| `mtl_video_config.{width,height,fps,transport_format,app_format}` | §2 | only `transport_format` / `app_format` named in 09 §4 | undefined; `mtl_audio_config`, `mtl_anc_config`, `mtl_fmd_config`, `mtl_st22_config` undefined too |
| `mtl_buffer_requirements.{plane[].span,row_bytes,stride}`, `min_count_direct` | §4 | prose in 05 §4.1, "per plane and per candidate path" | undefined; `plane[]` is ambiguous across candidate paths |
| `sc.completion.mode`, `sc.timing.{timeline,media_mode,late_policy}` | §2, §6 | a comment in 09 §1 | `mtl_completion_config` and `mtl_timing_config` undefined |
| `MTL_SUB_MEDIA`, `MTL_SUB_SAMPLES` | §6, §7 | not listed in 06 §4.1 | undefined constants |
| `mtl_rx_dequeue(s, timeout, &buf, &view, &unit)` | §1, §5 | 07 §2.2: `(s, &buf, &unit, timeout)` | argument order and count differ |
| `mtl_group_start(g, &p, &chosen)` | §1, §6 | 06 §10.2 calls it with two arguments | arity differs |
| `mtl_session_bind_cq` | §1 | 07 §2 bullet: `mtl_cq_bind`; 09: `completion` config binding | three spellings, two mechanisms |
| `mtl_session_update_flow` | §1 | 03 §3.2 and 04 §10: `update_destination` / `update_source` | names differ |
| instance creation | — | 02 §3 `mtl_instance_create`; 03 §7 and 11 §4 `mtl_init` | conflict |
| data-path policy | §4 `pool.data_path = REQUIRE_DIRECT` and `mem_desc.flags = MTL_MEM_REQUIRE_DIRECT` | 09 §6.2 `caps.direct_path = REQUIRE` as well | three knobs for one decision; precedence undefined. Keep `pool.data_path` (session) and the region flag (import-time mapping check); drop `caps.direct_path` |
| `sc.leg_count` | not set in §2 | 09 §1 | relies on an undocumented default |
| `mtl_start_params` literal without `struct_size` | 06 §10.2 | 11 §2.1 rule 2 | silently reads zero bytes (P1-4) |
| `planes[0] = { r, off, span, row_bytes, stride, rows }` | §4 | 05 §4 order matches | fine, but positional; `planes` (desc) versus `plane` (view, requirements) naming |
| progressive loop bound | §7 | — | FINAL never published (P4-1) |
| calls referenced elsewhere | — | 03, 05, 06, 07, 11 | missing from the 10 §1 header; listed below |

Calls that other documents use but the 10 §1 header does not declare:

- `mtl_cq_destroy`, `mtl_eq_destroy`, `mtl_session_detach_buffers`, `mtl_mem_get_info`, `mtl_video_layout_query`, `mtl_timeline_get_anchor`, `mtl_instance_cancel_all_waiters`;
- `mtl_port_count`, `mtl_group_get_state`, `mtl_sched_get_status`, `mtl_is_manager_alive`, `mtl_session_set_option`, `mtl_stat_get_u64`, `mtl_session_api_init`.

**Suggested change:** make 10 §1 the single normative header sketch, generated or checked against the structs in 03–09, and compile the examples in a doc test (a `tests/unit/unified/doc_examples.c` that only has to build).

---

## Boilerplate check against R08 §2

| # | Boilerplate | Absorbed? | Remaining gap |
|---|---|---|---|
| B1 | callback → condvar | yes: timeouts, fd, no callbacks | readiness fd for acquire (P2-5); plugins will now write a "thread on `cq_wait` that dispatches"; ship the Q-CMP-2 (b) helper in v1 |
| B2 | stop protocol | mostly | sticky cancel plus resume (P2-1) |
| B3 | NULL = try again | yes | — |
| B4 | port/IP block | yes: `mtl_flow`, legs as an array | literal-assignment defaults (P1-3); string parse helper (B-1) |
| B5 | instance singleton | **no** | P2-6 |
| B6 | format/fps tables | partly: rational fps, layout query | conversion-pair enumeration, a published FourCC / AV_PIX_FMT mapping in L4 (P2-7) |
| B7 | TAI for frame *n* | yes for owners of a timeline | named timelines for split elements (P2-9); per-row deadlines (P4-3) |
| B8 | RX latency | yes: `latency_ns`, histograms, latency range | — |
| B9 | stats copy and reset | yes: one schema, epochs | — |
| B10 | callback before handle | yes: no callbacks | — |
| B11 | audio re-framing | half | `mtl_tx_write` and a defined capacity (P3-3) |
| B12 | file loaders | n/a | — |
| new B13 | session refcount so buffers outlive stop | introduced by Q-LIFE-4 (a) | deferred destroy (P2-4) |
| new B14 | CQ reader thread in every TX app | introduced by default ALL | P1-1, plus the Q-CMP-2 helper |

---

## What would make the GStreamer and FFmpeg plugins clean

- **Sources (`GstBaseSrc` / `GstPushSrc`, live):**
  - `start` → `mtl_instance_acquire_default` + create + start.
  - `create` → `mtl_rx_dequeue(timeout)`, wrapped as a GstBuffer whose dispose calls `mtl_rx_release`.
  - `unlock` / `unlock_stop` → sticky cancel / resume.
  - `stop` → deferred destroy.
  - `query(LATENCY)` → `latency range`.
  - PTS → `mtl_time_convert(unit.timing.media → pipeline clock) − base_time`.
  - `RX_FORMAT` → renegotiation.
- **Sinks (`GstBaseSink`):**
  - `propose_allocation` offers an MTL-backed GstBufferPool: `acquire_buffer` → `mtl_tx_acquire`, `release_buffer` → `mtl_tx_abort`.
  - Foreign pools go through region import + `mtl_tx_acquire_buffer`.
  - `render` → `submit` with TAI mode, duplicate slots tolerated.
  - `unlock` → cancel; FLUSH_STOP → `mtl_session_flush`.
  - TX results → QoS messages, read through the CQ helper thread.
  - Pipeline sync off (`sync=false`), because MTL paces.
- **FFmpeg demuxers:**
  - `read_packet` → `dequeue(AVFMT_FLAG_NONBLOCK ? 0 : 100 ms)` in a loop that checks `ff_check_interrupt`.
  - `av_buffer_create(free = mtl_rx_release)` for zero-copy.
  - `pts = media_index` on a named timeline, with `time_base = 1/fps`.
  - `start_time_realtime` from T0.
- **FFmpeg muxers:**
  - A named timeline anchored at start, `INDEX = pts` (rescaled, duplicates tolerated).
  - `mtl_tx_write` for PCM.
  - A copy for video in v1, unless the narrow Q-MEM-10 lands (P2-3).

---

## Top 10 changes

1. **No silent stall from the CQ credit.** Default `NONE` for library pools and `ALL` for attached pools (Q-CMP-1 (c)); make acquire report *why* it is blocked; fix 10 §2 and 12 §4 (P1-1, P6-1).
2. **Sticky cancel plus resume, per waitable.** Add `mtl_session_resume_waiters`, `mtl_cq_cancel_waiters` and `mtl_eq_cancel_waiters`, so GStreamer `unlock`/`unlock_stop` and signal handlers work without races (P2-1).
3. **Name the buffer.** Add `mtl_tx_acquire_buffer(s, buf, …)` for attached pools, `MTL_RX_SLOT_BY_INDEX` for MXL, and cross-session lease transfer for forwarders. Without these, zero-copy from framework pools and MXL is not writable in v1 (P2-2, P5-1, P5-3).
4. **Anchor timelines at start, and let them be shared by name.** `MTL_ANCHOR_AT_START` as the default, plus `mtl_timeline_open(mt, "name")`. This fixes the headline A/V example and makes A/V sync reachable from separate FFmpeg muxers and GStreamer sinks (P3-1, P2-9).
5. **One place for each timing choice.** Media mode per session only; no `valid` bits on fields that carry a mode; the late-policy default follows the session; duplicate slots after snapping produce a result, not `-EINVAL` (P3-4, P3-5, P2-8).
6. **Zero means default, and `struct_size == 0` is an error.** `NULL` submission and `NULL` start params mean defaults; initialiser macros; `MTL_MS()` / `MTL_TIMEOUT_INFINITE`; the timeout argument last everywhere (P1-3, P1-4, P1-6, B-2, P7-3).
7. **A dry-run create query.** `mtl_video_session_query(mt, &sc, &vc, &info, &req)` gives caps, granted values and buffer requirements before anything is allocated; it is the `fi_getinfo` analogue, and plugin `get_caps` needs it (P2-7, P7-1).
8. **Waiting and lifetime that frameworks can use.** A session wait fd with an acquire-readiness bit and defined draining; deferred destroy while leases are held; `mtl_session_flush`; per-session default EQ; a refcounted default instance (P2-4, P2-5, P2-6, P2-8).
9. **Convenience for the copy essences and for P1.** `mtl_tx_write` for ST30/40/41; `mtl_flow_parse`; `mtl_instance_open_simple`; the Q-CMP-2 dispatcher helper in v1; an ANC packet table in the submission and the unit (P3-2, P3-3, P1-2, B-1).
10. **Make 10 normative and compile it.** Define the dozen missing structs; reconcile the signatures (`rx_dequeue`, `group_start`, bind, update_flow, instance); keep one data-path knob; fix the nested-struct ABI rule; fix the 1080/64 loop; build the examples in a doc test (consistency table, P4-1, B-1).

---

## Things the design got right

- **App-driven loops with no tasklet callbacks.** This one choice removes B1 and B10, the Rust dangling-`priv` undefined behaviour, and the Python GIL-callback problem, and it is the Rivermax shape (R10 §12 asks for exactly this).
- **Distinct return codes.** `-ETIMEDOUT`, `-ECANCELED`, `-ESHUTDOWN`, `-EBADF` and `-ESTALE` replace "NULL means anything" (B3). Every plugin's error mapping becomes a table instead of guesswork.
- **Lending RX buffers.** Release from any thread in any order, with an explicit overflow policy (`DROP_NEW` / `RECLAIM_OLDEST_READY`), is exactly what FFmpeg `AVBufferRef` and GStreamer `GstMemory` need. RX zero-copy for both frameworks is writable in v1 as designed.
- **Generation-tagged value handles and lease generations.** Ideal for bindings (a `u64` everywhere), and they turn double submit and release-after-destroy into error codes. Rust RAII wrappers fall out naturally.
- **Media time separate from launch time,** with INDEX mode and exported exact math. This absorbs B7 for owners of a timeline and answers the #1211 "pass both" request without two conflicting fields.
- **Per-unit results with status, reason and `margin_ns`, plus the stop accounting identity.** These give P8 and plugin QoS handling real data. Results stay drainable in every state, which fixes PR #1610's shutdown recipe.
- **The `trywait` + eventfd protocol.** It is copied from a proven design (`fi_trywait`), and the tasklet never makes the syscall.
- **Requested versus granted everywhere.** REQUIRE fails loudly, and granted values appear in `get_info`, which ends the silent zero-copy, pacing and DMA downgrades.
- **Small descriptors.** One `mtl_flow` for every essence and for updates (B4), and rational fps (B6).
- **The slot hint at acquire.** It directly removes the phase-lock workaround bobi.studio measured.
- **The latency range in `mtl_session_get_info`.** It answers GStreamer LATENCY queries without guesswork.
- **Exported `*_init()` functions and an explicit `struct_size`** (with P1-4 fixed). Rust `no_std` literals and SWIG no longer break when a field is added.
- **Separate CQ and EQ.** Informational floods can never cost an ownership completion; this is PR #1610's worst defect, removed by construction.
