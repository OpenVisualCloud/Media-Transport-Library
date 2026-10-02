# Review C2 — timing, pacing and standards correctness

| | |
|---|---|
| Lens | Broadcast IP timing: ST 2059-1/-2, ST 2110-10/-20/-21/-22/-30/-31/-40/-41, ST 2022-7, AES67, RFC 3550/4175/8331/9134, EBU LIST / JT-NM Tested |
| Date | 2026-09-29 |
| Reviewed | `06-timing-pacing-and-sync.md` in full; timing parts of 00, 01 §5, 03 §3.3/§5, 07 §1.1, 08, 09, 10 §6–7, 12, 13 §3/§7, OPEN-QUESTIONS Q-TIME-*; R05, R12; `doc/user-pacing-timestamp-contract.md` |
| Baseline | `main` @ `545a266a`; all code citations were read with `git show HEAD:<path>` |
| Method | Every number in 06 §3.3 and §10.1–10.2 was recomputed with Python `fractions.Fraction`. Common grids G were computed as the smallest G for which G/TFRAME, G×90000 and G×Fs are all integers |
| Labels | **[verified]** read in code, or computed exactly; **[inferred]** my derivation from a standard clause as quoted in R12, or engineering judgement; **[unknown]** could not establish from what was available (primary SMPTE texts were not re-read for this review) |

Findings are ordered by severity: 1 blocker, then majors, then minors. The arithmetic
summary is in §A at the end.

## Blocker

### 1. Live capture does not work with the specified defaults, and L ≥ 1 is not reconciled with ST 2110-10/-40 or JT-NM

- **Severity:** blocker. It changes header fields (`tx_slot_delay`, the TAI-mode snap rule
  and `audio_launch_offset` semantics), and 06 asserts that the model covers live capture.
- **Where:** 06 §4.1 (TAI mode "for capture with a hardware timestamp"), §5.1 (`N = E⁻¹(M) + L`, "0 for playout, ≥ 1 for live capture", ST40 "video slot of the associated frame"), §7.2 (DROP is the default for TAI), §8 (`audio_launch_offset` = one packet time), §11 (`latency_ns`, "JT-NM expects [0, 1 ms]").
- **Issue:**
  - *Defaults drop every live frame.* A camera frame stamped at its sampling instant M is
    complete no earlier than about M + TFRAME (readout), plus processing. With the default
    L = 0 the slot is TVD = M + TROFFSET (637.7 µs at 1080p59.94), so the pick-up deadline
    has passed before the frame exists. The default late policy for TAI mode is DROP, so a
    naive capture app drops 100 % of its frames and gets no hint why. Audio has the same
    problem: launch = M + D_a with D_a = ptime, but a capture packet's last sample exists
    only at M + ptime, so every packet is late. [inferred]
  - *L ≥ 1 fails the JT-NM/LIST checks the design cites.* With L = 1 at 1080p59.94 and
    type N (VRX_FULL = 9, TRS = 3.7074 µs), the first packet leaves at about
    M + 16.683 + 0.638 − 0.033 = **M + 17.29 ms**. LIST reports RTP offset = RTP −
    N_first·TFRAME = **−1501/−1502 ticks**, against a JT-NM pass window of
    `[−1, ceil(57.39)+1] = [−1, 59]` ticks, and latency 17.3 ms against `[0, 1 ms]`
    (R12 §3.4, `st_rx_timing_parser.c` uses the same windows). [verified arithmetic; the
    windows are verified in R12]
    This is standards-legal for a camera (ST 2110-10 §7.6.2 has no ±TFRAME bound, and JT-NM
    says "unless justified"). It is not legal for playback/synthetic sources with L ≥ 2,
    because §7.6.3 says the RTP "shall not exceed ±TFRAME from the most recent N × TFRAME".
    06 §5.1 does not say which of these applies when.
  - *ANC with L ≥ 1.* 06 §5.1 places ANC in "the ANC window of the video slot N+L". R12
    §5.1 defines TFST from `TAD = N × TFRAME + TROFFSET_ANC`. If N there is the frame that
    the ANC RTP denotes (my reading [inferred]; the primary text was not re-read
    [unknown]), then ANC sent L frames later misses the CTM bound `TFST + TEPO + 1 ms` by
    about 16.683 − 1 − TEPO ≈ 15 ms at 1080p59.94, and misses LLTM (TD = 118.6 µs) by
    more. The alternative, ANC RTP ≠ video RTP, breaks -40 §5.4. Either way, "RTP_anc =
    RTP_v" (06 §4.2, G-20) and a compliant window cannot both hold for L ≥ 1.
  - *Integer L cannot express camera phase.* A camera stamps `M = N·TFRAME + φ`, with a
    constant φ (for example mid-exposure). The rule `E⁻¹(M)` is undefined here (floor or
    nearest?). With floor, L = 0 and φ > TROFFSET, the first packet would leave before
    M, so the RTP is "in the future" at the receiver.
  - *There is a standard live model the design omits.* SDI→IP gateways and low-latency
    cameras transmit in the *same* frame period: L = 0, a larger constant TROFFSET
    (< TFRAME, signalled as TROFF), and line-progressive readiness. 06 §9 has progressive
    submission but never connects it to live timing.
- **Suggested change:**
  1. Replace integer `tx_slot_delay` with `min_tx_delay_ns` (D_min). The slot is the
     first N whose first-packet wire time `TVD(N) − VRX0·TRS ≥ M + D_min`. L stays as a
     reported value, and TSDELAY is reported from it.
  2. Add a declared `source_kind` = PLAYBACK | CAPTURE | GATEWAY with these defaults.
     PLAYBACK: D_min = 0, reject L ≥ 2 (§7.6.3). CAPTURE: D_min from the app, ≥ one
     unit; warn in `get_info` that JT-NM default windows will flag it; TSMODE = SAMP.
     GATEWAY: L = 0, TROFFSET from the app, progressive submission.
  3. Make DROP the TAI-mode default only for PLAYBACK. For CAPTURE, fail the first late
     unit loudly (an EQ event `TIMING_INFEASIBLE` carrying the measured shortfall), so the
     app learns the D_min it needs.
  4. For audio capture, the same D_min mechanism replaces `audio_launch_offset` (see #6).
  5. State the ANC rule explicitly once the -40 text has been re-read (OQ below).
- **New open question — Q-TIME-18:** For live ANC with video L ≥ 1, is the ST 2110-40
  window anchored on the frame the ANC RTP denotes, or on the video transmit slot? If
  the former, is live ANC restricted to L = 0 (gateway model), or does ANC carry its own
  transmit-slot RTP (breaking -40 §5.4)? Options: (a) ANC always L = 0 relative to its
  RTP, and video L ≥ 1 is allowed only without an associated ANC session in a group;
  (b) ANC follows video L and is flagged non-compliant; (c) require the gateway model for
  live ANC.

## Majors

### 2. The headline A/V example resolves the timeline anchor at the wrong time

- **Severity:** major (it breaks the answer to the maintainer's question as written).
- **Where:** 06 §3.1 (`NEXT_GRID`: "anchored at the next common grid point after now + lead"), §3.3 ("computes G from the rates of the sessions that use the timeline"), §10.2, §10.3, 10 §6, 03 §5 step 2.
- **Issue:**
  - `mtl_timeline_create(NEXT_GRID)` runs *before* any session exists (10 §6). It
    therefore knows neither the member rates (so it cannot compute G) nor
    `max(min_submit_lead)`.
  - Session creation then takes hundreds of ms to seconds (mempools, flow rules, ARP
    waits of up to 60 s are cited in 00). By `mtl_group_start(AT_MEDIA_INDEX 0)`, T0 is
    likely in the past, so index 0… is late and DROPPED under the example's own
    `late_policy = MTL_LATE_DROP`.
  - §10.3 then says group start "computes the start instant … snapped to the common
    grid". For `AT_MEDIA_INDEX k` there is no freedom: the instant is M(k), which the
    timeline already fixed. §3.1 and §10.3 contradict each other. [inferred]
- **Suggested change:**
  - Make `NEXT_GRID` lazy: T0 stays *unresolved* until the first `mtl_group_start` or
    `mtl_session_start` on the timeline.
  - At that point: `T0 = ceil((now + max_lead + preroll) / G) × G − k × period(k's stream)`,
    with G computed from the members actually attached.
  - `mtl_timeline_get_anchor` returns `-EAGAIN` until then.
  - `AT_MEDIA_INDEX` on an already-resolved timeline fails with `-ERANGE` if M(k) < now +
    lead. It never silently drops the start.
  - Fix 03 §5 step 2, which cites "06 §6"; the common grid is 06 §3.3.

### 3. The common-grid table is wrong for 24p, omits 44.1 kHz, and mixed families give multi-second grids that collide with the 1 s horizon

- **Severity:** major (contains an arithmetic error; undefined behaviour for supported
  formats).
- **Where:** 06 §3.3 table, §3.1, §7.4 (horizon), Q-TIME-3.
- **Issue:** exact results [verified with `fractions`]:

  | Members | Smallest common G | Frames | Note |
  |---|---|---|---|
  | 24p + 48 kHz | 1/24 s | 1 | the table's "1/30 s" row covers 24; 1/30 s = **0.8 frames** at 24p — wrong |
  | 60p + 48 kHz | 1/60 s | 1 | the table gives 1/30 (valid, but twice the start quantum) |
  | 24p + 30p | 1/6 s | 4 / 5 | |
  | 29.97/59.94/23.976/119.88 + 48 or 96 kHz | 1001/6000 s = 166.83 ms | 5/10/4/20 | table correct (15015 ticks, 8008 samples, 16016 at 96k) |
  | **29.97/59.94/23.976/119.88 + 44.1 kHz** | **1001/300 s = 3.337 s** | 100/200/80/400 | 300300 ticks, 147147 samples; 735.735 samples/frame at 59.94 |
  | 24p + 44.1 kHz | 1/12 s | 2 | |
  | 25p + 59.94p (two video families) | **1001/25 s = 40.04 s** | 1001 / 2400 | |
  | 50p + 59.94p | 1001/50 s = 20.02 s | | |
  | any + ST41 at an arbitrary rate (e.g. 1/7 Hz) | unbounded | | |

  - ST 2110-30 lists 44.1 kHz as a "should" rate and MTL supports it (`ST31_SAMPLING_44K`).
    With a 44.1 kHz member, NEXT_GRID can place T0 up to 3.34 s ahead. Pre-roll
    submissions for index 0 then exceed the default 1 s horizon (§7.4) and are rejected
    with `-ERANGE`. [inferred]
  - Mixed video families give 20–40 s start quanta.
- **Suggested change:**
  - Correct the table and add 44.1 kHz rows.
  - Define G as the lcm over *video and ANC* members only. Exclude ST41 always.
  - For audio (or any member) whose rate does not divide the grid: apply
    `floor(T0 × Fs)` and report the sub-tick phase (< 1 sample = 22.7 µs at 44.1 kHz,
    perceptually irrelevant).
  - Cap G (for example ≤ 1 s). Above the cap, fail with `-EINVAL` and give the reason.
  - Make the horizon check relative to "now or the resolved start instant, whichever is
    later".
- **New open question — Q-TIME-19:** Should NEXT_GRID require exact coincidence for
  every member, or only for video/ANC members, with audio floor-aligned (≤ 1 sample
  phase, reported)? Recommendation: video/ANC only, cap at 1 s.

### 4. `MTL_LATE_SEND` cascades, so the stream slides; the claims "nothing slides" and G-24 are false under it

- **Severity:** major.
- **Where:** 06 §7.2 (SEND_LATE "sent as soon as possible"), §10.2 ("video frame 38 keeps its exact media time. Nothing slides"), 13 G-24, 07 §1.1.
- **Issue:**
  - A 1080p59.94 frame occupies ≈ RACTIVE·TFRAME = 16.02 ms of wire time. A frame
    released even 1 ms late overlaps the next slot's TVD. The design gives no rule for
    this, and the contract's `tx_queue_available_time` (Appendix A) exists precisely for
    it.
  - Either unit k+1 waits, and is then late too, so every following unit is LATE: a
    permanent TX slide that T5 forbids. Or both are interleaved, and VRX_FULL (9 packets
    for type N) and CMAX overflow.
  - "As soon as possible" does not say the unit is still paced at TRS. A burst at line
    rate violates -21 grossly.
  - Receivers also discard it. MTL's own RX drops a packet whose RTP is older than every
    active slot (`st_rx_video_session.c:1176-1192`, until `ST_SESSION_REDUNDANT_ERROR_THRESHOLD`)
    [verified]. So a late frame that arrives after frame k+1 has started is lost anyway.
- **Suggested change:**
  - Define SEND_LATE as bounded. The unit is sent paced at TRS from its actual start, only
    if its last packet can complete before `TVD(next occupied slot) − VRX0·TRS`, and within
    `late_tolerance_ns` (default: min(TROFFSET, TFRAME − RACTIVE·TFRAME)). Otherwise the
    unit is DROPPED with reason `WOULD_OVERLAP`.
  - Scope G-24 to "DROP, and bounded SEND_LATE".
  - Qualify §10.2 accordingly.

### 5. TAI-mode "snap to the nearest unit" causes spurious duplicates and gaps for jittery capture timestamps, and discards the camera phase

- **Severity:** major.
- **Where:** 06 §4.1 (TAI "snapped to the nearest unit (snap error reported)"; "media time … must strictly increase … `-EINVAL`"), §8 (audio contiguity), G-02.
- **Issue:**
  - A genlocked camera stamps `M_k = N_k·TFRAME + φ + jitter`, where the jitter is µs-level
    from HW or SW capture timestamps. For φ near TFRAME/2, nearest-snapping flips between
    N and N+1, so two consecutive frames snap to the same N. The second is rejected with
    `-EINVAL`: a synchronous rejection with no CQ result (G-02), which looks like an API
    misuse, not lateness. The next frame then jumps by two, leaving a gap. [inferred]
  - Snapping also moves the RTP by up to TFRAME/2 away from the Image Sampling Instant,
    against -10 §7.6.2 "should reflect". Not snapping gives irregular increments, against
    §7.6.1.
  - Audio TAI mode has the same problem, at ±1 sample per submission, against the §8
    contiguity rule.
- **Suggested change:** TAI mode snaps with hysteresis to a *phase-locked* grid:
  - Lock φ₀ at the first unit, or at `DISCONTINUITY`. Each later unit's expected
    instant is `M_prev + period`.
  - Accept |M − expected| < `snap_tolerance` (default TFRAME/4 for video, a few samples
    for audio). Report `snap_error_ns` and a drift gauge.
  - Beyond tolerance: a DROP-style result with reason `OFF_GRID`, or an implicit
    re-anchor if the session allows it.
  - RTP = `floor((N·TFRAME + φ₀) × 90000)` keeps regular increments and preserves the
    sampling phase, which §7.6.3 allows as production intent within ±TFRAME.
  - Make snap mode selectable: NEAREST_EPOCH (φ₀ forced to 0, the default for playback)
    or LOCKED_PHASE (the default for capture).

### 6. The audio launch offset default (one ptime) breaks the JT-NM limit the design cites, and live audio needs a different mechanism

- **Severity:** major.
- **Where:** 06 §5.1 ST30 row, §8 bullet 3, Q-TIME-11 recommendation (a).
- **Issue:**
  - R12 §4.3 quotes JT-NM 2020 audio "delta packet vs RTP": min ≥ 0, **max ≤ 1 ms**.
    Level A (mandatory, ST 2110-30 §7) is 1 ms ptime. With D_a = ptime = 1 ms, measured
    latency = 1 ms + wire error + network. It exceeds 1 ms whenever the wire error is
    positive, which is about half the packets for symmetric SW pacing jitter. [inferred]
  - With 4 ms ptime, D_a = 4 ms fails outright.
  - The motivation (never "RTP in the future") needs only a margin equal to the pacing
    profile's worst *early* error, which is µs, not ms.
  - For capture (#1), the physical minimum is ≥ ptime plus app latency, and it must be
    reported as TSDELAY. One default cannot serve both cases.
- **Suggested change:**
  - Playback default: `D_a = clamp(granted_profile.max_early_error + margin, 0, ptime/2)`,
    for example 20–50 µs on RL/TSN.
  - Capture: D_min from #1 (≥ ptime).
  - Report D_a in `get_info` as TSDELAY.
  - Change the Q-TIME-11 recommendation.

### 7. Partial audio packets across submissions: latency, lease and result semantics are undefined

- **Severity:** major (touches G-01/G-05/G-21 and CQ credit accounting).
- **Where:** 06 §8 bullet 1, §4.1 (`sample_count`), 13 G-21, 05/07 lease rules.
- **Issue:** packets are cut on absolute sample indices, so a submission whose end is not
  on a packet boundary leaves a straddling packet. Common real sizes do this:
  - AAC 1024 samples = 21 × 48 + 16;
  - per-video-frame audio at 59.94 = 800/801 samples, not a multiple of 48;
  - 1001-cadence 1601/1602 at 29.97.

  The straddling packet raises questions the design does not answer:
  - *Timing.* Its deadline is its own launch time, but it cannot be built until
    submission n+1 arrives. So the effective submit deadline of submission n+1 is
    *earlier* than its own first-packet deadline, by up to one packet time. For live
    capture delivered in 1024-sample chunks, that samples' packet waits up to 21.3 ms
    (latency the app did not ask for). [inferred]
  - *Ownership.* Submission n's buffer is still being read after its last *whole* packet
    left. Is its terminal result held until n+1 arrives (a buffer held for one extra
    submission period, and CQ credits pinned)? Or does MTL copy the < S tail samples?
  - *Which result* reports the straddling packet's lateness?
  - At DRAIN/stop, DISCONTINUITY, RESLOT (AUTO) or a late DROP of n+1: is the partial
    packet padded (with what?), sent short (ST 2110-30/AES67 fix the packet time per
    stream, so this is non-compliant), or dropped (a gap of S samples)?
- **Suggested change:**
  - Specify that at pick-up MTL copies the tail (< S × channels × sample size bytes;
    ≤ 48 × 8 × 3 = 1152 B at Level A) into a per-session carry buffer. Submission n then
    completes at its last whole packet, and G-05 holds.
  - The straddling packet is attributed to n+1 and uses n+1's deadline.
  - On any discontinuity, stop or drop of n+1, the carry is zero-padded to a full packet
    and sent on time, counted as `samples_padded`. Otherwise it is dropped with a counter.
  - Document `min_submit_lead` for audio as "one packet earlier than the first packet's
    launch".

### 8. A default "SKIP" underrun violates ST 2110-40 and -41 keep-alive; the A/V example never submits ANC

- **Severity:** major (a "shall" clause).
- **Where:** 06 §7.3 (SKIP default for every essence), §4.2 ST40/ST41, 10 §6 (an ANC session is created and grouped, but no ANC submission appears in the loop).
- **Issue:**
  - ST 2110-40 §5.5 requires at least one RTP packet per field/frame/segment, with
    `ANC_Count = 0` and the marker set when there is no ANC (R12 §5.1).
  - ST 2110-41 requires at least one packet every 500 ms (R12 §5.2).
  - Today MTL sends nothing when the app has no ANC frame (`st_tx_ancillary_session.c:942-946`
    returns on `get_next_frame` failure; an empty frame sends one packet, `:966`)
    [verified]. The design inherits that.
  - In 10 §6 the ANC stream therefore sends nothing at all.
- **Suggested change:**
  - Add underrun policies `MTL_UNDERRUN_EMPTY_ANC`, the ST40 default: MTL emits the
    empty marker packet, timed at the start of the last VANC line, with `RTP = RTP_v(k)`
    and F bits from parity.
  - Add `MTL_UNDERRUN_KEEPALIVE`, the ST41 default: an empty packet if nothing was sent
    for 450 ms.
  - Add `MTL_UNDERRUN_SILENCE` as the audio option (zero payload with regular RTP). For
    AES67 receivers that mute or click on loss, it is the broadcast expectation, and it is
    preferable to REPEAT_LAST.
  - Fix 10 §6 to show ANC submissions, and note that the ANC deadline
    (`TFST + TEPO + TD`) is ≈ 0.3–1.3 ms after the frame epoch, so ANC for frame k must be
    submitted *before or with* video frame k.

### 9. ST22 packet count per frame is variable today and the design keeps it that way

- **Severity:** major (ST 2110-22 §5.2 "shall").
- **Where:** 06 §5.1 ST22 row, §4.1 `valid_bytes` ("audio/ANC/FMD/ST22: bytes used"), E-table.
- **Issue:**
  - -22 requires "a constant number of bytes per frame" and "a constant number of RTP
    packets per frame", with padding allowed (R12 §6).
  - MTL derives `st22_total_pkts = frame_size / pkt_len` from each frame's
    `codestream_size` and recomputes TRS per frame (`st_tx_video_session.c:2468-2500`,
    `tv_sync_pacing_st22` `:750-757`) [verified]. A VBR codestream therefore yields a
    variable packet count and a variable TRS.
  - The design's per-submission `valid_bytes` for ST22 invites exactly this.
- **Suggested change:**
  - The ST22 session has a constant `codestream_bytes`, which is granted and reported for
    SDP bitrate.
  - Submissions with fewer bytes are padded by MTL (RFC 9134 padding), and every frame
    sends the same NPACKETS at a constant TRS.
  - Larger submissions → `DROPPED/INVALID_PAYLOAD`.
  - Report `st22.padding_bytes`.

### 10. Clock steps: the recommended handling makes every timeline-anchored session drop forever; CLOCK_TAI may silently be UTC; PHC reads are syscalls

- **Severity:** major.
- **Where:** 06 §2.2, Q-TIME-1, Q-TIME-2 recommendation (a) "sessions keep media indices and re-derive launch".
- **Issue:**
  - *Forward step with a TAI-anchored timeline.* A file timeline's T0 is a fixed TAI
    value. After a +37 s step (MTL's own UTC→PHC switch at first sync,
    `mt_ptp.c:1062-1067` [verified]), "keep indices, re-derive launch" leaves every
    subsequent unit's launch 37 s in the past. Under DROP, the session drops
    *everything, forever*, until the app notices. [inferred]
  - *Backward step with AUTO media.* After a backward step (a grandmaster change to a GM
    with an earlier time), AUTO stamps the "current slot", whose index is < the last one.
    This violates the design's own strict-increase rule and sends RTP backwards.
  - *CLOCK_TAI.* It is `CLOCK_REALTIME + adjtimex().tai`, and the kernel TAI offset is 0
    unless a daemon sets it (phc2sys/chrony). Reading `CLOCK_TAI` as "external PTP"
    (06 §2.2 row 2) can silently reproduce today's 37 s UTC-labelled-TAI bug. [inferred]
  - *Reading a kernel PHC.* `clock_gettime` on a `/dev/ptpN` dynamic clock has no vDSO:
    it is a syscall plus a PCIe read, which is not allowed on tasklets (G-39) and is µs
    scale. With DPDK-bound ports, the PF PHC is only reachable through the PF kernel
    driver or `rte_eth_timesync_read_time`. [inferred]
  - *Per-port time.* Today pacing reads port P's time for every leg
    (`st_tx_video_session.c:695`, `st_tx_audio_session.c:265-266` "always use
    MTL_PORT_P") [verified]. With two NICs, each with its own PHC, the design does not say
    whose clock M and each leg's launch use.
- **Suggested change:**
  - Q-TIME-2: per timeline, choose `STEP_POLICY_REANCHOR` (the default for timelines not
    on the epoch: T0 += step, emitted as a DISCONTINUITY on every member, RTP jumps
    forward) or `STEP_POLICY_KEEP` (the epoch timeline; the late policy applies).
  - AUTO after a backward step waits until the grid passes the last index (bounded, else
    re-anchor), and never emits a smaller index.
  - Validate `adjtimex().tai` ≠ 0, or cross-check against the PHC, before labelling
    CLOCK_TAI as TAI.
  - All sources (PHC, CLOCK_TAI, user) feed the same published `{tsc_base, tai_base,
    ratio, seq}` time base, updated off-tasklet (04 §8 already proposes this for the user
    function; extend it to every source).
  - Define per-port offsets as compensated to one instance timescale.

### 11. RX media time is wrong for AES67 senders with non-zero RTP offset, and for `mediaclk:sender` streams

- **Severity:** major (interop; RX alignment and `presentation` depend on it).
- **Where:** 06 §11 (`media` = "RTP unwrapped to TAI using the arrival time"), `latency_ns`, `presentation`, G-25.
- **Issue:**
  - ST 2110 mandates offset 0, but AES67, and VSF TR-03-era 2110 gear, may use
    `a=mediaclk:direct=<offset>` ≠ 0. ST 2110-30:2025 §6.1 Note 1 warns about it (R12 §1.2).
  - Nearest-wrap unwrap then yields a media time off by `offset/Fs`, anywhere in
    [0, 24.86 h) at 48 kHz. `latency_ns` becomes hours or negative, `presentation` is
    wrong, and cross-session RX alignment breaks.
  - Asynchronous streams (`mediaclk:sender`, -10 §8.3; IPMX) have no TAI relationship at
    all.
- **Suggested change:**
  - RX config gains `rtp_offset` (from SDP; default 0) and `mediaclk_mode` DIRECT |
    SENDER.
  - With SENDER, or an unknown offset, `media` is flagged invalid, and only `rtp` and
    arrival times are valid.
  - The unwrap helper takes the offset.
  - Add a sanity check: if |arrival − media| > 1 s on DIRECT streams, raise
    `RX_TIMEBASE_SUSPECT` (a probable offset or clock mismatch).

### 12. `MTL_SUBMIT_DISCONTINUITY` allows RTP to jump backwards, and MTL's own RX drops that

- **Severity:** major.
- **Where:** 06 §4.1 bullet 3 ("the only way the next unit may jump backwards"), Q-TIME-6.
- **Issue:**
  - `rv_slot_by_tmstamp`-style logic treats an RTP older than every active slot as "in
    the past" and drops it until `ST_SESSION_REDUNDANT_ERROR_THRESHOLD` errors accumulate
    (`st_rx_video_session.c:1176-1197`) [verified]. Receivers doing 2022-7 dedup keyed on
    RTP+seq behave similarly.
  - A backwards jump inside a running stream therefore loses frames at MTL receivers, and
    at others. RFC 3550 receivers may even treat it as a new source only after the
    probation threshold. Meanwhile, -10 §7.3 Note 2 exists so that receivers can follow a
    sender restart *because* RTP stays on the TAI epoch.
- **Suggested change:**
  - Allow DISCONTINUITY only forward inside a RUNNING session.
  - A backward re-anchor requires stop/start: the receiver sees a signal gap of at least
    one unit, and sequence numbers restart per RFC 3550.
  - Document that on the epoch timeline, sender restarts are RTP-continuous by
    construction.

### 13. The 13 §7 contract-difference table is incomplete: the oracle it adopts would fail the design

- **Severity:** major (13 §7 makes the contract the reference oracle for G-19…G-28).
- **Where:** 13 §7; `doc/user-pacing-timestamp-contract.md`.
- **Issue:** the five listed rows are correct [verified against the contract text: floor
  (Appendix A), nearest/ties-later (Level 1), audio grid-or-reject (Appendix B), 1 s
  (Appendix A), ties-later ns versus `st_muldiv_u64_round_closest` ties-down,
  `st_fmt.c:951`]. The contract also says the following, which the design contradicts
  and the table omits:

  | Contract | Design | Consequence for the oracle |
  |---|---|---|
  | Too-early/insufficient-lead requests are **REJECTED**, consume no media unit, advance no RTP (Appendix A "State changes") | late at pick-up → accepted, then `DROPPED` (RTP slot consumed) or `RESLOT` | different state transitions; for AUTO the RTP of the *next* unit differs |
  | User timestamp must be a **non-zero** TAI value (Appendix A) | zero is valid when flagged (G-17) | rejection-reason set differs |
  | Exact ST40 request outside the LLTM/CTM window is **rejected** (Appendix D) | `EXACT` is sent and flagged `NON_COMPLIANT_TIMING` / `LATE` | opposite outcomes |
  | Audio packet-0 TX target = the grid point = the RTP instant (Appendix B) | TX = M + D_a | every audio TX expectation is off by D_a |
  | Audio RTP = `first_audio_rtp_timestamp + …`, anchored at the first *TX request* (user mode) | RTP from the timeline sample index | the anchor source differs |
  | `rtp_anchor_ticks` includes the delta; ST20/ST40 equal only "with the same RTP clock offset" (Level 2) | offset forbidden; `media_index_offset` | the oracle must not accept non-zero offsets for new-API sessions |
  | ST20 offsets are `packet_index × packet_interval` (uniform, one interval); no TLINE/2 in the ST20 section | linear TRS for NL/W; `+TFRAME/2 + TLINE/2` for the second field | packet-offset planner differs |
  | Packet-0 slot includes `− initial_virtual_receiver_packets × packet_interval` | 06 §5.1 says the first packet is at "TVD" (the pre-fill is not mentioned) | `scheduled_first` ambiguous (see #16) |
  | ST40 needs one deterministic target: `st40_target_delay` inside the window (Appendix D) | "inside the window" only | no deterministic expected TX |
  | `receive_timestamp` = 0 if packet 0 is missing | validity flags | |
  | `tx_queue_available_time` term in the scheduling cutoff | not modelled | see #4 |
  | Scope: ST20/ST30/ST40 only | ST22/ST41 included in G-19…G-28 | no oracle rules for ST22 CBR or ST41 |
  | User pacing snaps to the nearest **slot** (N·T + packet-0 offset) | legacy USER_PACING maps to TAI media, snapped to the **epoch** N·T (06 §12) | migrated apps' TX moves by ≈ TROFFSET − VRX0·TRS (≈ 604 µs at 1080p59.94) |

- **Suggested change:** extend the table with these rows, each with a Q-ID. State that
  13 adopts the contract's *method* (exact arithmetic, absolute anchors, per-packet
  comparison, no inference from output) but *replaces* its selection and state-transition
  rules. Write the new planner rules (D_a, linear PRS, TLINE/2, ANC target delay, ST22 CBR,
  VRX pre-fill) into an updated contract before Phase 3.

## Minors

### 14. NOT_BEFORE contradicts itself when VRX pre-fill is non-zero

- **Where:** 06 §5.2.
- **Issue:** "no packet leaves before t (the slot is the first one whose read schedule
  starts at or after t)". The read schedule starts at TPR0 = TVD, but the first packet
  leaves at TVD − VRX0·TRS (up to 9 × 3.707 = 33 µs earlier for type N at 1080p59.94, and
  far more for W) [verified arithmetic].
- **Suggested change:** "the first slot whose first-packet wire time ≥ t".

### 15. Add a stated invariant: no essence's first packet leaves before its media time

- **Where:** 06 §5.1.
- **Issue:** a type W sender may pre-fill up to VRX_FULL = 863 packets ≈ 3.2 ms > TROFFSET
  (637.7 µs), which would put packets before N·TFRAME ("RTP in the future", failing
  JT-NM's latency ≥ 0). MTL caps W pre-fill at 0.8·TRO/TRS today (R12 §3.5) [verified in
  R12]. The design does not carry the cap forward.
- **Suggested change:** add "launch(first packet) ≥ M" as a derived-launch invariant, with
  a test in G-27.

### 16. `scheduled_first` and "deadline" are each used in two senses

- **Where:** 06 §7.1/§7.5, 07 §1.1.
- **Issue:**
  - 07 defines ON_TIME as "first packet no later than its deadline + tolerance", but 06
    §7.5 defines `deadline` as the *pick-up* deadline.
  - ST 2110-21 lateness is per packet: every packet j by TPRj, and no VRX overflow. The
    first packet can be on time while the tail is late.
- **Suggested change:** separate `admission_margin_ns` (pick-up) from wire compliance
  (`max_packet_lateness_ns` versus TPRj). Define `scheduled_first` = TVD − VRX0·TRS
  explicitly.

### 17. Interlaced indexing: define the field rate, parity on non-epoch timelines, and L in frames

- **Where:** 06 §3.1, §4.2, §4.3.
- **Issue:**
  - If the index counts fields, `M(index) = T0 + index × den/num` needs the *field* rate
    (60000/1001 for 1080i59.94), yet the video config carries the frame rate (10 §2).
  - "Even = first field" holds only if T0 is on the *frame* grid. `MTL_ANCHOR_AT_TAI` with
    an arbitrary `tai_ns` breaks parity.
  - An odd L would put a first field in a second-field slot.
  - The §4.3 second-field formula agrees with `floor(M_field × 90000)` for 25i/29.97i/30i
    on the epoch grid (checked: 29.97i frames 0–3 give 1501/4504/7507/10510 both ways)
    [verified]. It diverges for off-grid T0.
- **Suggested change:**
  - Index counts fields at 2× the frame rate.
  - T0 must be frame-aligned for interlaced/PsF members: AT_TAI is snapped up to a frame
    boundary, and the snap is reported.
  - L (or D_min) is rounded to whole frames.

### 18. Small formula and wording errors in 06

- **Where:** 06 §4.3, §3.2, §10.1.
- **Issue:**
  - §4.3 audio: `RTP = floor(T0 × Fs) + first_sample + p × S` double-counts if p is the
    stream packet index. It should be `floor(T0·Fs) + p·S` (p absolute), or
    `floor(T0·Fs) + first_sample(p)`.
  - §10.1 `RTP_v = floor(T0·90000) + floor(1501.5·k)` is valid only because T0·90000 =
    15015n is an integer. Say so. For off-grid T0 use `floor((T0 + k·TFRAME)·90000)`.
  - §3.2 "TAI ns < 2^61": 2^61 ns = 73.07 years, so the bound expires in **2043**
    [verified]. It does not matter for 128-bit math, but do not state it as an invariant.
    Use "< 2^63 (year 2262)".
- **Verified correct in §10.1:** 15015n, 8008n and 10n slots; the pattern
  `floor(1501.5k)` = 0, 1501, 3003, 4504, …, 15015 at k = 10; per-frame audio at 59.94 on
  the epoch grid = 800, 801, 801, 801, 801 repeating.

### 19. `media_mode` is both a session config field and a per-submission field

- **Where:** 10 §6 (`sc_video.timing.media_mode = MTL_MEDIA_INDEX` and `.media = { MTL_MEDIA_INDEX, … }`), 06 §4.1.
- **Issue:** if a session may mix AUTO and INDEX, the monotonicity check for an INDEX
  unit after an AUTO unit cannot be synchronous, because AUTO's index is assigned at
  pick-up. RTP could then go backwards or duplicate, or fail late with no defined result.
- **Suggested change:** make media mode session-level only. Groups require INDEX or TAI
  (AUTO members cannot be synchronised).

### 20. §4.1 and §8 disagree on gaps

- **Where:** 06 §4.1 ("a gap in indices is allowed") versus §8 ("consecutive submissions
  must be contiguous unless DISCONTINUITY").
- **Issue:** the two rules contradict each other for audio.
- **Suggested change:** for audio, a forward gap is either allowed (and must then start on
  a packet boundary, or the carry is padded, see #7) or requires DISCONTINUITY. Pick one,
  and define packet re-phasing when a DISCONTINUITY index is not a multiple of S.

### 21. Audio packet time must be defined as S/Fs exactly; legacy ST31 80 µs is inconsistent

- **Where:** 06 §8; side finding for `side-findings.md`.
- **Issue:** `ST31_PTIME_80US` sets the packet time to 1/12500 s (`st_fmt.c:1111-1113`)
  but 4 samples at 48 kHz, 8 at 96 kHz (`:1172-1174`, `:1197-1199`). The actual durations
  are 83.33 µs. TX uses `trs = pkt_time`, `epochs = TAI / pkt_time`, RTP = epochs × S
  (`st_tx_audio_session.c:207-211`, `:238`, `:274-282`). The RTP therefore advances at
  50 000 (or 100 000) ticks/s, +4.17 %, drifting ≈ 41.7 ms per second against PTP.
  [verified by reading; not executed]
  The 44.1 kHz ST31 ptimes (48/44100 = 1.0884 ms, 6/44100 = 136.05 µs, 4/44100 = 90.70 µs)
  are consistent. 333⅓ µs = 16 samples at 48 kHz is fine as rational 1/3000.
- **Suggested change:** the new API takes `samples_per_packet` (integer) and derives
  ptime = S/Fs as a rational. Never take an enum ptime. Record the 80 µs bug in
  side-findings.

### 22. ANC details missing for interlaced and TEPO

- **Where:** 06 §4.2, §5.1, E7.
- **Issue:**
  - Second-field ANC must use TSST = TFST + TFRAME/2 + TLINE/2 and F = `0b11`. The first
    field uses `0b10`, progressive `0b00` (RFC 8331). 06 does not say that the F bits and
    TSST follow index parity.
  - TEPO needs the SDI raster (total lines, line timing), and the RFC 8331 line numbers
    of each ANC packet. The ANC session config therefore needs the associated video
    format, not only its rate.
  - `media_index_offset` (§10.4) on a video session must be inherited by its ANC session,
    or captions drift from picture.
- **Suggested change:** add these to the ST40 row, and to the group validation (ANC rate
  and raster = its video member's).

### 23. TSMODE must be declared, not inferred

- **Where:** 08 §4 ("TSMODE (SAMP for INDEX/TAI media time, NEW for AUTO)").
- **Issue:**
  - An app using TAI mode with arbitrary "ns I think in" is not a sampling-instant source.
    Claiming `TSMODE=SAMP` for it is false.
  - PASSTHROUGH (§5.3) is `PRES`, which is not listed.
  - TSDELAY must include L·TFRAME / D_min (see #1).
- **Suggested change:** add a `tsmode` session field, reported back with TSDELAY.

### 24. The 2022-7 default skew class is too narrow a claim, and it conflates tolerance with latency

- **Where:** 06 §11 bullet 2, Q-TIME-14 (class D default).
- **Issue:**
  - Class D (150 µs) is "physical-layer LAN redundancy". Red/blue fabrics with different
    hop counts are normally specified against class A (10 ms). [inferred]
  - MTL video RX already tolerates about (slots − 1) × TFRAME of skew through its slot
    array. [inferred from `st_rx_video_session.c` slot logic]
- **Suggested change:** report the *tolerated* PD (from buffer/slot sizing), default it to
  class A where memory allows (cheap for audio/ANC), and keep `link_offset`
  app-configured. Warn when link_offset < the configured PD.

### 25. RX flush deadline default is unspecified and must follow the sender model

- **Where:** 06 §11.
- **Issue:** a unit's last packet arrives at about M + L·TFRAME + TROFFSET +
  RACTIVE·TFRAME + network. At 1080p59.94 with L = 0 that is M + 16.66 ms before any
  network or PD. A flush offset shorter than that discards good frames.
- **Suggested change:** default `rx_flush_offset = expected_last_packet_offset + PD
  budget`, and bound it by `link_offset` when one is set.

### 26. Leap seconds and estimated clocks

- **Where:** 06 §2.2 row 4.
- **Issue:** a configured static UTC–TAI offset is wrong after the next leap second. The
  kernel's leap insertion in CLOCK_REALTIME, or an NTP leap smear (±11.6 ppm for 24 h), is
  a step or a rate error in "estimated TAI".
- **Suggested change:** take the offset from PTP announce `currentUtcOffset` when
  available, emit a TIME_STEP at leap events, and document the smear case.

### 27. Missing broadcast timing features

- **Where:** 06 overall, 08 §4, 01 non-goals.
- **Issue:** broadcast users will look for these and not find them:
  - (a) **Timecode.** ST 12-1 time address from media time: TAI→UTC via
    `currentUtcOffset`, local offset and daily-jam from ST 2059-2 SM TLVs, drop-frame at
    1001 rates. RP 188 / ST 12-2 ATC insertion into ST40 is the usual ask, and the
    ST 2059-2 fields belong in `mtl_time_status`.
  - (b) **A 1001 audio-cadence helper** (samples in frame k = `floor((k+1)·x) −
    floor(k·x)`, or the ST 299 five-frame phase, pinned). Dolby E over -31 needs
    frame-aligned audio.
  - (c) **A TX-side ST 2110-21 self-check.** VRX/CINST computed from observed TX times,
    exposed as counters (`tx.vrx_max`, `tx.cinst_max`, `tx.tpr_late_pkts`), cheap once E4
    exists.
  - (d) **Sender restart semantics.** SSRC kept or new, random initial sequence (RFC 3550),
    and a statement that epoch-timeline RTP is continuous across restarts (-10 §7.3 Note 2).
  - (e) **Preroll semantics** for `ARMED`. Units with M < start instant are `FLUSHED` or
    rejected.
  - (f) **Per-leg `observed_first`** in the TX result for 2022-7 senders, plus a
    leg-skew gauge.
  - (g) An **RX-side common link offset** per group (IPMX TR-10-1 §11.2).
- **Suggested change:** add (c), (d), (e) and (f) to 06/08. List (a), (b) and (g) as L4
  helpers or later items, with Q-IDs.

### 28. G-19 test ranges

- **Where:** 13 G-19.
- **Issue:** "10^7 units at today's TAI" misses the cases where rounding bugs hide:
  - the 2^32 RTP wrap (13.26 h at 90 kHz, 27.05 h at 44.1 kHz);
  - 44.1 kHz with 1001 rates;
  - ST41 rational rates;
  - negative indices relative to T0 (audio priming, R12 §10.4);
  - TAI values past 2^61 ns.
- **Suggested change:** add these as explicit oracle cases.

## Things the design got right

- Separating media time (→ RTP) from launch time (→ pacing), with launch derived by
  default, is exactly the ST 2110-10/-21 model: TVD on the PTP timescale, RTP as the
  sampling instant. It is the right answer to the maintainer's two-timestamp comment.
- RTP = `floor(M × R) mod 2^32` with zero offset, one rule for every essence, exact
  rational arithmetic computed from the anchor and never accumulated. Every 1001-rate
  number in 06 §3.3 (1001 families) and §10.1 checks out exactly: 15015n ticks, 8008n
  samples, 10n slots, 1501.5 ticks/frame, 800.8 samples/frame, 166.83 ms grid. So do
  R12's 57.39-tick TRODEFAULT, 3.7074/3.8619 µs TRS, the +66 ns double-`frame_time`
  error, and the ±6.63 h unwrap window.
- The default video media time = frame epoch fixes the verified ST20/ST40 RTP mismatch
  (TX-cursor RTP at `st_tx_video_session.c:724-730`, `:793`).
- The second-field RTP is floored separately (`+ floor(TFRAME·90000/2)`), and TLINE/2 is
  applied to launch only. Decoupling is what makes the -21 §6.3.3 read schedule possible.
  PsF segments share RTP.
- Linear PRS for W/NL (ST 2110-21:2022 §7.1.4), ST22 with the network compatibility model
  only, and ANC in LLTM/CTM windows are all standards-faithful targets.
- An explicit late policy with a per-unit result and margin. RESLOT is restricted to AUTO
  because it would otherwise rewrite content time. Invalid exact requests never fall back
  silently (today they do, `st_tx_video_session.c:1796-1805` [verified]).
- Lip-sync trim as an integer media-index offset instead of an RTP offset (-10 §7.3,
  §7.7.3). `rtp_timestamp_delta_us` is correctly identified as a de-facto non-zero RTP
  clock offset.
- `mtl_time` records with clock, flags and accuracy; the sticky PTP `locked` flag
  (`mt_ptp.c:540-553` never clears it on that path [verified]); UTC labelled TAI
  (`dev/mt_dev.c:2289` → `ptp_from_real_time` → `mt_get_real_time()` [verified]).
- Audio submissions that are sample-accurate and decoupled from video buffer boundaries,
  with packets cut from absolute sample indices. This is the right shape; #7 only asks for
  the edge semantics.
- SDP-relevant values (TP, TROFF, CMAX, TSMODE, TSDELAY, mediaclk, ts-refclk, TM) are
  exposed via `get_info`.
- The RX side reports raw RTP, unwrapped media time, per-leg arrival and an optional link
  offset, and never substitutes arrival for media time (G-25).

## A. Arithmetic summary

| Item | Stated | Exact | Verdict |
|---|---|---|---|
| 06 §3.3 row "30, 60, 24 → 1/30 s" | 1/30 s | 24p: **1/24 s** (1/30 s = 0.8 frames); 60p alone: 1/60 s; 24+30: 1/6 s | **error** |
| 06 §3.3 missing 44.1 kHz | — | 1001-family + 44.1 kHz: **1001/300 s = 3.337 s** (147147 samples, 300300 ticks) | **gap** |
| mixed video families | — | 25p + 59.94p: 1001/25 s = 40.04 s; 50p + 59.94p: 20.02 s | **gap** |
| 06 §3.2 "TAI ns < 2^61" | invariant | 2^61 ns ends in 2043 | **error (minor)** |
| 06 §4.3 audio RTP | `floor(T0·Fs) + first_sample + p·S` | double-counts; `floor(T0·Fs) + p·S` | **notation error** |
| 06 §10.1 15015n / 8008n / 10n / floor(1501.5k) | as stated | 15015n, 8008n, 10n; 0, 1501, 3003, 4504, … | correct |
| 06 §3.3 1001 rows | 3003/1501.5/3753.75/750.75 ticks; 1601.6/800.8/2002/400.4 samples; 1001/6000 s | same | correct |
| TRODEFAULT 1080p59.94 | 637.674 µs, 57.39 ticks | same | correct |
| TLINE/2 1080i50 / 1080i59.94 | ≈ 17.8 µs | 17.78 / 14.83 µs | correct |
| LLTM TD 1080p59.94 | 8/(FR·1125) | 118.64 µs | correct |
| Unwrap window at 90 kHz | ±6.6 h | ±6.628 h (96 kHz ±6.21 h; 44.1 kHz ±13.53 h) | correct |
| Legacy ST31 80 µs | — | 4 samples / 80 µs = 50 kHz RTP rate at 48 kHz (+4.17 %) | **code bug (side finding)** |
