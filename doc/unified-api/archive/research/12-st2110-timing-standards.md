# Research note 12: ST 2110 timing model and what it implies for API semantics

| Field | Value |
|---|---|
| Topic | Standards timing model (ST 2059-1, ST 2110-10/-20/-21/-22/-30/-40/-41, ST 2022-7, RFC 3550/4175/7273/8331/9134, AES67, JT-NM/EBU LIST) and the API semantics it implies for MTL |
| Date | 2026-09-29 |
| Status | Knowledge gathering only. Nothing implemented. Nothing in the repository changed except this file |
| Scope question | "Can we automatically sync multiple streams of different types (video 90 kHz grid, audio 48 kHz grid) timestamped by the app from a single file?" |
| Evidence labels | **[verified]** = read in the cited primary/public source; **[inferred]** = my derivation or a secondary source; **[unknown]** = could not establish |

## Sources actually read

The SMPTE ST 2110 suite, ST 2059-1 and ST 2022-7 are **freely downloadable from SMPTE's public document site** (`https://pub.smpte.org/pub/<doc>/`). Every SMPTE clause quoted below was read in these editions, so SMPTE claims below are **[verified]** against primary text, not against summaries. Other agents can use the same source.

| Short name | Edition read | URL |
|---|---|---|
| ST 2110-10 | 2022 | <https://pub.smpte.org/latest/st2110-10/st2110-10-2022.pdf> |
| ST 2110-20 | 2022 | <https://pub.smpte.org/pub/st2110-20/st2110-20-20221214-pub.ZIP> |
| ST 2110-21 | 2022 | <https://pub.smpte.org/pub/st2110-21/st2110-21-20221214-pub.ZIP> |
| ST 2110-22 | 2022 | <https://pub.smpte.org/pub/st2110-22/st2110-22-20220331-pub.ZIP> |
| ST 2110-30 | 2025 | <https://pub.smpte.org/pub/st2110-30/st2110-30-20251001-pub.ZIP> |
| ST 2110-40 | 2023 | <https://pub.smpte.org/pub/st2110-40/st2110-40-20231231-pub.ZIP> |
| ST 2110-41 | 2026-06 | <https://pub.smpte.org/pub/st2110-41/st2110-41-20260629-pub.ZIP> |
| ST 2059-1 | 2021 | <https://pub.smpte.org/pub/st2059-1/st2059-1-20201209-pub.ZIP> |
| ST 2022-7 | 2019 | <https://pub.smpte.org/pub/st2022-7/st2022-7-20181226-pub.ZIP> |
| VSF TR-03 | 2015-11-12 | <https://static.vsf.tv/download/technical_recommendations/VSF_TR-03_2015-11-12.pdf> |
| VSF TR-10-1 (IPMX timing) | 2024-02-23 | <https://static.vsf.tv/download/technical_recommendations/VSF_TR-10-1_2024-02-23.pdf> |
| RFC 3550, 4175, 7273, 8331, 9134 | current | `https://www.rfc-editor.org/rfc/rfcNNNN` |
| EBU LIST docs | master | <https://github.com/ebu/pi-list/tree/master/docs> (`video_timing_analysis.md`, `audio_timing_analysis.md`, `a2v_sync.md`, `ST_2022-7.md`) |
| EBU LIST source | master | `cpp/libs/st2110/lib/src/ebu/list/st2110/d21/{settings,vrx_calculator,c_calculator}.cpp`, `cpp/libs/analysis/lib/src/ebu/list/analysis/utils/rtp_utils.cpp`, `apps/listwebserver/src/{analyzers/rtp.ts,enums/profiles/profiles.ts}` |
| AES67 (paywalled) | via summary | <https://en.wikipedia.org/wiki/AES67> (secondary) |
| AMWA NMOS | current | IS-05 Behaviour <https://specs.amwa.tv/is-05/releases/v1.2.0/docs/Behaviour.html>, MS-04 Timing <https://specs.amwa.tv/ms-04/releases/v1.0.0/docs/2.5._Explanation_-_Timing.html> |
| In-repo | HEAD + working tree | `doc/compliance.md`, `doc/design.md` §6.11/§8.2, `doc/user-pacing-timestamp-contract.md`, `lib/src/st2110/st_rx_timing_parser.c`, `lib/src/st2110/st_tx_video_session.c`, `st_tx_audio_session.c`, `st_tx_ancillary_session.c`, `st_fmt.c`, `.github/copilot-docs/mtl-knowledge-base.md` §5 |

Not read: AES67 itself (paywalled), JT-NM Tested test plan PDFs (JT-NM criteria taken from EBU LIST docs/source that quote them), EBU R37 and ITU-R BT.1359 (values from secondary sources, marked [inferred]).

## 0. Key findings (read this if nothing else)

1. **One clock, one epoch, zero offset.** Every 2110 RTP clock is `floor(TAI_seconds_since_1970 × rate) mod 2^32`. ST 2110-10 §7.3 makes the RTP clock offset *zero* ("In this standard, the offset value shall be zero"),
   SDP carries `a=mediaclk:direct=0`. [verified] Therefore video (90 kHz) and audio (48 kHz) RTP values for the same instant are *computable from each other exactly*; A/V sync is a pure function of the TAI instant each
   unit represents.
2. **RTP timestamp = media time, not transmit time.** ST 2110-10 §7.5: RTP timestamps "shall reflect the sampling instant". For playback/synthetic video §7.6.3: the frame RTP "should represent a point in time of N ×
   TFRAME ... shall not exceed +/- TFRAME from the most recent N × TFRAME". [verified] MTL's *default* TX video path derives RTP from the TX cursor (`epoch + TRO − VRX·TRS`), about +57 ticks (≈0.6 ms) after `N × TFRAME`
   at 1080p59.94; `doc/design.md` §8.2 even says the RTP "reflects the actual wire time". That contradicts §7.5/§7.6.3 "should", and makes MTL's own default video RTP differ from its own ANC/audio RTP for the same
   instant. [verified in code]
3. **Transmit grid is defined on the PTP timescale, not on RTP.** ST 2110-21 defines `TVD = N × TFRAME + TROFFSET` "where the time scale has its origin at the SMPTE Epoch". [verified] So "RTP describes the content,
   pacing chooses TX" (the contract doc's central rule) is exactly the standards' model. The API should keep two separate, independently settable-or-derived quantities: *media time* and *launch time*.
4. **Standards give hard windows, not just "early/late".** Video: every packet j must be on the wire no later than `TPR_j`, VRX never above `VRX_FULL`, `0 ≤ TROFFSET` (and by definition `< TFRAME`). ANC:
   `TFST + TEPO + TD − TFRAME ≤ TX ≤ TFST + TEPO + TD` (TD = 1 ms CTM, `8/(FrameRate × TotalLines)` LLTM). Audio: AES67 sender jitter bound and JT-NM "RTP not in the future, ≤ 1 ms in the past" (2020) / ≤ 20 packet times
   (2022 profile). These are the natural definitions of "late", "early", "too far" for the API.
5. **Exact rational arithmetic is required and cheap.** 1001-rates give non-integer ticks per frame (59.94: 1501.5 ticks, 800.8 samples; 23.976: 3753.75 ticks). Standards say "truncate" (ST 2110-10 §7.6.1, RFC
   4175/8331/9134). MTL uses double `frame_time` (≈66 ns late at today's TAI for 1001 rates) and round-to-nearest (`st10_tai_to_media_clk`), which yields `floor + 1` on half of the frames at 59.94 and 23.976 in
   `RTP_TIMESTAMP_EPOCH` mode (Python emulation of the code, [inferred]). Compliance tools tolerate ±1 tick, exact oracles do not.
6. **Video and audio grids re-coincide every 1001/6000 s (166.83 ms)** for 59.94/29.97/23.976 with 48 kHz; every 40 ms for 25/50. Anchoring a multi-essence timeline on that common grid makes *every* essence start on an integer tick with no rounding. [inferred, computed]
7. **Answer to the maintainer's question: yes**, provided the API has a *timeline (sync group) object* holding one TAI anchor `T0` plus exact rational rates, the app timestamps units in *unit indices* (frame n, sample s) or in an exact rational file timebase, and MTL derives RTP (exact) and launch time (per-essence transmission model) from that. §10.6 gives the math.

## 1. PTP, ST 2059-1 epoch, media clock, RTP clock offset

### 1.1 Epoch and timescale

- "The SMPTE Epoch shall be 01 January 1970 00:00:00 TAI." Note 1: same as the PTP epoch of IEEE 1588-2008. Note 2: it is 63 072 010 s before 1972-01-01T00:00:00Z (UTC). (ST 2059-1:2021 §6.1) [verified]
- TAI has no leap seconds; UTC = TAI − 37 s since 2017 (PTP announce carries `currentUtcOffset`). MTL `doc/design.md` §5.4.3 already documents the 37 s confusion with third-party tools. [verified in repository; UTC offset value [inferred] from common knowledge]
- ST 2059-1 §5.2.1: "t is elapsed continuous time from SMPTE Epoch in seconds. Note: This is the same as PTP time". [verified]
- Every periodic signal is aligned so that "the Alignment Point would have occurred at the SMPTE Epoch": `AlignmentTime = n × AlignmentPeriod`, `NextAlignmentTime = floor(t / AlignmentPeriod + 1) × AlignmentPeriod` (§6.2). For HD/UHD SDI, `AlignmentPeriod = (H × V)/SR = 1/R` (one frame; two frames for ST 2051 two-frame mode, §7.4/7.4.1). [verified]
- AES3 audio is also epoch-aligned: alignment point = start of the Z preamble, `AlignmentPeriod = 192 × Tsamp` (§8.1). [verified]
- ST 2059-1 §5.1 defines `floor` and warns "Sufficient precision is necessary in calculations to ensure that rounding or truncation operations will not create errors in the end results." [verified] This is the standards basis for requiring exact integer/rational math in MTL.

### 1.2 Media clock, RTP clock, offset

| Term | Definition (ST 2110-10:2022 §4) |
|---|---|
| Media Clock | "timebase related to the sampling rate (or frame rate in the case of video) ... with a source specified by the mediaclk attribute in the SDP, used to advance the RTP timestamps" |
| RTP Clock | "counter advanced by the Media Clock at the rate specified for the media type, and which is sampled to determine the timestamps included in RTP packets" |
| Timestamp Reference Clock | "timebase specified with the ts-refclk attribute in the SDP" |
| Image Sampling Instant | "time instant representative of the scene capture time" |

[verified]

- §7.3 RTP Clock Offset: with `mediaclk:direct`, the SDP offset "indicates the RTP Clock value at the epoch ... In this standard, the offset value shall be zero." Note 1 explicitly overrides RFC 3550's random initial timestamp. Note 2: zero offset lets receivers resume after a sender restart without a new SDP. [verified]
- §7.4: "The RTP Clock and Media Clock shall advance at uniform rates." [verified]
- §8.3: SDP "shall have a media-level mediaclk attribute"; direct form with `a=mediaclk:direct=0`; asynchronous media uses `a=mediaclk:sender`. §8.2: `a=ts-refclk:ptp=IEEE1588-2008:<gmid>:<domain>` or `:traceable` or `a=ts-refclk:localmac=<mac>`. [verified]
- RFC 7273 §5.2: `a=mediaclk:direct[=<offset>] [rate=<num>/<den>]`; "The offset indicates the RTP timestamp value at the epoch (time of origin) of the reference clock"; worked example: 1970→2013-01-01 = 1 356 998 400 TAI
  s × 90 kHz mod 2^32 = 2 460 938 240. The `rate=` modifier "is not advised for video streams". [verified] RFC 7273 does not define `localmac`; that token is ST 2110-10 §8.2's extension. [verified]
- History: VSF TR-03 (2015) §9 allowed a *constant non-zero* offset conveyed in SDP (examples `a=mediaclk:direct=2216659908`). ST 2110 tightened this to zero. AES67 still allows an offset; ST 2110-30:2025 §6.1 Note 1 warns implementers to be mindful of it. [verified] Implication: MTL RX should tolerate non-zero offsets (AES67 interop), MTL TX must emit zero.

### 1.3 The resulting formula

```text
rtp(t, R) = floor( t_TAI_seconds × R ) mod 2^32        # R = 90000 (video/ANC/-22), 48000/96000/44100 (audio), signalled rate (-41)
t in exact rational seconds since 1970-01-01 00:00:00 TAI
```

Wrap periods: 90 kHz 13.26 h; 48 kHz 24.86 h; 96 kHz 12.43 h; 44.1 kHz 27.05 h. [inferred, computed] Unwrapping an RTP value to TAI needs an anchor within half a wrap (±6.6 h at 90 kHz). MTL `st10_media_clk_to_tai()` does this nearest-cycle unwrap. [verified in code]

## 2. ST 2110-10 RTP timestamp rules per essence

| Essence | Rule | Clause |
|---|---|---|
| All | RTP ts "shall reflect the 'sampling instant' of the essence samples contained within the RTP packet" | -10 §7.5 |
| Video, general | successive frames "shall advance at regular increments based on the prevailing frame rate, truncating to integer values when necessary. When in conflict, this requirement supersedes the cases in the subsections below." | -10 §7.6.1 |
| Video, interlaced | first-field ts advance per frame; second field "offset from the RTP timestamp of the first field by one half of the prevailing frame period, truncating to integer values when necessary" | -10 §7.6.1 |
| Video, PsF | "both segments shall have the same RTP Timestamp" | -10 §7.6.1 |
| Video, camera | "should reflect the Image Sampling Instant" | -10 §7.6.2 |
| Video, playback/synthetic | "should represent a point in time of N x TFRAME unless there is a specific production intent to place it differently; in any case it shall not exceed +/- TFRAME from the most recent N x TFRAME time point" (applies with `mediaclk:direct`) | -10 §7.6.3 |
| Video, from SDI | RTP clock sampled "at the Alignment Point of the SDI signal" (ST 2059-1) | -10 §7.6.4 |
| Video, per packet | all packets of one progressive frame (or one interlaced field) carry the same ts; clock 90 kHz | -20 §6.1.3 |
| Audio, general | advance "at regular increments based on the audio RTP Clock rate and the audio packet time" (AES67 §7.2); supersedes subsections | -10 §7.7.1 |
| Audio, capture | "should reflect the sampling instant of the first sample of the audio signal within the audio RTP packet" | -10 §7.7.2 |
| Audio, playback/synthetic | "should represent the time point at which the synthetic essence has the intended time relationship to other essences within the production" | -10 §7.7.3 |
| Audio, from SDI | first sample of each channel related to a video frame is "contemporaneous to the video frame RTP Timestamp ... offset by an amount determined during the de-embedding process" | -10 §7.7.4 |
| Audio, from AES3 | "a sample of the RTP Clock at the X or Z preamble of the first audio sample in the packet" | -10 §7.7.5 |
| ANC | "using the procedures specified for video ... such that the RTP Timestamp of the ANC Data is contemporaneous with the related field or frame of the video signal"; 90 kHz, offset per -10 | -40 §5.3–5.4 |
| Fast metadata | clock rate and ts meaning "defined in the document that specifies the Data Item Package Contents"; rate signalled in SDP; ts "may be used" for association with video/audio | -41 §5.3 |
| JPEG XS | ts = "sampling instant of the first octet of the video frame"; 90 kHz; non-integer instants truncated | RFC 9134 §4.2 |

All [verified].

Additional -10 provisions relevant to the API:

- **TSMODE / TSDELAY (§8.7).** `TSMODE=SAMP` (ts is the effective sampling instant, suitable for cross-essence alignment), `NEW` (created anew at egress from the sender's RTP clock; default if absent), `PRES` (preserved
  from an input not marked SAMP). `TSDELAY` = transmission delay `D_TX` in integer µs. Playback devices "can mark their output samples as TSMODE=SAMP" (Annex C). [verified] An MTL file-playout app that follows
  §7.6.3/§7.7.3 is a `TSMODE=SAMP` sender; MTL's current default video RTP (TX-derived) is closer to `NEW`.
- **Derived signals (§7.9).** Time-preserving processors keep the input ts (`T_NEWRTP(j) = T_RTP(j)`); time-resetting ones stamp "as if it were a new signal 'sampled' at the current time" (`T_NEWRTP(j) = T_NOW`). "In all cases, the 'regular increments' requirements ... shall take precedence." [verified] This maps onto an API choice "preserve input media time" vs "restamp at now".
- **Link offset (§7.8):** see §9.
- MTL `lib/` does not generate SDP at all (no `TSMODE`, `TROFF`, `TP=`, `mediaclk` strings in `lib/`, `include/`, `app/`, `ecosystem/`). [verified by grep] Whatever timing contract the API promises must be expressible in the SDP the *application* writes.

## 3. ST 2110-21 video timing model

### 3.1 Definitions and formulas (ST 2110-21:2022 §6)

```text
TFRAME      frame period (exact rational, e.g. 1001/60000 s)
NPACKETS    packets per frame (constant; ST 2110-22 also requires constant)
N           integer frame index on the SMPTE-epoch timescale
TVD         = N × TFRAME + TROFFSET                 "Video Transmission Datum"
TROFFSET    ≥ 0, "difference between the most recent integer multiple of TFRAME and TVD"  (so 0 ≤ TROFFSET < TFRAME)
TRODEFAULT  default TROFFSET; a different value must be signalled as TROFF=<µs>
TPR0        = TVD                                    first packet read from the virtual receiver buffer
TPRj        read instant of packet j

Gapped, progressive (6.3.2):   RACTIVE = 1080/1125, TRS = TFRAME × RACTIVE / NPACKETS, TPRj = TVD + j × TRS
                               TRODEFAULT = (43/1125) × TFRAME  if height ≥ 1080,  (28/750) × TFRAME  if height < 1080
Gapped, interlaced/PsF (6.3.3): TRS = TFRAME × RACTIVE / NPACKETS  (TFRAME = frame, NPACKETS = packets per frame)
                               TPRj = TVD + j × TRS                                    for 0 ≤ j < NPACKETS/2
                               TPRj = TVD + TFRAME/2 + TLINE/2 + (j − NPACKETS/2) × TRS  for NPACKETS/2 ≤ j < NPACKETS
                               RACTIVE = HEIGHT/525, /625, /1125; TRODEFAULT for 1125-line = INT((1125 − HEIGHT)/2)/1125 × TFRAME (= 22/1125 × TFRAME at 1080i)
Linear (6.4):                  TRS = TFRAME / NPACKETS, TPRj = TVD + j × TRS, TRODEFAULT as for gapped

Network Compatibility Model (6.6.1): leaky bucket, infinite size, drains one packet at every k × TDRAIN since the SMPTE Epoch
                               RNOMINAL = NPACKETS / TFRAME,  TDRAIN = (TFRAME / NPACKETS) × (1/β),  β = 1.10,  CINST ≤ CMAX always
Virtual Receiver Buffer (6.6.2): bucket of VRX_FULL packets, packet j drained at TPRj; sender must never overflow it and packet j must be emitted no later than TPRj (no underflow)
                               "evaluated using the RTP Clock timebase signaled in the SDP" (i.e. the ts-refclk/PTP timescale)
```

[verified] The 525/625-line TRODEFAULT cells in Table 1 are garbled in the published PDF (a trailing `+` with a missing term), as the MTL knowledge base §5 already notes; MTL and EBU LIST both use the 2017 fixed values 20/525 and 26/625. [verified in both codebases; the 2022 intent is [unknown]]

### 3.2 Sender types (§7.1) and receivers (§7.2)

| Type | PRS | VRX_FULL | CMAX | TP= |
|---|---|---|---|---|
| N (narrow) | gapped | `MAX(INT(1500×8/MAXUDP), INT(NPACKETS/(27000×TFRAME)))` | `MAX(4, INT(NPACKETS/(43200×RACTIVE×TFRAME)))` | `2110TPN` |
| NL (narrow linear) | linear | same as N | `MAX(4, INT(NPACKETS/(43200×TFRAME)))` | `2110TPNL` |
| W (wide) | **linear** | `MAX(INT(1500×720/MAXUDP), INT(NPACKETS/(300×TFRAME)))` | `MAX(16, INT(NPACKETS/(21600×TFRAME)))`, only for < 900 000 pkt/s | `2110TPW` |

[verified] `MAXUDP = 1500` for the standard UDP size limit. Optional SDP: `TROFF` (µs), `CMAX`. Receivers: type N needs same ts-refclk, `mediaclk:direct`, default TROFF; type W additionally accepts any TROFF and any
sender type; type A (asynchronous) accepts anything. ST 2110-20 §6.1.1: the VRX model applies only "where the Media Clock is locked to the timestamping reference clock"; the Network Compatibility Model applies always.
[verified]

Worked numbers, 1080p 4:2:2 10-bit, 4320 packets/frame [inferred, computed from the formulas]:

| Format | TRS gapped | TRS linear | TRODEFAULT | TRO/TRS | VRX_FULL N / W | CMAX N / NL / W | Vertical gap |
|---|---|---|---|---|---|---|---|
| 1080p59.94 | 3.7074 µs | 3.8619 µs | 637.674 µs (57.39 ticks) | 172 pkts | 9 / 863 | 6 / 5 / 16 | 667.3 µs |
| 1080p50 | 4.4444 µs | 4.6296 µs | 764.444 µs (68.8 ticks) | 172 pkts | 8 / 720 | 5 / 5 / 16 | 800.0 µs |

### 3.3 Frame-level timing diagram (progressive, gapped, type N)

```text
 TAI ──┬──────────────────────┬───────────────────────────────────────────────┬──────────────
       E(N) = N·TFRAME          TVD = E(N)+TROFFSET = TPR0          TPR(NP-1)  E(N+1)
       │  RTP ts of frame N     │                                             │   (vertical gap ≈ TFRAME·(1-RACTIVE) - TRO)
       │  (playback, §7.6.3)    │ ← read schedule: one packet every TRS →      │
       │                 ┌──────┴── sender may pre-fill up to VRX_FULL pkts   │
       │                 │ first packet on wire (FPT) = TVD - VRX0·TRS        │
       │<──── FPT ──────>│                                                    │
 EBU LIST/JT-NM: latency = TPA0 - RTPtime  must be in [0, 1 ms];  RTP offset = RTP - rtp(E(N)) in [-1, ceil(TRO·90k)+1] ticks
```

### 3.4 What compliance tools measure

| Metric | Definition | Source |
|---|---|---|
| N | `floor(t_first_packet / TFRAME)` | LIST `rtp_utils.cpp::calculate_n` [verified] |
| FPT | first captured packet time − `N × TFRAME` ("a measured TRoffset") | LIST `video_timing_analysis.md` [verified] |
| VRX | `vrx_prev + 1 − drained_delta`, `drained = floor((t − TVD + TRS)/TRS)`, reset to 0 per frame; TVD ideal = `N·TFRAME + TRO_default` (SDP TROFF optional) | LIST `vrx_calculator.cpp` [verified] |
| CINST | leaky bucket with `TDRAIN = TFRAME/NPACKETS/1.1`, started at the first packet of the capture | LIST `c_calculator.cpp` [verified] |
| Latency | `TP_A_0 − RTPTimestamp`; JT-NM Tested 4.4: RTP ts "not in the future, not more than 1 ms in the past (unless justified)" | LIST doc + `rtp.ts` limit `[0, 1 000 000] ns` [verified] |
| RTP offset | `RTP − N × TFRAME` (ticks); JT-NM profile `use_troffset`: `[-1, ceil(TRO_default × 90000) + 1]` | LIST `rtp.ts` [verified] |
| RTP ts delta | delta between consecutive frames/fields | LIST [verified] |
| Schedule | gapped if last→first gap ≥ 10 × inter-packet spacing | LIST doc [verified] |

LIST `calculate_rtp_timestamp()` uses `round()`, not truncation. [verified] Tools therefore tolerate ±1 tick; a strict standard reading (truncate) does not.

MTL's RX timing parser (`st_rx_timing_parser.c`) mirrors LIST closely: same FPT/latency/RTP-offset/VRX/CINST definitions and the same JT-NM pass windows (`latency ∈ [0, 1 ms]`, `rtp_offset ∈ [-1, ceil(TRO·90k)+1]`, `rtp_ts_delta ∈ [s, s+1]`). [verified in code] Deviations from the standard text, all [verified in code] unless marked:

- TRS always uses the *gapped* `RACTIVE`, so a type W or NL (linear) stream is evaluated against the wrong read schedule (ST 2110-21:2022 requires W to be linear).
- Frame-level CINST bucket restarts at each frame's first packet instead of draining at `k × TDRAIN` since the epoch (LIST does the same).
- TVD uses TRO_default only; signalled `TROFF` is ignored (LIST does the same).
- Uses `double` for absolute TAI ns; at 1.79e18 ns the ULP is 256 ns, so FPT/latency carry up to ~0.3 µs quantisation. [inferred] Fine for µs metrics, not for an exact oracle.

### 3.5 How MTL TX maps onto the model

From `tv_init_pacing()`/`transmission_start_time()`/`tv_sync_pacing()` [verified in code]:

```text
frame_time  = 1e9 × den / mul                       (double ns; for interlaced this is the FIELD period)
tr_offset   = TRODEFAULT (43/1125, 28/750; interlaced 20/525·2, 26/625·2, 22/1125·2 of the field period)
trs         = frame_time × RACTIVE / NPACKETS         (gapped RACTIVE even for ST21_PACING_WIDE)
start(N)    = N × frame_time + tr_offset − vrx × trs  (vrx = narrow VRX_FULL − compensations, wide = min(0.8·VRXW, 0.8·TRO/TRS))
pkt j       = start(N) + j × trs
RTP default = tai_to_media_clk(round_to_media_clk(start(N)))        # TX-derived, ≈ E(N) + TRO − vrx·TRS
RTP EPOCH   = tai_to_media_clk(N × frame_time)                        # ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH
```

Consequences:

- Default video RTP sits ~0.6 ms after the frame epoch. That still passes JT-NM (`≤ ceil(TRO·90k)+1`) and §7.6.3's *shall* (±TFRAME), but not its *should* (`N × TFRAME`). [verified]
- ANC TX (`tx_ancillary_session_sync_pacing`) and audio TX stamp RTP on their *epoch* (`N × frame_time`, `p × ptime`), so an MTL video stream and an MTL ANC stream for the same frame carry **different** RTP values by default (≈57 ticks at 1080p59.94). ST 2110-40 §5.4 requires them to be contemporaneous. [verified in code]
- Interlaced second field: MTL treats each field as its own epoch slot (`frame_time` = field period, even slot = first field) and omits `TLINE/2` from the second-field TX start, because TX and RTP are coupled (knowledge
  base §5: "Do not add 6.3.3's T_LINE/2 — it reaches the RTP timestamp"). In the standard, `TLINE/2` affects only the read schedule; the RTP second-field offset is exactly `TFRAME/2` (truncated). With decoupled
  media/launch time the correct `TPRj` can be used without touching RTP. At 1080i50 `TLINE/2` ≈ 17.8 µs ≈ 2 packets of VRX headroom. [inferred]
- `ST21_PACING_WIDE` keeps the gapped TRS; ST 2110-21:2022 §7.1.4 says type W "shall employ the linear PRS". A receiver checking W against a linear schedule sees the stream running ahead during the active period. [inferred from spec + code]
- Double `frame_time` for 1001 rates is ≈6.2e-10 ns/frame too long; at today's N ≈ 1.07e11 (59.94) the computed epoch is ≈66 ns late. [inferred, computed] Harmless for pacing, but together with round-to-nearest it flips RTP to `floor + 1` on odd frames at 59.94 (and 2 of 4 frames at 23.976/119.88) in EPOCH mode. [inferred: Python emulation of `tai_from_frame_count` + `st10_tai_to_media_clk`]

## 4. ST 2110-30 audio

### 4.1 Levels (ST 2110-30:2025 §7, Tables 2 and 3)

All senders and receivers shall support Level A. Senders must support the rate, ptime and at least one channel count of a claimed level; receivers must support all combinations in Table 3. [verified]

| Level | Sender: rate / ptime / channels | Receiver must also accept |
|---|---|---|
| A | 48 kHz / 1 ms / 1–8 | – |
| AX | 96 kHz / 1 ms / 1–4 | A |
| B | 48 kHz / 125 µs / 1–8 | A |
| BX | 96 kHz / 125 µs / 1–8 | A, B, 96 kHz 1 ms 1–4 |
| C | 48 kHz / 125 µs / 9–64 | A, 48 kHz 125 µs 1–64 |
| CX | 96 kHz / 125 µs / 9–32 | A, 48 kHz 125 µs 1–64, 96 kHz 1 ms 1–4, 96 kHz 125 µs 1–32 |

Vendor app notes differ from the 2025 text (e.g. DirectOut lists BX as 1–4 channels, a LinkedIn/DDG snippet claims "X" means shorter ptime); the table above is from the standard. [verified]

### 4.2 Clock, timestamps, packet time, buffering

- Media and RTP clock rate = sampling rate; offset per -10 §7.3 (zero); ts per -10 §7.5/§7.7. 48 kHz mandatory, 44.1/96 kHz "should" (one or both). Standard UDP size limit. Senders/receivers "shall observe the timing provisions of AES67 Clause 7.5 titled 'Sender timing and receiver buffering'". (-30 §6.1, §6.2.1) [verified]
- AES67 7.5 (secondary): receivers buffer ≥ 3 × ptime, recommended 20 × ptime (or 20 ms if smaller); sender jitter < 17 packet times (or 17 ms), recommended ≤ 1 packet time. AES67 ptimes: 125 µs, 250 µs, 333⅓ µs, 1 ms (required), 4 ms. [inferred: Wikipedia summary of AES67, primary not read]
- Samples per packet `S = Fs × ptime` must be an integer (48 at 1 ms/48 kHz, 6 at 125 µs). 333⅓ µs gives 16 samples at 48 kHz (exact rational 1/3000 s). [inferred]
- Packet `p` of a stream: `RTP_a(p) = (A0 + p × S) mod 2^32` (regular increments, -10 §7.7.1). Where the stream's first sample lies on the 48 kHz grid, `A0 = floor(T0 × Fs)` with no rounding. [inferred]
- "Tsm" does not occur in ST 2110-10 or -30; latency for audio is AES67's *link offset*, defined identically to -10 §7.8 when RTP = sampling instant (-10 §7.8 Note). [verified by grep; "Tsm" meaning [unknown]]

### 4.3 What tools measure for audio

| Metric | Definition / limit | Source |
|---|---|---|
| Delta packet vs RTP (latency) | arrival − RTP time. JT-NM 2020: min ≥ 0, max ≤ 1 ms. JT-NM 2022: min ≥ 0 ("expected not to be in the future"), max ≤ 20 × ptime, avg ≤ 2.5 ms. LIST narrow/wide rule: packetization (1 pkt) + transit (1 pkt) + jitter (1 / 17 pkt) → 1 ms: < 3 ms / < 20 ms | LIST `profiles.ts`, `audio_timing_analysis.md` [verified] |
| TS-DF | EBU Tech 3337 timestamped delay factor, 200 ms windows; tolerance 1, limit 17 × ptime | same [verified] |
| Inter-packet time | avg 0.99–1.01 ptime, max 17 ptime (JT-NM 2022) | `profiles.ts` [verified] |

MTL `ra_tp_*` uses dpvr narrow `3 × ptime`, wide `19 × ptime`, TSDF `1 / 17 × ptime`. [verified in code] MTL audio TX launches packet `p` at `p × ptime` exactly and stamps the same instant, i.e. delta ≈ 0; any early wire error makes it *negative* ("RTP in the future"), which fails the `min ≥ 0` criterion. A small positive launch offset would be safer. [inferred]

## 5. ST 2110-40 (ANC) and ST 2110-41 (fast metadata)

### 5.1 ST 2110-40:2023

- RTP: 90 kHz, video procedure, contemporaneous with the related field/frame (§5.3–5.4). RFC 8331 §2: ts = sampling instant of the frame (progressive) or field (interlaced); ANC packets from different frames/fields must not share an RTP packet; `F` = `0b10` first field, `0b11` second, `0b00` progressive/unspecified; marker = last ANC RTP packet of the frame/field. [verified]
- Keep-alive: at least one RTP packet per field/frame/segment; if no ANC, send `ANC_Count = 0` with marker set (§5.5). [verified]
- Timing model (§6): [verified]

```text
TLBO           time of the proposed SDI location relative to the most recent ST 2059-1 alignment point (second field: minus TSFO)
TSFO           = TFRAME/2 + TLINE/2
TEPO(j)        = min TLBO of the ANC packets in RTP packet j (empty packet: start of the last VANC line of the field)
TAD            = N × TFRAME + TROFFSET_ANC           (TROFF signalled if ≠ TRODEFAULT)
TFST           = TAD − TRODEFAULT                     (= N × TFRAME for the default)
TSST           = TFST + TSFO
LLTM:  TX(j) ≤ TFST + TEPO(j) + TD,  TD = 8 / (FrameRate × TotalLines)       (SDP TM=LLTM)
CTM:   TX(j) ≤ TFST + TEPO(j) + 1 ms                                           (TM=CTM or absent; receivers assume CTM)
both:  TX(j) ≥ (the bound above) − TFRAME                                      "shall not transmit RTP packets earlier than one frame period before"
SDP:   exactframerate mandatory; SSN=ST2110-40:2023 if TM present, else ST2110-40:2018
```

- RFC 8331 §2.1: "SHOULD transmit available ANC data packets as soon as practical"; "One millisecond is a reasonable upper bound". [verified]
- The contract doc Appendix D reproduces these formulas correctly (LLTM TD, CTM 1 ms, one-frame-early bound, TSFO). [verified by comparison]

### 5.2 ST 2110-41:2026-06

- RTP clock rate and ts meaning come from each Data Item's defining document; rate signalled in SDP (`rate=`); a stream may mix item types only if clock rate, source and ts meaning are compatible (§5.1, §5.3). [verified]
- At least one RTP packet every 500 ms even with no items; marker bit "shall be set to 0 for all packets" (this edition). MTL sets `marker = 0` (`st_tx_fastmetadata_session.c:186`). [verified]
- Network Compatibility Model (§7): `β = 1.1`, `TDRAIN = 1 / MAX(800, RNOMINAL × β)`, `CMAX = MAX(4, INT(RNOMINAL/43200))`, RNOMINAL averaged over ≥ 10 s. No frame-relative transmit window like -40. [verified]
- Implication: -41 is the one essence where the API cannot assume a video-frame grid; media time is whatever the item spec says, so the timeline object must accept an arbitrary rational rate or a per-unit explicit media time. [inferred]

## 6. ST 2110-22 and JPEG XS (RFC 9134)

- Compression or packetization "shall produce a constant number of bytes per frame" and "a constant number of RTP packets per frame" (padding allowed). 90 kHz. Traffic shaping per the ST 2110-21 *Network Compatibility Model* for N/NL/W, signalled `TP=2110TPN|NL|W`. (-22 §4, §5.2, §5.3) [verified]
- -22 §5.3 Note 1: "The Virtual Receiver Buffer Model compliance definitions of ST 2110-21 do not apply and therefore there is no requirement regarding 'gapped' or 'linear' transmission"; receiver buffering is left to the codec-specific standard. [verified] MTL sets `vrx = 0`, `warm_pkts = 0` for ST22 and recomputes `trs` from the per-frame packet count. [verified in code]
- RFC 9134 §4.2: ts = sampling instant of the frame's first octet, truncated; marker = last packet of frame/field; §4.3 `F` counter = frame number mod 32, `I` bits distinguish progressive/first/second picture segment;
  `T=1` sequential transmission. §5: traffic shaping per ST 2110-21 "RECOMMENDED", `TP` must be signalled. [verified] Whether RFC 9134 gives the second field its own ts is not clear from the fetched text; ST 2110-10
  §7.6.1 applies anyway through -22 §5.1. [unknown for RFC 9134 alone]
- "VBR" is not a 2110-22 mode; only CBR. RFC 9134 notes slice mode/VBR need padding or empty packets to keep packets per frame constant. [verified]

## 7. ST 2022-7 seamless protection

| Receiver class | Use case | PD limit SBR (< 270 Mb/s) | PD limit HBR (≥ 270 Mb/s) |
|---|---|---|---|
| A low-skew | intra-facility | ≤ 10 ms | ≤ 10 ms |
| B moderate-skew | short haul | ≤ 50 ms | ≤ 50 ms |
| C high-skew | long haul | ≤ 450 ms | ≤ 150 ms |
| D ultra low-skew | physical-layer LAN redundancy | ≤ 150 µs | ≤ 150 µs |

`PD = max |Pi − Pj|` (instantaneous path differential); `MD = PT − EA` where PT is the latest usable arrival and EA the earliest. Streams must have identical RTP headers and payloads; Annex A: RTP timestamps plus
sequence numbers correlate copies (16-bit sequence wraps too fast at HBR). Class C HBR startup suggestion: `PT = first arrival + 150 ms`, `EA = PT − 300 ms`. (ST 2022-7:2019 §4, §7, Annex A) [verified]

API implications [inferred]: (1) the receiver's link offset must include the PD budget of its class (ultra-low 150 µs is the realistic target for MTL dual-port on one site); (2) per-frame RX metadata must say which path
delivered packet 0 (the contract doc already requires this); (3) video `latency`/timing-parser metrics are per path; (4) because redundant copies share RTP headers, any per-port RTP difference (e.g. per-port launch
compensation leaking into RTP) breaks 2022-7 — another reason to derive RTP from media time only.

## 8. Cross-essence synchronisation

### 8.1 How 2110 aligns essences

- ST 2110-10 §7.1: "inter-stream synchronization at a common destination relies on comparison of RTP Timestamp values". VSF TR-03 §7: "Synchronization at the receiving device is achieved by the comparison of RTP
  timestamps with the common reference clock." [verified] There is no A/V "pairing" field anywhere in 2110; alignment = equal TAI instants after unwrapping each RTP value with its own clock rate.
- "Aligned" therefore means: video frame N (ts `floor(N·TFRAME·90000)`) and the audio sample whose ts is `floor(N·TFRAME·48000)` were sampled (or are intended to be presented) at the same TAI instant. Audio packet boundaries need not coincide with frame boundaries; they almost never do.
- For SDI-derived audio, ST 2059-1 §8.2 defines the 1001-rate cadence: "8008 audio samples are distributed over 5 video frames at the 30/1.001 frame rate", aligned on every 5th alignment point, controlled by the ST 318 ten-field sequence. [verified]

### 8.2 Exact numbers

| Video rate | 90 kHz ticks/frame | 48 kHz samples/frame | Frames until both are integers | Common grid period |
|---|---|---|---|---|
| 25 / 50 | 3600 / 1800 | 1920 / 960 | 1 | 40 ms / 20 ms |
| 29.97 (30000/1001) | 3003 | 1601.6 | 5 | 166.83 ms |
| 59.94 (60000/1001) | 1501.5 | 800.8 | 10 | 166.83 ms |
| 23.976 (24000/1001) | 3753.75 | 2002 | 4 | 166.83 ms |
| 119.88 (120000/1001) | 750.75 | 400.4 | 20 | 166.83 ms |

[inferred, computed with exact fractions] For all 1001 rates the common grid period is `1001/6000 s` (15 015 ticks, 8008 samples).

Samples per frame on an *epoch-anchored* grid (`floor((n+1)·x) − floor(n·x)`): at 29.97, frames 0..9 give `1601, 1602, 1601, 1602, 1602` repeating; at 59.94 `800, 801, 801, 801, 801`. [inferred, computed] The
often-quoted `1602/1601/1602/1601/1602` is the ST 272/299 embedding cadence started on the ST 318 sequence; the phase depends on the rounding convention and on `N mod 5`. The transport does not care: 2110 audio has no
per-frame unit. Only an MTL *pipeline* API that hands out "audio frames per video frame" has to choose a cadence, and it must derive it from absolute sample indices, never by accumulating 1601.6.

### 8.3 Sender-side rule for one timeline

```text
Given one timeline: anchor T0 (TAI, exact rational, ideally a multiple of the common grid period)
video unit k:   M_v(k)   = T0 + k × den_v/num_v                     RTP_v(k) = floor(M_v(k) × 90000) mod 2^32
  2nd field:    RTP_v2(k) = RTP_v(k) + floor((den_v/num_v) × 90000 / 2)   (equals floor(M_v(k)·90000 + TFRAME·45000) whenever M_v(k)·90000 and TFRAME·90000 are integers: true for 25i/29.97i/30i on the grid)
audio sample s: M_a(s)   = T0 + s / Fs                               RTP_a(packet p) = floor(T0 × Fs) + p × S  (mod 2^32), S = Fs × ptime
ANC for k:      RTP_anc(k) = RTP_v(k)   (RTP_anc2(k) = RTP_v2(k))
production intent offset (lip-sync trim, §7.7.3): add an integer number of units to s (or k); never a non-zero RTP clock offset
```

[inferred from -10 §7.3/7.6.1/7.7.1/7.7.3 and -40 §5.4]

### 8.4 Lip-sync tolerances (context for how exact "exact" must be)

- ITU-R BT.1359-1 detectability: sound +45 ms early / −125 ms late; EBU R37: per stage +5/−15 ms, end-to-end +40/−60 ms; ATSC IS-191: +15/−45 ms. [inferred: secondary sources via search, primary not read]
- MTL's default video-vs-audio RTP skew (~0.6 ms) is far below perceptual limits, so it is a *correctness/interop* issue (exact-equality checks, ANC-to-video matching, `TSMODE=SAMP` claims, 2022-7 identity), not an audible one. [inferred]
- EBU LIST's A/V sync tool measures the delay in both "network domain (packet capture timestamps) and media domain (RTP timestamps)" (`a2v_sync.md`). [verified] Both numbers will be reported by users; the API docs should say which one MTL guarantees.

## 9. Latency in 2110

### 9.1 Definitions (ST 2110-10 §7.8, §8.7)

```text
T_RTP(j)   time equivalent of packet j's RTP timestamp (first packet with that ts)
T_TX(j)    transmission instant
T_REC(j)   instant the media is reconstructed and "available for further use"
D_TX  = T_TX(j) − T_RTP(j)       sender transmission delay; signalled as TSDELAY=<µs>
D_NET = network delay (can be very small)
D_LO  = T_REC(j) − T_RTP(j)      receiver Link Offset Delay; "should be a constant value, designed or configured into the Receiver";
                                  receivers "should document their Link Offset Delay, and provide a means to configure it"
At T_NOW a receiver reconstructs the packet whose RTP time is T_NOW − D_LO.
The packet "could arrive at the Receiver as early as T_RTP(j)" → buffer ≥ packets arriving during D_LO.
```

[verified] IPMX TR-10-1 §11.2 makes Link Offset Delay a controllable attribute via the management API and uses it to give several receivers the same playout time. [verified] TR-03 §10: link offset "is determined by the receiver ... Some receivers may support a configurable link offset such that inter-stream synchronization (e.g. lip sync) can be achieved". [verified]

### 9.2 Budget and knobs [inferred]

```text
presentation_time(unit) = media_time(unit) + D_LO
D_LO ≥ D_TX(max) + D_NET(max) + D_2022-7(PD class) + D_RX_processing + reassembly(last packet of unit)
video unit completes at ≈ TPR(NP−1) = E(N) + TRO + (NP−1)·TRS  ≈ E(N) + TFRAME·(TRO/TFRAME + RACTIVE)  → ~16.6 ms after E(N) at 1080p59.94
audio packet p completes at T_RTP + ptime (capture) or at launch (playout)
```

Glass-to-glass for a camera = sensor exposure/readout + `D_TX` + `D_NET` + receiver `D_LO` + display; 2110 only standardises the middle terms. [inferred]

Knobs a user would expect from an MTL API:

| Side | Knob | Standards anchor |
|---|---|---|
| TX | media time per unit (or timeline anchor + unit index) | -10 §7.5–7.7 |
| TX | TSMODE the app will advertise (SAMP / NEW / PRES) and resulting TSDELAY estimate | -10 §8.7 |
| TX | video TROFFSET (default TRODEFAULT; must be constant; signal TROFF) | -21 §6.2, §8.2 |
| TX | sender type N / NL / W (fixes PRS, VRX_FULL, CMAX) | -21 §7.1 |
| TX | frame-slot delay L: transmit unit k in slot `N(k)+L` (0 for playout, ≥1 for live capture) | -21 TVD + -10 §7.6.3 bound |
| TX | ANC timing model LLTM/CTM and TROFFSET_ANC | -40 §6–7 |
| TX | audio launch offset `D_a` after media time (≥ 0; ≥ ptime for capture) | -10 §7.8, JT-NM, AES67 7.5 |
| TX | late policy: drop / repeat / re-anchor (never silent slip) | -10 §7.6.1 regular increments |
| RX | link offset D_LO (constant, per session or per sync group) | -10 §7.8 |
| RX | 2022-7 class / PD window | 2022-7 Table 1 |
| RX | early/late windows relative to `T_RTP + D_LO` | derived from §7.8 |
| RX | expose per-unit `media_time_tai` (unwrapped RTP), first/last packet RX time, path ID | LIST metrics, contract doc |

## 10. Implications for API semantics

### 10.1 Definitions to adopt

| Term | Proposed exact meaning | Set or derive |
|---|---|---|
| TAI time | integer ns (or exact rational s) since 1970-01-01 00:00:00 TAI; never UTC / CLOCK_REALTIME | MTL provides `now()` |
| Rate | exact rational `num/den` units per second (60000/1001 frames/s, 48000/1 samples/s, 90000/1 ticks/s) | user sets at create |
| Timeline (sync group) | `{T0, rates}`; all streams in the group share T0 | user creates; MTL may snap T0 to the common grid and report it |
| Unit | video frame / field / PsF segment, ANC unit (per frame/field), audio sample (packets are MTL's business), -41 item group | – |
| Media time `M(u)` | the TAI instant the unit represents (sampling or intended-relationship instant, -10 §7.5–7.7) | user sets (via index or explicit), MTL validates |
| Sampling instant | physical capture instant; for playback equal to `M(u)` by definition (§7.6.3/§7.7.3) | user |
| RTP timestamp | `floor(M(u) × clock_rate) mod 2^32`, offset 0 | **always derived**; never set directly in the synced API (keep a raw-RTP escape hatch outside the sync group) |
| Frame epoch `E(N)` | `N × TFRAME` since SMPTE epoch, N integer; exists for video/ANC only | derived |
| Transmit slot | the epoch index N whose `TVD(N)` carries the unit | derived from `M(u)` + slot delay L |
| Launch time | TAI instant the packet's first bit (SFD, per contract doc) leaves the NIC | derived per essence model; "exact launch" is an advanced opt-in |
| Deadline | latest launch that keeps the model: video `TPR_j`; ANC `TFST + TEPO(j) + TD`; audio `M + D_a,max` | derived |
| Late | submission arrives after `deadline − min_lead` for the unit's first packet | derived; triggers the configured late policy |
| Early | video: launch before `TPR_j − VRX_FULL·TRS` equivalent (VRX overflow); ANC: before `bound − TFRAME`; audio: launch before `M` ("RTP in the future") | derived; MTL holds the unit, never sends early |
| Too far in the future | `M(u)` more than a buffer horizon ahead of now (product limit; standards only bound RTP unwrap at ±half wrap and §7.6.3 ±TFRAME vs the TX slot) | MTL policy, configurable |
| Link offset | receiver constant `D_LO`, `presentation = M + D_LO` | user sets on RX |

### 10.2 Why media time must be the primary input

- The standards' TX models (TVD, TFST, AES67 ptime grid) are all anchored on the SMPTE epoch, and RTP is anchored on the same epoch with offset 0. If the API takes media time, MTL can derive *both* RTP and a compliant
  launch time; if the API takes launch time (today's `USER_PACING`), MTL must guess the media time and ends up coupling RTP to TX, which is exactly the default-RTP problem of §3.5. [inferred]
- `EXACT_USER_PACING` with arbitrary per-frame targets cannot satisfy ST 2110-21 in general: TVD must be `N·TFRAME + TROFFSET` with the *same* TROFFSET for every frame. An exact mode that is 2110-21 compatible is "user
  chooses a constant TROFFSET (signalled as TROFF)", not "user chooses each packet-0 time". [inferred from -21 §6.2] The contract doc already says "A freely shifted exact ST20 target does not automatically prove ST
  2110-21 delivery timing". [verified]

### 10.3 Proposed exact representation

```c
struct mtl_rational { uint64_t num; uint64_t den; };            /* rate in units per second, reduced */

struct mtl_media_time {                                           /* exact, no floating point */
  int64_t  index;                                                 /* unit index relative to the timeline anchor */
  struct mtl_rational rate;                                       /* units per second of this stream */
};

struct mtl_timeline {                                             /* one per sync group */
  uint64_t anchor_grid;                                           /* T0 = anchor_grid × G, G = common grid period (rational) */
  struct mtl_rational grid;                                       /* G, e.g. 1001/6000 s for 1001 families, 1/25 s for 25/50 */
};
```

Exact math (128-bit intermediates suffice: TAI ns < 2^61, rates < 2^20):

```text
T0 (s)               = anchor_grid × G.num / G.den
M(index) (s)         = T0 + index × rate.den / rate.num
M_ns                 = floor(M × 1e9)                     (report only; never feed back into RTP math)
RTP(clock R)         = floor(M × R) mod 2^32
                     = (anchor_grid × G.num × R / G.den  +  floor(index × rate.den × R / rate.num))  mod 2^32   when T0 × R is integer (grid choice guarantees it)
video slot N(index)  = T0/TFRAME + index + L              (integer when T0 on the grid; L = frame-slot delay)
video pkt j launch   = N·TFRAME + TROFFSET + TPRj_offset(j) − VRX0·TRS      (TPRj_offset per gapped/linear/interlaced formulas of §3.1)
audio pkt p          = samples [p·S, (p+1)·S);  RTP = floor(T0·Fs) + p·S;  launch = T0 + p·S/Fs + D_a
ANC unit for frame k = RTP_v(k); packet j launch in [TFST + TEPO(j) + TD − TFRAME, TFST + TEPO(j) + TD], TFST = N(k)·TFRAME (+TROFFSET_ANC − TRODEFAULT)
round only the final ns launch value; compute every unit from the anchor, never by adding a rounded period
```

[inferred] This matches the contract doc's Appendix A arithmetic rules ("Calculate each absolute time from its original anchor", "Never repeatedly add an already-rounded period", "floor, then retain the low 32 bits"). [verified by comparison]

### 10.4 Converting a file's timestamps

- A container gives `pts × time_base` (rational, e.g. `1/90000`, `1/48000`, `1001/60000`, or `1/1000` in Matroska). Map each unit to the nearest unit index of its stream rate: `index = round((pts − pts0) × tb × rate)`,
  then use §10.3. MS-04 describes the same "closest media rate tick" snapping (example 5) and says priming/pre-charge samples sit *before* the zero point. [verified MS-04 text; mapping rule [inferred]]
- Audio priming (encoder delay) → negative sample indices; MTL must not send them or must start the audio stream at index ≥ 0 while keeping the grid. [inferred]
- File rates labelled "29.97" must be treated as 30000/1001; never derive the rate from float fps. [inferred]

### 10.5 Late / early policies and RTP continuity

- -10 §7.6.1/§7.7.1 require "regular increments" and state they supersede everything else. If a unit misses its slot, the compliant choices are: (a) drop it and keep the grid (next unit's RTP is still on the grid; a
  one-period gap is visible, as the contract doc Appendix A requires), (b) repeat the previous unit's essence with the new unit's media time (continuity preserved, content frozen), (c) explicit re-anchor (a discontinuity
  the app requests and the receiver sees). Sliding the whole stream one slot later while keeping old media times is only valid while `|RTP − most recent N·TFRAME| ≤ TFRAME` (§7.6.3) and silently grows latency; it should
  not be a default. [inferred]
- MTL today on a late video frame jumps to the current epoch (`calc_frame_count_since_epoch()`: `frame_count = frame_count_tai`, `stat_epoch_drop`), and RTP follows the new epoch, i.e. policy (a) with the dropped count
  reported through `notify_frame_late`. [verified in code] In a sync group this must happen per unit index so that video and audio stay aligned (drop video frame k does not move audio). [inferred]
- "Too far in the future": MTL uses 1 s (`max_onward_epochs`, contract doc). No standard number exists; the only hard bounds are RTP unwrap ambiguity (6.6 h at 90 kHz) and, for synthetic video, §7.6.3's ±TFRAME between RTP and the transmit slot's epoch. [verified in code / spec]

### 10.6 Answer: can MTL auto-sync multi-essence streams timestamped from one file?

Yes, with these conditions [inferred]:

1. All sessions join one timeline object with a single `T0` snapped to the common grid of their rates (166.83 ms for 1001 families + 48 kHz), so every stream's first unit is on an integer RTP tick.
2. The app submits units with unit indices (or exact rational file timestamps that MTL snaps); MTL derives RTP with `floor(M·R)` and offset 0 for every essence, so equal media times give mutually consistent RTP by construction.
3. Each essence derives its own launch time from media time with its own model (video: slot `N(k)+L`, TROFFSET, PRS; audio: `M + D_a`; ANC: LLTM/CTM window). No essence's TX schedule can perturb any RTP value.
4. Start is gated: MTL arms all streams of the group for the same `T0`, which must be later than now + max(prep lead of all sessions). Per-essence lateness is handled per unit without re-anchoring the group.
5. Lip-sync trims are integer unit offsets on the media timeline (production intent, §7.7.3), not RTP offsets.

### 10.7 Where MTL's current contract, code and docs disagree with the standards

| # | Item | Standards | MTL contract doc / code / docs | Evidence |
|---|---|---|---|---|
| 1 | Default video RTP | sampling instant; playback `N×TFRAME` (-10 §7.5, §7.6.3 should) | code: TX-cursor derived (≈ E(N)+TRO); `design.md` §8.2: RTP "reflects the actual wire time"; epoch only with `ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH` | [verified] |
| 2 | Video vs ANC RTP for same frame | must be contemporaneous (-40 §5.4) | ANC uses epoch, video default uses TX cursor → ≈57-tick mismatch at 1080p59.94 | [verified in code] |
| 3 | RTP rounding | truncate (-10 §7.6.1, RFC 4175/8331/9134) | contract: floor (agrees); code: round-to-nearest ties-down (`st10_tai_to_media_clk`); LIST: `round()` | [verified] |
| 4 | Epoch arithmetic | exact (2059-1 §5.1 precision note) | code: double `frame_time` (+66 ns at today's TAI for 1001 rates) → EPOCH-mode RTP = floor+1 on some frames | [inferred, emulated] |
| 5 | RTP clock offset | shall be zero (-10 §7.3) | `rtp_timestamp_delta_us` shifts every RTP (a non-zero offset in effect); contract says ST20/ST40 RTP match "only when they also use the same RTP clock offset" | [verified] |
| 6 | Grid anchor | TVD relative to SMPTE epoch (`N×TFRAME`) | contract Appendix B `normal_grid_anchor` "fixed absolute TAI value established before transmission" — compliant only if it is ≡ 0 mod TFRAME; should say so | [verified text] |
| 7 | Interlaced 2nd-field TX | `TPRj = TVD + TFRAME/2 + TLINE/2 + ...` (-21 §6.3.3) | code omits TLINE/2 (knowledge base forbids it because TX and RTP are coupled); contract ST20 section does not define it (its ST40 appendix does use TSFO) | [verified] |
| 8 | Type W PRS | linear (-21:2022 §7.1.4) | TX wide pacing and RX parser both use gapped RACTIVE | [verified in code] |
| 9 | Exact user pacing | TVD needs constant TROFFSET | contract/code accept arbitrary per-unit targets (contract flags it as a separate claim) | [verified] |
| 10 | Audio RTP vs launch | ts = first-sample sampling instant; JT-NM: not in the future | code launches exactly at the RTP instant (zero margin); contract derives audio RTP from `first_audio_rtp_timestamp` not from media time | [verified in code/doc] |
| 11 | Rounding ties | – | contract: "halfway ... choose the later nanosecond"; code `st_muldiv_u64_round_closest`: ties round down | [verified] |
| 12 | RX parser | VRX/CINST per -21 §6.6 | per-frame CINST reset, TROFF ignored, double precision (same as LIST) | [verified in code] |
| 13 | SDP | TSMODE/TSDELAY/TROFF/TP/mediaclk required or recommended | MTL lib emits none; apps must, and today cannot query TROFF/TSDELAY-equivalents except `st20_tx_get_pacing_params()` | [verified by grep] |

Where the contract doc agrees with the standards (worth keeping): "RTP describes the content. Pacing chooses TX." (= -21 TVD on the TAI scale); every packet of a unit carries the same RTP; fractional-rate increments
alternate deterministically; second field `+ floor(TFRAME·90000/2)`; PsF segments share RTP; ANC LLTM/CTM windows; dropped accepted units leave an RTP gap; oracle must not infer anchors from captured packets. [verified
by comparison]

## Open questions for the maintainer

1. **Should the default video RTP become `N × TFRAME` (epoch) instead of the TX cursor?** Why: -10 §7.6.3 says playback "should" use `N × TFRAME`; ANC/audio already do; the current default makes MTL's own essences
   disagree by ~TRO and makes a `TSMODE=SAMP` claim false. Options: (a) flip the default and keep a legacy flag, (b) keep default and document it as `TSMODE=NEW` behaviour, (c) remove the TX-derived mode entirely in the
   new API.
2. **Is a timeline / sync-group object in scope for the new API?** Why: automatic A/V/ANC sync needs one shared `T0` and exact rates; per-session timestamps cannot guarantee it. Options: (a) explicit `mtl_timeline` object sessions attach to, (b) implicit group keyed by a user-supplied `T0`, (c) no object; document the math and let apps do it.
3. **What is the unit of user-supplied media time?** Why: integer ns cannot represent 1001-rate instants exactly; unit indices can. Options: (a) unit index + stream rate (exact), (b) TAI ns snapped by MTL to the nearest unit (lossy but familiar), (c) both, with (a) canonical and (b) convenience.
4. **Do we allow user-set RTP at all inside a synced API?** Why: -10 §7.3 zero offset and "regular increments" are easy to violate with raw RTP; but forwarders/time-preserving processors (-10 §7.9) need to copy input
   RTP. Options: (a) RTP always derived from media time, raw RTP only in a non-synced "passthrough" mode, (b) allow RTP override with validation (regular increments, within ±TFRAME of slot), (c) status quo flags.
5. **Should `rtp_timestamp_delta_us` survive?** Why: it is effectively a non-zero RTP clock offset (forbidden by -10 §7.3) but users use it as a sampling-instant correction. Options: (a) replace by a media-time offset in integer units (production intent), (b) keep, documented as non-compliant, (c) drop.
6. **Which launch-time control do we expose for video?** Why: arbitrary exact per-frame launch cannot meet -21; a constant TROFFSET can. Options: (a) TROFFSET (µs, reported for SDP `TROFF`) + frame-slot delay L, (b) keep exact per-unit launch as an explicitly "non-2110-21" mode, (c) both.
7. **Fix type W to linear PRS (TX and RX parser)?** Why: -21:2022 §7.1.4 mandates linear for W; LIST infers schedule heuristically so interop may already work, strict analyzers may not. Options: (a) W = linear, add explicit NL, (b) keep gapped-W and signal `TP=2110TPN`-like behaviour honestly, (c) make PRS a separate knob.
8. **Adopt exact integer/rational arithmetic for all epochs and RTP (floor), replacing double `frame_time` and round-to-nearest?** Why: exact-equality tests (contract doc) fail on 1001 rates today; standards say truncate. Options: (a) 128-bit rational everywhere, (b) keep double for pacing but exact math for RTP, (c) keep as is, tolerate ±1 tick in tests.
9. **What is the late-unit policy in a sync group?** Why: dropping, repeating or re-anchoring have different continuity and A/V consequences; silent slip grows latency. Options: (a) drop and report (today's behaviour, per essence), (b) repeat last unit, (c) app-selected per session, (d) group-wide re-anchor event.
10. **Interlaced second-field TX: add `TLINE/2` once RTP is decoupled from TX?** Why: -21 §6.3.3 read schedule includes it; current omission eats ~2 packets of narrow VRX headroom at 1080i. Options: (a) implement after decoupling, (b) leave and document the deviation.
11. **Audio launch margin: send exactly at media time, or add `D_a > 0`?** Why: JT-NM requires the RTP time not to be in the future; zero margin fails on any early wire error. Options: (a) fixed small margin (e.g. ptime/2), (b) configurable `D_a` reported as TSDELAY, (c) keep zero.
12. **Should MTL generate or at least provide the timing fields of the SDP (TP, TROFF, CMAX, TSMODE, TSDELAY, mediaclk, ts-refclk, TM)?** Why: the APIs timing promises are only interoperable if signalled; today apps must reconstruct them. Options: (a) an SDP helper in lib, (b) a query API returning the values, (c) out of scope.
13. **Receiver side: do we add a link-offset / presentation-time API?** Why: -10 §7.8 expects receivers to have a constant, configurable `D_LO`; multi-essence RX alignment is `M + D_LO`. Options: (a) MTL delivers units at `M + D_LO` (scheduled delivery), (b) MTL only reports `M` (unwrapped TAI) + arrival times and apps schedule, (c) both.
14. **Which 2022-7 skew class does MTL claim for dual-port RX?** Why: it sizes the reorder window and the minimum link offset (Class D 150 µs vs Class A 10 ms). Options: (a) Class D only, documented, (b) configurable PD window, (c) Class A default.
