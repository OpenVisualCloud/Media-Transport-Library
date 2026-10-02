# 17 — NMOS and IPMX

| | |
|---|---|
| Status | Draft for maintainer review — revision 4, addendum N (2026-10-01), after reviews [RN](reviews/RN-nmos-ipmx-review.md) and [RA](reviews/RA-api-coherence-review.md) ([response](reviews/RKNA-response.md)) |
| Date | 2026-10-01 |
| Baseline | `main` @ `545a266a` |
| Requirements | [N1 — NMOS requirements](interop/N1-nmos-requirements.md) (N-REQ-1…60, gaps G-N1…25, conflicts C-N1…14), [I1 — IPMX requirements](interop/I1-ipmx-requirements.md) (I-REQ-1…76, gaps GI-1…17, conflicts C-I1…14) |
| Headers | `mtl.h` (update, status, media mode SENDER, FREERUN, colorimetry fields, reserved legs, meta records, `MTL_SUBMIT_SENDER_TIME`, `MTL_WAIT_RTCP`); new `mtl_rtcp.h`, `mtl_crypto.h`, `mtl_sdp.h`; `mtl_sync.h` (time reference), `mtl_format.h` (colorimetry enums), `mtl_options.h`, `mtl_queue.h` (events 26–30), `mtl_packet.h` (`MTL_PKTE_HDR_EXT`) |
| Decision | M17 ([OPEN-QUESTIONS.md](OPEN-QUESTIONS.md)) |
| Implementation | **Phase 7, later** ([14 §3.8](14-implementation-roadmap.md), D-98): the design is kept so the API will not change for it; Phases 1–6 port today's functionality first. Only the atomic update with its planned instant and per-leg enable are Phase 2. `mtl_sdp.h`, `mtl_rtcp.h` and `mtl_crypto.h` are under `MTL_LATER` |

NMOS (AMWA IS-04, IS-05, the BCPs) is how an ST 2110 device is discovered, connected and
monitored. IPMX (VSF TR-10) is a profile on top of ST 2110 and NMOS for ProAV: it works
without PTP, carries async sources, adds RTCP sender reports with an Info Block, and adds
optional encryption. MTL is a transport, not an NMOS Node, so the split stays as it is:

- **The Node** (the application or a library such as nmos-cpp) owns the registry, the REST
  APIs, the JSON, the staged and active parameters, `master_enable`, and the policy: which
  activation to accept, and when to report a status change.
- **MTL** owns everything only the transport knows or can do on time: the granted values,
  the instant an activation took effect, the time reference, the wire format, the reports
  the wire must carry, and the counters.

The two studies found that revision 4 has the right shape: one atomic `mtl_session_update`
at a TAI instant, legs with an admin state, and named stats. They also found 25 NMOS gaps
and 17 IPMX gaps. This document says what is adopted, in which header, and what is deferred.
The core header gains no function: `mtl_session_update` returns its planned instant, as
`mtl_session_start` returns T0. Three new optional headers carry the rest, which only NMOS
and IPMX applications include.

## 1. The changes in one page

| Area | Change | Header |
|---|---|---|
| IS-05 activation | `mtl_session_update(..., &planned_tai_ns)`; the switch happens at the slot boundary by the clock, whether media flows or not; `status.update_state`, `update_applied_tai_ns`, `update_seq`; `MTL_EVENT_UPDATE` | `mtl.h`, `mtl_queue.h` |
| IS-05 semantics | `MTL_UPDATE_REAPPLY`, `MTL_UPDATE_DRY_RUN`; cancel is `parts` 0; every leg may be disabled (mute, `MTL_STATUS_MUTED`); reserved legs without an address; port changes while running, made before break; R options in the update's config apply at its boundary; neighbours resolved after commit; a past instant means now | `mtl.h` |
| IS-04 values | colorimetry, TCS and range in the video and compressed-video configs; the port's MAC from `mtl_port_get_spec`; `mtl_time_set_reference()` for a node-disciplined clock; `MTL_EVENT_GRANDMASTER`, `MTL_EVENT_PORT_ADDRESS` | `mtl.h`, `mtl_format.h`, `mtl_sync.h`, `mtl_queue.h` |
| SDP | `mtl_sdp_render()`, `mtl_sdp_parse()` | `mtl_sdp.h` |
| IPMX timing | `MTL_TIME_SOURCE_FREERUN`; AUTO re-evaluated while running, `time.fallback`, `time.freerun_slew_ppm`; `MTL_MEDIA_SENDER` for async sources; `MTL_SUBMIT_SENDER_TIME` for inline processors; `MTL_UNITF_SENDER_TIME` on RX | `mtl.h`, `mtl_options.h` |
| IPMX wire | `session.profile = ipmx`; `video.vtotal`, `video.htotal`, `cvideo.max_bitrate_bps`; `port.igmp_version`; `tx.precede`; `MTL_FLOWF_DSCP_LITERAL`; RX honours RTP header extensions and CSRCs (`MTL_PKTE_HDR_EXT`) | `mtl.h`, `mtl_options.h`, `mtl_packet.h` |
| RTCP | sender reports with SDES and the IPMX Info Block (TX), report reception and sender-time mapping (RX); MTL's NACK retransmission options renamed `rtx.*` | `mtl_rtcp.h`, `mtl_options.h` |
| Encryption | IPMX PEP (AES-CTR): the parameters are options `crypto.*`, keys only through `mtl_crypto_set_key()` | `mtl_crypto.h`, `mtl_options.h` |
| Monitoring | BCP-008 maps onto existing counters and events, plus a few keys (§2.5) | `mtl_observe.h` keys |

Count: `mtl_time_set_reference` is the one new v1 function for NMOS; SDP, RTCP and PEP add 6
under `MTL_LATER`. In all: 125 functions plus 14 for later phases; `mtl.h` has 32.

## 2. NMOS

### 2.1 IS-04: what a Node publishes that only the transport knows

| IS-04 attribute | From |
|---|---|
| Flow `frame_width`, `frame_height`, `interlace_mode`, `grain_rate`, `components` | `mtl_session_get_config()`: `video.raster`, `video.format` |
| Flow `colorspace`, `transfer_characteristic` (and SDP `colorimetry`, `TCS`, `RANGE`) | `video.colorimetry`, `tcs`, `range`. 0 is the ST 2110-20 default; colorimetry 0 is UNSPECIFIED, and the SDP still carries it, because §7.2 requires the parameter. |
| Flow audio `sample_rate`, `bit_depth`, channels | `audio.*` |
| Sender `transport`, per-leg addresses and ports, `source_port` | `mtl_session_info.leg[]` (granted values) |
| Sender `bit_rate` (BCP-004-02, kbps) | `info.wire_kbps{leg}`, `info.payload_kbps` |
| Node `clocks`: `ref_type`, `gmid`, `locked`, `traceable` | `time.*` stats. With the built-in client MTL knows them; with ptp4l on the node the application supplies them through `mtl_time_set_reference()`. Since a grandmaster can change at runtime, these are gauges, not constants. |
| Node `interfaces`: `chassis_id`, `port_id` (MAC), `name` | `mtl_port_get_spec()`: `mac`, `name`; LLDP neighbour deferred (§6) |
| Device version bump on change | `MTL_EVENT_GRANDMASTER`, `MTL_EVENT_PORT_ADDRESS`, `MTL_EVENT_PACING_CHANGED`, `MTL_EVENT_RTCP_INFO` |

BCP-004-02 forbids producing a stream incompatible with the advertised caps. A Node therefore
sets `caps.pacing_required = 1`, so pacing never falls back silently, or re-publishes on
`MTL_EVENT_PACING_CHANGED` (C-N14).

### 2.2 IS-05: activation

IS-05 PATCHes one resource, with every leg, and that resource's change is all or nothing.
In MTL that resource is one session, and one PATCH is one `mtl_session_update` with every
part it touches, typically `FLOWS | LEGS | REAPPLY` (C-N1). [ex13](10-api-sketch.md) is the
pattern.

**When an activation applies.** The switch happens at the slot boundary by the clock,
whether a unit is there or not. TX switches at the first slot at or after the instant. RX
switches for units whose media time is at or after it; for SENDER or unlocked clocks it
classifies packets by local arrival time instead. So an idle or muted sender still reaches
APPLIED and has an `activation_time` for its 200 response or its `/active` resource. Neighbour resolution starts at
commit and is not awaited: a leg to an address that does not answer waits in
`WAITING_NEIGHBOUR`, and the update still applies. That is what the IS-05 test suite's
activation to 192.0.2.1 expects.

| IS-05 | MTL |
|---|---|
| `activate_immediate` | `when` NULL. The 200 response is sent once `status.update_state` is APPLIED for the call's `update_seq`, or on `MTL_EVENT_UPDATE`, with `activation_time = update_applied_tai_ns`. |
| `activate_scheduled_absolute` / `_relative` | `MTL_AT_TAI` (relative: now from `mtl_time_now` plus the offset). The 202 response carries the returned `planned_tai_ns`: the media time of the boundary it will switch on. A time in the past means now (G-N25). |
| re-activation with identical parameters | `MTL_UPDATE_REAPPLY`. RX re-sends its membership reports and re-arms its rules without a leave, because a leave and join of the same group on both 2022-7 legs at once is a guaranteed hit. TX resolves the neighbour again and rebuilds its headers (C-N5). |
| a new activation while one is scheduled | IS-05 answers 423, and the Node does so without calling MTL. MTL's REPLACED state (a new update replacing a pending one) is for callers that are not NMOS Nodes. |
| `activation.mode = null` cancelling a scheduled one | `mtl_session_update(s, NULL, 0, NULL, NULL)`: 0 cancelled, 1 none pending, or `-MTL_EBUSY` (`UPDATE_COMMITTING`) when it is too late and will apply, which the Node must then report |
| validating staged parameters | `MTL_UPDATE_DRY_RUN` (G-N7): it plans and validates and leaves the status untouched. It reserves nothing, so a later update can still fail with `-MTL_ENOSPC`. |
| `master_enable = false` | every existing leg's bit in `legs_disabled`: the session is **muted** (C-N2, C-N7, Q-NMOS-1). It stays RUNNING; TX units retire at their slots, counted in `tx.units_muted`, not `tx.units_dropped` (which BCP-008 reads as unhealthy); no sender reports; RX leaves its groups. A scheduled disable is an ordinary scheduled update. See the note below. |
| `rtp_enabled = false` on one leg | its bit in `legs_disabled` |
| a receiver created before any connection, or a two-leg receiver given a one-leg SDP | a **reserved leg**: its `legs_disabled` bit set and its address all zero. Enabling it needs an address in the same update (`FLOWS \| LEGS`). The set of existing legs is fixed at create and changes only in STOPPED (G-N4, C-N4). `mtl_sdp_parse` reserves the legs an SDP lacks. |
| a new `interface_ip` (another port) | port changes while RUNNING, made before break: the new queue and rule are reserved first, `-MTL_ENOSPC` if they cannot be, or `-MTL_EBUSY` (`PORT_CHANGE_NEEDS_STOP`) on a backend that cannot (G-N5, C-N3) |
| both legs on one interface | allowed with an explicit `flows[1].port = 1` (port 0). ST 2110-10's rule that source and destination differ still holds (Q-NMOS-3). |
| a new SDP with a different format, at a time | colorimetry, TCS and range can change with the activation, while running, when no conversion uses them. For any other format change, two sessions swap at the instant (N1 §5.4). An RX `MTL_UPDATE_MEDIA` at a unit boundary is later (G-N6). |
| `mediaclk:direct=<offset>` and other per-activation transport values of the new SDP | put `rx.rtp_offset`, `rx.mediaclk`, `rx.link_offset_ns` or `crypto.*` in the update's config options (`mtl_sdp_parse` returns them in `meta.options`). R keys there apply at the same boundary as the flows (C-N10). |

**`master_enable` stays the Node's.** `subscription.active` and BCP-008's Inactive come from
the Node's own `master_enable`, not from `MTL_STATUS_MUTED`, which is only a transport
state. Stop stays for format changes.

Details that make a salvo land together:

- **Audio switches at a packet boundary**, not at the 10 ms unit, so video and audio
  scheduled for one `t` switch within one packet time of each other (Q-NMOS-4).
- **Double bandwidth.** A receiver scheduled far ahead receives old and new groups in the
  meantime. `rx.join_lead_ns` bounds that: the new joins go out at `max(call, t − lead)`
  (C-N13).
- **Counters are never reset** (08 §2.3). For BCP-008's reset on activation, the Node takes
  a baseline on `MTL_EVENT_UPDATE` and subtracts it (C-N8).
- **The UDP source port.** IS-05 `source_port` is one value: a Node sets it, and never uses
  `session.src_port_mode = MULTI`, which NMOS cannot describe (C-N11).
- **A time step.** A pending update across a step of the time base fails with reason
  `TIME_STEP`, and the Node re-schedules it.
- **RTCP transport parameters.** `rtcp.sr` is fixed at create and `rtcp.dst_port` changes
  only in STOPPED, so a Node constrains IS-05's `rtcp_*` parameters to the values in use.

### 2.3 SDP: `mtl_sdp.h`

Every NMOS sender must publish a complete, correct SDP on every activation and on every
grandmaster change, and every receiver must read one. Without a helper each Node writes the
same 300 lines with the same mistakes: a missing `TROFF` when it is not the default, an
unreduced `exactframerate`, or `traceable` claimed without the accuracy check. `mtl_sdp.h` has
two calls:

- **`mtl_sdp_render(s, meta, buf, cap)`** writes what a created session is now:
  - granted values and every enabled leg, ST 2022-7 as two m-lines under `a=group:DUP`
    (ST 2110-10 §8.5);
  - the time reference and colorimetry;
  - under the IPMX profile, the IPMX keyword and `TP=2110TPN`;
  - `a=rtcp`, and the `a=privacy` and `a=extmap` lines of the `crypto.*` options.

  The `o=` version changes on every activation and on every grandmaster or Info Block
  change.
- **`mtl_sdp_parse(sdp, len, &sc, &meta)`** fills a configuration:
  - both legs, whether as two m-lines or as one m-line with two source filters, and reserves
    the legs the SDP does not have;
  - `rx.rtp_offset`, `rx.mediaclk` and `crypto.*` as options in `meta.options`;
  - into `meta`: the IPMX keyword (`MTL_SDP_IPMX`), the sender's ts-refclk, the RTCP port, and
    the remaining fmtp text (profile, level, PAR, DID_SDID). Its strings are copied and
    NUL-terminated.

Both are built on public calls only and allocate nothing. NG3 still holds: MTL itself never
needs SDP. This replaces Q-MODE-6's "values only in v1" (M17).

### 2.4 IS-08, IS-11, BCP-004

- **IS-08 (audio channel mapping):** the Node applies a map at a media index in its own
  code. MTL keeps no channel map state. A remapping helper (`mtl_audio_remap`) is a later
  item, under `MTL_LATER`.
- **IS-11 and BCP-004-01 (sink capabilities, EDID):** these are Node and application logic.
  MTL gives `mtl_session_query()` to check whether a constrained format is possible before
  advertising it.

### 2.5 BCP-008 monitoring

BCP-008-01 (receiver) and -02 (sender) define link, connection or transmission, sync and
stream status. Each needs counters or events MTL already has (the N1 §5.5 mapping), plus
these keys ([08 §R4.5](08-observability.md)):

- `leg.pkts_late{leg}`, `rx.units_late_presentation`: late packets and late units (G-N16);
- `info.wire_kbps{leg}`, `info.payload_kbps`, `info.max_udp_bytes`, `info.troffset_default` (G-N15);
- `anc.did_sdid_seen`: up to 16 DID/SDID pairs (G-N19);
- `time.gm_traceable`, `time.gm_clock_class`, `time.gm_clock_accuracy`, `time.gm_priority1`;
  and `info.ts_refclk.*{leg}`, now gauges (G-N11, C-N9);
- `instance.port_count` (G-N13);
- `tx.units_muted`, so a muted sender does not look unhealthy.

Hysteresis, reporting delay and status messages are Node policy (Q-NMOS-9). MTL gives
counters and events.

### 2.6 NMOS in a pod

The Node's REST and mDNS traffic uses the pod's primary network, and media uses the SR-IOV
VFs. IS-04 unregistration belongs in the container's preStop hook, so the registry removes
the Node before SIGTERM. MTL's own shutdown follows inside what is left of the grace period
([16 §2.2](16-kubernetes-and-crash-safety.md)). Readiness (`MTL_HEALTH_READINESS`) is the
gate for registering. It fails only when the instance as a whole cannot carry media. One
stream in ERROR is `MTL_HEALTH_DEGRADED`, information only, so it does not unregister the
Node.

## 3. IPMX additions

### 3.1 The profile

`session.profile` (and the instance default `instance.profile`) is `ST2110` or `IPMX`. A
profile changes what zero means in a few fields, and the compliance labels. It never
changes behaviour that an explicit field asks for. D-22's zero-default rule stays: a zero
field means the profile's default, and where 0 is also a real value there is a way to ask
for it literally.

| Default | `ST2110` | `IPMX` |
|---|---|---|
| `rtcp.sr`, `rtcp.rx` | off | on |
| video schedule | from `sender_type` (N, NL, W) | N with the TR-10-1 CMAX = max(16, int(Npkts / (21600 · TFRAME))) and the IPMX VRX; SDP `TP=2110TPN` |
| DSCP (`dscp` 0) | CS0 | AF42 for video, ANC and compressed video; AF41 for audio. InfoFrame, FEC and HDCP streams take their associated stream's (TR-10-9 §16). `MTL_FLOWF_DSCP_LITERAL` asks for CS0. |
| `rx.mediaclk` | DIRECT | AUTO (from the reports' Info Block and the grandmaster) |
| `MTL_CVIDEO_VBR_MAX`, a free launch (`MTL_MEDIA_SENDER`) | `MTL_INFO_NON_COMPLIANT` | compliant (TR-10-7, TR-10-1) |
| checks | — | a source filter on every RX leg and in the SDP; DHCP by default on kernel ports (`port.dhcp`) |

### 3.2 RTCP sender reports: `mtl_rtcp.h`

The current `rtcp.*` options are MTL's own NACK retransmission (application packets with PT
204, named "IMTL"). They are renamed `rtx.*` (keys 1000–1004), and the legacy names stay in
the legacy bridge. `rtcp.*` (keys 1010–1013) now means RFC 3550 sender reports (C-I8). Both
use port +1 and are told apart by payload type. A plain RFC 3550 mode, without the Info
Block, is not offered: neither NMOS nor IPMX requires it.

**TX.** With `rtcp.sr`, each leg sends a sender report with the Info Block and SDES CNAME to
its own address at `rtcp.dst_port` (`udp_port + 1`), with the stream's DSCP. The schedule is
TR-10-1's, per essence:

- video and compressed video: one per frame, or per field when interlaced, queued just
  before the unit's first packet;
- ANC and fast metadata: one per new RTP timestamp, before its first packet (TR-10-1 §8.9.2);
- audio: one before the first packet, then every int(10 ms / ptime) packets.

No report goes out while the session is muted.

A control thread prepares the compound packet into a template, and the tasklet only stamps
it: the NTP field is the unit's media time, then the RTP timestamp, packet and octet counts.
That is a copy and a few stores per frame, about 100 ns **[inferred]**. With rate-limit
pacing the report takes one packet slot in the shaped queue. It is not a media packet for
VRX.

**The Info Block.** The library writes its own fields: ts-refclk and mediaclk from the time
state and media mode, unless the application sets them. It also writes the Media Info Block
of the essence from the session config and `struct mtl_rtcp_info` (PAR, measured rate,
channel order):

- video 0x0001;
- PCM 0x0002;
- AES3 0x0004;
- privacy 0x0011, from the `crypto.*` options.

It then appends the application's blocks unread: JPEG XS 0x0008, HDR 0x0006, the
compressed-video blocks. `MTL_RTCP_MIB_APP_ONLY` sends only the application's.

A unit's meta record `MTL_META_RTCP_MIB` appends blocks to that unit's report only, for HDR
metadata that changes per field (TR-10-16), and never changes the block version. The meta
area now holds records back to back, at most one per (kind, tag) (C-I10).
`mtl_rtcp_set_info()` changes the version when the bytes change, and `MTL_EVENT_RTCP_INFO`
then tells the application to re-render SDP and re-publish to NMOS. The SDP stays outside
the library: NG3 holds (C-I11).

**RX.** With `rtcp.rx`, the session receives reports on port +1 (on the system queue by
default, Q-I-8). It keeps the latest one per leg, and maps each unit's RTP onto the sender's
clock: `unit.media_tai_ns` flagged `MTL_UNITF_SENDER_TIME`. `mtl_rtcp_read()` gives the
reports with their Info Block, and `mtl_rtcp_mib_next()` iterates its blocks. A receiver
that drives HDMI out recovers the source's clock from these reports and from the gauge
`rx.sender_rate_ppb` (I-REQ-26).

### 3.3 Wire details

- **The IPMX schedule** is ST 2110-21's N with TR-10-1's CMAX and VRX (§3.1). RACTIVE comes
  from `video.vtotal`, which defaults to the ST 2110-21 value for its rasters and to the
  height otherwise (TR-10-9 §10). `video.htotal` defaults to the width. A separate IPMX
  sender type is not needed (RN-23).
- **Any raster.** Width and height up to 32767, any rate with num ≤ 4194303 and den ≤ 1023.
  The engine replaces its fixed frame-rate table with rationals (GI-7).
- **VBR compressed video:** `cvideo.max_bitrate_bps` is the ceiling, and CMAX comes from it.
  Library packetisation of H.264 and H.265 is later; until then the application uses packet
  units (GI-8).
- **RTP header extensions and CSRCs on RX.** Today the RX parsers read fixed offsets and
  ignore the X and CC bits (verified in I1; SF-68), so a stream with an HDCP or PEP
  extension, encrypted or not, is mis-parsed. The engine fix skips them before the payload
  header. Packet units keep `data` at the RTP header and flag `MTL_PKTE_HDR_EXT` (GI-9).
- **IGMPv2.** `port.igmp_version`: by default v3, falling back to v2 after a v2 querier (RFC
  3376). With V2, SSM is lost and `source_filter` is checked in software (GI-12).
- **InfoFrames before video.** `tx.precede = "<video session name>"` on an ST 2110-41
  session (port +3), set at create. Its unit k shares the video's queue, takes the video
  unit k's RTP timestamp and media time (TR-10-10 §6, §9), and is enqueued just before it, so
  the wire order survives rate-limit shaping. A Node activates both sessions with the same
  `when`; their boundaries coincide on the shared timeline (GI-14).
- **Link offset while active.** `rx.link_offset_ns` changes at the next unit, or at an
  update's boundary, and `MTL_LINK_OFFSET_AUTO` takes the measured minimum. The gauges
  `rx.link_offset_min_ns` and `rx.link_offset_max_ns` give IS-05's constraints (GI-13,
  C-I7).
- **In-band control on the media port** (TR-10-9 §18–19: TCP, mDNS, DHCP). DHCP exists
  (`port.dhcp`). On a DPDK port, the kernel needs `port.virtio_user` for TCP and mDNS, and
  224.0.0.251 forwarded (Q-I-9). In a pod the primary network carries them instead (§2.6).

## 4. Timing without PTP

IPMX has four clocks: the common reference (PTP, when present), each device's internal
clock, the media clock of a stream, and its RTP clock. When there is no grandmaster, the
internal clock runs free. An **async** source, such as an HDMI input at its own rate, has
an RTP clock that follows the source, not the internal clock, and the SDP says
`mediaclk:sender`. The sender reports carry (internal clock, RTP) pairs, so that receivers
can follow it.

Revision 4 had the right base: every time is an instance-clock time, flagged ESTIMATED when
it is not locked. It lacked these pieces, now added:

| Piece | Change |
|---|---|
| a clock that runs free without stepping | `MTL_TIME_SOURCE_FREERUN`: seeded once from the system clock, never stepped, ESTIMATED; its time state is FREERUN, which health counts as ready. `SYSTEM_TAI` follows NTP steps and stays for other uses. |
| staying close to the controller's clock | `time.freerun_slew_ppm`: FREERUN follows `CLOCK_TAI` by frequency only, bounded, never stepping. Without it a 10 ppm crystal drifts about 0.86 s a day **[inferred]**, past the 0.1 s the IS-05 test suite allows for absolute activations. A Node can also convert IS-05 times with `mtl_time_convert`. |
| switching between free run and PTP while running | AUTO is re-evaluated while running (below) |
| async sources | `MTL_MEDIA_SENDER` (below): RTP follows the source; the one exception to D-09's "RTP = floor(M × rate)" (C-I2) |
| inline processors keeping the input's timing (TR-10-1 §9) | `MTL_SUBMIT_SENDER_TIME`: RTP and the report's NTP come from the received unit (`unit.rtp`, `unit.media_tai_ns` with `SENDER_TIME`), and launch is submit + `min_tx_delay_ns` on the instance clock, so the upstream clock never sets a launch time |

**AUTO at runtime.** AUTO moves to a disciplined PHC or `CLOCK_TAI` when one appears. The
step posts `TIME_STATE` and `TIME_STEP`, and the Info Block's ts-refclk changes from
`localmac=` to `ptp=`. When the source is lost, AUTO holds over and then, by
`time.fallback` (default FREERUN), runs free without a step. Sessions on the epoch timeline
follow their step policy (06 §2.4), and pending updates fail with `TIME_STEP` (§2.2).

**SENDER mode.** `unit.media_tai_ns` is the source's own sampling instant, read on the
instance clock and never snapped:

- RTP = RTP0 + floor(k × period × rate), where RTP0 = floor(M0 × rate) at the first unit
  or after a `MTL_SUBMIT_DISCONTINUITY`, which re-anchors it.
- k advances by max(1, round((M − M_prev) / period)) per unit. A missed VSYNC skips one
  period, but drift does not build up, as it would with round((M − M0) / period).
- launch = M + `min_tx_delay_ns` on the nominal-period schedule. A unit that would overlap
  the previous one is `DROPPED/WOULD_OVERLAP`.
- The sender report carries M.
- `MTL_AT_INDEX`, `RX_BY_INDEX` and `tx.precede` targets are `-MTL_EINVAL` on such a
  session, because its indices are not on a grid.

Without this mode, an async source at +50 ppm on a 59.94 session would drop a frame every
5.6 minutes under NEAREST snapping (I1 §5.2). I1 §5.3 works an async HDMI source without PTP
through these rules, with exact numbers.

R5 now says what a time is: ns on the instance clock, TAI while the time base is locked,
`ESTIMATED` when it is not, and `SENDER_TIME` for an RX value on another device's clock
(C-I1).

**Receivers.** Units of one sender align through its clock even without PTP: `mtl_rx_align()`
works on `media_tai_ns`, including `SENDER_TIME` values of one sender. Units of different
senders need a common clock. `rx.mediaclk = AUTO` compares the Info Block's ts-refclk with
the instance's own grandmaster, and uses the reports when the two differ (I-REQ-68).

**Kubernetes.** AUTO's order in a pod ([16 §6](16-kubernetes-and-crash-safety.md)) ends in
FREERUN, so a pod with no PTP is an IPMX sender with `localmac=` and not an error.

## 5. Encryption: `mtl_crypto.h`

IPMX PEP (TR-10-13) encrypts payloads with AES in counter mode and puts the counter in an
RFC 8285 header extension. The library does what has to be on time and per packet:

- it writes and parses the extensions and keeps the counters;
- it sizes packets for the extension;
- it sends the same ciphertext on both ST 2022-7 legs.

Key provisioning stays with the application: pre-shared keys, the KDF, ECDH, a fresh key
generator per boot, and the IS-05 parameters (Q-I-10).

- **Configuration:** options `crypto.*` (keys 1020–1028): scheme, mode, extension IDs,
  clear bytes, base IV, substream, in-place. None is secret. They are R keys, so an IS-05
  activation to another encrypted sender changes them at its boundary without re-creating
  the session, and the SDP helper renders and parses them. The extension-block chain of the
  first draft is gone; `mtl_session_config.next` is reserved again.
- **Where the cipher runs:** never on a tasklet. In the caller at submit and dequeue, which
  become DPC, or on `crypto.workers` threads between submit and pick-up. The time it adds
  moves `min_submit_lead_ns`, and `mtl_session_info` reports it. AES-128-CTR with AES-NI
  costs 0.03–0.10 of a core for 1080p59.94, 0.12–0.50 for 2160p60 **[inferred: a spike
  measures it]**.
- **Memory:** library pools encrypt in place. Attached application memory takes a copy
  unless `crypto.in_place`; MAC modes always copy. `MTL_SESSION_REQUIRE_DIRECT` with a copy
  fails at create.
- **Keys** are never options, because options are readable (C-I9). The one call is
  `mtl_crypto_set_key(s, version, key, bytes, &when)`:
  - TX uses a key from the first unit at or after `when`. RX applies it to units whose media
    time is at or after `when`, the update rule.
  - The counter restarts at 0 only for key bytes this session never used, so re-installing
    a key after a re-activation never repeats an IV and counter pair under one key
    (TR-10-13 §15).
  - An RX packet with an unknown key version counts in `rx.crypto_unknown_key` and posts
    `MTL_EVENT_KEY_NEEDED`.
  - `key` NULL zeroises every key.
  - Keys live in locked, non-dumpable memory, are zeroised on replace and close, and never
    reach logs, stats or captures.

  A TX unit without a key is `DROPPED/NO_KEY`, never sent in clear. An RX packet that fails
  authentication is a lost packet.
- **HDCP** keys are licensed secrets that an open-source library should not hold. HDCP
  streams use packet units, with the vendor's code encrypting and writing the HDCP
  extension. A vendor cipher plugin is later (GI-11, Q-I-4).

## 6. Dispositions

### 6.1 NMOS gaps

| Gap | Disposition | Where |
|---|---|---|
| G-N1 activation instant | adopted, reduced: the planned instant from the update call, the state and the applied instant in the status, `MTL_EVENT_UPDATE`; the switch by the clock, not by the first unit. A separate getter, `first_media_index` and `first_rtp` are dropped. | `mtl.h` |
| G-N2 re-apply | adopted; RX re-sends reports without a leave | `MTL_UPDATE_REAPPLY` |
| G-N3 mute | adopted; muted units are not "dropped" for BCP-008 | `legs_disabled`, `MTL_STATUS_MUTED`, `tx.units_muted` |
| G-N4 reserved legs | adopted, simpler: no `legs_reserved` field; a set `legs_disabled` bit on an unaddressed leg reserves it | `mtl_flow` rule |
| G-N5 port change while running | adopted | `MTL_UPDATE_FLOWS` |
| G-N6 RX format change at an instant | **later**; the A/B swap of N1 §5.4 until then; colorimetry, TCS and range may change while running | — |
| G-N7 dry run | adopted | `MTL_UPDATE_DRY_RUN` |
| G-N8 cancel | adopted as `parts` 0, with three distinct results | `mtl_session_update` |
| G-N9 join lead | adopted | `rx.join_lead_ns` |
| G-N10 colorimetry, TCS, range | adopted; zero is the standard's default, colorimetry always rendered | `mtl.h`, `mtl_format.h` |
| G-N11 time metadata | adopted | `mtl_time_set_reference()`, `time.gm_*` gauges |
| G-N12 grandmaster change | adopted | `MTL_EVENT_GRANDMASTER` |
| G-N13 MAC, port count | adopted | `mtl_port_spec.mac`, `instance.port_count` |
| G-N14 LLDP | **deferred** (Q-NMOS-8): `chassis_id = null` is allowed, and topology tools read the switch | — |
| G-N15 bit rate, sizes | adopted | `info.*` keys |
| G-N16 late counters | adopted | keys |
| G-N17 SDP | adopted as an optional helper (M17) | `mtl_sdp.h` |
| G-N18 offsets at an activation, announce timeout | adopted: R options in the update's config apply at its boundary | `mtl_session_update`, `time.ptp_announce_timeout` |
| G-N19 DID/SDID seen | adopted | key |
| G-N20 RTCP | adopted through GI-1…3, IPMX reports only | `mtl_rtcp.h` |
| G-N21 ST 2022-5 FEC | **not adopted**: the Node omits the FEC set | — |
| G-N22 audio remap | **later** (`MTL_LATER`): the Node remaps in its own code | `mtl_convert.h` |
| G-N23 RX unicast | adopted as a rule | `mtl_flow` comment |
| G-N24 address change | adopted | `MTL_EVENT_PORT_ADDRESS` |
| G-N25 past instant | adopted | `mtl_session_update` comment |

### 6.2 IPMX gaps

| Gap | Disposition | Where |
|---|---|---|
| GI-1 sender reports | adopted, IPMX mode only, scheduled per essence | `rtcp.sr`, `rtcp.cname`, `rtcp.dst_port` |
| GI-2 Info Block | adopted, reduced: the library builds the essence block (no builder function, no `get_info`); per-unit blocks append without a version change | `mtl_rtcp_set_info`, `MTL_META_RTCP_MIB`, `MTL_EVENT_RTCP_INFO` |
| GI-3 RX reports and sender time | adopted; plus `mtl_rtcp_mib_next` to read blocks | `mtl_rtcp_read`, `MTL_WAIT_RTCP`, `MTL_UNITF_SENDER_TIME`, `MTL_MEDIACLK_AUTO`, `rtcp.rx` |
| GI-4 async media mode | adopted, with the k rule and inline processors | `MTL_MEDIA_SENDER`, `MTL_SUBMIT_SENDER_TIME`, `MTL_INFO_MEDIACLK_SENDER` |
| GI-5 free run, AUTO at runtime | adopted, with a bounded slew | `MTL_TIME_SOURCE_FREERUN`, `time.fallback`, `time.freerun_slew_ppm` |
| GI-6 IPMX sender type | **replaced**: the profile gives N the TR-10-1 CMAX and VRX; `vtotal` and `htotal` are options | `session.profile`, `video.vtotal`, `video.htotal` |
| GI-7 any raster | adopted (contract and engine fix) | `mtl_raster` comment |
| GI-8 VBR | adopted as an option; H.26x packetisation **later** | `cvideo.max_bitrate_bps` |
| GI-9 header extensions on RX | adopted (engine fix, SF-68) | `MTL_PKTE_HDR_EXT` |
| GI-10 PEP | adopted, parameters as options | `mtl_crypto.h`, `crypto.*`, reason `NO_KEY`, `MTL_EVENT_KEY_NEEDED` |
| GI-11 HDCP cipher plugin | **later**; packet units until then | — |
| GI-12 IGMPv2 | adopted, merged with K-REQ-20 | `port.igmp_version` |
| GI-13 link offset while active | adopted | `rx.link_offset_ns` R, `MTL_LINK_OFFSET_AUTO` |
| GI-14 InfoFrames first | adopted: shared queue, the video's RTP and media time | `tx.precede` |
| GI-15 FEC Profile A | **later** | — |
| GI-16 profile | adopted | `session.profile`, `instance.profile` |
| GI-17 keys | adopted | `tx.f2f_pp_ns`, `rx.sender_rate_ppb`, `rx.rtcp_info_version`, `rx.crypto_auth_fail` |

Requirements outside MTL: I-REQ-4 and -5 (BMCA and a PTP leader) need ptp4l in v1 (Q-I-7);
I-REQ-59 and -62 (key derivation, HKEP) belong to the application; I-REQ-75 (USB over IP) is
out of scope.

### 6.3 Conflicts

Every conflict of N1 §6 and I1 §7 is resolved by an adopted change above, except these four:

| Conflict | Resolution |
|---|---|
| C-I3 the TX grid snaps TAI units NEAREST | SENDER mode never snaps; NEAREST stays the default for sync sources |
| C-I4 compliance labels | judged against `session.profile` |
| C-I12 RX parses fixed offsets | GI-9 engine fix |
| C-I14 an HDMI source changing format mid-stream | stop, update, start, then `mtl_rtcp_set_info()`; the short gap is accepted, and G-N6 later removes it for RX |

Earlier documents that this supersedes are marked: 09 §7.1–7.2 (legs, re-staging, the
update status), 09 §9.3 (required addresses, except for reserved legs) and 07 §3.2 (events
23–30).

## 7. Open questions

M17 collects them in [OPEN-QUESTIONS.md](OPEN-QUESTIONS.md). Everything else in the studies'
question lists (Q-NMOS-1…11, Q-I-1…12) has the study's recommendation, which this document
adopts.

| ID | Question | Recommendation |
|---|---|---|
| Q-NI-1 | Ship `mtl_sdp.h` (render and parse) in v1, replacing Q-MODE-6's "values only"? | yes |
| Q-NI-2 | Ship `mtl_rtcp.h` in v1? IPMX cannot be met without sender reports | yes, phase 3 with the timing core |
| Q-NI-3 | Ship `mtl_crypto.h` in v1, or after a cost spike? | header in v1 as a proposal; implementation after the spike |
| Q-NI-4 | Rename the NACK options `rtcp.*` to `rtx.*`? | yes; the legacy names stay in the bridge |
| Q-NI-5 | Mute (every leg disabled) instead of a scheduled stop? | yes (Q-NMOS-1) |
| Q-NI-6 | Allow both legs on one port? | yes, with an explicit `flows[1].port` (Q-NMOS-3) |
| Q-NI-7 | Audio updates at a packet boundary? | yes (Q-NMOS-4) |
| Q-NI-8 | Profiles change zero defaults and labels only: accept `session.profile` as the one IPMX switch? | yes |
| Q-NI-9 | No separate IPMX sender type: the profile gives N TR-10-1's CMAX and VRX? | yes, unless the VSF requires a distinct TP value |
| Q-NI-10 | RX REAPPLY without a leave (reports re-sent) instead of a leave and join? | yes: a leave and join of both legs at once is a guaranteed hit |
