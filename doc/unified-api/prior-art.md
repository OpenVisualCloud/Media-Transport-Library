# Prior art: the standards, APIs and runtimes the design rests on

| | |
|---|---|
| Status | Maintained. Reference material; nothing here is a decision. The headers in [sketch/include/mtl/experimental/](sketch/include/mtl/experimental/) win over this text |
| Date | 2026-10-02 |
| Folded from | research notes 12, 09, 10 and 11 (`research/`), review C2 (`reviews/`), the familiarity map (design file 12), the Kubernetes studies K1 and K3 (`kubernetes/`), the Rivermax-chunk note of design file 06 §9, the prior-art part of study S8 §3, the Kubernetes facts of review RK §4; §9 also collects every external source the other former design files cite |

This file holds what the outside world says: the SMPTE, AES, IETF and VSF texts the timing model follows, the
I/O APIs (libfabric, Rivermax, DeckLink, AJA, GStreamer, io_uring, Vulkan and others) whose shapes the API
copies or avoids, and the Kubernetes and device-library facts behind the pod lifecycle. Each section gives the
facts with their reference (standard clause, man page, source file, URL), then **Took** and **Rejected** lines
that say what the design did with them and where. Facts were read in the cited source unless marked
*(inferred)* (reasoning or a secondary source) or *(unknown)* (not determinable from what was available).
Rules that the maintained documents already state are linked, not repeated: [timing.md](timing.md) owns the
timing contract, [deployment.md](deployment.md) the pod contract, [migration.md §13](migration.md#13-if-you-know-libfabric-or-rivermax)
the call map for libfabric and Rivermax users, and [decisions.md §3.2](decisions.md#32-why-prior-art) the
ten defaults taken from media APIs. Questions are in [questions.md](questions.md); review findings and their
outcomes are in [history.md](history.md). The code facts about MTL itself are at `545a266a`.

## 1. Contents

| § | Area | Main source |
|---|---|---|
| 2 | ST 2110 timing standards | research note 12, review C2 |
| 3 | libfabric (OFI) | research note 09 |
| 4 | NVIDIA Rivermax | research note 10 |
| 5 | Concept map for libfabric and Rivermax users | the familiarity map (design file 12) |
| 6 | Professional media I/O and async-I/O APIs | research note 11 |
| 7 | How Kubernetes runs a DPDK media process | K1 |
| 8 | Safe shutdown and crash cleanup elsewhere | K3 |
| 9 | Bibliography | every former design file |

## 2. ST 2110 timing standards

### 2.1 What was read

- **Primary texts read** (free on `pub.smpte.org`): ST 2110-10:2022, -20:2022, -21:2022, -22:2022, -30:2025, -40:2023,
  -41:2026-06, ST 2059-1:2021, ST 2022-7:2019; VSF TR-03 (2015-11-12) and TR-10-1 (2024-02-23); RFC 3550, 4175, 7273,
  8331, 9134; AMWA IS-05 v1.2.0 Behaviour and MS-04 v1.0.0 timing; the EBU LIST documentation and source
  (`video_timing_analysis.md`, `audio_timing_analysis.md`, `a2v_sync.md`, `ST_2022-7.md`;
  `cpp/libs/st2110/lib/src/ebu/list/st2110/d21/{settings,vrx_calculator,c_calculator}.cpp`,
  `cpp/libs/analysis/lib/src/ebu/list/analysis/utils/rtp_utils.cpp`, `apps/listwebserver/src/{analyzers/rtp.ts,enums/profiles/profiles.ts}`).
- **Not read:** AES67 (paywalled; taken from a secondary summary, so marked *(inferred)*), the JT-NM Tested test
  plan PDFs (their criteria come from the EBU LIST docs and source that quote them), EBU R37 and ITU-R BT.1359
  (secondary sources).
- **Review C2** did not re-read the primary texts; it recomputed every number with exact fractions
  (Python `fractions.Fraction`) and read every MTL code citation with `git show HEAD:<path>`.
- In-repository inputs: `doc/compliance.md`, `doc/design.md` §6.11 and §8.2, `doc/user-pacing-timestamp-contract.md`
  (a separate draft), `lib/src/st2110/st_rx_timing_parser.c`, `st_tx_video_session.c`, `st_tx_audio_session.c`,
  `st_tx_ancillary_session.c`, `st_fmt.c`, and `.github/copilot-docs/mtl-knowledge-base.md` §5.

### 2.2 Epoch, media clock and RTP clock

| Fact | Reference |
|---|---|
| The SMPTE Epoch is 1970-01-01 00:00:00 TAI, the PTP epoch of IEEE 1588-2008; it lies 63 072 010 s before 1972-01-01T00:00:00Z (UTC). `t` is "elapsed continuous time from SMPTE Epoch in seconds … the same as PTP time" | ST 2059-1 §6.1, §5.2.1 |
| TAI has no leap seconds; UTC = TAI − 37 s since 2017, carried in PTP announce `currentUtcOffset` (the value *(inferred)*); `doc/design.md` §5.4.3 already documents the 37 s confusion with third-party tools | ST 2059-1; MTL docs |
| Every periodic signal is aligned as if an alignment point occurred at the epoch: `AlignmentTime = n × AlignmentPeriod`, `NextAlignmentTime = floor(t / AlignmentPeriod + 1) × AlignmentPeriod`. HD/UHD SDI: `AlignmentPeriod = (H × V)/SR = 1/R`, one frame (two frames in ST 2051 two-frame mode) | ST 2059-1 §6.2, §7.4, §7.4.1 |
| AES3 audio: the alignment point is the start of the Z preamble, `AlignmentPeriod = 192 × Tsamp` | ST 2059-1 §8.1 |
| 1001-rate audio cadence: "8008 audio samples are distributed over 5 video frames at the 30/1.001 frame rate", aligned on every 5th alignment point under the ST 318 ten-field sequence | ST 2059-1 §8.2 |
| `floor` is defined, with the warning "Sufficient precision is necessary in calculations to ensure that rounding or truncation operations will not create errors in the end results": the basis for exact arithmetic (timing.md T4) | ST 2059-1 §5.1 |
| Media Clock: "timebase related to the sampling rate (or frame rate …) … used to advance the RTP timestamps". RTP Clock: "counter advanced by the Media Clock … sampled to determine the timestamps". Timestamp Reference Clock: the `ts-refclk` timebase. Image Sampling Instant: "representative of the scene capture time" | ST 2110-10 §4 |
| With `mediaclk:direct` the SDP offset "indicates the RTP Clock value at the epoch … In this standard, the offset value shall be zero". Note 1 overrides RFC 3550's random initial timestamp; Note 2: zero offset lets receivers follow a sender restart without a new SDP. RTP and Media Clock "shall advance at uniform rates" | ST 2110-10 §7.3, §7.4 |
| SDP: a media-level `mediaclk` is mandatory, `a=mediaclk:direct=0`, asynchronous media `a=mediaclk:sender`; `a=ts-refclk:ptp=IEEE1588-2008:<gmid>:<domain>`, `ptp=IEEE1588-2008:traceable`, or `localmac=<mac>` | ST 2110-10 §8.2, §8.3 |
| `a=mediaclk:direct[=<offset>] [rate=<num>/<den>]`; "The offset indicates the RTP timestamp value at the epoch (time of origin) of the reference clock". Worked example: 1970 → 2013-01-01 is 1 356 998 400 TAI s, × 90 kHz mod 2^32 = 2 460 938 240. `rate=` "is not advised for video streams". `localmac` is ST 2110-10's extension, not RFC 7273 | RFC 7273 §5.2 |
| History: TR-03 allowed a constant non-zero offset in SDP (example `a=mediaclk:direct=2216659908`); ST 2110 tightened it to zero; AES67 still allows an offset and ST 2110-30:2025 warns implementers about it | TR-03 §9; ST 2110-30 §6.1 note 1 |
| Formula `rtp(t, R) = floor(t × R) mod 2^32`, wraps and the nearest-wrap unwrap: [timing.md §2.5, §11.2](timing.md#112-media-time-and-media-index-from-rtp). MTL's `st10_media_clk_to_tai()` unwraps to the nearest cycle today | code |

### 2.3 RTP timestamp rules per essence

| Essence | Rule | Clause |
|---|---|---|
| all | the timestamp "shall reflect the 'sampling instant' of the essence samples contained within the RTP packet" | -10 §7.5 |
| video, general | successive frames "shall advance at regular increments based on the prevailing frame rate, truncating to integer values when necessary. When in conflict, this requirement supersedes the cases in the subsections below" | -10 §7.6.1 |
| video, interlaced | first-field timestamps advance per frame; the second field is "offset … by one half of the prevailing frame period, truncating to integer values when necessary" | -10 §7.6.1 |
| video, PsF | "both segments shall have the same RTP Timestamp" | -10 §7.6.1 |
| video, camera | "should reflect the Image Sampling Instant" (no ±TFRAME bound) | -10 §7.6.2 |
| video, playback or synthetic | "should represent a point in time of N x TFRAME unless there is a specific production intent to place it differently; in any case it shall not exceed +/- TFRAME from the most recent N x TFRAME time point" (with `mediaclk:direct`) | -10 §7.6.3 |
| video from SDI | the RTP clock sampled "at the Alignment Point of the SDI signal" | -10 §7.6.4 |
| video, per packet | every packet of a progressive frame (or of one field) carries the same timestamp; 90 kHz | -20 §6.1.3 |
| audio, general | advance "at regular increments based on the audio RTP Clock rate and the audio packet time" (AES67 §7.2); supersedes the subsections | -10 §7.7.1 |
| audio, capture | "should reflect the sampling instant of the first sample of the audio signal within the audio RTP packet" | -10 §7.7.2 |
| audio, playback or synthetic | "should represent the time point at which the synthetic essence has the intended time relationship to other essences within the production" | -10 §7.7.3 |
| audio from SDI | the first sample of each channel related to a frame is "contemporaneous to the video frame RTP Timestamp … offset by an amount determined during the de-embedding process" | -10 §7.7.4 |
| audio from AES3 | "a sample of the RTP Clock at the X or Z preamble of the first audio sample in the packet" | -10 §7.7.5 |
| ANC | "using the procedures specified for video … such that the RTP Timestamp of the ANC Data is contemporaneous with the related field or frame of the video signal"; 90 kHz, offset per -10 | -40 §5.3–5.4 |
| fast metadata | clock rate and timestamp meaning "defined in the document that specifies the Data Item Package Contents"; rate signalled in SDP; the timestamp "may be used" to associate with video or audio | -41 §5.3 |
| JPEG XS | the "sampling instant of the first octet of the video frame"; 90 kHz; non-integer instants truncated | RFC 9134 §4.2 |

### 2.4 TSMODE, TSDELAY, derived signals and SDP

- **TSMODE and TSDELAY** (-10 §8.7): `TSMODE=SAMP` (the timestamp is the effective sampling instant, fit for cross-essence
  alignment), `NEW` (created anew at egress from the sender's RTP clock; the default when absent), `PRES` (preserved from
  an input not marked SAMP). `TSDELAY` is the transmission delay `D_TX` in integer µs. Playback devices "can mark their
  output samples as TSMODE=SAMP" (Annex C). A file playout that follows §7.6.3 and §7.7.3 is a SAMP sender; MTL's
  TX-cursor video RTP today is closer to NEW. The unified rule (declared, not inferred): [timing.md §5.6](timing.md#56-invariants-and-signalling).
- **Derived signals** (-10 §7.9): time-preserving processors keep the input timestamp (`T_NEWRTP(j) = T_RTP(j)`);
  time-resetting ones stamp "as if it were a new signal 'sampled' at the current time" (`T_NEWRTP(j) = T_NOW`); "In all
  cases, the 'regular increments' requirements … shall take precedence". The unified processor recipe:
  [timing.md §10.5](timing.md#105-recipes).
- **SDP today.** MTL's `lib/`, `include/`, `app/` and `ecosystem/` contain no `TSMODE`, `TROFF`, `TP=` or `mediaclk`
  string (grep): the library generates no SDP, so every timing promise must be expressible in the SDP the application
  writes. The values are `info.*` stats in the unified API; SDP rendering is Phase 7, later (`mtl_sdp.h`, `MTL_LATER`).

### 2.5 ST 2110-21, receivers and what compliance tools measure

The formulas (TVD, TRS, TPRj for gapped, linear, interlaced and PsF, the network compatibility model), the sender
types N, NL, W with VRX_FULL, CMAX and `TP=`, and the worked numbers for 1080p59.94 and 1080p50 are in
[timing.md §5.1](timing.md#51-the-st-2110-21-model). What that section does not hold:

- **Model details.** The network compatibility model is a leaky bucket of infinite size that drains one packet at every
  `k × TDRAIN` since the epoch, `RNOMINAL = NPACKETS / TFRAME`, and `CINST ≤ CMAX` always (-21 §6.6.1). The virtual
  receiver buffer drains packet j at TPRj; the sender never overflows it and emits packet j no later than TPRj, "evaluated
  using the RTP Clock timebase signaled in the SDP", the ts-refclk (PTP) timescale (§6.6.2). `MAXUDP = 1500`; optional SDP
  `TROFF` (µs) and `CMAX`.
- **Where VRX applies.** The VRX model applies only "where the Media Clock is locked to the timestamping reference clock";
  the network compatibility model always (-20 §6.1.1).
- **Receivers** (-21 §7.2): type N needs the same ts-refclk, `mediaclk:direct` and the default TROFF; type W also accepts
  any TROFF and any sender type; type A (asynchronous) accepts anything.
- **A defect in the 2022 text.** The 525- and 625-line TRODEFAULT cells of Table 1 are garbled in the published PDF (a
  trailing `+` with a missing term). MTL and EBU LIST both use the 2017 fixed values 20/525 and 26/625; the 2022 intent is
  *(unknown)*.

What EBU LIST (and so the JT-NM Tested criteria) measures:

| Metric | Definition | Source |
|---|---|---|
| N | `floor(t_first_packet / TFRAME)` | LIST `rtp_utils.cpp::calculate_n` |
| FPT | first captured packet time − `N × TFRAME`, "a measured TRoffset" | `video_timing_analysis.md` |
| VRX | `vrx_prev + 1 − drained_delta`, `drained = floor((t − TVD + TRS)/TRS)`, reset to 0 per frame; ideal TVD = `N·TFRAME + TRO_default` (SDP TROFF optional) | `vrx_calculator.cpp` |
| CINST | leaky bucket with `TDRAIN = TFRAME/NPACKETS/1.1`, started at the first packet of the capture | `c_calculator.cpp` |
| latency | `TP_A_0 − RTPTimestamp`; JT-NM Tested 4.4: RTP "not in the future, not more than 1 ms in the past (unless justified)"; `rtp.ts` limit `[0, 1 000 000] ns` | LIST doc, `rtp.ts` |
| RTP offset | `RTP − N × TFRAME` in ticks; profile `use_troffset`: `[−1, ceil(TRO_default × 90000) + 1]` | `rtp.ts` |
| RTP ts delta | between consecutive frames or fields | LIST |
| schedule | gapped if the last-to-first gap is ≥ 10 × the inter-packet spacing | LIST doc |

LIST's `calculate_rtp_timestamp()` uses `round()`, not truncation, so tools tolerate ±1 tick where a strict reading
(truncate) does not; the unified oracle does not tolerate it ([timing.md §16.3](timing.md#163-substrate-and-budgets)).

**MTL's RX timing parser** (`st_rx_timing_parser.c`) mirrors LIST: the same FPT, latency, RTP offset, VRX and CINST
definitions and the same pass windows (`latency ∈ [0, 1 ms]`, `rtp_offset ∈ [−1, ceil(TRO·90k)+1]`,
`rtp_ts_delta ∈ [s, s+1]`). Its deviations from the standard text:

| # | Deviation | Also in LIST |
|---|---|---|
| 1 | TRS always uses the gapped RACTIVE, so a type W or NL (linear) stream is checked against the wrong read schedule (-21:2022 §7.1.4 requires W to be linear) | — |
| 2 | the frame-level CINST bucket restarts at each frame's first packet instead of draining at `k × TDRAIN` since the epoch | yes |
| 3 | TVD uses TRO_default only; a signalled TROFF is ignored | yes |
| 4 | absolute TAI ns in `double`: at 1.79e18 ns the ULP is 256 ns, so FPT and latency carry up to ≈ 0.3 µs quantisation *(inferred)*; fine for µs metrics, not for an exact oracle | — |

**MTL's TX today against the model** (`tv_init_pacing()`, `transmission_start_time()`, `tv_sync_pacing()`):

```text
frame_time  = 1e9 × den / mul                       double ns; the FIELD period for interlaced
tr_offset   = TRODEFAULT (43/1125, 28/750; interlaced 20/525·2, 26/625·2, 22/1125·2 of the field period)
trs         = frame_time × RACTIVE / NPACKETS        gapped RACTIVE even for ST21_PACING_WIDE
start(N)    = N × frame_time + tr_offset − vrx × trs  narrow: VRX_FULL − compensations; wide: min(0.8·VRXW, 0.8·TRO/TRS)
pkt j       = start(N) + j × trs
RTP default = tai_to_media_clk(round_to_media_clk(start(N)))     TX-derived: E(N) + TRO − VRX0·TRS
RTP EPOCH   = tai_to_media_clk(N × frame_time)                     ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH
```

- The default video RTP sits after the frame epoch by `TRO − VRX0·TRS` (+54.4…+55.7 ticks at 1080p59.94 for VRX0 9…5;
  research note 12 first gave ≈ 57 ticks, without the pre-fill). It passes JT-NM and the §7.6.3 *shall*, not its
  *should*. ANC and audio TX stamp their epoch (`N × frame_time`, `p × ptime`), so MTL's own video and ANC for one frame
  disagree, against -40 §5.4. `doc/design.md` §8.2 even says the RTP "reflects the actual wire time".
- Interlaced: each field is its own epoch slot (even slot = first field) and the second field omits `TLINE/2`, because TX
  and RTP are coupled (knowledge base §5: "Do not add 6.3.3's T_LINE/2 — it reaches the RTP timestamp"). In the standard
  `TLINE/2` touches only the read schedule. At 1080i50 it is ≈ 2 packets of VRX headroom *(inferred)*.
- `ST21_PACING_WIDE` keeps the gapped TRS, although -21:2022 §7.1.4 says type W "shall employ the linear PRS"; a receiver
  checking W against a linear schedule sees the stream run ahead during the active period *(inferred)*.
- `double frame_time` for 1001 rates is ≈ 6.2e-10 ns per frame too long; at today's N ≈ 1.07e11 (59.94) the epoch is
  ≈ 66 ns late. Harmless for pacing, but with round-to-nearest (`st10_tai_to_media_clk`) it gives `floor + 1` on odd
  frames at 59.94 and on 2 of 4 frames at 23.976 and 119.88 in EPOCH mode *(inferred: a Python emulation of
  `tai_from_frame_count` and `st10_tai_to_media_clk`)*.

### 2.6 ST 2110-30 and AES67

| Level | Sender rate / ptime / channels | Receiver must also accept |
|---|---|---|
| A | 48 kHz / 1 ms / 1–8 | – |
| AX | 96 kHz / 1 ms / 1–4 | A |
| B | 48 kHz / 125 µs / 1–8 | A |
| BX | 96 kHz / 125 µs / 1–8 | A, B, 96 kHz 1 ms 1–4 |
| C | 48 kHz / 125 µs / 9–64 | A, 48 kHz 125 µs 1–64 |
| CX | 96 kHz / 125 µs / 9–32 | A, 48 kHz 125 µs 1–64, 96 kHz 1 ms 1–4, 96 kHz 125 µs 1–32 |

- Every sender and receiver supports Level A; a sender claiming a level supports its rate, ptime and at least one channel
  count; a receiver supports every combination of its level (-30:2025 §7, Tables 2 and 3). Vendor notes differ from the
  2025 text (DirectOut lists BX as 1–4 channels; another snippet claims "X" means a shorter ptime); the table is the
  standard's.
- Media and RTP clock = the sampling rate; 48 kHz mandatory, 44.1 and 96 kHz "should"; the standard UDP size limit;
  senders and receivers "shall observe the timing provisions of AES67 Clause 7.5" (-30 §6.1, §6.2.1).
- AES67 §7.5 *(inferred, secondary)*: receivers buffer at least 3 × ptime, recommended 20 × ptime (or 20 ms if smaller);
  sender jitter below 17 packet times (or 17 ms), recommended ≤ 1 packet time. AES67 ptimes: 125 µs, 250 µs, 333⅓ µs,
  1 ms (required), 4 ms.
- "Tsm" occurs in neither -10 nor -30; audio latency is AES67's link offset, defined as -10 §7.8 when the RTP is the
  sampling instant (-10 §7.8 note).
- Packetisation, carry and the sample grid: [timing.md §8](timing.md#8-audio).

What tools measure for audio (LIST `profiles.ts`, `audio_timing_analysis.md`):

| Metric | Limit |
|---|---|
| delta packet vs RTP (latency) | JT-NM 2020: min ≥ 0, max ≤ 1 ms. JT-NM 2022: min ≥ 0 ("expected not to be in the future"), max ≤ 20 × ptime, average ≤ 2.5 ms. LIST narrow/wide rule: packetisation (1 packet) + transit (1 packet) + jitter (1 or 17 packets), so at 1 ms ptime < 3 ms / < 20 ms |
| TS-DF (EBU Tech 3337) | 200 ms windows; tolerance 1, limit 17 × ptime |
| inter-packet time | average 0.99–1.01 ptime, maximum 17 ptime (JT-NM 2022) |

MTL's `ra_tp_*` uses DPVR narrow 3 × ptime, wide 19 × ptime, and TSDF 1 / 17 × ptime. MTL audio TX launches packet p at
`p × ptime` and stamps the same instant, so the delta is ≈ 0 and any early wire error makes it negative ("RTP in the
future"). **Took:** a small launch offset `D_a` (≤ ptime/2, tens of µs on hardware pacing), because one ptime breaks
the JT-NM 2020 1 ms maximum at Level A ([timing.md §5.3](timing.md#53-derived-launch-per-essence)).

### 2.7 ANC, fast metadata and compressed video

- **ST 2110-40:2023 and RFC 8331.** RTP at 90 kHz by the video procedure; the timestamp is the frame's (progressive) or
  field's (interlaced) sampling instant; ANC packets from different frames or fields never share an RTP packet; `F` =
  `0b10` first field, `0b11` second, `0b00` progressive or unspecified; marker on the last ANC RTP packet of the frame or
  field (RFC 8331 §2). RFC 8331 §2.1: "SHOULD transmit available ANC data packets as soon as practical"; "One millisecond
  is a reasonable upper bound". SDP: `exactframerate` mandatory; `SSN=ST2110-40:2023` when `TM` is present, else
  `ST2110-40:2018`; receivers assume CTM when `TM` is absent. Keep-alive (§5.5), the transmit window (§6) and the
  live-ANC rule: [timing.md §9](timing.md#9-anc-and-fastmeta); the pacing contract draft's Appendix D reproduces the
  window formulas correctly.
- **ST 2110-41:2026-06.** The RTP clock rate and timestamp meaning come from each Data Item's defining document, the rate
  is signalled in SDP (`rate=`), and a stream mixes item types only when clock rate, source and timestamp meaning are
  compatible (§5.1, §5.3). At least one packet every 500 ms; the marker "shall be set to 0 for all packets" in this
  edition. Its network compatibility model (§7): `β = 1.1`, `TDRAIN = 1 / MAX(800, RNOMINAL × β)`,
  `CMAX = MAX(4, INT(RNOMINAL / 43200))`, RNOMINAL averaged over ≥ 10 s; there is no frame-relative window as in -40.
  So -41 is the one essence where the API cannot assume a video grid *(inferred)*; the unified API keeps 90 kHz and
  leaves item-defined clocks for later ([timing.md §9.3](timing.md#93-fastmeta-st-2110-41)).
- **ST 2110-22:2022.** Compression or packetisation "shall produce a constant number of bytes per frame" and "a constant
  number of RTP packets per frame" (padding allowed); 90 kHz; shaping per the -21 network compatibility model for N, NL,
  W with `TP=` (§4, §5.2, §5.3). §5.3 note 1: "The Virtual Receiver Buffer Model compliance definitions of ST 2110-21 do not
  apply and therefore there is no requirement regarding 'gapped' or 'linear' transmission"; receiver buffering is left
  to the codec standard. MTL sets `vrx = 0` and `warm_pkts = 0` for ST22 and recomputes `trs` from each frame's packet
  count. "VBR" is not a -22 mode.
- **RFC 9134** (JPEG XS): the timestamp truncated as in §2.3; marker on the last packet of a frame or field (§4.2); the
  `F` counter is the frame number mod 32 and the `I` bits mark progressive, first or second segment; `T=1` is sequential
  transmission (§4.3); shaping per ST 2110-21 is RECOMMENDED and `TP` must be signalled (§5); slice mode and VBR need
  padding or empty packets to keep the packet count constant. Whether RFC 9134 alone gives a second field its own
  timestamp is *(unknown)* from the fetched text; -10 §7.6.1 applies anyway through -22 §5.1. The unified CBR rule:
  [timing.md §5.3](timing.md#53-derived-launch-per-essence).

### 2.8 ST 2022-7 seamless protection

| Class | Use case | PD limit SBR (< 270 Mb/s) | PD limit HBR (≥ 270 Mb/s) |
|---|---|---|---|
| A low-skew | intra-facility | ≤ 10 ms | ≤ 10 ms |
| B moderate-skew | short haul | ≤ 50 ms | ≤ 50 ms |
| C high-skew | long haul | ≤ 450 ms | ≤ 150 ms |
| D ultra low-skew | physical-layer LAN redundancy | ≤ 150 µs | ≤ 150 µs |

- The table is ST 2022-7:2019 Table 1; what the receiver does with it is [timing.md §11.7](timing.md#117-due-time-completion-and-2022-7-skew).
  `PD = max |Pi − Pj|` is the instantaneous path differential; `MD = PT − EA`, PT the latest usable arrival and EA the
  earliest (ST 2022-7:2019 §4, §7).
- Copies have identical RTP headers and payloads; Annex A correlates them by RTP timestamp plus sequence number, because
  the 16-bit sequence wraps too fast at high bit rates. For class C at high bit rate the start-up suggestion is
  `PT = first arrival + 150 ms`, `EA = PT − 300 ms`.
- Implications *(inferred)*: the receiver's link offset includes the PD budget of its class; per-unit RX metadata names
  the path that delivered packet 0; timing metrics are per path; any per-port RTP difference (per-port launch
  compensation leaking into RTP) breaks 2022-7, one more reason RTP comes from media time only.
- **Took** (review C2): class D is "physical-layer LAN redundancy", while red/blue fabrics are normally specified against
  class A *(inferred)*; MTL video RX already tolerates about (slots − 1) × TFRAME of skew. So the default skew budget is
  class A (`rx.skew_budget_ns` 10 ms) and the tolerated PD is reported (`info.tolerated_skew_ns`).

### 2.9 Synchronisation, latency and lip-sync

- **Alignment is by RTP only.** "Inter-stream synchronization at a common destination relies on comparison of RTP Timestamp
  values" (-10 §7.1); "Synchronization at the receiving device is achieved by the comparison of RTP timestamps with the
  common reference clock" (TR-03 §7). No ST 2110 field pairs essences: aligned means equal TAI instants after each RTP is
  unwrapped at its own rate. Audio packet boundaries almost never coincide with frame boundaries.
- **Latency terms** (-10 §7.8, §8.7):

```text
T_RTP(j)  time equivalent of packet j's RTP timestamp (first packet with that timestamp)
T_TX(j)   transmission instant;   T_REC(j)  instant the media is reconstructed and "available for further use"
D_TX  = T_TX(j) − T_RTP(j)        sender transmission delay, signalled as TSDELAY=<µs>
D_NET = network delay ("can be very small")
D_LO  = T_REC(j) − T_RTP(j)       receiver Link Offset Delay: "should be a constant value, designed or configured into
                                  the Receiver"; receivers "should document their Link Offset Delay, and provide a means
                                  to configure it"
At T_NOW a receiver reconstructs the packet whose RTP time is T_NOW − D_LO. A packet "could arrive at the Receiver as
early as T_RTP(j)", so the buffer holds every packet arriving during D_LO.
```

- IPMX TR-10-1 §11.2 makes the link offset a controllable attribute through the management API and uses it to give several
  receivers one playout time; TR-03 §10: the link offset "is determined by the receiver … Some receivers may support a
  configurable link offset such that inter-stream synchronization (e.g. lip sync) can be achieved".
- **Budget** *(inferred)*: `D_LO ≥ D_TX(max) + D_NET(max) + D_2022-7(PD class) + D_RX processing + reassembly of the last
  packet`. A video unit completes at about `TPR(NP−1) ≈ E(N) + TROFFSET + RACTIVE·TFRAME`, ≈ 16.6 ms after E(N) at
  1080p59.94; an audio packet at `T_RTP + ptime` (capture) or at launch (playout). Glass-to-glass is exposure and readout +
  D_TX + D_NET + D_LO + display; ST 2110 standardises only the middle terms. The unified RX fields:
  [timing.md §11.3](timing.md#113-presentation-and-link-offset).
- **Lip-sync tolerances** *(inferred, secondary sources)*: ITU-R BT.1359-1 detectability +45 ms (sound early) / −125 ms
  (late); EBU R37 per stage +5/−15 ms, end-to-end +40/−60 ms; ATSC IS-191 +15/−45 ms. MTL's default ≈ 0.6 ms video-vs-audio
  RTP skew is far below these, so it is a correctness and interop issue (exact-equality checks, ANC-to-video matching,
  TSMODE=SAMP claims, 2022-7 identity), not an audible one.
- EBU LIST's A/V tool (`a2v_sync.md`) measures the delay in the "network domain (packet capture timestamps) and media
  domain (RTP timestamps)"; users will report both, so the documentation says which one MTL guarantees (the media domain).
- **File timestamps.** A container gives `pts × time_base` (`1/90000`, `1/48000`, `1001/60000`, `1/1000` in Matroska).
  MS-04 (timing explanation, example 5) snaps to the "closest media rate tick" and puts priming (pre-charge) samples
  before the zero point; audio priming therefore becomes negative sample indices (timing.md §4.4). A rate labelled
  "29.97" is 30000/1001, never derived from a float fps *(inferred)*.

### 2.10 Where MTL today disagrees with the standards

| # | Item | Standard | Today | Unified answer |
|---|---|---|---|---|
| 1 | default video RTP | sampling instant; playback `N × TFRAME` (-10 §7.5, §7.6.3 should) | TX cursor; `design.md` §8.2 "actual wire time"; epoch only with `ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH` | epoch (D-10, timing.md T2) |
| 2 | video vs ANC RTP of one frame | contemporaneous (-40 §5.4) | ANC on the epoch, video on the cursor | one rule (T6, G-20) |
| 3 | rounding | truncate (-10 §7.6.1, RFC 4175, 8331, 9134) | round-to-nearest, ties down (`st10_tai_to_media_clk`); LIST `round()`; the contract draft says floor | floor (G-19) |
| 4 | epoch arithmetic | exact (2059-1 §5.1) | `double frame_time` (+66 ns), EPOCH-mode RTP `floor + 1` on some frames *(emulated)* | exact rationals (D-11, E2) |
| 5 | RTP clock offset | zero (-10 §7.3) | `rtp_timestamp_delta_us` shifts every RTP; the contract draft matches ST20/ST40 only "with the same RTP clock offset" | `media_time_offset_ns`, `tx.index_offset` (timing.md §4.5) |
| 6 | grid anchor | TVD on the SMPTE epoch | the contract draft's `normal_grid_anchor` is any fixed TAI value: compliant only when ≡ 0 mod TFRAME | T0 on the common grid (timing.md §3.3) |
| 7 | interlaced second field TX | `TVD + TFRAME/2 + TLINE/2 + …` (-21 §6.3.3) | TLINE/2 omitted (coupled TX and RTP) | added once decoupled (E5, G-22) |
| 8 | type W schedule | linear (-21:2022 §7.1.4) | gapped on TX and in the RX parser | linear for NL and W (E5) |
| 9 | exact user pacing | constant TROFFSET | any per-unit target | `MTL_SUBMIT_EXACT`, counted non-compliant (timing.md §5.4) |
| 10 | audio RTP vs launch | first-sample instant; JT-NM "not in the future" | launch exactly at the RTP instant; the contract draft anchors audio RTP at the first TX request | `D_a` (timing.md §5.3); RTP from the sample index |
| 11 | ns rounding ties | — | contract draft: ties to the later ns; code `st_muldiv_u64_round_closest`: ties down | open, Q-TIME-15 |
| 12 | RX parser | -21 §6.6 VRX and CINST | per-frame CINST reset, TROFF ignored, `double` (as LIST) | §2.5 table; the parser is kept |
| 13 | SDP | TSMODE, TSDELAY, TROFF, TP, mediaclk required or recommended | none emitted; apps can query little beyond `st20_tx_get_pacing_params()` | `info.*` stats; `mtl_sdp.h` Phase 7, later |

Kept from the pacing contract draft, because they agree with the standards: "RTP describes the content. Pacing chooses
TX"; every packet of a unit carries one RTP; deterministic alternation of fractional increments; the second field
`+ floor(TFRAME·90000/2)`; PsF segments share RTP; the ANC LLTM/CTM windows; dropped accepted units leave an RTP gap; the
oracle never infers anchors from captured packets. The full contract comparison is [timing.md §16.2](timing.md#162-oracle-and-contract).

### 2.11 Review C2: what it recomputed and added

C2 (1 blocker, 12 majors, 15 minors) is applied throughout [timing.md](timing.md); each finding and where it landed is in
[history.md](history.md). Its exact arithmetic, which the maintained text relies on:

| Item | Exact value | Verdict on the draft |
|---|---|---|
| common grid 24p (+48 kHz) | 1/24 s (1/30 s is 0.8 frames); 60p alone 1/60 s; 24p + 30p 1/6 s | the draft's "30, 60, 24 → 1/30 s" row was wrong |
| 1001 family + 44.1 kHz | 1001/300 s = 3.337 s (147147 samples, 300300 ticks; 735.735 samples per frame at 59.94) | gap, now excluded from G above the 1 s cap |
| mixed video families | 25p + 59.94p: 1001/25 s = 40.04 s (1001 and 2400 frames); 50p + 59.94p: 20.02 s | gap, now `-MTL_EINVAL` above the cap |
| "TAI ns < 2^61" | 2^61 ns is 73.07 years: the bound ends in 2043 | use < 2^63 (2262) |
| audio RTP notation | `floor(T0·Fs) + p·S` with p absolute | the draft double-counted `first_sample` |
| 1001 rows | 3003, 1501.5, 3753.75, 750.75 ticks; 1601.6, 800.8, 2002, 400.4 samples; G = 1001/6000 s; 15015n, 8008n (16016 at 96 kHz), 10n; `floor(1501.5k)` = 0, 1501, 3003, 4504, … | correct |
| TRODEFAULT 1080p59.94 | 637.674 µs = 57.39 ticks; TRS 3.7074 µs gapped, 3.8619 µs linear | correct |
| TLINE/2 | 17.78 µs at 1080i50, 14.83 µs at 1080i59.94 | correct |
| LLTM TD 1080p59.94 | 8/(FR·1125) = 118.64 µs | correct |
| unwrap window | ±6.628 h at 90 kHz, ±6.21 h at 96 kHz, ±13.53 h at 44.1 kHz | correct |
| second field, 29.97i, frames 0–3 | 1501, 4504, 7507, 10510 by both formulas on the epoch grid; they diverge for an off-grid T0 | correct on the grid |
| L = 1 at 1080p59.94, type N | first packet ≈ M + 17.29 ms; LIST RTP offset −1501/−1502 ticks against a `[−1, 59]` window, latency 17.3 ms against `[0, 1 ms]` | legal for a camera (§7.6.2, "unless justified"), not for playback with L ≥ 2 |
| legacy ST31 80 µs | 4 samples per 80 µs at 48 kHz is a 50 kHz RTP rate (+4.17 %, ≈ 41.7 ms per second against PTP) | code bug, SF-35 |

Facts C2 added beyond research note 12, all applied in timing.md: `CLOCK_TAI` is `CLOCK_REALTIME + adjtimex().tai` and the
kernel offset is 0 unless a daemon sets it; `clock_gettime` on `/dev/ptpN` has no vDSO (a syscall plus a PCIe read); the
built-in PTP switches from UTC to the PHC at first sync (`mt_ptp.c:1062-1067`), a ≈ 37 s step; MTL RX drops an RTP older
than every active slot until `ST_SESSION_REDUNDANT_ERROR_THRESHOLD` (`st_rx_video_session.c:1176-1197`); an NTP leap smear
is ±11.6 ppm for 24 h; RFC 3550 receivers may treat a backward jump as a new source only after probation; and the
broadcast features users will look for (ST 12-1 timecode from media time via `currentUtcOffset`, ST 2059-2 SM TLVs for
local offset and daily jam, drop-frame at 1001 rates, RP 188 / ST 12-2 into ST40, a 1001 audio-cadence helper because
Dolby E over -31 needs frame-aligned audio, a TX-side -21 self-check, sender-restart semantics, preroll, per-leg observed
times, an RX common link offset per group).

Its two new questions: **Q-TIME-18** (live ANC with a video slot delay; answered by D-67, M4, [timing.md §9.2](timing.md#92-the-anc-transmit-window-and-live-anc))
and **Q-TIME-19** (common grid rules; proposed default (b): G covers video, ANC and grid fastmeta and every audio rate that
keeps G ≤ 1 s, other audio rates floor-aligned with the sub-sample phase reported, ST41 free-running excluded, a video
grid above the cap `-MTL_EINVAL`; interlaced members contribute TFRAME; applied in [timing.md §3.4](timing.md#34-the-common-grid);
status in [questions.md](questions.md)).

### 2.12 Research note 12's questions and where they were answered

| # | Question | Answer |
|---|---|---|
| 1 | default video RTP = `N × TFRAME`? | yes in the unified API; legacy keeps the cursor behind an opt-in (D-10, M7) |
| 2 | a timeline (sync group) object? | `mtl_timeline_create` plus start arrays, no group object (D-78; timing.md §3, §7) |
| 3 | unit of user media time? | both: `MTL_MEDIA_INDEX` (exact) and `MTL_MEDIA_TAI` (snapped) (D-38; timing.md §4.1) |
| 4 | user-set RTP in a synced API? | derived by default; `MTL_SUBMIT_RTP_TS` with validation, not under AUTO (timing.md §4.2) |
| 5 | keep `rtp_timestamp_delta_us`? | replaced by `media_time_offset_ns` and `tx.index_offset` (D-53; timing.md §4.5) |
| 6 | which video launch control? | constant TROFFSET, the slot rule with `min_tx_delay_ns`, `NOT_BEFORE`, and `EXACT` as non-compliant (D-39; timing.md §5) |
| 7 | type W linear? | yes, NL and W linear (E5) |
| 8 | exact integer arithmetic? | yes, 128-bit rationals (D-11, E2) |
| 9 | late policy in a sync group? | per unit: DROP, bounded SEND_LATE, RESLOT for AUTO; no group re-anchor (D-13; timing.md §6.2) |
| 10 | add TLINE/2 once decoupled? | yes (E5, G-22) |
| 11 | audio launch margin? | small `D_a`, reported as TSDELAY; CAPTURE uses `min_tx_delay_ns` (timing.md §5.3) |
| 12 | SDP timing fields? | `info.*` stats now; `mtl_sdp.h` Phase 7, later (D-94) |
| 13 | RX link offset / presentation time? | `rx.link_offset_ns` and `presentation_tai_ns` reported; scheduled delivery later (Q-TIME-12) |
| 14 | 2022-7 skew class? | class A default, tolerated skew reported (§2.8) |

### 2.13 Took and rejected

- **Took:** one epoch and zero RTP offset; media time separate from launch, RTP from media time only; the -21 model as the
  launch model, with linear W and NL and TLINE/2; exact rationals and floor; the common grid; per-essence launch models
  (video slot, audio `M + D_a`, ANC window); the -40 and -41 keep-alives; constant-size ST22; declared TSMODE and reported
  TSDELAY; the -10 §7.8 link offset on RX; tolerance of non-zero AES67 offsets on RX (`rx.rtp_offset`) and `mediaclk:sender`.
- **Rejected:** exact per-unit launch as the default (no constant TROFFSET); sliding late units (breaks regular
  increments); RTP offsets for lip-sync (forbidden by -10 §7.3; whole-unit offsets instead); LIST's ±1 tick tolerance in
  the oracle; class D as the 2022-7 default; an audio launch offset of one ptime.

## 3. libfabric (OFI)

Read: libfabric `main` (`FI_MAJOR_VERSION 2`, `FI_MINOR_VERSION 7`; `NEWS.md` lists v2.7.0 on 2026-09-18), the man pages
`fabric.7`, `fi_arch.7`, `fi_intro.7`, `fi_setup.7`, `fi_provider.7`, `fi_getinfo.3`, `fi_domain.3`, `fi_endpoint.3`,
`fi_mr.3`, `fi_cq.3`, `fi_eq.3`, `fi_cntr.3`, `fi_poll.3`, `fi_msg.3`, `fi_cm.3`, `fi_av.3`, `fi_trigger.3`, `fi_peer.3`,
`fi_version.3`; the headers `fabric.h`, `fi_domain.h`, `fi_endpoint.h`, `fi_errno.h` (`fi_errno.3` and `libfabric.map.in`
did not render), `src/abi_1_0.c`; the wiki pages "ABI Versions", "Common OFI Mistakes to Avoid", "Provider Feature Matrix
v2.7.x". libfabric has **no notion of time**: no scheduled transmit, deadline, late completion or timestamp in a
completion, and its triggers are counter thresholds only. It shapes the plumbing; MTL's timing comes from §2.

### 3.1 Facts

| Area | libfabric | Took / rejected |
|---|---|---|
| objects | `fid_fabric` (resources of one network) → `fid_domain` (one NIC or port; owns MR, AV, CQ, counters, endpoints) → `fid_ep` (`FI_EP_MSG`, `FI_EP_DGRAM`, `FI_EP_RDM`; passive `fid_pep`); | took instance, port, session; a session may span two ports (2022-7), so a domain is not a session's parent |
|  | `fid_cq`, `fid_eq`, `fid_cntr`, `fid_mr`, `fid_av` (address → compact `fi_addr_t`), `fid_mc` (`fi_join`); wait and poll sets **deprecated in 2.0**; scalable endpoints and shared contexts |  |
| lifecycle | open → bind (CQ, counters, EQ, AV "must be bound before enabling") → `fi_enable`; `fi_setopt` before enable | took create → start (`mtl_session_start`) |
|  | unconnected endpoints are bound to an AV, connected ones to an EQ (`fi_setup.7`); "An endpoint must be enabled before it may be used to perform data transfers" (`fi_endpoint.3`) |  |
| ops tables | every object starts `struct fid { fclass; context; ops }`; `struct fi_ops { size; close; bind; control; ops_open; tostr; ops_set }`; | took exported functions plus `struct_size` (D-74); named extension tables `mtl_open_ext()` (`MTL_LATER`); rejected inline vtable dispatch, which freezes layouts into applications |
|  | data calls are `static inline` dispatch through function pointers, so few symbols are exported; |  |
|  | tables grow at the tail, guarded by `FI_CHECK_OP(ops, type, op)` (`size > offsetof`), for example `fi_domain2()` returns `-FI_ENOSYS` when absent; the fast path is not NULL-checked; |  |
|  | `fi_open_ops(fid, name, …)` opens provider interfaces; `FABRIC_DIRECT` links one provider directly |  |
|  | `struct fid_ep { fid; ops; cm; msg; rma; tagged; atomic; collective }`, one ops table per class (`fi_endpoint.h`); libfabric "only exports a handful of functions", its ABI surface "most notably the fi_getinfo call and its returned attribute structures" (`fabric.7`); |  |
|  | tail growth seen in `fi_ops_domain`: `query_atomic`, then `query_collective`, `endpoint2`, `xpu_ctx` (`fi_domain.h`); `fi_set_ops` installs caller ops (overrides); `fi_control(fid, cmd, arg)` is the generic ioctl (`FI_GETWAIT`); |  |
|  | for MTL a vtable would buy ABI evolution and backend polymorphism (DPDK PMD, AF_XDP, kernel socket), not provider plug-ins *(inferred)* |  |
| discovery | `fi_getinfo(version, node, service, flags, hints, &info)`: a non-zero hint is required ("must support … or fail"), zero is a wildcard ("a value that works best"), | took requested against granted, zero = library choice, the grant reported (`mtl_session_query`, `caps.*` with `MTL_REQ_PREFER`/`REQUIRE`/`OFF`; D-19; contract.md); |
|  | caps and attributes are floors ("greater than or equal to requested"); primary caps must be requested, modifiers default to all, secondary caps fail with `FI_ENODATA`; | rejected mode bits (one implementer satisfies its own restrictions) |
|  | mode bits are "bottom up restrictions" a provider may clear, else it is skipped (`FI_CONTEXT`, `FI_MSG_PREFIX`, `FI_ASYNC_IOV`, `FI_RX_CQ_DATA`; `FI_LOCAL_MR` deprecated); |  |
|  | results best first; hints "must come from `fi_allocinfo()` or `fi_dupinfo()`" (2.0); counts such as `cq_cnt` are optimal, not limits; limits are exposed, not segmented internally |  |
|  | `fi_info {next, caps, mode, addr_format, src_addrlen, dest_addrlen, src_addr, dest_addr, handle, tx_attr, rx_attr, ep_attr, domain_attr, fabric_attr, nic}`; `fabric_attr {name, prov_name, prov_version, api_version}`; |  |
|  | `domain_attr`: `threading`, `control_progress`, `progress` (union alias `data_progress`), `resource_mgmt`, `av_type`, `mr_mode`, `mr_key_size`, `cq_cnt`, `ep_cnt`, `tx_ctx_cnt`, `rx_ctx_cnt`, `cntr_cnt`, `mr_iov_limit`, `max_err_data`, `mr_cnt`, `tclass`, `max_cntr_value`; |  |
|  | `tx_attr`/`rx_attr`: `size` (queue depth), `iov_limit`, `inject_size`, `op_flags` (default flags), `msg_order`, `comp_order` (deprecated); allocating more than `cq_cnt` and similar "means sharing underlying resources"; |  |
|  | extra caps are returned only when they cost no performance and do not widen communication; `FI_PROV_ATTR_ONLY` lists providers without probing hardware; |  |
|  | modes: `FI_CONTEXT`/`FI_CONTEXT2` = the app passes provider scratch `struct fi_context { void *internal[4]; }` ("should NOT be allocated on the stack"), valid until completion or cancel; `FI_MSG_PREFIX` = header space reserved ahead of buffers; `FI_ASYNC_IOV` = the iovec array untouched until completion; |  |
|  | limits exposed "rather than segmenting large transfers internally" (`fi_setup.7`); MTL's analogue: max frame size, max planes per frame, max units in flight, reported, not hidden |  |
| memory | `fi_mr_reg`/`regv`/`regattr` → `fid_mr { mem_desc; key }`; `fi_mr_attr` with iov or dmabuf (`fd, offset, len, base_addr`, only with `FI_HMEM`), | took explicit regions `mtl_mem_import`/`mtl_mem_alloc` and the library pool as the NULL-desc path; stricter close: refcounted, `mtl_mem_close` returns 1 until retired (D-16); |
|  | `iface` (`SYSTEM, CUDA, ROCR, ZE, NEURON, SYNAPSEAI`), sub-MRs; `mr_mode` bits (`LOCAL`, `VIRT_ADDR`, `ALLOCATED`, `PROV_KEY`, `MMU_NOTIFY` with `fi_mr_refresh`, | device memory `mtl_mem_import_device` (`MTL_LATER`) |
|  | `RMA_EVENT`/`ENDPOINT` needing `fi_mr_enable`, `HMEM`, `RAW`, `COLLECTIVE`; `BASIC`, `SCALABLE`, `UNSPEC` deprecated); |  |
|  | since 1.22 `desc` is valid or NULL and NULL means "handled by the provider"; registration is costly, with a cache (`FI_MR_CACHE_MAX_SIZE`, `_MAX_COUNT`, `_MONITOR` = userfaultfd, memhooks, |  |
|  | kdreg2, disabled); closing an MR in use is undefined ("may attempt to access an invalid memory region"), EP-bound MRs close first (`-FI_EBUSY`), no refcount for in-flight local ops |  |
|  | `fi_mr_reg(domain, buf, len, access, offset, requested_key, flags, &mr, context)`, `fi_mr_regv(…iov, count…)`, `fi_mr_regattr(domain, attr, flags, &mr)`, `fi_mr_desc(mr)`, `fi_mr_key(mr)`, `fi_mr_refresh(mr, iov, count, flags)`; |  |
|  | `fi_mr_attr`: union `mr_iov` or `dmabuf`, `iov_count`, `access`, `offset` (must be 0), `requested_key`, `context`, `auth_key*`, `iface`, a `device` union (`cuda, ze, neuron, synapseai, rocr`), `hmem_data` (must be NULL), `page_size`, `base_mr`, `sub_mr_cnt`; |  |
|  | `iface` "is ignored unless the application has requested the FI_HMEM capability"; dmabuf is selected by `FI_MR_DMABUF` (commented out at `1ULL << 40` in `fabric.h`, so defined elsewhere *(inferred)*); |  |
|  | `mr_mode`: `LOCAL` even local buffers registered and `desc` passed; `VIRT_ADDR` peers address by VA, not offset; `ALLOCATED` pages backed at registration, the app must not free or remap them; `PROV_KEY` the provider picks the key; `HMEM` device buffers only through `fi_mr_regattr` with `iface`/`device`; |  |
|  | new ops against a closed MR fail; a base MR cannot close while sub-MRs exist; the registration cache is used by "simply not setting the relevant mr_mode bits", `FI_MR_CACHE_MAX_COUNT = 0` disables it |  |
| data ops | `fi_send(ep, buf, len, desc, dest, context)`, `fi_sendv`, `fi_sendmsg`, `fi_inject`; | took push (`mtl_tx_acquire` → `mtl_tx_submit`, D-02), a plain 64-bit cookie, one stated terminal result (D-18), copy-on-submit `mtl_tx_write` for ST30/40/41, |
|  | flags `FI_COMPLETION` (1<<24), `FI_MORE` (1<<18, doorbell batching), `FI_INJECT` (1<<25, buffer reusable on return, up to `inject_size`), `FI_INJECT_COMPLETE` (1<<26, reusable; | `-MTL_EAGAIN` as the only back-pressure code; no batch hint |
|  | "does not indicate that the data has been transmitted"), `FI_TRANSMIT_COMPLETE` (1<<27; |  |
|  | unreliable: "delivered to the fabric"), `FI_DELIVERY_COMPLETE` (1<<28, reliable only), `FI_MATCH_COMPLETE`, `FI_COMMIT_COMPLETE`, `FI_FENCE`, `FI_MULTICAST`, `FI_MULTI_RECV`; |  |
|  | "a completion indicates that it is safe to reuse the buffer"; **no man page states a default completion level**; |  |
|  | `context` returned verbatim (`FI_CONTEXT` makes it provider scratch: "might appear as stack corruption"); |  |
|  | `-FI_EAGAIN` is back-pressure, and with manual progress the app must read completions after it |  |
|  | `fi_sendv(ep, iov, desc[], count, dest, ctx)`; `struct fi_msg {msg_iov, desc, iov_count, addr, context, data}`; the simple calls use the endpoint's default `op_flags`, `*msg` calls take flags; `desc[]` one per iov segment, capped by `tx_attr.iov_limit`; |  |
|  | `FI_COMPLETION` matters only on an endpoint bound with `FI_SELECTIVE_COMPLETION`, "or this flag is ignored"; `FI_MORE`: a delaying provider "must ensure that all previously delayed calls be flushed when an error is returned from a new call"; `fi_inject()` suppresses the success completion, a failure still gives an error entry; |  |
|  | `FI_TRANSMIT_COMPLETE` on unreliable endpoints also means "no longer dependent on local resources"; `FI_DELIVERY_COMPLETE` "processed by the destination endpoint(s)"; `FI_MATCH_COMPLETE` matched to a posted buffer; `FI_COMMIT_COMPLETE` persisted (experimental); |  |
|  | `FI_FENCE` waits for prior ops to the same peer; `FI_MULTICAST` destination from `fi_mc_addr()`; `FI_MULTI_RECV` one buffer takes many messages, with a final completion "even under selective completion"; every level already means "buffer reusable", levels only add guarantees beyond reuse; |  |
|  | `fi_setup.7` recommends "the provider's default flags for best performance"; `context` is ignored when no success completion will be generated; `-FI_EAGAIN` = the provider "currently lacks the resources", reading completions after it is required with manual progress and "strongly recommended" with AUTO; |  |
|  | queue depth is `tx_attr.size`, but there is "not necessarily a one-to-one mapping between a transmit operation and a queue entry" |  |
| completion queues | entry formats `CONTEXT` ⊂ `MSG` ⊂ `DATA` ⊂ `TAGGED`; `fi_cq_read` batched, `fi_cq_sread(…, timeout)` (negative = infinite); a 0-count read drives manual progress; | took the results ring that cannot overflow (capacity = pool, `MTL_BLOCKED_RESULTS`; D-03), status inline, record size per call (D-42); |
|  | failures go to an error queue: reads return `-FI_EAVAIL` until `fi_cq_readerr` (`err`, `prov_errno`, `err_data`); | rejected the error queue (a corrupted RX frame is data with a verdict) and fatal overrun |
|  | codes are errno values plus fabric codes from 256 (`FI_EAVAIL`, `FI_ETRUNC`, `FI_EOVERRUN`, `FI_ENORX`, …); out-of-order completion; **overrun is fatal** (`FI_EOVERRUN`); |  |
|  | `FI_RM_ENABLED` fails posts with `-FI_EAGAIN`, `FI_RM_DISABLED` makes overruns fatal to every endpoint on the CQ |  |
|  | `fi_cq_sread` returns early when signalled (`fi_cq_signal`); `struct fi_cq_err_entry {op_context, flags, len, buf, data, tag, olen, err, prov_errno, err_data, err_data_size, src_addr}`: `err` a positive fabric errno, `prov_errno` "a debugging aid", |  |
|  | `err_data` a caller buffer filled up to `err_data_size` (legacy: a provider buffer valid until the next read), printed by `fi_cq_strerror`; under selective completion unknown fields of an error entry (`op_context`) are NULL/0; |  |
|  | CQ `size` is "the minimum size", 0 the provider default; an overrun CQ keeps returning valid entries, then `FI_EOVERRUN`, and is unusable (completions lost); `FI_RM_ENABLED` may fail the post or retry internally; with selective completion "CQ space for failures is not reserved", so the CQ must be at least as large as the bound queues |  |
| selective completion | bind with `FI_SELECTIVE_COMPLETION`; "Operations that fail asynchronously will still generate completions"; a CQ is required even when successes are suppressed | took results opt-in for library pools, always for app memory (`MTL_SESSION_RESULTS`, D-77) |
| waiting | `FI_WAIT_NONE`, `UNSPEC`, `FD` (select/poll/epoll), `YIELD`, `SET` and `MUTEX_COND` deprecated; `fi_control(FI_GETWAIT)`; | took one wait handle per session (`mtl_session_get_wait_handle`) and arming on `-MTL_EAGAIN` (mtl.h R2), so no trywait call exists; hints that may be ignored become options |
|  | `fi_trywait` must succeed before blocking or the wake can be lost, and returns `-FI_EAGAIN` when events are queued (not deprecated); |  |
|  | `FI_CQ_COND_THRESHOLD` and `signaling_vector` are hints that may be ignored |  |
|  | `FI_WAIT_NONE` is the default and forbids `sread`; with `FI_WAIT_UNSPEC` the app is "not guaranteed to retrieve the underlying wait object"; `FI_WAIT_YIELD` spins; `FI_AFFINITY` + `signaling_vector` is an interrupt-CPU hint |  |
| counters | `fi_cntr_read`, `readerr`, `add`, `set`, `fi_cntr_wait(threshold, timeout)` (`-FI_ETIMEDOUT`; an error change unblocks); `FI_CNTR_EVENTS_COMP` or `BYTES`; they count every success, also under selective completion; "a small, but undefined, delay" before the value updates | took cumulative counters in the stats registry (D-80); no wait-on-counter |
|  | also `fi_cntr_adderr`, `fi_cntr_seterr`; used for credit-based flow control |  |
| event queue | control-plane events (CM, MR, AV, joins, async errors), separate from CQs "for performance reasons", often software; | took per-session events and shared queues (`mtl_session_read_events`, `mtl_queue_*`, D-79), bounded and coalescing; rejected user posts (removed in D-79) |
|  | `FI_CONNREQ`, `FI_CONNECTED`, `FI_SHUTDOWN`, `FI_MR_COMPLETE`, `FI_AV_COMPLETE`, `FI_JOIN_COMPLETE` (`FI_NOTIFY` is in `fi_eq.h`, not the man page); one event per read, `FI_PEEK`; |  |
|  | `fi_eq_write` with `FI_WRITE`; overrun fatal; async MR and AV deprecated in 2.0; a multicast join completes asynchronously; |  |
|  | a maintainer in PR #5566: "Reading the EQ isn't supposed to be a fast path operation" |  |
|  | CM events use `struct fi_eq_cm_entry {fid, info, data[]}`; `FI_MR_COMPLETE`, `FI_AV_COMPLETE`, `FI_JOIN_COMPLETE` use `struct fi_eq_entry {fid, context, data}`; `fi_eq_readerr` as for CQs; `fi_eq_sread(…, timeout)`; |  |
|  | "Applications cannot issue multicast transfers until receiving notification that the join operation has completed" (`fi_cm.3`); the MTL analogue is the flow state `MTL_FLOW_JOINING` → `MTL_FLOW_JOINED` / `MTL_FLOW_JOIN_FAILED` (mtl.h) |  |
| peer API | `fi_peer(3)`: one provider writes into another's CQ; "developmental", "not designed for direct use by applications"; `fi_import_fid` details "outside the scope" | app-supplied completion sinks are not a stable libfabric idea; MTL's dispatch threads (`mtl_queue_dispatch_start`) serve frameworks instead |
| progress | `FI_PROGRESS_AUTO` (may "create hidden threads"), `MANUAL` (only inside fabric calls; "select/poll do not count"), `CONTROL_UNIFIED` (lets the provider drop locks); 2.0 "Simplify progress definition" | MTL is AUTO with visible pinned threads; manual progress `mtl_sched_run_once` is `MTL_LATER`; unsafe for hardware-paced video |
|  | internal progress threads cost measurably; "The manual progress model can avoid this overhead", but "it is critical that the application thread call into libfabric in a timely manner" (`fi_setup.7`); `CONTROL_UNIFIED` removes all locking in the data progress path only together with `FI_THREAD_DOMAIN` or `FI_THREAD_COMPLETION` |  |
| threading | `FI_THREAD_SAFE` (required of all providers), `DOMAIN`, `COMPLETION`; `ENDPOINT` and `FID` deprecated (2.0); control calls thread safe unless `FI_PROGRESS_CONTROL_UNIFIED` | took the `COMPLETION` promise: acquire and release MP-safe, one submitter unless `MTL_SESSION_MT_SUBMIT`, `MTL_SESSION_SINGLE_READER` (D-50) |
|  | `fi_setup.7` recommends `FI_THREAD_SAFE` or `FI_THREAD_DOMAIN` and warns "providers may still implement more serialization than is needed"; |  |
|  | resource management: `FI_RM_ENABLED` also retries missing RX buffers on reliable endpoints internally; under `FI_RM_DISABLED` a queue overrun is "fatal to the context" and the endpoint is disabled until re-enabled |  |
| cancel, close | `fi_cancel(fid, context)`: async, bounded, `FI_ECANCELED` entry, not guaranteed, needs a context; | took "every accepted unit gets a result": `MTL_STOP_FLUSH`, `mtl_tx_withdraw`, `mtl_session_close` returning 1 while leases are out (D-36); rejected silent discard |
|  | `fi_close(ep)`: "Discarded operations will silently be dropped, with no completions reported"; a critical error discards pending ops; |  |
|  | **no `fi_ep_flush`** (only `FI_FLUSH_WORK` in `fi_trigger.3`); scalable EP with open contexts `-FI_EBUSY` |  |
|  | `fi_cancel` posts no entry of its own ("No specific entry related to fi_cancel itself"); ops without a valid context "cannot be canceled"; when several ops match a context one is cancelled, which one is provider-defined; `-FI_EAGAIN` means retry after progress; |  |
|  | after `fi_close(ep)` with outstanding ops, buffers must not be reused until a completion arrives or `fi_close` returns, and the provider may also discard CQ entries already queued |  |
| ABI | `FI_VERSION(major, minor) = (major << 16) \| minor`, passed hard-coded to`fi_getinfo` (passing `FI_MAJOR_VERSION` risks uninitialised new fields; | took `struct_size` in every input, `MTL_API_VERSION`, `mtl_version_num()`, an experimental DSO with its own soname and version node (D-23); rejected per-version shims unless needed |
|  | a newer version than the library is `-FI_ENOSYS`); API version vs symbol-version ABI with the rule`(va1 ≤ va2 ≤ va3) && (vb1 ≤ vb2 ≤ vb3)`; |  |
|  | append-only structs, frozen old layouts (`fi_info_1_0`),`COMPAT_SYMVER` shims for `FABRIC_1.0/1.1/1.2/1.3/1.7/1.8/1.9`; |  |
|  | ABI 1.1 = libfabric 1.5 (`api_version`,`mr_mode`bitfield), 1.2 = 1.7 (`nic`), 1.3 = 1.9 (`tclass`), 1.4 = 1.12 (`fi_tostr_r`), 1.5 = 1.13 (`fi_open`), 1.6 = 1.14 (`fi_log_ready`), |  |
|  | 1.7 = 1.20 (`max_ep_auth_key`), 1.8 = 2.0 (`fi_fabric2`,`max_group_id`; 2.0 kept "1.8 ABI compat"), 1.9 = 2.5 (`max_cntr_value`); |  |
|  | enum`_LAST` values never change, removed flags stay as zero stubs, deprecations warn by `_Pragma`; only library-allocated`fi_info` |  |
|  | `fi_version()` returns the library version; the `COMPAT_SYMVER` shims convert old hints up ("Any new fields in the latest definition will be zeroed") and cast results back to the old layout |  |

MTL today: `mtl_init(struct mtl_init_params*)` and every `*_ops` struct are app-allocated with no size or version field,
and `lib/` has no linker map, so any new field is an ABI break *(inferred)*; `ST20P_RX_FLAG_DMA_OFFLOAD` "could fallback to
CPU if no DMA device is available" (`st_pipeline_api.h`, `experimental/st20_combined_api.h`) and pacing falls from rate
limit to TSC without a report; completions are lcore callbacks (`notify_frame_done`, `notify_frame_available`,
`notify_frame_ready`, "only non-block method can be used within this callback"); events go through `notify_event`
(`ST_EVENT_VSYNC`, `RECOVERY_ERROR`, `FATAL_ERROR`); ext frames add a third release step (`..._EXT_FRAME_MANUAL_RELEASE`,
`st20p_tx_notify_ext_frame_free()`); whether `st20p_*_get/put_frame` are safe from several threads was *(unknown)*.

### 3.2 Its pain points, and the lesson taken

| Pain point (evidence) | Lesson in the design |
|---|---|
| combinatorial negotiation: caps × modes × `mr_mode` × attributes; 2.0 removed several (`fi_getinfo.3`, `NEWS.md`) | no mode bits; grants reported; the app never adapts its buffer layout |
| too many threading and progress models, later collapsed | two threading promises, one library-owned progress model |
| wait sets and poll sets added, then deprecated | one wait handle per session for epoll; no aggregation object |
| provider inconsistency: only RDM and basic send/recv are universal, `FI_MULTICAST` only in `udp`, `efa` vs `efa-direct` (feature matrix) | capability query per port and backend; a fallback is reported, never silent |
| fatal CQ and EQ overrun; `RM_DISABLED` | rings sized by construction; `-MTL_EAGAIN` always |
| silent discard on close | every buffer returned with a result |
| `FI_CONTEXT` scratch owned by the provider, allocated by the app | the cookie is opaque; MTL keeps its own state |
| MR close with in-flight ops undefined | refcount, close returns 1 |
| error queue separate from successes (`-FI_EAVAIL` gate) | inline status; media quality is not an error |
| no stated default completion level | one default, stated: the terminal result after storage and transport are done |
| "dozens of checks" in the fast path for generic flags (`fi_intro.7`) | per-session defaults fixed at create (libfabric's `op_flags`); few per-submit flags |
| a very large surface (RMA, atomics, tagged, collectives, AV sets, triggers) *(inferred)* | a minimal object set for one-way isochronous streams |

### 3.3 Research note 09's questions and where they were answered

| # | Question | Options weighed | Answer |
|---|---|---|---|
| 1 | push or pull for TX? | its Q-ID in [questions.md](questions.md) | push, with an explicit underrun policy (D-02, D-13) |
| 2 | queue or callback completions? | its Q-ID in [questions.md](questions.md) | queues; callbacks only on library dispatch threads (D-04, R6) |
| 3 | which completion levels, which default? | (a) one level "buffer released"; (b) that default plus opt-in "on wire with timestamp"; (c) both in one entry with two timestamps | one terminal result with observed times (D-18; `sent_tai_ns`, `observed_first_tai_ns[leg]`) |
| 4 | media degradation a status or an error? | (a) inline status, error path only for session failure; (b) the libfabric split | status inline (`MTL_RX_INCOMPLETE`, `MTL_TX_DROPPED` + reason) |
| 5 | close contract for user buffers? | (a) stop flushes every buffer, destroy guarantees no later callback; (b) destroy blocks until all buffers return; (c) libfabric: assume free when destroy returns | every unit gets a result; close returns 1 until leases return (D-36) |
| 6 | a memory registration object? | (a) a registration object, mandatory for zero copy; (b) the same, raw-IOVA frames kept as a fast path; (c) keep `mtl_dma_map` | regions, slots named by index, raw IOVA only through `mtl_mem_iova` (D-16, D-76) |
| 7 | requested vs granted? | (a) libfabric rule: non-zero = required, zero = auto, grant reported; (b) per-feature REQUIRED/PREFERRED/OFF; (c) today's fallback, always reported | yes, `REQUIRE`/`PREFER`/`OFF`, grants reported (D-19) |
| 8 | ABI versioning mechanism? | its Q-ID in [questions.md](questions.md) | `struct_size` + experimental DSO (D-23, D-74) |
| 9 | app-driven progress? | its Q-ID in [questions.md](questions.md) | not in v1 (Q-THR-3); `mtl_sched_run_once` later |
| 10 | threading promise? | its Q-ID in [questions.md](questions.md) | declared per session (D-50) |
| 11 | where does VSYNC go? | its Q-ID in [questions.md](questions.md) | `mtl_tx_next_slot` and the opt-in `MTL_EVENT_EPOCH_TICK` (timing.md §6.5) |
| 12 | extension mechanism? | (a) named versioned extension tables; (b) flags in the main ops struct; (c) separate experimental headers | options for knobs; `mtl_open_ext` later |

## 4. NVIDIA Rivermax

Read from source, not from the licensed Rivermax User Manual (it needs a developer login; `docs.nvidia.com/networking/display/rivermax`
returns 404): NVIDIA/rivermax-examples (`api_demo/`, `legacy/`, at `4dca2694`, 2026-06-11), NVIDIA/rivermax-dev-kit
(`source/`, at `ebbb89e4`, 2026-04-23), NVIDIA/DeepStream `gst-plugins/gst-nvdsudp/` (new `rmx_*` and `legacy_api/`
`rmax_*`, at `0a63ef8b`; its README pins Rivermax 1.80.x), Unreal Engine's `RivermaxCore` plugin through the public
unofficial mirror orgitcog/u9n (at `97a2175e`; evidence of how a large integrator uses the API, not NVIDIA
documentation), the DOCA Rivermax programming guide (DOCA 3.5.0, updated 2026-09-01), and the product page and FAQ.
`rivermax_api.h` is not public; UE's generated `RivermaxWrapper.h` (a table of 131 `rmx_*` entry points) served as the
prototype list. The Holoscan advanced-network Rivermax manager was not found in `nvidia-holoscan/holohub`.

### 4.1 Facts

| Area | Rivermax | Source |
|---|---|---|
| generations | legacy `rmax_*` (`rmax_init`, `rmax_out_create_stream_ex`, `rmax_out_get_next_chunk`, **stream-level** `rmax_out_commit(id, time, flags)`, `rmax_in_get_next_chunk(id, min, max, timeout, flags, &comp)`, `RMAX_ERR_*`) and current `rmx_*` (app-allocated param structs set by `rmx_*_init*()` and `rmx_*_set_*()`, | DS legacy sink/src; UE `RivermaxWrapper.h:12-142` |
|  | app-owned chunk handles, `RMX_*` codes); RX moderation moved from a per-call argument to `rmx_input_set_completion_moderation(id, min, max, timeout_usec)` |  |
|  | legacy also had `rmax_in_create_stream` and `rmax_set_clock`; current chunk handles are `rmx_output_media_chunk_handle`, `rmx_input_chunk_handle`; the legacy stream-level `rmax_out_commit(id, …)` is probably the origin of today's FIFO commit rule *(inferred)* |  |
| ABI | version-suffixed exports (`rmx_output_media_commit_chunk_v1`, …), init `_rmx_init(const rmx_version*)` (so `rmx_init()` is presumably a macro passing `RMX_VERSION_*` *(inferred)*); | UE `RivermaxWrapper.cpp`, `RivermaxOutStream.cpp:1371-1372` |
|  | many accessors inline in the header, so completion structs have a fixed public layout (UE casts `rmx_output_chunk_completion*` to `…_metadata*` to read `user_token` and `timestamp`) |  |
|  | header-inline accessors seen in UE: `rmx_output_media_get_chunk_strides`, `rmx_input_get_completion_ptr`, `rmx_input_get_packet_size`, `rmx_input_get_packet_timestamp`; lesson: ABI stability from caller-allocated parameter blocks set by `*_init()` plus versioned exports, not from opaque heap objects (relevant to the ABI fragility of today's `*_ops` structs) |  |
| library | `rmx_set_cpu_affinity(mask, count)` pins the one internal thread **before** `rmx_init`; `rmx_enable_system_signal_handling()` before init makes blocking calls return `RMX_SIGNAL`; process-global `rmx_init`/`rmx_cleanup` (the Dev Kit refcounts it); | RDK `facade.cpp:96-163`, `rt_threads.cpp:659-695`; DOCA |
|  | `rmx_apply_lib_param` with string `name=value` (unknown name `RMX_NOT_IMPLEMENTED`, bad value `RMX_INVALID_PARAM_2`; names *(unknown)*); device list and getters; **devices are selected by local IP** (`rmx_retrieve_device_iface_ipv4`); `rmx_enquire_device_capabilities` (contents *(unknown)*; DOCA names PTP clock support) |  |
|  | `rmx_mark_cpu_for_affinity(mask, cpu)` builds the mask; lib params `rmx_init_lib_param`, `rmx_set_lib_param_name/value/forced`, `rmx_apply_lib_param`; `rmx_get_version_string()`, `rmx_get_version_numbers()`; |  |
|  | devices: `rmx_get_device_list`, `rmx_get_device_count`, `rmx_get_device`, `rmx_get_device_interface_name/ip_count/ip_address/mac_address/id/serial_number`, `rmx_free_device_list`; `rmx_retrieve_device_iface(&iface, &rmx_ip_addr)` and `_ipv4`; `rmx_apply_device_config`/`rmx_revert_device_config` (prototypes only, UE `RivermaxWrapper.h:34-36`); |  |
|  | streams bind to a NIC by local address: input `rmx_input_set_stream_nic_address(sockaddr)`, generic output `rmx_output_gen_set_local_addr`, media output by the source address in the SDP (`a=source-filter`) (RE `receive.cpp:56`, `generic_send.cpp:113`) |  |
| objects | process → clock (system, user callback, NIC PTP), memory registrations (addr, len) × device → mkey, output media stream (SDP + memory blocks), output generic stream, input stream with flows attached at runtime, an event channel per stream, an out-of-band stats consumer; stream IDs are plain values; | RDK `media_sender_io_node.cpp:361-449` |
|  | no object groups TX and RX or audio and video: A/V alignment is the app's (a shared start time through an app-side `ISynchronizer`) |  |
| media TX creation | `rmx_output_media_init`, `assign_mem_blocks`, `set_sdp` (the whole SDP text), `set_idx_in_sdp` (which `m=`), `set_packets_per_chunk`, `set_stride_size`, `set_packets_per_frame`, | RE `media_send.cpp:68-84`; RDK `video_settings_calculator.cpp` |
|  | optional `set_pcp/dscp/ecn`, `set_source_ports`, `set_tx_adaptive_scheduling_factor` (semantics *(unknown)*), `create_stream`; |  |
|  | the SDP is the single source of addresses, NIC, format, rate and sender type (the Dev Kit generates `TP=2110TPN\|TPNL\|TPW`,`ts-refclk`,`source-filter`,`mid`/group); |  |
|  | runtime changes only for QoS (`rmx_output_update_dscp`,`_ecn`); which SDP attributes drive pacing *(unknown)* |  |
|  | after create the app reads back resolved addresses: `rmx_output_media_init_context(&ctx, id)`, `rmx_output_media_set_context_block(&ctx, sdp_idx)`, `get_local_address`/`get_remote_address` (UE `RivermaxOutStream.cpp:194-206`); also `rmx_output_get_chunk_count`; |  |
|  | media streams have no destination update (generic streams have `rmx_output_gen_update_remote_addr`); needing `packets_per_frame` next to the SDP suggests the rate comes from frame period / packets per frame *(inferred)*; |  |
|  | Dev Kit SDP `a=fmtp`: `sampling`, `width`, `height`, `exactframerate`, `depth`, `TCS`, `colorimetry`, `PM`, `SSN`, `TP`; `a=mid` and a group for multi-flow, 2022-7 `a=mid:a`, `a=mid:b`; how `set_idx_in_sdp` and `set_source_ports` work with several paths is *(unknown)*; DS calls `set_idx_in_sdp` once per path |  |
| memory hierarchy | stream → memory blocks → chunk count, sub-blocks (1 = contiguous, 2 = header/data split) → packet layout → chunk = `packets_per_chunk` strides; | RE `hds_media_send.cpp`, `dynamic_media_send.cpp:70-163`; RDK `media_stream.cpp:108-173` |
|  | 2022-7 via `get_dup_sub_block` with one mkey per path; the Dev Kit uses one block of 10 frames (UE: one block "potentially improving SDK performance"); strides cache-line aligned; |  |
|  | HDS: sub-block 0 RTP headers (20 B for 2110-20 single SRD), 1 payload; dynamic packet sizes for ST 2110-40 (`set_chunk_packet_count` before `get_next_chunk`, |  |
|  | sizes in `get_chunk_packet_sizes`) |  |
|  | calls: `rmx_output_media_init_mem_blocks`, `set_chunk_count(block, C)`, `set_sub_block_count(block, 1 \| 2)`, `set_packet_layout(block, sb, uint16_t sizes[C × ppc])`, `get_sub_block` → `rmx_mem_region {addr, length, mkey}`, `get_dup_sub_block` → `rmx_mem_multi_key_region {addr, length, mkey[RMX_MAX_DUP_STREAMS]}`; |  |
|  | a stride is a fixed slot per packet per sub-block (`set_stride_size(params, sb, bytes)`), hardware stride and alignment requirements *(unknown)*; Dev Kit `media_units_in_mem_block` = 10; UE CVar `Rivermax.Output.UseSingleMemblock` |  |
| chunk size | Dev Kit default 4 video lines per chunk, a divisor of packets per frame; payload the largest pgroup-aligned line fraction ≤ 1440 B: 1080p 4:2:2 10-bit = 4 packets per line, 1200 B + 20 B header, 4320 packets per frame, 16 per chunk | RDK `video_settings_calculator.cpp:150-205` |
|  | chunk size trades CPU work per commit against how finely the app can fill just in time and how soon memory is reusable *(inferred)* |  |
| send loop | per frame `send_time = t0 + k × period`; `get_next_chunk` spinning on `RMX_NO_FREE_CHUNK`; the app writes RTP headers and payload per stride; only chunk 0 carries the time, the rest 0 (`SEND_IMMEDIATELY_AFTER_PENDING_CHUNKS_TIMESTAMP = 0`); commit spins on `RMX_HW_SEND_QUEUE_IS_FULL` | RE `media_send.cpp:117-148`; UE `RivermaxOutStream.cpp:472-535` |
|  | to repeat a frame UE skips `(N_buffers − 1) × chunks_per_frame` chunks to land back on the frame just sent (`RivermaxOutStream.cpp:815-821`, `:1177-1202`); senders optionally track the first or second-to-last chunk of each frame for diagnostics |  |
| commit time | ns in the configured clock: system clock = **UTC**, PTP clock = NIC PHC (**TAI**), so DS and the player subtract the leap-second offset when **not** on the PTP clock; the time of the first packet of the chunk, a `uint64_t`; | RDK `video_settings_calculator.cpp:86-130`; UE `:497-609`; DS `gstnvdsudpsink.c:697-770`; RE `rivermax_player.cpp:1596-1604` |
|  | **the app adds TRO** (`alignment_point + TRO`, `tro = mult·T_frame − 2·T_RS`); a commit too close fails, so integrators pass 0 when `time ≤ now + 600 ns` (UE CVar; code *(unknown)*); |  |
|  | after a late frame UE calls `skip_chunks(h, 0)` "Otherwise, scheduling time / Tro isn't respected"; no lateness status: apps compare with `rmx_get_time()`; a late frame logs "Timeout occurred. Send time exceeded by …" |  |
| packet building | the app writes RTP (V/P/X/CC/M/PT, seq, timestamp, SSRC, extended sequence, SRD) and computes the RTP from its frame time, not from the commit time; Rivermax builds L2–L4 from the SDP *(inferred)* | RDK `rtp_smpte_2110_20_packet_buffer_writer.cpp:35-90`, `rtp_video_send_stream.cpp` |
| FIFO commit | "If multiple chunks were acquired … the **oldest acquired chunk will be sent**, not the one whose method was called"; the ring is strictly sequential; repeat or drop by `rmx_output_media_skip_chunks(h, n)`; UE computes future stride addresses itself | RDK `media_chunk.h:115-131`; UE `:630-633`, `:815-821` |
|  | the generic stream carries the same "oldest acquired chunk" warning (`generic_chunk.h:138`) |  |
| pacing | by the NIC; the app only submits ahead; the PTP clock handler needs "ConnectX-6 Dx or DPU devices only"; the last chunk is reported complete only "after the TRoffset gap", and stats count "dummy WQEs", so gaps are dummy work entries *(inferred)* | product page; UE `:482`; RDK `statistics_reader.cpp` |
| teardown, 2022-7 TX | `cancel_unsent_chunks`, then `destroy_stream` retried while `RMX_BUSY`; one stream sends both legs (an SDP `m=` per path, memory registered per NIC) | RDK `media_sender_io_node.cpp:203-238`, `:594-613` |
| generic TX | local and optional remote address, max packets per chunk, optional HW rate (`init_rate(bps)`, `set_rate_max_burst`, `set_rate_typical_packet_size`); scatter-gather `append_packet_to_chunk`; per-chunk destination; runtime `update_rate`, `update_remote_addr`; app-registered memory required | RE `generic_send.cpp:71-160`; RDK `generic_stream.cpp:49-213` |
|  | create also takes `set_max_sub_blocks` and `set_pcp/dscp/ecn`; the rate is attached by `rmx_output_gen_set_rate(&p, &r)`; data path `rmx_output_gen_append_packet_to_chunk(&h, rmx_mem_region sub_blocks[], count)` (each region with its own mkey), optional `rmx_output_gen_set_chunk_remote_addr`, |  |
|  | then `rmx_output_gen_commit_chunk(&h, time)`, 0 = as soon as possible; the payload is the UDP payload and Rivermax adds L2–L4; generic streams are rate paced (a HW token bucket), not frame paced *(inferred)*; |  |
|  | MTL today has no public equivalent (internal datapath queues, the `ld_preload` UDP shim); the unified generic essence is RTP only, raw UDP on demand (Q-PKT-3) |  |
| TX tracking | opt-in: `mark_chunk_for_tracking(h, token)` after `get_next_chunk`; `poll_for_completion` (`RMX_OK` or `RMX_BUSY`, drains untracked ones first); `get_last_completion` → token and "time of completing the chunk transmit set by HW", in the `rmx_get_time` domain; in order; reuse is implied by `get_next_chunk` succeeding | RDK `media_chunk.cpp:141-165`; UE `:1373-1381` |
|  | getters `rmx_output_get_completion_user_token(c)`, `rmx_output_get_completion_timestamp(c)`; generic `rmx_output_gen_mark_chunk_for_tracking`, `_poll_for_completion`, `_get_last_completion`; the Dev Kit latency tool subtracts the app send time from the completion timestamp (`generic_latency_io_node.cpp:342-366`); no `RMX_OUTPUT_MEDIA_*` status code exists in public sources |  |
| RX | `rmx_input_init_stream(APP_PROTOCOL_PACKET or RAW_PACKET)`, NIC address, `CREATE_INFO_PER_PACKET`, timestamp format `RAW_NANO` or `SYNCED` (DOCA: default raw counter), capacity in packets, | RE `receive.cpp:53-143`; DOCA; UE `RivermaxInputStream.cpp:253-1242` |
|  | sub-blocks, entry sizes, `determine_mem_layout`; `RTP_SMPTE_2110_20_DYNAMIC_HDS`; HDS may be absent; |  |
|  | **HW placement** by 16- or 32-bit sequence (`…SEQN_PLACEMENT_ORDER`): "should not expect the packet receive events … to arrive in any specific order"; |  |
|  | flows attach and detach at runtime, detached flows drain; `get_next_chunk` returns 0..max packets with first and last timestamps and per-packet size, flow tag and time; |  |
|  | moderation semantics disagree between DOCA ("busy wait") and the Dev Kit; every example uses `(0, 5000, 0)` and sleeps 100–300 µs; |  |
|  | **no release call**: strides are valid until the ring wraps *(inferred)*; no frame assembly; an event channel fd (epoll) or IOCP with `rmx_request_notification` |  |
|  | `APP_PROTOCOL_PACKET` delivers from the UDP payload (RTP header first), `RAW_PACKET` whole frames with network headers; `RTP_SMPTE_2110_20_DYNAMIC_HDS` splits at the variable RTP + SRD header boundary *(inferred from the name)*; HDS is absent when the header block length is ≤ 0 ("Header data split not supported for device"); |  |
|  | layout: `rmx_input_set_mem_capacity_in_packets` (may be rounded; read back with `get_mem_capacity_in_packets`), `set_mem_sub_block_count`, `set_entry_size_range(sb, min, max)` or `set_entry_uniform_size`, `determine_mem_layout`, `get_stride_size`, `get_mem_block_buffer` (`{addr, length, mkey}`: leave it, or fill addr and mkey); |  |
|  | flows: `rmx_input_init_flow`, `set_flow_local_addr`, `set_flow_remote_addr`, `set_flow_tag`, `attach_flow`/`detach_flow`; one stream carries many flows (SSM multi-source, several destinations); |  |
|  | reading: `rmx_input_get_next_chunk` returns `RMX_OK` also with 0 packets; `get_completion_chunk_size`, `get_completion_ptr(c, sb)`, `get_completion_timestamp_first/last`, `RMX_INPUT_COMPLETION_FLAG_MORE`; per packet (`CREATE_INFO_PER_PACKET`) `get_packet_info`, `get_packet_size`, `get_packet_flow_tag`, `get_packet_timestamp`; |  |
|  | packet i is at `base + i × stride`; DOCA's event also returns the first packet's sequence number, a public getter is *(unknown)*; moderation: DOCA says the timeout is the µs of busy wait for at least `min` packets, the Dev Kit says timeout 0 busy-loops until `max` and `min = max = 0` avoids waiting; |  |
|  | event channel `rmx_init_event_channel`, `rmx_set_event_channel_handle`, `rmx_establish_event_channel`, armed by `rmx_request_notification` (seen on TX after `RMX_NO_FREE_CHUNK`, RX use *(inferred)*); `SYNCED` timestamps are what latency tools compare with `rmx_get_time` (clock domain *(inferred)*); |  |
|  | a consumer must finish or copy before `capacity` more packets arrive |  |
| 2022-7 RX (IPO) | one input stream per path over the **same** buffers; the NIC places a packet at slot `seq % capacity`, so a duplicate lands on the same slot; software releases a contiguous run after `max_path_differential` (50 ms in the app); sender restart after 100 ms idle; no library 2022-7 object | RDK `ipo_receive_stream.cpp:56-534` |
| clock | system (default, UTC), user callback (`rmx_set_user_clock_handler`), NIC PTP (`rmx_use_ptp_clock`, then spin on `rmx_check_clock_steady()`); `rmx_get_time(RMX_TIME_PTP, &ns)`; **no PTP servo of its own**: linuxptp or DOCA Firefly disciplines the PHC; UE falls back to the system clock | RDK `clock.cpp:31-72`; FAQ |
|  | user clock `rmx_init_user_clock`, `rmx_set_user_clock_handler(&p, uint64_t (*)(void* ctx))`, `rmx_set_user_clock_context`, `rmx_use_user_clock`; PTP `rmx_init_ptp_clock`, `rmx_set_ptp_clock_device(&p, &iface)`, `rmx_use_ptp_clock`, then spin on `rmx_check_clock_steady()` while `RMX_BUSY`; |  |
|  | on Windows the PHC is disciplined through BlueField / DOCA Firefly; other `rmx_time_type` values *(unknown)*; UE's system-clock fallback is `RivermaxManager.cpp:417-460` |  |
| memory | three provisioning modes: Rivermax-managed (addr NULL, `RMX_MKEY_INVALID`), app-allocated and Rivermax-registered, app-registered (`rmx_register_memory` → mkey; per device, so 2022-7 registers twice; one large registration for many streams helps HW caches); registrations outlive streams; | RE `memory_registration_media_send`; RDK `gpu.cpp:237-290` |
|  | GPUDirect only with HDS, detected from the pointer ("Rivermax will use GPUDirect mode seamlessly"); Dev Kit allocators Malloc, HugePage 2 MB/512 MB/1 GB, GPU, GPUHostPinned; DOCA recommends ≥ 800 × 2 MB hugepages; Dev Kit `MediaUnitPool` copies into chunks, so true zero copy means laying frames out as the block (UE does) |  |
|  | app-registered: `rmx_init_mem_registry(&rp, &iface)`, optional `rmx_set_mem_registry_option` (options *(unknown)*), `rmx_register_memory(&region, &rp)` → `region.mkey`, `rmx_deregister_memory(&region, &iface)`; app-allocated but Rivermax-registered: fill addr and length, `mkey = RMX_MKEY_INVALID`; |  |
|  | GPUDirect payload is CUDA VMM memory from `cuMemCreate` with `gpuDirectRDMACapable = 1`, headers in host memory (the FAQ confirms the split); hugepage allocations are rounded to the page size; GPU alignment from `gpu_query_alignment`; |  |
|  | the Dev Kit `MediaUnitBuffer` holds owned or borrowed, host or GPU memory, `MediaUnitPool` is a mutex and condvar pool; UE lays frames out as the block (`StreamMemory.BufferAddresses[frame_index]`, `RivermaxOutStream.cpp:646`) |  |
| errors | `RMX_OK`; `RMX_NO_FREE_CHUNK` (ring full: spin or notification); `RMX_HW_SEND_QUEUE_IS_FULL` (spin, DS sleeps 10 µs); `RMX_BUSY` (retry, 1 ms); `RMX_SIGNAL` (exit the loop); `RMX_HW_COMPLETION_ISSUE` (fatal for the stream); `RMX_CHECKSUM_ISSUE` (count, continue); `RMX_NOT_INITIALIZED`, `NOT_IMPLEMENTED`, `INVALID_PARAM_2` (per-argument codes *(inferred)*); | RE, RDK, UE, DS |
|  | the full enum *(unknown)* |  |
|  | sources: `RMX_NO_FREE_CHUNK` also from `skip_chunks`; `RMX_BUSY` also from `check_clock_steady` and `request_notification`; `RMX_HW_COMPLETION_ISSUE` comes from `commit_chunk`; `RMX_CHECKSUM_ISSUE` from the legacy generic receiver |  |
| stats, threads | out-of-band consumer by process ID (`rmx_stats_*`: session, TX/RX queue, time messages; dummy WQEs, CRC errors); no per-stream "get stats"; one internal thread (what it does *(unknown)*); app threads drive everything; each stream used from one thread, Dev Kit IO nodes pinned at RT priority; | RDK `statistics_reader.cpp:40-270`; DOCA |
|  | concurrent calls on one stream *(unknown)*; root or `cap_net_raw,cap_sys_nice` |  |
|  | consumer calls `rmx_stats_init_config`, `rmx_stats_config_set_process_id`, `rmx_stats_config_register_stats_type(RMX_STATS_SESSION_START \| STOP \| RUN \| TX_QUEUE \| RX_QUEUE \| TIME)`, `rmx_stats_create_consumer`, `rmx_stats_consumer_pop_message` → typed messages: |  |
|  | committed chunks and strides, user, free and busy chunks; TX packets, bytes, packet WQEs, dummy WQEs, free WQEs; RX packets, bytes, used strides, CRC errors; session start W × H × fps; |  |
|  | Dev Kit IO nodes run at `RMAX_THREAD_PRIORITY_TIME_CRITICAL − 1`; different streams on different threads is the documented pattern |  |
| app structure | start = now + 1 s aligned to the next frame plus TRO, shared by the process's streams; ring of ~10 frames; sleep until ~2 ms before send (`SLEEP_THRESHOLD_NS`), fill just in time; late → commit 0 and log; UE "EnableTimingProtection" skips an interval; continuous output repeats by `skip_chunks`; | RDK `media_sender_io_node.cpp:451-574` |
|  | receivers poll with sleeps and do assembly, loss detection and IPO themselves |  |

### 4.2 What Rivermax users find alien in MTL, and what MTL does not copy

- **Alien today:** no SDP entry point; callbacks from library threads instead of an app loop; the library builds RTP and
  paces; the frame, not the chunk, is the unit; ports by BDF with EAL, hugepages, VFs and MtlManager instead of a netdev
  with an IP and a licence; MTL works in TAI and computes TRO itself; RX delivers frames with explicit release.
- **What eases migration (taken):** the app-driven `acquire → fill → submit` loop with a pollable wait handle; packet
  units with Rivermax-like knobs (`MTL_UNIT_PACKETS`, D-82); row units ≈ chunks of lines; one documented clock domain
  (TAI); lateness as a per-unit result; regions shared by many sessions; port lookup by IP (`mtl_port_find`); an SDP parse
  helper (Phase 7, later).
- **Not copied:** the stream-level FIFO commit (identity mismatch); RX lifetime by ring wrap; optional-only TX completion;
  failing a commit < 600 ns ahead (MTL accepts and reports late); a vendor mkey in the public type; SDP as the only
  creation path (audio, ANC, fastmeta and tests need none).

### 4.3 Research note 10's questions and where they were answered

| # | Question | Options weighed | Answer |
|---|---|---|---|
| 1 | an SDP constructor? | its Q-ID in [questions.md](questions.md) | `mtl_sdp_parse` on public calls only, Phase 7 (D-94) |
| 2 | app-built RTP first-class? | its Q-ID in [questions.md](questions.md) | packet units on every essence and a generic RTP essence (D-82) |
| 3 | what a submission time means? | (a) the frame's alignment point (TAI), MTL adds TRO; (b) exact first-packet time, the app owns TRO; (c) both through an enum | media time; MTL adds TRO; `MTL_SUBMIT_EXACT` for exact first-packet time (timing.md §5.4) |
| 4 | late: reject, clamp or skip? | its Q-ID in [questions.md](questions.md) | per-session late policy with a result (D-13) |
| 5 | chunk = slice? | sub-frame submission as a frame with a "lines ready" watermark, or as separate chunk objects | row units on one frame buffer (`MTL_UNIT_ROWS`, timing.md §6.7) |
| 6 | buffer identity vs ring position? | confirm an explicit "repeat last" / "skip N" operation besides kept identity | identity kept: commit sends the lease committed; REPEAT_LAST later (Q-TIME-4) |
| 7 | RX packet-chunk mode? | (a) no packet mode; (b) packet mode with explicit release; (c) a ring view "valid until N more packets" | packet units with explicit release (`MTL_PKT_RX_LEND`) |
| 8 | waiting model? | callbacks, blocking calls or pollable fds; a pollable fd on every session, callbacks optional? | one wait handle per session; callbacks only on dispatch threads |
| 9 | device by IP? | IP lookup in instance or session creation, although DPDK-bound VFs may have no kernel IP | `mtl_port_find(mt, name, BDF or IP)`; a DPDK VF uses its configured IP |
| 10 | memory registration shape? | one region handle per port (2022-7 on two NICs) shared by sessions; GPU domain explicit or inferred from the pointer | regions mapped per port lazily or with `MTL_MEM_MAP_ALL`, shared by sessions; device memory explicit (`mtl_mem_import_device`, later) |
| 11 | mandatory completion with HW TX time? | mandatory completion, and with a HW TX time when the NIC gives one | results always for app memory; `MTL_TXR_SENT_HW` when the NIC stamps |
| 12 | ABI technique? | setters and versioned symbols (heavier, strong ABI) or size-tagged structs (lighter, familiar) | `struct_size` structs, not setters and versioned symbols |
| 13 | out-of-process stats? | its Q-ID in [questions.md](questions.md) | in-process only in v1 (Q-OBS-2) |
| 14 | licensed docs? | — | **Q-EXT-1**: an action, not a decision: someone with a Rivermax developer account confirms the unknowns (full `rmx_status`, the commit-time failure code, moderation where DOCA and the Dev Kit disagree, thread safety, which SDP attributes drive pacing, `tx_adaptive_scheduling_factor`) before Rivermax-like names freeze (Phase 0 exit), [questions.md](questions.md) |

## 5. Concept map for libfabric and Rivermax users

All three APIs share one shape: register memory once, get a slot, fill, post (`fi_send`, `commit_chunk`,
`mtl_tx_submit`), read a completion (`fi_cq_read`, `poll_for_completion`, `mtl_tx_reap`); the diagram is
[diagrams.md §3.7](diagrams.md#37-the-shape-shared-with-libfabric-and-rivermax). What only MTL has is time. The call map
in revision-4 names is [migration.md §13](migration.md#13-if-you-know-libfabric-or-rivermax); the rows below complete it.

| libfabric / Rivermax | Unified MTL | Different, and why |
|---|---|---|
| `FI_SELECTIVE_COMPLETION`, errors always reported | `MTL_SESSION_RESULTS` off for library pools; app memory always gets results | with imported memory the result is how the app learns its slot is free |
| `FI_THREAD_COMPLETION` | acquire and release MP-safe; one submitter unless `MTL_SESSION_MT_SUBMIT`; `MTL_SESSION_SINGLE_READER` elides the reaper lock | lock-free by contract |
| `FI_RM_ENABLED` | always: `-MTL_EAGAIN`, never fatal | |
| `FI_INJECT` | `mtl_tx_write` (copy essences), reported as `MTL_TXR_COPIED`, not a flag | |
| Rivermax memory block → sub-block → chunk → stride | pool → slot → planes; packet layout is MTL's | MTL builds RTP and packetises; header/payload split is a later memory-domain feature |
| one registration shared by many streams | one region, many sessions; an RX library pool is a region too (`mtl_session_get_pool_region`) | provider-neutral handles, not mkeys |
| only chunk 0 carries a time | a row unit's slot and RTP are fixed at its first submit; later submits of the same lease with a larger `used` publish rows | a Rivermax chunk of N lines is one row publish step for timing, but the chunk is separate memory with app-written RTP headers, while a row is a counter over one frame buffer |
| commit time 0 = "after the pending chunks" | `MTL_MEDIA_AUTO` with derived launch | back-to-back continuation |
| RX chunk reused for TX by the app | `unit.hold` keeps an RX slot until the TX unit is done (`mtl_tx_send_slot`, D-55) | zero copy fan-out to N TX sessions |
| IPO `max_path_differential` | `rx.skew_budget_ns` (10 ms) | MTL does the merge |
| system / user / PTP clock | `MTL_TIME_SOURCE_PTP_BUILTIN`, `PHC`, `CLOCK_TAI` (validated), `SYSTEM_TAI` (ESTIMATED), `USER`; `FREERUN` later; `mtl_time_cross` maps other clocks | MTL may run its own PTP client; every time is TAI |
| `fid_av`, `fi_addr_t` | the flow (`struct mtl_flow`) fixed at create | a session has 1–2 fixed destinations; no per-op address on the fast path |
| `fi_join`, `FI_JOIN_COMPLETE` | IGMP join at RX start; `MTL_FLOW_JOINING`, `MTL_FLOW_JOINED`, `MTL_FLOW_JOIN_FAILED` | |
| `FI_INJECT_COMPLETE` | the TX result in zero-copy mode | with imported memory "reusable" equals "the NIC finished reading by DMA" *(inferred)* |
| `FI_DELIVERY_COMPLETE` | none | defined only for reliable endpoints; multicast UDP has no delivery report (RTCP at most) |
| `FI_TRIGGER` (counter threshold) | the launch time (`unit.launch_tai_ns`, media-time launch) | libfabric has no time trigger (`fi_trigger.3`) |
| `fid_cntr` | stats per port and per 2022-7 leg | richer than one success/error pair |
| `fi_tostr` | `mtl_error_name()`, `mtl_reason_name()` | |

Revision-3 statements of the familiarity map that the headers changed:

- `mtl_cq_trywait` returning 1 ready, 0 armed: gone; the data call or `mtl_session_wait` returns `-MTL_EAGAIN` and arms the
  one wait handle (D-88, mtl.h R2), so `-MTL_EAGAIN` now means "nothing now, armed".
- `mtl_instance_params.api_version` per instance and `mtl_struct_known_size()`: gone; `MTL_API_VERSION`, `mtl_version_num()`
  and `struct_size` with `MTL_INIT` (D-74) instead.
- `mtl_port_get_caps`, `mtl_port_get_capacity`, `mtl_capability_request {ANY, PREFER, REQUIRE, OFF}`: now `caps.*` stats and
  options with `MTL_REQ_PREFER`, `MTL_REQ_REQUIRE`, `MTL_REQ_OFF` (absent = any), capacity by `MTL_QUERY_CHECK_CAPACITY` (D-60).
- `mtl_mem_import{va, length, domain, device_mask, access}`: now `struct mtl_mem_desc {numa, va, length, flags}` with
  `MTL_MEM_READ`, `_WRITE`, `_MAP_ALL`, `_NUMA_CHECK`; device memory `mtl_mem_import_device` (`MTL_LATER`).
- EQ subscription masks `MTL_EQ_SUB_*` and `mtl_eq_post`: per-session events and queues with `MTL_SUB_PORT`, `_TIME`,
  `_INSTANCE`, `_SESSIONS` on `mtl_queue_create`; user posts removed (D-79).
- completion mode `MTL_COMPLETE_NONE` default / `ALL`: `MTL_SESSION_RESULTS` (D-77).
- packet-chunk RX "reserved (NG2)": packet units exist on every essence (D-82, Phase 2P).
- `mtl_session_discard_queued` + `mtl_session_destroy` retried: `mtl_session_discard`, `mtl_session_close(s, timeout)`
  returning 1 and posting `MTL_EVENT_SESSION_RETIRED`.
- exported `*_init()` per struct: `MTL_INIT(&s)`; `MTL_LAUNCH_EXACT`: `MTL_SUBMIT_EXACT` (`MTL_INFO_NON_COMPLIANT`);
  `blocked_on = RING`: not in `enum mtl_blocked_on` (`NONE`, `BUFFERS`, `RESULTS`, `APP_LEASES`, `APP_PINS`).

## 6. Professional media I/O and async-I/O APIs

Read 2026-09-29: the DeckLink SDK Manual (March 2026) and `DeckLinkAPI.h` from SDK 12.0 and 12.2.2 (as vendored in OBS
and GStreamer); AJA libajantv2 `ntv2publicinterface.h`, `ntv2card.h`; GStreamer design docs (synchronisation, latency,
QoS, buffer pool), `GstBaseSink`, `gstbuffer.h`, `gstrtpjitterbuffer.c`; Linux `io_uring.h` and `io_uring(7)`,
`io_uring_setup(2)`, `io_uring_enter(2)`, `io_uring_register(2)`; Vulkan `vulkan_core.h`, `fundamentals.adoc` and
refpages; OpenXR 1.1 refpages; alsa-lib PCM doc and `pcm.h`, kernel `timestamping.rst` and `asound.h`; JACK headers;
PipeWire `stream.h`; CoreAudio SDK headers (MacOSX 11.3 mirror); DPDK `rte_mbuf_dyn.h`, `rte_ethdev.h`, `rte_mbuf_core.h`;
NDI send docs; SRT `statistics.md`; W3C webrtc-stats; the OpenTelemetry metrics data model. Not read: the full OpenXR spec,
the Vulkan queue-ownership and timeline-semaphore chapters, Apple's online docs, the NDI headers.

### 6.1 Facts per API

| API | Facts |
|---|---|
| DeckLink | `ScheduleVideoFrame(frame, displayTime, displayDuration, timeScale)` before `StartScheduledPlayback` prerolls; `E_OUTOFMEMORY` "Too many frames are already scheduled" (§2.5.3.13); start sets the scheduler time to `playbackStartTime` (§2.5.3.25); `StopScheduledPlayback(at, *actualStopTime)` flushes later frames (§2.5.3.26); `GetBufferedVideoFrameCount` gauge; |
|  | `GetHardwareReferenceClock` ("absolute values … are meaningless"); `GetFrameCompletionReferenceTimestamp` valid **only** inside `ScheduledFrameCompleted` and before reschedule (§2.5.3.30); a stream timeline in caller-chosen rational units anchored at start, no TAI. `ScheduledFrameCompleted(frame, result)` once per frame, also the reuse signal (§2.5.6); |
|  | results {Completed, DisplayedLate (out at a later slot; the least-late frame), Dropped (superseded by a less-late frame), Flushed (user stop or speed change)} (§3.9); no failure value, no reason; what goes out on an empty slot *(unknown)*; |
|  | callbacks on a dedicated thread must take "less time than a frame time" (§2.5.6.1). Audio: `ScheduleAudioSamples` accepts partially (`*sampleFramesWritten`), continuous mode appends with time 0; `FlushBufferedAudioSamples`; |
|  | pull preroll `RenderAudioSamples(preroll)` at 50 Hz. Memory: `IDeckLinkVideoBuffer::StartAccess/EndAccess(flags)` (`E_ACCESSDENIED` without it) is a CPU access lease; app allocators (`IDeckLinkVideoBufferAllocatorProvider`; SDK 12 `IDeckLinkMemoryAllocator`; when it changed *(unknown)*); refcounted COM frames. Input: the frame is valid only in the callback unless `AddRef`; |
|  | audio still arrives when video is slow; signal loss is a **per-frame flag** `bmdFrameHasNoInputSource`; opt-in format detection with a bitmask of what changed (§2.5.10.2). DeckLink IP: media API unchanged, 2110 flows are a separate object family (`IDeckLinkIPFlow`, SDP via `GetString(bmdDeckLinkIPFlowSDP)`; "achieved with an off-the-shelf NMOS controller"); |
|  | key-addressed stats `IDeckLinkStatistics::GetInt[WithParam]` (PTP loss-of-lock, DPLL error, temperature, per-port RX drops) (§2.5.63); playback groups start several outputs together (§2.4.13) |
|  | `GetScheduledStreamTime(timeScale, *streamTime, *speed)` = elapsed time since scheduled playback began (§2.5.3.27); `GetReferenceStatus` genlock bitmask (`bmdReferenceLocked`, `bmdReferenceNotSupportedByHardware`); `ScheduledPlaybackHasStopped` is a separate lifecycle callback; |
|  | audio `bmdAudioOutputStreamTimestamped` requires timestamps (continuous mode: `streamTime = timeScale = 0` appends), `GetBufferedAudioSampleFrameCount` is a gauge in sample frames (§2.5.3.21–23); `CreateVideoFrameWithBuffer(…, IDeckLinkVideoBuffer*, …)` wraps app buffers (§2.5.3.9, §2.5.54–55); |
|  | `StartAccess/EndAccess` calls must balance per access flag and "the final release of this interface should resolve all outstanding calls to EndAccess" (§2.5.53); each input frame has `GetStreamTime` and `GetHardwareReferenceTimestamp` (§2.5.11); |
|  | format detection is opt-in (`bmdVideoInputEnableFormatDetection`) and capability-gated (`BMDDeckLinkSupportsInputFormatDetection`), the bitmask names display mode, field dominance, colorspace; DeckLink IP family `IDeckLinkIPExtensions`, `IDeckLinkIPFlow::Enable/Disable`, attributes, status, settings (§2.4.15, §2.5.59); |
|  | per-port stats `EthernetRxPackets`, `EthernetRxDroppedPackets`; playback group via `bmdVideoOutputSynchronizeToPlaybackGroup`, `StartScheduledPlayback` on any member starts all. Manual sections read: §2.4.2 Playback, §2.4.13, §2.4.15, §2.5.3.9–30, §2.5.6, §2.5.10–11, §2.5.53–55, §2.5.59, §2.5.63, §3.6, §3.9 |
| AJA NTV2 | AutoCirculate: a driver ring of device frames, `AutoCirculateInitForOutput(…, inFrameCount = 7, …)` ("Fewer frames reduces latency, but increases the likelihood of frame drops"); blocking `AutoCirculateTransfer` after checking `CanAcceptMoreOutputFrames`; states `DISABLED, INIT, STARTING, PAUSED, STOPPING, RUNNING, STARTING_AT_TIME` (start deferred to a host tick); |
|  | `Stop(abort)`, `Pause/Resume(clearDropCount)`, `Flush` (queued frames discarded, active frame untouched), `PreRoll`. Polled `AUTOCIRCULATE_STATUS` (state, frames, host and 48 kHz audio clock at start and now, `acFramesProcessed`, cumulative `acFramesDropped`, `acBufferLevel`); |
|  | per-transfer `TRANSFER_STATUS` with `FRAME_STAMP` (VBI time on two clocks, `acStartSample` drift check, `acCurrentFrameTime`, `acCurrentReps`, `acCurrentUserCookie` = which frame was on air at the last VBI); 64-bit `acInUserCookie`; **no per-frame result**; `DMABufferLock` page-locks buffers (optional, faster); |
|  | every new struct starts `NTV2_HEADER` (FourCC tag, type, header and body version, `fSizeInBytes`, pointer size, RPC ID since 16.3, `fResultStatus` since 17.5) and ends `NTV2_TRAILER`; its own `@bug`: S2110 ANC "performs many heap allocations" per transfer |
|  | `AutoCirculateStop(channel, inAbort)`: graceful at the next VBI or immediate abort; `Flush` leaves the state unchanged; `AutoCirculatePreRoll` only for frames DMA'd outside AutoCirculate; `AutoCirculateStart(channel, inStartTime = 0)` defers until "the host OS tick clock exceeds the inStartTime value"; `acRDTSCStartTime`/`acAudioClockStartTime` = first VBI after start on both clocks; |
|  | `AUTOCIRCULATE_TRANSFER_STATUS`: `acTransferFrame` (−1 if failed), audio and anc byte counts; `FRAME_STAMP` capture-only fields `acFrameTime`, `acAudioClockTimeStamp`, `acAudioInStartAddress/StopAddress`; `acCurrentReps` = repeats remaining on playout, drops on record; |
|  | `AUTOCIRCULATE_TRANSFER` inputs: client-owned host video/audio/anc buffers ("page-aligned" recommended), `acOutputTimeCodes`, `acFrameRepeatCount`, `acDesiredFrame`; per-transfer format change only with `AUTOCIRCULATE_WITH_FBFCHANGE`; |
|  | `DMABufferLock(buf, inMap, inRDMA)` can also lock the segment map or a GPUDirect buffer for P2P; `DMABufferUnlock`, `UnlockAll`, `DMABufferAutoLock`: the analogue of MTL region registration, explicit, outside the per-frame call, optional (unlocked buffers work, slower; optionality *(inferred)*) |
| GStreamer | `running_time = (timestamp − (start + offset)) / ABS(rate) + base`; the sink waits for `running_time + base_time`; stream time is never used for sync. LATENCY query `{live, min, max}`: min sums each element's hold-back, the pipeline takes `MAX(all min)`; `MIN(all max) < latency` is "an impossible situation"; |
|  | a live source without latency compensation has "all buffers … dropped". QoS: jitter `J = CT − B`; event `{type, proportion, jitter, timestamp}`; message with `processed`/`dropped` since the last flush. `GstBaseSink`: `max-lateness` (−1 unlimited; |
|  | video sinks ≈ 20 ms *(inferred)*), `render-delay`, `ts-offset`, `processing-deadline` (1.16), `stats` (1.18: average-rate, dropped, rendered). Pools: ALLOCATION query `{pool, size, min, max}`; exhausted acquire blocks, `DONTWAIT` gives `GST_FLOW_EOS`, an inactive pool `FLUSHING`; |
|  | buffers return at refcount 0. `GstReferenceTimestampMeta {reference caps, timestamp, duration, info}` with clock identity in caps (`timestamp/x-ptp, version=IEEE1588-2008, domain=1`, `x-ntp`, `x-unix`, `x-system-monotonic`), several per buffer; |
|  | `rtpjitterbuffer` adds RFC 7273 reference meta, stats {pushed, lost, late, duplicates, avg-jitter}, coalesced "drop-msg" {seqnum, timestamp, reason, num-too-late, num-drop-on-latency since the last} rate-limited by `drop-messages-interval` |
|  | LATENCY max: blocking elements add their buffer size, leaky elements take `MIN(upstream_max, own_max)`; the chosen latency is distributed in a LATENCY event and sinks add it to sync times; a live buffer stamped T exists only at T+D; dynamic latency changes are posted on the bus, cause glitches, "reserved for special conditions"; |
|  | QoS event types OVERFLOW, UNDERFLOW, THROTTLE; QoS message fields `live, running-time, stream-time, timestamp, duration, jitter, proportion, quality, format, processed, dropped` (since the last READY or flush); `GstBaseSink` also `sync`, `qos`, `throttle-time`; |
|  | lateness rule: late when presentation time + duration is before the clock's now; beyond `max-lateness` the buffer is dropped without calling render; the ALLOCATION answer also lists allocators `{allocator, params (align, padding)}` and accepted metas; deactivating a pool wakes blocked acquires with FLUSHING; |
|  | a small `max_buffers` rate-limits the producer by recycling; `GstReferenceTimestampMeta.info` since 1.28; `rtpjitterbuffer` `rfc7273-reference-timestamp-meta-only` keeps the reference as meta without touching PTS, `drop-on-latency` drops the oldest packets when full; its stats include rtx counters |
| io_uring | `user_data` "passed unchanged"; "exactly one matching CQE … for every SQE" except multishot (`IORING_CQE_F_MORE`) and zero-copy send, whose second CQE `IORING_CQE_F_NOTIF` says the buffer is free (`IORING_SEND_ZC_REPORT_USAGE` tells whether it was copied); any completion order (`IOSQE_IO_LINK` for order). `IORING_FEAT_NODROP` (5.5+): overflow CQEs kept in kernel memory; |
|  | dropped only on OOM, then the overflow counter and since 5.19 `-EBADR`; older kernels `-EBUSY`; `IORING_SQ_CQ_OVERFLOW`; `IORING_SETUP_CQSIZE` (default 2 × SQ *(inferred)*). `SQPOLL` sets `IORING_SQ_NEED_WAKEUP` after `sq_thread_idle`, woken by `IORING_ENTER_SQ_WAKEUP`. `EXT_ARG` timeouts and `min_wait_usec`; `ASYNC_CANCEL` 0 / `-ENOENT` / `-EALREADY`; |
|  | broken links `-ECANCELED`, link timeout `-ETIME`. Registered buffers lock pages against `RLIMIT_MEMLOCK` (1 GiB each); resource tags post a CQE "after the resource had been unregistered and it's not used anymore"; provided-buffer rings, power of two, ≤ 32768 entries; unregistering before shutdown is unnecessary but unpinning may be asynchronous |
|  | a failed overflow allocation shows as `io_cqring_offsets.overflow` rising; `IORING_SETUP_CQSIZE` must be > entries, rounded to a power of two; `IORING_ENTER_GETEVENTS` waits for `min_complete`; `struct io_uring_getevents_arg { sigmask; sigmask_sz; min_wait_usec; ts; }`; |
|  | the cancelled request itself completes `-ECANCELED` *(inferred)*; `IOSQE_IO_HARDLINK` chains do not sever; `IORING_OP_LINK_TIMEOUT` completes `-ETIME` if it fired, `-ECANCELED` if the linked op won; cancellation still yields exactly one CQE per SQE; |
|  | registered buffers are used by `READ_FIXED`/`WRITE_FIXED` with `buf_index`, tags come with `REGISTER_BUFFERS2`/`BUFFERS_UPDATE`; provided-buffer ring CQEs carry `IORING_CQE_F_BUFFER` and the buffer ID, an empty ring gives `-ENOBUFS` *(inferred)*; lesson: NODROP is "no drop unless OOM, and then tell you", it relies on allocation |
| Vulkan, OpenXR | every extensible struct starts `{sType, pNext}`; components "must skip over … any extending structures in the chain not defined" by what they support; outputs are extensible too; two-call count/fill. `VK_GOOGLE_display_timing`: request `{presentID, desiredPresentTime}`; |
|  | past timing `{desired, actual, earliest, presentMargin}` ("how early … compared to how soon it needed to be processed"), returned once. `VK_EXT_present_timing` (v3): `{flags, targetTime, timeDomainId, presentStageQueries, …}`, `PRESENT_AT_RELATIVE_TIME`, `PRESENT_AT_NEAREST_REFRESH_CYCLE`, "do not provide a strict guarantee"; |
|  | stages `QUEUE_OPERATIONS_END`, `REQUEST_DEQUEUED`, `IMAGE_FIRST_PIXEL_OUT`, `IMAGE_FIRST_PIXEL_VISIBLE`, "a time value of 0 … not available", `reportComplete`, partial and out-of-order flags; time domains (`DEVICE`, `CLOCK_MONOTONIC`, `_RAW`, `QUERY_PERFORMANCE_COUNTER`, …) with a reported fallback; property counters; |
|  | a results queue sized by `vkSetSwapchainPresentTimingQueueSizeEXT`, a slot reserved at present, else `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT` (shrinking below pending: `VK_NOT_READY`); |
|  | `vkWaitForPresent2KHR`. Ownership transfer by matched release/acquire barriers and 64-bit timeline semaphores *(inferred)*. OpenXR: `xrWaitFrame` returns `{predictedDisplayTime (monotonic), predictedDisplayPeriod, shouldRender}`; begin without wait `XR_ERROR_CALL_ORDER_INVALID`, a second begin the success code `XR_FRAME_DISCARDED`; no throttling in `xrBeginFrame` |
|  | chain walkers `VkBaseInStructure`/`VkBaseOutStructure`; `desiredPresentTime` non-zero = "not presented any sooner", result retention depth *(unknown)*; `VK_EXT_present_timing`: without `PRESENT_AT_NEAREST_REFRESH_CYCLE` the app "would strictly prefer the image to not be visible before targetTime"; |
|  | time domains also `PRESENT_STAGE_LOCAL_EXT`, `SWAPCHAIN_LOCAL_EXT`; counters `timingPropertiesCounter`, `timeDomainsCounter`; `vkWaitForPresent2KHR(device, swapchain, {presentId, timeout})` pairs with `VK_KHR_present_id2` (64-bit IDs); |
|  | mapping to MTL *(inferred)*: submit is a release to MTL, the terminal result a release back, a timeline value a cheap "all submissions ≤ N are terminal" watermark. OpenXR: `predictedDisplayTime` "must be monotonically increasing"; `shouldRender = false` skips the work but keeps the call cadence; the runtime names the display time before the producer renders |
| ALSA, JACK | ALSA: `SND_PCM_STATE_XRUN`, I/O `-EPIPE`, `snd_pcm_recover`; `avail` and `delay` read together; `snd_pcm_status` fields (`trigger_tstamp`, `avail_max` and `overrange` **reset after the status call**, `audio_tstamp`, `driver_tstamp`, accuracy); |
|  | audio timestamp types `DEFAULT`, `LINK`, `LINK_ABSOLUTE`, `LINK_ESTIMATED`, `LINK_SYNCHRONIZED`, config `{type_requested, report_delay}`, report `{valid, actual_type, accuracy_report, accuracy ns}`, an unsupported request falls back to DEFAULT and says so; exactly one (system, audio) pair, "a conscious design decision"; |
|  | the latency ladder (analog, link, DMA, app, buffer). JACK: `jack_set_xrun_callback` with **no information**, magnitude via `jack_get_xrun_delayed_usecs`, max and reset calls; `jack_get_cycle_times` where `current_usecs` equals the previous `next_usecs` unless an xrun occurred, `period_usecs` a DLL estimate; latency ranges `{min, max}` per port and direction |
|  | ALSA: recovery also by `prepare`/`drop`/`drain`; `delay` = "the time it will take to hear a new sample after all queued samples have been played out"; `snd_pcm_avail_delay()` returns both in sync; `snd_pcm_status` also `state, tstamp, appl_ptr, hw_ptr, delay, avail`; |
|  | `audio_tstamp` is sample counter, wall clock, PHC or on-demand synced; `driver_tstamp` exists because the system timestamp may be latched late relative to avail/delay; the one-pair rule: "the more timestamps are read the more imprecise the combined measurements are"; |
|  | JACK: latency ranges in frames, recomputed via a latency callback; `jack_get_max_delayed_usecs`, `jack_reset_max_delayed_usecs` |
| PipeWire, CoreAudio | PipeWire `struct pw_time {now, rate, ticks (discontinuities visible), delay (may be negative), queued, buffered, queued_buffers, avail_buffers, size}`, total latency = buffered + queued + (delay − elapsed); `pw_stream_get_time_n(stream, *time, size)` with fields "Since 0.3.50", "Since 1.1.0"; event tables start with `version` (`PW_VERSION_STREAM_EVENTS 2`); |
|  | `pw_buffer.user_data` returned unmodified. CoreAudio `AudioTimeStamp {mSampleTime, mHostTime, mRateScalar, mWordClockTime, mSMPTETime, mFlags}`, "different representations of the same point in time", validity bits per field; `AudioDeviceIOProc(…, inNow, inInputTime, outputData, inOutputTime, …)` gives the producer the output deadline ahead; |
|  | `kAudioDeviceProcessorOverload` when a cycle ran past its deadline; `SafetyOffset`, `Latency` properties |
|  | PipeWire: `queued` is the sum of app-set `pw_buffer.size` in app units; `pw_buffer.requested` is a playback hint, `pw_buffer.time` the capture cycle time. CoreAudio: `mRateScalar` = actual host ticks per sample frame / nominal; flag `kAudioTimeStampSampleHostTimeValid`; |
|  | `inNow` is the cycle start including scheduling latency, `inInputTime` when the first input frame was acquired, `inOutputTime` when the first output frame will reach the hardware; `kAudioDeviceProcessorOverload` is dispatched synchronously from the IO thread; latency property semantics *(inferred)* |
| DPDK | `rte_eth_tx_burst` returns the count sent (partial acceptance) and frees earlier mbufs lazily below `tx_free_thresh`, so an extbuf `free_cb` is reuse time, not wire time; `rte_eth_tx_done_cleanup` forces reclaim. Launch time `RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP` with `rte_dynfield_timestamp`/`rte_dynflag_tx_timestamp`: units "maintained always the same for a given port"; |
|  | "If the specified one is in the past it should be ignored, if one is in the distant future it should be capped"; no reordering; maybe only the first packet of a burst. `rte_eth_read_clock` raw ticks, frequency by regression; `rte_eth_timesync_read_tx_timestamp` is port-wide (`-EINVAL` "No timestamp is available"), unfit for media packets *(inferred)* |
|  | extbuf `struct rte_mbuf_ext_shared_info { free_cb; fcb_opaque; refcnt; }`, `free_cb(addr, opaque)` at refcnt 0, so release is lazy and batched; `rte_eth_read_clock` ticks have "no given time reference", the app derives frequency and offset by regression |
| NDI, SRT | NDI `send_video_v2_async`: memory used "until a synchronizing API call" (next send, NULL frame, sync send, destroy); freeing early crashes "in an NDI thread", reuse tears; ping-pong two buffers; `clock_video`/`clock_audio` rate-limit submission ("only clock one of them"); |
|  | `NDIlib_send_timecode_synthesize` = `INT64_MAX` sentinel. SRT: *total*, *interval* (`srt_bstats(…, clear=1)`) and *instantaneous* (`srt_bistats(…, 1)`; else a moving average); `pktSndDrop` ("no chance to be delivered in time"), `pktRcvDrop`, `pktRcvBelated` ("received but IGNORED"), the TLPKTDROP formula; buffer levels in time (`msSndBuf`) as well as packets and bytes |
|  | NDI synthesized timecode is UTC in 100 ns units. SRT: `pktRcvDrop` counts never-arrived, too-late and undecryptable packets; buffer level also `pktSndBuf` (packets) and `byteAvailSndBuf` |
| WebRTC, OpenTelemetry | webrtc-stats: `framesDropped` includes frames that "missed its display deadline"; jitter-buffer sums plus `jitterBufferEmittedCount`; `freezeCount`, `totalFreezesDuration`, squared inter-frame delay; "A stats object, once returned, never changes", counters "must always increase", no implementation-computed averages, zero before the first increment; |
|  | `totalPacketSendDelay` text truncated *(unknown)*. OpenTelemetry: Sum (monotonic flag), Gauge, Histogram `(lower, upper]`, ExponentialHistogram; cumulative temporality repeats the start timestamp, delta advances it; the start timestamp marks resets |
|  | WebRTC jitter-buffer sums `jitterBufferDelay`, `jitterBufferTargetDelay`, `jitterBufferMinimumDelay` per emitted item; `totalPacketSendDelay` is by name a sum of per-packet packetization-to-send time *(inferred)*; OpenTelemetry Gauge = "last-sampled event"; the model is a ready vocabulary for exporting MTL stats |

Two cross-cutting comparisons:

| API | Granularity | On time | Late but sent | Not sent (late) | Not sent (user) | Identity | Error |
|---|---|---|---|---|---|---|---|
| DeckLink | frame | Completed | DisplayedLate | Dropped | Flushed | frame object | — |
| AJA | aggregate | processed++ | — | dropped++ | Flush | cookie, polled | transfer returns false |
| GStreamer basesink | buffer | rendered | within `max-lateness` | dropped + QoS | flush | buffer | flow error |
| Vulkan present timing | present | actual ≈ target | margin | — | — | presentId | `VkResult` |
| io_uring | op | res ≥ 0 | — | `-ETIME` | `-ECANCELED` | user_data | `-errno` |
| CoreAudio, JACK | cycle | — | overload / xrun | — | — | none | — |
| NDI async | frame | implicit | — | — | — | none | — |
| MTL today | frame | `notify_frame_done(COMPLETE)` | — | `DROPPED`; `notify_frame_late(epoch_skipped)` | — | `frame_idx` + meta | — |

The MTL row was checked in `include/st20_api.h` (`notify_frame_done`, `notify_frame_late(void* priv, uint64_t epoch_skipped)`) and `include/st_api.h` (`st_frame_status`, `stat_frames_dropped`).

| API | Requested | Resolved / scheduled | Observed | Margin | Clock identity | Validity |
|---|---|---|---|---|---|---|
| DeckLink | displayTime | — | completion timestamp (callback only) | — | implicit | method code |
| AJA | — | — | `acCurrentFrameTime`, cookie | `acStartSample` drift | host + 48 kHz pair | `0xFFFFFFFF` |
| Vulkan GOOGLE / EXT | desired / target | `earliestPresentTime` (GOOGLE only) | actual / per stage | `presentMargin` / derivable | implicit / `timeDomain` | — / time 0, `reportComplete` |
| ALSA | — | — | `tstamp`, `audio_tstamp` | delay, avail | `actual_type` | valid, accuracy bits |
| CoreAudio | `inOutputTime` (given) | — | — | — | several representations | `mFlags` |
| GStreamer | PTS → running time | — | clock at render | jitter (signed) | caps in the meta | `GST_CLOCK_TIME_NONE` |

App-built packets without driver buffers (study S8 §3, the prior art behind packet units, D-82):

| API | Facts | Lesson |
|---|---|---|
| DPDK | `rte_pktmbuf_attach_extbuf` with a `free_cb` run "once all the mbufs are detached"; `rte_pktmbuf_pool_create_extbuf`; buffer split only with patched DPDK; `SEND_ON_TIMESTAMP` sends a past time at once | a per-chunk `shinfo` refcount = per-chunk completion (as `st_tx_video_session.c:1295`) |
| AF_XDP | UMEM of equal "2K or 4K" frames by offset; TX ring `{addr, len, options}`, multi-buffer by `XDP_PKT_CONTD`; FILL/RX rings; a COMPLETION entry "does not guarantee successful packet transmission" | fixed slots + lengths are the universal zero-copy shape; reusable ≠ sent |
| io_uring `SEND_ZC` | a result CQE plus a `NOTIF` CQE when the buffer is reusable | D-18's one terminal result is simpler |
| libfabric | `fi_sendmsg` iov + `desc[]`, `FI_MORE`, `FI_INJECT`; `FI_MULTI_RECV` one buffer for many messages | a chunk is one posted buffer for many packets |
| GStreamer | payloaders push `GstBufferList`s; `gst_rtp_base_payload_push_list` silently "refreshes" SSRC, PT, seqnum, timestamp | state which header fields the layer writes |
| FFmpeg | `rtp` muxer `pkt_size`; `udp` paces only by `bitrate`/`burst_bits` | rate pacing is the minimum a packet sender needs |
| PipeWire `module-rtp-sink` (AES67) | packets of `rtp.ptime`/`rtp.framecount` within `net.mtu`; `sess.ts-offset` (default random), `sess.ts-refclk` | audio packet mode = ptime cadence + an RTP offset |
| DeckLink, AJA | no packet API (DeckLink IP is frame level) | — |

Shapes that work: fixed-stride slots + per-packet length table (Rivermax, AF_XDP); header and payload as planes (Rivermax
HDS); one time per chunk with "follow" (Rivermax); one completion per chunk meaning "storage reusable" (AF_XDP, io_uring
NOTIF, D-18); RX chunks of 0..max packets with moderation plus explicit release; a stated rule for header fields the
layer writes. The design that takes them: [contract.md §13](contract.md#13-packet-units).

### 6.2 Patterns adopted and avoided

The ten adopted defaults with their decisions are in [decisions.md §3.2](decisions.md#32-why-prior-art); the full lists:

| # | Pattern adopted | From | In the design |
|---|---|---|---|
| A1 | per-submission terminal result separating late-but-sent from not-sent, with a reason | DeckLink §3.9 | `MTL_TX_ON_TIME`, `LATE`, `DROPPED`, `FLUSHED`, `FAILED` + `reason` (D-14, D-81) |
| A2 | requested, resolved, scheduled, observed times and a signed margin | Vulkan `presentMargin`, `earliestPresentTime` | `mtl_tx_result.margin_ns`, `mtl_tx_result_full` times (timing.md §6.6, G-18) |
| A3 | clock tag and validity bits on every time, one coherent snapshot | CoreAudio, ALSA, Vulkan `timeDomain`, GStreamer reference caps | `MTL_UNITF_*`, `MTL_TXR_*`, `MTL_TXR_SENT_HW`, `mtl_time_cross` (D-15) |
| A4 | a results queue that cannot overflow, by admission | Vulkan `QUEUE_FULL`, DeckLink `E_OUTOFMEMORY` | ring = pool, `MTL_BLOCKED_RESULTS` (D-03) |
| A5 | one terminal result after both transport and storage | io_uring `SEND_ZC` + `NOTIF`; DPDK lazy free | D-18, with `sent_tai_ns` separate so timing is not delayed by reclaim |
| A6 | an opaque 64-bit cookie round trip | io_uring, AJA, Vulkan, PipeWire | `unit.cookie` → `mtl_tx_result.cookie` |
| A7 | buffer-level gauges in count and time, with watermarks | DeckLink, AJA, SRT `msSndBuf`, ALSA `avail_max`, PipeWire | queued units and stats gauges (contract.md) |
| A8 | tell the producer which slot it fills | OpenXR `predictedDisplayTime`, CoreAudio `inOutputTime`, JACK `next_usecs` | `mtl_tx_next_slot` (timing.md §6.5) |
| A9 | preroll and start-at-time as lifecycle steps; group start | DeckLink scheduled start and playback groups, AJA `STARTING_AT_TIME` | `MTL_STATE_ARMED`, `struct mtl_when` with `preroll_ns`, start arrays (D-08, D-78) |
| A10 | latency as a range; impossible configurations rejected at setup | GStreamer LATENCY, JACK ranges | `latency_min_ns`, `latency_max_ns`, `min_submit_lead_ns` from the dry run |
| A11 | RX anomalies flagged per unit, cadence kept; format change as a bitmask event | DeckLink `bmdFrameHasNoInputSource`, `VideoInputFormatChanged` | `MTL_RX_INCOMPLETE`, `MTL_UNITF_FORMAT_CHANGED`, `MTL_EVENT_RX_FORMAT`, `MTL_EVENT_RX_SIGNAL` |
| A12 | immutable snapshots of cumulative counters from 0, gauges and histograms separate, sums and counts, no averages | WebRTC, OpenTelemetry cumulative | the stats registry (D-80) |
| A13 | key-addressed extension stats | DeckLink `GetInt[WithParam]` | `mtl_stat_find`, `mtl_stat_read` by name |
| A14 | coalesced events carrying counts since the last; ownership results never coalesced | GStreamer "drop-msg" | coalescing events (D-79) |
| A15 | `struct_size` first, size-parameterised getters | PipeWire `get_time_n(size)`, AJA `fSizeInBytes` | mtl.h R3 (D-74); a `pNext` chain was not needed |
| A16 | report the mode actually chosen; never degrade an exact request | ALSA `actual_type`, Vulkan fallback domain | `pacing_class`, `MTL_EVENT_PACING_CHANGED`, `caps.pacing_required`, no fallback for exact launch (G-26) |
| A17 | a visible poller-sleep flag and wake protocol | io_uring `SQ_NEED_WAKEUP` | the armed-waiter bit and waker (D-04, D-68) |
| A18 | deferred release notification instead of `-EBUSY` polling | io_uring resource tags | `MTL_EVENT_REGION_RELEASED`, `MTL_EVENT_SESSION_RETIRED` |
| A19 | transport (flows, SDP) separate from media I/O | DeckLink IP flows | flows in `struct mtl_flow`; SDP and NMOS outside the data path (Phase 7, later) |

| # | Pattern avoided | Example | Instead |
|---|---|---|---|
| V1 | results valid only inside a callback | DeckLink completion timestamp | retained, copyable result records |
| V2 | implicit positional lifetime | NDI async send | lifetime ends at an explicit result (D-01) |
| V3 | information-free exception callbacks | JACK xrun; MTL `notify_frame_late(priv, epoch_skipped)` | per-unit result with deadline and margins |
| V4 | aggregate-only drop accounting | AJA `acFramesDropped` | counters plus per-unit results |
| V5 | trusting the NIC to report lateness | DPDK "should be ignored" | MTL compares against the deadline before enqueue (D-13) |
| V6 | mbuf free time as "transmitted at" | DPDK `tx_free_thresh` | observed wire time labelled by source (`MTL_TXR_SENT_HW`) |
| V7 | overflow storage that allocates | io_uring NODROP (`-EBADR`) | bounded in-flight work |
| V8 | implementation moving averages, implicit windows | SRT, basesink `average-rate` | sums and counts |
| V9 | reset-on-read counters | ALSA `avail_max`, SRT `clear`, AJA `inClearDropCount`, MTL `*_reset_session_stats` | cumulative for the session's life (Q-OBS-1) |
| V10 | heap allocation in the per-frame path | AJA's S2110 ANC `@bug` | the two-world rule |
| V11 | correctness tied to callback runtime | DeckLink callback thread | queues primary; no app code on tasklets (D-04) |
| V12 | only stream-relative clocks for scheduling | DeckLink stream time | absolute TAI on every submission and result |
| V13 | sentinel values in data fields | NDI `INT64_MAX`, Vulkan time 0, AJA `0xFFFFFFFF` | validity bits |
| V14 | blocking calls that combine admission, copy and result | AJA `AutoCirculateTransfer` | non-blocking acquire and submit, results apart |

The design's object model against its closest prior art (research note 11 §12):

| Design concept | Closest prior art |
|---|---|
| region and registration (`mtl_region_h`, `mtl_mem.h`) | AJA `DMABufferLock`; io_uring `REGISTER_BUFFERS` + tags; DeckLink allocator provider |
| slot, immutable layout | DeckLink `IDeckLinkVideoBuffer` + `CreateVideoFrameWithBuffer`; `GstBuffer`/`GstMemory` |
| access lease (`mtl_lease_h`) | DeckLink `StartAccess/EndAccess(flags)`; Vulkan release/acquire barriers *(inferred)* |
| submission with per-use timing | DeckLink `ScheduleVideoFrame(displayTime)`; `VkPresentTimingInfoEXT`; `AUTOCIRCULATE_TRANSFER` |
| terminal result exactly once | DeckLink `ScheduledFrameCompleted`; io_uring CQE; Vulkan past-presentation record |
| no-drop results queue | Vulkan reserved slot + `QUEUE_FULL`; io_uring NODROP (caveats) |
| flush, drain, abort | DeckLink `Flushed` + `StopScheduledPlayback(at, &actual)`; AJA `Flush`/`Stop(abort)`; io_uring cancel + `-ECANCELED` |
| RX dequeue and release | GStreamer return-on-unref; io_uring provided-buffer ring; DeckLink `AddRef/Release` on input frames |

### 6.3 Research note 11's questions and where they were answered

| # | Question | Answer |
|---|---|---|
| 1 | what is "late" on TX, against which reference? | both: the pick-up deadline gives the verdict and `margin_ns`; wire lateness per packet is `max_packet_lateness_ns` (timing.md §6.1, §6.6) |
| 2 | a missed slot: late, next slot or dropped? | the late policy, default by media mode; REPEAT_LAST later (D-13, Q-TIME-4) |
| 3 | one result or result plus buffer notification? | one terminal result, observed wire time separate (D-18) |
| 4 | CQ capacity? | capacity = pool by construction, events bounded and coalescing (D-03, D-79) |
| 5 | delivery: dequeue, callbacks or both? | dequeue and wait handles; callbacks only on dispatch threads; results in submission order (D-03, D-04) |
| 6 | which observed timestamps on E810/E830? | `sent_tai_ns` with `MTL_TXR_SENT_HW` only from NIC timestamps; `enqueued_tai_ns` is SW, not wire time (timing.md §6.6) |
| 7 | gauge units and watermarks? | count and time (contract.md) |
| 8 | cumulative-only stats? | yes, for the session's life (D-80, Q-OBS-1) |
| 9 | `struct_size` alone, or `pNext`? | `struct_size` (D-74) |
| 10 | advertise latency and minimum lead? | `latency_min_ns`, `latency_max_ns`, `min_submit_lead_ns` in `mtl_session_info` |
| 11 | RX when the source is absent? | flagged incomplete units and `MTL_EVENT_RX_SIGNAL` after `rx.signal_timeout_ns` (timing.md §11.7) |
| 12 | preroll and group start in v1? | yes: `ARMED`, `preroll_ns`, start arrays (D-08, D-78) |

## 7. How Kubernetes runs a DPDK media process

Read 2026-10-01 (K1): the Kubernetes docs from `kubernetes/website` (S1–S15), the SR-IOV device plugin, CNI and operator
(S16–S18), Linux vfio, ice, iavf, xsk and time-namespace sources (S19–S22), DPDK source and guides (S23–S24), linuxptp
(S25), libxdp (S26), the AF_XDP Kubernetes plugins (S27), OpenShift 4.16 and Holoscan for Media guides (S28–S29), Media
Communications Mesh manifests (S30), RFC 3376 (S31), the OpenShift PTP chapter (S32, headings only); there is no S4. The
facts the pod contract rests on, with their sources, are the table in [deployment.md §4.1](deployment.md#41-what-a-pod-changes);
this section holds the rest.

### 7.1 Further facts

| Topic | Fact | Source |
|---|---|---|
| termination | the grace countdown starts before MTL hears anything; the docs' example: grace 60 s, `preStop` 55 s, stop 10 s → killed; `preStop` exec runs in the container, httpGet or sleep from the kubelet; a probe-level `terminationGracePeriodSeconds` (KEP-2238, GA about v1.27 *(inferred)*) can shorten a liveness kill | S1, S2 |
|  | `preStop` is not run when the process has already exited; `/dev/termination-log` holds 4096 B per container, 12 KiB per pod, `FallbackToLogsOnError` uses the log tail when nothing was written | S2; Kubernetes "determine reason for pod failure" page |
| eviction | a soft threshold uses the lesser of the soft grace period and `eviction-max-pod-grace-period`; `HugepageAwareEviction` subtracts hugepage capacity from `memory.available`, because hugepage RAM "can delay eviction and lead to OOM kills"; API eviction (`kubectl drain`) respects PDBs and grace *(inferred)* | S6 |
|  | graceful node shutdown gives pods their share of `shutdownGracePeriod`, regular pods first, then critical ones; the SR-IOV Network Operator drains by pool and `maxUnavailable` when it reconfigures VFs, so a VF policy change restarts every SR-IOV pod on the node (the restart *(inferred)*) | S7, S18 |
| probes | startup gates liveness and readiness; liveness "determine when to restart a container" and does not wait for readiness; readiness runs for the whole lifetime; exec, httpGet, tcpSocket, gRPC (stable v1.27); `timeoutSeconds` 1 s, `periodSeconds` 10 and `failureThreshold` 3 *(inferred)*; an exec probe forks into the container's cgroup and competes with tasklets *(inferred)* | S1, S8 |
| restarts | back-off 10 s, 20 s, 40 s … capped at 300 s, reset after 10 min clean; gates `ReduceDefaultCrashLoopBackOffDecay` (1 s up to 60 s), per-node `maxContainerRestartPeriod`; `restartPolicyRules` on exit codes; `RestartAllContainers` | S1 |
| readiness and multicast | readiness steers no multicast (media goes to groups, not Services); it gates rollouts and whatever watches pod conditions (NMOS controllers, operators) *(inferred)* | — |
| sidecars | an init container with `restartPolicy: Always`, stable v1.33; started in order; on termination the kubelet "postpones terminating sidecar containers until the main application container has fully stopped", then stops them "in the reverse order"; out of time, "all remaining containers … terminated simultaneously with a short grace period"; | S1, S3, KEP-753 |
|  | init containers suit host checks but cannot hold devices (each container opens VFIO itself) *(inferred)* |  |
|  | an MTL gateway sidecar outlives the app containers it serves only if they stop well within the grace period; out of time, everything is stopped at once *(inferred)* |  |
| PID 1 | `shareProcessNamespace: true`: "The container process no longer has PID 1", the pause process is; `/proc/$pid/root` visible across containers; Docker's `init: true` (`docker/docker-compose.yml`) has no pod equivalent: tini, dumb-init or a handler | S5, pid_namespaces(7) |
| SysV shm in a pod | containers of a pod share IPC ("SystemV semaphores or POSIX shared memory"); `IPC_RMID` runs only in MTL's uninit (`mt_sch.c:596-605`); the stale-entry check frees an entry only when hostname and user match and `kill(pid, 0)` fails (`mt_sch.c:685-699`); | S10; code |
|  | in a pod the hostname is the same and PIDs restart, so a reused PID keeps a dead lcore "active" *(inferred)*; `doc/shm_lcore.md` already warns the PID check "becomes less useful" in containers |  |
| MtlManager leftovers | client lcores, queues and flows are released at socket close (`mtl_instance.hpp:66-88`); UDP filter refcounts are per interface, not per client, so they leak (`mtl_instance.hpp:272-286`, `mtl_interface.hpp:102-134`); the XDP program is detached only with the interface object (`mtl_interface.hpp:453`) | code |
| device plugin | resources `<prefix>/<name>` (default `intel.com`) selected by vendor, device, `drivers: ["vfio-pci"]`, `pfNames` with VF ranges (`netpf0#0,2-7`; first selector wins); `PCIDEVICE_<RESOURCE>_INFO={"…":{"vfio":{"vfio-dev-mount":"/dev/vfio/169","vfio-mount":"/dev/vfio/vfio"}}}`; a vfio pod runs "in a non-privilege Pod with only IPC_LOCK capability added"; | S16 |
|  | no-IOMMU VMs: `/dev/vfio/noiommu-N`, "the pod must be privileged and have both IPC_LOCK and CAP_SYS_RAWIO"; Multus publishes the IPAM address and a `device-info` block in `k8s.v1.cni.cncf.io/network-status` *(inferred)* |  |
|  | the env name is `PCIDEVICE_` + resource prefix + `_` + name, `.` and `/` turned to `_`, upper case, comma-separated BDFs, plus `PCIDEVICE_<RESOURCE>_INFO` JSON: `intel.com/e810_red` → `PCIDEVICE_INTEL_COM_E810_RED` | device plugin `pkg/resources/pool_stub.go` (`GetEnvs`) |
| sriov-cni | DPDK mode when the VF has a DPDK driver (`config.go:90`); ADD records the original state (`FillOriginalVfInfo`) and always applies VLAN, QoS, proto, admin MAC, min/max Tx rate, spoofchk, trust, link state; caches NetConf in `/var/lib/cni/sriov`; IPAM "only allocate[s]" for DPDK VFs; DEL takes a per-device lock and skips `ReleaseVF` in DPDK mode; | S17, S18 |
|  | the lock and allocated-PCI file stop reuse races (`cni.go:240-247`, `config.go:56-77`); the operator exposes `vlan`, `vlanQoS`, `vlanProto`, `spoofChk`, `trust`, `linkState`, `maxTxRate`, `minTxRate` |  |
|  | in DPDK mode only the netns move (`SetupVF`) is skipped at ADD (`cni.go:98-110`); DEL calls `ResetVFConfig`, which restores VLAN, spoofchk, trust and rates only if ADD set them and the admin MAC only when one was cached, then deletes the allocated-PCI record (`cni.go:240-325`) | S17 |
| leaks from a VF's last tenant | nothing on the data path (queues, filters, promisc, FDIR and VSI-level TM go with the reset *(inferred)*); PF-side admin state set outside the CNI (`ip link set <pf> vf N …`) persists; PF setup for MTL's patched-ICE rate-limit pacing is outside Kubernetes' view *(inferred)*; | S20 |
|  | an admin-set MAC stops an untrusted VF adding or deleting unicast MACs (`virtchnl.c:657-666`); spoofchk drops foreign source MACs *(inferred)*; DCF is a node role; ADQ has no Kubernetes integration *(inferred)* |  |
|  | an untrusted VF asking for promiscuous or all-multicast is refused with the PF kernel log "Unprivileged VF %d is attempting to configure promiscuous mode" (`virtchnl.c:508-509`): the line to look for in `dmesg` when `MTL_REASON_VF_UNTRUSTED` (612) fires; | S20 |
|  | the iavf VF PHC is `VIRTCHNL_OP_1588_PTP_GET_TIME` and the kernel registers only `gettimex64` (`iavf_ptp.c:184-332`): read, never adjust | S20 |
| hugepages | `hugepages-2Mi`/`-1Gi` per container, requests equal limits, no overcommit, a CPU or memory request required, ResourceQuota per namespace; "isolated at a container scope"; volumes `emptyDir: {medium: HugePages}` or `HugePages-<size>`, never above the pod request; new node pages need a kubelet restart; | S11, S24 |
|  | with `--in-memory` DPDK "bypasses the need to access hugepage mount point" |  |
|  | an emptyDir `medium: HugePages` survives a container crash ("safe across container crashes"), so hugetlbfs files would stay; MTL leaves none, because `--in-memory` backs hugepages with `memfd_create(MFD_HUGETLB)` (DPDK `eal_memalloc.c:224-247`); | S9, S23 |
|  | DPDK still reads `/sys/kernel/mm/hugepages`, whose `free_hugepages` is the host pool, not the container's limit *(inferred)*; memory not touched at allocation could still SIGBUS later *(inferred)*, which is why every pool is faulted in at create (K-REQ-10) |  |
| CPU | static policy options `full-pcpus-only` (GA 1.33), `distribute-cpus-across-numa` (beta), `strict-cpu-reservation` (GA 1.35), `prefer-align-cpus-by-uncorecache` (GA 1.36), `align-by-socket` and `distribute-cpus-across-cores` (alpha); Topology Manager `none`, `best-effort`, `restricted`, `single-numa-node`, container or pod scope, stable v1.27; | S12, S13, S23, S29 |
|  | a rejected pod is not rescheduled ("will **not** attempt to reschedule"), and the default scheduler is not topology aware, so Holoscan for Media needs a topology-aware secondary scheduler; an `-l` outside the mask fails "please check specified cores are part of …" (`eal_common_options.c:2174-2181`); |  |
|  | `sched_setaffinity` outside the cpuset is EINVAL, a partial overlap is cut *(inferred)*; MtlManager grants lcores from a host-global view (`mt_sch.c:713-731`); a Guaranteed pod has no CPU outside its set for housekeeping |  |
|  | the static policy needs a non-zero CPU reservation, and the kubelet reconciles assignments into cgroupfs; `mtl_bind_to_lcore` already pins with the EAL's per-lcore cpuset (`mt_main.c:735-741`); with load balancing disabled (as with `isolcpus`, `doc/isolation.md`) unpinned `TASKLET_THREAD` schedulers can stack on one CPU (gap G4, EK7); | S12; code |
|  | the feature gate `DisableCPUQuotaWithExclusiveCPUs` (Beta, on by default since 1.33) drops the CFS quota for exclusive CPUs; with the gate off every Guaranteed pod has a quota, so "a quota is present" says nothing: flag only quota ÷ period < CPUs in the mask (RK-25, EK17) | Kubernetes feature-gates page |
| OpenShift and Holoscan | PerformanceProfile `isolated`/`reserved` CPUs, `runtimeClassName: performance-<profile>`, optional RT kernel (`realTimeKernel.enabled`), `globallyDisableIrqLoadBalancing`; | S28, S29, S32 |
|  | "rootless DPDK pods" need the `container_use_devices` SELinux boolean only for a TAP device. Holoscan for Media: isolated `8-63`, reserved `0-7`, `nosmt`, `idle=poll`, `single-numa-node`, Guaranteed pods with `cpu: 4`, `hugepages-2Mi: 4Gi`, an SR-IOV pool; |  |
|  | PTP as "a service" (OC profile `ptp4l -2 -s` on the PF, `phc2sys -w -m -n <domain> -s <if>` at SCHED_FIFO 10); PTP Operator events by REST v1/v2 and cloud-event-proxy |  |
| time | a pod's usable sources in order: the VF-readable PHC disciplined by the node's ptp4l (TAI by definition); `CLOCK_TAI` from the node's phc2sys (sub-µs to a few µs *(inferred)*); MTL's own PTP only where no node PTP exists; `CLOCK_REALTIME` + an assumed offset, ESTIMATED; | S21, S25, S29 |
|  | DPDK's iavf has `timesync_read_time` and a PHC sync alarm (`iavf_ethdev.c:156-171`, `:272`, `:1111-1149`); ptp4l on a VF cannot servo; one PHC per E810 serves all its ports and VFs *(inferred)*; N pods with N built-in clients give N estimates |  |
|  | phc2sys sets the kernel TAI offset with `clock_adjtime(CLOCK_REALTIME, ADJ_TAI)` only when `pmc_agent_utc_offset_traceable` (`phc2sys.c:1137-1141`, `clockadj.c:211-219`), otherwise `CLOCK_TAI == CLOCK_REALTIME`; setting it needs `CAP_SYS_TIME`, which a workload pod should not have; | S25 |
|  | built-in PTP on a VF needs PTP multicast 224.0.1.129 or 01:1B:19:00:00:00 delivered to the VF, each a MAC filter of the budget |  |
| capabilities | NUMA policy calls need `SYS_NICE` under the default seccomp ("Lets DPDK set the NUMA memory policy", `docker/README.md`); `NET_RAW` for an XSK (`if (!ns_capable(net->user_ns, CAP_NET_RAW))`); loading XDP needs `NET_ADMIN` + `BPF` (or `SYS_ADMIN`) and bpffs: a node agent's job; `ethtool` ntuple rules need `NET_ADMIN` in the netdev's namespace; | S9, S14, S15, S22 |
|  | Baseline PSS forbids host namespaces, `privileged` and hostPath and allows only `AUDIT_WRITE, CHOWN, DAC_OVERRIDE, FOWNER, FSETID, KILL, MKNOD, NET_BIND_SERVICE, SETFCAP, SETGID, SETPCAP, SETUID, SYS_CHROOT`; |  |
|  | Restricted adds `runAsNonRoot`, no privilege escalation, seccomp `RuntimeDefault` or `Localhost`, drop ALL, and the volume types configMap, csi, downwardAPI, emptyDir, ephemeral, PVC, projected, secret; "If you can avoid using a hostPath volume, you should"; |  |
|  | user-namespace pods cannot use host namespaces and their capabilities "are limited to the pod user namespace" |  |
|  | the Pod API has no rlimit field (the container inherits the runtime's default), so DPDK pods add `IPC_LOCK` rather than raise memlock *(inferred)*; MTL's `docker/docker-compose.yml` uses `ulimits.memlock: -1`, which has no pod equivalent; |  |
|  | with `runAsNonRoot` and no `runAsUser` the image needs a numeric `USER`, or the kubelet refuses to start it *(inferred)* |  |
| AF_XDP | a netlink-attached program stays after its loader exits *(inferred)*; libxdp pins dispatcher components in bpffs (`/sys/fs/bpf/xdp/dispatch-IFINDEX-DID/…`) because "The kernel will automatically detach component programs … once the last reference … disappears"; | S26, S27 |
|  | manager-less MTL loads libxdp's default program ("please run with mtl manager or root user", `mt_af_xdp.c:434`), with the manager it passes `XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD` and gets the map fd (`:390-419`); |  |
|  | Intel's plugin loads the program on the host and hands the map over a per-pod UDS (30–300 s inactivity timeout, a UID setting for non-root) or pinned maps (a privileged DaemonSet for mount propagation), "User 0 does not imply that the pod needs to be privileged"; bpfman (CNCF) owns eBPF lifetimes *(inferred)* |  |
| IGMP | MTL is the IGMP host on a DPDK VF: unsolicited reports every 10 s until a query (`mt_mcast.h:14`, `mt_mcast.c:359`), a leave at teardown (`mt_mcast.c:735`); robustness 2 × query interval 125 s + response 10 s = 260 s (RFC 3376 §8.4 *(inferred)*); without a querier switches may flood indefinitely *(inferred)*; | S31 |
|  | the flood follows the VF's port, both 2022-7 legs leak, a recreated pod joins from a new port |  |
|  | if the stale-flooded VF goes to another pod, that pod's VSI filters drop the traffic, but the uplink bandwidth is still used until the group ages out |  |

How others deploy (S29, S30; the rest *(inferred)*): MTL via Media Communications Mesh (`media-proxy` DaemonSet, privileged,
root, hostPath `/dev/vfio`, VF hard-coded as `-d 0000:31:01.5`, hugepage emptyDirs, `/var/run/imtl` from a hostPath PV,
hostPorts, no probes, no `preStop`); Holoscan for Media (Rivermax; operators for SR-IOV, PTP and NUMA resources, Helm,
Guaranteed pods); Rivermax on mlx5 (a bifurcated driver: no VFIO and no reset on exit; "no privilege" per S16); telco
CNFs (Guaranteed QoS, exclusive CPUs, the CRI-O annotations, vfio VFs, health served by a control thread); VPP (a privileged
hostNetwork DaemonSet serving pods over memif or tun; probes hit the agent); OVS-DPDK (vhost-user sockets; a pod restart
does not touch the NIC); AWS CDI, EVS, Grass Valley AMPP, Nevion: no public detail (the CDI docs returned 403). Common
patterns: the NIC owner is the pod or a privileged node agent, never both; PTP is a node service; liveness reads the
control thread's view of dataplane counters, never packet flow; readiness waits for link, time lock and session start.

### 7.2 Gaps at `545a266a` and the requirements they produced

| K1 gap | At HEAD | Requirements |
|---|---|---|
| G1 | no deadline-bounded instance stop (`mtl_instance_close` had no timeout; per-session closes add up) | K-REQ-1, K-REQ-2 |
| G2 | SysV lcore table and `/tmp` lock survive container restarts, fail the PID check, need writable `/tmp` | K-REQ-4, K-REQ-5, K-REQ-8 |
| G3 | MtlManager identity from the message, world-writable socket on a hostPath, netns-blind for AF_XDP, UDP filter refcounts leak | K-REQ-17, K-REQ-18 |
| G4 | `TASKLET_THREAD` schedulers unpinned (wrong with load balancing off) | K-REQ-9 |
| G5 | no health or progress signal | K-REQ-7 |
| G6 | no port by Kubernetes resource or IPAM result | K-REQ-11 |
| G7 | no up-front check of VF trust, MAC budget, memlock, capabilities | K-REQ-6, K-REQ-10, K-REQ-12, K-REQ-16 |

The others rest on single facts: K-REQ-3 (no handlers, the PID 1 rule, DPDK's SIGBUS handler), K-REQ-13 (a VF may be reused),
K-REQ-14 (no-IOMMU is node compromise), K-REQ-15 (time from the node), K-REQ-19 (multi-container pods), K-REQ-20 (IGMP
after a crash). Their text and disposition: [requirements.md](requirements.md); the design that meets them:
[deployment.md §4](deployment.md#4-kubernetes).

## 8. Safe shutdown and crash cleanup elsewhere

Read 2026-10-01 (K3): Linux vfio, uverbs and `ib_verbs.h`, the VFIO driver-API doc; man pages `ibv_fork_init(3)`,
`ibv_get_async_event(3)`, `io_uring_register(2)`, `pthread_mutexattr_setrobust(3)`, `fcntl(2)` (`fcntl_locking(2)` not read,
so the OFD details are *(inferred)*); DPDK 26.07 headers and the EAL and multi-process guides; SPDK `env.h`, `nvme.h`;
libxdp `xsk.c`, `libxdp.c`; the DOCA Core guide; `fi_endpoint(3)`; VPP `threads.c`; OVS `ovs-rcu.c`. From knowledge only
*(inferred)*: Kubernetes termination (the fetch was truncated), Vulkan device loss, CUDA, gRPC, Envoy, Java, Go, systemd,
PipeWire, GStreamer base classes. The pattern summary is [deployment.md §4.17](deployment.md#417-why-this-shape-prior-art).

### 8.1 Facts

| Source | Fact |
|---|---|
| uverbs | `ib_uverbs_close()` → `uverbs_destroy_ufile_hw(file, RDMA_REMOVE_CLOSE)` destroys objects in rounds while any can go, then forces the rest with `RDMA_REMOVE_DRIVER_FAILURE` (`uverbs_main.c:990-1027`, `rdma_core.c:927-967`); `enum rdma_remove_reason` DESTROY ("could fail"), CLOSE, DRIVER_REMOVE, ABORT, DRIVER_FAILURE (`ib_verbs.h:1529-1543`); |
|  | a busy uobject is `-EBUSY` (`rdma_core.c:111-114`), so children go first; on removal "We disassociate HW resources and immediately return. Userspace will see a EIO errno for all future access", mmaps get "a dummy writable zero page", and without disassociation support removal waits for every close (`uverbs_main.c:695-730`, `:954-969`, `:1256-1283`); |
|  | destroy waits for affiliated async events to be acknowledged; `IBV_EVENT_DEVICE_FATAL`; `ibv_fork_init()` uses `MADV_DONTFORK` and fails with EINVAL after registration (`RDMAV_FORK_SAFE`); kernels since about 5.12 copy pinned pages early at fork *(inferred)* |
|  | destroying an MR releases its `ib_umem`, which unpins the pages *(inferred)*; `ibv_dealloc_pd` fails while QPs or MRs still use the PD *(inferred)*; `IBV_EVENT_DEVICE_FATAL` = "CA is in FATAL state" (`ibv_get_async_event(3)`) |
| DPDK | `rte_eal_cleanup()`: "After this call, no DPDK function calls may be made"; `rte_eth_dev_close()`: "The device cannot be restarted!"; `rte_eth_dev_reset()` may return `-EAGAIN`; hugetlbfs "backing files may persist … in case of a crash", removed at startup, `--in-memory` "is free of filename conflict and leftover file issues" and has no multi-process; |
|  | "Running a secondary process in a different container namespace from the primary is not supported", "There is no privilege separation"; `RTE_ETH_EVENT_INTR_RMV` callbacks run on the interrupt thread, "Care must be taken not to close the device from the interrupt handler context"; `INTR_RESET`, `ERR_RECOVERING` → `RECOVERY_SUCCESS`/`FAILED`; |
|  | no locks in callbacks (`rte_ethdev.h:4207-4292`); `rte_dev_event_monitor_start()`, `rte_dev_hotplug_handle_enable()` (`rte_dev.h:393`, `:413`; a SIGBUS handler remapping a removed BAR *(inferred)*); `rte_eth_dev_owner_new/set` is in-process only (`rte_ethdev.h:2280-2292`); `rte_lcore_register_usage_cb` feeds `/eal/lcore/info` telemetry (`rte_lcore.h:360-372`) |
|  | `rte_eth_dev_reset()` stops the port and reruns the PMD uninit and init; per the EAL guide the PMD keeps callbacks safe after the "PCI mappings [are] unmapped"; the multi-process guide says nothing about primary death (one more reason for NG5: one process, no secondaries); telemetry is read over a Unix socket without locking the lcore *(inferred)* |
| VFIO | on device release (`vfio_pci_core_close_device` → `vfio_pci_core_disable`): D0, `pci_clear_master()` ("Stop the device from further DMA"), IRQs off, ioeventfds and BAR maps released, `needs_reset`, PCI_COMMAND with INTx disabled, `__pci_reset_function_locked()` (FLR) if it works, config restored, a bus or slot reset if still dirty; on open `pci_try_reset_function()`; |
|  | type1 and iommufd unpin only after the device is detached, so pinned pages are never reused while DMA is possible *(inferred)*; no-IOMMU "w/o physical IOMMU protection", taints the kernel *(inferred)*, and at exit `exit_mm` runs before `exit_files` *(inferred, spike SF-K3-6)*; the request IRQ (`VFIO_PCI_REQ_IRQ_INDEX`) asks userspace to release a device being unregistered |
|  | the PCI_COMMAND write with INTx disabled also clears bus master and decode; the bus or slot reset runs only when no other device in the set is open; the VFIO driver-API doc covers only the `dma_unmap` callback (the unpin-after-detach order is *(inferred)*); no-IOMMU needs `CAP_SYS_RAWIO` (`drivers/vfio/group.c:428-429`) |
| other libraries | SPDK `spdk_env_fini()` ("no SPDK env function calls may be made"), `spdk_nvme_detach_async()` + `_poll_async()` from one thread (`env.h:163-170`, `nvme.h:1172-1210`); Rivermax §4.1 teardown; DOCA `doca_ctx_stop()` enters **stopping**: "all in-flight tasks are guaranteed to fail", the app progresses them, then idle, contexts before the progress engine; |
|  | libfabric silent discard (§3.1); CUDA `cudaDeviceReset()` destroys everything at once, CUDA from `atexit` fails *(inferred)*; Vulkan children before `vkDestroyDevice`, `vkDeviceWaitIdle`, after `VK_ERROR_DEVICE_LOST` waits finish and destroy succeeds *(inferred)*; io_uring: no explicit unregister needed, unpin may be asynchronous, resource tags; |
|  | libxdp `xsk_umem__delete` `-EBUSY` while sockets use it, the default program's refcount in a BPF map, attach by netlink with freplace links pinned in bpffs under a `flock`, stale pins cleaned only on request (`libxdp_clean_references()`); a UMEM stays pinned until the last socket and fd go; an XDP `bpf_link` detaches at its last fd unless pinned *(inferred)* |
|  | DOCA: stop then destroy also for mmap and inventory objects; CUDA `cudaDeviceReset()`: the caller must make sure no other thread uses the device *(inferred)*; Rivermax: `rmx_output_media_cancel_unsent_chunks()`, then retry `rmx_output_media_destroy_stream()` while `RMX_BUSY` (chunks in flight); |
|  | libfabric `fi_close(ep)`: outstanding operations discarded with no completions, buffers unusable until a completion or close returns; a scalable EP with open contexts is `-FI_EBUSY`; MRs bound to an EP close first (`fi_endpoint(3)`) |
| frameworks | GStreamer PLAYING → PAUSED → READY → NULL, `unlock`/`unlock_stop`, FLUSH_START/STOP, a deactivated pool frees returning buffers; PipeWire reclaims a dead client's memfd buffers at socket close; gRPC `GracefulStop()` raced against a timer calling `Stop()`; Envoy `--drain-time-s`, `/drain_listeners?graceful`; Java `Runtime.addShutdownHook` (never on SIGKILL; |
|  | "finish their work quickly"); Go `signal.NotifyContext`, `os.Exit` skips defers; systemd SIGTERM, `TimeoutStopSec` (90 s), SIGKILL to the cgroup, `EXTEND_TIMEOUT_USEC`, `WatchdogSec` + `WATCHDOG=1` (all *(inferred)*) |
|  | PipeWire `pw_stream_disconnect` comes before destroy *(inferred)*; Java shutdown hooks run on normal exit and on SIGTERM, SIGINT and SIGHUP; Kubernetes marks endpoints terminating before `preStop`; the SR-IOV device plugin hands the same VF to the next pod with no deallocate hook *(inferred)*; lesson: cleanup in a hook is best effort, never the guarantee |
| quiesce shapes | `vkDeviceWaitIdle` + destroy; DOCA stop (`DOCA_ERROR_IN_PROGRESS`, stopping → idle callback); SPDK detach poll (`-EAGAIN` *(inferred)*); Rivermax `RMX_BUSY`; io_uring tags; gRPC two stages; systemd result `timeout`/`success`; uverbs disassociate (EIO, fd stays); |
|  | `cudaDeviceReset`. None reports "device quiesced, memory still referenced" as one value: the device stage is bounded, memory release is deferred and notified |
| liveness | VPP: the main thread waits for `workers_at_barrier == count`, past `cpu { barrier-timeout }` prints "worker thread deadlock" and calls `os_panic()` (`threads.c`); OVS: "blocked %u ms waiting for %s to quiesce" at 1 s, doubling (`ovs-rcu.c`), `pmd-stats-show` *(inferred)*; DPDK lcore usage telemetry; |
|  | systemd watchdog. Shared shape: a per-thread counter (relaxed store), a supervisor comparing two readings, warn → event → panic |
| cross-process locks | `flock` and OFD locks (`F_OFD_SETLK`) are released by the kernel at the last close of the open file description, across PID, IPC and user namespaces when the inode is shared, not on NFS; OFD `F_OFD_GETLK` reports `l_pid = -1` *(inferred)*; POSIX `fcntl` locks drop at **any** close by the process (unsafe in a library); |
|  | a daemon socket is closed by the kernel (`SO_PEERCRED`, `SO_PEERPIDFD` 6.5+); robust mutexes give `EOWNERDEAD`, then `ENOTRECOVERABLE` without `pthread_mutex_consistent`; `pidfd` is immune to PID reuse; `kill(pid, 0)` is a poll that fails across PID namespaces; SysV `shm_nattch` is per IPC namespace; |
|  | cgroup v2 `cgroup.events` `populated 0` tells a node agent a pod is gone; the kubelet arbitrates CPUs and VFs |
|  | POSIX `fcntl` locks translate `l_pid` across PID namespaces, 0 when invisible *(inferred)*; robust mutexes detect death across PID namespaces, but the stored TID means nothing elsewhere, PI futexes break, and across IPC namespaces they need a shared file mapping (not SysV) *(inferred)*; a `pidfd` needs the PID visible or a pidfd passed over `SCM_RIGHTS` *(inferred)* |

### 8.2 Patterns P-1…P-12

| ID | Pattern | From | Carried by |
|---|---|---|---|
| P-1 | the process is the unit of cleanup: anchor everything external on a descriptor the kernel closes; list and reconcile what cannot be | uverbs close, VFIO release, AF_XDP UMEM, OFD locks, MtlManager socket EOF | mtl.h R8; descriptors close-on-exec; residuals listed ([deployment.md §4.5](deployment.md#45-the-crash-contract)) |
| P-2 | two stages, one deadline: drain, then a bounded hard stop that never waits on the app or the network | gRPC, Envoy, systemd, the grace period, DOCA stopping | `mtl_instance_close(mt, timeout_ns)`, `mtl_instance_shutdown` (D-89); a second SIGTERM → `mtl_instance_abort` |
| P-3 | report "device quiesced" apart from "retired" | `vkDeviceWaitIdle`, VFIO bus master off, io_uring tags, DOCA, uverbs | returns 0 / 1 / `-MTL_EIO`, `struct mtl_shutdown_report` ([deployment.md §4.3](deployment.md#43-outcomes-and-the-report)) |
| P-4 | children before parents by references, with a forced last round; memory under a stale lease kept until exit | Vulkan, libfabric `-EBUSY`, uverbs rounds, GStreamer refcounts | instance closes its sessions; `bytes_kept`; never a use-after-free |
| P-5 | device loss is a state: fail fast with one code, bounded waits, close never touches the device | uverbs disassociate, `VK_ERROR_DEVICE_LOST`, DPDK `INTR_RMV`, VFIO request IRQ | `MTL_EVENT_PORT_REMOVED`, `DEVICE_GONE`, `-MTL_ENODEV`, `instance.hotplug` default off (EK14; deployment.md §4.6) |
| P-6 | fd-anchored CPU leases; never PIDs; defer to the kubelet | flock, OFD, libxdp, socket EOF, CPU manager | affinity mask, OFD byte locks in `instance.runtime_dir`, `instance.cpu_arbitration` (D-92, EK6) |
| P-7 | kernel programs detached by fd close, or pinned on purpose and reconciled | libxdp, `bpf_link` | MtlManager holds a `bpf_link` or reconciles (EK5); `port.xsk_map` from a node agent |
| P-8 | lock-free heartbeat counters, a doubling warning ladder, the library never kills | VPP, OVS, DPDK telemetry, systemd watchdog, probes | `mtl_instance_get_health`, `MTL_EVENT_SCHED_STALLED`, `sched.loops`, `sched.last_loop_tai_ns` (D-91) |
| P-9 | no handlers or `atexit` in the library; AS kicks; the work on a normal thread | DPDK, Java, Go, systemd, PID 1 | mtl.h R8; the ex11 recipe ([deployment.md §4.4](deployment.md#44-signals-and-fork-r8)) |
| P-10 | a fork rule | `ibv_fork_init`, `MADV_DONTFORK` | mtl.h R8: `-MTL_EBADF` (reason `FORKED`) in the child, descriptors closed, library memory `MADV_DONTFORK` |
| P-11 | startup reconciliation | EAL backing files, VFIO reset at enable, `libxdp_clean_references`, manager re-announce | open flushes `rte_flow` and rate-limit state, logs `instance.reconciled{kind}` |
| P-12 | say who holds what when close cannot finish | Vulkan validation, GStreamer leaks tracer, io_uring tags | `session.leases_out`, `session.oldest_lease_ns`, the report's counts |

### 8.3 Code findings, gaps and what the headers changed

- **Findings F-1…F-8** (`545a266a`): F-1 no ethdev event callback is registered (only `rte_eal_hotplug_add` for vdevs,
  `dev/mt_dev.c:1558`) → EK14; F-2 manager-less arbitration is one `flock` on `/tmp/kahawai_lcore.lock` over a SysV table
  (`mt_sch.c:505-605`, `:686-698`, `:1310-1330`) and F-3 its pod consequences (no arbitration across IPC namespaces; with
  `hostIPC` + `hostNetwork` a live pod's CPUs freed, EPERM counted as dead) → EK6; F-4 MtlManager releases at socket EOF
  (`mtl_manager.cpp:186-198`, `mtl_instance.hpp:66-89`) but trusts a self-reported PID and UID (`:191-194`, also SF-47) → EK5;
  F-5 `load_xdp` overwrites the SKB mode with NATIVE (`mtl_interface.hpp:428-430`, `:450-452`) → EK5; F-6 manager-less
  AF_XDP's libxdp program refcount leaks on SIGKILL (`mt_af_xdp.c:417-419`): **corrected in review**, the path is
  unreachable at HEAD because native AF_XDP needs MtlManager; F-7 EAL `--in-memory` with a file prefix (`mt_dev.c:337-345`);
  F-8 `mtl_abort` only sets `instance_aborted` (an atomic store, so already AS-safe) and `mtl_uninit` calls
  `rte_eal_cleanup()`, so EAL cannot re-init in that process (`mt_main.c:747-757`). The full audit is [deployment.md §4.16](deployment.md#416-todays-code-at-545a266a).
- **K3 gaps** G1 no instance shutdown with a deadline and report; G2 no device-removal contract; G3 no liveness signal; G4
  SysV arbitration in pods; G5 XDP programs outlive a crash; G6 no IGMP leave after SIGKILL (inherent, mitigated); G7 no
  fork rule. All are closed by P-1…P-12 above.
- **The K3 proposal against the headers** (the headers win): `mtl_instance_shutdown` keeps K3's signature but returns
  `-MTL_EIO` (`QUEUE_QUARANTINED`), never `-MTL_ETIMEDOUT`; `MTL_SHUTDOWN_ABORT` became timeout 0 or `mtl_instance_abort`
  during the call, and `MTL_SHUTDOWN_PROCESS` became `MTL_SHUTDOWN_ALL_REFERENCES`, with `MTL_SHUTDOWN_DRAIN` added; the
  report has `bytes_kept`, `threads_unjoined`, `groups_left`, `summary[128]` instead of `bytes_leaked_until_exit` and
  `plugins_unjoined`; `mtl_instance_health` is `mtl_instance_get_health` (`mtl_observe.h`); `instance.stall_ns` defaults to
  1 s, not 100 ms (thread mode sees 100 ms preemptions); `instance.hotplug_sigbus` is `instance.hotplug`; reason
  `DEVICE_REMOVED` is `MTL_REASON_DEVICE_GONE`; K3's R8 (fork) and R9 (signals) are one rule R8; `instance.allow_noiommu`
  as proposed; the core stayed at 32 functions in `mtl.h` because shutdown lives in `mtl_observe.h`.
- **Side findings** SF-K3-1 XDP mode overwrite → EK5; SF-K3-2 ethdev RMV, RESET, RECOVERY callbacks → EK14; SF-K3-3
  replace SysV lcores → EK6; SF-K3-4 MtlManager identity, reconciliation, `bpf_link` → EK5; SF-K3-5 `O_CLOEXEC` audit →
  EK15; SF-K3-6 spike: no-IOMMU DMA window at SIGKILL, and spike S8 (queue stop releases external mbufs) gating P-3; SF-K3-7
  manager-sent IGMP leaves for dead clients (later). Engine items: [engine.md](engine.md).
- **Questions** Q-K3-1, Q-K3-2, Q-K3-3, Q-K3-4, Q-K3-5, Q-K3-6, Q-K3-7, Q-K3-8: [questions.md](questions.md).

## 9. Bibliography

Every external source the former design files cite, one row per document or family. "Cited for" names the section of this file or
the maintained document that uses it.

| Source | Edition and location | Cited for |
|---|---|---|
| SMPTE ST 2110-10 | 2022, <https://pub.smpte.org/latest/st2110-10/st2110-10-2022.pdf> | §2; timing.md |
| SMPTE ST 2110-20 | 2022, <https://pub.smpte.org/pub/st2110-20/st2110-20-20221214-pub.ZIP> | §2.3, §2.5 |
| SMPTE ST 2110-21 | 2022, <https://pub.smpte.org/pub/st2110-21/st2110-21-20221214-pub.ZIP> | §2.5; timing.md §5 |
| SMPTE ST 2110-22 | 2022, <https://pub.smpte.org/pub/st2110-22/st2110-22-20220331-pub.ZIP> | §2.7 |
| SMPTE ST 2110-30 | 2025, <https://pub.smpte.org/pub/st2110-30/st2110-30-20251001-pub.ZIP> | §2.6 |
| SMPTE ST 2110-31 (AES3 transparent), ST 2110-43 (timed text) | not read; named by I1 (TR-10-12) and S8 | nmos-ipmx.md; packet units |
| SMPTE ST 2110-40 | 2023, <https://pub.smpte.org/pub/st2110-40/st2110-40-20231231-pub.ZIP> | §2.7; timing.md §9 |
| SMPTE ST 2110-41 | 2026-06, <https://pub.smpte.org/pub/st2110-41/st2110-41-20260629-pub.ZIP> | §2.7 |
| SMPTE ST 2059-1 | 2021, <https://pub.smpte.org/pub/st2059-1/st2059-1-20201209-pub.ZIP> | §2.2 |
| SMPTE ST 2059-2 | PTP profile; SM TLVs (local offset, daily jam); not read | §2.11, timecode (later) |
| SMPTE ST 2022-7 | 2019, <https://pub.smpte.org/pub/st2022-7/st2022-7-20181226-pub.ZIP> | §2.8 |
| SMPTE ST 2022-5 (FEC), ST 2022-6 (SDI over IP) | not read; TR-10-6 FEC profile, `SMPTE2022-6/27000000` encoding (N1) | nmos-ipmx.md |
| SMPTE ST 12-1, ST 12-2, RP 188 | timecode and its ANC carriage; not read | timing.md §17 |
| SMPTE ST 291-1, RP 168, ST 272, ST 299, ST 318, ST 2051 | ANC packets, switching point, audio embedding, ten-field sequence, two-frame mode; via the texts above | §2.2, timing.md §8, §9 |
| IEEE 1588-2008 (PTP) | via ST 2059-1 and `ts-refclk` | §2.2 |
| AES67 | paywalled; secondary summary <https://en.wikipedia.org/wiki/AES67> | §2.6 |
| AES3 | via ST 2059-1 §8.1, -10 §7.7.5 | §2.2, §2.3 |
| RFC 3550 (RTP) | <https://www.rfc-editor.org/rfc/rfc3550> | §2.2, timing.md §4.4 |
| RFC 4175 (uncompressed video), RFC 8331 (ANC), RFC 9134 (JPEG XS) | `https://www.rfc-editor.org/rfc/rfcNNNN` | §2.3, §2.7 |
| RFC 7273 (`mediaclk`, `ts-refclk`) | `https://www.rfc-editor.org/rfc/rfc7273` | §2.2 |
| RFC 3376 (IGMPv3) | <https://www.rfc-editor.org/rfc/rfc3376> (S31, not re-read) | §7.1; deployment.md §4.5 |
| RFC 4566 (SDP), RFC 4570 (source filters), RFC 7104 (duplication grouping), RFC 3605 (`a=rtcp`) | not read in these studies; named by N1 and RN | nmos-ipmx.md, `mtl_sdp.h` |
| RFC 8285 (header extensions), RFC 6184 (H.264), RFC 7798 (H.265), RFC 3640 (MPEG-4 audio) | named by I1 and S8 | nmos-ipmx.md, packet units |
| VSF TR-03 | 2015-11-12, <https://static.vsf.tv/download/technical_recommendations/VSF_TR-03_2015-11-12.pdf> | §2.2, §2.9 |
| VSF TR-10-1 (IPMX system timing) | 2024-02-23, <https://static.vsf.tv/download/technical_recommendations/VSF_TR-10-1_2024-02-23.pdf> | §2.9; nmos-ipmx.md |
| VSF TR-10-0, -2 … -16 (IPMX parts) | editions and status of every part as listed 2026-10-01 (the table in [nmos-ipmx.md §2.1](nmos-ipmx.md#21-which-specifications-need-mtl); -9 v2 Draft 2025-05-13 is the PQCR baseline) at <https://vsf.tv/technical-recommendations/> (texts `static.vsf.tv/download/technical_recommendations/VSF_TR-10-*.pdf`); TR-10 TP-1 (2026-07-31) | nmos-ipmx.md |
| AIMS IPMX PQCR v1.1 and profiles | <https://ipmx.io/technical-information/> | nmos-ipmx.md |
| HDCP 2.3 | via TR-10-5; not read | nmos-ipmx.md |
| AMWA IS-04 | v1.3.3, <https://specs.amwa.tv/is-04/releases/v1.3.3/> (schemas `…/APIs/schemas/`, `…/docs/Behaviour_-_Nodes.html`) | nmos-ipmx.md |
| AMWA IS-05 | v1.2.0, <https://specs.amwa.tv/is-05/releases/v1.2.0/> (`docs/Behaviour.html`, `Behaviour_-_RTP_Transport_Type.html`, `Interoperability_-_IS-04.html`) | §2.1; nmos-ipmx.md; timing.md §13 |
| AMWA IS-07, IS-08 (v1.0.1 Behaviour), IS-09 (v1.0.0), IS-10, IS-11 (v1.0.0 server-side), IS-12, IS-13 | `https://specs.amwa.tv/is-NN/…` | nmos-ipmx.md |
| AMWA MS-04 | v1.0.0, <https://specs.amwa.tv/ms-04/releases/v1.0.0/docs/2.5._Explanation_-_Timing.html> | §2.9 |
| AMWA MS-05-02, NMOS Parameter Registers | <https://specs.amwa.tv/ms-05-02/>, <https://specs.amwa.tv/nmos-parameter-registers/> | nmos-ipmx.md |
| AMWA BCP-002-01/02, BCP-003-01, BCP-004-01/02 (v1.0.0), BCP-005-01/02/03, BCP-006-01 (v1.0.0), -02, -03, -04, BCP-007-01, BCP-008-01/02 (v1.0.0) | `https://specs.amwa.tv/bcp-NNN-NN/…`; BCP-007-01 at <https://github.com/AMWA-TV/bcp-007-01> | nmos-ipmx.md |
| JT-NM Tested criteria | via EBU LIST (2020 and 2022 profiles); the test plan PDFs not read | §2.5, §2.6 |
| EBU LIST | <https://github.com/ebu/pi-list/tree/master/docs> and source (master) | §2.5, §2.6, §2.9 |
| EBU Tech 3337 (TS-DF), EBU R37, ITU-R BT.1359-1, ATSC IS-191 | via LIST and secondary sources | §2.6, §2.9 |
| libfabric | v2.7 `main`: man pages <https://ofiwg.github.io/libfabric/main/man/> (`fi_endpoint.3.html` …), headers `https://raw.githubusercontent.com/ofiwg/libfabric/main/include/rdma/{fabric.h,fi_domain.h,fi_endpoint.h,fi_errno.h}`, `src/abi_1_0.c`, `NEWS.md`, wiki; PR #5566 | §3, §8.1 |
| Rivermax examples | <https://github.com/NVIDIA/rivermax-examples> at `4dca2694` (`api_demo/output_media/{memory_registration_media_send,memory_allocation_media_send,media_send,hds_media_send,dynamic_media_send}`, `api_demo/output_generic/generic_send`, | §4 |
|  | `api_demo/input/{memory_registration_receive,memory_allocation_receive,receive,multi_source_receive,drain_detached_flow_receive}`, `legacy/rivermax_player`, `legacy/generic_receiver`) |  |
| Rivermax Dev Kit | <https://github.com/NVIDIA/rivermax-dev-kit> at `ebbb89e4` (`media_chunk.h`, `header_payload_memory_layout.h`, `media_essence_source.h`, `memory_allocator_interface.h`) | §4 |
| DeepStream `gst-nvdsudp` | <https://github.com/NVIDIA/DeepStream> at `0a63ef8b`, `src/gst-plugins/gst-nvdsudp/` (`gstnvdsudpsink.c`, `gstnvdsudpsrc.c`, `legacy_api/`); its README pins Rivermax 1.80.x | §4 |
| Unreal Engine RivermaxCore (unofficial mirror) | <https://github.com/orgitcog/u9n> at `97a2175e`, `Plugins/VirtualProduction/Rivermax/RivermaxCore/Source/RivermaxCore/Private/` (`RivermaxWrapper.h/.cpp`, `RivermaxManager.cpp`, `Streams/RivermaxOutStream.cpp`, `Streams/RivermaxInputStream.cpp`) | §4 |
| DOCA Rivermax guide; DOCA Core | <https://docs.nvidia.com/doca/sdk/doca-rivermax/index.html> (DOCA 3.5.0); <https://networking-docs.nvidia.com/doca/sdk/doca+core> | §4, §8.1 |
| Rivermax product page and FAQ | <https://developer.nvidia.com/networking/rivermax>, <https://developer.nvidia.com/networking/rivermax/faq> | §4 |
| Blackmagic DeckLink | SDK Manual March 2026 <https://documents.blackmagicdesign.com/UserManuals/DeckLinkSDKManual.pdf>; SDK 12.0 header <https://github.com/obsproject/obs-studio/blob/master/plugins/decklink/linux/decklink-sdk/DeckLinkAPI.h>; SDK 12.2.2 header in GStreamer `gst-plugins-bad/sys/decklink/linux/DeckLinkAPI.h` | §6 |
| AJA libajantv2 | <https://github.com/aja-video/libajantv2/blob/main/ajantv2/includes/ntv2publicinterface.h>, `…/ntv2card.h` | §6 |
| GStreamer | design docs <https://gstreamer.freedesktop.org/documentation/additional/design/qos.html>, `latency.html`, `synchronisation.html`, `bufferpool.html`; <https://gstreamer.freedesktop.org/documentation/base/gstbasesink.html>; source `gstbuffer.h`, `gstrtpjitterbuffer.c` on gitlab.freedesktop.org | §6 |
| io_uring | <https://github.com/torvalds/linux/blob/master/include/uapi/linux/io_uring.h>; man7 `io_uring(7)`, `io_uring_setup(2)`, `io_uring_enter(2)`, `io_uring_register(2)` | §6, §8.1 |
| Vulkan | `vulkan_core.h` (KhronosGroup/Vulkan-Headers), `fundamentals.adoc` (Vulkan-Docs); refpages `VkPastPresentationTimingGOOGLE`, `VkPastPresentationTimingEXT`, `VkPresentTimingInfoEXT`, `vkSetSwapchainPresentTimingQueueSizeEXT`, `vkQueuePresentKHR` at docs.vulkan.org | §6 |
| OpenXR 1.1 | <https://registry.khronos.org/OpenXR/specs/1.1/man/HTML/xrWaitFrame.html>, `XrFrameState.html`, `xrBeginFrame.html` | §6 |
| ALSA | <https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html>; kernel `Documentation/sound/designs/timestamping.rst`, `include/uapi/sound/asound.h`; alsa-lib `include/pcm.h` | §6 |
| JACK, PipeWire, CoreAudio | <https://github.com/jackaudio/headers>; PipeWire `src/pipewire/stream.h` on gitlab.freedesktop.org; MacOSX 11.3 SDK headers via <https://github.com/phracker/MacOSX-SDKs> | §6 |
| NDI, SRT, WebRTC, OpenTelemetry | <https://docs.ndi.video/all/developing-with-ndi/sdk/ndi-send>; <https://github.com/Haivision/srt/blob/master/docs/API/statistics.md>; <https://www.w3.org/TR/webrtc-stats/>; <https://opentelemetry.io/docs/specs/otel/metrics/data-model/> | §6 |
| DPDK | <https://github.com/DPDK/dpdk> (`rte_mbuf_dyn.h`, `rte_ethdev.h`, `rte_mbuf_core.h`, `eal_common_options.c`, `eal_memalloc.c`, `eal_vfio.c`, `telemetry.c`, iavf `iavf_ethdev.c`); 26.07 headers; | §6, §7, §8; deployment.md |
|  | guides `linux_gsg/linux_drivers.html`, `linux_eal_parameters.html`, `enable_func.html`, `prog_guide/env_abstraction_layer.html`, `multi_proc_support.html` at doc.dpdk.org |  |
| SPDK | <https://github.com/spdk/spdk/blob/master/include/spdk/env.h>, `nvme.h` | §8.1 |
| Linux kernel | vfio (`vfio_pci_core.c` <https://raw.githubusercontent.com/torvalds/linux/master/drivers/vfio/pci/vfio_pci_core.c>, `vfio_iommu_type1.c`, `group.c`), uverbs <https://github.com/torvalds/linux/tree/master/drivers/infiniband/core>, `ib_verbs.h`, ice (`ice_vf_lib.c`, `virtchnl.c`, `virtchnl.h`), iavf `iavf_ptp.c`, `net/xdp/xsk.c`, `kernel/time/namespace.c`; | §7, §8 |
|  | <https://docs.kernel.org/driver-api/vfio.html> |  |
| Linux man pages | man7 `ibv_fork_init(3)`, `ibv_get_async_event(3)`, `pthread_mutexattr_setrobust(3)`, `fcntl(2)`; not read: `fcntl_locking(2)`, `pid_namespaces(7)`, `sched_setaffinity(2)`, `prctl(2)`, `systemd.service(5)` | §7, §8 |
| linuxptp | <https://github.com/richardcochran/linuxptp> (`phc2sys.c`, `clockadj.c`) | §7.1; deployment.md §4.1 |
| libxdp, xdp-tools | <https://github.com/xdp-project/xdp-tools/tree/main/lib/libxdp> (`xsk.c`, `libxdp.c`, `README.org`) | §7.1, §8.1 |
| AF_XDP plugins for Kubernetes | <https://github.com/intel/afxdp-plugins-for-kubernetes> (unmaintained; fork `redhat-et/afxdp-plugins-for-kubernetes`) | §7.1 |
| VPP, OVS | <https://github.com/FDio/vpp/blob/master/src/vlib/threads.c>; <https://github.com/openvswitch/ovs/blob/main/lib/ovs-rcu.c> | §8.1 |
| Kubernetes documentation (S1–S3, S5–S15) | S1 <https://kubernetes.io/docs/concepts/workloads/pods/pod-lifecycle/>, S2 `…/concepts/containers/container-lifecycle-hooks/`, S3 `…/concepts/workloads/pods/sidecar-containers/`, S5 `…/tasks/configure-pod-container/share-process-namespace/`, S6 `…/concepts/scheduling-eviction/node-pressure-eviction/`, | §7 |
|  | S7 `…/concepts/cluster-administration/node-shutdown/` |  |
| Kubernetes documentation, continued | S8 `…/tasks/configure-pod-container/configure-liveness-readiness-startup-probes/`, S9 `…/concepts/storage/volumes/`, S10 `…/concepts/workloads/pods/`, S11 `…/tasks/manage-hugepages/scheduling-hugepages/`, S12 `…/tasks/administer-cluster/cpu-management-policies/`, S13 `…/tasks/administer-cluster/topology-manager/`, | §7 |
|  | S14 `…/concepts/security/pod-security-standards/`, S15 `…/concepts/workloads/pods/user-namespaces/` |  |
|  | review RK also `…/reference/command-line-tools-reference/feature-gates/`, `…/tasks/debug/debug-application/determine-reason-pod-failure/` (all under <https://kubernetes.io/docs/>) |  |
| Kubernetes KEPs and issues | KEP-753 sidecars <https://github.com/kubernetes/enhancements/tree/master/keps/sig-node/753-sidecar-containers>; KEP-2238 probe grace period; kubernetes/kubernetes#56374 (capabilities of non-root containers, open), #92211 (device ownership from security context) | §7.1; deployment.md §4.14 |
| SR-IOV (S16–S18) | <https://github.com/k8snetworkplumbingwg/sriov-network-device-plugin> (`docs/dpdk/README.md`, `README-virt.md`, `pkg/resources/pool_stub.go`), | §7.1; deployment.md §4.1 |
|  | <https://github.com/k8snetworkplumbingwg/sriov-cni> (`cni.go`, `sriov.go`, `config.go`, configuration reference), <https://github.com/k8snetworkplumbingwg/sriov-network-operator> (`sriovnetwork_types.go`, `advanced-features.md`) |  |
| OpenShift 4.16 (S28, S32) | <https://docs.redhat.com/en/documentation/openshift_container_platform/4.16/HTML-single/scalability_and_performance/index>; <https://docs.redhat.com/en/documentation/openshift_container_platform/4.16/HTML/networking/using-ptp-hardware> (headings only) | §7.1 |
| NVIDIA Holoscan for Media (S29) | <https://docs.nvidia.com/holoscan-for-media/latest/user-guide/app-development/index.html>, platform setup `openshift-cluster-tuning.html`, `openshift-cluster-configuration.html` | §7.1 |
| Media Communications Mesh (S30) | <https://github.com/OpenVisualCloud/Media-Communications-Mesh/tree/main/deployment> (`DaemonSet/media-proxy*.yaml`, `pv.yaml`) | §7.1; deployment.md §4.1 |
| External MTL consumers | <https://github.com/bob-integration/bobistudio-plugin-2110_io>, <https://github.com/NUDA9A/ST2110-OBS-PLUGIN>, <https://github.com/CorangesS/GPT_mtl_encode_sdk>, <https://github.com/OpenVisualCloud/directview-led-software-toolkit> | research.md (consumers survey) |
| MTL issues and PR #1610 | issues #657, #687, #870, #948, #1139, #1147, #1157, #1170, #1176, #1179, #1185, #1208, #1211, #1222, #1239, #1242, #1276, #1305, #1318, #1321, #1325, #1337, #1341, #1357, #1370, #1378, #1424, #1517, #1560, #1620, #1678 at <https://github.com/OpenVisualCloud/Media-Transport-Library/issues>; | research.md, history.md |
|  | PR <https://github.com/OpenVisualCloud/Media-Transport-Library/pull/1610> (files at `14a1f80c`: `include/mtl_session_api.h`, `lib/src/new_api/mt_session_{buffer,video_rx,video_tx}.c`; baseline files at `bf58f6e9`) |  |
| Vendor app notes on ST 2110-30 levels | DirectOut and an unattributed web snippet (secondary) | §2.6 |
| MTL current API (comparison baseline) | `include/st_api.h` (`st_frame_status`, `st_tx_user_stats`), `include/st20_api.h` (`notify_frame_done`, `notify_frame_late`), `include/st_pipeline_api.h` (`*_reset_session_stats`) | §6 |
