# 06 — Timing, pacing, lateness and multi-essence synchronisation

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5) |
| Date | 2026-09-30 |
| Requirements | R-TIME-1…11, R-CMP-3 |
| Research | [R12 standards](research/12-st2110-timing-standards.md) and [R05 current timing](research/05-timing-pacing.md) (primary); [R11 prior art](research/11-media-io-prior-art.md); reviews [C2](reviews/C2-timing-standards.md) and [C5](reviews/C5-adversarial-user-review.md) with its [response](reviews/C5-response.md); `doc/user-pacing-timestamp-contract.md` ("the contract" — note: an **untracked working draft**, not in `545a266a`) |
| Conventions | Names follow C5-response Part A (A2 TAI-ns times in CQ records, A4 handles, A5 `MTL_E*` codes, A6 names, A7 zero defaults, A8 knob placement). Items changed in revision 3 are marked **(r3)** with the C5 finding they answer. If a struct here disagrees with `sketch/include/mtl/experimental/mtl_unified.h`, the header wins (A1). |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

This document answers the three items the maintainer left on PR #1610:

1. *"Consider adding 2 timestamps, one for user pacing, second for user timestamp."* →
   §4–§5: **media time** (what the unit represents; always drives RTP) and **launch time**
   (when packets leave; derived by default from media time and the session's source kind,
   overridable). The standards already separate them.
2. *"Both audio and video read from a single file, timestamped by the app … error up to
   half a video frame … can we make it easier?"* → §10: yes. A shared **timeline** whose
   anchor is fixed when the streams start, plus unit indices, makes every essence's RTP
   exact by construction; each essence derives its own launch time. The receiver half of
   the same question is answered in §11.6 (r3), and a legacy-API version ships first, in
   Phase 0.5 (§15, r3).
3. *"Can we automatically sync multiple streams of different types?"* → §10: a **group**
   starts them at one instant on one timeline, or independent sessions share a **named**
   timeline (one process) or the epoch plus an agreed index (several processes, §10.7);
   late handling is per unit, so one essence's problem never shifts another.

**What revision 3 changes** (every item answers a C5 finding):

- T0 is computed so that it is on the common grid for every start index, and interlaced
  members contribute TFRAME, so field parity never flips (§3.1) `[C5 §5.1]`.
- Preroll is a declared duration, and the horizon is measured from the resolved start;
  a start that would strand queued units beyond the horizon fails atomically (§7.4)
  `[C5 §5.2]`.
- One RTP rule for every field (§4.5); G-22 is rewritten `[C5 §5.3]`.
- `min_submit_lead_ns` is defined relative to media time, and `media_time_offset_ns`
  gives just-in-time producers a declared latency (§7.1, §4.6) `[C5 §5.4, §3.4]`.
- Processors, framework sinks, seeks and separate processes each have a written recipe
  (§10.7, §10.8) `[C5 §5.5, §5.6, §5.8, §5.9]`.
- New RX timing model (§11) `[C5 §5.7, §4.9, §8.4]`.
- TAI snapping defaults to NEAREST for every source kind, and it never locks out
  (§4.3). Audio absorbs PTS jitter and keeps its packet grid across gaps (§8)
  `[C5 §5.8, §5.14]`.
- Groups: start without a video member, join a running group, restart after a partial
  failure; the offset is inherited through the timeline (§10.6) `[C5 §5.10]`.
- The slot-delay bound comes from the receiver's link-offset budget, not from ST 2110-10
  §7.6.3 (§5.1) `[C5 §5.11]`.
- ST 2110-22 rate modes (§5.2), ST 2110-41 rate and launch (§4.2, §5.2), and the live-ANC
  window rule (§5.5, Q-TIME-18) `[C5 §5.12, §5.13, §5.15]`.

## 1. Principles

> **RTP describes the content. Pacing chooses when packets leave. Receive time is
> transmit time plus the network.** (the contract, Level 0; ST 2110-21's TVD model)

| # | Principle | Standards anchor | Today in MTL |
|---|---|---|---|
| T1 | One epoch: 1970-01-01 00:00:00 TAI; RTP clock offset zero: `rtp = floor(t × rate) mod 2^32` | ST 2059-1 §6.1, ST 2110-10 §7.3 | default time source is `CLOCK_REALTIME` (UTC) labelled TAI (`dev/mt_dev.c:2288`) `[R05 F1]` |
| T2 | RTP timestamp = the unit's sampling (or intended-presentation) instant; for playback video `N × TFRAME` | ST 2110-10 §7.5, §7.6.3 | default video RTP = packet-0 TX cursor `E(N) + TRO − VRX0·TRS`, **+54.4 to +55.7 ticks** at 1080p59.94 (note ¹, r3) `[R12 §3.5, C5 §5.15]` |
| T3 | The transmit grid is on the PTP timescale: `TVD = N × TFRAME + TROFFSET`, constant TROFFSET | ST 2110-21 §6.2 | EXACT user pacing accepts arbitrary per-frame targets `[R12 §10.2]` |
| T4 | Exact arithmetic; truncate | ST 2059-1 §5.1, ST 2110-10 §7.6.1 | `double frame_time` (≈ +66 ns at today's TAI for 1001 rates, **[inferred, computed]**) and round-to-nearest (`st_fmt.c:986-993`) `[R12 §3.5]` |
| T5 | "Regular increments" supersede everything: a late unit is dropped (gap), repeated, or the stream is explicitly re-anchored — never silently slid | ST 2110-10 §7.6.1, §7.7.1 | late frames go out in the current epoch with no counter if picked before the next epoch `[R05 F5]` |
| T6 | Essences of one programme agree by TAI instant, not by buffer boundaries | ST 2110-10 §7.1, ST 2110-40 §5.4 | default ST20 and ST40 RTP differ for the same frame `[R05 F7]` |
| T7 | No packet of a unit leaves before the unit's media time ("RTP not in the future" at the receiver) | JT-NM Tested; ST 2110-21 VRX model | not guaranteed for wide senders with large pre-fill `[C2 #15]` |

¹ `st_tx_video_session.c:63-70` (start time), `:793` (RTP from that cursor). 604–619 µs,
depending on the granted VRX0: 9 for TSC_NARROW, 6 for TSC with bulk 4, 5 for RL
(`:566-579`; VRX_FULL = 9 from `:3365`). C5's "≈ 608 µs ≈ 54.7 ticks" is the VRX0 = 8 point
of this range; revision 2's "+57" was TRO alone (57.39 ticks). Computed exactly (r3 script).

## 2. Clocks and time sources

### 2.1 Timestamp record

API arguments and getters use a small self-describing record, never a bare integer with a
magic zero (R-TIME-1). **CQ records do not** (r3, A2): every time inside a TX result or RX
unit is an `int64_t` TAI nanosecond value, with one bit per time field in the CQ header's
`uint32_t time_valid` (07 §1). "Every time in a CQ record is TAI or invalid" is a header
rule.

```c
struct mtl_time {
  int64_t  ns;              /* nanoseconds since the clock's epoch */
  uint16_t clock;           /* MTL_CLOCK_TAI | MTL_CLOCK_PHC_RAW | MTL_CLOCK_MONOTONIC | MTL_CLOCK_REALTIME */
  uint16_t flags;           /* MTL_TIME_VALID | _ESTIMATED | _HW | _SW | _HOLDOVER | _ACCURACY_VALID */
  uint32_t accuracy_ns;     /* meaningful only with MTL_TIME_ACCURACY_VALID */
};
int mtl_time_convert(mtl_instance_h mt, const struct mtl_time* in, uint32_t to_clock, struct mtl_time* out);
int mtl_time_cross_timestamp(mtl_instance_h mt, int64_t* tai_ns, int64_t* monotonic_ns, int64_t* realtime_ns);  /* r3 */
int mtl_time_diff_ns(int64_t a, int64_t b, uint32_t time_valid, uint32_t required, int64_t* out);                 /* A2 */
```

- **`MTL_CLOCK_TSC` is removed** from the public API (r3) `[C5 §5.15]`. A TSC value means
  nothing to a framework, and it differs between sockets. TSC stays an internal detail
  of the published time base (§2.3).
- **`mtl_time_convert`** maps TAI ↔ MONOTONIC ↔ REALTIME. It uses the cross-timestamps
  that the published time base carries (§2.3); it never reads a clock pair of its own. The
  result is exact at the record's instant and, between refreshes, extrapolated on the
  record's servo ratio.
- **`mtl_time_cross_timestamp`** (r3) returns one coherent triple, taken from one seqlocked
  time-base record and extrapolated to "now". A framework whose clock is neither MONOTONIC
  nor REALTIME builds its own map with it. Examples are the GStreamer audio ring-buffer
  clock, a `GstNetClientClock`, or an app media clock. The app samples its clock right
  before and right after the call, then fits `X ↔ TAI` over a few samples
  `[C5 §5.15, §7.8]`.
- **`mtl_time_diff_ns`** subtracts two CQ-record times. It returns `-MTL_EINVAL` when a bit
  of `required` is missing from `time_valid`, so an unset time never takes part in a
  difference.

This follows CoreAudio's `AudioTimeStamp` validity flags, ALSA's `actual_type` +
`accuracy_report`, and Vulkan's time-domain tags `[R11 §10 #3]`.

### 2.2 Instance time sources

| Source | How | Lock state available | Proposal |
|---|---|---|---|
| Built-in PTP client (`MTL_FLAG_PTP_ENABLE`) | PHC disciplined by MTL | yes (MTL's servo) | keep; fix sticky `locked` (`mt_ptp.c:540-552`) `[R06 Q2]` |
| External PTP disciplining the NIC PHC (linuxptp `ptp4l`/`phc2sys`) | read the PHC off the tasklet | via pmc or "unknown" | **add**; this is how Rivermax works `[R10 §6]` |
| `CLOCK_TAI` | kernel TAI clock | from the daemon | **add, validated**: `CLOCK_TAI = CLOCK_REALTIME + adjtimex().tai`, and the kernel TAI offset is 0 unless a daemon sets it; accept only if `tai ≠ 0` or a PHC cross-check agrees, otherwise it is UTC and today's 37 s bug returns `[C2 #10]` |
| User time source | app | app-declared | the app updates the published time base (§2.3) from a normal thread; no per-read callback |
| System realtime | `CLOCK_REALTIME` | none | only as an explicitly labelled `MTL_CLOCK_TAI` with `flags = ESTIMATED`; the UTC–TAI offset is taken from PTP announce `currentUtcOffset` when available, else configured; leap seconds emit `TIME_STEP`; an NTP leap smear is a rate error and is documented as such `[C2 #26]` |
| Test source (A12, `mtl_debug.h`) | `mtl_time_test_source(mt, base_tai_ns, rate)` + `mtl_time_test_advance` | declared LOCKED | the null backend and the U tier; the published time base honours it; build and runtime gating in 15 §6 |

### 2.3 Published time base

Tasklets never read a clock directly. Every source feeds a seqlocked record that is
refreshed off the tasklet, by the PTP servo, the admin thread, or the app for a user
source. Tasklets compute `tai = tai_base + (tsc − tsc_base) × ratio` wait-free.

This removes three kinds of hot-path work today `[C1 #7, C2 #10]` (Q-THR-7a):

- the user `ptp_get_time_fn` callback;
- `rte_eth_timesync_read_time` PMD reads for built-in PTP;
- `clock_gettime` on `/dev/ptpN`, which has no vDSO (a syscall plus a PCIe read).

The record's contract is owned by [04 §8](04-threading-and-execution.md) (r3, from
C5 §4.5):

- it is refreshed at least every 100 ms;
- the ratio comes from a least-squares servo over recent PHC/TSC cross-timestamps;
- publication is **slewed**: a new record never steps, except on a declared clock step,
  which follows the timeline's `step_policy` (§2.4);
- there is one record per socket;
- the record carries `monotonic_base`/`realtime_base` cross-timestamps, which
  `mtl_time_convert` and `mtl_time_cross_timestamp` use.

This document relies on two properties of it:

- every tasklet on every socket computes the same TAI for the same instant, within the
  servo error;
- a refresh never moves a scheduled launch backwards by more than the slew bound.

How close the extrapolated record comes to a direct PHC read is not assumed: spike S7
measures it, and its bound is the accuracy figure (04 §8.1, 13 §8).

With two NICs, each leg's PHC offset is compensated into the one instance timescale.
Media time and every leg's launch use that timescale. Today pacing reads port P for
every leg (`st_tx_video_session.c:695`, `st_tx_audio_session.c:265-266`).

### 2.4 Status and steps

```c
int mtl_time_now(mtl_instance_h mt, struct mtl_time* now);            /* DP: wait-free */
int mtl_time_get_status(mtl_instance_h mt, uint32_t port, struct mtl_time_status* st);
/* source, state (FREERUN | ACQUIRING | LOCKED | HOLDOVER | LOST), offset_ns, path_delay_ns,
   last_sync_age_ns, grandmaster id + domain (for SDP ts-refclk), utc_offset, step count,
   ST 2059-2 SM TLV fields when present (for timecode, later) */
```

Events: `TIME_STATE` (lock/unlock/holdover) and `TIME_STEP` (with the step size). Both go
to every EQ subscribed with `MTL_EQ_SUB_TIME`. A downstream engine patched MTL three times
to get exactly these getters `[R08 §3.3]`.

Clock steps are handled **per timeline** (`step_policy`), because the right answer depends
on what the anchor means `[C2 #10]` (Q-TIME-2):

| Policy | Default for | On a step of Δ |
|---|---|---|
| `MTL_STEP_REANCHOR` | timelines not on the epoch (file playout, AT_START anchors) | `T0 += Δ`; every member session emits one implicit DISCONTINUITY; RTP jumps with the clock; nothing is dropped for the step itself |
| `MTL_STEP_KEEP` | the epoch timeline and AT_TAI timelines | media indices stay; the late policy applies to units whose deadlines passed (forward step) |

**AUTO sessions after a step (r3)** `[C5 §5.15]`. The epoch timeline cannot re-anchor.
The AUTO *cursor* is what reacts:

- **Forward step.** Queued AUTO units whose slot is now in the past are RESLOT forward
  (AUTO's late policy, §7.2). Each result carries `slots_skipped_before`.
- **Backward step Δ ≤ `horizon_ns`.** AUTO never stamps a smaller index. The next unit
  gets `last + 1`, which now lies Δ further in the future, so the stream pauses for Δ with
  continuous RTP.
- **Backward step Δ > `horizon_ns`.** `TIME_STEP` is posted, and the session re-anchors
  **its AUTO cursor only**: the next unit takes the current slot, which is a smaller index.
  The result carries `MTL_TX_TIMING_DISCONTINUITY`. The timeline, and INDEX/TAI sessions
  on it, are unchanged. This is the one case where AUTO emits a backward RTP jump, and
  receivers relock (§11.5).

## 3. Timelines

### 3.1 Definition and anchoring

A timeline maps a *unit index* of any stream to an exact TAI instant:

```text
M(index) = T0 + index × P          P = the stream's unit period (seconds, exact rational)
```

```c
struct mtl_timeline_config {
  uint32_t struct_size;
  uint32_t anchor_mode;          /* MTL_ANCHOR_AT_START (0, default) | MTL_ANCHOR_NEXT_GRID | MTL_ANCHOR_AT_TAI | MTL_ANCHOR_EPOCH */
  int64_t  tai_ns;               /* AT_TAI: the anchor; NEXT_GRID: lower bound */
  struct mtl_rational grid;      /* used only with MTL_TIMELINE_GRID_EXPLICIT; otherwise derived (§3.3) */
  uint64_t flags;                /* MTL_TIMELINE_GRID_EXPLICIT, … */
  uint32_t step_policy;          /* §2.4; 0 = default for the anchor mode */
  uint32_t reserved0;
  uint64_t reserved[4];
};                               /* 80 B */
int mtl_timeline_create(mtl_instance_h mt, const struct mtl_timeline_config* c, mtl_timeline_h* out);
int mtl_timeline_open  (mtl_instance_h mt, const char* name, const struct mtl_timeline_config* c, mtl_timeline_h* out);
mtl_timeline_h mtl_timeline_epoch(mtl_instance_h mt);                    /* A4: the epoch timeline has a real handle */
int mtl_timeline_get_anchor(mtl_timeline_h tl, struct mtl_rational* t0_seconds, struct mtl_time* t0); /* -MTL_EAGAIN until resolved */
int mtl_timeline_index_at(mtl_timeline_h tl, mtl_session_h s, int64_t tai_ns, int64_t* k);          /* r3, L4, §10.7 */
int mtl_rational_index_at(int64_t tai_ns, struct mtl_rational unit_rate, int64_t* k);               /* r3, L4, no instance */
```

| Anchor mode | T0 is fixed… | Use |
|---|---|---|
| `EPOCH` | at create: T0 = 0 (the SMPTE epoch) — a video index is the frame number since 1970, an audio index the sample number | the implicit timeline of every session that names none; live generators; **every RX session by default** (§11.1); separate processes (§10.7) |
| `AT_START` (**default** for created timelines) | **lazily**, at the first `mtl_group_start` or `mtl_session_start` on the timeline, by the formula below | file playout: index k (usually 0) is always the first unit, never in the past |
| `NEXT_GRID` | lazily as AT_START, but never before `tai_ns` | NMOS-scheduled starts with a lower bound |
| `AT_TAI` | at create: T0 = `tai_ns` snapped up to the grid G (snap reported); with `MTL_TIMELINE_GRID_EXPLICIT`, G is the given grid, so every process that passes the same `tai_ns` and grid gets the same T0 | externally agreed anchors; RX (§11.1); separate processes (§10.7) |

**The start formula (r3)** `[C5 §5.1]`. A start at media index k, whether
`AT_MEDIA_INDEX k` or `START_NOW` (which means k = 0 on an unresolved timeline), resolves:

```text
A  = now + lead + preroll_ns                      lead = max over members of max(0, min_submit_lead_ns)   (§7.1)
T0 = ceil( max(A − k·P_k, B) / G ) · G            P_k = unit period of k's stream; B = tai_ns for NEXT_GRID, else −∞
S  = T0 + k·P_k                                   the resolved start instant (returned by start)
```

- **T0 is on the grid by construction.** Unit k's media time is `S ≥ A`. Revision 2
  computed `ceil(A/G)·G − k·P`. That is on the grid only when `k·P` is a multiple of G:
  at 59.94p, only when k ≡ 0 (mod 10). For k = 3, 7 or 13 it gave a half-integer
  `T0·90000` (for example `…999991831/2`) and a fractional `T0·48000` (`.6`, `.4`).
- **k's stream** is the group's first video member. With no video member it is the first
  member in add order (§10.6). For a single session it is the session itself.
- **Interlaced members contribute TFRAME to G, not TFIELD** (§3.3). T0 is therefore always
  a first-field instant, whatever k's parity: even indices are first fields, and parity
  never flips. An odd k starts on a second field; that unit's M is `T0 + k·TFIELD`.
- The same formula applies to `MTL_START_AT_MEDIA_INDEX k` on a single session with an
  unresolved timeline.
- **Checked numerically (r3)** with exact rationals, for k = 0…100 and 20 random `now`,
  lead and preroll values per configuration. The configurations were 59.94p + 48 kHz +
  ANC, 50p + 48 kHz + ANC, 29.97p + 48 + 96 kHz, 1080i59.94 + 48 kHz + ANC(i), 1080i50 +
  48 kHz + ANC(i), 59.94p + 29.97p + 48 kHz, audio only, and 59.94p + 48 + 44.1 kHz
  (44.1 kHz excluded).
  - The new formula gave `T0·R` integer for every grid member, T0 frame-aligned for every
    video/ANC member, and `S ≥ A`, in 2020 of 2020 cases per configuration.
  - The old formula put T0 off the grid in 1800 of 2020 cases at 59.94p, and in 1000 of
    2020 at 1080i50 (odd k flips parity).
  - This is G-row "T0 on the grid" in 13 §3.

Other rules:

- **Resolved timelines.** Starting `AT_MEDIA_INDEX k` on an already-resolved timeline whose
  `M(k)` is closer than `now + lead` fails with `-MTL_ERANGE`, reason `START_IN_PAST`. It
  never silently drops the start. Sessions that join a resolved timeline later start with `MTL_START_NOW` (§10.3),
  or re-base with `mtl_session_discard_queued` (§10.8).
- **Named timelines** (r3) `[C5 §5.6]`. `mtl_timeline_open(mt, "prog1", c, &tl)` creates
  the timeline on first open and returns the same object, refcounted, afterwards.
  - A later open whose known config fields differ from the first opener's returns
    `-MTL_EEXIST`, reason `TIMELINE_CONFIG_MISMATCH`. Before this revision the behaviour
    was undefined.
  - Names are **per instance**, so a named timeline is never shared across processes
    (§10.7 covers that).
  - Session names and timeline names are **separate namespaces**. Both are ≤ 63 bytes.

### 3.2 Exact arithmetic

All media-time math uses integers with 128-bit intermediates. That holds while TAI ns
< 2^63 (until 2262) and rates < 2^20.

- RTP is `floor(M × R)`; reported ns values are `floor(M × 1e9)`.
- Only final launch instants are rounded to ns.
- Every unit is computed from the anchor, never by adding a rounded period `[R12 §10.3]`.

The same functions are exported as L4 helpers, so apps, tests, the RX side (§11.2) and
the Phase 0.5 legacy helper (§15) all use identical math.

### 3.3 Common grid

The grid G is the smallest period on which all of the following hold:

- every **video, ANC and grid-member fastmeta** stream starts on an integer *frame*
  (interlaced: frame, not field, r3);
- the 90 kHz clock ticks an integer number of times;
- every audio rate ticks an integer number of times — **except audio rates that would push
  G above the 1 s cap**.

Excluded audio rates are floor-aligned (`floor(T0 × Fs)`), and their sub-sample phase is
reported (< 1 sample, 22.7 µs at 44.1 kHz). Free-running fastmeta streams (§4.2) are
excluded. If the video/ANC and 90 kHz part alone exceeds 1 s, the group fails with
`-MTL_EINVAL` and a reason `[C2 #3]` (Q-TIME-19).

These exact values were recomputed with rational arithmetic by review C2, and again for
revision 3; every row checked:

| Members | Smallest common G | Frames per G | Note |
|---|---|---|---|
| 25p or 50p (+ 48/96 kHz) | 1/25 s, 1/50 s | 1 | 1080i50 (25 frames) + 48 kHz: 1/25 s |
| 24p (+ 48 kHz) | 1/24 s | 1 | the draft's "1/30 s" was wrong for 24p (0.8 frames) |
| 30p, 60p (+ 48 kHz) | 1/30 s, 1/60 s | 1 | |
| 24p + 30p | 1/6 s | 4 / 5 | |
| 29.97, 59.94, 23.976, 119.88 (+ 48/96 kHz); also 1080i59.94 (29.97 frames) | **1001/6000 s = 166.83 ms** | 5, 10, 4, 20 | 15015 ticks, 8008 samples (16016 at 96 kHz) |
| 1001 families + 44.1 kHz audio only | 44.1 kHz would need 1001/300 s = 3.337 s, **above the cap** → excluded, floor-aligned, phase reported; G = 1001/30000 s for 29.97/59.94/119.88p, 1001/6000 s for 23.976p | 1, 2, 4 (23.976p: 4) | 3003 ticks (15015 for 23.976p); 735.735 samples/frame at 59.94 |
| 1001 families + 48 kHz + 44.1 kHz | 1001/6000 s (set by 48 kHz); 44.1 kHz excluded and floor-aligned as above | 5, 10, 4, 20 | 8008 samples per G at 48 kHz; 7357.35 at 44.1 kHz (not integer, hence excluded) |
| 24p + 44.1 kHz | 1/12 s | 2 | |
| audio only (48 kHz) | 1/6000 s (90 kHz and 48 kHz ticks) | — | k counts samples |
| 25p + 59.94p (two video families) | 1001/25 s = 40.04 s → above the cap: `-MTL_EINVAL` | — | |
| 50p + 59.94p | 1001/50 s = 20.02 s → above the cap | — | |

### 3.4 Interlaced and PsF members

- **Interlaced.** The index counts **fields**, at twice the frame rate: 60000/1001 fields/s
  for 1080i59.94. The configured `fps` is still the **frame** rate (10 §1.3).
  - T0 is always frame-aligned (§3.1, §3.3), so even indices are first fields.
  - Any slot delay is rounded to whole frames `[C2 #17]`.
  - Legacy `ops.fps` for interlaced sessions is the *field* rate: `frame_time` is computed
    from it and is a field period (`st_tx_video_session.c:499-518`), and the ST 2110-22 box
    header halves it back to the frame rate (`num >> interlaced`, `:865-866`). A migrating
    app that maps `ST_FPS_P59_94` + interlaced to `{60000, 1001}` asks for a 119.88-field
    raster, so the migration table in 11 halves it.
- **PsF (r3)** `[C5 §5.15]` is paced as interlaced: two segments, the second read from
  `TVD + TFRAME/2 + TLINE/2` (ST 2110-21 §6.3.3 covers "interlaced/PsF"). It has **one
  unit, one media index and one RTP per frame**, because both segments carry the same RTP
  (ST 2110-10 §7.6.1).
  - The layout has height rows (05, from C5 §6.9).
  - ANC for PsF has one unit per frame. The keep-alive rule sends at least one RTP packet
    per segment (ST 2110-40 §5.5). Both carry the frame's RTP, and the F bits follow the
    segment in the interlaced SDI raster that carries PsF.

## 4. Media time on TX

### 4.1 Session media mode and the submission

The media-time *mode* is a property of the session, not of each submission. A session
therefore cannot mix modes, and a forgotten flag cannot silently turn a timestamp into
AUTO `[C3 P3-4, P3-5; C2 #19]`:

| Session `media_mode` | What the app says per submission | Who it is for |
|---|---|---|
| `MTL_MEDIA_AUTO` (0) | nothing; MTL stamps the unit with the slot it gets | live generators, simple apps |
| `MTL_MEDIA_INDEX` | `media_index` = unit *k* of the timeline | playout, file sources, sync groups — **exact** |
| `MTL_MEDIA_TAI` | `media_tai_ns` = the instant this unit represents | capture, framework sinks, processors, apps that think in ns; snapped per `snap_mode` (§4.3) |

Timing fields of `struct mtl_tx_submission` (the full struct, with `hold`, `anc` and meta,
is in the header and 10 §1.8):

```c
  int64_t  media_index;               /* session mode INDEX */
  int64_t  media_tai_ns;              /* session mode TAI (absolute TAI, on any timeline, §4.6) */
  struct mtl_launch { uint32_t mode;  /* 0 = MTL_LAUNCH_DERIVED | NOT_BEFORE | EXACT */
                      uint32_t reserved; int64_t tai_ns; } launch;
  uint32_t rtp_override;              /* valid bit MTL_SUB_RTP; PASSTHROUGH sessions only (§5.4) */
  uint32_t sample_count;              /* audio: authoritative */
  uint32_t valid_bytes;               /* ANC/fastmeta/cvideo bytes used (audio: derived from sample_count) */
  uint32_t ready_rows;                /* progressive: rows already published at submit (§9) */
  uint64_t flags;                     /* MTL_SUBMIT_DISCONTINUITY, … */
```

- Zero means "library default" for every mode field (`launch.mode = 0` is DERIVED). A NULL
  submission pointer means "all defaults". `struct_size` must be set; 0 is `-MTL_EINVAL`
  `[C3 P1-4]`.
- A non-zero value in a field that the session mode does not use (for example
  `media_index` in an AUTO session) is `-MTL_EINVAL`.

### 4.2 Units per essence

| Essence | Unit | What `media_index` counts | Notes |
|---|---|---|---|
| video / cvideo progressive | frame | frames | |
| video / cvideo interlaced | field | fields at the field rate; even = first field (§3.4) | the library no longer toggles `second_field` on its own |
| video PsF | frame (two segments) | frames | both segments carry the same RTP (ST 2110-10 §7.6.1), §3.4 |
| audio | first sample of the submission | samples | a submission carries `sample_count` samples; buffer boundaries need not match video frames or packets (§8) |
| anc | ANC unit for one video frame/field | the associated *video* index | RTP equals the associated video RTP; the unit's ANC packets are described by `struct mtl_anc_packet`¹ in the submission (TX) and in the buffer's meta area (RX) `[C3 P3-2]`; F bits follow index parity (`0b10` first field, `0b11` second, `0b00` progressive) `[C2 #22]`; limits in §4.7 |
| fastmeta (r3) | data item group for one video frame/field, or one free-running unit | the associated video index (default), or units of `mtl_fastmeta_config.rate` | R = 90000 always; §4.8 `[C5 §5.13]` |

¹ `struct mtl_anc_packet { did, sdid, line, hoffset, c, stream, udw_count, udw_offset }` (10 §1.3).

### 4.3 Snapping TAI-mode times (r3)

A TAI-mode submission's `media_tai_ns` (after `media_time_offset_ns`, §4.6) is snapped to a
slot of the session's grid. Revision 2 locked a phase φ₀ at the first unit for CAPTURE and
accepted later units only within TFRAME/4 of it. C5 showed that this locks out:

- a free-running source drifts outside the tolerance (a 50 ppm crystal at 59.94 in about a
  day; a 30.000 fps camera on a 29.97 session in seconds);
- after that, every frame is `OFF_GRID`, the session stays RUNNING, and the receiver sees
  black `[C5 §5.8]`.

Revision 3 makes NEAREST the default for every source kind:

| `snap_mode` | Rule | Default for |
|---|---|---|
| `MTL_SNAP_NEAREST` (0) | each unit snaps to the nearest slot `N·P` (φ = 0), with a hysteresis band around the expected slot (below) | **every source kind** (PLAYBACK, CAPTURE, GATEWAY); AUTO and INDEX sessions do not snap |
| `MTL_SNAP_LOCKED_PHASE` (opt-in) | φ₀ is locked at the first unit (or a DISCONTINUITY); each later unit is expected at `M_prev + P` and accepted within `snap_tolerance_ns` (default TFRAME/4); RTP = `floor((N·P + φ₀) × R)` keeps regular increments and the sampling phase (production intent, ST 2110-10 §7.6.2) | genlocked cameras whose sampling phase must be preserved |

**Hysteresis.** The expected slot is `prev + 1`. A value within `P/2 + snap_tolerance_ns`
(default TFRAME/8) of the expected slot takes it, so jitter around the half-period never
flips between N and N+1 `[C2 #5]`.

**`snap_tolerance_ns` = 0 means "by snap mode"**: TFRAME/8 under NEAREST (the hysteresis
band above) and TFRAME/4 under LOCKED_PHASE, for video, cvideo, ANC and fastmeta. Audio
snaps to the sample grid and uses `audio_absorb_samples` under NEAREST (§8); under
LOCKED_PHASE its tolerance is one packet (S samples).

**NEAREST does frame-rate adaptation by drop and repeat, and it never locks out.** Each unit
is snapped on its own; nothing is locked.

- A source that runs faster than the session makes two units land on one slot. The second
  is `DROPPED/DUPLICATE_SLOT`, a *result*, never a synchronous error.
- A source that runs slower leaves a slot empty. The underrun policy covers it: SKIP
  (default, a gap) or, later, REPEAT_LAST (§7.3).
- RTP always stays on the `N·TFRAME` grid, so regular increments hold unit by unit, and a
  receiver never sees a phase jump.

This is better than C5's proposed `REANCHOR` default:

- every re-anchor moves φ, so the RTP-offset metric at the receiver jumps at each one;
- each re-anchor is an implicit DISCONTINUITY;
- a steadily drifting source re-anchors periodically forever.

NEAREST needs none of that: a drop or a gap is exactly what ST 2110-10 §7.6.1 ("regular
increments") allows.

**LOCKED_PHASE relock (r3).** `off_grid_policy`, used by LOCKED_PHASE only, is one of:

- `MTL_OFF_GRID_RELOCK` (0, default): after **3 consecutive** `OFF_GRID` units, the third
  relocks. Its phase becomes φ₀, it is sent with `MTL_TX_TIMING_RELOCKED` and an implicit
  forward DISCONTINUITY, and `TIMING` status reports the phase step. The first two are
  `DROPPED/OFF_GRID`.
- `MTL_OFF_GRID_REANCHOR`: relock at the first `OFF_GRID` unit.
- `MTL_OFF_GRID_DROP`: never relock. For genlock monitoring only; it reports a
  `timing_warning`.

If the new phase would move RTP backwards relative to the last unit sent, the relocking unit
takes the next slot, because DISCONTINUITY is forward-only (§4.4).

Snap error and phase drift are reported per result (`snap_error_ns`) and as a gauge. The
default snap per source kind is in the 09 defaults table (NEAREST everywhere) `[C5 §3.7]`.

### 4.4 Ordering, gaps and discontinuities

- Media indices per session strictly increase in submission order.
  - **INDEX mode (r3):** a submission whose index is not greater than the last one is a
    **result**, not an error: `DROPPED/DUPLICATE_INDEX` for an equal index,
    `DROPPED/BEHIND` for a smaller one. This covers framework muxers whose
    `av_rescale_q` of a 29.97 stream repeats an index `[C5 §5.8]`. `-MTL_EINVAL` stays
    for a malformed submission only.
  - Audio is covered by §8 (absorb, gap fill, overlap), never by `-MTL_EINVAL`.
- A **forward gap** means "no unit for those slots"; the receiver sees a regular-increment
  gap. Audio forward gaps keep the packet grid (§8, r3).
- `MTL_SUBMIT_DISCONTINUITY` marks an intended re-anchor. **While RUNNING it may only jump
  forward** `[C2 #12]`.
  - Other receivers drop a backwards RTP jump inside a running stream, and so does MTL's
    own RX: `st_rx_video_session.c:1176-1197` treats older RTP as "in the past".
  - The one exception is in that same code: once every port's redundant-error count has
    reached `ST_SESSION_REDUNDANT_ERROR_THRESHOLD` (20, `st_header.h:81`), it accepts the
    old timestamp, which is today's implicit relock (C5 §5 note; the unified RX rule is
    §11.5).
  - A backward re-anchor requires stop/start (sequence numbers restart per RFC 3550).
  - On the epoch timeline, sender restarts are RTP-continuous by construction (ST 2110-10
    §7.3 note 2).
- **Negative indices (r3)** `[C5 §5.15]` are legal on EPOCH and AT_TAI timelines when
  `M(k) ≥` the session's start instant S. An example is audio priming before an AT_TAI
  programme start. On an AT_START timeline every index below the start index is before S, and
  is `FLUSHED/BEFORE_START`. The G-19 oracle checks negative indices in the math;
  the engine tests check them only where they are legal.

### 4.5 Derived RTP

```text
RTP(unit) = floor(M(unit) × R) mod 2^32            R = 90000 (video, cvideo, ANC, fastmeta), Fs (audio)
video, T0 on the grid:  RTP_v(k) = T0·90000 + floor(k × ticks_per_unit)      (T0·90000 is an integer, §3.1)
video, general:         RTP_v(k) = floor((T0 + k·P) × 90000)
every field:            RTP_v(field) = floor(M(field) × 90000),  M(2m+1) = M(2m) + TFRAME/2          (r3)
audio packet p:         RTP = floor(T0 × Fs) + p × S          (p = absolute packet index since T0; S = samples per packet;
                                                              after a re-phasing DISCONTINUITY at index d: floor(T0 × Fs) + d + p × S, §8)
ANC / fastmeta for video unit k:   RTP(k) = RTP_v(k)
```

**Second field (r3)** `[C5 §5.3]`. The fixed line `second field = first + floor(TFRAME ×
90000 / 2)` is deleted. There is one rule, `floor(M × R)`, for every unit, and that is
what MTL does today: each field comes from its own epoch, and an even slot is the first
field (`st_tx_video_session.c:706-711`).

The two rules agree for every SDI interlaced format. With T0 on the grid (§3.1), the
second-field offset per frame is:

| Format | Offset | Why |
|---|---|---|
| 1080i59.94 (29.97 frames, TFRAME = 3003 ticks) | always **+1501** | TFRAME·90000 is an integer |
| 1080i50 | always **+1800** | TFRAME·90000 is an integer |
| 1080i60 | always **+1500** | TFRAME·90000 is an integer |
| a 119.88-field raster (59.94 *frames*, TFRAME = 1501.5 ticks) | alternates **+750/+751** | TFRAME·90000 is not an integer; this is where C5's numbers come from |

So C5's "G-19 and G-22 cannot both pass" does not occur for broadcast rasters. A
119.88-field raster is exactly what a migrating app asks for by mistake (§3.4). ST 2110-10
§7.6.1 ("offset … by one half of the prevailing frame period, truncating") is satisfied by
truncating the result, as §7.5 requires of every timestamp. All four rows were verified
with exact rationals (r3 script).

One rule for all essences, from media time only (R-TIME-3). Default media time is the grid
(`N × TFRAME`), which fixes the ST20/ST40 disagreement (T2, T6).

### 4.6 Media-time offsets (r3)

Two per-session offsets move media time. Both move RTP with it, and neither is an RTP clock
offset (ST 2110-10 §7.3 stays zero):

| Field (`mtl_timing_config`) | Unit | Applies | Meaning |
|---|---|---|---|
| `media_index_offset` | whole units | INDEX: `M = T0 + (k + offset)·P`; TAI: `M = snap(media_tai_ns + media_time_offset_ns) + offset·P` | lip-sync trim, production intent (ST 2110-10 §7.7.3); inherited through the timeline (§10.6) |
| `media_time_offset_ns` | ns, signed | TAI, AUTO and INDEX (see below) | TAI: the **framework latency** knob for just-in-time producers (§10.8) `[C5 §5.4]`; AUTO/INDEX: the ns-precision replacement for `rtp_timestamp_delta_us` `[C5 §3.4]` |

How `media_time_offset_ns` applies:

- **TAI mode:** it is added *before* snapping, so it selects the slot, and the derived
  launch follows.
- **AUTO and INDEX modes:** it is added to the stamped M *after* the slot is chosen, and
  the launch stays the slot's. This is exactly today's `rtp_timestamp_delta_us`, RxTxApp's
  compliance-test trim (`tests/tools/RxTxApp/src/tx_st20p_app.c:311`), with ns precision.
  Its magnitude must be below TFRAME (ST 2110-10 §7.6.3), else create fails with
  `-MTL_ERANGE`. `get_info` flags `RTP_OFF_GRID` when it is not a multiple of P.

- **TAI mode on a non-epoch timeline** `[C5 §5.15]`.
  - `media_tai_ns` is absolute TAI.
  - The reported `media_index` is timeline-relative: the snapped slot's `(M − T0)/P`.
  - `media_index_offset` shifts M, so RTP moves too.
- There is **no RESLOT for TAI**: "never slide" holds. A TAI-mode producer that is late is
  told so by `DROPPED/TOO_LATE` with `margin_ns`. The fix is a larger
  `media_time_offset_ns`, and §10.8 gives the number.

### 4.7 ANC limits (r3)

`[C5 §5.15]`:

- **`anc_count ≤ 255` per unit.** RFC 8331 `ANC_Count` is 8 bits. Today the TX meta array
  is capped at `ST40_MAX_META = 20` (`st40_api.h:308`); the unified API checks 255 and the
  unit's `buffer_capacity_bytes` (09, default 64 KiB) and returns `-MTL_EINVAL` at submit.
- **`udw_count ≤ 255`, checked** (`-MTL_EINVAL`). The wire field is 8 bits;
  `st40_rfc8331_encode_packet` already rejects more (`st40_api.h:901`).
- **`line = 0` means "unspecified".** MTL writes RFC 8331's no-specific-line code, and the
  ST 2110-40 §6.2.1 TLBO rule for packets without a line number applies: two lines after
  the RP 168 switching point.
- **RX ANC does not use `total_lines`.** It is a TX-only value, needed for TEPO (§5.2).
- **Interlaced ANC from a file** uses `index = 2·pts + field` and two ANC units per frame
  (10 §8).
- **`mtl_tx_write(anc, udw, len, sub, timeout)`** takes one UDW byte run for the whole
  unit. Each `mtl_anc_packet.udw_offset` points into it, and the packets may have different
  DID/SDID/line. Offsets are increasing and non-overlapping, and `udw_offset + udw_count ≤
  len`; otherwise the call returns `-MTL_EINVAL`.

### 4.8 Fastmeta (ST 2110-41) timing (r3)

`[C5 §5.13]`. Today's ST41 is video-rate based. Its pacing epoch is
`frame_time = 1e9·den/mul` from the session `fps`, with RTP `epochs × frame_time_sampling`
at 90 kHz (`st_tx_fastmetadata_session.c:206-234`; `sampling_clock_rate = 90000` in
`st_fmt.c`). `fps`, `interlaced` and `second_field` are in `st41_tx_ops` and
`st41_tx_frame_meta` (`st41_api.h:135-175`). Revision 2's "arbitrary rational rate,
excluded from the grid" contradicted this. Revision 3:

- **R = 90000**, as today. A Data Item spec that needs another RTP clock is a later
  extension; ST 2110-41 §5.3 lets the item spec define the clock.
- **`mtl_fastmeta_config.rate`** is the **associated video frame rate**, like ANC's `fps`.
  - The unit is one data item group per video frame/field. `media_index` is the video
    index, RTP = the video RTP, and the member sits on the group grid like ANC.
  - `rate` = 0/0 takes the frame rate of the group's video member at group start. A
    session that starts outside such a group with 0/0 fails start with `-MTL_EINVAL`,
    reason `FIELD_REQUIRED`.
  - With `MTL_FASTMETA_FREE_RUNNING` (in `fastmeta_flags`), `rate` is the stream's own unit
    rate and is required. The stream is off the video grid (excluded from G), and M(k) =
    T0 + k/rate. In TAI mode it takes each unit's media time as given, with no snap; RTP =
    `floor(M × 90000)`.
- **`interlaced`** is in `mtl_fastmeta_config`, and field parity follows the index (as for
  ANC). `second_field` is not an input.
- **Launch** is `M + fastmeta_target_delay_ns` (§5.2), in `mtl_fastmeta_config`.

## 5. Launch time

### 5.1 Source kinds, the minimum transmit delay and the slot-delay bound

Review C2 showed that the draft's defaults drop every live camera frame. With slot delay 0
the slot is `M + TROFFSET`, which passes before a captured frame even exists `[C2 #1]`. The
session therefore declares what it is:

| `source_kind` | Meaning | Defaults |
|---|---|---|
| `PLAYBACK` (0, default) | content exists before its media time (files, graphics, generators) | `min_tx_delay = 0`; snap NEAREST; `tsmode = SAMP` if media mode is INDEX/TAI |
| `CAPTURE` | the unit exists only after its sampling instant (camera, microphone, RX→TX processor) | `min_tx_delay` **0 = one unit period + the session's `pickup_lead_ns`** (§7.1; never 0, r3) `[C5 §3.7]`; snap NEAREST; the first late unit posts `TIMING_INFEASIBLE` and sets `timing_warning` (reason `TIMING_SHORTFALL`, the shortfall, a suggested `min_tx_delay`); `tsmode = SAMP` |
| `GATEWAY` | SDI→IP and low-latency cameras that transmit in the same frame period | L = 0; `troffset_ns` from the app (< TFRAME, signalled as TROFF); progressive submission (§9); `tsmode = PRES` or `SAMP` as declared |

The CAPTURE default adds `pickup_lead_ns`, not `min_submit_lead_ns`: the latter is derived
from `min_tx_delay` and is negative for CAPTURE (§7.1). The late policy follows the **media
mode**, not the source kind (§7.2, r3).

**The slot rule.** The slot is **the first N whose first-packet wire time `TVD(N) − VRX0·TRS`
is ≥ M + min_tx_delay**.

- L (= N − E⁻¹(M)) is reported, not configured, and TSDELAY is reported from it.
- Example, 1080p59.94, N sender, VRX0 = 8, all numbers recomputed (r3):
  - With L = 1 the first packet leaves at M + 17.29 ms. LIST reports an RTP offset of
    −1501.5 ticks (−1501/−1502 after its rounding). That is legal for a camera.
  - A **frame submitted when its readout ends** (M + TFRAME) meets the pick-up deadline
    of slot N+1 only if it is ready by M + 15.39 ms: 1.29 ms before the frame period ends,
    because the pick-up lead is 1.90 ms (§7.1).
  - The CAPTURE default `min_tx_delay` = 16.683 + 1.898 = 18.58 ms therefore gives
    **L = 2**. The first packet goes at M + 33.97 ms, and `min_submit_lead_ns` = −32.08 ms.
  - L = 1 needs progressive submission (GATEWAY) or a producer that finishes early; the
    app declares that with a smaller `min_tx_delay`.

**The slot-delay bound (r3)** `[C5 §5.11]`. Revision 2 said "PLAYBACK L ≤ 1, L ≥ 2
rejected: ST 2110-10 §7.6.3 ±TFRAME". That justification is dropped. Two reasons:

- §7.6.3 ("shall not exceed ±TFRAME from the most recent N × TFRAME time point") bounds
  the **RTP value** of a playback frame relative to the frame grid, as production intent
  `[R12 §2, line 104]`. It says nothing about transmit time relative to RTP. With
  `tsmode = SAMP`, any L satisfies §7.5.
- The bound was vacuous where stated: PLAYBACK with `min_tx_delay = 0` and on-grid media
  always has L = 0. It was absent where it matters: CAPTURE.

The real constraints are these:

- **JT-NM Tested / EBU LIST windows** `[R12 §3.4]`:
  - video RTP offset `[−1, ceil(TRO·90000) + 1]` ticks ([−1, 59] at 1080p59.94);
  - video latency (first packet − RTP time) `[0, 1 ms]`;
  - audio latency ≥ 0 and ≤ 1 ms (2020) or ≤ 20 × ptime (2022).

  Both video windows say "unless justified", and a CAPTURE stream with L ≥ 1 is the
  justified case. `get_info` says so (`JTNM_DEFAULT_WINDOW_EXCEEDED`).
- **The receiver's link offset** (ST 2110-10 §7.8, `D_LO ≥ D_TX + D_NET + …`, `[R12 §9]`).

Hence a new cross-essence field, `link_offset_budget_ns` (`mtl_timing_config`; 0 = none).
It is the downstream receivers' declared link-offset budget. It derives `max_slot_delay` =
the largest L whose last packet still lands within the budget after M: for video,
`L·TFRAME + TRO + RACTIVE·TFRAME` plus the declared network allowance ≤ budget.

- A unit whose derived L exceeds it raises a `timing_warning` in `mtl_session_get_status`
  (reason `LINK_OFFSET_BUDGET`, with the shortfall). It is **never rejected**, because the budget belongs to the
  receiver, not to the standard.
- `get_info` reports the granted L, TSDELAY and `max_slot_delay`.

### 5.2 Derived launch per essence

| Essence | First packet | Per packet |
|---|---|---|
| video | `scheduled_first = TVD(N) − VRX0·TRS`, `TVD = N·TFRAME + TROFFSET` | `TPRj` per the sender type's read schedule: gapped for N; **linear for NL and W** (ST 2110-21:2022 §7.1.4, Q-TIME-17); interlaced/PsF second field `+ TFRAME/2 + TLINE/2` (possible now that TX and RTP are decoupled) |
| cvideo (ST 2110-22) (r3) | `scheduled_first = TVD(N)`: the VRX model does not apply (ST 2110-22 §5.3 note 1), so VRX0 = 0 — as today (`st_tx_video_session.c:582-585`) | Network Compatibility Model only; rate per `rate_mode` (below) |
| audio | `launch(p) = M(first sample of p) + D_a` | `D_a` (`mtl_audio_config.audio_launch_offset_ns`): PLAYBACK default `clamp(granted pacing profile's max early error + margin, 0, ptime/2)` — tens of µs on RL/TSN, so JT-NM's 1 ms limit at 1 ms ptime holds; CAPTURE uses `min_tx_delay` (≥ ptime); reported as TSDELAY `[C2 #6]` |
| anc | the ST 2110-40 window of frame **N = E⁻¹(M) + L_anc** (L_anc per §5.5, r3): `[TFST + TEPO + TD − TFRAME, TFST + TEPO + TD]`, `TFST = N·TFRAME (+TROFFSET_ANC − TRODEFAULT)`; second field TSST = TFST + TFRAME/2 + TLINE/2 | TD = 1 ms (CTM) or `8/(FrameRate × TotalLines)` (LLTM, 118.6 µs at 1080p59.94); a deterministic target inside the window (`anc_target_delay_ns`)¹ ² |
| fastmeta (r3) | `M + D_fmd` (`mtl_fastmeta_config.fastmeta_target_delay_ns`); 0 = PLAYBACK: the audio `D_a` rule (pacing early error + margin, ≤ 1 ms); CAPTURE: `min_tx_delay`; as a group member with a video member, it inherits L_v like ANC (§5.5) | ST 2110-41 §7 Network Compatibility Model; keep-alive every 450 ms (§7.3) |

¹ TEPO needs the SDI raster, so the ANC config carries the associated video format
(`total_lines`, TX only, §4.7). `total_lines` = 0 takes the SDI raster of the group's
video member at group start: 1125 for 1080 lines, 750 for 720, 2250 for 2160, 625 for
576i, 525 for 480i/486i. Outside a group it means 1125, the 1080-line raster. `get_info`
reports the value used.

² The target follows the contract's Appendix D. Today ANC packets are spread over the whole
frame (`st_tx_ancillary_session.c:1108`) `[R05 F25]`.

**cvideo rate modes (r3)** `[C5 §5.12]`. ST 2110-22 §4 and §5.2 require "a constant number
of bytes per frame" and "a constant number of RTP packets per frame", padding allowed.
VBR is not a -22 mode `[R12 §6]`. Hence:

| `mtl_cvideo_config.rate_mode` | Wire | Submission |
|---|---|---|
| `MTL_CVIDEO_RATE_CBR` (0, **default**) | every frame (interlaced: every **field**) sends the same NPACKETS at a constant TRS; shorter codestreams are padded (RFC 9134 padding) | `codestream_bytes` is the per-unit **ceiling**, including the box header (below); the granted size rounds up to whole packets |
| `MTL_CVIDEO_RATE_VBR_MAX` (opt-in, legacy-compatible) | today's behaviour: NPACKETS varies per frame (`:2481-2485`), and TRS is taken from the ceiling (today it is recomputed per frame, `:750-757`) | as CBR; flagged **non-compliant** (`MTL_INFO_NON_COMPLIANT_2110_22`) in `get_info` and in the SDP values `get_info` returns |

- **The box header counts.** `codestream_bytes` includes the jpvs/jpvi box header that MTL
  prepends; today `frame_size = codestream_size + st22_box_hdr_length`
  (`st_tx_video_session.c:2481`). `get_info` reports `box_hdr_bytes` and the headroom
  (granted − requested).
- **An oversize codestream fails synchronously at submit** with `-MTL_ENOSPC`, reason
  `CODESTREAM_OVERSIZE`, when `valid_bytes > granted − box_hdr_bytes`, and the lease stays
  with the app. It is never a
  deferred drop. Today the check is at build time and is reported only through
  `notify_frame_done` (`:2467-2475`).
- The FFmpeg muxer keeps working unchanged. It already accepts any `pkt->size ≤ frame_size`
  (`ecosystem/ffmpeg_plugin/mtl_st22p_tx.c:266-286`), with `frame_size` from the `bpp`
  option (`:98`). Under CBR, `codestream_bytes = frame_size` has exactly that acceptance
  rule; only the wire gains padding.
- The designer's earlier pick (VBR_MAX as default) is reversed by the standard. An encoder
  that is not byte-exact is handled by the ceiling and the padding, not by VBR.

Invariant for every essence: **no packet of a unit leaves before the unit's media time**
(T7). A wide sender's VRX pre-fill is capped so that `scheduled_first ≥ M`; today MTL caps
W pre-fill at 0.8·TRO/TRS `[R12 §3.5]`.

**Where each knob lives (r3, A8).**

- **`mtl_timing_config`** (cross-essence): `source_kind`, `media_mode`, `late_policy`,
  `late_tolerance_ns`, `underrun_policy`, `snap_mode`, `snap_tolerance_ns`,
  `off_grid_policy`, `horizon_ns`, `tsmode`, `rtp_mode`, `min_tx_delay_ns`,
  `media_index_offset`, `media_time_offset_ns`, `link_offset_budget_ns`.
  - RX fields: `link_offset_ns`, `rx_flush_offset_ns`, `rx_skew_budget_ns`,
    `rx_rtp_offset`, `mediaclk_mode`, `rx_incomplete`.
- **Media configs** (essence-specific):
  - video: `troffset_ns`, `sender_type`, `progressive_late`;
  - cvideo: `rate_mode`, `codestream_bytes`;
  - audio: `samples_per_packet`, `audio_launch_offset_ns`, `audio_absorb_samples`;
  - anc: `anc_timing_model`, `anc_target_delay_ns`, `anc_window_anchor`, `total_lines`;
  - fastmeta: `rate`, `interlaced`, `fastmeta_target_delay_ns`.

Every zero value is in the 09 defaults table.

### 5.3 Launch overrides (expert)

| Mode | Meaning | Compliance |
|---|---|---|
| `NOT_BEFORE t` | derive as usual, but use the first slot whose **first-packet wire time** is ≥ t | ST 2110-21 compliant |
| `EXACT t` | first packet leaves at t; the rest follow the read schedule from there | not ST 2110-21 in general (TROFFSET not constant): reported as `ON_TIME` with `MTL_TX_TIMING_NON_COMPLIANT` when on time `[R12 §10.2]` |

An invalid exact request fails explicitly and never falls back to another pacing mode. An
invalid request is one in the past or closer than the minimum lead (reason
`LAUNCH_IN_PAST`), or beyond the horizon (`BEYOND_HORIZON`). It fails with `-MTL_ERANGE` at
submit when that can be known then, otherwise with a result per the late policy.

- Today it silently falls back to default pacing (`st_tx_video_session.c:1796-1805`)
  `[R05 F3]`.
- An exact ST40 request outside the LLTM/CTM window is rejected (the contract's
  Appendix D).

### 5.4 RTP passthrough (r3)

Time-preserving processors (ST 2110-10 §7.9) keep the input's RTP. In revision 3 the
**recommended** way is *derived* RTP from the input's media time: the processor recipe in
§10.8 (TAI + CAPTURE + `min_tx_delay`). It gives a fixed L, and a derived RTP equal to the
input RTP whenever the input was compliant `[C5 §5.5]`. `rtp_mode = MTL_RTP_PASSTHROUGH`
(`tsmode = PRES`) remains for non-compliant inputs that must be copied verbatim.

- **`PASSTHROUGH` + `AUTO` is forbidden** (`-MTL_EINVAL`, reason
  `PASSTHROUGH_AUTO`). Under AUTO, launch is "whatever slot the unit gets", and
  RX-then-forward jitter makes L alternate between two values. LIST then sees the RTP
  offset toggle, for example between ≈ −3003 and −4504 ticks at 1080p59.94.
  PASSTHROUGH therefore needs INDEX or TAI, and the slot comes from that media time.
- `rtp_override` is accepted only on PASSTHROUGH sessions. Such sessions cannot join a
  sync group. MTL validates "regular increments" and reports violations.
- **Audio passthrough.** The override applies to the **first sample** of the submission,
  and later packets are `+S` each. The input's first sample must lie on the session's
  output packet grid: `(override − grid RTP) mod S = 0`. Otherwise the submission is
  treated as a DISCONTINUITY and re-phased (§8), which is reported.
- **ANC** has no passthrough of its own. It uses the same TAI media time as its video
  (§10.8), so ANC RTP = video RTP (ST 2110-40 §5.4).
- `rtp_timestamp_delta_us` is replaced by `media_time_offset_ns` (§4.6, ns precision) for
  the compliance-test trim. In effect it was a static µs RTP offset, a non-zero RTP clock
  offset forbidden by ST 2110-10 §7.3. Lip-sync trims use `media_index_offset` (§10.5)
  (Q-TIME-5).

### 5.5 Live ANC and fastmeta when video has L ≥ 1 (r3, Q-TIME-18)

`[C5 §5.15, C2 #1]`. This was open standards question Q-TIME-18, and the GStreamer
`mtl_st40p_tx` author was blocked on it. The plugin stamps ANC with
`GST_BUFFER_PTS + pts_for_pacing_offset` as TAI (`gst_mtl_st40p_tx.c:719-720`). The
decision rule (re-read from the ST 2110-40:2023 primary text, `[R12 §5.1]`):

> **ANC for video unit k carries `RTP_anc(k) = RTP_v(k)` and is transmitted inside the
> ST 2110-40 CTM/LLTM window of the frame in which video unit k is actually transmitted:
> N = E⁻¹(M(k)) + L_v**, where L_v is the group video member's granted slot delay.
> Standalone ANC applies the same rule with its own L (its `source_kind` and
> `min_tx_delay`; PLAYBACK gives L = 0).

Why this reading is the correct one:

1. **-40 §6.3 builds the window on the video transmission timing.** The clause opens with
   "ST 2110-21 defines the latest allowed moment of transmission of the first video packet
   of each video frame … as TPR0 or TVD", and defines `TAD = (N × TFRAME) +
   TROFFSET_ANC`, which mirrors -21's `TVD = N × TFRAME + TROFFSET`. -40 §6.1 imports only
   TFRAME, TLINE, TROFFSET and TRODEFAULT from -21; N is not defined in -40. In -21, N
   indexes the **transmitted** frame on the epoch grid. Nothing in -40 ties N to the ANC
   RTP.
2. **§6.1 bounds the time between an ANC packet appearing in "an input SDI signal" and its
   transmission.** In an L-frame-delayed chain, the SDI frame that carries unit k's ANC is
   the delayed one, N = k + L_v.
3. **The association is by RTP.** §5.4 requires the ANC RTP to be "contemporaneous with
   the related field or frame of the video signal", and the §6.3 NOTE says "RTP Timestamp
   values are used to time-align the contents of ANC data RTP streams and other streams".
   The window is a transmission-regularity model; the RTP carries the association. Both
   hold under this rule. Revision 2's options (a) and (c) forbid live ANC with L ≥ 1, and
   option (b) flags a correct stream as non-compliant.
4. **It is the only feasible rule for live sources.** ANC for a captured frame, such as
   camera timecode or captions derived from the picture, exists no earlier than the frame
   itself. A window anchored on the RTP frame closes ≈ 15.7 ms (CTM) before a CAPTURE
   frame with L = 1 can leave.
5. **Receivers get what they need.** Video and ANC for frame k arrive in the same frame
   period, so an SDI-reconstructing receiver composes them without extra ANC buffering. An
   analyser that computes N from the ANC RTP reports the same ≈ −L·TFRAME offset for ANC as
   for video, which is the "unless justified" case JT-NM already allows for CAPTURE video.

Consequences:

- `mtl_anc_config.anc_window_anchor`:
  - `MTL_ANC_WINDOW_AUTO` (0): the video member's transmit frame in a group with a video
    member, and the unit's own frame + own L otherwise;
  - `MTL_ANC_WINDOW_MEDIA` (strict): N = E⁻¹(M), that is L = 0, for users who read N as
    the RTP frame. Live units then usually arrive too late, and are
    `DROPPED/TOO_LATE`.
- The ANC **submit deadline** for unit k is the window end of frame N minus the ANC pick-up
  lead. With L_v = 1, a live ANC producer has one more frame period than with L_v = 0; the
  hint (§7.6) reports it.
- The group checks that an ANC member's rate and raster equal its video member's (§10.3),
  and the ANC member takes L_v from it at start. L_v is constant, because `min_tx_delay`
  is constant and snapping keeps M on the grid. A dropped video unit does not move the ANC
  window: ANC k still goes out in frame k + L_v, and so does the empty keep-alive packet
  (§7.3).
- `get_info` reports `anc_window_frame_offset` (= L_anc) and the resulting TSDELAY.
- Fastmeta members of a group follow the same rule (§5.2).
- For the GStreamer `st40p` sink this means the timing is TAI + CAPTURE (or PLAYBACK) on
  the same timeline as the video sink, with no pacing offset of its own (§10.8).

## 6. Pacing engine selection

Pacing is chosen per port at `mtl_init` today, and it is silently downgraded in five places
`[R05 F22]`:

- RL training fails;
- ST22;
- a shared TX queue;
- ST30 RL with ptime ≥ 2 ms;
- a runtime RL failure flips the whole port.

In the new API:

- a session *requests* a pacing class with REQUIRE/PREFER:
  - `MTL_PACING_HW_RATE` (RL);
  - `MTL_PACING_HW_LAUNCH` (TSN launch time, E830);
  - `MTL_PACING_SW` (TSC; today's `TSC_NARROW` and `PTP` ways are SW variants with their
    own accuracy profiles);
  - `MTL_PACING_BEST_EFFORT` (today's `BE`: frame start only, no ST 2110-21 claim);
  - `MTL_PACING_ANY`;
- create reports the granted class and its accuracy profile (the contract's advertised
  profile per NIC × pacing combination);
- a runtime downgrade is an EQ `PACING_CHANGED` event on every affected session plus a
  state getter.

Whether sessions may request a pacing class different from their port's is Q-TIME-7.

## 7. Admission, lateness and the per-unit result

### 7.1 Where the decision happens (r3)

The engine makes the timing decision for a unit when it picks the unit up. That point is
well before the unit's first packet. For video, it is about one ring depth (≤ 512 packets)
before the previous frame ends `[R05 §7.1]`.

```text
submit ────▶ QUEUED ────────────▶ pick-up (decision) ───────▶ scheduled_first ──────▶ last packet
   │                                    │                         = TVD − VRX0·TRS (video)
   │   deadline = scheduled_first − pickup_lead_ns                            │
   │   min_submit_lead_ns = M − deadline          (for a unit on its derived slot)
   └── synchronous checks (lease, layout, horizon, exact-in-the-past, cvideo size)
       margin_ns = deadline − submitted;   pickup_slack_ns = deadline − pickup_time
```

Two leads are reported in `mtl_session_get_info` and by the dry-run query
`mtl_<media>_session_query` `[C5 §5.4]`:

- **`pickup_lead_ns`** is how long before `scheduled_first` the engine decides. It covers
  the ring depth × TRS, the conversion stage if any, and one packet for audio, because of
  the carry (§8).
- **`min_submit_lead_ns` = M − deadline** (r3) is the same lead measured from **media
  time**. It is `pickup_lead_ns − (scheduled_first − M)`, which is constant for on-grid
  media because L and φ are constant.
  - It is the one number a producer needs: **submit unit k before `M(k) −
    min_submit_lead_ns`**.
  - For a framework it is the element's minimum **latency**: GStreamer's LATENCY query
    minimum, FFmpeg's `-muxdelay` (§10.8).
  - It is negative for CAPTURE: the unit may be submitted after its media instant.

Revision 2 defined `min_submit_lead_ns` as the lead before `scheduled_first` but used it as
"M ≥ now + lead", which differed by ≈ 608 µs for PLAYBACK and by `min_tx_delay` for
CAPTURE `[C5 §5.15]`. The redefinition makes the text that 10 and §10.3 already use exact.

**Worked number (r3), 1080p59.94 PLAYBACK**, N sender, 4320 packets, ring 512 packets,
TRS 3.7074 µs:

| Quantity | Value |
|---|---|
| `pickup_lead_ns` | 512 × TRS = 1.898 ms |
| `scheduled_first − M` = TRO − VRX0·TRS | 604–619 µs (VRX0 9…5; 608.0 µs at VRX0 = 8) |
| `min_submit_lead_ns` | **1.279–1.294 ms** |
| deadline | ≈ **M − 1.29 ms** |

A just-in-time producer that hands frame k over *at* M(k) — a GStreamer sink with
`sync=TRUE` rendering at running time, or `ffmpeg -re` — is 1.29 ms late on every frame
under INDEX or TAI, and gets `DROPPED/TOO_LATE`. C5's scenario is confirmed. The answer is
`media_time_offset_ns` ≥ `min_submit_lead_ns` + a scheduling margin, declared as the
element's latency (§4.6, §10.8). It is not a slide.

### 7.2 Late policy (r3)

| Policy | A unit whose pick-up deadline passed… | RTP / grid | Result |
|---|---|---|---|
| `MTL_LATE_DROP` (default for **INDEX and TAI** media) | is not sent | its RTP slot stays empty (regular increments kept) | `DROPPED`, reason `TOO_LATE`, margins |
| `MTL_LATE_SEND_LATE` (**bounded**) | is sent paced at TRS from its actual start, only if its last packet completes before the next occupied slot's `TVD − VRX0·TRS` and within `late_tolerance_ns` (default `min(TROFFSET, TFRAME − RACTIVE·TFRAME)`); otherwise dropped | RTP unchanged | `LATE` with observed times, or `DROPPED/WOULD_OVERLAP` |
| `MTL_LATE_RESLOT` (default for **AUTO** media) | gets the next free slot and that slot's media time | stays regular | `ON_TIME` with `slots_skipped_before = n` (the underrun is reported, not the unit) |

**Precedence (r3)** `[C5 §5.15, §3.7]`: **the media mode wins.** AUTO → RESLOT for every
source kind; INDEX/TAI → DROP for every source kind. The plain generator (AUTO + PLAYBACK)
therefore gets RESLOT. A CAPTURE session also raises `TIMING_INFEASIBLE` on its first late
unit.

- With completion NONE, `slots_skipped_before` is still counted in the stats
  (`tx.slots_skipped`), so a slow renderer is visible without results.
- RESLOT is valid only when the app did not assign a media time. INDEX/TAI sessions reject
  `late_policy = RESLOT` with `-MTL_EINVAL`.

Unbounded "send as soon as possible" would cascade `[C2 #4]`:

- a 1080p59.94 frame occupies ≈ 16 ms of wire time, so a frame released even 1 ms late
  overlaps the next slot, and every following unit becomes late — the permanent slide that
  T5 forbids;
- MTL's own RX would drop it anyway (`st_rx_video_session.c:1176-1192`).

`RESLOT` is today's default for sessions without user timing (jump to the current epoch,
`stat_epoch_drop`) `[R05 §3.1]`.

### 7.3 Underrun policy (no unit for a slot)

| Policy | Wire | Default for |
|---|---|---|
| `MTL_UNDERRUN_SKIP` | nothing for the slot; counter `tx.slots_empty`; the next result carries `slots_skipped_before` | video, cvideo, audio |
| `MTL_UNDERRUN_EMPTY_ANC` | the empty ANC packet (`ANC_Count = 0`, marker set) at the start of the last VANC line, `RTP = RTP_v(k)`, F bits from parity, inside the window of §5.5 — ST 2110-40 §5.5 requires one RTP packet per field/frame/segment | **anc** |
| `MTL_UNDERRUN_KEEPALIVE` | an empty packet if nothing was sent for 450 ms — ST 2110-41 requires one every 500 ms | **fastmeta** |
| `MTL_UNDERRUN_SILENCE` | zero payload with regular RTP | audio option |
| `MTL_UNDERRUN_REPEAT_LAST` | the previous unit's payload with the new slot's media time/RTP | later capability (Q-TIME-4) |

Today MTL sends nothing when the app has no ANC frame (`st_tx_ancillary_session.c:942-946`),
which breaks the ST 2110-40 keep-alive rule `[C2 #8]`.

### 7.4 Early, too-far-ahead and preroll (r3)

`[C5 §5.2]`:

- **A unit is never sent early.** It waits for its slot.
- **Horizon.** `horizon_ns` (0 = 1 s; configurable, because deep-buffer playout needs more,
  Q-TIME-3) is measured from **`max(now, S)`**, where S is the session's resolved start
  instant (§3.1).
  - A RUNNING or ARMED-after-resolution submission whose media time is beyond it is
    rejected at submit with `-MTL_ERANGE`, reason `BEYOND_HORIZON`.
  - Today such a unit is honoured by the builder and then sent immediately by the
    transmitter with an error log (`st_video_transmitter.c:444-461`) `[R05 F4]`.
- **Preroll is a duration.** `mtl_start_params.preroll_ns` (session and group start) is
  extra lead added to the T0 resolution (§3.1). The returned instant is computable from
  documented inputs: `now`, the members' `min_submit_lead_ns`, `preroll_ns`, k and G.
  Because the horizon is measured from S, a long preroll does not make every unit "too far
  ahead": preroll depth is bounded by the pool and the horizon from S, not by 1 s from now.
- **Units queued before resolution** (submitted in CREATED or ARMED, before the timeline
  resolved) are validated when start resolves S:
  - units with M < S are `FLUSHED/BEFORE_START`;
  - if any unit has M > S + `horizon_ns`, **start fails atomically** with `-MTL_ERANGE`,
    reason `BEYOND_HORIZON`, and nothing starts. The session and every group member stay
    where they were, with their queues intact. The app withdraws units, calls
    `mtl_session_discard_queued`, or raises `horizon_ns` through `reconfigure` in STOPPED.
  - C5's scenario: 2 s of 48 kHz audio (indices 0…95999) pre-submitted with the default
    1 s horizon. Revision 2 had no outcome. Revision 3 fails the start with
    `BEYOND_HORIZON`, because indices > 48000 lie beyond S + 1 s. `horizon_ns ≥ 2 s` makes
    it start.

### 7.5 TX result timing fields (r3, A2)

```c
struct mtl_tx_timing {                /* inside mtl_tx_result (07 §1.1); every *_tai_ns is TAI or invalid per hdr.time_valid */
  int64_t  media_tai_ns;              /* M after offsets and snapping                     MTL_TT_MEDIA */
  int64_t  media_index;               /* on the session's timeline (after media_index_offset) */
  int64_t  submitted_tai_ns;          /*                                                  MTL_TT_SUBMITTED */
  int64_t  deadline_tai_ns;           /* latest pick-up that meets the slot                MTL_TT_DEADLINE */
  int64_t  scheduled_first_tai_ns;    /* TVD − VRX0·TRS (or the ANC/audio/fastmeta target) MTL_TT_SCHEDULED */
  int64_t  enqueued_first_tai_ns;     /* SW: TSC at the burst that carried packet 0 — enqueue, not wire time */
  int64_t  observed_first_tai_ns[MTL_MAX_LEGS];   /* HW TX timestamp per leg (2022-7)     MTL_TT_OBSERVED_FIRST(leg) */
  int64_t  observed_last_tai_ns;
  int64_t  margin_ns;                 /* deadline − submitted: positive = submitted early enough */
  int64_t  pickup_slack_ns;           /* deadline − pickup */
  int32_t  snap_error_ns;             /* TAI mode: media_tai_ns (+offset) − snapped M */
  int32_t  max_packet_lateness_ns;    /* worst packet vs its TPRj (TX self-check, §12) */
  uint32_t rtp;                       /* RTP on the wire (first packet) */
  uint32_t flags;                     /* NON_COMPLIANT, SNAPPED, RELOCKED, DISCONTINUITY, REPEATED, RESLOTTED */
  uint32_t slots_skipped_before;
  uint32_t samples_padded;            /* audio: silence from carry or gap fill (§8) */
  uint32_t samples_dropped;           /* audio: overlap removed (§8) */
  uint32_t samples_inserted;          /* audio: silence a re-phase added (§8) */
};                                    /* 120 B; the MTL_TT_* valid bits are in the CQ header */
```

`margin_ns` is Vulkan's `presentMargin` idea from the submitter's view: it answers "how
close was I?", not just "was I late?" `[R11 §5.2]`. Under RL pacing, SW observation at
`tx_burst` precedes the wire by the NIC ring occupancy, so it is reported as
`enqueued_first`; only HW TX timestamps populate `observed_*` `[C1 #18]` (Q-TIME-10). The
header is normative for the layout (A1, A2).

### 7.6 Slot hint at acquire (r3)

```c
struct mtl_slot_hint {
  uint32_t struct_size;
  uint32_t queued;                   /* units already queued ahead */
  int64_t  next_media_index;         /* the admission test, §7.1: the smallest k > last submitted
                                        with deadline(k) = M(k) − min_submit_lead_ns ≥ now */
  int64_t  next_media_tai_ns;        /* M(next_media_index) */
  int64_t  submit_deadline_tai_ns;   /* deadline(next_media_index): submit before this */
};
```

There is **one formula** for AUTO, joining sessions and INDEX producers: the admission test
itself `[C5 §5.15]`. For AUTO it is the slot the unit would get; for INDEX it is the first
index a joiner can still make.

`mtl_tx_acquire` fills the hint. It is OpenXR's `predictedDisplayTime` and CoreAudio's
`inOutputTime`: the producer learns which slot it is filling *before* it renders
`[R11 §5.4, §6.4]`. That removes the fixed phase lock a downstream engine measured (≈ 27 ms
wait) `[R08 §3.3]`.

## 8. Audio (r3)

- **Packetisation from absolute sample indices.** `samples_per_packet` S is an integer in
  the config. ptime = S/Fs is derived as a rational, never taken from an enum `[C2 #21]`.
  (Today `ST31_PTIME_80US` sends 4 samples every 80 µs, a 50 kHz RTP rate at 48 kHz: side
  finding SF-35.) Packet p covers samples `[p·S, (p+1)·S)` since T0, or since the last
  re-phasing DISCONTINUITY (below).
- **Carry-over.** A submission whose end is not on a packet boundary leaves a partial
  packet. Examples: AAC 1024 = 21 × 48 + 16; per-video-frame audio of 800/801 samples at
  59.94; 1601/1602 at 29.97.
  - At pick-up MTL copies that tail into a per-session carry buffer. The tail is < S ×
    channels × sample size, ≤ 1152 B at Level A.
  - Submission n therefore completes at its last whole packet, and its buffer returns
    promptly.
  - The straddling packet is attributed to submission n+1, and uses n+1's deadline.
  - On a stop, a drop or a late n+1, the carry is zero-padded to a full packet and sent on
    time (`samples_padded`) `[C2 #7]`.
  - The audio `pickup_lead_ns` is therefore one packet earlier than the first packet's
    launch.
- **Where a submission lands.** Let e be the *expected* first sample, the end of the
  previous submission, and s the given one: `media_index`, or
  `round(media_tai_ns × Fs)` in TAI mode after snapping to the sample grid.
  - **Absorb** (`mtl_audio_config.audio_absorb_samples`; 0 = default by source kind and
    media mode: **S (one packet) for CAPTURE and for TAI mode, 0 for PLAYBACK and GATEWAY
    in INDEX mode**; AUTO is contiguous by construction; the `MTL_AUDIO_NO_ABSORB` flag in
    `audio_flags` forces 0) `[C5 §5.8]`. If `|s − e| ≤ absorb`, the submission is contiguous: it is
    placed at e by shifting the carry, and `s − e` accumulates into the session's
    `audio_drift_samples` gauge. That covers `alsasrc` PTS jitter and the
    `round(PTS × 48000)` off-by-one, without a click and without an error.
  - **Forward gap** (`s > e + absorb`) (r3) `[C5 §5.14]`. The **packet grid is kept.**
    - Silence fills `[e, s)` inside the packets that contain e and s.
    - Packets wholly inside the gap are not sent. The receiver sees an RTP gap of a
      multiple of S, which is regular.
    - The new data starts at its exact sample s. No sample is dropped, and
      `samples_padded` reports the silence.
    - Every dropped video frame's audio (800 mod 48 = 32 at 59.94) therefore leaves
      packet boundaries, and "packet p covers `[p·S, (p+1)·S)`", intact. AES67 receivers
      see no packet-phase change.
  - **Overlap** (`s < e − absorb`). The samples `[s, e)` of the new submission were
    already sent or queued. They are removed (`samples_dropped`), and the rest continues
    at e. Nothing is re-phased.
- **Explicit `MTL_SUBMIT_DISCONTINUITY`** is the only thing that re-phases (and a
  passthrough override off the output grid, which is treated as one, §5.4). The carry is
  zero-padded, and those zero samples are reported as `samples_inserted`, apart from the
  ordinary carry and gap-fill padding in `samples_padded`. If the new index d is not a
  multiple of S, the session's packet grid is re-anchored: packet p covers `[d + p·S,
  d + (p+1)·S)`. RTP stays `floor(M × Fs)` of each packet's first sample. While RUNNING,
  only forward (§4.4).
- **Sample-accurate start.** A session that starts at instant S sends its first packet at
  sample index s0, the smallest multiple of `samples_per_packet` whose media time
  `M(s0) ≥ S`. Samples before s0 are `FLUSHED/BEFORE_START`. §15's
  `st_timeline_audio_first` uses the same rule.
- **Convenience.** `mtl_tx_write(s, data, bytes, &sub, timeout)` accepts an arbitrary byte
  run for audio, anc and fastmeta, which copy into packets anyway (the `FI_INJECT`
  analogue). It takes the submission's `media_index`, `sample_count` or `anc`, and it
  acquires, splits and submits internally. Frameworks no longer re-frame audio by hand
  `[R08 B11, C3 P3-3]`.
- Per-packet RTP is computed from the sample index (§4.5).

## 9. Progressive (line) submission

Line operation is readiness inside one timed unit, not another buffer mode (review §12).
It is chosen at create (`unit = MTL_UNIT_ROWS`, reported as granted); the submit flag only
asserts it `[C3 P4-2]`.

```text
acquire → fill rows [0,64) → submit(media k, ready_rows = 64)
        → fill → mtl_tx_publish(s, lease, ready = 128)
        → …   → mtl_tx_publish(s, lease, ready = height)      /* ready == unit rows is FINAL */
        → terminal result
```

- `ready = n` publishes the immutable prefix `[0, n)` with release semantics, and the
  engine reads only published rows (acquire semantics).
  - For planar formats a row is ready only when every plane's row is written.
  - Interlaced: rows count field lines.
- The unit's slot and RTP are fixed at submit from its media time. If the engine reaches a
  packet whose rows are not published by that packet's deadline, `progressive_late`
  applies (Q-TIME-13):
  - `TRUNCATE` (default): stop sending the unit; the result reports `rows_late`;
  - `PAD`: send the rest from a black/previous-line source;
  - `STALL`: today's slice behaviour; it breaks pacing, so legacy only.
- `mtl_session_row_deadline(s, &hint, row, &t)` (L4) returns the latest publish time for
  rows `[0, row)`, using the same exact math as the engine `[C3 P4-3]`.
- This maps onto today's ST20 slice mode (`query_frame_lines_ready`,
  `st_tx_video_session.c:2021`). That is a session *type* that `st20p` does not offer, so
  line mode needs a session-layer adapter for ST20 (Phase 6).
- A Rivermax "chunk" of N lines is one `publish` step *for timing*. Unlike a Rivermax
  chunk, it is a row counter over one frame buffer, not separate memory with app-written
  RTP headers `[R10 §12]`.
- RX: a progressive RX session delivers the unit at the first slice. `mtl_rx_dequeue`
  returns it with `ready_rows`, then `MTL_CQE_RX_PROGRESS` entries follow, the last one
  `FINAL` with the terminal status. `mtl_rx_wait_progress` is a convenience over them.

## 10. Multi-essence synchronisation

### 10.1 Mechanism

```text
                   timeline TL:  T0 = n × 1001/6000 s   (resolved at group start, on the common grid)
                     /                     |                        \
   video session (59.94p)          audio session (48 kHz)       ANC session
   unit k → M = T0 + k·1001/60000   sample s → M = T0 + s/48000   unit k → same M as video k
   RTP_v = 15015n + floor(1501.5·k)  RTP_a(pkt p) = 8008n + 48p  RTP_anc = RTP_v
   launch: slot 10n + k (+L),        launch: M(first sample)      launch: ANC window of
   TROFFSET, read schedule           + D_a                         frame 10n + k + L_v (§5.5)
                     \                     |                        /
                      group G: validate all → resolve T0 → arm all → run
```

15015n = T0·90000, 8008n = T0·48000 and 10n = T0/TFRAME are integers because T0 is on the
1001/6000 s grid. Review C2 checked this exactly, and revision 3 re-checked it for any k
with the fixed formula (§3.1), for n = 0, 1 and 10727972028 (near today's TAI):

- every `RTP_v` for k = 0…39 matches;
- every `RTP_a` for p = 0…99 matches;
- `floor(1501.5·k)` runs 0, 1501, 3003, 4504, 6006, 7507, ….

### 10.2 The worked answer to the maintainer's question

A file contains 59.94p video (`time_base = 1001/60000`), 48 kHz audio
(`time_base = 1/48000`) and caption ANC, all starting at pts 0.

```c
mtl_timeline_h tl;
struct mtl_timeline_config tc;  mtl_timeline_config_init(&tc);   /* anchor_mode 0 = AT_START */
mtl_timeline_create(mt, &tc, &tl);
/* video, audio, anc: .timing.timeline = tl, .timing.media_mode = MTL_MEDIA_INDEX (source_kind 0 = PLAYBACK) */
mtl_group_h g;  mtl_group_create(mt, tl, NULL, &g);
mtl_group_add(g, video);  mtl_group_add(g, audio);  mtl_group_add(g, anc);
struct mtl_start_params sp;  mtl_start_params_init(&sp);
sp.mode = MTL_START_AT_MEDIA_INDEX;  sp.media_index = 0;         /* preroll_ns 0 */
struct mtl_time t0;
if (mtl_group_start(g, &sp, &t0) < 0) { /* mtl_last_error() says which member and why */ }

/* demux loop — timeouts last */
video frame, pts p      → submit(video, .media_index = p, .user_cookie = p)
ANC for video frame p   → submit(anc,   .media_index = p, .anc = pkts, .anc_count = n)   /* before or with video p */
audio packet, pts q, n  → submit(audio, .media_index = q, .sample_count = n)
```

Result:

- RTP for video frame *k*, its ANC, and the audio sample at the same instant are exact
  functions of `T0 + k·1001/60000`: no half-frame snap, no TR_OFFSET contamination, no
  floating point. That holds for any start index k (§3.1, r3).
- Each essence transmits on its own model:
  - video in its slot with TROFFSET and the read schedule;
  - audio at `M + D_a`;
  - ANC inside its window, ≈ 0.3–1.3 ms after the frame epoch for PLAYBACK. ANC for frame
    k must therefore be submitted before or with video frame k.

  Frames with no ANC still get the empty keep-alive packet (§7.3).
- If video frame 37 is late, it is dropped (DROP), or sent late only if it still fits
  (bounded SEND_LATE). Audio is unaffected, and video frame 38 keeps its exact media time.
  Nothing slides.
- Today the same app gets up to half a video frame plus TR_OFFSET of RTP-derived A/V error
  with USER_PACING, and ST30P cannot even set a user timestamp `[R05 §6]`. The Phase 0.5
  helper (§15) closes that on the legacy API first.

### 10.3 Group start

`mtl_group_start` does four things:

1. validates every member, as session start would;
2. resolves T0 on the timeline with the §3.1 formula, including `preroll_ns`;
3. checks the §7.4 horizon rule for every queued unit;
4. arms every member, and reports the start instant S (DeckLink playback groups, AJA
   `STARTING_AT_TIME` `[R11 §1.6, §2.1]`).

If any member cannot be armed, none is.

- TX members use media mode INDEX or TAI. AUTO stamps whatever slot a unit gets and cannot
  be synchronised, so `mtl_group_add` returns `-MTL_EINVAL` for an AUTO session.
- Validation also checks two things for ANC and grid fastmeta members: their rate and
  raster equal their video member's, and they take the video's L (§5.5) and offset (§10.6).

**Without a group.** Framework plugins own one session each: separate FFmpeg muxers,
separate GStreamer sinks. **In one process**, they open the same *named* timeline and start
independently:

- the **first starter** resolves T0 (typically `AT_MEDIA_INDEX 0`, lazily as in §3.1);
- **later sessions join** with `MTL_START_NOW` on the resolved timeline. Their first unit
  index is `hint.next_media_index` from `mtl_tx_acquire` (§7.6): the first index they can
  still make;
- `MTL_START_AT_MEDIA_INDEX k` on a resolved timeline whose M(k) has passed, or is closer
  than the lead (§3.1), returns `-MTL_ERANGE`, reason `START_IN_PAST`.

No group object is needed for alignment, only for atomic start. Separate **processes** are
§10.7.

### 10.4 Lip-sync trims

A deliberate A/V offset ("production intent", ST 2110-10 §7.7.3) is an integer
`media_index_offset` on one session, never an RTP offset. ANC and fastmeta members inherit
their video's offset through the timeline (§10.6), so captions stay with the picture.

### 10.5 Late handling inside a group

Late handling is per unit and per session, as in §7.2. The group never re-anchors on its
own; the only exception is the timeline's clock-step policy (§2.4). A member that enters
ERROR emits `GROUP_MEMBER_FAILED`, and the others continue (Q-TIME-8, answered (a); the
restart is in §10.6).

### 10.6 Group edge cases (r3)

`[C5 §5.10]`:

- **No video member.** In an audio-only, ANC-only or fastmeta-only group, `AT_MEDIA_INDEX
  k` counts units of the **first member in add order**. The §3.1 formula uses that
  member's period.
- **Offset inheritance is a timeline property.** The timeline keeps a `frame_offset`,
  readable with `mtl_timeline_get_info`.
  - It is set by the first start of a video member on the timeline: that member's
    `media_index_offset`, the "offset owner".
  - It is updated when that member is re-based (`MTL_DISCARD_REBASE`, §10.8).
  - An ANC or fastmeta session on the timeline whose rate equals the owner's, and whose own
    `media_index_offset` is 0, inherits it at start or join. When such a session is
    re-based itself, it adopts the timeline's current value instead of computing its own.
  - This holds with or without a group, so a caption inserter that starts mid-programme on
    a named timeline gets the video's offset without copying it.
  - Audio never inherits a frame offset, because a frame offset is not an integer number
    of samples at 1001 rates (800.8/frame). Audio trims are its own `media_index_offset`
    in samples.
- **`mtl_group_add` on a RUNNING group** is legal for a CREATED or STOPPED session. The
  session joins with START_NOW semantics: its first index is the next feasible index
  (§7.6), and it takes the inherited offset and L_v (§5.5). The group stays RUNNING.
- **Partial-failure restart.** One member enters ERROR, the group reports
  `MTL_GROUP_PARTIAL_FAILURE`, and the others keep running. The app then:
  1. calls `mtl_session_stop(member)` (ERROR → STOPPED);
  2. fixes the cause (`update_flows`, `reconfigure`, or a port that came back);
  3. calls `mtl_session_start(member, START_NOW)`. That is legal for a member of a group in
     RUNNING or PARTIAL_FAILURE (03 §5), as the one exception to "members start only
     through the group". The member rejoins like an add on RUNNING, with the inherited
     offset.

  The group returns to RUNNING when every member is RUNNING. It is never stopped for one
  member.
- **RX groups** are §11.4.

### 10.7 Separate processes (r3)

`[C5 §5.6]`. A named timeline lives in one instance (§3.1), so the common deployment of
one FFmpeg process per essence, on two VFs, cannot share one. There are two ways:

| Way | Setup in each process | Index for a PTS |
|---|---|---|
| **EPOCH + INDEX** (recommended) | sessions on the epoch timeline (the default), `media_mode = INDEX`; the processes agree on a start instant `t_start` out of band (a command-line argument, NMOS activation time) | `k = k0 + pts_units`, with `k0 = mtl_rational_index_at(t_start, unit_rate)`: the first unit at or after `t_start` since the SMPTE epoch — frames for video, samples for audio |
| **AT_TAI** | `mtl_timeline_create` with `anchor_mode = AT_TAI`, the same `tai_ns` and `MTL_TIMELINE_GRID_EXPLICIT` with the same grid (for example 1001/6000 s) in every process | `k = pts_units` on that timeline; `mtl_timeline_index_at(tl, s, tai_ns, &k)` for a join |

Why this is exact across processes:

- `mtl_rational_index_at` is `ceil(tai_ns × unit_rate / 1e9)`, computed exactly. It needs
  no instance, so a process with only an audio session computes the same instant as one
  with only video.
- On the epoch, `M(k0_video) = k0_video · 1001/60000` and `M(k0_audio) = k0_audio /
  48000` are both the first unit instants at or after `t_start`. If `t_start` is on the
  1001/6000 s grid, they are equal, and every RTP matches the single-process group exactly.
  Otherwise the audio start is within one sample of the video start.
- The helper rounds `t_start` to the grid for the app: pass `t_start = ceil(t/G)·G`.
- AT_TAI without an explicit grid would snap by each process's own members (§3.1), and two
  processes with different essences would disagree. That is why the grid is mandatory for
  this recipe.

The pts → k mapping is the math the app was told not to reimplement, so both helpers are
L4 exports on the same exact code (§3.2).

### 10.8 Recipes (r3)

**Live framework sinks: TAI + NEAREST + a declared latency** `[C5 §5.4, §5.8]`. INDEX is
for whole-timeline owners: file playout, and a single process that owns every essence.

- **GStreamer sink.**
  - Let `ct = base_time + running_time(PTS) + latency` be the buffer's presentation clock
    time, where `latency` is the pipeline latency that basesink synchronises with.
  - Map `ct` to TAI:
    - a `GstPtpClock` is already on the PTP timescale;
    - the default `GstSystemClock` is MONOTONIC: use
      `mtl_time_convert(MONOTONIC → TAI)`;
    - any other clock (audio ring buffer, net client): build a map from
      `mtl_time_cross_timestamp` samples taken around `gst_clock_get_time` (§2.1).
  - Submit with `media_tai_ns = TAI(ct)`.
  - Set basesink `render-delay = D`, with `D ≥ min_submit_lead_ns + margin`. The margin
    covers the pipeline's scheduling jitter (1 ms is a safe start), and
    `min_submit_lead_ns` comes from the dry-run query. With `sync=TRUE`, basesink then
    hands each buffer over D before its presentation time, and adds D to the latency it
    reports. MTL paces to the slot.
  - Without basesink sync (`sync=FALSE`, or any producer that submits at running time),
    use `media_tai_ns = TAI(base_time + running_time)` and `media_time_offset_ns = D`,
    with D ≥ upstream latency + `min_submit_lead_ns` + margin. Report D as the element's
    latency.
  - Seeks are harmless: after a flushing seek, basesink's running time restarts but the
    clock-time mapping stays monotonic, so TAI never repeats (below).
  - For `mtl_st40p_tx`, replace today's `PTS + pts_for_pacing_offset` with the same TAI
    as the video sink (§5.5).
- **FFmpeg muxer, live (`-re`).** Set `media_tai_ns = TAI(start_time_realtime) +
  rescale(pts)` and `media_time_offset_ns = -muxdelay` (≥ `min_submit_lead_ns` + margin).
  - An FFmpeg run with `CLOCK_TAI` but no ptp4l gets time source SYSTEM_TAI, labelled
    ESTIMATED, with a `timing_warning` (reason `TIME_ESTIMATED`, 09 §8).
- **What NEAREST gives these sinks.**
  - A free-running v4l2src/decklinksrc camera, or a 30.000 fps source on a 29.97 session,
    is adapted by occasional `DUPLICATE_SLOT` drops or gaps, and never locks out (§4.3).
  - `alsasrc` jitter is absorbed (§8).
- **OBS output.** Same recipe, with `mtl_time_convert(MONOTONIC → TAI)` of the OBS
  timestamp. Today's `tfmt = MEDIA_CLK` with a monotonic value is a bug (side finding).

**Time-preserving processor (RX → TX)** `[C5 §5.5]`:

- set `media_mode = TAI` with `media_tai_ns = u.timing.media_tai_ns` (the RX unit's media
  time, §11);
- set `source_kind = CAPTURE` and `min_tx_delay_ns` = the pipeline budget (RX completion
  at ≈ M + 16.7 ms for 1080p59.94, plus processing, plus margin);
- keep snap NEAREST.

What this gives:

- **L is fixed.** The slot is computed from M, not from arrival jitter. A unit that
  overruns the budget is `DROPPED/TOO_LATE`; L never toggles.
- **The derived RTP equals the input RTP** whenever the input was on the grid. The RX media
  time is exact to less than one tick, and NEAREST snaps it back to the same `N·TFRAME`.
  An off-grid input, such as a camera with phase φ, is re-stamped to the grid. To keep φ,
  use LOCKED_PHASE, or PASSTHROUGH with INDEX/TAI (§5.4).
- **Audio.** Use the same recipe with the RX audio unit's first-sample media time. The
  output packetisation stays on its own grid (§8), and each packet's RTP is `floor(M ×
  Fs)` of its first sample. The result equals the input RTP for equal S, and is correct
  for any S.
- **ANC.** Use the video unit's media time, which gives ANC RTP = video RTP.

**Seeks with an INDEX sink** `[C5 §5.9]`:

- TAI is recommended for any sink whose PTS can restart, as above.
- For INDEX sinks, call `mtl_session_discard_queued(s, &p)` with `p.flags =
  MTL_DISCARD_REBASE` and `p.first_index = new_pts_index` (A6; `struct
  mtl_discard_params` carries `flags` and `first_index`; the verb is 03 §3.4):
  - the session stays RUNNING;
  - queued units become `FLUSHED/DISCARD`;
  - the session's `media_index_offset` is set so that `first_index` maps to the **next
    feasible slot** on the shared timeline (§7.6);
  - the next submission is an implicit forward DISCONTINUITY.
- The app keeps submitting PTS-derived indices. Results report timeline indices, and
  `get_info().media_index_offset` reports the new offset.
- For a group or a named timeline, re-base the video member first, then its ANC/fastmeta
  members, which adopt the timeline's new offset (§10.6).
- Revision 2's stop plus `start(AT_MEDIA_INDEX 0)` failed with `-MTL_ERANGE`, because M(0)
  had passed. That remains the behaviour of start; `discard_queued` is the verb for seeks.

## 11. RX timing model (r3)

`[C5 §5.7, §4.9, §8.4]`. Revision 2 gave RX timing fields but no model. This section is the
model. Start and arm semantics (IGMP join at the first start, ARMED = joined and
discarding) are owned by [03 §3.5](03-object-model-and-lifecycle.md); the tasklet hook that
enforces the due time is [04 §4.4](04-threading-and-execution.md).

Two lifecycle tests use this section's media time (§11.2):

- **ARMED discard.** A session started `AT_TAI t` or `AT_MEDIA_INDEX k` discards every unit
  whose media time is < t and counts it in `units_before_start`. A SENDER stream has no
  valid media time, so its arrival time is compared instead, and the first delivered unit
  says so in `time_valid` (03 §3.5).
- **`mtl_session_update_flows` with `AT_TAI t`.** The new rule takes the units whose
  RTP-derived media time is ≥ t, and the old rule the units before them (03 §3.5, 09 §1.1).

### 11.1 Timeline binding

An RX session binds only an **EPOCH** timeline (the default) or an **AT_TAI** timeline. An
AT_TAI timeline has an explicit anchor, and may be shared with TX sessions in the same
instance.

- AT_START and NEXT_GRID are `-MTL_EINVAL` for RX (reason `RX_TIMELINE_LAZY`). A receiver
  has no lead and no start instant to resolve them with.
- A timeline makes `media_index` meaningful. Without one, i.e. with `mediaclk_mode =
  SENDER`, only `rtp` and the arrival times are valid.

### 11.2 Media time and media index

For every unit:

```text
rtp_u        = RTP unwrapped to 64 bits, nearest wrap to the arrival time (±6.63 h at 90 kHz),
               after subtracting rx_rtp_offset (SDP mediaclk:direct=<offset>)
media_tai_ns = floor(rtp_u × 1e9 / R)                        (the RTP instant, as today)
media_index  = ceil((rtp_u + 1 − T0·R) / (P·R)) − 1          = the largest k with floor(M(k)·R) ≤ rtp_u
media_phase  = rtp_u − floor(M(media_index)·R)               (ticks; 0 for a sender on the grid)
```

- **`media_index` is the exact inverse of the TX rule** `RTP = floor(M × R)` (§4.5). For a
  sender on the grid it returns exactly the sender's k, and a sender with a phase φ < P
  maps to the slot it falls in.
- The obvious `floor((media − T0)/P)` is **wrong by one** whenever the sender truncated a
  fractional tick. That is half the frames at 59.94p, every second field at 1080i59.94,
  and 3 of 4 frames at 23.976p. It was checked (r3 script) for 59.94p, 1080i59.94,
  23.976p, 119.88p, 48 kHz and floor-aligned 44.1 kHz, k = −50…4999:
  - the exact inverse had 0 mismatches;
  - `floor` had 2525 per 5050 units at 59.94p.
- For audio, P·R = 1, so `media_index` is the first sample's index
  (`rtp_u − floor(T0·Fs)`).
- ANC and fastmeta units carry their video's RTP, so their `media_index` is the video
  frame/field index.

### 11.3 Presentation and link offset

`presentation_tai_ns = media_tai_ns + link_offset_ns` (ST 2110-10 §7.8, `D_LO`).

- A session's `timing.link_offset_ns` sets it; a group's `link_offset_ns` overrides it
  (§11.4).
- A unit whose `delivered_tai_ns > presentation_tai_ns` carries
  `MTL_RXT_LATE_FOR_PRESENTATION` and is counted: the link offset is too small for this
  sender and network.
- `mtl_session_get_info` reports the *tolerated* 2022-7 path differential (from buffer
  and slot sizing). The default aims at class A (10 ms) where memory allows (cheap for
  audio/ANC), and a warning is reported when `link_offset_ns` is smaller than
  `rx_skew_budget_ns` `[C2 #24]` (Q-TIME-14).
- **Scheduled delivery** (holding units until `presentation`) is still later (NG7,
  Q-TIME-12). v1 reports the value, and apps schedule.

### 11.4 RX groups

RX sessions may form a group. A group is one direction: `mtl_group_config.direction =
MTL_DIR_RX`, and mixing directions is `-MTL_EINVAL`.

- **One `link_offset_ns` per group** (`mtl_group_config.link_offset_ns`). It is the
  presentation clock of every member. This is a configuration value, not scheduling, so it
  is not deferred behind NG7 (C5 §5.7; IPMX TR-10-1 §11.2 makes link offset a controllable
  attribute for exactly this).
- **`mtl_group_start(g_rx, {NOW | AT_TAI t | AT_MEDIA_INDEX k})`** arms every member's
  filter together:
  - flow rules and IGMP joins for all members are installed before any member is RUNNING;
  - every member discards units whose media time < t (`units_before_start`; arrival time
    for a SENDER stream, as above) and delivers units with media ≥ t;
  - all members therefore begin at one media instant, whichever sender's packets arrive
    first. `AT_MEDIA_INDEX k` means t = M(k) of the first member in add order.
- `mtl_group_add` for RX checks only that every member uses the group's timeline.

### 11.5 Duplicates, stale units, gaps and relock

- **A packet whose RTP equals the unit being assembled** merges into it. That covers 2022-7
  copies, and PsF segments, which share an RTP.
- **A packet for an already delivered or older unit is stale.** It is counted
  (`pkts_stale`, `units_stale`) and never delivered.
- **Forward gaps.** The next delivered unit carries `units_missing_before = n` (by index
  difference), the RX twin of TX `slots_skipped_before`. A forward jump of more than
  `horizon_ns` of media time is flagged `MTL_RXT_DISCONTINUITY` instead.
- **Relock.** A sender restart with a backward RTP produces stale units forever unless the
  receiver relocks.
  - The rule: after stale packets of **3 distinct, increasing RTP values in a row** (a new
    regular sequence behind the old one), the session relocks.
  - The next complete unit is delivered with `MTL_RXT_DISCONTINUITY`, `media_index` may go
    backwards, and the `RX_DISCONTINUITY` event carries the old and new RTP.
  - It replaces today's implicit per-packet rule (20 consecutive redundant errors on every
    port, `st_rx_video_session.c:1188-1196`, `st_header.h:81`) with a per-unit rule that
    is observable.
- **Seek** is not applicable on RX: the receiver follows the sender.

### 11.6 The receiver answer to the maintainer's A/V question

A monitor receives the 59.94p video, 48 kHz audio and ANC of one programme:

- all three RX sessions stay on the epoch timeline (the default);
- they are in one RX group with `link_offset_ns = D`;
- the group starts with `mtl_group_start(g, NOW)`.

Then:

- `video.media_index = k` is the frame number since the epoch, and the ANC unit for that
  frame has the same k. **ANC ↔ video is index equality.**
- `audio.media_index = s0` is the index of the audio unit's first sample. **The sample
  contemporaneous with frame k is `s = ceil(k × 48000 × 1001/60000) = ceil(800.8·k)`**,
  exact integer arithmetic on the epoch.
- `mtl_rx_align(&video_unit, &audio_unit, &sample_offset)` (L4) returns `s − s0`, the
  offset into that audio unit. The value is negative when the frame starts before the unit,
  and ≥ `sample_count` when it starts after.
  - It returns **0** when exact: both units have a valid `media_index`, and the video's
    `media_phase` is 0.
  - It returns **1** when approximate, derived from `media_tai_ns` (±1 sample).
  - It returns `-MTL_EINVAL` for SENDER streams.
- `presentation = media + D` for all three. A renderer that presents at presentation time
  is in sync, whatever each sender's L and each packet's arrival order.

This is the RX mirror of §10.2, and it needs no helper beyond `mtl_rx_align`.

### 11.7 Completion time: due time and force-complete

Today a unit completes only when it is full (`st_rx_video_session.c:1855`) or when a newer
timestamp evicts its slot (`rv_slot_by_tmstamp`, `:1214-1221`), and DMA completion is
asynchronous (`:1507-1527`). Revision 3 adds a due time. The 04 §4.4 RX hook checks it every
iteration, in Phase 1 `[C5 §4.9]`:

```text
due = arrival_first (earliest leg) + unit_period + rx_flush_offset_ns
      capped at presentation_tai_ns when a link offset is set (session or group)
rx_flush_offset_ns: 0 = derived — rx_skew_budget_ns (tolerated path differential) with 2 legs, 1 ms with one leg
```

- `unit_period` covers the unit's own packet spread:
  - video: the last packet ≈ first + RACTIVE·TFRAME, less than TFRAME;
  - audio: `rx_unit_samples`/Fs (09, ≈ 10 ms);
  - ANC and fastmeta: TFRAME.
- The due time is keyed on the **first packet's arrival**, not on media time. That works
  unchanged for CAPTURE senders with L ≥ 1 (arrival ≈ M + 17–34 ms) and for SENDER-clock
  streams, which have no usable media time.
- **At the due time**, or on a force-complete command (stop DRAIN, `discard_queued`), a
  RECEIVING unit completes as follows: with `rx_incomplete = DELIVER` it is READY with
  status INCOMPLETE and its missing ranges (`MTL_CQE_RX_MISSING`, A2); with DISCARD it is
  dropped and counted.
- A newer RTP still completes the older unit early (today's eviction). Whichever comes
  first wins.
- With **no due time** (no packets), the waker sleeps at most 1 ms (04 §5.2), and signal
  loss is reported after `rx_signal_timeout_ns` (0 = `max(4 × unit period, 20 ms)`) by the
  `RX_SIGNAL` event and `mtl_session_get_status().rx_signal`.

### 11.8 RX timing fields (r3, A2)

```c
struct mtl_rx_timing {                          /* inside mtl_rx_unit (07 §1.2); every *_tai_ns is TAI or invalid per hdr.time_valid */
  int64_t  media_tai_ns;                        /* §11.2                          MTL_RT_MEDIA */
  int64_t  media_index;                         /* §11.2; valid with a timeline   MTL_RT_INDEX */
  int64_t  arrival_first_tai_ns[MTL_MAX_LEGS];  /* per 2022-7 leg                 MTL_RT_ARRIVAL_FIRST(leg) */
  int64_t  arrival_last_tai_ns[MTL_MAX_LEGS];
  int64_t  presentation_tai_ns;                 /* media + link offset (§11.3) */
  int64_t  delivered_tai_ns;                    /* when the unit became READY */
  uint32_t rtp;                                 /* raw RTP of the unit */
  uint32_t flags;                               /* HW_ARRIVAL, DISCONTINUITY, LATE_FOR_PRESENTATION, INCOMPLETE_BY_DUE */
  int32_t  media_phase_ticks;                   /* §11.2 */
  uint32_t units_missing_before;                /* §11.5 */
};                                              /* 80 B; the MTL_RT_* valid bits are in the CQ header */
```

- `arrival_*` flags say HW or SW.
  - HW: `MTL_FLAG_ENABLE_HW_TIMESTAMP` on a port whose PMD offers the RX timestamp
    offload (`mt_dev.c:2422-2440`).
  - SW: processing time; today it is taken on port P regardless of the RX port
    (`mt_ptp.c:1635-1641`).
- Latency is `arrival_first − media` (`mtl_time_diff_ns`): sender offset plus network.
  JT-NM expects [0, 1 ms] for playback video. It is not stored.
- **RTP offset and clock mode.** ST 2110 mandates offset 0, but AES67 and TR-03-era gear may
  signal `a=mediaclk:direct=<offset>` ≠ 0. Asynchronous streams (`mediaclk:sender`) have no
  TAI relationship at all.
  - The RX config carries `rx_rtp_offset` (from SDP, default 0) and `mediaclk_mode`
    DIRECT | SENDER. With SENDER, only `rtp` and the arrival times are valid.
  - On a DIRECT stream, `|arrival − media| > 1 s` raises `RX_TIMEBASE_SUSPECT`
    `[C2 #11]`.

## 12. Broadcast extras

| Feature | Status |
|---|---|
| **TX ST 2110-21 self-check**: VRX/CINST computed from `enqueued_*`/`observed_*` times, exposed as counters (`tx.vrx_max`, `tx.cinst_max`, `tx.tpr_late_pkts`, windowed per 08) and per unit as `timing.max_packet_lateness_ns` vs TPRj (§7.5) | v1 once E4 exists; cheap |
| **Sender restart semantics**: SSRC kept or new per config; random initial sequence (RFC 3550); epoch-timeline RTP continuous across restarts | v1, documented |
| **Per-leg observed times** and a leg-skew gauge for 2022-7 senders | v1 (§7.5) |
| **Timecode** (ST 12-1 time address from media time via `currentUtcOffset`, local offset and daily jam from ST 2059-2 SM TLVs, drop-frame at 1001 rates; RP 188 / ST 12-2 insertion into ST40) | later, L4 helper (Q-TIME-20) |
| **1001 audio-cadence helper** (samples in frame k = `floor((k+1)·x) − floor(k·x)`, or a pinned ST 299 five-frame phase; Dolby E over -31 needs frame-aligned audio) | later, L4 helper (Q-TIME-20); on the epoch grid 59.94 gives 800, 801, 801, 801, 801 repeating (r3, recomputed) |

## 13. Mapping today's timing surface to the new model

| Today | New model |
|---|---|
| `*_FLAG_USER_TIMESTAMP` + `timestamp`/`tfmt` | session `media_mode = TAI` (or INDEX) |
| `*_FLAG_USER_PACING` (TAI, snapped to the nearest *epoch*) | `media_mode = TAI` with derived launch; the "send at t" reading is `launch.mode = NOT_BEFORE`. The contract instead snaps to the nearest *slot* (N·T + packet-0 offset); migrated apps' TX moves by TROFFSET − VRX0·TRS (≈ 604–619 µs at 1080p59.94 depending on VRX0; 608 µs at VRX0 = 8, r3) depending on which is kept (Q-TIME-21) |
| `*_FLAG_EXACT_USER_PACING` | `launch.mode = EXACT` (flagged non-compliant) |
| `ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH` | the default (always) |
| `rtp_timestamp_delta_us` | `media_time_offset_ns` (ns, §4.6, r3) for the compliance trim; `media_index_offset` for lip-sync; `rtp_mode = PASSTHROUGH` + override for verbatim copies |
| `*_FLAG_DROP_WHEN_LATE` (silently needs USER_PACING) | `late_policy = DROP` (the INDEX/TAI default; works in every mode) |
| `notify_frame_late(priv, epoch_skipped)` (3 units, wrong `priv` on one path) | per-unit result status + `margin_ns` + `slots_skipped_before`; counters |
| `ST_EVENT_VSYNC` | `mtl_slot_hint` at acquire; the `EPOCH_TICK` event via EQ subscription `MTL_EQ_SUB_EPOCH_TICK` (r3) |
| `st20_tx_get_pacing_params` | `mtl_session_get_info`: troffset, trs, vrx, sender type, `pickup_lead_ns`, `min_submit_lead_ns`, L, TSDELAY, granted pacing, `max_slot_delay` |
| `pacing` in `mtl_init_params` (per port, 7 ways) | port default + session request/grant with 4 classes + accuracy profiles (§6) |
| ST22 `codestream_size` (box header added by MTL) + per-frame packet count | `codestream_bytes` ceiling incl. box header; `rate_mode` CBR (default) or VBR_MAX (§5.2, r3) |
| ST41 `fps`, `interlaced`, `second_field` | `mtl_fastmeta_config.rate` (0 = video rate), `interlaced`; parity from the index (§4.8, r3) |
| RX `timestamp` (media clock) / `receive_timestamp` / `timestamp_first_pkt` | `rtp`, `media_tai_ns`, `media_index`, `arrival_first_tai_ns[leg]` (§11) |
| **Mixed legacy/unified hazard** (r3) | see the note below the table; 11 states it as a known hazard |

**Mixed legacy/unified hazard (r3).** A **legacy ST20** session with the default
TX-cursor RTP differs by +54.4…+55.7 ticks at 1080p59.94 (T2) from any epoch-based
session, unified or legacy ANC/audio. A **legacy ST40** session paired with a unified ST20
session agrees within ±1 tick: legacy ANC stamps its epoch
(`st_tx_ancillary_session.c:420-427`, `:462`), and only the rounding differs
(round-to-nearest vs floor, Q-TIME-15). C5 had the direction reversed.

## 14. Engine changes this requires (L0 plumbing)

Which of these changes legacy users get, and behind which opt-in flag, is the legacy-default
column of [14 §2](14-implementation-roadmap.md): bugfixes are on for legacy users,
wire-visible changes are off on the legacy API behind a new opt-in flag and on in the
unified API.

| # | Change | Where | Risk |
|---|---|---|---|
| E1 | Carry media time and launch separately into `tv_*`; RTP from media time only | `tv_sync_pacing`, `tv_update_rtp_time_stamp` (`st_tx_video_session.c:692`, `:762`) | medium: default video RTP is wire-visible, so legacy default per 14 §2 |
| E2 | Exact rational epoch math and floor for RTP (replacing double `frame_time` and round-to-nearest); the r3 T0 formula; shared with the Phase 0.5 helper (§15) | `st_fmt.c:943-993`, epoch helpers | low; unit-testable; ±1 tick on 1001 rates, so legacy may keep rounding (Q-TIME-15) |
| E3 | Admission at pick-up reports `DROPPED`/`LATE`/`slots_skipped_before` with margins; bounded SEND_LATE; source kinds and `min_tx_delay`; NEAREST with hysteresis, LOCKED_PHASE relock, `DUPLICATE_INDEX`, `discard_queued` REBASE (r3) | `calc_frame_count_since_epoch` (`:637-690`) | medium |
| E4 | Enqueue and (where available) HW observed times per unit and leg; TX self-check counters | transmitters | low |
| E5 | Linear read schedule for W/NL; TLINE/2 for the second field and PsF segment; pre-fill cap so `scheduled_first ≥ M` | `tv_init_pacing` | medium (wire change; behind sender type) |
| E6 | Audio: packet RTP from the sample index; integer `samples_per_packet`; launch offset; carry buffer; absorb, grid-keeping gap fill and overlap trim (r3) | `st_tx_audio_session.c` | medium |
| E7 | ANC: RTP from media time; deterministic target inside the CTM/LLTM window of frame E⁻¹(M) + L (r3); empty keep-alive packet on underrun; `anc_count`/`udw_count` checks | `st_tx_ancillary_session.c:942-946`, `:1108` | medium |
| E8 | RX: per-leg arrival times; exact `media_index` inverse; due time and force-complete (with 04 §4.4); stale/relock per unit; RTP offset and SENDER mode; unwrap helper shared with TX math (r3) | `st_rx_*` | medium (was "low" in r2; the due-time hook is new code on every RX tasklet) |
| E9 | Time: explicit source types, `CLOCK_TAI` validation, lock state that clears, step events, published time base for every source (04 §8) | `mt_ptp.c`, `dev/mt_dev.c:1597-1614` | medium |
| E10 | cvideo: CBR (constant NPACKETS and TRS with padding, default) and VBR_MAX (TRS from the ceiling); synchronous size check at submit (r3) | `st_tx_video_session.c:2467-2485`, `:750-757` | medium |
| E11 | Dynamic ST20/ST22 frame arrays; lift `ST20_FB_MAX_COUNT` / `ST22_FB_MAX_COUNT` = 8, Phase 2 (r3, 05 §11) | `include/st20_api.h:24`, `:29`; `st_tx_video_session.c:4073`, `st_rx_video_session.c:4266` | medium |
| E12 | Grow the ST40 RX meta array beyond `ST40_MAX_META = 20`, with E11 (r3, 05 §11) | `include/st40_api.h:308` | low |
| E13 | fastmeta: rate from the associated video or explicit, launch offset, index parity (r3) | `st_tx_fastmetadata_session.c:206-234` | low |

## 15. Phase 0.5: the legacy timeline helper `st_timeline_*` (r3)

`[C5 §1.5]`. Research R05 §6 shows that USER_TIMESTAMP + USER_PACING already gives exact RTP
on ST20, ST30 and ST40 *sessions*. What the legacy API lacks is the anchor math, which
RxTxApp does in 30 lines of `double` (`tests/tools/RxTxApp/src/rxtx_app.c:673-703`). Phase
0.5 ships that math as a public, pure helper over the legacy API, plus two pipeline flag
fixes. It is the same code as E2 and the unified L4 helpers (§3.2), so the two APIs cannot
disagree.

```c
/* include/st_timeline.h — legacy API, Phase 0.5. Pure functions: no instance, no allocation, no locks. */
struct st_timeline_member {
  uint32_t kind;                 /* ST_TL_VIDEO | ST_TL_AUDIO | ST_TL_ANC | ST_TL_FASTMETA */
  uint32_t interlaced;           /* video/anc/fastmeta: 1 = index counts fields */
  uint32_t rate_num, rate_den;   /* video/anc/fastmeta: FRAME rate; audio: Fs/1 */
  uint32_t samples_per_packet;   /* audio: S */
  uint32_t reserved;
};
struct st_timeline {             /* plain value, caller-owned; T0 = n × G, exact */
  uint64_t grid_num, grid_den;   /* G (§3.3) */
  int64_t  n;                    /* T0 / G; valid after st_timeline_anchor* */
  uint32_t member_count, flags;  /* flags: ST_TL_ANCHORED */
  struct st_timeline_member member[8];
};
struct st_timeline_unit {
  uint64_t media_ns;             /* floor(M × 1e9) */
  uint64_t legacy_ts_ns;         /* ceil(floor(M × R) × 1e9 / R): feed as `timestamp` with ST10_TIMESTAMP_FMT_TAI */
  uint32_t rtp;                  /* floor(M × R) mod 2^32: the exact RTP (for ST10_TIMESTAMP_FMT_MEDIA_CLK users) */
  uint32_t second_field;         /* interlaced: index parity */
  uint64_t epoch;                /* floor(M / P): the legacy "epoch" of the unit */
};
int st_timeline_init(struct st_timeline* tl, const struct st_timeline_member* m, uint32_t count);  /* computes G; -EINVAL above the 1 s cap */
int st_timeline_anchor(struct st_timeline* tl, uint64_t not_before_tai_ns, uint32_t k_member, int64_t k); /* §3.1: T0 = ceil((not_before − k·P)/G)·G */
int st_timeline_anchor_at(struct st_timeline* tl, uint64_t tai_ns);                                      /* AT_TAI: first grid instant ≥ tai_ns */
int st_timeline_unit(const struct st_timeline* tl, uint32_t member, int64_t index, struct st_timeline_unit* out);
int st_timeline_index_at(const struct st_timeline* tl, uint32_t member, uint64_t tai_ns, int64_t* index); /* first index with M ≥ tai_ns */
int st_timeline_audio_first(const struct st_timeline* tl, uint32_t member, uint64_t start_tai_ns, int64_t* sample); /* §8 start rule */
```

The contract:

- **Math.** Exact rationals with 128-bit intermediates; the §3.3 grid rules, including the
  1 s cap and floor-aligned excluded audio; the §3.1 T0 formula; the §4.5 RTP rules,
  including every field.
  - It is unit-tested against the 13 §7 oracle at every rate in the §3.3 table, and against
    the r3 script's cases.
  - `not_before_tai_ns` is the app's `now + lead + preroll`. The helper never reads a
    clock.
- **`legacy_ts_ns` makes today's round-to-nearest exact.** Legacy `USER_TIMESTAMP` with
  `tfmt = TAI` computes RTP as `st10_tai_to_media_clk(ts)`, which is an integer
  round-to-closest (`st_fmt.c:986-993`). For `ts = ceil(X·1e9/R)` with `X = floor(M·R)`,
  `ts·R/1e9 ∈ [X, X + R/1e9)`, which rounds to X for every R ≤ 96 kHz. Verified: 0
  mismatches in 800 000 random cases at 90 k, 48 k, 96 k and 44.1 k (r3 script).
  - The same value drives USER_PACING. It is within one tick of M, so "nearest epoch" is
    unit k's slot.
  - `rtp_timestamp_delta_us` must be 0.
- **Legacy recipe per essence.**
  - **ST20/ST20P, ST22/ST22P.** `USER_TIMESTAMP | USER_PACING`, and `timestamp =
    legacy_ts_ns` of unit k. Interlaced legacy `fps` is the field rate, so pass the frame
    rate and `interlaced = 1` to the helper.
  - **ST30 session.** `USER_TIMESTAMP | USER_PACING`, and a frame of n packets gets
    `legacy_ts_ns` of its first sample. Frames must hold whole packets, because the legacy
    API has no carry. Start at `st_timeline_audio_first`.
  - **ST30P.** Needs the new `ST30P_TX_FLAG_USER_TIMESTAMP` (Phase 0.5 fix).
  - **ST40/ST40P.** ANC member k uses video index k, so the RTP is the video's. ST40P
    must honour USER_TIMESTAMP without USER_PACING (Phase 0.5 fix; today it is silently
    ignored `[R05 §6]`).
- **Known legacy limits**, not fixed in Phase 0.5:
  - ST30 launches exactly at the RTP instant (zero margin, R12 Q11);
  - the legacy ST20 default RTP (TX cursor) must not be mixed in (§13 hazard): every
    session of the programme uses USER_TIMESTAMP;
  - the legacy late handling stays as it is.
- **Guarantee** (wording for 13). For identical members, anchor inputs and indices,
  `st_timeline_unit().rtp` equals the unified engine's wire RTP, and the unified
  `mtl_timeline_get_anchor` equals `n × G`.
- **Phase 0.5 exit.** The maintainer's A/V/ANC example (§10.2) runs on the legacy API in
  RxTxApp, and every RTP is verified by the oracle.
