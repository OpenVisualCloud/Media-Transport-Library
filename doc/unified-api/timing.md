# Timing: media time, launch, sync and clocks

| | |
|---|---|
| Status | Maintained. Design for review; nothing is implemented. The headers in [sketch/include/mtl/experimental/](sketch/include/mtl/experimental/) win over this text |
| Date | 2026-10-02 |
| Folded from | the revision-3 timing chapter 06 (primary); research notes 05, 06 §2 and 12; reviews C2 and R2 (verification §6); chapters 04 §8.1, 13 §3, §5, §7, 16 §6 and 17 §4; study S2 P5; K1 §5; I1 §5.2; side finding SF-35. Standards detail: [prior-art.md](prior-art.md) §2; today's code: [research.md](research.md) §7, §8; revisions and reviews: [history.md](history.md) §5, §6.2 |

This document is the timing contract of the unified API: what a time is, how a unit's media time and RTP timestamp are fixed, when its packets leave, how several essences stay in sync, and what a receiver can rely on. Names are those of `mtl.h`, `mtl_sync.h`, `mtl_options.h`, `mtl_observe.h` and `mtl_reasons.h`.
`sc.*` and essence members (`video.sender_type` = `sc.video.sender_type`) are typed config fields; other knobs are option keys (`tx.late_policy` = `MTL_OPT_LATE_POLICY`), absent meaning the default given here; `info.*`,
`time.*`, `tx.*` counters and `rx.*` counters are stats (`mtl_observe.h`). Three sentences carry the design:

> **RTP describes the content. Pacing chooses when packets leave. Receive time is transmit time plus the network.**

The maintainer's three questions on PR #1610 are answered here: two times per unit (media time and launch time, §4–§5); A/V from one file without half-frame error (§10); automatic sync of several streams (§7, §10).

## 1. Principles

| # | Principle | Standards anchor | Today in MTL |
|---|---|---|---|
| T1 | One epoch, 1970-01-01 00:00:00 TAI; RTP clock offset zero: `rtp = floor(t × rate) mod 2^32` | ST 2059-1 §6.1, ST 2110-10 §7.3 | default time source is `CLOCK_REALTIME` (UTC) labelled TAI (`dev/mt_dev.c:2288`) |
| T2 | The RTP timestamp is the unit's sampling (or intended) instant; for playback video `N × TFRAME` | ST 2110-10 §7.5, §7.6.3 | default video RTP is the packet-0 TX cursor `E(N) + TRO − VRX0·TRS`, +54.4…+55.7 ticks after `N × TFRAME` at 1080p59.94 (VRX0 9…5; note 1) |
| T3 | The transmit grid is on the PTP timescale: `TVD = N × TFRAME + TROFFSET`, TROFFSET constant | ST 2110-21 §6.2 | EXACT user pacing accepts any per-frame target |
| T4 | Exact arithmetic; truncate | ST 2059-1 §5.1, ST 2110-10 §7.6.1 | `double frame_time` (≈ +66 ns at today's TAI for 1001 rates) and round-to-nearest (`st_fmt.c:986-993`) |
| T5 | Regular increments win: a late unit is dropped (a gap), repeated, or explicitly re-anchored, never silently slid | ST 2110-10 §7.6.1, §7.7.1 | a late frame goes out in the current epoch with no counter |
| T6 | Essences of one programme agree by TAI instant, not by buffer boundaries | ST 2110-10 §7.1, ST 2110-40 §5.4 | default ST20 and ST40 RTP differ for the same frame |
| T7 | No packet of a unit leaves before the unit's media time | JT-NM Tested; ST 2110-21 VRX model | not guaranteed for wide senders with large pre-fill |

Note 1: start time `st_tx_video_session.c:63-70`, cursor `:724-730`, RTP from the cursor `:793`; VRX0 per pacing `:566-579`, VRX_FULL = 9 from `:3365`. 608 µs ≈ 54.7 ticks is the VRX0 = 8 point; TRO alone (57.39 ticks) is not the offset.

Decisions behind them: D-09 (media time and launch separate, one RTP rule), D-10 (default video media time is the frame epoch), D-11 (exact rationals). See [decisions.md](decisions.md); M3 and M7 are the open maintainer answers.

## 2. Clocks and the time base

### 2.1 What a time is

`mtl.h` rule R5: every time is `int64_t` ns since 1970-01-01 TAI on the **instance clock**, valid only when its flag says so.

- TAI while the time base is locked; flagged ESTIMATED when it is not (a free-running clock, the system clock). An RX value on a sender's own clock is flagged `MTL_UNITF_SENDER_TIME` (Phase 7, later).
- `mtl_time_now(mt, &tai_ns)` is DP and wait-free; it returns `MTL_TIMEF_VALID`, `MTL_TIMEF_ESTIMATED`, `MTL_TIMEF_HOLDOVER` as a mask (≥ 0).
- Zero is never a sentinel. Validity is a flag: `MTL_UNITF_INDEX_VALID`, `MTL_UNITF_TAI_VALID` on units; `MTL_TXR_MEDIA_VALID`, `MTL_TXR_MARGIN_VALID`, `MTL_TXR_SENT_VALID`, `MTL_TXR_SENT_HW`, `MTL_TXR_ESTIMATED` on results; `MTL_TXF_*` and `MTL_RXF_*` in the full records.
- There is no TSC clock in the API. TSC is an internal detail of the published time base and differs between sockets.

### 2.2 Time sources

`mtl_instance_params.time_source`, enum `mtl_time_source`:

| Source | What it reads | Lock state | Notes |
|---|---|---|---|
| `AUTO` (0) | a disciplined NIC PHC, else `CLOCK_TAI` (kernel TAI offset set), else `SYSTEM_TAI` (ESTIMATED) | from the source | never starts the built-in PTP client (D-85). AUTO chooses **once, at open**. Phase 7: `FREERUN` replaces `SYSTEM_TAI` as the last step, and AUTO is re-evaluated while running (§13) |
| `PTP_BUILTIN` | MTL's own PTP client | MTL's servo | only when named. On a PF MTL owns: that port's PHC. On a VF (no timesync): software mode as today (`ptp->no_timesync`, `mt_ptp.c:1390-1394`), MTL's own time base only, never the VF's PHC. `CLOCK_NOT_OWNED` only for a request to steer a PHC MTL does not own. Fix the sticky `locked` (`mt_ptp.c:540-552`) |
| `PHC` | the NIC PHC, disciplined by `ptp4l`/`phc2sys` | `time.phc_trust`, or the app via `mtl_time_set_reference` | how Rivermax works; read only, off the tasklet |
| `CLOCK_TAI` | the kernel TAI clock | from the daemon | **validated**: `CLOCK_TAI = CLOCK_REALTIME + adjtimex().tai`, and the kernel offset is 0 unless a daemon sets it. Rejected when the offset is 0 (otherwise it is UTC, and the 37 s error returns) |
| `SYSTEM_TAI` | `CLOCK_REALTIME` + UTC offset | none: state FREERUN, ESTIMATED | the offset from PTP announce `currentUtcOffset` when known, else configured; follows NTP steps; a leap second posts `TIME_STEP`; an NTP leap smear (±11.6 ppm for 24 h) is a rate error |
| `USER` | `mtl_time_user_update(mt, tai_ns, monotonic_ns)` from an app thread | app-declared | no per-read callback on the tasklet |
| `FREERUN` | seeded once from the system clock, never stepped | FREERUN (ESTIMATED) | IPMX internal clock without PTP: **Phase 7, later** (§13) |
| test clock | `mtl_test_clock(mt, base_tai_ns, rate)`, `mtl_test_clock_advance(mt, ns)` (`mtl_debug.h`) | locked | debug builds only; the null backend and unit tests (§16) |

MTL adjusts a clock only with `PTP_BUILTIN`: the PHC of a port it owns (a PF), or, on a VF, its own software time base; never `CLOCK_REALTIME`, never a clock a node daemon disciplines. The built-in phc2sys (`time.phc2sys`) steers the host's `CLOCK_REALTIME`; it is never used in a pod and restores the frequency at close. Instance time options: `time.ptp_domain`, `time.ptp_pi`, `time.ptp_unicast`,
`time.ptp_source_tsc`, `time.ptp_pi_kp`, `time.ptp_pi_ki`, `time.ptp_announce_timeout`, `time.phc_trust`, `time.fallback`, `time.freerun_slew_ppm` (keys 2200–2230).

### 2.3 Time state, events and the reference

- `enum mtl_time_state`: `FREERUN`, `ACQUIRING`, `LOCKED`, `HOLDOVER`, `LOST`. LOCKED exists only for `PTP_BUILTIN`, `PHC`, `CLOCK_TAI` and `USER`; FREERUN is the state of the `FREERUN` and `SYSTEM_TAI` sources, and of AUTO after a fall back to free run.
- Events (per session, `mtl_queue.h`): `MTL_EVENT_TIME_STATE` (port, state, offset), `MTL_EVENT_TIME_STEP` (step ns), `MTL_EVENT_GRANDMASTER` (new and old ID). Stats, port scope: `time.source`, `time.state`, `time.offset_ns`, `time.path_delay_ns`, `time.last_sync_age_ns`, `time.grandmaster_id`, `time.utc_offset_s`, `time.step_count`, `time.disciplined_by`, `time.error_ns`, `time.gm_*`.
- Health: `mtl_health.time_state` and `time_error_ns`; `MTL_HEALTH_TIME_UNLOCKED` is set for ACQUIRING and LOST only, so FREERUN and HOLDOVER count as ready.
- `mtl_time_set_reference(mt, port, &ref)`: what the instance cannot see when a node daemon disciplines its clock (grandmaster ID, domain, traceable, clock class and accuracy, `locked`). The app reads it with `pmc` or from the PTP operator. With `time.phc_trust` detect, `locked = 0` is the only way MTL learns the node lost its grandmaster, so the call is needed from Phases 1–2 (pods),
  not only for IPMX in Phase 7.

### 2.4 The published time base

Tasklets never read a clock. Every source feeds one seqlocked record per instance and **per CPU socket**, refreshed off the tasklet (PTP servo, admin thread, or the app for `USER`):

```text
{ seq, tsc_base, tai_base_ns, ratio (TAI ns per TSC tick, 32.32 fixed point),
  monotonic_base_ns, realtime_base_ns, state, accuracy_ns }          one writer
reader (wait-free):  tai = tai_base_ns + (tsc − tsc_base) × ratio
```

| Property | Rule |
|---|---|
| Refresh | at least every 100 ms, never on a tasklet |
| Ratio | least-squares servo over the last N (default 16) PHC/TSC cross-timestamps, each a PHC read bracketed by two TSC reads, rejected when the bracket is too wide. Today `impl->tsc_hz` is a one-shot calibration; 1 ppm is 1 µs per second |
| Publication | **slewed, never stepped**: a new record starts on the previous line at the switch instant, with a ratio that meets the new estimate within one refresh period. The only steps are declared ones (`TIME_STEP`, §12) |
| Per socket | TSC is not guaranteed synchronised across sockets; each record is built from cross-timestamps taken on that socket |
| Two NICs | each leg's PHC offset is compensated into the one instance timescale; media time and every leg's launch use it. Today pacing reads port P for every leg (`st_tx_video_session.c:695`, `st_tx_audio_session.c:265-266`) |

The timing rules rely on two properties of the record: every tasklet on every socket computes the same TAI for the same instant, within the servo error; and a refresh never moves a scheduled launch backwards by more than the slew bound.

This removes from the hot path the user `ptp_get_time_fn` callback, the `rte_eth_timesync_read_time` PMD reads, and `clock_gettime` on `/dev/ptpN` (no vDSO: a syscall plus a PCIe read). The accuracy against a direct PHC read is not assumed: spike S7 measures it, and its bound becomes a performance gate (§16).

### 2.5 Clock conversion

- `mtl_time_convert(mt, in_ns, from_clock, to_clock, &out_ns)` maps `MTL_CLOCK_TAI`, `MTL_CLOCK_MONOTONIC`, `MTL_CLOCK_REALTIME` through the cross-timestamps of the record; exact at the record's instant, extrapolated on its ratio between refreshes.
- `mtl_time_cross(mt, &tai, &mono, &real)` returns one coherent triple. A framework with its own clock (GStreamer audio ring buffer, `GstNetClientClock`, an app media clock) samples that clock just before and after the call and fits `X ↔ TAI` over a few samples.
- `mtl_media_ticks(tai_ns, clock_rate)` = `floor(M × rate) mod 2^32`; `mtl_media_tai(near_tai_ns, ticks, clock_rate)` unwraps ticks to the instant nearest `near_tai_ns`. Both AS and exact.

### 2.6 Time in a pod

In a pod, `AUTO` reads the VF's PHC (read only) when a node daemon disciplines it, else `CLOCK_TAI` when the kernel TAI offset is set, else `SYSTEM_TAI`, ESTIMATED (`FREERUN` from Phase 7); it chooses once at open. `PHC`, `CLOCK_TAI` and `USER` are read only. `PTP_BUILTIN` on a VF runs MTL's client in software mode, as today, and disciplines only MTL's own time base, never the VF's PHC;
RxTxApp `--ptp` and the `ptp` group of the nightly pytest run this way on VFs. "Disciplined" needs a
signal the pod can see, `time.phc_trust`: absent or detect = the PHC agrees with `CLOCK_TAI` within a bound at start (the bound is a spike; agreement cannot tell a locked PHC from a node in holdover); `YES` = trust the node; `NO` = never use the PHC. No source needs `CAP_SYS_TIME` or write access to `/dev/ptp*`. Details: [deployment.md](deployment.md).

## 3. Timelines and T0

### 3.1 The timeline object

A timeline maps a unit index of any stream to an exact TAI instant:

```text
M(k) = T0 + k × P          P = the session's index period (exact rational seconds)
```

`mtl_sync.h`:

```c
struct mtl_timeline_config { struct_size; anchor; step_policy; reserved0; int64_t tai_ns; struct mtl_rational grid; char name[64]; reserved[4]; };  /* 136 B */
int mtl_timeline_create(mt, &cfg, &tl);  /* CP; name "" = private, else open-or-create, refcounted */
int mtl_timeline_close(tl);              /* CP; sessions hold their own reference */
int mtl_timeline_get_info(tl, &info, size);  /* DP; null = the epoch */
```

| `anchor` | T0 is fixed | Use |
|---|---|---|
| `MTL_ANCHOR_AT_START` (0) | lazily, at the first start on the timeline, by the formula of §3.3; never before `tai_ns` (a lower bound, 0 = none) | file playout: index 0 is the first unit and never in the past; NMOS-scheduled starts with a lower bound |
| `MTL_ANCHOR_AT_TAI` | at create: `tai_ns` rounded up to the grid G. With an explicit `grid`, every process that passes the same `tai_ns` and grid gets the same T0 | externally agreed anchors; RX; separate processes (§10.4) |
| `MTL_ANCHOR_EPOCH` | T0 = 0, the SMPTE epoch | the **null timeline handle**: every session that names none. A video index is the frame number since 1970, an audio index the sample number |

- `mtl_timeline_info`: `flags` (`MTL_TIMELINE_RESOLVED` once T0 is known), `sessions`, `t0_tai_ns`, `t0_s` (exact rational), `grid`, `index_offset` (the whole-unit trim the timeline keeps, §10.3).
- Names are per instance, ≤ 63 bytes, a namespace separate from session names. A second create with the same name and another config is `-MTL_EEXIST`. A named timeline is never shared across processes (§10.4 covers that).
- `grid` = `{0, 0}`: the common grid of the sessions' index periods (§3.4). **Open:** for AT_TAI that grid is not known at create, so either T0 resolves at the first start or AT_TAI requires an explicit grid (see decisions.md §5).

### 3.2 Index periods

| Essence | Unit | `media_index` counts | P |
|---|---|---|---|
| video, cvideo progressive | frame | frames | TFRAME |
| video, cvideo interlaced | field | fields; even = first field | TFIELD = TFRAME/2 |
| video PsF (`MTL_PSF`) | frame (two segments) | frames | TFRAME |
| audio | the samples of a submission | its **first sample** | 1/Fs |
| ANC, grid fastmeta | the ANC (or item group) for one video frame or field | the associated video index | the video's |
| free-running fastmeta | one unit | own units | 1/`fastmeta.video.fps` |
| generic RTP | unit of `rtp.unit` | units | 1/unit rate |

### 3.3 The start formula

A start at media index k (`MTL_AT_INDEX k`, or `MTL_NOW`, which is k = 0 on an unresolved timeline) resolves:

```text
A  = now + lead + preroll_ns            lead = max over the started sessions of max(0, min_submit_lead_ns)   (§6.1)
T0 = ceil( max(A − k·P_k, B) / G ) · G  P_k = index period of k's stream; B = tai_ns of AT_START (else −∞)
S  = T0 + k·P_k                         the resolved start instant
```

- **T0 is on the grid by construction**, so `T0·R` is an integer for every grid member and T0 is a whole number of frames for every video, ANC and grid fastmeta member. The rejected formula `ceil(A/G)·G − k·P` is on the grid only when `k·P` is a multiple of G: at 59.94p only for k ≡ 0 (mod 10); k = 3, 7 or 13 gave a half-integer `T0·90000` and a fractional `T0·48000` (.6 or .4).
- **k's stream** is the first video session in the start array, else `s[0]`. `mtl_session_start(..., &t0_tai_ns)` returns T0; `mtl_timeline_get_info` returns it exactly.
- **Interlaced members contribute TFRAME to G**, so T0 is always a first-field instant: even indices are first fields and parity never flips. An odd k starts on a second field.
- Checked with exact rationals for k = 0…100 and 20 random `now`/lead/preroll per configuration (59.94p + 48 kHz + ANC; 50p + 48 kHz + ANC; 29.97p + 48 + 96 kHz; 1080i59.94 + 48 kHz + ANC; 1080i50 + 48 kHz + ANC; 59.94p + 29.97p + 48 kHz; audio only; 59.94p + 48 + 44.1 kHz): the formula gave on-grid T0 and `S ≥ A` in 2020 of 2020 cases; the rejected one put
  T0 off the grid in 1800 of 2020 at 59.94p and in 1000 of 2020 at 1080i50, where an odd k flips field parity. This is G-68.

### 3.4 The common grid

G is the smallest period on which every video, ANC and grid fastmeta stream starts on an integer **frame** (interlaced: frame, not field), the 90 kHz clock ticks an integer number of times, and every audio rate ticks an integer number of times, **except audio rates that would push G above
the 1 s cap**. Excluded audio rates are floor-aligned (`floor(T0 × Fs)`) and their sub-sample phase is reported (< 1 sample, 22.7 µs at 44.1 kHz). Free-running fastmeta is excluded. If the video/ANC/90 kHz part alone exceeds 1 s, the start fails with `-MTL_EINVAL` and a reason.

| Members | G | Frames per G | Note |
|---|---|---|---|
| 25p or 50p (+ 48/96 kHz) | 1/25 s, 1/50 s | 1 | 3600 / 1800 ticks; 1920 / 960 samples at 48 kHz. 1080i50 + 48 kHz: 1/25 s |
| 24p (+ 48 kHz) | 1/24 s | 1 | 3750 ticks; 2000 samples |
| 30p, 60p (+ 48 kHz) | 1/30 s, 1/60 s | 1 | 3000 / 1500 ticks; 1600 / 800 samples |
| 24p + 30p | 1/6 s | 4 / 5 | 15000 ticks |
| 29.97, 59.94, 23.976, 119.88 (+ 48/96 kHz); 1080i59.94 | **1001/6000 s = 166.83 ms** | 5, 10, 4, 20 | 15015 ticks; 8008 samples (16016 at 96 kHz) |
| 1001 family + 44.1 kHz only | 44.1 kHz would need 1001/300 s = 3.337 s: excluded; G = 1001/30000 s (29.97/59.94/119.88p), 1001/6000 s (23.976p) | 1, 2, 4 (23.976p: 4) | 3003 ticks (15015 at 23.976p); 735.735 samples per frame at 59.94 |
| 1001 family + 48 + 44.1 kHz | 1001/6000 s (set by 48 kHz); 44.1 kHz excluded | 5, 10, 4, 20 | 7357.35 samples per G at 44.1 kHz |
| 24p + 44.1 kHz | 1/12 s | 2 | 7500 ticks; 3675 samples |
| audio only (48 kHz) | 1/6000 s | — | k counts samples |
| 25p + 59.94p | 1001/25 s = 40.04 s: above the cap, `-MTL_EINVAL` | — | |
| 50p + 59.94p | 1001/50 s = 20.02 s: above the cap | — | |

Per-frame numbers at 1001 rates: 59.94 = 1501.5 ticks, 800.8 samples; 29.97 = 3003 ticks, 1601.6 samples; 23.976 = 3753.75 ticks, 2002 samples; 119.88 = 750.75 ticks, 400.4 samples. Ticks and samples are both integers after 10 (59.94), 5 (29.97), 4 (23.976) and 20 (119.88) frames, which is the G row above. Other exact values: at 1080p59.94 the read of an L = 0 unit ends at ≈ M + 16.654 ms;
TROFFSET − VRX0·TRS = 604.31 µs at VRX0 = 9 (the low end of 604–619 µs, §6.1); the late-tolerance default is min(637.67, 667.33) µs = 637.67 µs (§6.2); TAI ns passes 2^61 in 2043.1 and 2^63 in 2262.3 (§3.5).

### 3.5 Exact arithmetic

- Integers with 128-bit intermediates; valid while TAI ns < 2^63 (until 2262) and rates < 2^20.
- RTP is `floor(M × R)`; reported ns values are `floor(M × 1e9)`; only final launch instants are rounded to ns.
- Every unit is computed from the anchor, never by adding a rounded period.
- One implementation serves the engine, the `mtl_sync.h` helpers, the RX inverse (§11.2) and the Phase 0.5 legacy helper (§14.2), so they cannot disagree.

### 3.6 Interlaced and PsF

- **Interlaced** (`MTL_INTERLACED`): the index counts fields at twice the frame rate (60000/1001 fields/s for 1080i59.94), while `raster.rate`/`fps` is the **frame** rate. Any slot delay is whole frames. Legacy `ops.fps` for interlaced sessions is the field rate (`frame_time` is a field period, `st_tx_video_session.c:499-518`; the ST 2110-22 box header halves it back,
  `:865-866`), so [migration.md](migration.md) halves it: `ST_FPS_P59_94` + interlaced maps to a 29.97 raster, never to a 119.88-field raster.
- **PsF** (`MTL_PSF`) is paced as interlaced: two segments, the second read from `TVD + TFRAME/2 + TLINE/2` (ST 2110-21 §6.3.3). It has one unit, one index and one RTP per frame, because both segments carry the same RTP (ST 2110-10 §7.6.1). A PsF unit's layout has `height` rows. ANC for PsF has one unit per frame and sends at least one RTP packet per segment (ST 2110-40 §5.5), its keep-alive
  included, both with the frame's RTP; the F bits follow the segment in the interlaced SDI raster that carries PsF.

## 4. Media time on TX

### 4.1 Media modes

`sc.media_mode` is a session property, so a session cannot mix modes and a forgotten flag cannot turn a timestamp into AUTO.

| `media_mode` | Per unit the app sets | For |
|---|---|---|
| `MTL_MEDIA_AUTO` (0) | nothing; MTL stamps the unit with the slot it gets | live generators, simple apps |
| `MTL_MEDIA_INDEX` | `unit.media_index` = unit k of the timeline | playout, file sources, one process owning every essence: **exact** |
| `MTL_MEDIA_TAI` | `unit.media_tai_ns` = the instant the unit represents, snapped (§4.3) | capture, framework sinks, processors |
| `MTL_MEDIA_SENDER` | `unit.media_tai_ns` = the source's own sampling instant, never snapped | async sources (IPMX `mediaclk:sender`): **Phase 7, later** (§13) |

- A non-zero value in a field the mode does not use (`media_index` in an AUTO session) is `-MTL_EINVAL`. Zero means "library default" for every mode field. Launch selection per unit: `MTL_SUBMIT_NOT_BEFORE` or `MTL_SUBMIT_EXACT` with `unit.launch_tai_ns` (§5.4); they exclude each other.

### 4.2 Derived RTP: exact, and its exceptions

```text
RTP(unit) = floor(M(unit) × R) mod 2^32            R = 90000 (video, cvideo, ANC, fastmeta); Fs (audio); rtp.clock_rate (generic RTP)
video, T0 on the grid:   RTP_v(k) = T0·90000 + floor(k × ticks_per_unit)      (T0·90000 is an integer, §3.3)
every field:             RTP_v(field) = floor(M(field) × 90000),   M(2m+1) = M(2m) + TFRAME/2
audio packet p:          RTP = floor(T0 × Fs) + p × S        (p since T0; after a re-phasing DISCONTINUITY at index d: floor(T0 × Fs) + d + p × S)
ANC, grid fastmeta, unit k:   RTP(k) = RTP_v(k)
```

There is no fixed second-field line (`first + floor(TFRAME × 90000 / 2)`); the one rule `floor(M × R)` covers every field, and that is what MTL does today (each field from its own epoch, an even slot is the first field, `st_tx_video_session.c:706-711`). With T0 on the grid:

| Format | First → second field | Why |
|---|---|---|
| 1080i59.94 (TFRAME = 3003 ticks) | always +1501 (then +1502 to the next frame) | TFRAME·90000 is an integer |
| 1080i50 | always +1800 | |
| 1080i60 | always +1500 | |
| a 119.88-field raster (59.94 frames) | alternates +750/+751 | TFRAME·90000 = 1501.5; only reached by a migration mistake (§3.6) |

ST 2110-10 §7.6.1 ("offset by one half of the prevailing frame period, truncating") is met by truncating the result. The default media time is the grid (`N × TFRAME`), which removes the ST20/ST40 disagreement (T2, T6).

**Exceptions to `RTP = floor(M × R)`:**

| Exception | Rule | Phase |
|---|---|---|
| `MTL_SUBMIT_RTP_TS` + `unit.rtp` | the app's RTP, for a verbatim copy of a non-compliant input (rules below) | 1–6 |
| packet units (`MTL_UNIT_PACKETS`) | the app writes the RTP header; `packet.set_fields` 0 = verbatim. `MTL_PKT_TIME_FROM_RTP` takes the unit's time from its first packet's RTP | 2P |
| `MTL_MEDIA_SENDER` | `RTP = RTP0 + floor(k × period × rate)`, §13 | 7 |
| legacy API | default ST20 RTP from the TX cursor; round-to-nearest; `rtp_timestamp_delta_us` (§14) | legacy |

`MTL_SUBMIT_RTP_TS` rules:

- not with AUTO: `-MTL_EINVAL`, reason `RTP_TS_AUTO`. Under AUTO, RX-then-forward jitter makes L alternate and the RTP offset toggle (≈ −3003 / −4504 ticks at 1080p59.94), so the slot must come from an INDEX or TAI media time;
- MTL validates regular increments and reports violations;
- audio: the value is the first sample's RTP, later packets `+S`; an override off the session's packet grid (`(rtp − grid RTP) mod S ≠ 0`) is treated as a DISCONTINUITY and re-phased (§8);
- the recommended way for a time-preserving processor is still *derived* RTP from the input's media time (§10.5).

### 4.3 Snapping TAI-mode times

A TAI-mode `media_tai_ns` (after `media_time_offset_ns`, §4.5) is snapped to a slot of the session's grid. `tx.snap_mode`:

| Mode | Rule | Default for |
|---|---|---|
| `MTL_SNAP_NEAREST` | each unit snaps to the nearest slot `N·P` (phase 0), with hysteresis around the expected slot | every source kind (AUTO and INDEX do not snap) |
| `MTL_SNAP_LOCKED_PHASE` | phase φ₀ is locked at the first unit (or a DISCONTINUITY); each later unit is expected at `M_prev + P` and accepted within `tx.snap_tolerance_ns`; RTP = `floor((N·P + φ₀) × R)` keeps the sampling phase (ST 2110-10 §7.6.2) | genlocked cameras whose phase must be kept |

- **Hysteresis.** The expected slot is `prev + 1`. A value within `P/2 + snap_tolerance` of it takes it, so jitter around the half period never flips between N and N+1.
- **`tx.snap_tolerance_ns` absent** = by mode: TFRAME/8 under NEAREST, TFRAME/4 under LOCKED_PHASE, for video, cvideo, ANC and fastmeta. Audio snaps to the sample grid and absorbs (§8); under LOCKED_PHASE its tolerance is one packet.
- **NEAREST adapts frame rate by drop and gap, and never locks out.** A faster source lands two units on one slot: the second is `DROPPED/DUPLICATE_SLOT`, a result, never a synchronous error. A slower source leaves a slot empty: the underrun policy covers it. RTP stays on the `N·TFRAME` grid. Rejected: locking a phase by default (a 50 ppm crystal at 59.94 drifts out in
  about a day, a 30.000 fps camera on a 29.97 session in seconds, and every later frame is `OFF_GRID`); re-anchoring by default (every re-anchor moves the phase and is a discontinuity).
- **LOCKED_PHASE relock**, `tx.off_grid_policy`: `MTL_OFF_GRID_RELOCK` (default) relocks at the third consecutive `OFF_GRID` unit (the first two are `DROPPED/OFF_GRID`; the third is sent as an implicit forward DISCONTINUITY); `MTL_OFF_GRID_REANCHOR` relocks at the first; `MTL_OFF_GRID_DROP`
  never relocks (genlock monitoring). A relock that would move RTP backwards takes the next slot. A relock sets `MTL_STATUS_TIMING_WARNING` and reports the phase step; under `MTL_OFF_GRID_DROP` the warning stays set while units are dropped.
- Snap error per unit: `mtl_tx_result_full.snap_error_ns`; `MTL_TXR_SNAPPED` when moved.
- **Open:** `status.timing_reason` has no value for an off-grid state (`mtl_reasons.h` has 403–406 only; `OFF_GRID` 505 is a result reason), and the stat catalogue has no gauge for snap error or phase drift (see decisions.md §5).
- There is **no RESLOT for TAI**: a late TAI producer gets `DROPPED/TOO_LATE` with its margin; the fix is a larger `media_time_offset_ns` (§10.5).

### 4.4 Ordering, gaps and discontinuities

- Media indices per session strictly increase in submission order. INDEX mode: an equal index is `DROPPED/DUPLICATE_INDEX`, a smaller one `DROPPED/BEHIND`, both results (framework muxers whose `av_rescale_q` of a 29.97 stream repeats an index). `-MTL_EINVAL` is for malformed units only.
- A **forward gap** means no unit for those slots: the receiver sees a regular-increment gap.
- `MTL_SUBMIT_DISCONTINUITY` marks an intended re-anchor and, **while RUNNING, may only jump forward**. Receivers drop a backwards RTP jump, and so does MTL's RX (`st_rx_video_session.c:1176-1197`; it accepts the old timestamp only after `ST_SESSION_REDUNDANT_ERROR_THRESHOLD` = 20 errors on every port, `st_header.h:81`). A backward
  re-anchor needs stop and start.
- **A restart on the wire.** A start after a stop keeps the session's SSRC (fixed at create: `flows[].ssrc`, 0 = random; the granted value is `mtl_leg_info.ssrc`) and begins at a random RTP sequence number (RFC 3550). On the epoch timeline the RTP timestamps continue the grid, so a restart is RTP-continuous by construction (ST 2110-10 §7.3 note 2); receivers relock only after a backward
  jump (§11.5).
- **Negative indices** are legal on EPOCH and AT_TAI timelines when `M(k) ≥ S` (audio priming before an AT_TAI programme start). On an AT_START timeline every index below the start index is before S and is `FLUSHED/BEFORE_START`.

### 4.5 Media-time offsets

Both move media time and RTP with it; neither is an RTP clock offset (ST 2110-10 §7.3 stays zero).

| Knob | Unit | Applies | Meaning |
|---|---|---|---|
| `tx.index_offset` (R) | whole units | INDEX: `M = T0 + (k + offset)·P`; TAI: `M = snap(media_tai_ns + media_time_offset_ns) + offset·P` | lip-sync trim, production intent (ST 2110-10 §7.7.3); ANC/fastmeta inherit it through the timeline (§10.3) |
| `sc.media_time_offset_ns` | ns, signed | TAI: added **before** snapping, so it selects the slot and the launch follows; AUTO and INDEX: added to the stamped M **after** the slot is chosen, the launch stays the slot's | TAI: the declared **latency** of a just-in-time producer (§10.5); AUTO/INDEX: the ns replacement of `rtp_timestamp_delta_us` |

Under AUTO and INDEX, `media_time_offset_ns` is RxTxApp's compliance trim (`tests/tools/RxTxApp/src/tx_st20p_app.c:311`) with ns precision; its magnitude must be < TFRAME (ST 2110-10 §7.6.3), else create fails with `-MTL_ERANGE`. In TAI mode on a non-epoch timeline `media_tai_ns` is absolute TAI and the reported `media_index` is timeline-relative, `(M − T0)/P`.

## 5. Launch time

### 5.1 The ST 2110-21 model

```text
TFRAME   frame period (exact rational)            N    integer frame index since the SMPTE epoch
TVD      = N × TFRAME + TROFFSET                  TROFFSET constant, 0 ≤ TROFFSET < TFRAME; ≠ TRODEFAULT is signalled TROFF=<µs>
TPRj     read instant of packet j                 first packet on the wire: scheduled_first = TVD − VRX0·TRS
Gapped progressive:      RACTIVE = 1080/1125, TRS = TFRAME × RACTIVE / NPACKETS, TPRj = TVD + j × TRS
                         TRODEFAULT = 43/1125 × TFRAME (height ≥ 1080), 28/750 × TFRAME (< 1080)
Gapped interlaced/PsF:   TRS = TFRAME × RACTIVE / NPACKETS (TFRAME the frame, NPACKETS per frame); RACTIVE = HEIGHT/525, /625, /1125 by line count
                         TPRj = TVD + j × TRS (j < NPACKETS/2);  TVD + TFRAME/2 + TLINE/2 + (j − NPACKETS/2) × TRS after
                         TRODEFAULT (1125 lines) = INT((1125 − HEIGHT)/2)/1125 × TFRAME = 22/1125 × TFRAME at 1080i
Linear:                  TRS = TFRAME / NPACKETS, TRODEFAULT as gapped
Network compatibility:   leaky bucket, TDRAIN = (TFRAME/NPACKETS)/β, β = 1.10, CINST ≤ CMAX
All schedules:           TPR0 = TVD; NPACKETS is constant per frame (ST 2110-22 requires it too)
```

A launch time is the TAI instant the first bit of the packet (the Ethernet start-of-frame delimiter, SFD) leaves the NIC; `launch_tai_ns`, `scheduled_first` and `sent_tai_ns` use that reference point, as the legacy pacing contract draft defines it.

| Sender type (`video.sender_type`) | Read schedule | VRX_FULL | CMAX | TP= |
|---|---|---|---|---|
| `MTL_SENDER_N` (0) | gapped | `MAX(INT(1500×8/MAXUDP), INT(NPACKETS/(27000×TFRAME)))` | `MAX(4, INT(NPACKETS/(43200×RACTIVE×TFRAME)))` | 2110TPN |
| `MTL_SENDER_NL` | **linear** | as N | `MAX(4, INT(NPACKETS/(43200×TFRAME)))` | 2110TPNL |
| `MTL_SENDER_W` | **linear** (ST 2110-21:2022 §7.1.4) | `MAX(INT(1500×720/MAXUDP), INT(NPACKETS/(300×TFRAME)))` | `MAX(16, INT(NPACKETS/(21600×TFRAME)))` | 2110TPW |

Today W and the RX parser use the gapped schedule; E5 fixes TX (behind the sender type). Worked numbers, 1080p 4:2:2 10-bit, 4320 packets per frame:

| Format | TRS gapped | TRS linear | TRODEFAULT | TRO/TRS | VRX_FULL N / W | CMAX N / NL / W | vertical gap |
|---|---|---|---|---|---|---|---|
| 1080p59.94 | 3.7074 µs | 3.8619 µs | 637.674 µs (57.39 ticks) | 172 packets | 9 / 863 | 6 / 5 / 16 | 667.3 µs |
| 1080p50 | 4.4444 µs | 4.6296 µs | 764.444 µs (68.8 ticks) | 172 packets | 8 / 720 | 5 / 5 / 16 | 800.0 µs |

The vertical gap is `TFRAME·(1 − RACTIVE)`. `TLINE/2` is 17.78 µs at 1080i50 and 14.83 µs at 1080i59.94. Today MTL omits it from the second field because TX and RTP are coupled; once they are decoupled (E1) the correct `TPRj` is used without touching RTP. `video.troffset_ns` 0 = TRODEFAULT; an explicit 0 is the option `tx.troffset_ns`. Granted values: `info.troffset_ns`, `info.trs_ps`,
`info.vrx_full`, `info.cmax`.

### 5.2 Source kinds, `min_tx_delay_ns` and the slot rule

With slot delay 0 the slot is `M + TROFFSET`, which passes before a captured frame exists, so the session declares what it is (`sc.source_kind`):

| `source_kind` | Meaning | Defaults |
|---|---|---|
| `MTL_SOURCE_PLAYBACK` (0) | content exists before its media time (files, graphics, generators) | `min_tx_delay_ns` 0; snap NEAREST; `tx.tsmode` SAMP for INDEX/TAI |
| `MTL_SOURCE_CAPTURE` | the unit exists only after its sampling instant (camera, microphone, RX → TX) | `min_tx_delay_ns` 0 = **one unit period + `pickup_lead_ns`** (never 0); snap NEAREST; the first late unit posts `MTL_EVENT_TIMING_INFEASIBLE` and sets `MTL_STATUS_TIMING_WARNING` (`timing_reason` = `TIMING_SHORTFALL`, `shortfall_ns`, `suggested_min_tx_delay_ns`); tsmode SAMP |
| `MTL_SOURCE_GATEWAY` | SDI → IP and low-latency cameras sending in the same frame period | L = 0; `video.troffset_ns` from the app (< TFRAME, signalled TROFF); row units (§6.7); tsmode PRES or SAMP as declared |

The late policy follows the **media mode**, not the source kind (§6.2).

**The slot rule.** The slot is the first N whose first-packet wire time `TVD(N) − VRX0·TRS` is ≥ `M + min_tx_delay`. The slot delay `L = N − E⁻¹(M)` is reported (`info.slot_delay`), not configured, and TSDELAY (`info.tsdelay_ns`) follows from it.

Worked numbers, 1080p59.94, N sender, VRX0 = 8:

| Case | Result |
|---|---|
| L = 1 | first packet at M + 17.29 ms; LIST reports an RTP offset of −1501.5 ticks (−1501/−1502 after its rounding): legal for a camera |
| frame handed over when its readout ends (M + TFRAME) | meets slot N+1's pick-up deadline only if ready by M + 15.39 ms (1.29 ms before the period ends; pick-up lead 1.898 ms) |
| CAPTURE default `min_tx_delay` = 16.683 + 1.898 = 18.58 ms | **L = 2**; first packet at M + 33.97 ms; `min_submit_lead_ns` = −32.08 ms |
| L = 1 for a camera | needs row units (GATEWAY) or a producer that finishes early and declares a smaller `min_tx_delay_ns` |

**The slot-delay bound.** No standard bounds L. ST 2110-10 §7.6.3 ("shall not exceed ±TFRAME from the most recent N × TFRAME") bounds the playback RTP value against the frame grid, not transmit time; with tsmode SAMP any L satisfies §7.5. The real limits are:

- JT-NM Tested / EBU LIST windows: video RTP offset `[−1, ceil(TRO·90000) + 1]` ticks ([−1, 59] at 1080p59.94); video latency (first packet − RTP time) `[0, 1 ms]`; audio latency ≥ 0 and ≤ 1 ms (2020) or ≤ 20 × ptime (2022). Both video windows say "unless justified", and CAPTURE with L ≥ 1 is the justified case.
- The receiver's link offset (ST 2110-10 §7.8, `D_LO ≥ D_TX + D_NET + …`). Option `tx.link_offset_budget_ns` (absent = none) declares the downstream budget and derives `info.max_slot_delay` = the largest L whose last packet still lands within it (video: `L·TFRAME + TRO + RACTIVE·TFRAME` + network allowance ≤ budget). A larger L sets
  `MTL_STATUS_TIMING_WARNING` with reason `LINK_OFFSET_BUDGET`; it is **never rejected**, because the budget belongs to the receiver.

### 5.3 Derived launch per essence

| Essence | First packet | Per packet |
|---|---|---|
| video | `scheduled_first = TVD(N) − VRX0·TRS` | `TPRj` of the sender type (gapped N, linear NL and W); second field and PsF segment `+ TFRAME/2 + TLINE/2` |
| cvideo (ST 2110-22) | `scheduled_first = TVD(N)`: the VRX model does not apply (ST 2110-22 §5.3 note 1), VRX0 = 0, as today (`st_tx_video_session.c:582-585`) | network compatibility model only; rate per `cvideo.rate_mode` |
| audio | `launch(p) = M(first sample of p) + D_a` | `D_a` = `audio.launch_offset_ns`: PLAYBACK default `clamp(pacing profile's max early error + margin, 0, ptime/2)`, tens of µs on RL/TSN, so JT-NM's 1 ms at 1 ms ptime holds; CAPTURE uses `min_tx_delay` (≥ ptime); reported as TSDELAY. Today audio launches exactly at the RTP instant, and any early wire error makes RTP "in the future" |
| ANC | inside the ST 2110-40 window of frame `N = E⁻¹(M) + L_anc` (§9.2) | a deterministic target inside the window, `anc.target_delay_ns`; today packets are spread over the whole frame (`st_tx_ancillary_session.c:1108`) |
| fastmeta | `M + D_fmd` (`fastmeta.target_delay_ns`): PLAYBACK as audio's `D_a` (≤ 1 ms); CAPTURE `min_tx_delay`; with a video in its start it inherits L_v like ANC | ST 2110-41 §7 network compatibility model; keep-alive (§6.3) |
| packet units | `MTL_PKT_PACE_UNIT` = the essence's model over each unit; `MTL_PKT_PACE_LAUNCH` = a chunk at `unit.launch_tai_ns`, then the session rate; `MTL_PKT_PACE_ASAP` = non-compliant | |

**cvideo rate modes.** ST 2110-22 §4 and §5.2 require a constant number of bytes and RTP packets per frame (padding allowed); VBR is not a -22 mode.

| `cvideo.rate_mode` | Wire | Submission |
|---|---|---|
| `MTL_CVIDEO_CBR` (0) | every unit (interlaced: every field) sends the same packet count at a constant TRS; shorter codestreams are padded (RFC 9134 padding) | `codestream_bytes` is the per-unit ceiling **including the box header** MTL prepends; the grant rounds up to whole packets (`info.box_hdr_bytes`, `info.cbr_headroom_bytes`) |
| `MTL_CVIDEO_VBR_MAX` | today's behaviour: packets follow the codestream (`:2481-2485`), TRS from the ceiling (today recomputed per frame, `:750-757`) | as CBR; `MTL_INFO_NON_COMPLIANT` in `info.flags` (IPMX uses it) |

An oversize codestream fails **synchronously at submit** with `-MTL_ENOSPC`, reason `CODESTREAM_OVERSIZE`, when `used > granted − box_hdr_bytes`. Today the check is at build time and reported only through `notify_frame_done` (`:2467-2475`). The box header is the jpvs/jpvi box MTL prepends; today `frame_size = codestream_size + st22_box_hdr_length` (`st_tx_video_session.c:2481`). The FFmpeg
muxer takes `frame_size` from its `bpp` option (`ecosystem/ffmpeg_plugin/mtl_st22p_tx.c:98`), so with `codestream_bytes = frame_size` its acceptance rule (`pkt->size ≤ frame_size`, `:266-286`) is unchanged under CBR and only the wire gains padding. An encoder that is not byte-exact is handled by the ceiling and the padding, not by VBR (a first draft defaulted to VBR_MAX; ST 2110-22 §4, §5.2
reversed it).

### 5.4 Launch overrides

- `MTL_SUBMIT_NOT_BEFORE` + `launch_tai_ns` = t: derive as usual, but use the first slot whose **first-packet wire time** is ≥ t. ST 2110-21 compliant.
- `MTL_SUBMIT_EXACT` + t: the first packet leaves at t, the rest follow the read schedule from there. Not ST 2110-21 in general (TROFFSET is not constant); counted as non-compliant.

An invalid launch fails explicitly and **never falls back** to another pacing mode (G-26): in the past or closer than the minimum lead is `LAUNCH_IN_PAST`, beyond the horizon `BEYOND_HORIZON`, with `-MTL_ERANGE` at submit when knowable then, else a result per the late policy. Today it
silently falls back to default pacing (`st_tx_video_session.c:1796-1805`). An EXACT ST40 request outside the LLTM/CTM window is rejected.

### 5.5 Pacing classes

Today pacing is per port at `mtl_init` and silently downgraded in six places, all to TSC: a shared TX queue, even over an explicit RL (`dev/mt_dev.c:1461-1465`, an info log only); RL training fails for a session (`st_tx_video_session.c:536-542`); the two 2022-7 legs get different ways, and both become TSC (`:545-552`); ST22 (`:3431-3434`); ST30: an explicit RL is honoured only below 2
ms ptime and AUTO tries RL only below 0.5 ms (`st_tx_audio_session.c:2082-2111`); a runtime queue RL failure flips the whole port while running sessions keep "RL" (`dev/mt_dev.c:1649-1654`). Each site raises the downgrade status and event below; the full table, with AUTO's choice and the TSN checks, is in [engine.md](engine.md). In the unified API:

- the session requests a class with option `caps.pacing` (enum `mtl_pacing`: `MTL_PACING_HW` any hardware, `HW_RATE` rate limiter, `HW_LAUNCH` TSN launch time on E830, `SW` TSC, `SW_NARROW`, `PTP`, `BEST_EFFORT` frame start only with no ST 2110-21 claim; absent = any) and `caps.pacing_required` (fail instead of fall back, reason `PACING_UNAVAILABLE`);
- `mtl_session_info.pacing_class` reports the grant and `info.pacing_profile` its accuracy profile (per NIC × class);
- a runtime downgrade posts `MTL_EVENT_PACING_CHANGED` on every affected session and sets `MTL_STATUS_PACING_DOWNGRADED`.

Classes stay per port; sessions request and see the grant (Q-TIME-7, proposed default (a)).

### 5.6 Invariants and signalling

- **T7: no packet of a unit leaves before its media time.** A wide sender's pre-fill is capped so that `scheduled_first ≥ M` (today MTL caps W pre-fill at `min(0.8·VRX_FULL, 0.8·TRO/TRS)`, TRO/TRS from §5.1). `NOT_BEFORE t` never sends a packet before t (G-59).
- **`tx.tsmode`** (`MTL_TSMODE_SAMP`, `NEW`, `PRES`) is declared, not inferred (ST 2110-10 §8.7): an app that "thinks in ns" is not a sampling-instant source. Default from the source kind; reported as `info.tsmode` with `info.tsdelay_ns` (which includes L·TFRAME or `min_tx_delay`).
- The values an SDP needs (TP, TROFF, CMAX, TSMODE, TSDELAY, ts-refclk) are `info.*` stats; MTL renders SDP only in Phase 7 (`mtl_sdp.h`, `MTL_LATER`).

## 6. Admission and lateness

### 6.1 Where the decision happens

The engine decides a unit's timing when it **picks the unit up**, well before its first packet. For video that is about one ring depth (≤ 512 packets) before the previous frame ends.

```text
submit ──▶ QUEUED ─────────▶ pick-up (decision) ─────────▶ scheduled_first ──────▶ last packet
  │                                │                         (video: TVD − VRX0·TRS)
  │  deadline = scheduled_first − pickup_lead_ns
  │  min_submit_lead_ns = M − deadline           (for a unit on its derived slot)
  └─ synchronous checks: lease, layout, horizon, exact launch in the past, cvideo size
     margin_ns = deadline − submitted;      pickup_slack_ns = deadline − pickup
```

- **`pickup_lead_ns`** (`info.pickup_lead_ns`): how long before `scheduled_first` the engine decides. It covers ring depth × TRS, a conversion stage if any, and one packet for audio (the carry, §8).
- **`min_submit_lead_ns`** (`mtl_session_info.min_submit_lead_ns`) = `M − deadline` = `pickup_lead_ns − (scheduled_first − M)`, constant for on-grid media. It is the one number a producer needs: **submit unit k before `M(k) − min_submit_lead_ns`**. For a framework it is the element's minimum latency (GStreamer LATENCY minimum, FFmpeg `-muxdelay`;
  `mtl_session_info.latency_min_ns`). It is negative for CAPTURE.
- `mtl_session_query` returns both before create (dry run).

Worked numbers, 1080p59.94 PLAYBACK, N sender, 4320 packets, ring 512 packets, TRS 3.7074 µs:

| Quantity | Value |
|---|---|
| `pickup_lead_ns` | 512 × TRS = 1.898 ms |
| `scheduled_first − M` = TRO − VRX0·TRS | 604–619 µs (VRX0 9…5; 608.0 µs at VRX0 = 8). Today VRX0 is 9 for TSC_NARROW, 6 for TSC with bulk 4, 5 for RL (`st_tx_video_session.c:566-579`) |
| `min_submit_lead_ns` | **1.279–1.294 ms** |
| deadline | ≈ **M − 1.29 ms** |

A just-in-time producer that hands frame k over *at* M(k) (a GStreamer sink with `sync=TRUE`, `ffmpeg -re`) is 1.29 ms late on every frame under INDEX or TAI and gets `DROPPED/TOO_LATE`. The answer is `media_time_offset_ns ≥ min_submit_lead_ns + margin`, declared as latency (§10.5); it is not a slide.

### 6.2 Late policy

`tx.late_policy`:

| Policy | A unit whose pick-up deadline passed… | RTP / grid | Result |
|---|---|---|---|
| `MTL_LATE_DROP` (default for **INDEX and TAI**) | is not sent | its slot stays empty: regular increments kept | `MTL_TX_DROPPED`, reason `TOO_LATE`, margins |
| `MTL_LATE_SEND_LATE` (**bounded**) | is sent at TRS from its actual start, only if its last packet completes before the next occupied slot's `TVD − VRX0·TRS` and within `tx.late_tolerance_ns` (default `min(TROFFSET, TFRAME − RACTIVE·TFRAME)`); else dropped | RTP unchanged | `MTL_TX_LATE`, or `DROPPED/WOULD_OVERLAP` |
| `MTL_LATE_RESLOT` (default for **AUTO**) | takes the next free slot and that slot's media time | stays regular | `ON_TIME` with `MTL_TXR_RESLOTTED`; the full record's `slots_skipped_before = n` |

- **The media mode sets the default**: AUTO → RESLOT for every source kind (so AUTO + PLAYBACK, the plain generator, gets RESLOT); INDEX/TAI → DROP for every source kind. CAPTURE also raises `TIMING_INFEASIBLE` on its first late unit.
- RESLOT is valid only when the app did not assign a media time: INDEX/TAI sessions reject it with `-MTL_EINVAL`. It is today's default without user timing (jump to the current epoch, `stat_epoch_drop`).
- `slots_skipped_before` is only in `mtl_tx_result_full` (reap with its size); the core `mtl_tx_result` says only `MTL_TXR_RESLOTTED`. Without results, or with core records only, `tx.slots_empty` and `tx.units_dropped{reason=too_late}` still count.
- Rejected: unbounded "send as soon as possible". A 1080p59.94 frame occupies ≈ 16 ms of wire time, so a frame 1 ms late overlaps the next slot and every later unit becomes late (the slide T5 forbids); MTL's own RX would drop it anyway (`st_rx_video_session.c:1176-1192`).

### 6.3 Underrun policy (no unit for a slot)

`tx.underrun_policy`, default by essence:

| Policy | Wire | Default for |
|---|---|---|
| `MTL_UNDERRUN_SKIP` | nothing; `tx.slots_empty`; the next full result carries `slots_skipped_before` | video, cvideo, audio |
| `MTL_UNDERRUN_EMPTY_ANC` | the empty ANC packet (`ANC_Count = 0`, marker set) at the start of the last VANC line, `RTP = RTP_v(k)`, F bits from parity, inside the window of §9.2: ST 2110-40 §5.5 requires one RTP packet per field, frame or segment | ANC |
| `MTL_UNDERRUN_KEEPALIVE` | an empty packet when nothing was sent for 450 ms: ST 2110-41 requires one every 500 ms | fastmeta |
| `MTL_UNDERRUN_SILENCE` | zero payload with regular RTP | audio option |
| REPEAT_LAST | the previous payload with the new slot's media time | later capability (Q-TIME-4) |

Today MTL sends nothing when the app has no ANC frame (`st_tx_ancillary_session.c:942-946`), which breaks the keep-alive rule; an empty ANC frame from the app does send one packet (`:966`). `MTL_EVENT_TX_UNDERRUN` reports slots filled by the policy.

### 6.4 Early, too far ahead, and preroll

- **A unit is never sent early**; it waits for its slot.
- **Horizon** `tx.horizon_ns` (default 1 s), measured from **`max(now, S)`**, S = the resolved start instant. A submission beyond it is `-MTL_ERANGE`, reason `BEYOND_HORIZON`. Today such a unit is honoured by the builder and then sent at once with an error log (`st_video_transmitter.c:444-461`). No standard gives a horizon: the only hard bounds are RTP unwrap ambiguity (half a wrap, ±6.6 h
  at 90 kHz) and, for playback video, ST 2110-10 §7.6.3's ±TFRAME between the RTP and the slot's epoch. 1 s is a product limit, the same as today's `max_onward_epochs`.
- **Preroll** is a duration, `mtl_when.preroll_ns`, added to the lead of the T0 resolution (§3.3). The returned instant is computable from `now`, the members' `min_submit_lead_ns`, `preroll_ns`, k and G. Because the horizon counts from S, a long preroll is bounded by the pool and the horizon from S, not by 1 s from now.
- **Units queued before resolution** are checked when start resolves S: units with M < S are `FLUSHED/BEFORE_START`; if any unit has M > S + horizon, **start fails atomically** with `-MTL_ERANGE`, `BEYOND_HORIZON`, nothing starts and every queue stays intact. Example: 2 s of 48 kHz audio (indices 0…95999) pre-submitted with the default horizon fails, because indices above
  48000 lie beyond S + 1 s; `tx.horizon_ns ≥ 2 s` makes it start. After such a failure the app withdraws units (`mtl_tx_withdraw`), discards the queue (`mtl_session_discard`), or raises `tx.horizon_ns` (an S key: allowed while CREATED or STOPPED) and starts again. Deep-buffer playout is why the horizon is configurable (Q-TIME-3).

### 6.5 The slot hint

`mtl_tx_next_slot(s, &hint, size)` (DP) fills `struct mtl_slot_hint` (32 B): `queued`, `next_media_index`, `next_media_tai_ns`, `submit_deadline_tai_ns`. One formula for AUTO, joiners and INDEX producers, the admission test itself: `next_media_index` = the smallest k after the last submitted with
`M(k) − min_submit_lead_ns ≥ now`. For AUTO it is the slot the unit would get; for a joiner the first index it can still make. It is OpenXR's `predictedDisplayTime` and CoreAudio's `inOutputTime`: the producer learns its slot
before it renders, which removes the fixed phase lock a downstream engine measured (≈ 27 ms). The per-slot tick `MTL_EVENT_EPOCH_TICK` is opt-in (`session.epoch_tick`); it replaces `ST_EVENT_VSYNC`.

### 6.6 Result timing fields

Core record `mtl_tx_result` (96 B): `status` (`ON_TIME`, `LATE`, `DROPPED`, `FLUSHED`, `FAILED`), `reason`, `media_index`, `media_tai_ns` (after offsets and snapping), `margin_ns` (deadline − submit; negative = late), `sent_tai_ns` (first packet), `rtp` (first packet on the wire), `flags`.
The full record `mtl_tx_result_full` (216 B, `mtl_observe.h`; pass its size to `mtl_tx_reap`) adds `submitted_tai_ns`; `deadline_tai_ns` (latest pick-up that meets the slot); `scheduled_tai_ns` (the planned first-packet launch: `TVD − VRX0·TRS`, or the ANC, audio, fastmeta target);
`enqueued_tai_ns` (SW: TSC at the burst that carried packet 0, not wire time); `observed_first_tai_ns[leg]` and `observed_last_tai_ns` (NIC TX timestamps per 2022-7 leg, `MTL_TXF_OBSERVED_*`); `pickup_slack_ns` (deadline − pick-up); `snap_error_ns`; `max_packet_lateness_ns` (worst packet against
its TPRj); `slots_skipped_before`; and the audio counts `samples_padded`, `samples_dropped`, `samples_inserted` (§8).

`margin_ns` answers "how close was I?", not just "was I late?" (Vulkan `presentMargin`). Under RL pacing a SW observation at `tx_burst` precedes the wire by the NIC ring occupancy, so only HW TX timestamps fill `observed_*`. TX self-check counters (`tx.vrx_max`, `tx.cinst_max`, `tx.tpr_late_pkts`, `tx.margin_min_ns`, histograms) are v1 once E4 exists.

### 6.7 Row units (progressive submission)

Row submission is readiness inside one timed unit, chosen at create (`sc.unit = MTL_UNIT_ROWS`): submit the lease with `used` = rows ready, then submit the same lease again with a larger `used`. It maps onto today's ST20 slice mode (`query_frame_lines_ready`, `st_tx_video_session.c:2021`), a session type `st20p` does not offer: Phase 6.

- A unit's slot and RTP are fixed at the first submit from its media time.
- If the engine reaches a packet whose rows are not published by that packet's deadline, `tx.rows_late` applies: `MTL_ROWS_TRUNCATE` (default; stop sending the unit, the result reports it), `MTL_ROWS_PAD` (send the rest from a black or previous-line source), `MTL_ROWS_STALL` (today's slice behaviour; breaks pacing; legacy only).
- `mtl_tx_row_deadline(s, k, row, &tai_ns)` returns the latest publish time of `row` for the slot of index k, with the engine's math. Interlaced rows count field lines; planar formats count a row when every plane's row is written.
- RX: a unit dequeued with `MTL_UNITF_PARTIAL` grows while held; `mtl_rx_wait_rows(s, lease, min_rows, &rows, timeout)`, wake-ups every `rx.rows_step` rows (64).

## 7. Start

### 7.1 Starting sessions together

There is no group object (D-78): `mtl_session_start(s, n, when, &t0)` takes an array. `struct mtl_when` (24 B) carries `kind` (`MTL_NOW` = 0, `MTL_AT_TAI`, `MTL_AT_INDEX`), `value` and `preroll_ns`; NULL means NOW.

`mtl_session_start` with n > 1:

1. validates every session as a single start would; the array must be one timeline and one direction (`-MTL_EINVAL`, `START_SET_MIXED`), and no TX session may be AUTO (AUTO stamps whatever slot a unit gets and cannot be synchronised);
2. resolves T0 once with §3.3, including `preroll_ns`;
3. checks the horizon rule of §6.4 for every queued unit;
4. arms every session and returns T0. If any cannot be armed, none is (reason `START_SET` on the others). G-23.

ANC and fastmeta sessions take their raster, rate and L_v from the first video session of their start; for a later single start, from the timeline's video owner. On the epoch timeline the ANC raster (`anc.video`) is required. An ANC or grid-fastmeta session whose explicit `anc.video` / `fastmeta.video` has another rate or raster than the first video session of the start fails the start's
validation (step 1) with `-MTL_EINVAL`. **Open:** its reason code (`START_SET_MIXED` or `GRID_MISMATCH` would fit; see decisions.md §5). `mtl_session_stop(s, n, mode, timeout)` issues every stop first and waits once.

### 7.2 Joining a resolved timeline

- `MTL_NOW` on a resolved timeline joins at the first feasible index (`mtl_tx_next_slot`). A joiner whose unit period does not fit the resolved grid (G not a whole number of its frames; an audio rate excluded from G is floor-aligned as in §3.4) fails with `-MTL_EINVAL`, `GRID_MISMATCH`, as a MEDIA update does ([contract.md](contract.md) §4.7).
- `MTL_AT_INDEX k` on a resolved timeline whose M(k) is closer than `now + lead` fails with `-MTL_ERANGE`, reason `START_IN_PAST`; it never silently drops the start (G-63: a lazily anchored timeline is never resolved in the past).
- Framework plugins that own one session each (separate FFmpeg muxers, GStreamer sinks) share a **named** timeline in one process: the first starter resolves T0, the others join with `MTL_NOW`. No array is needed for alignment, only for atomic start.
- RX: `when` is the earliest media time delivered (§11.4).

**Restart after a failure.** Late handling is per unit and per session; a timeline never re-anchors on its own, except through its step policy (§12). A session that enters ERROR posts `MTL_EVENT_SESSION_STATE`; the others continue. Restart: `mtl_session_stop` (ERROR → STOPPED), fix the cause
(`mtl_session_update`, a port that came back), then `mtl_session_start(&s, 1, NULL, NULL)`: it rejoins at the next
feasible index with the inherited offset and L_v.

## 8. Audio

- **Packetisation from absolute sample indices.** Samples per packet S comes from `audio.ptime` and the sample rate through today's table (`st30_get_sample_num`, `st_fmt.c:1152-1210`): 48 at 1 ms/48 kHz, 6 at 125 µs, 16 at 333⅓ µs; the three 44.1 kHz values are defined in samples (48, 6, 4: `MTL_PTIME_1_09MS`, `_0_14MS`, `_0_09MS`), so S is exact there too. A pair outside the table is
  `-MTL_EINVAL`, as today. The packet time is S/Fs, exact. Packet p covers `[p·S, (p+1)·S)` since T0, or since the last re-phasing DISCONTINUITY.
- **The non-integer case, 80 µs.** `MTL_PTIME_80US` is 3.84 samples at 48 kHz (7.68 at 96 kHz). Today it sends 4 (8) samples every 80 µs (`st_fmt.c:1197-1199`; ST30 TX uses `trs = pkt_time`, `epochs = TAI / pkt_time`, RTP = epochs × S, `st_tx_audio_session.c:207-211`, `:238`, `:274-282`), a 50 kHz RTP rate at 48 kHz (+4.17 %, ≈ 41.7 ms per second against PTP; side finding SF-35). Proposed: S
  = 4 (8) and the packet time S/Fs = 83⅓ µs, so `RTP = floor(M × Fs)` holds; this is what open PR #1770 does on the legacy path. **Open:** confirm
  this rule, or reject `MTL_PTIME_80US` at create with `-MTL_EINVAL`.
- **Carry.** A submission whose end is not on a packet boundary leaves a partial packet (AAC 1024 = 21 × 48 + 16; 800/801 samples per frame at 59.94; 1601/1602 at 29.97). At pick-up MTL copies the tail into a per-session carry buffer (< S × channels × sample size, ≤ 1152 B at Level A). Submission n completes at its last whole packet and its slot returns promptly; the
  straddling packet belongs to n+1 and uses n+1's deadline. On a stop, a drop or a late n+1, the carry is zero-padded to a full packet and sent on time (`samples_padded`). The audio `pickup_lead_ns` is therefore one packet earlier than the first packet's launch.
- **Where a submission lands.** e = the expected first sample (end of the previous submission); s = the given one (`media_index`, or `round(media_tai_ns × Fs)` in TAI mode after snapping to the sample grid).
  - **Absorb** (`audio.absorb_samples`; absent = **S for CAPTURE and for TAI mode, 0 for PLAYBACK and GATEWAY in INDEX mode**; AUTO is contiguous by construction; present 0 = never): if `|s − e| ≤ absorb`, the submission is placed at e and `s − e` accumulates in the `audio.drift_samples` gauge. That covers `alsasrc` PTS jitter and the `round(PTS × 48000)`
    off-by-one, without a click or an error.
  - **Forward gap** (`s > e + absorb`): the packet grid is kept. Silence fills `[e, s)` inside the packets that contain e and s; packets wholly inside the gap are not sent, so the receiver sees an RTP gap of a multiple of S; the new data starts at its exact sample s; `samples_padded` reports the silence. A dropped video frame's audio (800 mod 48 = 32 at 59.94) therefore never
    moves packet boundaries, and AES67 receivers see no packet-phase change.
  - **Overlap** (`s < e − absorb`): samples `[s, e)` were already sent or queued; they are removed (`samples_dropped`) and the rest continues at e. Nothing is re-phased.
- **Only `MTL_SUBMIT_DISCONTINUITY` re-phases** (and an RTP override off the output grid). The carry is zero-padded (`samples_inserted`). If the new index d is not a multiple of S, packet p covers `[d + p·S, d + (p+1)·S)`. RTP stays `floor(M × Fs)` of each packet's first sample. While RUNNING, forward only.
- **Sample-accurate start.** A session started at S sends its first packet at s0, the smallest multiple of S whose `M(s0) ≥ S`; earlier samples are `FLUSHED/BEFORE_START`.
- **Copy path.** `mtl_tx_write(s, data, bytes, &how, timeout)` (`mtl_util.h`) takes an arbitrary byte run for audio, ANC and fastmeta, with the `media_index` of `how`; it acquires, splits and submits internally. The sample count follows from the bytes (`unit.used` is bytes for audio).
- **1001 cadence** (helper, later, Q-TIME-20): samples in frame k = `floor((k+1)·x) − floor(k·x)`; on the epoch grid 59.94 gives 800, 801, 801, 801, 801 repeating, 29.97 gives 1601, 1602, 1601, 1602, 1602. The often-quoted 1602/1601/… is the ST 272/299 embedding cadence on the ST 318 sequence. 2110 audio has no per-frame unit. The alternative is a pinned ST 299 five-frame phase instead of
  the epoch formula. Dolby E carried over ST 2110-31 needs frame-aligned audio, which is why the helper is wanted.

## 9. ANC and fastmeta

### 9.1 ANC units, RTP and limits

- One unit per video frame or field; `media_index` is the associated video index; RTP = `RTP_v(k)`. F bits follow index parity: `0b10` first field, `0b11` second, `0b00` progressive. Marker on the last ANC RTP packet of the frame or field. Interlaced ANC from a file uses `index = 2·pts + field`, two units per frame.
- The packets are `struct mtl_anc_packet` records (`did`, `sdid`, `line`, `hoffset`, `c`, `stream`, `udw_count`, `udw_offset`) in the slot's meta area (`MTL_META_ANC`), the UDW bytes in plane 0, snapshotted at submit (D-86).
- Limits checked at submit with `-MTL_EINVAL`: packets per unit ≤ 255 (RFC 8331 `ANC_Count` is 8 bits; `anc.max_packets`); `udw_count` ≤ 255 (today's `st40_rfc8331_encode_packet` already rejects more, `include/st40_api.h:901`); `udw_offset + udw_count` within `used`, offsets increasing and non-overlapping. Today the TX meta array is capped at `ST40_MAX_META` = 20 (`include/st40_api.h:308`, E12).
- `line = 0` means unspecified: MTL writes RFC 8331's no-specific-line code, and ST 2110-40 §6.2.1's TLBO rule for packets without a line number applies (two lines after the RP 168 switching point).

### 9.2 The ANC transmit window and live ANC

```text
TLBO     time of the packet's SDI location after the most recent ST 2059-1 alignment point
TSFO     = TFRAME/2 + TLINE/2          (second field: TSST = TFST + TSFO)
TEPO(j)  = min TLBO of the ANC packets in RTP packet j (empty packet: start of the last VANC line)
TAD      = N × TFRAME + TROFFSET_ANC;    TFST = TAD − TRODEFAULT (= N × TFRAME by default)
window   [TFST + TEPO + TD − TFRAME, TFST + TEPO + TD]
TD       = 1 ms (CTM, default, also when TM is absent)  or  8 / (FrameRate × TotalLines) (LLTM; 118.64 µs at 1080p59.94)
```

`anc.timing_model` (`MTL_ANC_CTM`, `MTL_ANC_LLTM`), `anc.target_delay_ns` (the deterministic target inside the window), `anc.total_lines` (TEPO needs the SDI raster: absent = from the video raster, 1125 for 1080 lines, 750 for 720, 2250 for 2160, 625 for 576i, 525 for 480i/486i). `info.total_lines` and `info.anc_tm` report the values used. `anc.total_lines` is TX only (TEPO); an RX ANC
session ignores it.

**The live-ANC rule** (D-67, M4):

> ANC for video unit k carries `RTP_anc(k) = RTP_v(k)` and is sent inside the ST 2110-40 window of
> the frame in which video unit k is actually transmitted: **N = E⁻¹(M(k)) + L_v**, L_v = the
> video's granted slot delay. Standalone ANC uses its own L (its source kind and
> `min_tx_delay_ns`; PLAYBACK gives 0).

Why: ST 2110-40 §6.3 builds the window on the video transmission timing (`TAD = N × TFRAME + TROFFSET_ANC` mirrors -21's TVD, and N indexes the transmitted frame); §6.1 bounds the time from the ANC appearing in the input SDI signal, which in an L-frame-delayed chain is frame k + L_v; the association is by RTP (§5.4, the §6.3 note); and ANC for a captured frame exists no earlier
than the frame, so a window anchored on the RTP frame closes ≈ 15.7 ms (CTM) before a CAPTURE frame with L = 1 can leave. ST 2110-40 §6.1 imports only TFRAME, TLINE, TROFFSET and TRODEFAULT from -21; N is not defined in -40 and nothing in -40 ties N to the ANC RTP, so the window is a transmission-regularity model and the RTP carries the association (§6.3 note). Receivers gain: video and ANC
of frame k arrive in the same frame period, so an SDI-reconstructing receiver needs no extra ANC buffering, and an analyser that computes N from the ANC RTP reports the same ≈ −L·TFRAME offset for ANC as for video (JT-NM's "unless justified" case). Rejected: forbidding live ANC with L ≥ 1, or flagging such a correct stream as non-compliant.

- `anc.window`: `MTL_ANC_WINDOW_AUTO` (default; the video's transmit frame, or the unit's own frame + own L without a video) or `MTL_ANC_WINDOW_MEDIA` (strict N = E⁻¹(M); live units then usually arrive too late and are `DROPPED/TOO_LATE`).
- The ANC submit deadline for unit k is the window end of frame N minus the ANC pick-up lead; with L_v = 1 a live producer has one more frame period; `mtl_tx_next_slot` reports it.
- L_v is constant (constant `min_tx_delay`, snapping keeps M on the grid). A dropped video unit does not move the ANC window: ANC k still goes out in frame k + L_v, and so does the keep-alive.
- `info.anc_window_frame_offset` = L_anc. For PLAYBACK the ANC goes ≈ 0.3–1.3 ms after the frame epoch, so ANC for frame k must be submitted before or with video frame k.
- The GStreamer `mtl_st40p_tx` sink stamps `GST_BUFFER_PTS + pts_for_pacing_offset` as TAI today (`gst_mtl_st40p_tx.c:719-720`); it becomes TAI + CAPTURE (or PLAYBACK) on the video's timeline with no pacing offset of its own.

### 9.3 Fastmeta (ST 2110-41)

Today ST41 is video-rate based: `frame_time = 1e9·den/mul` from the session fps, RTP `epochs × frame_time_sampling` at 90 kHz (`st_tx_fastmetadata_session.c:206-234`; `sampling_clock_rate = 90000` in `st_fmt.c`); `fps`, `interlaced` and `second_field` are in `st41_tx_ops` and `st41_tx_frame_meta` (`include/st41_api.h:135-175`). R stays 90000; an item spec needing another RTP clock is a later
extension (ST 2110-41 §5.3 lets the item spec define it).

- `fastmeta.video` is the **associated video raster**: one data item group per video frame or field, `media_index` = the video index, RTP = the video RTP, on the grid like ANC. All zero on a created timeline takes the first video of the start; on the epoch timeline, or with no video, it is required (`-MTL_EINVAL`, `FIELD_REQUIRED`).
- `MTL_FASTMETA_FREE_RUNNING`: `video.fps` is the stream's own unit rate (required). The stream is off the video grid (excluded from G), `M(k) = T0 + k/rate`; in TAI mode each unit's time is taken as given (no snap); RTP = `floor(M × 90000)`.
- Field parity follows the index (`video.scan`); there is no `second_field` input. Launch `M + fastmeta.target_delay_ns` (§5.3); keep-alive every 450 ms; marker 0 on every packet (this edition; MTL sets 0, `st_tx_fastmetadata_session.c:186`).

## 10. A/V/ANC synchronisation

### 10.1 Mechanism

```text
                   timeline TL:  T0 = n × 1001/6000 s   (resolved at start, on the common grid)
                     /                     |                        \
   video (59.94p)                   audio (48 kHz)                  ANC
   unit k → M = T0 + k·1001/60000   sample s → M = T0 + s/48000     unit k → the M of video k
   RTP_v = 15015n + floor(1501.5·k) RTP_a(pkt p) = 8008n + 48p      RTP_anc = RTP_v
   launch: slot 10n + k (+L),       launch: M(first sample) + D_a   launch: window of frame
   TROFFSET, read schedule                                          10n + k + L_v (§9.2)
                     \                     |                        /
              mtl_session_start(array): validate all → resolve T0 → arm all → run
```

15015n = T0·90000, 8008n = T0·48000 and 10n = T0/TFRAME are integers because T0 is on the 1001/6000 s grid. Checked exactly for n = 0, 1 and 10727972028 (near today's TAI): every `RTP_v` for k = 0…39 and every `RTP_a` for p = 0…99 match; `floor(1501.5·k)` runs 0, 1501, 3003, 4504, 6006, 7507, ….

### 10.2 File playout: the maintainer's example

A file holds 59.94p video (`time_base = 1001/60000`), 48 kHz audio (`1/48000`) and caption ANC, all from pts 0. [Example 7](sketch/examples/ex07_av_anc_playout.c) is the program:

- one `mtl_timeline_create` with the defaults (anchor AT_START);
- three TX sessions with `sc.timeline = tl`, `sc.media_mode = MTL_MEDIA_INDEX`, source kind PLAYBACK; the ANC raster comes from the video it starts with;
- `mtl_session_start(s, 3, NULL, NULL)`: all three or none;
- the demux loop: video frame pts p → `media_index = p`; ANC for frame p → `media_index = p` (before or with video p); audio packet pts q → `mtl_tx_write` with `media_index = q`.
- the general file rule: `index = round((pts − pts0) × time_base × rate)` with exact rationals (time_base `1/90000`, `1/48000`, `1001/60000`, Matroska `1/1000`), the "closest media rate tick" of MS-04 example 5; then `M = T0 + index·P`. A rate labelled "29.97" is 30000/1001. Audio priming (encoder delay) gives negative sample indices: do not send them, or start the audio at index ≥ 0 and
  keep the grid (§4.4).

- RTP for video frame k, its ANC, and the audio sample at the same instant are exact functions of `T0 + k·1001/60000`, for any start index: no half-frame snap, no TR_OFFSET contamination, no floating point.
- Each essence transmits on its own model: video in its slot, audio at `M + D_a`, ANC in its window. Frames without ANC still get the empty keep-alive packet.
- A late video frame 37 is dropped (DROP), or sent late only if it fits (bounded SEND_LATE). Audio is unaffected and frame 38 keeps its media time. Nothing slides (G-24).
- Today the same app gets up to half a video frame plus TR_OFFSET of RTP-derived A/V error with USER_PACING, and ST30P cannot set a user timestamp. Phase 0.5 closes that on the legacy API first (§14.2).

### 10.3 Lip-sync trims and offset inheritance

- A deliberate A/V offset (production intent, ST 2110-10 §7.7.3) is an integer `tx.index_offset` on one session, never an RTP offset.
- The offset is a **timeline property** (`mtl_timeline_info.index_offset`): set by the first start of a video session on the timeline (the "offset owner"), updated when that session is re-based (§10.5). An ANC or fastmeta session whose rate equals the owner's and whose own offset is 0
  inherits it at start or join; when re-based itself it adopts the timeline's current value. This holds with or without a start array, so a caption inserter that joins mid-programme gets the video's offset.
- Audio never inherits a frame offset (800.8 samples per frame at 1001 rates is not an integer); its trim is its own `tx.index_offset` in samples.

### 10.4 Separate processes

A named timeline lives in one instance, so one FFmpeg process per essence cannot share it:

| Way | Each process | Index for a pts |
|---|---|---|
| **EPOCH + INDEX** (recommended) | sessions on the epoch timeline (null handle), `media_mode = INDEX`; the processes agree on `t_start` out of band (command-line, NMOS activation time) | `k = k0 + pts_units`, k0 = the first unit at or after `t_start` from `mtl_epoch_index_at(t_start, unit_rate)` (frames for video, samples for audio; rounding below) |
| **AT_TAI** | `mtl_timeline_create` with `anchor = MTL_ANCHOR_AT_TAI`, the same `tai_ns` and the same explicit `grid` (for example 1001/6000 s) in every process | `k = pts_units`; for a join, `mtl_index_at(s, tai_ns, &k)`, then k + 1 when `tai_ns` is past unit k's start |

- **Rounding.** `mtl_epoch_index_at(t, rate, &k)` and `mtl_index_at(s, t, &k)` round **down**, exactly: k is the unit that contains t, `M(k) ≤ t < M(k + 1)` (`mtl_index_at` on the session's timeline; `-MTL_EBUSY`, `WRONG_STATE`, while T0 is unresolved). The **first unit at or after t** is k when `M(k) = t`, else k + 1. A join or a start that used k for an off-grid t would pick a unit
  already in the past.
- `mtl_epoch_index_at` needs no instance, so a process with only audio computes the same instant as one with only video.
- **Recipe.** Pass a grid-aligned `t_start = ceil(t/G)·G` (G = 1001/6000 s for the 1001 family with 48 kHz). Then `M(k0) = t_start` for every essence, k0 is both the containing and the first unit, `M(k0_video) = M(k0_audio)` exactly, and every RTP matches the single-process start (G-105). With an off-grid `t_start`, each process takes k0 + 1 for a stream whose own unit grid does not hit
  `t_start`; video then starts up to one frame after it, audio within one sample, and the RTPs no longer match a single-process start.
- AT_TAI without an explicit grid would snap by each process's own members, and two processes with different essences would disagree; that is why the grid is mandatory here.

### 10.5 Recipes

**Live framework sinks: TAI + NEAREST + a declared latency.** INDEX is for whole-timeline owners (file playout, one process that owns every essence).

- **GStreamer sink.** `ct = base_time + running_time(PTS) + latency` is the buffer's presentation clock time. Map `ct` to TAI: a `GstPtpClock` is already PTP; the default `GstSystemClock` is MONOTONIC, use `mtl_time_convert(MONOTONIC → TAI)`; any other clock, fit a map from `mtl_time_cross` samples around `gst_clock_get_time`. Submit `media_tai_ns = TAI(ct)`. Set
  basesink `render-delay = D ≥ min_submit_lead_ns + margin` (1 ms margin is a safe start); with `sync=TRUE` basesink hands each buffer over D early and adds D to its latency. Without basesink sync, use `media_tai_ns = TAI(base_time + running_time)` and `media_time_offset_ns = D` with `D ≥ upstream latency + min_submit_lead_ns + margin`, reported as latency. Seeks are harmless:
  the clock-time mapping stays monotonic, so TAI never repeats.
- **FFmpeg muxer, live (`-re`).** `media_tai_ns = TAI(start_time_realtime) + rescale(pts)`, `media_time_offset_ns = -muxdelay` (≥ `min_submit_lead_ns` + margin). With `CLOCK_TAI` and no ptp4l the source is SYSTEM_TAI, ESTIMATED, and the status carries reason `TIME_ESTIMATED`.
- **OBS output.** The same, with `mtl_time_convert(MONOTONIC → TAI)` of the OBS timestamp (today's `tfmt = MEDIA_CLK` with a monotonic value is a bug).
- NEAREST adapts a free-running camera or a 30.000 fps source on a 29.97 session by occasional `DUPLICATE_SLOT` drops or gaps and never locks out; `alsasrc` jitter is absorbed (§8).

**Time-preserving processor (RX → TX)**, [example 10](sketch/examples/ex10_processor.c):

- `media_mode = MTL_MEDIA_TAI`, `unit.media_tai_ns` = the received unit's `media_tai_ns` (only when `MTL_UNITF_TAI_VALID`; a `mediaclk:sender` input has none);
- `source_kind = MTL_SOURCE_CAPTURE` (an RX → TX unit exists only after its media time, §5.2) and `min_tx_delay_ns` = the pipeline budget (RX completion at ≈ M + 16.7 ms for 1080p59.94, plus processing, plus margin); snap NEAREST (GATEWAY's L = 0 cannot hold a processing budget).
- L is fixed, because the slot comes from M, not from arrival jitter; a unit over budget is `DROPPED/TOO_LATE`, and L never toggles. The derived RTP equals the input RTP whenever the input was on the grid (the RX media time is exact to less than one tick, and NEAREST snaps back to the same `N·TFRAME`). An off-grid input is re-stamped to the grid; to keep its phase use
  LOCKED_PHASE, or `MTL_SUBMIT_RTP_TS` with INDEX or TAI.
- Audio: the same with the RX unit's first-sample media time; output packets stay on their own grid, RTP `floor(M × Fs)` of each first sample, equal to the input RTP for equal S. ANC: the video unit's media time, so ANC RTP = video RTP.

**Seeks with an INDEX sink.** TAI is recommended for any sink whose pts can restart. For INDEX: `mtl_session_discard(s, MTL_DISCARD_REBASE, new_pts_index)`: the session stays RUNNING; queued units become `FLUSHED/DISCARD`; `tx.index_offset` is set so that `first_index` maps to the next
feasible slot on the shared timeline; the next submission is an implicit forward DISCONTINUITY; `mtl_get_option` of `tx.index_offset` (`MTL_OPT_INDEX_OFFSET`) then returns the re-based offset (an R key, the effective value). The app keeps submitting pts-derived indices; results report timeline indices. On a shared timeline, re-base the video first, then its ANC/fastmeta, which adopt the new
offset. A stop plus
`start(MTL_AT_INDEX 0)` fails with `START_IN_PAST`, because M(0) has passed; discard is the verb.

## 11. RX timing

Start and arm (IGMP join at the first start, ARMED = joined and discarding) are in [contract.md](contract.md); the tasklet hook that enforces the due time is in [engine.md](engine.md).

### 11.1 Timeline binding and the media clock

- An RX session binds the **epoch** timeline (default) or an **AT_TAI** timeline (an explicit anchor, shareable with TX sessions of the instance). AT_START is `-MTL_EINVAL` for RX: a receiver has no lead and no start instant to resolve it with.
- `rx.mediaclk`: `MTL_MEDIACLK_DIRECT` (RTP on the PTP timescale) or `MTL_MEDIACLK_SENDER` (`a=mediaclk:sender`: no TAI relation; only `rtp` and arrival times are valid); `AUTO`, from the sender reports, is Phase 7.
- `rx.rtp_offset`: the SDP `a=mediaclk:direct=<offset>` in ticks (default 0). ST 2110 mandates 0, but AES67 and TR-03-era gear may signal another value.
- On a DIRECT stream `|arrival − media| > 1 s` posts `MTL_EVENT_RX_TIMEBASE_SUSPECT`.

### 11.2 Media time and media index from RTP

```text
rtp_u        = RTP unwrapped to 64 bits, nearest wrap to the arrival time, after subtracting rx.rtp_offset
               (wraps: 13.26 h at 90 kHz, 24.86 h at 48 kHz, 12.43 h at 96 kHz, 27.05 h at 44.1 kHz; window ±half)
media_tai_ns = floor(rtp_u × 1e9 / R)                              (the RTP instant)
media_index  = ceil((rtp_u + 1 − T0·R) / (P·R)) − 1                = the largest k with floor(M(k)·R) ≤ rtp_u
media_phase  = rtp_u − floor(M(media_index)·R)                    (ticks; 0 for a sender on the grid)
```

- `media_index` is the **exact inverse** of `RTP = floor(M × R)`: a sender on the grid gets back exactly its k; a sender with phase φ < P maps to the slot it falls in.
- The obvious `floor((media − T0)/P)` is wrong by one whenever the sender truncated a fractional tick: half the frames at 59.94p, every second field at 1080i59.94, 3 of 4 frames at 23.976p. Checked for 59.94p, 1080i59.94, 23.976p, 119.88p, 48 kHz and floor-aligned 44.1 kHz, k = −50…4999: 0 mismatches for the inverse, 2525 per 5050 units for `floor` at 59.94p (G-76).
- Audio: P·R = 1, so `media_index` = `rtp_u − floor(T0·Fs)`, the first sample's index.
- ANC and fastmeta carry their video's RTP, so their `media_index` is the video frame or field index. `MTL_UNITF_SECOND_FIELD` marks a second field from the stream.
- On the unit: `media_tai_ns` with `MTL_UNITF_TAI_VALID`, `media_index` with `MTL_UNITF_INDEX_VALID`, raw `rtp`. Arrival time never replaces media time (G-25).

### 11.3 Presentation and link offset

`presentation_tai_ns = media_tai_ns + rx.link_offset_ns` (ST 2110-10 §7.8, `D_LO`; `mtl_rx_detail` with `MTL_RXF_PRESENTATION`).

- A unit with `delivered_tai_ns > presentation_tai_ns` carries `MTL_RXF_LATE_FOR_PRESENTATION` and counts in `rx.units_late_presentation`: the link offset is too small for this sender and network.
- `info.tolerated_skew_ns` reports the tolerated 2022-7 path differential (from buffer and slot sizing); a warning is reported when `rx.link_offset_ns < rx.skew_budget_ns`.
- **Scheduled delivery** (holding units until presentation) is later (Q-TIME-12): v1 reports the value and apps schedule. `MTL_LINK_OFFSET_AUTO` (the measured minimum) is an option value.

### 11.4 Starting receivers together

`mtl_session_start(rx_array, n, when, NULL)` with `MTL_NOW`, `MTL_AT_TAI t` or `MTL_AT_INDEX k`:

- flow rules and IGMP joins of every session are installed before any is RUNNING;
- every session discards units whose media time < t (`rx.units_before_start`; for a SENDER stream its arrival time is compared instead, and the first delivered unit says so in its flags) and delivers units with media ≥ t;
- so all begin at one media instant, whichever sender's packets arrive first. `MTL_AT_INDEX k` means t = M(k) of the first video session (else `s[0]`);
- the sessions of one RX start must carry the same `rx.link_offset_ns`, else `-MTL_EINVAL`. A common link offset is configuration, not scheduling, so it is not deferred (IPMX TR-10-1 §11.2 makes link offset a controllable attribute for this).

A flow change `mtl_session_update(..., MTL_UPDATE_FLOWS, &when)` with `MTL_AT_TAI t` takes the units whose RTP-derived media time is ≥ t on the new flow and the earlier ones on the old (G-75).

### 11.5 Duplicates, stale units, gaps and relock

- A packet whose RTP equals the unit being assembled merges into it: 2022-7 copies, and PsF segments, which share an RTP.
- A packet for an already delivered or older unit is stale: counted (`rx.pkts_stale`, `rx.units_stale`, `leg.pkts_late`), never delivered.
- **Forward gaps.** The next delivered unit carries `units_missing_before = n` (by index difference, `mtl_rx_detail`), the RX twin of `slots_skipped_before`. A forward jump of more than one horizon of media time is flagged `MTL_UNITF_DISCONTINUITY` instead. `unit.missed_before` counts units the pool could not take.
- **Relock.** A sender restart with a backward RTP produces stale units forever unless the receiver relocks. Rule: after stale packets of **3 distinct, increasing RTP values in a row** (a new regular sequence behind the old one), the session relocks; the next complete unit is delivered with `MTL_UNITF_DISCONTINUITY` and `media_index` may go backwards (`rx.discontinuities`
  counts). This replaces today's implicit per-packet rule (20 consecutive redundant errors on every port, `st_rx_video_session.c:1188-1196`, `st_header.h:81`).
- A receiver does not seek; it follows the sender.

### 11.6 A/V alignment on receive

A monitor receives the 59.94p video, 48 kHz audio and ANC of one programme: all three on the epoch timeline, started together with one `rx.link_offset_ns = D`.

- `video.media_index = k` is the frame number since the epoch, and its ANC unit has the same k: **ANC ↔ video is index equality**.
- `audio.media_index = s0` is the unit's first sample. **The sample contemporaneous with frame k is `s = ceil(k × 48000 × 1001/60000) = ceil(800.8·k)`**, exact on the epoch.
- `mtl_rx_align(&video_unit, &audio_unit, sample_rate, &sample_offset)` (AS) returns `s − s0`, the offset into that audio unit (negative when the frame starts before the unit, ≥ its sample count when after). Return 0 = exact (both indices valid, video phase 0); 1 = within one sample (from `media_tai_ns`); < 0 = not alignable (SENDER streams).
- `presentation = media + D` for all three: a renderer that presents at presentation time is in sync, whatever each sender's L and the packet arrival order.

### 11.7 Due time, completion and 2022-7 skew

Today a unit completes only when full (`st_rx_video_session.c:1855`) or when a newer timestamp evicts its slot (`rv_slot_by_tmstamp`, `:1214-1221`). The unified RX adds a due time, checked every iteration by the RX hook (Phase 1):

```text
due = arrival_first (earliest leg) + unit_period + rx.flush_offset_ns
      capped at presentation_tai_ns when rx.link_offset_ns is set
rx.flush_offset_ns absent = rx.skew_budget_ns with two legs, 1 ms with one leg
rx.skew_budget_ns absent  = 10 ms (ST 2022-7 class A)
```

- `unit_period` covers the unit's own packet spread: video, last packet ≈ first + RACTIVE·TFRAME (< TFRAME); audio, `audio.unit_samples`/Fs (≈ 10 ms); ANC and fastmeta, TFRAME.
- The due time is keyed on the **first packet's arrival**, not on media time, so it works unchanged for CAPTURE senders with L ≥ 1 (arrival ≈ M + 17–34 ms) and for SENDER streams.
- At the due time, or on a force-complete (stop DRAIN, `mtl_session_discard`), a receiving unit completes: with `rx.incomplete = MTL_RX_DELIVER` (default) it is delivered as `MTL_RX_INCOMPLETE` with its missing ranges (`mtl_rx_get_missing`); with `MTL_RX_DISCARD` it is dropped and counted. A newer RTP still completes the older unit early; whichever comes first wins (G-82).
- With no packets, the waker sleeps at most 1 ms, and signal loss is reported after `rx.signal_timeout_ns` (absent = `max(4 × unit period, 20 ms)`) by `MTL_EVENT_RX_SIGNAL` and `MTL_STATUS_RX_SIGNAL`.
- 2022-7 receiver classes (ST 2022-7:2019 Table 1; PD limit below / at or above 270 Mb/s): A low-skew, intra-facility, ≤ 10 ms; B moderate-skew, short haul, ≤ 50 ms; C high-skew, long haul, ≤ 450 ms / ≤ 150 ms; D ultra low-skew, physical-layer LAN redundancy, ≤ 150 µs. Red/blue fabrics are normally specified against class A, and MTL video RX already tolerates about (slots − 1) × TFRAME. Both
  legs carry identical RTP headers, which is one more reason RTP comes from media time only ([prior-art.md](prior-art.md) §2.8).

### 11.8 RX timing fields

On the unit: `rtp`, `media_index`, `media_tai_ns`, flags. `mtl_rx_get_detail(s, lease, &d, size)` (DP) for a held unit:

- `arrival_first_tai_ns[leg]`, `arrival_last_tai_ns[leg]` per 2022-7 leg: `MTL_RXF_HW_ARRIVAL` with `MTL_INSTANCE_HW_TIMESTAMP` on a port whose PMD has the RX timestamp offload (`mt_dev.c:2422-2440`), else processing time (today taken on port P whatever the RX port, `mt_ptp.c:1635-1641`);
- `presentation_tai_ns` (media + link offset), `delivered_tai_ns` (when the unit became ready), `media_phase_ticks`, `units_missing_before`; completeness: `pkts_expected`, `pkts_received[leg]`, `pkts_recovered`, `missing_ranges`.

Latency = `arrival_first − media` (sender offset plus network; JT-NM expects [0, 1 ms] for playback video); it is not stored. With `rx.timing_parser`, `mtl_rx_get_timing(s, lease, leg, &t, size)` returns the ST 2110-21 measures per leg (compliance narrow/wide, CINST, VRX, FPT, latency, RTP
offset and delta; audio DPVR, IPT, TSDF).

Without NIC RX timestamps (a VF, or a PMD without the offload) the parser runs on processing time, as today: packets inside an RX burst cannot be timed at arrival and are skipped and counted in `tp.untrusted_pkts` (`st_rx_video_session.c:1546-1557`); the unit's `mtl_rx_detail` then lacks `MTL_RXF_HW_ARRIVAL`, which says the measures are estimates. The twins that use the parser
(`st20p_test.cpp`, `noctx/testcases/st20p_ptp_epoch_recovery_tests.cpp`) run on VFs, so a NIC-only parser would report nothing there. **Open:** Q-OBS-3 says "NIC-timestamped packets only"; port first (D-98) keeps processing time, so Q-OBS-3 needs restating, or the regression listed in migration.md (OI-29, decisions.md §5).

The parser's known deviations (gapped TRS for W and NL, per-frame CINST reset, TROFF ignored, double-precision TAI, as EBU LIST) are listed in [prior-art.md](prior-art.md) §2.5.

## 12. Time steps and step policies

The published time base never steps except at a declared `TIME_STEP` (G-62). What a step does depends on what the anchor means, so it is a timeline property, `mtl_timeline_config.step_policy`:

| Policy | On a step of Δ |
|---|---|
| `MTL_STEP_REANCHOR` | `T0 += Δ`; every session on the timeline emits one implicit DISCONTINUITY; RTP jumps with the clock; nothing is dropped for the step itself |
| `MTL_STEP_KEEP` | media indices stay; the late policy applies to units whose deadlines passed (forward step) |
| `MTL_STEP_DEFAULT` (0) | KEEP on the epoch timeline (it cannot re-anchor); REANCHOR on created timelines |

**Open** (OI-31, decisions.md §5): the default also re-anchors AT_TAI timelines, but RX sessions bind them (§11.1) and separate processes rely on their T0 agreeing (§10.4); a re-anchor shifts the receiver's `media_index` against the sender's. Revision 3 kept AT_TAI timelines anchored.

**AUTO sessions after a step.** The AUTO cursor reacts, not the timeline:

- **Forward step**: queued AUTO units whose slot is now past are RESLOT forward; each result carries `MTL_TXR_RESLOTTED`, and the full record `slots_skipped_before`.
- **Backward step Δ ≤ horizon**: AUTO never stamps a smaller index; the next unit gets `last + 1`, now Δ further away, so the stream pauses for Δ with continuous RTP.
- **Backward step Δ > horizon**: `TIME_STEP` is posted and the session re-anchors **its AUTO cursor only**: the next unit takes the current slot, a smaller index, with a discontinuity. Timelines and INDEX/TAI sessions on them are unchanged. This is the one case where AUTO emits a backward RTP jump; receivers relock (§11.5).

Also: a pending `mtl_session_update` fails with reason `TIME_STEP`. Today the built-in PTP switches the time function from UTC to the PHC when the master is first seen (`mt_ptp.c:1062-1067`), a ≈ 37 s step for running sessions; G-62 tests exactly that with `mtl_debug_inject(TIME_STEP)`.

## 13. Timing without PTP (Phase 7, later)

IPMX has four clocks: the common reference (PTP when present), each device's internal clock, a stream's media clock and its RTP clock. Without a grandmaster the internal clock runs free; an async source (an HDMI input at its own rate) has an RTP clock that follows the source, signalled
`mediaclk:sender`. The declarations exist in the headers; the implementation is Phase 7 (D-98).

| Piece | Rule |
|---|---|
| `MTL_TIME_SOURCE_FREERUN` | seeded once from the system clock, never stepped, ESTIMATED; state FREERUN, which health counts as ready. `SYSTEM_TAI` follows NTP steps and stays for other uses |
| `time.freerun_slew_ppm` | FREERUN follows `CLOCK_TAI` by frequency only, bounded, never stepping. Without it a 10 ppm crystal drifts ≈ 0.86 s a day, past the 0.1 s the IS-05 test suite allows for absolute activations |
| AUTO at runtime | until Phase 7 AUTO chooses once at open and ends in `SYSTEM_TAI`. From Phase 7 its last step is `FREERUN` and it moves to a disciplined PHC or `CLOCK_TAI` when one appears (posts `TIME_STATE` and `TIME_STEP`; the Info Block ts-refclk changes from `localmac=` to `ptp=`); when the source is lost, holds over, then by `time.fallback` (default FREERUN) runs free without a step |
| `MTL_MEDIA_SENDER` | RTP follows the source (below); without it, an async source at +50 ppm on a 59.94 session drops a frame every 5.6 minutes under NEAREST |
| `MTL_SUBMIT_SENDER_TIME` | inline processors keeping the input's timing (TR-10-1 §9): RTP and the sender report's NTP come from the received unit (`unit.rtp`, `unit.media_tai_ns` with `SENDER_TIME`); launch = submit + `min_tx_delay_ns` on the instance clock |
| receivers | units of one sender align through its clock even without PTP: `mtl_rx_align` works on `SENDER_TIME` values of one sender. `rx.mediaclk = AUTO` compares the Info Block's ts-refclk with the instance's grandmaster and uses the reports when they differ |

Which settings each source case takes:

| Case | Time source | Media mode | Source kind | SDP mediaclk / ts-refclk |
|---|---|---|---|---|
| file playout or test pattern, PTP present | `PHC` or `PTP_BUILTIN` | `INDEX` or `AUTO` | PLAYBACK | `direct=0` / `ptp=` |
| file playout, no PTP (TR-10-9 §9: a sync sender) | `FREERUN` | `INDEX` or `AUTO` | PLAYBACK | `direct=0` / `localmac=` |
| genlocked capture locked to PTP | `PHC` | `TAI` (NEAREST) or `INDEX` | CAPTURE, GATEWAY | `direct=0` / `ptp=` |
| HDMI capture at its own rate, PTP or not | any | `SENDER` | CAPTURE, GATEWAY | `sender` / `ptp=` or `localmac=` |
| inline processor keeping the input timing | any | `SENDER` + `MTL_SUBMIT_SENDER_TIME` | GATEWAY | as the input |

For a sync sender without PTP, R5's "TAI ns" is the internal clock with `MTL_TIMEF_ESTIMATED`, and `RTP = floor(M × rate)` on the epoch timeline is exactly TR-10-1 §8.6. SENDER mode is for async sources: a +50 ppm source on a 59.94 session drifts 0.83 µs per frame, so NEAREST drops one frame every 1/50 ppm = 20 000 frames (5.6 min); an audio source at −1000 ppm gets 48 samples per second
absorbed or inserted.

`MTL_MEDIA_SENDER`: `media_tai_ns` is the source's own sampling instant on the instance clock, never snapped.

```text
RTP  = RTP0 + floor(k × period × rate)     RTP0 = floor(M0 × rate) at the first unit or after a DISCONTINUITY
k   += max(1, round((M − M_prev) / period)) per unit       (a missed VSYNC skips one period; no drift builds up)
launch = M + min_tx_delay_ns on the nominal-period schedule;  an overlapping unit is DROPPED/WOULD_OVERLAP
```

`MTL_AT_INDEX`, `MTL_SESSION_RX_BY_INDEX`, `tx.precede` and `mtl_index_at()` are `-MTL_EINVAL` on such a session; `MTL_INFO_MEDIACLK_SENDER` is set in the info. From Phase 7, AUTO's order in a pod ends in FREERUN, so a pod without PTP is an IPMX sender with `localmac=`, not an error. Details: [nmos-ipmx.md](nmos-ipmx.md) §12.

## 14. The legacy timing surface

### 14.1 Today → unified

| Today | Unified |
|---|---|
| `*_FLAG_USER_TIMESTAMP` + `timestamp`/`tfmt` | `media_mode = TAI` (or INDEX) |
| `*_FLAG_USER_PACING` (TAI, snapped to the nearest epoch) | `media_mode = TAI` with derived launch; the "send at t" reading is `MTL_SUBMIT_NOT_BEFORE`. The legacy shim keeps nearest-epoch; migrated apps' TX moves by TROFFSET − VRX0·TRS (604–619 µs at 1080p59.94) (Q-TIME-21) |
| `*_FLAG_EXACT_USER_PACING` | `MTL_SUBMIT_EXACT` (non-compliant) |
| `rtp_timestamp_delta_us` | `media_time_offset_ns` (ns) for the compliance trim; `tx.index_offset` for lip-sync; `MTL_SUBMIT_RTP_TS` for verbatim copies |
| `*_FLAG_DROP_WHEN_LATE` (inert without USER_PACING) | `tx.late_policy = MTL_LATE_DROP` (the INDEX/TAI default, every mode) |
| `notify_frame_late(priv, epoch_skipped)` (three units, wrong `priv` on one path) | per-unit status, `margin_ns`, `mtl_tx_result_full.slots_skipped_before`; counters |
| `ST_EVENT_VSYNC` | `mtl_tx_next_slot`; `MTL_EVENT_EPOCH_TICK` (opt-in) |
| `st20_tx_get_pacing_params` | `mtl_session_info` and `info.*` stats (troffset, trs, vrx, sender type, pick-up lead, `min_submit_lead_ns`, L, TSDELAY, pacing class, `max_slot_delay`) |
| RX `timestamp` / `receive_timestamp` / `timestamp_first_pkt` | `rtp`, `media_tai_ns`, `media_index`, `arrival_first_tai_ns[leg]` |

The full field and call map is in [migration.md](migration.md).

**Mixed legacy and unified sessions.** A legacy ST20 session with the default TX-cursor RTP differs by +54.4…+55.7 ticks at 1080p59.94 from any epoch-based session (unified, or legacy ANC/audio). A legacy ST40 session with a unified ST20 session agrees within ±1 tick (legacy ANC stamps its epoch, `st_tx_ancillary_session.c:420-427`, `:462`; only the rounding differs). Every
session of one programme must use the same RTP rule.

### 14.2 Phase 0.5: the legacy `st_timeline_*` helper

R05 §6 shows that USER_TIMESTAMP + USER_PACING already gives exact RTP on ST20, ST30 and ST40 sessions; the legacy API lacks only the anchor math, which RxTxApp does in 30 lines of `double` (`tests/tools/RxTxApp/src/rxtx_app.c:673-703`). Phase 0.5 ships it as a public, pure helper (`include/st_timeline.h`: no instance, no allocation, no locks; the same code as E2 and the
`mtl_sync.h` helpers), plus two pipeline flag fixes.

```c
struct st_timeline_member {
  uint32_t kind;                /* ST_TL_VIDEO, ST_TL_AUDIO, ST_TL_ANC, ST_TL_FASTMETA */
  uint32_t interlaced;          /* video, anc, fastmeta: 1 = the index counts fields */
  uint32_t rate_num, rate_den;  /* video, anc, fastmeta: the frame rate; audio: Fs/1 */
  uint32_t samples_per_packet;  /* audio: S */
  uint32_t reserved;
};
struct st_timeline {            /* a plain value, caller-owned; T0 = n × G, exact */
  uint64_t grid_num, grid_den;  /* G (§3.4) */
  int64_t n;                    /* T0 / G; valid after st_timeline_anchor* (flag ST_TL_ANCHORED) */
  uint32_t member_count, flags;
  struct st_timeline_member member[8];
};
struct st_timeline_unit { uint64_t media_ns, legacy_ts_ns; uint32_t rtp, second_field; uint64_t epoch; };

int st_timeline_init(struct st_timeline* tl, const struct st_timeline_member* m, uint32_t count);  /* G; -EINVAL above the 1 s cap */
int st_timeline_anchor(struct st_timeline* tl, uint64_t not_before_tai_ns, uint32_t k_member, int64_t k);  /* §3.3 */
int st_timeline_anchor_at(struct st_timeline* tl, uint64_t tai_ns);                       /* first grid instant ≥ tai_ns */
int st_timeline_unit(const struct st_timeline* tl, uint32_t member, int64_t index, struct st_timeline_unit* out);
int st_timeline_index_at(const struct st_timeline* tl, uint32_t member, uint64_t tai_ns, int64_t* index);   /* as mtl_index_at: the unit containing tai_ns */
int st_timeline_audio_first(const struct st_timeline* tl, uint32_t member, uint64_t start_tai_ns, int64_t* sample);
/* st_timeline_unit: media_ns = floor(M × 1e9); rtp = floor(M × R) mod 2^32; second_field; epoch = floor(M / P);
   legacy_ts_ns = ceil(floor(M × R) × 1e9 / R) */
```

- **`legacy_ts_ns` makes today's rounding exact.** Legacy USER_TIMESTAMP with `tfmt = TAI` computes RTP with `st10_tai_to_media_clk(ts)`, an integer round-to-closest (`st_fmt.c:986-993`). For `ts = ceil(X·1e9/R)` with `X = floor(M·R)`, `ts·R/1e9 ∈ [X, X + R/1e9)`, which rounds to X for every R ≤ 96 kHz: 0 mismatches in 800 000 random cases at 90 k, 48 k, 96 k and 44.1 k. The
  same value drives USER_PACING (within one tick of M, so "nearest epoch" is unit k's slot). `rtp_timestamp_delta_us` must be 0. `legacy_ts_ns` is fed as `timestamp` with `ST10_TIMESTAMP_FMT_TAI`; `rtp` is for `ST10_TIMESTAMP_FMT_MEDIA_CLK` users. `not_before_tai_ns` is the app's `now + lead + preroll`; the helper never reads a clock. It is unit-tested against the exact oracle at every rate
  of the §3.4 table.
- **Per essence.** ST20/ST20P, ST22/ST22P: `USER_TIMESTAMP | USER_PACING`, `timestamp = legacy_ts_ns` of unit k (pass the frame rate and `interlaced = 1` to the helper). ST30: `USER_TIMESTAMP | USER_PACING`, a frame of n packets gets the `legacy_ts_ns` of its first sample; frames hold whole packets (no carry); start at `st_timeline_audio_first`. ST30P: needs the
  new `ST30P_TX_FLAG_USER_TIMESTAMP`. ST40/ST40P: ANC member k uses video index k; ST40P must honour USER_TIMESTAMP without USER_PACING (today it is silently ignored).
- **Members.** Up to eight per timeline, of the four kinds above. `st_timeline_index_at` rounds down like `mtl_index_at` (one implementation, §3.5); the first unit at or after t is the index, or the index + 1 when t is past its start (§10.4). The revision-3 draft returned the first index with M ≥ t.
- **Known legacy limits**, not fixed in Phase 0.5: ST30 launches exactly at the RTP instant (a zero launch margin); every session of the programme must use USER_TIMESTAMP; the legacy ST20 default RTP must not be mixed in; legacy late handling stays.
- **Guarantee** G-106: for identical members, anchor and indices, `st_timeline_unit().rtp` equals the unified engine's wire RTP, and the unified `mtl_timeline_info.t0_tai_ns` equals the helper's `n × G`. **Exit:** the A/V/ANC example of §10.2 runs on the legacy API in RxTxApp and every RTP is verified by the oracle.

## 15. Engine changes that carry these rules

The changes, their code locations and their legacy defaults are owned by [engine.md](engine.md) and [implementation-plan.md](implementation-plan.md). The timing ones, in track order E2 → E4/E8/E9 → E1/E3 → E11/E12 → E5/E6/E7/E10/E13; bugfixes are on for the legacy API, wire-visible changes are opt-in there and default in the unified API (M7):

E1 media time and launch carried separately (§4.2); E2 exact epoch math, floor and the T0 formula (§3); E3 admission at pick-up, bounded SEND_LATE, source kinds, snapping, discard REBASE (§4.3, §5.2, §6); E4 enqueue and HW observed times (§6.6); E5 linear W/NL schedule, TLINE/2, pre-fill cap
(§5.1); E6 audio (§8); E7 ANC (§9); E8 RX timing (§11); E9 time sources and the published time base (§2, §12); E10 cvideo rate modes (§5.3); E13 fastmeta (§9.3).

Phase 3 builds the L2 timing core (modes, timelines, start arrays, RX timing) over this track; Phase 1 carries the ST20 path and the RX due-time hook.

## 16. Timing tests

### 16.1 Guarantees

The full guarantee list is [implementation-plan.md](implementation-plan.md) §8.2; the methods of the timing guarantees not given here are [requirements.md](requirements.md) §4.3. Tiers: U unit (null backend, test clock), UB unit with the engine's packets, I integration on VFs, M measured with a capture. P = pass/fail, BE = best effort.

| ID | Guarantee | Tier |
|---|---|---|
| G-17, G-18 | every time field names its clock, unit and validity (zero is valid when flagged); requested, resolved, scheduled, enqueued and observed times are separate fields | U |
| G-19 | RTP = `floor(M × R) mod 2^32` exactly, every essence, no cumulative drift | U, UB |
| G-20 | default video media time is the frame epoch; ST20 and ST40 of one frame carry equal RTP | UB |
| G-21 | audio RTP identifies each packet's first sample; submission boundaries change no packet | UB |
| G-22 | interlaced: each field its own M, `M(second) = M(first) + TFIELD`, RTP by G-19; launch includes TLINE/2 | UB |
| G-23 | a start array starts all or none | U, I |
| G-24 | under DROP and bounded SEND_LATE, one unit's lateness shifts no later unit of any session | UB |
| G-25, G-26 | arrival time never replaces media time; an invalid exact launch fails explicitly, never falls back | UB |
| G-27 | the launch schedule matches ST 2110-21 for the granted sender type within the accuracy profile; a Phase 2 exit criterion | M |
| G-28 | a late unit gets the policy's outcome and a result with margins | UB |
| G-53 | submitting before the hint's deadline is ON_TIME in the hinted slot; before `M − min_submit_lead_ns` never `TOO_LATE` | U, UB |
| G-59 | no packet before its media time; `NOT_BEFORE t` never before t | UB, M |
| G-61 | ST40 keep-alive per frame or field without ANC; ST41 at least every 500 ms | UB |
| G-62 | a step applies the step policy and posts `TIME_STEP`; no session drops forever after a forward step | U, UB |
| G-63 | a lazily anchored timeline is never resolved in the past | U |
| G-66 | cvideo follows `rate_mode`; oversize is `-MTL_ENOSPC` at submit | UB |
| G-67 | RX honours `rx.rtp_offset`; SENDER streams mark media invalid; large offsets post `RX_TIMEBASE_SUSPECT` | UB |
| G-68 | the T0 formula: `T0·R` integer for every member rate, whole frames for video/ANC/grid fastmeta, unit k at ≥ `now + lead + preroll` | U |
| G-75 | flow updates at a media index or instant switch on every leg at that unit | U, I |
| G-76 | RX on EPOCH or AT_TAI reports the exact inverse `media_index`; RX starts arm together | U, UB |
| G-82 | a due RX unit is force-completed within one iteration; with no due time the waker sleeps ≤ 1 ms | UB |
| G-85 | CAPTURE + NEAREST: collisions are `DUPLICATE_SLOT`, skips follow the underrun policy, never a permanent drop | U, UB |
| G-86 | audio forward gaps keep the packet grid and are padded with silence (`samples_padded`), overlaps are dropped (`samples_dropped`); only DISCONTINUITY re-phases | UB |
| G-94 | preroll extends the lead; the horizon counts from S; a stranded queue fails the start atomically | U |
| G-104 | with video slot delay L_v, ANC k has RTP_v(k) and lies in the window of frame `E⁻¹(M(k)) + L_v`, keep-alive included | UB |
| G-105, G-106 | two processes on the epoch with a grid-aligned `t_start` emit the RTP of one process; the Phase 0.5 helper equals the engine and `legacy_ts_ns` puts that RTP on the wire | U, UB |

Methods that matter:

- **G-19**: an exact 128-bit rational oracle at every rate including 1001 families; 10^7 units at today's TAI, across the 2^32 wrap (13.26 h at 90 kHz, 27.05 h at 44.1 kHz), 44.1 kHz with 1001 rates, ST41 at 90 kHz with rational unit rates, TAI past 2^61 ns. Negative indices (audio priming) are checked in the oracle at every rate, and in the engine only on the epoch timeline and AT_TAI
  timelines with M(k) ≥ S.
- **G-22** and **G-68**: the field increments of §4.2; k = 0…100 over the eight configurations of §3.3.
- **G-85**: a 60.0 Hz source into a 59.94 session for a simulated 24 h on the test clock, and any source within ±1 %: the fraction of units sent is ≥ 1 − the rate error and every sent RTP is on the grid.
- **G-86**: drop one video frame's audio (800.8 samples) repeatedly; every later packet's RTP stays on the original grid; packet p covers `[p·S, (p+1)·S)` with RTP `floor(T0·Fs) + p·S`; no submitted sample outside an overlap is dropped.
- **G-94**: preroll a full pool, then start with a horizon one unit too short.

### 16.2 Oracle and contract

Adopt the method of the pacing contract draft (`doc/user-pacing-timestamp-contract.md`, Level 3 and Appendix A; a separate draft, not part of this baseline): compute every expected value from the anchor with exact arithmetic, check every packet (not only packet 0), never infer anchors from captured output. This design
replaces several of the contract's selection rules, so the contract needs an update before the engines track changes the wire. The differences:

| Contract says | This design | Question |
|---|---|---|
| user pacing = nearest slot, may be before the request | media time primary; `NOT_BEFORE` never sends early; the legacy shim keeps nearest-epoch | Q-TIME-21 |
| later audio buffers must equal the grid point or be rejected | contiguous unless DISCONTINUITY; absorb ±one packet for CAPTURE/TAI; gaps keep the grid | Q-TIME-6 |
| 1 s horizon | configurable, default 1 s, from the resolved start instant | Q-TIME-3 |
| ns rounding ties choose the later ns | open: the code rounds ties down (`st_muldiv_u64_round_closest`, `st_fmt.c:951`); pick one and pin it | Q-TIME-15 |
| too-early or insufficient-lead requests are rejected and advance no RTP | late at pick-up → accepted, then DROPPED (slot consumed) or RESLOT; only synchronously knowable cases are rejected at submit | Q-TIME-23 |
| audio packet 0 TX = the RTP instant | TX = M + D_a, D_a small | Q-TIME-11 |
| ST20 packet offsets `index × interval`, no TLINE/2 | linear TRS for NL/W; `+TFRAME/2 + TLINE/2` for the second field | Q-TIME-17 |
| `rtp_anchor_ticks` includes a delta | RTP offsets are forbidden; whole-unit `tx.index_offset`, or `media_time_offset_ns` | Q-TIME-5 |
| the user timestamp must be a non-zero TAI value | zero is valid when flagged (G-17); the media mode, not the value, says what is meant | — |
| audio RTP anchored at the first TX request in user mode | RTP from the timeline's sample index (§8) | — |
| `receive_timestamp` = 0 when packet 0 is missing | validity flags (`MTL_UNITF_*`, `MTL_RXF_*`) | — |
| a `tx_queue_available_time` term in the scheduling cutoff | bounded `MTL_LATE_SEND_LATE` (`WOULD_OVERLAP`) | — |
| scope ST20/ST30/ST40 | ST22 `rate_mode` and ST41 at 90 kHz too; the contract needs rules for them | — |

Agreements kept: "RTP describes the content, pacing chooses TX"; floor (the legacy API keeps today's rounding unless the opt-in flag is set, Q-TIME-15); every packet of a unit carries one RTP; `scheduled_first = TVD − VRX0·TRS`; the ST40 target inside the window (`anc.target_delay_ns`, Q-TIME-22); exact ST40 requests outside the window rejected; dropped accepted units leave an RTP gap.

### 16.3 Substrate and budgets

- Null backend `null:<n>`: units complete at their scheduled launch on the instance clock; with the test clock (`mtl_test_clock`, `mtl_test_clock_advance`) a run is deterministic (G-92).
- `mtl_debug_inject(obj, MTL_FAULT_TIME_STEP, {step_ns})` and `MTL_FAULT_TIME_LOST` drive the step and holdover cases; debug builds only (`-Denable_debug_api=true`).
- Performance: the frame-start error of the published time base against a direct PHC read is measured by spike S7 and becomes a gate; ST 2110-21 narrow compliance under stress shows no regression versus legacy per NIC × pacing class (G-27, Phase 2 exit). EBU LIST tolerates ±1 tick (it uses `round()`); the oracle does not.

## 17. Open items

| Item | Where decided |
|---|---|
| M3: media time primary + derived launch, source kinds | [decisions.md](decisions.md) |
| M4: live ANC window of the transmitted frame | [decisions.md](decisions.md) |
| M7: wire-visible engine changes opt-in on the legacy API | [decisions.md](decisions.md) |
| Q-TIME-15 ns rounding ties; Q-TIME-21 legacy USER_PACING mapping; Q-TIME-4 REPEAT_LAST; Q-TIME-12 scheduled RX delivery; Q-TIME-20 timecode and cadence helpers | [questions.md](questions.md) |
| Q-OBS-3 / OI-29 timing parser on processing time (§11.8); OI-31 step policy of AT_TAI timelines (§12); OI-34 missing statuses and reasons (§4.3, §7.1) | [decisions.md](decisions.md) §5 |
| Open: audio `MTL_PTIME_80US` (3.84 samples at 48 kHz): S = 4 with an 83⅓ µs packet time (as PR #1770), or reject at create (§8). Revision 3 made S an integer config field; the headers carry the legacy `audio.ptime` enum | maintainer, with E6 |
| timecode (ST 12-1 from media time via `currentUtcOffset`, ST 2059-2 SM TLVs, drop-frame at 1001 rates; RP 188 / ST 12-2 into ST40) | later, helper |
