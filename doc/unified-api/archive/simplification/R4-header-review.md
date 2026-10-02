# R4 — adversarial review of the revision-4 headers

| | |
|---|---|
| Status | Review of the revision-4 header set and examples, 2026-10-01 |
| Scope | `sketch/include/mtl/experimental/*.h` (14 headers), `sketch/examples/*`, `sketch/check.sh` |
| Version reviewed | the files after the S7 samples audit was adopted (`mtl_session_open`, `mtl_session_close`, the new R2 rule, `mtl_session_get_wait_handle`) |
| Checked against | 04 (call classes, armed waiters, §4.5 stalled queues), 05 (imported memory), 06 (exact RTP, audio index), 07 (one result per unit, order), 11 (ABI), S2, S4, S8, REVISION-4 D-71…D-87 |
| Method | read every declaration and comment, compile probes for the macros and inline helpers, cross-read the examples as a user would |

The review started on the first revision-4 drop and was redone on the S7 update. Findings the update already fixed are listed in §4 with their original severity, so the history of each decision stays visible.

## 1. Summary

| Severity | Open | Resolved by the S7 update |
|---|---|---|
| Blocker | 1 | 1 |
| Major | 15 | 2 |
| Minor | 24 | 4 |
| Nit | 11 | 1 |

The open Blocker is the rows unit: a failed re-submit returns the slot to the pool while earlier rows are still queued or on the wire (RV-01). The Majors cluster in five places:

- **Memory contracts the header does not state**: the meta-area layout disagrees with the packet helpers (RV-02), D-86's snapshot at submit is missing (RV-03), retirement is defined only for application leases (RV-04), pools attached over another session's pool can be sent while the owner writes them (RV-13), RX_BY_INDEX can target a leased slot (RV-14).
- **Waiting**: the new R2 rule is not applied to every call and breaks ex04 (RV-05); what "arms" means, and what it costs a tasklet, is undefined (RV-06).
- **Timing**: the start-array sentence is wrong for audio and mixed rates (RV-07), the audio index unit is ambiguous (RV-08), ANC/fastmeta rasters depend on a start that may never come (RV-09).
- **Configuration round-trips**: `mtl_session_update` with a whole config (RV-10), options that `mtl_session_config_parse` has nowhere to store (RV-11), option scope (RV-12).
- **ABI and threading**: plugin formats share one number space (RV-15), and two library-thread callbacks contradict R6 (RV-16).

None of these needs a new concept. Most fixes are one sentence in a header comment, or one field.

## 2. check.sh

- Current files: `check.sh: OK (168 compile runs, compilers: gcc clang g++ clang++)`. That covers gcc 13.3, clang, C99 `-Wpadded -pedantic`, C++17, `-DMTL_LATER` and `-D__bindgen`. 113 functions, 4 more reserved under `MTL_LATER`.
- First drop: the only failures were the 15 "not copied verbatim into 10-api-sketch.md" lines, as expected. There were no compile or lint failures.
- Extra probes (not in check.sh), all on the current files:
  - Every header compiles clean with `-Wconversion -Wsign-conversion -Wcast-qual`.
  - `MTL_SAME(session, instance)` only warns in C under gcc and clang. It is an error only in C++ (RV-36).
  - `MTL_SAME(s, MTL_NULL(mtl_session_h))` fails in C++ with "taking address of rvalue" (RV-36).
  - `mtl_pkt_tx_table()` gives a writable table from a `const struct mtl_unit*` with no `-Wcast-qual` warning (RV-44).
- Lint gaps (RV-51):
  - Lint 2's comment says `mtl_reasons.h` includes nothing and that every other header includes a sibling. The code checks neither: a header with no `#include` passes, and so would `mtl_reasons.h` including `mtl.h`.
  - C++ runs omit `-pedantic`.
  - The examples are never compiled with `-D__bindgen`.

## 3. Open findings

### RV-01 — Blocker — a failed rows re-submit frees a slot that is still being sent

- **Where**: mtl.h:816-819 (`mtl_tx_submit`); ex08_progressive_rows.c:23.
- **Problem**:
  - The failure rule is "on failure the slot goes back to the pool without a result". For `MTL_UNIT_ROWS` the same lease is submitted again and again.
  - When the second or a later submit fails (for example `-MTL_ERANGE` because a row is past its deadline, or `-MTL_EINVAL` because `used` went down), rows from the earlier submits are already queued or on the wire.
  - ex08's comment says something else: "the frame ends here, with what was sent". 07 requires exactly one result per accepted unit, and the first submit was accepted.
- **Failure**:
  - A gateway with an attached pool (app memory) gets `-MTL_ERANGE` on submit 3 of 17. The header says the slot is free, so the app hands the surface back to its capture card, which overwrites it while the NIC is still DMA-reading rows 0-191.
  - With a library pool, the next acquire gets the same slot and the app renders frame k+1 into packets of frame k still being sent.
  - Either way no result arrives, so a cookie-tracking app leaks the frame.
- **Fix**: give rows units their own failure rule in mtl.h:
  ```c
  /* ... On failure of a first submit the slot goes back to the pool without a result.
     Rows units: once a submit was accepted, a later failing submit ends the unit as if
     used had stopped there (tx.rows_late policy); the lease is consumed and the unit's
     one result (LATE or DROPPED, reason) follows when its packets have left. */
  ```

### RV-02 — Major — the meta-area header and the packet-table helpers disagree

- **Where**: mtl.h:785-798 ("the meta area of a slot starts with this header, on TX and RX alike", `MTL_META_PACKETS`); mtl_packet.h:78-83 (`mtl_pkt_tx_table`/`mtl_pkt_rx_table` return `u->meta` itself); mtl_packet.h header comment; ex12:28.
- **Problem**: two normative statements give two layouts. The static inline helpers are compiled into applications, so whichever one the library implements, every binary built with the other is wrong.
- **Failure**: the library puts `struct mtl_meta_hdr` first, as mtl.h says. ex12 writes `len[0]` over the header's `kind`/`tag_version`, and every later length lands one entry off. The library then reads `count` from packet lengths, sends slot lengths shifted by 16 bytes, or rejects every chunk.
- **Fix**: keep one layout. Either the helpers skip the header and the app fills it:
  ```c
  static inline struct mtl_pkt_tx* mtl_pkt_tx_table(const struct mtl_unit* u) {
    return (struct mtl_pkt_tx*)((uint8_t*)u->meta + sizeof(struct mtl_meta_hdr));
  }
  ```
  or mtl.h says that packet units have no header and the table starts at `meta` (count = `used`). The second is simpler because `used` already carries the count. It also lets `MTL_META_PACKETS` go.

### RV-03 — Major — D-86's snapshot at submit is not in the header (TOCTOU on the meta area)

- **Where**: mtl.h:769, 785-809, 816-820; mtl_packet.h:47-52 (`struct mtl_pkt_tx`); REVISION-4 D-86.
- **Problem**: D-86 says ANC packet tables and user meta are snapshotted at submit. The header never says so. As written, a library may read `udw_offset`/`udw_count`, user meta and `mtl_pkt_tx.len` when the tasklet picks the unit up, after the app regains nothing but can still write the memory. The meta area is app-writable for the lease's whole life, and for attached pools it is app memory.
- **Failure**:
  - A captions thread reuses its scratch pointer and rewrites `udw_count = 600` after submit. The tasklet reads 600 x 10-bit words past plane 0's end: an out-of-bounds read on a pinned core, or DMA of unrelated memory onto the wire.
  - For packet units, a len raised above `slot_bytes` after validation sends neighbouring slot bytes.
- **Fix**: one sentence at mtl.h:816, plus the per-kind copy rule in mtl_packet.h:
  ```c
  /* ... The meta area (ANC table, user meta, packet lengths) is validated and copied at
     submit; later writes to it never reach the wire. Plane bytes are read at send time. */
  ```

### RV-04 — Major — retirement covers application leases only, and a closed handle cannot be waited on safely

- **Where**: mtl.h:694-698 (`mtl_session_close`), mtl.h:252-254 (CLOSING, RETIRED "until its slot is reused"), mtl.h:876 (`MTL_WAIT_RETIRED`), mtl.h:674-677 (stop), mtl_queue.h:37, mtl_queue.h:104-105.
- **Problem**:
  1. "1: still retiring because the application holds leases" leaves out the documented case of device references. On a stalled shared TX queue (04 §4.5 step 3) the session stays retiring with no app lease out. Is that 1, or `-MTL_ETIMEDOUT` (R2: "ETIMEDOUT is for ... close")?
  2. If close can return a negative value, nothing says whether the handle is still valid afterwards, so cleanup code cannot know whether to call close again.
  3. After a 1, the only core way to learn about retirement is `mtl_session_wait(s, MTL_WAIT_RETIRED)` on a closed handle. Once the handle-table slot is reused that call returns `-MTL_EBADF`, which cannot be told apart from a bug. Session events cannot be read on a closed handle either.
  4. stop promises "every accepted unit gets its result before it returns". A unit on a hung queue has no result until the queue reset in 04 §4.5 finishes, which can take longer than the timeout.
  5. close discards unread results, so cookies are lost; nothing says so.
  6. Whether close unbinds a queue is not stated, while `mtl_queue_close` is `-MTL_EBUSY` "while sessions are bound".
- **Failure**:
  - An app frees its arena after close returns `-MTL_ETIMEDOUT` with the link down, reading it as "already stopped". The NIC still holds descriptors into the arena.
  - A GStreamer element waits for `MTL_WAIT_RETIRED`, gets `-MTL_EBADF` after slot reuse, assumes a bug and leaks the arena forever.
- **Fix**:
  ```c
  /* Always consumes s, even on a negative return: never call close twice. 0: retired,
     no memory, lease, hold or device reference remains. 1: still retiring (leases out or
     device references pending); MTL_WAIT_RETIRED on s and MTL_EVENT_SESSION_RETIRED report
     the end, and a wait on a retired handle returns MTL_WAIT_RETIRED, never -MTL_EBADF
     (handle ids are never reused). Unread results are discarded; the session is unbound
     from every queue. CP. */
  ```
  For stop: "-MTL_ETIMEDOUT: queued units became FLUSHED; units already handed to the device get their result when the device releases them (state FLUSHING until then)".

### RV-05 — Major — the new R2 is not applied everywhere, and ex04 now leaves its loop on the first idle reap

- **Where**: mtl.h:30-33 (R2); mtl.h:865-868 (`mtl_tx_reap` "Count >= 0"); mtl.h:877-879 (`mtl_session_wait` returns 0 for "nothing"); mtl_mem.h:135 (`mtl_tx_acquire_slot`: `-MTL_EBUSY`); mtl_util.h:29 (`mtl_tx_send_slot`: `-MTL_EBUSY`); mtl_queue.h:81-83, 114-122 ("Count >= 0"); ex04:43-44, 49; ex09:51.
- **Problem**: R2 now says a data call that finds nothing returns `-MTL_EAGAIN`. Five places still say something else:
  - reap and the queue reads document "Count >= 0".
  - wait returns 0.
  - `mtl_tx_acquire_slot` and `mtl_tx_send_slot` return `-MTL_EBUSY`.
  - ex04 treats `reap()` < 0 as fatal and treats acquire_slot's "in flight" as `-MTL_EAGAIN`.
- **Failure**:
  - ex04 line 43: `ret = reap(s)` returns `-MTL_EAGAIN` the first time no result is ready. `continue` re-tests `ret >= 0` and the loop exits. ex_fail prints "zero copy: MTL_EAGAIN", and the sender stops after one frame.
  - If the header is right about `-MTL_EBUSY`, line 49 never matches and the loop exits on the first busy surface.
- **Fix**:
  - Say in R2 that count-returning reads return `-MTL_EAGAIN`, never 0.
  - Make `mtl_tx_acquire_slot`/`mtl_tx_send_slot` return `-MTL_EAGAIN` ("slot not free yet").
  - Keep `mtl_session_wait`'s 0 but state it as the one exception, or make it `-MTL_EAGAIN` too.
  - Fix ex04:
  ```c
  ret = reap(s);
  if (ret == -MTL_EAGAIN) ret = 0;
  if (ret < 0 || next_framework_frame(&i, &id) != 0) continue;
  ```

### RV-06 — Major — "arms its wait target" has no defined lifetime, cost or owner

- **Where**: mtl.h:30-33, 116, 877-883; 04 §5.1-§5.3; ex03.
- **Problem**:
  - 04 §5.1 arms with a counted waiter (fetch_add before sleeping, fetch_sub when woken). R2 now arms whenever a data call finds nothing, including timeout-0 polls, which never sleep and so never disarm.
  - The header does not say whether the arm is a flag cleared when the handle fires, or a count.
  - It does not say whether busy-loop callers arm (04 G-79: they must not touch wait objects).
  - It does not say which calls drain the eventfd ("the library drains it": on wait, on every successful data call, or only on `mtl_session_wait`?).
  - It does not say whether `get_wait_handle` with two different masks returns one fd or two.
- **Failure**:
  - A W0 app polls `mtl_rx_dequeue(s, &u, 0)` on its own core at 1 MHz. Every miss arms RX_READY. With the auto W2 wake for sub-ms sessions (04 §5.2), every completed audio packet now makes the tasklet `write()` the eventfd: 8,000 syscalls/s per session on a pinned core for an app that never sleeps.
  - If the arm is a count, it grows without bound.
  - If the fd is drained only by `mtl_session_wait`, an app that drains with acquire/reap keeps POLLIN set and a level-triggered epoll spins.
- **Fix**: state the protocol in mtl.h next to R2:
  ```c
  /* Arming: a data call that returns -MTL_EAGAIN on an APP thread sets the target's
     wake-request bit; the first completion for that target clears it and signals the
     session's one wait handle (one eventfd per session; mask selects the targets that may
     signal it, the union of all masks requested). Every data call on an APP thread drains
     the handle when it was signalled. Busy-loop threads never arm and never drain. */
  ```

### RV-07 — Major — the start-array sentence is wrong for audio and mixed rates; the direction rule lives only in a reason code

- **Where**: mtl.h:669-673; mtl.h:662-667 (`preroll_ns` "start only", `MTL_AT_INDEX` "(TX)"); mtl_reasons.h:63 (`START_SET_MIXED`); mtl_sync.h:27, 42; ex07:42; ex09:38.
- **Problem**:
  - "Every TX unit index k of every session gets the same media time" is false whenever periods differ. In ex07, video index k is k/59.94 s and audio index k is sample k (06 §3: k/48000 s), so "same media time" would put audio sample 60 at frame 60. What is actually shared is T0, and index 0 of every session.
  - "One timeline, one direction" (D-78) appears only as a reason code.
  - The header does not say what `when` means for an RX session, what `preroll_ns` does in `mtl_session_update`, what a late join (a single start on an already-resolved timeline) gets, or what happens when a later session's period is off the `AT_START` grid fixed at the first start (`GRID_MISMATCH` exists, but no rule names it).
- **Failure**: an implementer takes the sentence literally and aligns audio unit k (10 ms units) with video frame k. Audio then leads video by k x (16.68 - 10) ms, so lip-sync drifts by 6.7 ms per frame.
- **Fix**:
  ```c
  /* Starts n sessions, all or none. They must share one timeline and one direction
     (-MTL_EINVAL, START_SET_MIXED). T0 is resolved once; index 0 of every session is at T0,
     index k at T0 + k x that session's index period (mtl_sync.h). On a resolved timeline a
     later start joins it: its first index is the first feasible one, or when->value with
     MTL_AT_INDEX. RX: when is the earliest media time delivered. preroll_ns is ignored
     by mtl_session_update. */
  ```

### RV-08 — Major — the audio media-index unit is ambiguous

- **Where**: mtl_sync.h:7 (`M(k) = T0 + k * period`), mtl_sync.h:70 (`mtl_index_at`: "period of session s"), mtl.h:775, mtl_sync.h:41-48 (`mtl_slot_hint.next_media_index`), ex07:9, 20, 52; 06 §3 table ("an audio index the sample number").
- **Problem**: for audio, "the session's unit period" is `unit_samples / rate` (10 ms by default), but 06 and ex07 use the sample number. mtl_sync.h never names the index period per essence, so `mtl_index_at`, `mtl_tx_next_slot` and `unit.media_index` can each be read either way.
- **Failure**:
  - A library that counts audio in units computes RTP = floor((T0 + k x 10 ms) x 48000) for ex07's `pts` in samples. That is 480 times too far ahead: every unit is `BEYOND_HORIZON` and dropped.
  - Two processes computing `mtl_epoch_index_at` with `unit_rate` = 100 Hz versus 48 kHz disagree.
- **Fix**: in mtl_sync.h:
  ```c
  /* The index period: video and cvideo one frame (one field when interlaced), audio one
     sample (1 / sample_rate), ANC and fastmeta the video frame or field they follow, RTP the
     unit rate. An audio unit's media_index is the index of its first sample. */
  ```

### RV-09 — Major — ANC and fastmeta rasters come from a start that may never happen

- **Where**: mtl.h:487-489 (`anc.video` "all zero: the first video session it starts with"), mtl.h:498; S2 table (the "timeline's video owner" rule is not in the header); ex07:40.
- **Problem**:
  - Validation, `mtl_session_query`, `mtl_session_requirements` and `mtl_session_get_info` (packets per unit, ANC window, pool sizing) all run before start, so for these sessions they cannot be complete.
  - On the epoch timeline (the default) there is no video owner.
  - A lone ANC start, a restart of only the ANC session, or an ANC session on a process with no video TX has no source for the raster.
- **Failure**: an ANC inserter for a video stream produced by another process uses the epoch timeline (the cross-process recipe of 06 §10.7). `mtl_session_start(&anc, 1, ...)` has no video, so it fails or silently uses some default rate, and the ANC window is wrong on the wire.
- **Fix**: require `anc.video.fps` (and scan) at create unless the session's timeline is a created timeline whose first start includes a video. The rule then sits next to the field:
  ```c
  struct mtl_raster video; /* required on the epoch timeline; all zero on a created
                              timeline: the first video session of its first start */
  ```
  An all-zero raster with the epoch timeline is `-MTL_EINVAL`, `FIELD_REQUIRED`.

### RV-10 — Major — `mtl_session_update` with a whole config: what happens to the fields outside `parts`

- **Where**: mtl.h:679-687, 653-654; ex13:10-14, 21-26.
- **Problem**: the call takes a whole `struct mtl_session_config` and a `parts` mask. Nothing says whether fields outside `parts` must equal the current configuration, are ignored, or are applied.
  - `MTL_UPDATE_OPTIONS` does not say whether `sc->options` replaces the whole option set (R7: an absent key means the default, so a replace resets every unnamed key) or merges into it.
  - `MTL_UPDATE_MEDIA` does not say whether `essence`, `direction` and `unit` may change.
  - `MTL_UPDATE_FLOWS` does not say whether adding a second leg (all-zero `flows[1]` becomes non-zero) is allowed.
  - `when` is not defined for `MTL_UPDATE_POOL`/`MTL_UPDATE_MEDIA`, which need STOPPED anyway.
- **Failure**:
  - ex13's `set_leg` passes `MTL_UPDATE_LEGS`, but a framework that also edited `sc.flows[0]` in the same struct is silently ignored or silently applied, depending on the implementation.
  - A GStreamer property setter calls update with `MTL_UPDATE_OPTIONS` and one key. Under replace semantics every other tuned key (late policy, skew budget) drops back to its default on a live stream.
- **Fix**:
  ```c
  /* Fields outside `parts` must equal the current configuration (-MTL_EINVAL naming the
     first that differs). OPTIONS merges: each key given is set, keys not given keep their
     value. direction, essence and unit never change. FLOWS may add or remove flows[1]
     only when STOPPED. `when` applies to FLOWS and LEGS; it must be NULL otherwise. */
  ```
  Simpler alternative: drop `MTL_UPDATE_OPTIONS`, because `mtl_set_option` already exists (RV-22).

### RV-11 — Major — options parsed from a spec string have nowhere to live

- **Where**: mtl.h:587-591 (`mtl_session_config_parse`: "keys are ... the option names of mtl_options.h"), mtl.h:573 (`const struct mtl_option* options; copied at create`), mtl.h:592-596 (`mtl_session_open` with `base`), mtl.h:653-654 (`get_config`), mtl.h:303-310 (`str`).
- **Problem**:
  - `parse` writes into a caller struct whose `options` is a const pointer to caller-owned memory. To store "rx.skew_budget_ns=2ms" it must allocate an array the caller never frees (no free call exists), or keep it in thread-local or static storage (overwritten by the next parse).
  - `get_config` has the same problem in reverse: who owns the returned `options` and the `str` strings, and for how long?
  - `mtl_session_open` merges `base->options` with spec options, with no rule for duplicates.
  - The string values (`str`) are not stated to be deep-copied at create.
- **Failure**:
  - An app parses two configs in a row (video, then audio). Both point at one static option buffer, so the video session is created with the audio options.
  - A Rust binding frees its `CString` after create because "copied at create" was read as covering the strings, and the library reads freed memory for `time.ptp_unicast`.
- **Fix**: give the config inline storage for the options parse and get_config produce, and say that create deep-copies everything:
  ```c
  /* options: caller-owned array, deep-copied at create (str included). parse and
     get_config never write options/option_count; parsed option keys go to
     parsed_options[], read after options[] (later entries win). */
  struct mtl_option parsed_options[16]; /* in mtl_session_config, before reserved[] */
  ```
  Or: `parse` rejects option keys (`-MTL_ENOTSUP`, `OPTION_UNKNOWN`) and only `mtl_session_open`, which owns its copy, accepts them.

### RV-12 — Major — option scope means a different thing per key, and the key list does not say which

- **Where**: mtl.h:303-305 ("scope 0, or a leg or port index"); mtl_options.h:87 (`audio.rl_warmup` "scope 1 = accuracy"), 111 (`sched_sleep_us` "scope = scheduler + 1"), 112 and 140 ("scope 0 TX, 1 RX"), 150 (`ptp_pi` "scope 1/2 = kp/ki"), 125 (ports group "scope = port"); mtl_options.h:223-231 (`struct mtl_option_desc` has no scope field).
- **Problem**:
  - Scope is any of: a plain index (port, leg), an index + 1 (scheduler), a direction, or a sub-parameter selector. For per-port keys it is unclear whether scope 0 means "port 0" or "every port".
  - `mtl_option_desc` does not describe the scope, so the stated goal ("GStreamer properties, FFmpeg AVOptions and bindings can expose every knob without code per knob") cannot be met for any scoped key.
  - Duplicate (key, scope) pairs in one array are undefined.
- **Failure**: a generic binding exposes `port.rx_desc` as one integer and sends scope 0. On a two-port instance the user's value applies to port 0 only, and port 1 silently keeps the default, so the redundant leg drops packets under load.
- **Fix**: one convention, described in the descriptor:
  ```c
  enum mtl_option_scope { MTL_SCOPE_NONE = 0, MTL_SCOPE_PORT = 1, MTL_SCOPE_LEG = 2,
                          MTL_SCOPE_SCHED = 3, MTL_SCOPE_DIR = 4 };
  /* scope: 0 = every port/leg/scheduler, else index + 1; DIR: 1 TX, 2 RX. A later duplicate
     (key, scope) wins. Sub-parameters are separate keys: time.ptp_pi_kp, time.ptp_pi_ki,
     audio.rl_warmup_accuracy. */
  ```
  Add `uint32_t scope_kind; int64_t def;` to `mtl_option_desc`.

### RV-13 — Major — a pool attached over another session's pool can send memory its owner is writing

- **Where**: mtl_mem.h:102-103 (`mtl_session_get_pool_region`), mtl_mem.h:79-98, mtl.h:778 (`hold`), mtl_reasons.h:60 (`HOLD_REQUIRED`), ex09:23-52.
- **Problem**: TX slot j lies over RX slot j, but the header never says what makes that safe.
  - `HOLD_REQUIRED` implies a submit must carry a hold. Nothing says the hold has to be the RX lease of the same underlying slot.
  - Nothing says what closing the owner (RX) while TX sessions are attached does.
  - Nothing says whether the region handle from `get_pool_region` must be released with `mtl_mem_destroy` (refcount ownership).
  - The RX session can be restarted or `MTL_UPDATE_POOL`-resized while TX sessions sit on its old layout.
- **Failure**:
  - A forwarder bug passes `how.hold = in.lease` from RX slot 3 to `mtl_tx_send_slot(tx, 5, ...)`. RX slot 5 is free on the RX side and is being written with the next frame, so the TX sends a torn frame. The library checked only that "a hold is present".
  - `mtl_session_close(rx)` frees the library pool while four TX sessions still have queued units over it, and the NIC DMAs freed hugepages.
- **Fix**: in mtl_mem.h, next to `get_pool_region`:
  ```c
  /* A session attached over another's pool submits slot j only with hold = a lease of
     the owner's slot j (-MTL_EINVAL, HOLD_REQUIRED otherwise). The owner cannot change
     its pool (update, detach) while attached sessions exist (-MTL_EBUSY); closing it returns
     1 until they detach or close. The returned region holds a reference: mtl_mem_destroy
     drops it. */
  ```

### RV-14 — Major — RX_BY_INDEX can target a slot the application still holds

- **Where**: mtl.h:548-549 (`RX_BY_INDEX`, `RX_LATEST`), mtl.h:780 (`missed_before`), ex06:23.
- **Problem**: with `RX_BY_INDEX` the destination of unit k is fixed (k mod pool_count). Nothing says what happens when that slot is leased (dequeued, not yet released) or held by a TX unit. `RX_LATEST` "reclaims the oldest unread unit", but in BY_INDEX mode the slot to reclaim is not the oldest, it is the one the index names. The combination of the two flags is undefined.
- **Failure**: an MXL reader is slow for 9 frames with GRAINS = 8. Unit k+8 maps to grain k, which the app still holds, and the library DMA-writes into a grain the reader is copying out. MXL readers then see a torn grain whose header says it is complete.
- **Fix**:
  ```c
  #define MTL_SESSION_RX_BY_INDEX 0x8u /* RX slot = media index mod pool_count (MXL). If that
     slot is leased or held, the unit is dropped and counted in the next unit's
     missed_before; never written over. RX_LATEST is ignored with BY_INDEX. */
  ```

### RV-15 — Major — plugin formats mix three number spaces in one `uint32_t`

- **Where**: mtl_plugin.h:41 (`mtl_plugin_frame.format` "transport, app or MTL_PLUGIN_CODESTREAM()"), mtl_plugin.h:54-55, 81-84 (`in_format`, `out_format`); mtl.h:402-419 (transport 1..16), mtl_format.h:23-49 (app 1..40), mtl_format.h:53-56 (`MTL_V210_NONSTD = 33`).
- **Problem**: transport and app formats overlap numerically. 2 is `MTL_YUV422_10` (RFC 4175 pgroups) and also `MTL_APP_V210`. 33 is `MTL_V210_NONSTD` and also `MTL_APP_ARGB`. `mtl_convert_image` and `mtl_format_names` carry `is_app`; the plugin ABI does not.
- **Failure**: a decoder plugin advertises `{CODESTREAM(JPEGXS), 2}` meaning V210 out. The host reads it as transport `YUV422_10`, lends a pgroup-sized frame (2.5 B/px rather than V210's 128-byte line alignment), and the plugin writes past the plane.
- **Fix**: tag the space in the value, as `MTL_PLUGIN_CODESTREAM` already does:
  ```c
  #define MTL_PLUGIN_APP(fmt) (0x20000u | (uint32_t)(fmt))  /* mtl_format.h app format */
  /* plugin formats: a transport format (mtl.h, < 0x10000), MTL_PLUGIN_APP(), or
     MTL_PLUGIN_CODESTREAM(); never a bare app value. */
  ```

### RV-16 — Major — two callbacks contradict "the library never calls the application", with no reentrancy rules

- **Where**: mtl.h:45-47 (R6); mtl_queue.h:130-134 (`mtl_queue_dispatch_start`); mtl_observe.h:170-174 (`mtl_log_set_sink`).
- **Problem**: R6 says the library never calls the application, yet the dispatch thread calls `fn` for every result and event, and the log sink calls `fn` from a library thread. Missing:
  - which MTL calls `fn` may make;
  - whether the dispatch thread is a "library busy-loop thread" (the EDEADLK rule);
  - what happens when `fn` blocks;
  - whether `dispatch_stop`/`mtl_log_set_sink(NULL)` wait for an `fn` call in progress (needed before `user` can be freed).
- **Failure**:
  - `fn` calls `mtl_session_close(s, 1 s)` on a session bound to the same queue. Close drains, and its last results must be delivered by the dispatch thread that is blocked inside `fn`, so it deadlocks for the full timeout every time.
  - An app frees `user` right after `dispatch_stop` while `fn` is still running: a use-after-free.
- **Fix**: R6: "The library calls the application only from the threads mtl_queue_dispatch_start() and mtl_log_set_sink() create, never from a tasklet". At both declarations:
  ```c
  /* fn runs on one library thread (not a busy-loop thread); it may make any call except
     close/stop/update of a session bound to q, mtl_queue_close(q) and dispatch_stop(q)
     (-MTL_EDEADLK). dispatch_stop waits for a running fn, so user may be freed after. */
  ```

### RV-17 — Minor — `*out` on failure is defined only for `mtl_session_open`; ex01 closes an uninitialised instance

- **Where**: mtl.h:362-366, 592-596, 601-602; ex01:8, 13, 32.
- **Problem**: only `mtl_session_open` says "on failure *out stays null". ex01 declares `mtl_instance_h mt;` uninitialised. When `mtl_instance_open` fails (MTL_PORTS unset), `mtl_instance_close(mt)` reads an indeterminate value. It is also not stated whether `mtl_instance_close` is null-safe, as `mtl_session_close` is.
- **Failure**: on a CI host without MTL_PORTS, ex01 passes stack garbage to `mtl_instance_close`. This is formally UB and in practice `-MTL_EBADF`, but it is copied into real programs.
- **Fix**: R4 gains "Every call with an `out` handle writes the null handle on failure; every close/destroy returns 0 for a null handle". Initialise `mt = MTL_NULL(mtl_instance_h)` in ex01.

### RV-18 — Minor — the output-size rule (R3) has exceptions it does not list

- **Where**: mtl.h:34-37; mtl.h:653-654 (`mtl_session_get_config` writes an input struct, sized by `sc->struct_size`, no size argument); mtl_format.h:60-68 (`mtl_format_names`: output, no size argument); mtl_queue.h:65 ("One event record; fixed size") versus `ev_size` at mtl_queue.h:82-83, 121.
- **Problem**: three structs written by the library do not follow "output structs carry no struct_size and are filled up to the size argument". R3 also does not say whether bytes beyond what the library knows are zeroed. Without that, a newer app on an older library reads stack garbage in new fields.
- **Fix**: R3: "...filled up to the size argument; bytes the library does not know are zeroed. In/out structs (`mtl_unit`, `mtl_session_config` from get_config/parse) use their own struct_size instead." Add `size_t size` to `mtl_format_names`. Drop "fixed size" from `mtl_event`.

### RV-19 — Minor — `struct mtl_unit` in/out rules: failure, struct_size and templates

- **Where**: mtl.h:759-783; mtl_util.h:21-31 ("A received unit is a valid `how`"); ex10:4-5, 38.
- **Problem**:
  - The header does not say whether acquire/dequeue leave `*u` untouched on failure.
  - It does not say that acquire/dequeue write only up to `u->struct_size`.
  - It does not say which fields `mtl_tx_write`/`mtl_tx_send_slot` take from `how`. An RX unit's `used`, `status`, `rtp`, `missed_before` and `lease` are all set. Only "RX-only flags are dropped" is stated.
- **Failure**: a forwarder passes an incomplete RX video unit (`used` = rows received < height) as `how`. The TX takes `used`, publishes a truncated frame, and the rest of the frame is filled by the rows-late policy on a non-rows session.
- **Fix**: "As a template, `how` contributes media_index, media_tai_ns, cookie, hold, launch_tai_ns and the low 16 bits of flags; every other field is ignored. On failure acquire and dequeue leave *u unchanged."

### RV-20 — Minor — launch-time validity and flag combinations

- **Where**: mtl.h:523 (`MTL_PKT_PACE_LAUNCH`: "a chunk at unit.launch_tai_ns"), mtl.h:779 ("with MTL_SUBMIT_NOT_BEFORE or MTL_SUBMIT_EXACT"), mtl.h:743-744.
- **Problem**: in `PACE_LAUNCH` mode, `launch_tai_ns` is read without either flag, which contradicts the field comment. `NOT_BEFORE | EXACT` together is undefined. `MTL_SUBMIT_UNIT_END` on a frame unit is undefined.
- **Fix**: "launch_tai_ns: read with NOT_BEFORE or EXACT, and always with PKT_PACE_LAUNCH. NOT_BEFORE together with EXACT, or UNIT_END on a non-packet unit, is -MTL_EINVAL."

### RV-21 — Minor — `used` changes meaning with plane shape

- **Where**: mtl.h:772 ("bytes when plane 0 has one row, else rows of plane 0").
- **Problem**: the unit of `used` depends on a layout detail, not on the unit kind. A 1-row video raster, or a cvideo pool laid out with rows, flips the meaning.
- **Fix**: define it per kind: "video frames and rows: rows; packets: packets; audio, cvideo, ANC UDW, fastmeta: bytes; TX 0 = all".

### RV-22 — Minor (simplification) — the same knob exists twice

- **Where**:
  - mtl.h:550 `MTL_SESSION_RX_NO_FILL` and mtl_options.h:53 `rx.fill`.
  - mtl.h:333 `MTL_INSTANCE_HW_TIMESTAMP` and mtl_options.h:71 `caps.hw_timestamps`.
  - mtl.h:684 `MTL_UPDATE_OPTIONS` and mtl_options.h:207 `mtl_set_option`.
  - mtl.h:547 `MTL_SESSION_REQUIRE_DIRECT` and mtl_options.h:72 `caps.tx_copy`.
  - mtl.h:532 `packet.slot_bytes` and mtl_options.h:60 `session.max_udp_payload`.
- **Problem**: two spellings of one decision, with no rule for when they disagree (`RX_NO_FILL` with `rx.fill = ZERO`).
- **Fix**: keep the typed field where most apps need it, and delete the option (`rx.fill`, `caps.hw_timestamps`). Delete `MTL_UPDATE_OPTIONS`. Make `REQUIRE_DIRECT` with `caps.tx_copy = 1` `-MTL_EINVAL`. Define `max_udp_payload` as the limit `slot_bytes` defaults from.

### RV-23 — Minor (simplification) — two dry runs

- **Where**: mtl.h:646-650 (`mtl_session_query`), mtl_mem.h:117-131 (`mtl_session_requirements`).
- **Problem**: both validate the same config without allocating. `mtl_session_info` already carries `unit_bytes`, `pool_slot_pitch`, `max_count` and `meta_capacity`.
- **Fix**: fold `mtl_buffer_requirements` into `mtl_session_query` as an optional second output (`struct mtl_buffer_requirements* MTL_NULLABLE req, size_t req_size`). That is one function fewer and one validation path.

### RV-24 — Minor — shared instances and port sources

- **Where**: mtl.h:329 (`MTL_INSTANCE_SHARED` "opens merge"), mtl.h:346 (`port_count` "0 with a spec string" versus mtl.h:358-361 "spec, p or both"), mtl.h:358-361 (MTL_PORTS).
- **Problem**: "merge" is undefined: may a second open add ports, change lcores or change time_source? `INSTANCE_MISMATCH` exists but has no rule. MTL_PORTS is read by a library, so a setuid helper or a plugin inherits the host's environment. With neither a spec nor MTL_PORTS, the result is unstated.
- **Fix**: "A shared open with ports, lcores, time_source or instance options that differ from the live instance is -MTL_EEXIST (INSTANCE_MISMATCH); a shared open with none of them joins. With neither spec, ports nor MTL_PORTS: -MTL_EINVAL, field "ports". MTL_PORTS is ignored when the process is set-uid." Fix the `port_count` comment.

### RV-25 — Minor — `mtl_flow` addressing is under-specified

- **Where**: mtl.h:376-391 (`ip[16]`, `ip_family` "0 = IPv4", `source_filter`), mtl.h:318-319 (`prefix_len` "0 = 24"), mtl.h:532 (`slot_bytes` "port MTU - 28"), mtl.h:564 ("a non-zero flows[1]").
- **Problem**:
  - The position of an IPv4 address in `ip[16]` (bytes 0-3, or IPv4-mapped) is unstated.
  - The non-zero value for IPv6 is unstated (`AF_INET6` is 10 on Linux and 23 on Windows).
  - A /24 default and "MTU - 28" are wrong for IPv6.
  - "Non-zero flows[1]" counts a stray `ttl`.
- **Fix**: "IPv4: ip[0..3], the rest zero; ip_family 0 = IPv4, 6 = IPv6; a leg exists when its udp_port != 0; IPv6 defaults: prefix 64, slot_bytes = MTU - 48."

### RV-26 — Minor — zero as "default" blocks a legal value

- **Where**: mtl.h:446 (`troffset_ns` "0 = TRODEFAULT"), mtl.h:571 (`min_tx_delay_ns` "0 = by source kind").
- **Problem**: a gateway that wants zero added delay, or a measurement setup that wants TROFFSET = 0, cannot ask for it. The options header already uses "absent, not zero".
- **Fix**: use `MTL_DEFAULT_NS (INT64_MIN)` for "derived", or document `-1` as the explicit zero. Either way, say it next to the field.

### RV-27 — Minor — the spec-string grammar is ambiguous

- **Where**: mtl.h:587-591; mtl_util.h:39-40 (`mtl_flow_parse`: ",pt=112,ssrc=...,dscp=34"); ex02:12-13; ex08:2-3.
- **Problem**:
  - The comma separates legs in `addr=` and also separates flow options in the flow grammar. "addr=239.1.1.1:20000,dscp=34,239.2.1.1:20000" can bind dscp to leg 0 or to both legs.
  - Positional tokens ("1080p59.94", "pcm24", "48000", "2ch") and enum spellings ("unit=rows", "media_mode=index") are not in any grammar.
  - Nested names ("video.raster.fps", "flows[1].dscp") are not defined.
  - "Fields the spec does not name keep their values" is not stated for `mtl_flow_parse` (ex13 reuses a live flow, so an old `source_filter` or `dst_mac` survives).
- **Fix**: publish the grammar (EBNF) in mtl_util.h. Separate legs with `|` or a repeated `addr=`. State that `mtl_flow_parse` zero-fills the flow first.

### RV-28 — Minor — withdraw: result order and AUTO slots

- **Where**: mtl_mem.h:138-139.
- **Problem**: 07 says results are published in submission order (the ring waits for predecessors). The header does not say that a withdrawn unit's FLUSHED result waits for them. It also does not say whether later AUTO units move up into the freed slot or leave a hole.
- **Fix**: "Its FLUSHED/WITHDRAWN result is published in submission order. Later AUTO units keep their slots; the withdrawn slot follows the underrun policy."

### RV-29 — Minor — attach flags, pool_count and access

- **Where**: mtl_mem.h:74 (`MTL_ATTACH_META_IN_SLOT 0x1`) versus mtl_mem.h:32 (`MTL_MEM_READ 0x1`) in one field (mtl_mem.h:90); mtl.h:566 (`pool_count`) versus mtl_attach.count; mtl_mem.h:96-99 (append, detach).
- **Problem**:
  - An attach with a null region imports memory but cannot pass READ/WRITE, because their bits collide with ATTACH bits.
  - The relation between `sc.pool_count` and the attached `count`s (equal? at least? appended beyond?) is unstated.
  - `detach` has no state rule.
- **Fix**: give ATTACH flags their own range (`0x100u`) and allow `MTL_MEM_READ/WRITE` in attach flags. State "with POOL_ATTACHED, pool_count is 0 or the total to be attached; start is -MTL_EINVAL (POOL_TOO_SMALL) until the attached count reaches it; detach: CREATED or STOPPED, no lease, hold or attached-over session".

### RV-30 — Minor — pointer lifetimes are stated for two pointers out of eight

- **Where**: mtl.h:348 (`lcores`), mtl.h:309 (`mtl_option.str`), mtl_mem.h:87 (`slot_offset`), mtl_observe.h:181 (`path`), mtl_plugin.h:88-91 (`name`, `pairs`), mtl.h:576 (`next`).
- **Fix**: one rule in R3: "Every pointer in an input struct is read during the call and deep-copied; the caller may free it on return. Exception: mtl_plugin_device and what it points to stay valid until mtl_plugin_unload returns."

### RV-31 — Minor — stats labels truncate and slots can move

- **Where**: mtl_observe.h:50-60 (`label[24]`), mtl_observe.h:62-68.
- **Problem**: "reason=rejected_at_pickup" is 25 characters, and "reason=media_time_backwards" 27, so they do not fit `label[24]`. Slot indices from `mtl_stat_list` are not promised to be stable when the schema grows (a second leg added by update, a reason label appearing), so a bulk reader reads the wrong counters with no error.
- **Fix**: `label[48]`; add `uint32_t schema_gen` to the list output, and make `mtl_stat_read` return `-MTL_ESTALE` when the generation changed.

### RV-32 — Minor — core fields point into optional headers

- **Where**: mtl.h:707 (`flow_state`: "mtl_queue.h enum mtl_flow_state"), mtl.h:629 (`pacing_class`: no enum named; `enum mtl_pacing` is in mtl_options.h).
- **Problem**: an app that includes only mtl.h, as D-71 promises it can, cannot interpret two status/info fields.
- **Fix**: move `enum mtl_flow_state` and the pacing-class enum into mtl.h, or name them in the field comments with their header.

### RV-33 — Minor — packet RX dequeue: copy or lend, and its call class

- **Where**: mtl.h:826 (`mtl_rx_dequeue` WT); mtl.h:536 (`rx_ring_packets` "NIC buffers the session may hold"); mtl_packet.h:13-16; S8 §5 table (COPY dequeue is DPC, `-MTL_EDEADLK` from busy loops); REVISION-4 M14 ("RX copies in the caller by default").
- **Problem**: the header has no COPY/LEND selector. The copy path makes dequeue DPC, not WT/DP, and that is not labelled.
- **Fix**: add `MTL_PKT_RX_LEND` to `packet_flags` (default copy), and annotate dequeue "DPC for packet units without MTL_PKT_RX_LEND".

### RV-34 — Minor — the results-ring invariant behind `MTL_BLOCKED_RESULTS` is unstated

- **Where**: mtl.h:263-266; mtl.h:674-677.
- **Problem**: losslessness (07) needs "a unit is admitted only if its result has a ring entry". Without that rule written down, an implementation that sizes the ring independently can make stop(DRAIN) block on a full ring that the stopping thread itself should reap.
- **Fix**: "The results ring holds pool_count entries; acquire reserves one, so producing a result never waits."

### RV-35 — Minor — no reason for units flushed by a drain timeout

- **Where**: mtl.h:674-675 ("ETIMEDOUT if DRAIN missed the deadline (then FLUSH)"); mtl_reasons.h:102 (`STOP_FLUSH`), mtl_reasons.h:21 (`DRAIN_TIMEOUT` is a lifecycle reason); 07 §2 (`STOP_TIMEOUT` result reason).
- **Fix**: add `MTL_REASON_STOP_TIMEOUT = 518` for those results, or state that they carry `DRAIN_TIMEOUT`.

### RV-36 — Minor — `MTL_SAME` does not do what its comment says

- **Where**: mtl.h:170-171.
- **Problem**: "mixing types does not compile" holds only in C++. In C it is a warning (gcc "comparison of distinct pointer types lacks a cast", clang `-Wcompare-distinct-pointer-types`). In C++, `MTL_SAME(s, MTL_NULL(mtl_session_h))` is an error (address of an rvalue), so the obvious null comparison fails there.
- **Fix**: say "warns in C, fails in C++", and add `#define MTL_IS_NULL` usage in the comment for null checks. Or use a C11 `_Generic`/C++ overload pair when available.

### RV-37 — Minor — option descriptors miss defaults, enum names and "unset"

- **Where**: mtl_options.h:205-236.
- **Problem**: `mtl_option_desc` has no default value and no enum value names, so a generic GStreamer property cannot show either. `mtl_get_option` returns `int64_t` only, so STR keys (`time.ptp_unicast`, `instance.dma`) cannot be read. Nothing returns a set option to "absent" (the derived default).
- **Fix**: `int64_t def; const char* enum_names;` ("drop,send_late,reslot") in the descriptor. `mtl_get_option_str()`, or `str` in an out `mtl_option`. `MTL_OPTION_UNSET` as a `mtl_set_option` flag.

### RV-38 — Minor — `mtl_last_error` cost on the polling path

- **Where**: mtl.h:131-138; R2.
- **Problem**: every `-MTL_EAGAIN` from a timeout-0 poll is a failure. If it formats `detail[128]` and `field[48]` into TLS, a 1 MHz poller pays a `snprintf` per miss.
- **Fix**: "-MTL_EAGAIN sets only code and reason; detail and field are left empty."

### RV-39 — Minor — interlaced field parity on RX without a valid index

- **Where**: mtl.h:225-229, 747; S1 U-187 ("parity from the media index").
- **Problem**: parity comes only from `media_index`, and RX sets `MTL_UNIT_INDEX_VALID` only sometimes (mediaclk sender, unlocked time). A receiver then cannot tell the first field from the second.
- **Fix**: add `#define MTL_UNIT_SECOND_FIELD 0x400000u` (RX: from the RTP second-field bit), valid independently of the index.

### RV-40 — Minor — ex03 reaps once and so may not re-arm RESULTS

- **Where**: ex03:18-19.
- **Problem**: R2's rule is "drain until -MTL_EAGAIN". `drain()` reaps once with max 16. When 1-16 results are ready the call returns a count, RESULTS stays un-armed, and later results do not fire the fd, so the loop wakes only at epoll's 100 ms timeout. It is the example that teaches the pattern.
- **Fix**:
  ```c
  int n;
  while ((n = mtl_tx_reap(s, r, sizeof(r[0]), 16, 0)) > 0)
    for (int i = 0; i < n; i++) source_frame_done(r[i].cookie, r[i].status, r[i].margin_ns);
  ```

### RV-41 — Nit — R6's busy-loop rule has no subject in revision 4

- **Where**: mtl.h:45-47.
- **Problem**: with user tasklets cut (M12), no application code runs on a library busy-loop thread, so "from a library busy-loop thread only DP calls ..." cannot apply. It reads as a leftover.
- **Fix**: keep it as "(applies to the advanced tasklet header, if kept; M12)".

### RV-42 — Nit — "hold" names two different things

- **Where**: mtl.h:778 (`unit.hold`: an RX lease kept by a TX unit), mtl_mem.h:140-141 (`mtl_tx_hold`: a slot kept out of acquire), mtl.h:268 (`MTL_BLOCKED_APP_HOLDS`).
- **Fix**: rename `mtl_tx_hold` to `mtl_tx_pin` (and the blocked reason to `MTL_BLOCKED_APP_PINS`), or rename the field `src_lease`.

### RV-43 — Nit — one prefix, several value spaces

- **Where**:
  - `MTL_UNIT_FRAME/ROWS/PACKETS` (kind) next to `MTL_UNIT_INDEX_VALID`... (RX flags), mtl.h:219-223, 747-752.
  - `MTL_EVENT_TIME_VALID` (flag) among event types, mtl_queue.h:64.
  - `MTL_FLOW_USER_MAC` (flag) next to `MTL_FLOW_WAITING_NEIGHBOUR` (state).
  - `MTL_PKT_*` used for packet_flags, RX entry flags and pacing enums: mtl_packet.h:41, 55 both have value 0x1u.
- **Fix**: `MTL_UNITF_*` for RX unit flags, `MTL_EVF_TIME_VALID`, `MTL_FLOWF_USER_MAC`, `MTL_PKTE_*` for table-entry flags.

### RV-44 — Nit — const-correctness

- **Where**: mtl_packet.h:78 (`mtl_pkt_tx_table(const struct mtl_unit*)` returns a writable pointer); ex03:11, ex07:11, ex08:8, ex10:9 (fill/decode/render/transform take `const struct mtl_unit*` and write the planes).
- **Fix**: take `struct mtl_unit*` in `mtl_pkt_tx_table` and in the example hooks that write. Keep `const` for readers.

### RV-45 — Nit — static inline helpers are invisible to bindgen

- **Where**: mtl.h:188-197 (`mtl_obj`), mtl_packet.h:75-115.
- **Problem**: bindgen emits no wrappers for `static inline` without `--wrap-static-fns`, so Rust users lose `mtl_obj`, `mtl_pkt_slot` and the RTP accessors.
- **Fix**: note "bindings reimplement these; they are layout-only", or export AS twins.

### RV-46 — Nit — pure parsers are labelled CP

- **Where**: mtl.h:591 (`mtl_session_config_parse`), mtl_util.h:40-42 (`mtl_flow_parse`, `mtl_raster_parse`), while `mtl_fps_parse` is AS.
- **Fix**: AS (or DP) for all four. They allocate nothing once RV-11 is fixed.

### RV-47 — Nit — close, destroy and unload

- **Where**: `mtl_session_close` (always consumes), `mtl_queue_close` (`-MTL_EBUSY` while bound, mtl_queue.h:104-105), `mtl_timeline_close`, `mtl_instance_close` (drop a reference), `mtl_mem_destroy` (`-MTL_EBUSY` while referenced), `mtl_plugin_unload`.
- **Fix**: one verb with one meaning. Let `close` always consume and defer (queue: unbind and retire; region: retire at the last reference with `MTL_EVENT_REGION_RELEASED`), and rename `mtl_mem_destroy` to `mtl_mem_close`.

### RV-48 — Nit — stale text after the S7 update

- **Where**: mtl_queue.h:9 ("behind one wait object": the struct is gone); mtl.h:116 (EAGAIN comment versus `mtl_session_wait` returning 0).
- **Fix**: "behind one wait handle"; align with RV-05.

### RV-49 — Nit — small consistency items

- `mtl_rx_timing_result.compliance` uses magic numbers 1/2/3 (mtl_observe.h:144); give them an enum.
- `MTL_MS(1.5)` truncates to 1 ms, because the cast happens before the multiply (mtl.h:104-106); say "integers only".
- `mtl_mem_alloc(..., void** va)` (mtl_mem.h:48-49) does not use the `MTL_ADDR` convention the structs use for bindings.

### RV-50 — Nit — concurrency flags default in opposite directions

- **Where**: mtl.h:551-552.
- **Problem**: submit is single-thread unless `MT_SUBMIT`, but reap/dequeue are multi-thread unless `SINGLE_READER`. Readers pay for CAS by default, and `mtl_rx_release` "any thread" is unaffected by `SINGLE_READER`, which the name suggests otherwise.
- **Fix**: make both default to single-thread and rename the flag to `MTL_SESSION_MT_READ`. State that release is always any-thread.

### RV-51 — Nit — check.sh lint gaps

- **Where**: check.sh lint 2 (comment versus code), compile runs.
- **Fix**:
  - Check that `mtl_reasons.h` has no `#include` and that every other non-core header includes at least one sibling.
  - Add `-pedantic` to the C++ runs.
  - Compile the examples with `-D__bindgen` once.
  - Add a probe that `MTL_SAME` of two handle types fails under `-Werror` in C.

## 4. Resolved by the S7 update

| ID | Was | Finding (first drop) | How the update resolves it | Residue |
|---|---|---|---|---|
| RV-52 | Blocker | `mtl_session_destroy` returned 0 ("retired") with only app leases in mind. With a stalled shared TX queue (04 §4.5) device references remained, and ex04 freed its arena after 0, risking DMA into freed memory | `mtl_session_close` defines 0 as "retired, every memory reference is gone" | 1 is still explained as "because the application holds leases" only (RV-04) |
| RV-53 | Major | `MTL_DESTROY_FORCE` ("do not wait for leases") either freed a pool the app still reads, or meant nothing | close has no force flag | — |
| RV-54 | Major | ex03 armed a fixed `ACQUIRE \| RESULTS` wait object. When the source was empty and slots were free, `mtl_session_wait(..., 0)` returned ready at once and the loop spun at 100 % CPU | arming only on `-MTL_EAGAIN` and a "slot first, then source" drain; the loop now sleeps in epoll | arming semantics themselves (RV-06); ex03 reaps once (RV-40) |
| RV-55 | Minor | `mtl_session_get_wait_object` wrote an output struct without a size argument (R3) | replaced by `mtl_session_get_wait_handle(s, mask, intptr_t*)` | — |
| RV-56 | Minor | stop with a timeout promised every result before returning, with no timeout outcome and no restart rule | stop: `-MTL_ETIMEDOUT` then FLUSH; "can be started again" | device-held units (RV-04), result reason (RV-35) |
| RV-57 | Minor | ex11 had to clear the instance interrupt before stop, because stop's drain counted as a wait | "stop and close still work"; interrupts cancel data waits only | — |
| RV-58 | Minor | examples released a lease after a failed submit while the header said the lease stayed the app's: easy to leak or double-release | submit consumes the lease on failure except `-MTL_EAGAIN`; `TxLease` follows | the rows-unit case (RV-01) |
| RV-59 | Nit | sketch/README.md described revision 3 (`mtl_unified.h`, `*_init()` lint, `MTL_UNIFIED_*` variants) | README rewritten for revision 4 | — |

## 5. What became simpler, and what became too simple

Simpler and still correct:

- one config struct;
- one unit struct;
- `MTL_INIT` for every input;
- results on or off;
- one reason vocabulary;
- the queue merging CQ and EQ;
- close as the one teardown call;
- packet units as a unit kind.

The size reduction (217 to 113 functions) did not cost a use case that the coverage check counts.

Became too simple to be correct, each fixable without a new concept:

- the start sentence (RV-07) and the audio index (RV-08);
- the ANC raster (RV-09);
- one failure rule for every unit kind (RV-01);
- "arms" (RV-06);
- the meta area (RV-02, RV-03);
- `update` with a whole config (RV-10).

Still more complex than needed (concrete cuts): the duplicated knobs (RV-22), the two dry runs (RV-23), `MTL_UPDATE_OPTIONS` (RV-10/RV-22), and the meta header for packet units (RV-02).
