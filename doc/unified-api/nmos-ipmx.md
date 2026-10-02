# NMOS and IPMX

| | |
|---|---|
| Status | Design kept for a later phase. **Phase 7, later** (D-98), except the atomic `mtl_session_update` with its planned instant and per-leg enable, which are **Phase 2**. Awaiting maintainer decision M17 ([decisions.md](decisions.md)) |
| Date | 2026-10-02 |
| Folded from | the archived note 17 (NMOS and IPMX, primary); the interop studies N1 (NMOS requirements) and I1 (IPMX requirements), whose requirement rows are now [requirements.md](requirements.md) §6, §7; the reviews RN, RKNA and RV (now [history.md](history.md) §6.9–§6.11) |
| Headers | `mtl.h` (update contract, legs, status), [mtl_sdp.h](sketch/include/mtl/experimental/mtl_sdp.h), [mtl_rtcp.h](sketch/include/mtl/experimental/mtl_rtcp.h), [mtl_crypto.h](sketch/include/mtl/experimental/mtl_crypto.h), `mtl_options.h` (113, 201–213, 1000–1039, 2040), `mtl_sync.h`, `mtl_format.h`, `mtl_queue.h` (events 26–30), `mtl_packet.h` |

NMOS (AMWA IS-04, IS-05 and the BCPs) is how an ST 2110 device is found, connected and monitored.
IPMX (VSF TR-10) is a profile on top of ST 2110 and NMOS for ProAV. It works without PTP, carries
async sources, adds RTCP sender reports with an Info Block, and adds optional encryption. This
document says what MTL does for both, in which header and phase. The design is in the headers now,
so the API does not change when the code comes.

## 1. What is built when

| Item | Phase | Notes |
|---|---|---|
| atomic `mtl_session_update` (today's `update_destination` / `update_source`), `planned_tai_ns`, `status.update_*`, `MTL_EVENT_UPDATE`, the switch at the slot boundary by the clock, a past instant means now | **2** | it replaces an existing call; the contract costs nothing extra once the update is atomic |
| per-leg enable and disable (`legs_disabled`, `MTL_UPDATE_LEGS`) | **2** | today's redundancy legs |
| the rest of the NMOS contract: `MTL_UPDATE_REAPPLY`, `MTL_UPDATE_DRY_RUN`, cancel, mute (every leg disabled), reserved legs, port changes while running, R options at the boundary, `rx.join_lead_ns` | 7 | an NMOS Node can be built on Phase 2 with stop, update and start (§4.5) |
| `mtl_time_set_reference()` and `time.phc_trust` | **1–2** | the pod time rules need them: the application tells MTL the node lost its grandmaster ([deployment.md](deployment.md)) |
| the other IS-04 values: colorimetry fields, the port MAC, the `info.*`, `time.gm_*` and BCP-008 keys; the IS-04 use of events 27 (`MTL_EVENT_GRANDMASTER`, posted from Phases 1–2 with `mtl_time_set_reference`) and 28 (`MTL_EVENT_PORT_ADDRESS`, posted with `port.dhcp`, a ported feature) | 7 | declared outside `MTL_LATER` already; not scheduled before Phase 7 unless pulled forward |
| `mtl_sdp.h` | 7 | first in Phase 7, with the NMOS contract |
| IPMX timing: `MTL_TIME_SOURCE_FREERUN`, AUTO at runtime, `MTL_MEDIA_SENDER`, `MTL_SUBMIT_SENDER_TIME`; `mtl_rtcp.h` | 7 | second |
| IPMX profile and wire: `session.profile`, IGMPv2, `tx.precede`, `video.vtotal`/`htotal`, `cvideo.max_bitrate_bps`, RX header extensions (SF-68) | 7 | third |
| `mtl_crypto.h` (PEP) | 7 | last, after its cost spike |

Phase 7 starts after the ABI freeze (Phase 6 exit). The six functions of `mtl_sdp.h`, `mtl_rtcp.h`
and `mtl_crypto.h` are 6 of the 14 under `MTL_LATER`; their keys, enums, events and reserved fields
are declared, so the freeze does not block them. Effort **[estimate]**: NMOS contract and SDP 2–3 EM,
IPMX timing and RTCP 4–6 EM, PEP 2–4 EM, outside the plan's total ([implementation-plan.md](implementation-plan.md)).

## 2. The split between the Node and MTL

| The Node (the application, or a library such as nmos-cpp) | MTL |
|---|---|
| registry, mDNS, REST and WebSocket APIs, JSON, UUIDs | the granted values: ports, SSRC, MACs, TP, TROFF, CMAX, bit rate |
| staged and active parameters, `master_enable` | the instant an activation took effect, and the boundary it will use |
| policy: which activation to accept, 423 while one is scheduled, when to report a status change | the time reference and its metadata |
| resolving `auto` transport parameters | the wire format, including sender reports and encryption |
| BCP-008 hysteresis, reporting delay, messages, counter baselines | counters and events |
| key derivation (PSK, KDF, ECDH), HDCP, HKEP | per-packet cipher, extensions, counters |

One Node is one MTL instance; the NMOS APIs use a management interface, not an MTL port. Each
Sender or Receiver is one session named by its UUID (`sc.name`, 64 B, kept across
`MTL_UPDATE_MEDIA`), created at boot with every leg reserved and disabled, started, and so muted
(Phase 7). Activations are then boundary updates: nothing is allocated, and scheduled timing is exact.
Node `clocks[0]` is the one instance timescale: each leg's PHC is compensated into it, and the
per-port `time.*` keys feed sync health. Node `interfaces[i]` is MTL port i (`mtl_port_get_spec()`,
`mac`).

### 2.1 Which specifications need MTL

| Specification | What it needs from MTL |
|---|---|
| IS-04 Discovery and Registration 1.3 | Flow, Sender, Receiver and Node attributes only the transport knows; change notifications to bump `version` (§3) |
| IS-05 Connection Management 1.1 | per-leg RTP transport parameters, `master_enable`, immediate and scheduled activation, `activation_time`, SDP (§4, §5) |
| IS-08 Audio Channel Mapping | units per media index; the map is the Node's (§6) |
| IS-09 System Parameters | `global.json` requires `ptp.domain_number` (0–127) and `ptp.announce_receipt_timeout` (2–10), read at Node start: `time.ptp_domain` and `time.ptp_announce_timeout` (2209) at start for the built-in client; with ptp4l (`MTL_TIME_SOURCE_PHC`) both are ptp4l's configuration |
| IS-11 Stream Compatibility | reconfigure (`MTL_UPDATE_MEDIA` in STOPPED), mute, detect what a receiver gets (`MTL_DETECT_ON`, `MTL_EVENT_RX_FORMAT`) |
| IS-12 / MS-05-02 monitoring | the device model is the Node's: `NcStatusMonitor` (`overallStatus`, `statusReportingDelay`, default 3 s), `NcReceiverMonitor` and `NcSenderMonitor` (Control Feature Sets `monitoring/models`) are Node objects whose values come from MTL through BCP-008 (§7); the delay is Node policy |
| BCP-004-01 / -02 capabilities | `mtl_session_query` for "can MTL receive or send X"; never a stream outside the advertised caps |
| BCP-006-01 JPEG XS | profile, level, sublevel, colour, bit rate with and without RTP overhead, packetisation mode, TP (§3) |
| BCP-008-01 / -02 status monitoring | link, connection, synchronisation and stream status; lost, late and error counters (§7) |
| IS-07, IS-10 and BCP-003-01, IS-13, BCP-002, BCP-005-01, BCP-007-01 | nothing: control-plane, security, labels, grouping, EDID and NDI are outside the transport |

IPMX (VSF TR-10) makes NMOS mandatory (TR-10-8), so every row above is also an IPMX requirement.
The parts that ask something of the transport:

| TR-10 part | Edition (VSF list, 2026-10-01) | Needs from the transport |
|---|---|---|
| -0 Overview | 2026-07-07 Final, informative | the Media Info Block (MIB) registry (§5): 0x0001 video, 0x0002 PCM, 0x0003 CBR compressed, 0x0004 AES3, 0x0005 VBR compressed, 0x0006 HDR, 0x0008 JPEG XS, 0x0009 H.265, 0x000A H.264, 0x0010 HKEP, 0x0011 PEP |
| -1 System Timing | 2024-02-23 Final | with and without PTP, free-running internal clock, BMCA, IPMX CMAX and VRX, async and sync sources, sender reports with the Info Block on port + 1, inline processors, receiver timing and link offset |
| -2 Uncompressed Video | 2024-02-23 Final | ST 2110-20 subset; receivers take YCbCr 4:2:2/10 and RGB 4:4:4/8. MIB 0x0001: sampling, depth, packing, interlace, PAR, range, colorimetry, TCS, size, rate (22/10-bit rational), measured pixel clock, htotal, vtotal |
| -3 PCM, -4 ANC, -12 AES3 | -3 2024-02-23 Final; -4 2023-04-14 Draft; -12 2023-08-30 Draft | ST 2110-30/-40/-31 subsets. Audio: 48 kHz L16/L24 MUST, 44.1 kHz (L16) and 96 kHz (L24) SHOULD. ANC reports carry the Info Block, no MIB |
| -5 HDCP, -13 PEP | 2026-02-17 v2 Final (both) | an IV-counter RTP header extension in every packet, the payload encrypted, RTP header and payload header in clear; the HDCP extension comes first; ANC is not encrypted. PEP keys are derived from pre-shared keys plus parameters in SDP and IS-05 (TR-10-13 §3, §13; BCP-005-03), not exchanged through IS-10 |
| -6 FEC | 2023-08-07 Draft | optional ST 2022-5 Profile A on ports + 2 and + 4 (not adopted): column FEC L = 2, D = 16 above 32 packets/ms, 1 × 1 below, partial matrices at frame end and on timeout |
| -7 VBR compressed, -11 CBR compressed | -7 2024-11-22 Draft; -11 2024-02-23 Final | ST 2110-22 payload; CMAX from the maximum rate; no VRX for VBR; MIB 0x0003 has the 0x0001 layout |
| -15 codecs | Part 1 (JPEG XS) 2026-09-22 Final; Parts 2, 3 (H.265, H.264) 2026-06-04 Draft | JPEG XS High444.12, sublevels above 4 bpp allowed, MIB 0x0008 (T, P, Ppih, Plev) after 0x0003; H.265 and H.264: BCP-006-03 / -02 limits, no PACI, `TP=2110TPW` (no gapped mode), HRD and SEI rules |
| -8 NMOS | 2026-01-06 Final | the table above, plus `ext_link_offset_delay` |
| -9 Device Behaviour | 2025-05-13 v2 Draft (the PQCR baseline) | sender report + SDES CNAME; frame interval ≤ 2 ms peak to peak over 2 s; DHCP, DSCP AF42/AF41/EF, IGMPv3 SSM and IGMPv2, in-band control |
| -10 InfoFrames | 2024-10-07 Draft | ST 2110-41 on port + 3 with the video's RTP, sent before the video's first packet |
| -14 USB, -16 HDR | -14 2026-04-07 Draft; -16 2025-11-18 Draft | USB is outside MTL (-14 also uses PEP's CMAC-AAD modes on TCP messages); MIB 0x0006 follows the colorimetry MIB, may change per field, and its N bit says "for the next field" |

The parts are -4 ANC, -9 device behaviour, -10 InfoFrames and -14 USB (earlier notes named them
otherwise). Compliance is AIMS PQCR v1.1 (August 2026): baseline TR-10-1, -8 and -9, with HDCP, InfoFrames, PEP, USB and HDR
optional and tested when declared; SDP, NMOS and RTCP must describe the same stream. The test plan
is TR-10 TP-1: §13.3 checks the sender reports (port, DSCP, RTP/NTP pairing, Info Block, schedule,
order) and §13.5 checks CMAX, VRX and the 2 ms frame interval. The AIMS profiles: uncompressed
(senders RGB 8-bit 4:4:4 or YCbCr 10-bit 4:2:2, receivers both; resolution and frame rate "Undefined /
Any"), PCM, JPEG XS, HEVC. Specification URLs: [prior-art.md](prior-art.md) §9.

## 3. IS-04: values only the transport knows

| IS-04 attribute | From |
|---|---|
| Flow `media_type` (registered: `video/raw`, `video/jxsv`, `video/H264`, `audio/L24`, `audio/L16`, `video/smpte291`) | `essence` and the format |
| Flow `frame_width`, `frame_height`, `interlace_mode`, `grain_rate` | `mtl_session_get_config()`: `video.raster` (rate, size, scan) |
| Flow (video/raw) `components[]` (name, width, height, bit_depth; required by `flow_video_raw.json`) | `video.format` and the raster: `mtl_format_names().sdp`, `mtl_format_pgroup()` |
| Flow (coded) `profile`, `level`, `sublevel`, `bit_rate` (BCP-006-01 MUST) | profile, level and sublevel from the application or the codec plugin; codestream rate from the granted `codestream_bytes` (`mtl_session_get_info()`) × rate, or `info.payload_kbps` |
| Flow (data) `DID_SDID[]` (optional) | the application's on TX; RX: the observed set `anc.did_sdid_seen` |
| Flow `colorspace`, `transfer_characteristic`; SDP `colorimetry`, `TCS`, `RANGE` | `video.colorimetry`, `tcs`, `range` (also in `mtl_cvideo_config`). 0 is UNSPECIFIED, SDR, NARROW. UNSPECIFIED is still rendered, because ST 2110-20 §7.2 requires the parameter. FULLPROTECT with BT2100 is `-MTL_EINVAL` |
| Flow audio `sample_rate`, `bit_depth`, channels | `audio.*` |
| Sender `transport`, per-leg addresses and ports, `source_port` | `mtl_session_info.leg[]` (granted values) |
| Sender and Receiver `interface_bindings` (one per leg; the same interface twice when both legs use it) | the Node's name of port `info.leg[i].port` |
| Sender and Receiver `subscription.active` (MUST be true exactly when configured to send or receive) | the Node's `master_enable` (§4.2) |
| Sender `manifest_href` (SHOULD serve the SDP; MAY be 404 while inactive) | `mtl_sdp_render()` (§5); the Sender `version` MUST change when the SDP does |
| Sender `bit_rate` (BCP-004-02, kbps rounded up) | `info.wire_kbps{leg}`, `info.payload_kbps` |
| Node `clocks[]`: `ptp` {`name`, `ref_type`, `traceable`, `version` = `IEEE1588-2008`, `gmid`, `locked`}, all required (`clock_ptp.json`), or `internal` | `time.*` stats: gauges, since a grandmaster can change. The built-in PTP client fills them |
| the same with ptp4l (PHC, CLOCK_TAI and USER sources) | the application calls `mtl_time_set_reference()`: `struct mtl_time_reference` (40 B) with `gmid[8]`, `domain`, `traceable`, `clock_class`, `clock_accuracy`, `locked`. With `time.phc_trust` detect, `locked` tells MTL the node lost its grandmaster |
| Node `interfaces[]`: `name`, `port_id` (MUST be a MAC), `chassis_id`, `attached_network_device`; `node.json` asks for ARP at a minimum, "and ideally LLDP" | `mtl_port_get_spec()`: `mac` (output only, ignored on input), `name`; `instance.port_count`; built-in ARP. LLDP is deferred: `chassis_id = null` is allowed |
| BCP-006-01 (ST 2110-22 JPEG XS) Sender `bit_rate`, `packet_transmission_mode`, `st2110_21_sender_type` | `info.wire_kbps` (RTP overhead included), the ST 2110-22 packing, `cvideo.sender_type` |
| every resource's `version`: a TAI `<s>:<ns>` updated on every change; IS-05: incremented on every activation, also of the same parameters | `mtl_time_now()`; the bump of a Device or Sender on `MTL_EVENT_GRANDMASTER`, `MTL_EVENT_PORT_ADDRESS`, `MTL_EVENT_PACING_CHANGED`, `MTL_EVENT_RTCP_INFO`, `MTL_EVENT_UPDATE` |

BCP-004-02 forbids a stream that does not match the advertised caps: a Node sets
`caps.pacing_required = 1`, or re-publishes on `MTL_EVENT_PACING_CHANGED` (C-N14). IS-11 and
BCP-004-01 constraint sets use `mtl_session_query(MTL_QUERY_CHECK_CAPACITY)` before a format is
advertised. A BCP-004-01 receiver lists `constraint_sets` (any one satisfied means compatible) over
the Capabilities register: `media_type`, `grain_rate`, `frame_width`, `frame_height`,
`interlace_mode`, `colorspace`, `transfer_characteristic`, `color_sampling`, `component_depth`,
`channel_count`, `sample_rate`, `sample_depth`, `bit_rate`, `profile`, `level`, `sublevel`,
`event_type`, and the transport caps `packet_time`, `max_packet_time`, `st2110_21_sender_type`,
`packet_transmission_mode`, `bit_rate`, `hkep`, `privacy`, `usb_class`. It MUST bump its IS-04 `caps.version`
when its capabilities change, capacity included. The transport's answer is one dry run per candidate
set; no enumeration call is needed, since the formats are a closed enum that `mtl_format_names()`
iterates.

BCP-006-01 (JPEG XS): the Flow MUST carry `video/jxsv`, `components`, `profile`, `level`, `sublevel`
and the codestream `bit_rate`; the Sender MUST carry `bit_rate` "including the RTP transport
overhead", `packet_transmission_mode` for slice mode, and `st2110_21_sender_type` if ST 2110-22
compliant; the SDP follows RFC 9134 (§5).

## 4. IS-05: the activation contract

### 4.1 One PATCH is one update

IS-05 PATCHes one resource (one session), every leg, all or nothing: one PATCH is one
`mtl_session_update(s, sc, parts, when, &planned_tai_ns)` (CP), typically with `MTL_UPDATE_FLOWS |
MTL_UPDATE_LEGS | MTL_UPDATE_REAPPLY` (C-N1). [ex13](sketch/examples/ex13_nmos_switch.c) reads the
active configuration, copies only what IS-05 changed, updates, and reads `status.update_seq` to
match the later event. The `mtl.h` comment is normative:

- **All or nothing.** Resources are reserved before commit. Fields outside `parts` must equal the
  active configuration (`-MTL_EINVAL` naming the first that differs). `direction`, `essence` and
  `unit` never change.
- **Options in the update.** Keys in `sc->options` that may change while running (R) apply at the
  same boundary. Any other key there must equal its current value (`-MTL_EBUSY`, `OPTION_STATE`).
  `mtl_session_get_config()` returns no options, so the Node passes them.
- **Neighbours are not awaited.** Resolution starts at commit; a leg without one waits in
  `MTL_FLOW_WAITING_NEIGHBOUR` and the update still applies (the IS-05 suite activates to 192.0.2.1).
- **The switch is by the clock.** It happens at the slot boundary whether a unit is there or not. TX
  switches at the first slot at or after `when`. RX switches for units whose media time is at or
  after it. For SENDER or unlocked clocks RX classifies packets by local arrival time. Audio
  switches at a packet boundary, so a salvo lands within one packet time. An idle or muted session
  still reaches APPLIED.
- **Times.** NULL `when` is now. An `AT_TAI` or `AT_INDEX` already past means now (G-N25). In ARMED,
  a `when` before T0 means T0. In CREATED and STOPPED the update applies during the call, and
  planned = applied = now.
- **Result.** The call returns once posted; `planned_tai_ns` gets the media time of the boundary.
  `status.update_state`, `update_reason`, `update_seq`, `update_applied_tai_ns` (INT64_MIN until
  then) and `MTL_EVENT_UPDATE` (new = state, value[0] = applied TAI, value[1] = seq) report it.
- **States.** `MTL_UPDATE_STATE_` NONE, PENDING, APPLIED, FAILED (the old configuration stays),
  REPLACED, CANCELLED. `update_seq` counts posted updates, not dry runs or cancels; the Node
  serialises the updates of one session and reads the seq after the call. A pending update fails
  with `TIME_STEP` if the time base steps, and the Node re-schedules it.

What IS-05 itself requires, and how the contract meets it:

- A PATCH with no activation, or one cancelling a scheduled activation, returns 200.
  `activate_immediate` returns 200 "only once the new transport parameters have been applied", with
  `activation_time` the actual TAI instant; a scheduled one returns 202 with the instant it "will
  actually transition", which may differ (frame boundary, end of GOP): `planned_tai_ns`.
- "On activation all instances of `auto` must be resolved ... If ... `auto` cannot be resolved, the
  active transport parameters must not change": the all-or-nothing rule.
- Scheduled activations exist to synchronise salvos and are "not intended ... for scheduling
  activations far in the future". On an error between scheduling and activation, `/active` SHOULD
  reflect reality (`master_enable = false` if the sender stopped).
- Packet loss MUST NOT be reported through `/active`: that is BCP-008's job (§7).
- ST 2022-7: a request carries as many legs as the constraints, an unchanged leg as `{}`. A two-leg
  receiver given a one-leg SDP SHOULD set leg 2 `rtp_enabled = false`; a one-leg receiver given a
  2022-7 SDP SHOULD join the first leg. The first SDP stream is path 1 (leg 0). Of the RFC 7104 forms
  (separate destinations: two m-lines, `a=group:DUP`; separate sources: one m-line, two sources,
  `a=ssrc-group:DUP`; temporal redundancy: same source and destination, `duplication-delay`),
  ST 2110-10 §8.5 forbids the third, so a Node constrains it out.
- A receiver PATCH with an SDP SHOULD use its media information; where the SDP and the
  `transport_params` of the same PATCH disagree, the parameters win.
- Constraints are per leg; `auto` must be supported where the schema allows it, and senders and
  receivers SHOULD offer an enum of interface addresses. Constraints MTL implies: interfaces are
  ports (one IPv4 address each, IPv6 reserved), legs on distinct ports by default, no FEC, RTCP only
  under IPMX.

### 4.2 IS-05 to MTL

Endpoints MUST support the core parameter set and MAY support the multicast, FEC and RTCP sets, each
all or nothing. Sender core: `source_ip`, `destination_ip`, `source_port`, `destination_port`,
`rtp_enabled`. Receiver core: `source_ip`, `interface_ip`, `destination_port`, `rtp_enabled`, plus
`multicast_ip` if it can do multicast. There is one parameter object per leg (two with ST 2022-7).

| IS-05, per leg | `auto` in the schema | MTL |
|---|---|---|
| TX `source_ip` | the sender picks its interface | `flows[i].port`; a port owns one address (`mtl_port_get_spec()`, `sip`) |
| TX `destination_ip` | the sender picks a group (MADCAP, ZMAAP, ...) | `flows[i].ip`; allocating it is the Node's job |
| TX `source_port` | 5004 | `flows[i].udp_src_port`; granted in `info.leg[i].udp_src_port` |
| TX and RX `destination_port` | 5004 | `flows[i].udp_port` |
| RX `multicast_ip` (null = unicast) | — | `flows[i].ip` = the group; unicast: the `mtl_flow` rule (G-N23) |
| RX `source_ip` (unicast source or SSM filter; null = any) | — | `flows[i].source_filter` |
| RX `interface_ip` | multicast: the receiver chooses; unicast: the controller supplies it | `flows[i].port`; the Node maps the address to a port by `sip` (`mtl_port_find()` looks up by name) |
| `fec_*`, `rtcp_*` | — | FEC omitted (G-N21). `rtcp_*` only under IPMX, constrained to the fixed ports (+1 reports, +2 and +4 FEC, +3 InfoFrames) and the values in use (§4.4); only `rtcp_destination_port` maps, to `rtcp.dst_port` (Q-I-11) |
| `ext_*` | — | `ext_link_offset_delay`, `ext_privacy_*`, `ext_infoframe_enabled` (rows below) |

| IS-05 | MTL | Phase |
|---|---|---|
| `activate_immediate` | `when` NULL. Answer 200 once `status.update_state` is APPLIED for the call's `update_seq`, or on `MTL_EVENT_UPDATE`, with `activation_time = update_applied_tai_ns` | 2 |
| `activate_scheduled_absolute` / `_relative` | `MTL_AT_TAI` (relative: `mtl_time_now()` plus the offset). The 202 carries the returned `planned_tai_ns`. A time in the past means now | 2 |
| `rtp_enabled = false` on one leg | its bit in `legs_disabled`, with `MTL_UPDATE_LEGS` | 2 |
| re-activation with identical parameters | `MTL_UPDATE_REAPPLY`. RX re-sends its membership reports and re-arms its rules **without a leave**: a leave and join of one group on both ST 2022-7 legs at once is a certain hit. TX resolves the neighbour again and rebuilds its headers (C-N5) | 7 |
| a new activation while one is scheduled | IS-05 answers 423; the Node does so without calling MTL. MTL's REPLACED is for callers that are not NMOS Nodes | Node |
| `activation.mode = null` cancelling a scheduled one | `mtl_session_update(s, NULL, 0, NULL, NULL)`: 0 cancelled, 1 none pending, `-MTL_EBUSY` (`UPDATE_COMMITTING`) when it will still apply, which the Node must then report | 7 |
| validating staged parameters | `MTL_UPDATE_DRY_RUN`: validates and plans (`planned_tai_ns` filled), reserves and posts nothing, leaves the status untouched. A later update can still fail with `-MTL_ENOSPC` | 7 |
| `master_enable = false` | every existing leg's bit in `legs_disabled`: **muted**. The session stays RUNNING. TX units retire at their slots, counted in `tx.units_muted`, not `tx.units_dropped` (which BCP-008 reads as unhealthy). No sender reports. RX leaves its groups. `MTL_STATUS_MUTED` is set. A scheduled disable is an ordinary scheduled update | 7 |
| a receiver created before any connection; a two-leg receiver given a one-leg SDP | a **reserved leg**: its `legs_disabled` bit set and its flow all zero. Enabling it needs an address in the same update (`FLOWS \| LEGS`). The set of existing legs is fixed at create and changes only in STOPPED (G-N4, C-N4). `mtl_sdp_parse()` reserves the legs an SDP lacks | 7 |
| IPMX `ext_link_offset_delay` (µs, or `auto`) | `rx.link_offset_ns` = value × 1000 in the update's options; `auto` = `MTL_LINK_OFFSET_AUTO`; the min/max constraints from `rx.link_offset_min_ns` / `_max_ns`, reported only while active; the value reads 0 while inactive | 7 |
| IPMX `ext_privacy_*` | `crypto.*` options in the update; the key through `mtl_crypto_set_key()` before `when`. The values are fixed at an activation with `master_enable` true. A receiver with an unknown `key_id` fails the activation (BCP-005-03) | 7 |
| IPMX `ext_infoframe_enabled` | the legs of the `tx.precede` session (the ST 2110-41 InfoFrame stream, port + 3) | 7 |
| IPMX `hkep` Sender attribute (BCP-005-02); FEC on ports + 2 and + 4 | outside MTL (HKEP over TCP); FEC is not adopted | — |
| a new `interface_ip` (another port) | port change while RUNNING, made before break: the new queue and rule are reserved first; `-MTL_ENOSPC` if they cannot be, `-MTL_EBUSY` (`PORT_CHANGE_NEEDS_STOP`) on a backend that cannot (G-N5, C-N3). On such a backend the Node advertises one `interface_ip` per leg, so a controller never asks for it **[inferred]** | 7 |
| both legs on one interface | allowed with an explicit `flows[1].port = 1` (port index 0 + 1). ST 2110-10's rule that source and destination differ still holds | 7 |
| a new SDP with colorimetry, TCS or range changed | `MTL_UPDATE_MEDIA` while running, at the same `when`, when no conversion uses them | 7 |
| a new SDP with any other format change, at a time | two sessions swap at the instant (§4.5). An RX `MTL_UPDATE_MEDIA` at a unit boundary is later (G-N6) | later |
| `mediaclk:direct=<offset>` and other per-activation values of the new SDP | `rx.rtp_offset`, `rx.mediaclk`, `rx.link_offset_ns` or `crypto.*` in the update's options (`mtl_sdp_parse()` returns them in `meta.options`). They apply at the flows' boundary (C-N10) | 7 |

**`master_enable` stays the Node's.** IS-04 `subscription.active` and BCP-008's Inactive come from
the Node's own `master_enable`, not from `MTL_STATUS_MUTED`, which is only a transport state. Stop
stays for format changes.

### 4.3 Answering the controller

The PATCH handler, on parameters already merged into the staged set:

1. `mtl_session_get_config()`; if a transport file came, `mtl_sdp_parse()` it (the parameters win
   over the SDP). Per leg, resolve `auto` (Node policy) into `flows[i]` and set bit i of
   `legs_disabled` to !(`master_enable` && `rtp_enabled[i]`). Pass the R options again:
   `mtl_session_get_config()` returns none.
2. If the media differs, take the format-change path (§4.5); else `parts = MTL_UPDATE_FLOWS |
   MTL_UPDATE_LEGS`.
3. No activation (staging only): 200, after an optional `MTL_UPDATE_DRY_RUN` whose failure answers
   400. A null activation cancelling a scheduled one: `mtl_session_update(s, NULL, 0, NULL, NULL)`,
   then 200; `-MTL_EBUSY` means it will still apply, which the Node reports.
4. `when`: NULL (immediate), `{.kind = MTL_AT_TAI, .value = tai}` (absolute), or `mtl_time_now()`
   plus the offset (relative). `mtl_session_update(s, &sc, parts | MTL_UPDATE_REAPPLY, when,
   &planned)`; a failure answers 500 with `/active` unchanged. Read `status.update_seq`.
5. Scheduled: answer 202 with `planned`. Immediate: wait for `MTL_EVENT_UPDATE` of that seq (or poll
   the status; timeout 1 s), then answer 200 with `update_applied_tai_ns`. An immediate activation
   applies within one unit period plus the command acknowledgement (`instance.cmd_ack_timeout_ns`,
   100 ms).
6. On APPLIED (both cases): commit `/active` with the granted values (`info.leg[i].udp_src_port`,
   the port's `sip`, the group), regenerate the SDP, bump the IS-04 `version`, take the BCP-008
   baseline. On FAILED `/active` keeps the old values; if the session went to ERROR,
   `active.master_enable = false`.

**Timing.** TX `planned_tai_ns` is t rounded up to the next frame or field boundary (≤ 16.7 ms at
59.94p), and every leg switches on the same unit; RX switches by the media time derived from RTP,
exact to the unit. IS-05 sets no accuracy figure; a controller compares `activation_time` across
devices, which is why the boundary actually used is reported. Joins and ARP for the new flows start
at the call, so a Node calls `mtl_session_update()` as soon as the PATCH arrives, and bounds the
double bandwidth with `rx.join_lead_ns` (§4.4).

**How the AMWA suite checks it.** The nmos-testing suite (`IS05Utils._check_perform_activation`,
`Config.py`) reads `/active` once (`maxTries = 1`) right after an immediate activation, and up to
three times, `API_PROCESSING_TIMEOUT` (1 s) apart, after a scheduled one. It runs against Nodes that
usually send and receive no essence, so the update must reach APPLIED with no unit in flight (RN-1).
Unicast activations go to `UNICAST_STREAM_TARGET` 192.0.2.1 (RN-2), and absolute activations must
land within `MAX_TIME_SYNC_OFFSET` (0.1 s; RN-27).

### 4.4 Details that make a salvo land together

- **Double bandwidth.** A receiver scheduled far ahead receives old and new groups meanwhile.
  `rx.join_lead_ns` (213, R) sends the new joins at `max(call, t − lead)` (C-N13); `JOIN_FAILED` then
  arrives after the 202, which IS-05 allows.
- **Counters are never reset.** For BCP-008's reset on activation, the Node takes a baseline on
  `MTL_EVENT_UPDATE` and subtracts it (C-N8).
- **The UDP source port.** IS-05 `source_port` is one value (`auto` = 5004; MTL's default is
  `udp_port`): the Node always sets it and never uses `session.src_port_mode` MULTI (C-N11).
- **RTCP transport parameters.** `rtcp.sr` is fixed at create and `rtcp.dst_port` changes only in
  STOPPED. The Node constrains IS-05's `rtcp_*` parameters to the values in use.

### 4.5 A Node on Phase 2 only

Without the Phase 7 extras a Node still works:

- `master_enable = false`: stop the session; re-enable starts it, with `AT_TAI` when scheduled.
- Re-activation: an update with the same flows; IGMP is not re-sent.
- Format change (IS-11, new SDP), immediate: `mtl_session_stop(&s, 1, MTL_STOP_FLUSH, MTL_MS(100))`,
  `mtl_session_update(s, &sc, MTL_UPDATE_MEDIA | MTL_UPDATE_FLOWS | MTL_UPDATE_LEGS, NULL, NULL)`,
  `mtl_session_start(&s, 1, NULL, NULL)`; identity, stats and the monitor survive; answer 200. A TX
  format change at a time is the same, started at the activation time.
- Scheduled RX format change at t, the **A/B swap**: name the new configuration `<uuid>.b` and
  `mtl_session_create()` it (a `-MTL_ENOSPC` with its reason answers 500: both sessions need capacity
  until t); `mtl_session_start(&nb, 1, &(struct mtl_when){.kind = MTL_AT_TAI, .value = t}, NULL)` (RX
  delivers units with media time ≥ t); answer 202 with `activation_time` = t; at t (a Node timer,
  Q-NMOS-10) mute or stop the old session, swap the handles, and `mtl_session_close(old, MTL_MS(200))`.
- Without reserved legs, a Node can also create the session at the first activation and close it on
  `master_enable = false`. It works, but every activation then costs a create (allocations, rules,
  queues), and a scheduled one must create early and start with `when`.

### 4.6 Boot (Phase 7 form)

1. `mtl_instance_open()`; read `instance.port_count`; per port, `mtl_port_get_spec()` gives
   `interfaces[p]` (`name`, `port_id` = `mac`, `chassis_id` = null); `clocks[0]` from
   `time.grandmaster_id`, `time.state` and `time.gm_traceable`. Create one queue subscribed to port,
   time and session events.
2. Per Sender or Receiver: `MTL_INIT(&sc)`, direction, essence, the essence member with
   `colorimetry`, `tcs`, `range`; `sc.name` = the UUID; every leg reserved and disabled (`flows[i]`
   all zero, `legs_disabled` = 0x1 or 0x3); option `caps.pacing_required = 1` (BCP-004-02).
   `mtl_session_create()`, `mtl_queue_bind(q, s, MTL_BIND_EVENTS)`, `mtl_session_start(&s, 1, NULL,
   NULL)`: RUNNING and muted, nothing on the wire.
3. Publish the IS-04 resources with `subscription.active = false` and `version` = now. Flow:
   `grain_rate` = the raster rate, `frame_*` = the raster, `interlace_mode` = the scan, `colorspace`
   = the colorimetry, `components` from `mtl_format_names()` (`sdp`), `bit_rate` =
   `info.payload_kbps`. Sender: `interface_bindings[i]` = the name of port `info.leg[i].port`,
   `bit_rate` = `info.wire_kbps`, `st2110_21_sender_type` = `info.sender_type`, `manifest_href` →
   `mtl_sdp_render()`.

## 5. SDP helper: `mtl_sdp.h` (`MTL_LATER`)

Every NMOS sender publishes an SDP on every activation and grandmaster change, and every receiver
reads one. Without a helper each Node repeats the same mistakes (a missing `TROFF`, an unreduced
`exactframerate`, `traceable` claimed without the accuracy check). The helper uses public calls only
and allocates nothing. Non-goal NG3 (no SDP parsing inside `lib/`; the library itself never needs
SDP, and `mtl_session_get_info` returns every SDP-relevant value) still holds: the helper is built on
public calls only (D-94, replacing Q-MODE-6).

**`mtl_sdp_render(s, meta, buf, cap)`** (CP) writes what a created session is now:

- the granted values and every existing leg. A muted session renders its legs as configured, since
  IS-05 still needs a transport file. ST 2022-7 is always two m-lines under `a=group:DUP` (ST
  2110-10 §8.5); the one-m-line form is parsed but never rendered;
- the time reference (`ts-refclk`, `mediaclk`) and colorimetry, always;
- under the IPMX profile, the `IPMX` keyword and `TP=2110TPN`;
- `a=rtcp`, and the `a=privacy` and `a=extmap` lines of the `crypto.*` options;
- `a=source-filter`, unless `MTL_SDP_NO_SOURCE_FILTER` (not allowed under IPMX).

It returns the length written (without the NUL), `-MTL_ENOSPC` if `cap` is short, or `-MTL_EBUSY`
(`WRONG_STATE`) while a value it needs is unknown (an ANC raster before start). The `o=` version
changes on every activation, grandmaster change and Info Block change; a sender re-renders on
`MTL_EVENT_GRANDMASTER` and `MTL_EVENT_RTCP_INFO`, so a change of clock source changes SDP, NMOS
and the reports together (TR-10-9 §12).

What it writes, and where each value comes from (the render specification; parse reads the same
lines):

| SDP element | Rule | Value |
|---|---|---|
| `c=`, `m=<media> <port> RTP/AVP <pt>` per leg; `a=source-filter: incl` | RFC 4566, RFC 4570; ST 2110-10 §8.4 SHOULD | `flows[i].ip`, `udp_port`, `info.leg[i].payload_type`; the source is the port's `sip` |
| `a=group:DUP <mid1> <mid2>`, `a=mid:` | ST 2110-10 §8.5; RFC 7104 | two legs, two m-lines |
| `a=ts-refclk:ptp=IEEE1588-2008:<gmid>:<domain>`, or `:traceable`, or `localmac=<mac>` | ST 2110-10 §8.2 MUST; `traceable` only when the grandmaster's `timeTraceable` is set and `clockAccuracy` ≤ 250 ns (0x22) | `info.ts_refclk.*`, `time.grandmaster_id`, `time.ptp_domain`, `time.gm_traceable`, `time.gm_clock_accuracy`; localmac: `info.leg[i].src_mac` |
| `a=mediaclk:direct=0`, or `sender` | ST 2110-10 §8.3 MUST, offset zero | TX: implied by RTP = floor(M × rate), `sender` for `MTL_MEDIA_SENDER`; RX: `rx.mediaclk`, `rx.rtp_offset` |
| `TSMODE`, `TSDELAY` | ST 2110-10 §8.7 SHOULD | `info.tsmode`, `info.tsdelay_ns` |
| `MAXUDP` | ST 2110-10 §8.6, required above the standard UDP size | `info.max_udp_bytes` |
| video `raw/90000`; fmtp `sampling`, `depth`, `width`, `height`, `exactframerate`, `colorimetry`, `PM`, `SSN` | ST 2110-20 §7.1–7.2, required | `video.format` → `mtl_format_names()` `sdp`; the raster; packing GPM and GPM_SL → `2110GPM`, BPM → `2110BPM`; colorimetry; SSN from colorimetry and TCS |
| fmtp `interlace`, `segmented`, `TCS`, `RANGE`, `PAR` | ST 2110-20 §7.3, when not the default | `video.raster.scan`, `video.tcs`, `video.range`; PAR in `fmtp_extra` |
| fmtp `TP`, `TROFF`, `CMAX` | ST 2110-21 §8.1: TP required; TROFF required when not TRODEFAULT (§6.2); CMAX optional | `info.sender_type`, `info.troffset_ns`, `info.troffset_default`, `info.cmax` |
| ST 2110-22 `jxsv/90000`, `packetmode`, `transmode`, `profile`, `level`, `sublevel`, `b=AS` | RFC 9134, BCP-006-01; `interlace`, `segmented` as appropriate | `cvideo.*`, the packing; profile, level, sublevel in `fmtp_extra`; `b=AS` from `info.wire_kbps` |
| audio `L24/48000/<ch>`, `L16`, `AM824`; `a=ptime`; `channel-order=SMPTE2110.(...)` | ST 2110-30, -31 | `audio.format`, `sample_rate`, `channels`, `ptime`; `meta.channel_order` |
| ANC `smpte291/90000`; `DID_SDID`, `VPID_Code`, `exactframerate`, `TM`, `SSN` | ST 2110-40:2023 | `anc.video.fps` (0 until start when taken from a start, so render is `-MTL_EBUSY` until then), `info.anc_tm`; DID_SDID and VPID in `fmtp_extra` |
| fast metadata (ST 2110-41) | rate and DIT in the SDP | `fastmeta.*`; no NMOS media type (Q-NMOS-6) |
| ST 2022-6 `SMPTE2022-6/27000000` | — | the RTP essence's encoding and clock rate |

IPMX Nodes also write lines the helper does not produce: `measuredpixclk`, `htotal`, `vtotal`,
`measuredsamplerate` (only as `fmtp_extra` text, §16), `a=hkep`, `FECPROFILE=profile-a` and `b=AS`
for TR-10-7 VBR; `a=privacy` and `a=infoframe` are the open RN-17 item (§16).

**`mtl_sdp_parse(sdp, len, &sc, &meta)`** (CP) fills a configuration for a receiver:

- flows (a leg per m-line, or per source of an `a=ssrc-group:DUP`), payload types and their check,
  source filters and the essence member;
- legs from `a=group:DUP`, as two m-lines or one m-line with two source filters. Legs beyond those
  parsed are zeroed and reserved, so a one-leg SDP never leaves a stale second leg;
- `rx.rtp_offset`, `rx.mediaclk` and the `crypto.*` options into `meta.options` (at most 8), for the
  caller to pass in `sc->options`;
- into `meta`: `MTL_SDP_IPMX`, the sender's `ts_refclk`, `rtcp_port`, and the remaining fmtp text in
  `fmtp_extra` (profile, level, sublevel, PAR, DID_SDID, measured values).

It returns the leg count. Unknown attributes are ignored; a malformed required one is `-MTL_EINVAL`
naming it. `struct mtl_sdp_meta` is 608 B with fixed string arrays, copied and NUL-terminated
(`-MTL_ENOSPC` names a field that does not fit). NMOS JSON stays outside MTL.

## 6. IS-08 and IS-11

IS-08 channel maps (capability flags `reordering` and `block_size`; activations may be scheduled)
are applied by the Node in its own code, at the media index of the activation instant
(`mtl_index_at()`). MTL keeps no map state, and `mtl_audio_remap()` (`mtl_convert.h`) is
`MTL_LATER`: a remap in MTL's copy path would save one copy in RX to TX gateways (G-N22).

IS-11 and BCP-004-01 (sink capabilities, EDID) are Node logic over `mtl_session_query()`. A
Sender's active constraints start empty; when they are violated "the Sender MUST become inactive. An
inactive Sender in this state MUST NOT allow activations": the Node mutes the sender. Sender states
are `unconstrained`, `constrained`, `active_constraints_violation`, `no_essence`, `awaiting_essence`;
receiver states `compliant_stream`, `non_compliant_stream` (SHOULD become inactive) and `unknown`
(no transport file). EDID is the Node's.

## 7. BCP-008 monitoring

BCP-008-01 (receiver) and -02 (sender) both require IS-12 and MS-05-02. Their domains and states:
`linkStatus` (AllUp, SomeDown, AllDown); receiver `connectionStatus` and sender `transmissionStatus`
(Inactive, Healthy, PartiallyHealthy, Unhealthy); `externalSynchronizationStatus` (NotUsed, Healthy,
PartiallyHealthy, Unhealthy) with `synchronizationSourceId`; receiver `streamStatus`, sender
`essenceStatus`; transition counters. Methods: `GetLostPacketCounters`, `GetLatePacketCounters`
(receiver), `GetTransmissionErrorCounters` (sender). Counters and messages reset on every activation
by default (`autoResetCountersAndMessages`). A 3 s `statusReportingDelay` holds back moves to a
healthier state, never to a worse one; deactivation goes straight to Inactive. "Late packets are
packets that arrived but arrived too late to be usable by presentation time"; an implementation that
cannot count them one by one "MUST at the very least increment every time the presentation is
affected". A change of synchronisation source MUST cause a temporary PartiallyHealthy.

The Node lists the schema once (`mtl_stat_list`), reads every value each 100–250 ms with
`mtl_stat_read()` (DP, one snapshot), and reacts to events. Hysteresis, reporting delay and status
messages are Node policy (Q-NMOS-9).

| BCP-008 property | Computed from |
|---|---|
| `linkStatus` | `port.link_up` of each enabled leg's port; `MTL_EVENT_PORT_LINK`, `MTL_EVENT_LEG_STATE`; the message names the interfaces of the legs that are down |
| `connectionStatus` (RX) | Healthy: `MTL_STATUS_RX_SIGNAL` and no new incomplete or redundancy-used units. PartiallyHealthy: Δ`rx.units_used_redundancy` or Δ`leg.pkts_lost{leg}` with complete units. Unhealthy: no signal, `JOIN_FAILED`, Δ`rx.units_incomplete_*`, Δ`rx.pkts_lost_est`, late packets |
| `GetLostPacketCounters` | `leg.pkts_lost{leg}`, `rx.pkts_lost_est`, minus the baseline |
| `GetLatePacketCounters` | `rx.pkts_stale`, `rx.units_stale`, `leg.pkts_late{leg}`, `rx.units_late_presentation` |
| `streamStatus` | Δ`rx.pkts_rejected{cause}`, `MTL_STATUS_FORMAT_CHANGED`, `rx.detected.*`; optional `tp.*` with `rx.timing_parser` |
| `transmissionStatus` | PartiallyHealthy: Δ`tx.units_late`, `MTL_STATUS_PACING_DOWNGRADED`, `MTL_STATUS_TIMING_WARNING`, one leg `WAITING_NEIGHBOUR`, Δ`leg.pkts_skipped`. Unhealthy: Δ`tx.units_dropped{reason}`, Δ`tx.units_failed`, every leg waiting, ERROR, Δ`port.tx_errors`. `tx.units_muted` is not an error |
| `GetTransmissionErrorCounters` | `tx.units_dropped{reason}`, `tx.units_late`, `tx.units_failed`, `leg.pkts_skipped{leg}`, `port.tx_errors`, `tx.build_overrun`, minus the baseline |
| `essenceStatus` | `MTL_EVENT_TX_UNDERRUN`, Δ`tx.slots_empty`; content checks are the application's |
| `externalSynchronizationStatus`, `synchronizationSourceId` | per port `time.state`, `time.grandmaster_id`, `MTL_EVENT_TIME_STATE`, `MTL_EVENT_GRANDMASTER`; NotUsed when `rx.mediaclk` follows the sender |
| `overallStatus` | the least healthy domain; Inactive from the Node's `master_enable` |

**The monitor**, one per session. On `MTL_EVENT_UPDATE` APPLIED, with auto-reset: read every value
as the baseline, clear messages and transition counters, set every domain Healthy and hold it until
t + `statusReportingDelay`. Each tick (100–250 ms, and on every event): while the Node's
`master_enable` is false every domain is Inactive or NotUsed at once. Link: the count of enabled legs
whose port has `port.link_up` 0 gives AllUp, SomeDown or AllDown. Sync: per leg port, `time.state`
LOCKED on all is Healthy, on some PartiallyHealthy, on none Unhealthy, an internal clock NotUsed;
`MTL_EVENT_GRANDMASTER` gives PartiallyHealthy for one reporting delay. Then delta = current − last;
worse states apply at once, better ones after the delay has held.

**The keys NMOS adds** (Phase 7; the registry is [contract.md](contract.md) §11.3):

- `leg.pkts_late{leg}`, `rx.units_late_presentation`: late packets and late units (G-N16);
- `info.wire_kbps{leg}`, `info.payload_kbps` (kbps rounded up), `info.max_udp_bytes` (its input is
  `session.max_udp_payload`), `info.troffset_default` (G-N15);
- `anc.did_sdid_seen`: up to 16 DID/SDID pairs (G-N19);
- `time.gm_traceable`, `time.gm_clock_class`, `time.gm_clock_accuracy`, `time.gm_priority1`, and
  `info.ts_refclk.*{leg}`, all gauges because a grandmaster can change at runtime (G-N11, C-N9);
- `instance.port_count` (G-N13);
- `tx.units_muted`, so a muted sender does not look unhealthy (it is not `tx.units_dropped`).

## 8. NMOS in a pod

REST and mDNS use the pod's primary network; media uses the SR-IOV VFs. IS-04 unregistration goes in
the preStop hook, before SIGTERM; MTL's shutdown follows in the rest of the grace period
([deployment.md](deployment.md)). `MTL_HEALTH_READINESS` gates registration and fails only when the
instance cannot carry media; one stream in ERROR is `MTL_HEALTH_DEGRADED`, information only.

## 9. The IPMX profile

`session.profile` (1030, C; the instance default is `instance.profile`, 2040) is
`MTL_PROFILE_ST2110` or `MTL_PROFILE_IPMX`. A profile changes only what zero means in some fields,
and the compliance labels (`MTL_INFO_NON_COMPLIANT` is judged against it, C-I4). It never changes
what an explicit field asks for. Where 0 is also a real value, there is a way to ask for it
literally.

| Default | `ST2110` | `IPMX` |
|---|---|---|
| `rtcp.sr`, `rtcp.rx` | off | on |
| video schedule | from `sender_type` (N, NL, W) | N with the TR-10-1 CMAX = max(16, int(Npkts / (21600 · TFRAME))) and the IPMX VRX; SDP `TP=2110TPN` |
| DSCP (`mtl_flow.dscp` 0) | CS0 | AF42 for video, ANC and compressed video; AF41 for audio. InfoFrame, FEC and HDCP streams take their associated stream's (TR-10-9 §16). `MTL_FLOWF_DSCP_LITERAL` asks for CS0 |
| `rx.mediaclk` | DIRECT | AUTO (from the reports' Info Block and the grandmaster) |
| `MTL_CVIDEO_VBR_MAX`, a free launch (`MTL_MEDIA_SENDER`) | non-compliant | compliant (TR-10-7, TR-10-1) |
| checks | none | a source filter on every RX leg and in the SDP; DHCP by default on kernel ports (`port.dhcp`) |

No separate IPMX sender type: TR-10-1 allows a Type N CMAX and signals `TP=2110TPN` (RN-23), so
`MTL_SENDER_N = 0` means N under both profiles. RACTIVE comes from `video.vtotal` (505, C; the ST
2110-21 value for its rasters, else the height); `video.htotal` (506) defaults to the width.

## 10. RTCP sender reports: `mtl_rtcp.h` (`MTL_LATER`)

**Two uses of port +1.** Today's `rtcp.*` options are MTL's own NACK retransmission (application
packets, PT 204, named "IMTL"). They are renamed `rtx.*` (1000–1004); the legacy names stay in the
legacy bridge. `rtcp.*` (1010–1013) now means RFC 3550 sender reports (C-I8). Both use port +1 and
are told apart by payload type. A plain RFC 3550 mode without the Info Block is not offered: neither
NMOS nor IPMX requires it. Keys: `rtcp.sr` (1010, bool, C), `rtcp.cname` (1011, C; the session
name @ the port IP), `rtcp.dst_port` (1012, S; `udp_port` + 1), `rtcp.rx` (1013, bool, C); the
profile sets the two bools.

**TX.** Each leg sends a compound packet (sender report, Info Block, SDES CNAME) to its own address
at `rtcp.dst_port`, with the leg's DSCP and TTL, on the TR-10-1 schedule:

- video and compressed video: one per frame, or per field when interlaced, queued just before the
  unit's first packet;
- ANC and fast metadata: one per new RTP timestamp, before its first packet (TR-10-1 §8.9.2);
- audio: one before the first packet, then every int(10 ms / ptime) packets;
- none while the session is muted.

A control thread prepares the compound packet into one of two template buffers and publishes it
with a release store. When the tasklet takes unit k it copies the template into an mbuf, stamps NTP
(from M), RTP and the packet and octet counts, and enqueues it ahead of the unit's first packet on
the same queue, so the TR-10-1 order holds by queue order: about 100 ns per frame **[inferred]**.
Under rate-limit pacing the report takes one packet slot (≈ 1 µs at 1080p) in the shaped queue and
is not a media packet for VRX. Each ST 2022-7 leg sends its own copy. The library checks only the
32-bit alignment of application blocks and that report, Info Block and SDES fit one datagram; it
never interprets blocks it did not build. A TX session does not listen on port +1 for reports (only
the NACK retransmission does).

**The Info Block.** The library writes its own fields: ts-refclk from the time state and mediaclk
from the media mode, unless the application sets them (`MTL_RTCP_REFCLK_APP`,
`MTL_RTCP_MEDIACLK_APP`). It writes the Media Info Block of the essence from the config and
`struct mtl_rtcp_info` (144 B: PAR, `measured_rate_milli`, `channel_order`): video 0x0001, PCM 0x0002, AES3
0x0004, privacy 0x0011 from the `crypto.*` options. It then appends the application's blocks unread
(JPEG XS 0x0008, HDR 0x0006, the compressed-video blocks); `MTL_RTCP_MIB_APP_ONLY` sends only
those. The SDP stays outside the library: NG3 holds (C-I11).

- `mtl_rtcp_set_info()` (CP) applies from the next report. When the bytes change, the block version
  increments (`tx.rtcp_info_version`) and `MTL_EVENT_RTCP_INFO` tells the application to re-render
  SDP and re-publish to NMOS. `-MTL_ENOSPC` if report, Info Block and SDES do not fit one datagram;
  `-MTL_EINVAL` beyond the wire field sizes (ts-refclk 64, mediaclk 12 characters).
- A unit's meta record `MTL_META_RTCP_MIB` appends blocks to that unit's report only, for HDR
  metadata that changes per field (TR-10-16), and never changes the version. The meta area holds
  records back to back, at most one per (kind, tag) (C-I10).

**RX.** With `rtcp.rx` the session receives reports on port +1 (the system queue by default,
Q-I-8), keeps the latest per leg, and maps each unit's RTP onto the sender's clock
(`unit.media_tai_ns` flagged `MTL_UNITF_SENDER_TIME`).

- `mtl_rtcp_read()` (WT) gives the oldest unread `struct mtl_rtcp_report` (48 B) and copies its Info
  Block. It returns 1, or `-MTL_EAGAIN` with `MTL_WAIT_RTCP` (in `mtl.h`) armed. A full ring drops
  the oldest (`rx.rtcp_sr_dropped`).
- `mtl_rtcp_mib_next()` (AS) iterates the Media Info Blocks.
- An HDMI-out receiver recovers the source clock from the reports and `rx.sender_rate_ppb`
  (I-REQ-26); locking the output clock is the application's job.

## 11. IPMX wire details

- **Any raster.** Width and height up to 32767, any rate with num ≤ 4194303 and den ≤ 1023 (the MIB
  fields). The engine replaces its fixed frame-rate table with rationals (GI-7).
- **VBR compressed video.** `cvideo.max_bitrate_bps` (515, C) is the ceiling, and CMAX comes from
  it. Library packetisation of H.264 and H.265 is later; until then the application uses packet
  units (GI-8).
- **RTP header extensions and CSRCs on RX.** The RX parsers read fixed offsets and ignore X and CC
  (SF-68 **[verified]**; TX writes X = CC = 0 at `st_tx_video_session.c:959-960`), so an HDCP or PEP stream is mis-parsed. The engine fix skips them before
  the payload header; packet units keep `data` at the RTP header and flag `MTL_PKTE_HDR_EXT` (GI-9).
- **IGMPv2.** `port.igmp_version` (2124, C): v3 by default, falling back to v2 after a v2 querier
  (RFC 3376). With V2, SSM is lost and `source_filter` is checked in software (GI-12).
- **InfoFrames before video.** `tx.precede` (113, C) on an ST 2110-41 session (port +3) names a
  video session on the same timeline. Its unit k shares the video's queue, takes the video unit k's
  RTP and media time (TR-10-10 §6, §9), and goes out just before it, so the order survives
  rate-limit shaping. A Node activates both with the same `when` (GI-14).
- **Link offset while active.** `rx.link_offset_ns` (203, R) changes at the next unit, or at an
  update's boundary. `MTL_LINK_OFFSET_AUTO` takes the measured minimum. The gauges
  `rx.link_offset_min_ns` (a high percentile of unit complete − media time, plus delivery) and
  `rx.link_offset_max_ns` (pool depth × unit period − one unit) give IS-05's constraints (GI-13,
  C-I7).
- **Measured rates.** `tx.f2f_pp_ns` is max − min of the first-packet intervals over 2 s (TR-10-9
  §11.2); `rx.sender_rate_ppb` compares RTP with arrival time over a window (GI-17).
- **In-band control** (TR-10-9 §18–19): DHCP is `port.dhcp`; on a DPDK port TCP and mDNS need
  `port.virtio_user` with 224.0.0.251 forwarded (Q-I-9); in a pod the primary network carries them.

## 12. Timing without PTP and SENDER mode

IPMX has four clocks: the common reference (PTP, when present), each device's internal clock (free
running without a grandmaster), a stream's media clock, and its RTP clock. An **async** source (an
HDMI input at its own rate) has an RTP clock that follows the source, the SDP says
`mediaclk:sender`, and the reports carry (internal clock, RTP) pairs for receivers to follow.

R5 says what a time is: ns on the instance clock, TAI while the time base is locked,
`MTL_TIMEF_ESTIMATED` when not, `MTL_UNITF_SENDER_TIME` for an RX value on another device's clock
(C-I1). Time sources in general: [timing.md](timing.md); which time source, media mode and source
kind each IPMX case uses (playout with or without PTP, genlocked capture, HDMI capture, inline
processor), and why SENDER mode exists: [timing.md](timing.md) §13.

| Piece | Design |
|---|---|
| a clock that runs free without stepping | `MTL_TIME_SOURCE_FREERUN`: seeded once from `CLOCK_REALTIME` plus the UTC offset, then runs on the TSC or an undisciplined PHC, never stepped, ESTIMATED. Its time state is `MTL_TIME_FREERUN`, which health counts as ready. `SYSTEM_TAI` follows NTP steps and stays for other uses |
| staying close to the controller's clock | `time.freerun_slew_ppm` (2230, R): FREERUN follows `CLOCK_TAI` by frequency only, bounded, never stepping; 0 = never. A 10 ppm crystal drifts about 0.86 s a day **[inferred]**, past the 0.1 s the IS-05 test suite allows for absolute activations. A Node can also convert IS-05 times with `mtl_time_convert()` |
| switching between free run and PTP while running | AUTO re-evaluated while running (below); `time.fallback` (2208: HOLDOVER or FREERUN, default FREERUN) |
| async sources | `MTL_MEDIA_SENDER`: RTP follows the source, the one exception to D-09's RTP = floor(M × rate) (C-I2) |
| inline processors keeping the input's timing (TR-10-1 §9, a MUST) | `MTL_SUBMIT_SENDER_TIME`: RTP and the report's NTP come from the received unit (`unit.rtp`, `unit.media_tai_ns` with SENDER_TIME); launch = submit + `min_tx_delay_ns` on the instance clock, so the upstream clock never sets a launch time |

**AUTO at runtime.** AUTO picks a disciplined NIC PHC, else `CLOCK_TAI`, else FREERUN, and moves to
a better source when one appears. The step posts `MTL_EVENT_TIME_STATE` and `MTL_EVENT_TIME_STEP`,
and the Info Block's ts-refclk changes from `localmac=` to `ptp=`. When the source is lost, AUTO
holds over and then, by `time.fallback`, runs free without a step. Sessions on the epoch timeline
follow their step policy; pending updates fail with `TIME_STEP`. When AUTO locks to a new
grandmaster the internal clock steps: SENDER sessions keep their RTP (it follows the source) and
their report NTP values jump; INDEX and AUTO sessions on the epoch timeline move their RTP to the
PTP grid, which receivers see as a discontinuity. The Info Block version increments and
`MTL_EVENT_RTCP_INFO` tells the application to update SDP and NMOS (TR-10-9 §12, PQCR §4.1.1). In a pod AUTO's order ends in
FREERUN, so a pod without PTP is an IPMX sender with `localmac=`, not an error.

**SENDER mode** (`mtl.h`, `enum mtl_media_mode`). `unit.media_tai_ns` is the source's own sampling
instant, read on the instance clock and never snapped:

- RTP = RTP0 + floor(k × period × rate). RTP0 = floor(M0 × rate) at the first unit, or after
  `MTL_SUBMIT_DISCONTINUITY`, which re-anchors it.
- k advances by max(1, round((M − M_prev) / period)) per unit. A missed VSYNC skips one period, and
  drift does not build up as it would with round((M − M0) / period). A result's `media_index` is k.
- Launch = M + `min_tx_delay_ns` on the nominal-period schedule. A unit that would overlap the
  previous one is DROPPED (`WOULD_OVERLAP`).
- The sender report carries M. `MTL_INFO_MEDIACLK_SENDER` is set in the session info.
- `MTL_AT_INDEX`, `RX_BY_INDEX`, `tx.precede` targets and `mtl_index_at()` are `-MTL_EINVAL` on such
  a session, because its indices are not on a grid.
- An inline processor (`MTL_SUBMIT_SENDER_TIME`) also copies the input's ts-refclk and mediaclk into
  its own Info Block with `MTL_RTCP_REFCLK_APP` and `MTL_RTCP_MEDIACLK_APP`, so the output report keeps
  the input's time domain (TR-10-1 §9).
- Late policy (proposed): bounded SEND_LATE within `tx.late_tolerance_ns`, then DROP. Open: the
  `tx.late_policy` comment in `mtl_options.h` names defaults only for AUTO, INDEX and TAI; the SENDER
  default must be stated there (see decisions.md §5).

Without this mode, an async source at +50 ppm on a 59.94 session would drop a frame every 5.6
minutes under NEAREST snapping. NEAREST stays the default for sync sources (C-I3).

**Worked example: async HDMI, no PTP.** 1080p59.94 video and 48 kHz audio from one HDMI input, no
grandmaster, `MTL_TIME_SOURCE_FREERUN`, `MTL_MEDIA_SENDER`, `MTL_SOURCE_CAPTURE`; the video source is
50 ppm fast, the audio clock measures 47 952 Hz (TR-10-3 §12's example).

1. First VSYNC on the internal clock: M0 = 1 700 000 000.123 456 789 s; RTP0 = floor(M0 × 90 000)
   mod 2^32 = 153 000 000 011 111 mod 2^32 = 380 025 703.
2. Frame k: RTP = RTP0 + floor(k × 1501.5) = 380 025 703, 380 027 204, 380 028 706, 380 030 207
   (TP-1 §13.3 expects increments of 1501.5), whatever the VSYNC timing.
3. VSYNCs come every 16.683 333 ms / (1 + 50 ppm) = 16 682 499.2 ns: M1 = …140 139 288 ns, M2 =
   …156 821 787 ns; the application submits `media_tai_ns` = Mk.
4. The report for frame 1, before its first packet: NTP MSW 1 700 000 000 (seconds, low 32 bits),
   LSW 140 139 288 (ns, the PTP truncated format, not an NTP fraction), RTP 380 027 204, the counts;
   Info Block ts-refclk `localmac=<port MAC>`, mediaclk `sender`; MIB 0x0001 with `measuredpixclk`
   148 359 066 Hz (148.5 MHz × 1000/1001 × (1 + 50 ppm)), htotal 2200, vtotal 1125; SDES CNAME.
5. The first packet leaves at Mk + `min_tx_delay_ns`, then the IPMX gapped schedule at TRS = TFRAME ×
   (1080/1125) / Npkts.
6. Report 600: RTP 380 926 603, NTP 1 700 000 010.132 956 314. A receiver computes ΔRTP / ΔNTP =
   900 900 / 10.009 499 525 s = 90 004.5 Hz: +50 ppm against the sender's internal clock.
7. Audio, 125 µs ptime (6 samples): first sample at M0 + 2 ms, RTP0 = floor(that × 48 000) mod 2^32 =
   4 211 316 613; a report every 80 packets, RTP +480, NTP +480 / 47 952 s = +10 010 010 ns
   (…125 456 789, …135 466 799, …145 476 809); the MIB says 48 000 Hz nominal, `measuredsamplerate`
   47 952.

A PTP-locked sync sender at that instant stamps floor(N × TFRAME × 90 000) = 380 024 949 at grid
frame N = 101 898 101 905: the async sender is 754 ticks (8.4 ms) off the grid, which
`mediaclk:sender` makes legal.

**Receivers.** The IPMX VRX is 2 × CMAX packets (32 for HD, TR-10-1 Appendix A) and starts half
full, so a receiver needs no grid and no TAI; MTL's slot reassembly already works on arrival. The
method (TR-10-9 §11.1): the reports when ts-refclk is `localmac=` or a PTP clock the receiver is not
locked to; PTP plus the reports when both share the grandmaster. Units of one sender align through its clock even without PTP: `mtl_rx_align()` works
on `media_tai_ns`, including SENDER_TIME values of one sender. Units of different senders need a
common clock. `rx.mediaclk = MTL_MEDIACLK_AUTO` compares the Info Block's ts-refclk with the
instance's own grandmaster and uses the reports when they differ (I-REQ-68).

**BMCA** (TR-10-1 §7.2): the built-in client takes the first Announce (`mt_ptp.c:1032`
**[verified]**), so the IPMX recipe is `MTL_TIME_SOURCE_PHC` with ptp4l (Q-I-7).

## 13. PEP encryption and HDCP: `mtl_crypto.h` (`MTL_LATER`)

IPMX PEP (TR-10-13) encrypts payloads with AES-CTR in 16-byte slices; RTP header, extensions and
payload header stay clear, and the counter rides in an RFC 8285 extension (Full on the first packet
of a frame, field, slice or audio packet and on every non-A/V packet, Short otherwise). The library
writes and parses the extensions, keeps the counters, sizes packets for them (8 B, 20 B on the
first, 8 B more with MAC modes: about 0.7 % more packets at 1200 B), and sends one ciphertext on
both ST 2022-7 legs; RTCP stays clear. Key provisioning (PSK, KDF, ECDH, a fresh key generator per
boot, the IS-05 parameters) stays with the application (Q-I-10). The packet layout is
[diagrams.md](diagrams.md) §11.3.

**Counters and sizing.** The payload header stays clear: RFC 4175's extended sequence number and
SRDs, RFC 9134's 4 bytes, RFC 8331's header; audio has none. Packet i of a unit gets ctr_i = the
counter of its first slice, and ctr_(i+1) = ctr_i + ceil(len_i / 16) **[inferred: every slice
counts, partial ones included; Q-I-12]**. The Full extension carries key_version and the 64-bit
ctr; the Short one the low 24 bits (a gap below 2^24). Sizing subtracts the extension (8 B, 20 B on
the first packet), and 8 B more with MAC modes, from the 1460 B Standard UDP Size Limit. HDCP
(TR-10-5 §14) uses the same structure with its own Full and Short IV counters (frz, streamCtr,
inputCtr); PEP can encrypt an HDCP stream in place, reusing inputCtr (TR-10-13 §20.4).

| Function | Library | Application or NMOS layer |
|---|---|---|
| PSK storage and provisioning, key_id, KDF (CMAC, HMAC-SHA-512/256), ECDH, iv and key_generator choice | no | yes (TR-10-13 §12, §17; BCP-005-03) |
| `a=privacy`, `a=extmap`, IS-05 `ext_privacy_*` | values from the `crypto.*` options, rendered by `mtl_sdp.h` | writes the NMOS side |
| PEP MIB 0x0011 | built from the `crypto.*` options | — |
| install keys at an activation, rotate at a boundary | `mtl_crypto_set_key(..., when)` | decides when (IS-05 activation, RTP_KV schedule) |
| write and parse the CTR extensions, counters, payload sizing | yes | no |
| AES-CTR (and CMAC) over payloads | yes, off the tasklet | or the application encrypts itself with packet units |
| HDCP: AKE, HKEP over TCP, session key, lc128 | no (licensed secrets) | vendor code; a cipher plugin later, or packet units |

**Configuration** is options, none of them secret. The R keys let an IS-05 activation to another
encrypted sender change them at its boundary, and the SDP helper renders and parses them:

- R: `crypto.scheme` (1021: `MTL_CRYPTO_PEP_RTP`, one key per activation, or `MTL_CRYPTO_PEP_RTP_KV`,
  key versions at unit boundaries; absent = none), `crypto.mode` (1022: `MTL_CRYPTO_AES128_CTR`,
  mandatory and the default, AES256 and the CMAC64 and CMAC64_AAD variants), `crypto.ext_id_full`
  and `crypto.ext_id_short` (1023, 1024: RFC 8285 IDs 1–14, required), `crypto.clear_bytes` (1025,
  the payload header), `crypto.iv` (1026, the 64-bit base IV, required), `crypto.substream` (1027,
  0..1023);
- C: `crypto.workers` (1020, cipher threads; 0 = in the caller), `crypto.in_place` (1028, TX may
  overwrite attached memory with ciphertext).

**Where the cipher runs:** never on a tasklet (AES over a 5 MB frame is milliseconds of one core);
in the caller at submit and dequeue (both become DPC) or on `crypto.workers` threads. The time it
adds moves `min_submit_lead_ns` (in `mtl_session_info`).

| Path | TX | RX |
|---|---|---|
| frame and row units, library pool | in the caller during `mtl_tx_submit` (DPC), in place, per packet region; rows per submitted range | the tasklet lands ciphertext and stores each packet's ctr in the slot's packet table (8 B per packet); `mtl_rx_dequeue` decrypts in place in the caller (DPC), then zero-fills gaps; rows in `mtl_rx_wait_rows` |
| attached application memory | a copy to a library shadow slot while encrypting, unless `crypto.in_place`; `MTL_SESSION_REQUIRE_DIRECT` without it fails at create | as library pools: ciphertext sits in the application's slot until dequeue |
| `unit.hold` (RX to TX zero copy) | the copy path (in place would corrupt the shared RX slot) | — |
| packet units | per packet at submit (DPC); or the application encrypts and writes its own extension with `packet.set_fields` verbatim | `MTL_PKT_RX_LEND` and the application decrypts, or the library decrypts at dequeue |
| MAC modes | a MAC per packet, so payload regions no longer match the frame: always the copy path | the 8 MAC bytes interleave with pixels: packets go through a staging copy; a failed MAC counts in `rx.crypto_auth_fail` and the packet is lost |
| offload | `crypto.workers` threads encrypt between submit and pick-up, adding their time to `min_submit_lead_ns` | the same for decrypt before delivery |

**Cost** **[inferred unless noted]**; the spike measures cycles per byte with DPDK's ipsec-mb or
OpenSSL on the target CPUs before defaults are fixed:

| Item | Cost |
|---|---|
| AES-128-CTR with AES-NI / VAES | ≈ 1 / ≈ 0.3 cycle per byte per core |
| 1080p59.94 YCbCr 4:2:2 10-bit (5.18 MB per frame, 311 MB/s) **[computed]** | 0.03–0.10 of a 3 GHz core, TX or RX |
| 2160p60 YCbCr 4:2:2 10-bit (1.24 GB/s) / RGB 8-bit (1.49 GB/s) **[computed]** | 0.12–0.50 of a core |
| CMAC-64 modes | about the same again, serial within a packet (multi-buffer across packets) |
| tasklet, TX | the extension write per packet (2 stores into the header mbuf); sizing at create |
| tasklet, RX | the extension parse (+1 branch) and an 8 B ctr store per packet |
| wire | +8 B per packet (≈ 0.7 % at 1200 B), +20 B once per frame |

**Keys** are never options, because options are readable (C-I9). The one call is
`mtl_crypto_set_key(s, key_version, key, key_bytes, when)` (CP; 16 or 32 bytes, copied):

- TX uses the key from the first unit at or after `when` (NULL = the next unit). RX applies it to
  units whose media time is at or after `when` (the update rule), keeps it beside the current key,
  and picks per packet by the `dynamic_key_version` of the Full extension (RTP_KV).
- RTP_KV: `key_version` may increase by one at a frame, field or GOP boundary (audio: a packet), and
  the counter restarts at 0 (TR-10-13 §20.3). `mtl_crypto_set_key(s, v + 1, key, 16, &when)` with
  `MTL_AT_INDEX` gives an exact unit boundary: the tasklet switches with the unit and the caller-side
  cipher picks the key by unit index, so no lock is shared.
- The counter restarts at 0 only for key bytes this session never used. Re-installing a used key
  continues its counter, so no IV and counter pair repeats under one key (TR-10-13 §15).
- An RX packet with an unknown key version counts in `rx.crypto_unknown_key{version}` and posts
  `MTL_EVENT_KEY_NEEDED` (30).
- `key` NULL zeroises every key. Keys live in locked, non-dumpable memory (`mlock`,
  `MADV_DONTDUMP`), are zeroised on replace and close, are never readable back, and never reach logs,
  stats or captures (captures record ciphertext).
- A TX unit without a key is DROPPED (`NO_KEY`, counted in `tx.units_no_key`), never sent in clear.
  An RX packet that fails authentication is a lost packet (`rx.crypto_auth_fail`).

**HDCP** keys are licensed secrets that an open-source library should not hold: HDCP 2.3's cipher is
AES-128 in counter mode too, so the data path would fit it **[inferred]**, but its session key and
lc128 fall under the DCP licence robustness rules. HDCP streams use
packet units, with the vendor's code encrypting and writing the HDCP extension. An HDCP receiver
also uses packet units, because frame units deliver ciphertext pixels and no extension values
(streamCtr). A vendor cipher plugin is later (GI-11, Q-I-4).

## 14. Gap dispositions

### 14.1 NMOS gaps (N1)

| Gap | Disposition | Where | Phase |
|---|---|---|---|
| G-N1 activation instant | adopted, reduced: planned instant from the call, state and applied instant in the status, `MTL_EVENT_UPDATE`; the switch by the clock. Rejected: a separate getter and first-index/first-RTP fields, because the status and event carry what IS-05 needs | `mtl.h` | 2 |
| G-N2 re-apply | adopted; RX re-sends reports without a leave | `MTL_UPDATE_REAPPLY` | 7 |
| G-N3 mute | adopted; muted units are not "dropped" | `legs_disabled`, `MTL_STATUS_MUTED`, `tx.units_muted` | 7 |
| G-N4 reserved legs | adopted, simpler: a set bit on an unaddressed leg reserves it; no extra field | `mtl_flow` rule | 7 |
| G-N5 port change while running | adopted, make before break | `MTL_UPDATE_FLOWS` | 7 |
| G-N6 RX format change at an instant | **later**; the A/B swap until then; colorimetry, TCS and range may change while running | — | later |
| G-N7 dry run | adopted | `MTL_UPDATE_DRY_RUN` | 7 |
| G-N8 cancel | adopted as `parts` 0, results 0 / 1 / `-MTL_EBUSY` | `mtl_session_update` | 7 |
| G-N9 join lead | adopted | `rx.join_lead_ns` | 7 |
| G-N10 colorimetry, TCS, range | adopted; zero is the standard's default; colorimetry always rendered | `mtl.h`, `mtl_format.h` | 7 |
| G-N11 time metadata | adopted | `mtl_time_set_reference()` (Phases 1–2, for pods), `time.gm_*` | 1–2; 7 |
| G-N12 grandmaster change | adopted | `MTL_EVENT_GRANDMASTER` (27) | 7 |
| G-N13 MAC, port count | adopted | `mtl_port_spec.mac`, `instance.port_count` | 7 |
| G-N14 LLDP | **deferred**: `chassis_id = null` is allowed; topology tools read the switch | — | later |
| G-N15 bit rate, sizes | adopted | `info.*` keys | 7 |
| G-N16 late counters | adopted | `leg.pkts_late`, `rx.units_late_presentation` | 7 |
| G-N17 SDP | adopted as an optional helper | `mtl_sdp.h` | 7 |
| G-N18 offsets at an activation, announce timeout | adopted: R options in the update's config apply at its boundary | `mtl_session_update`, `time.ptp_announce_timeout` (2209) | 7 |
| G-N19 DID/SDID seen | adopted | `anc.did_sdid_seen` | 7 |
| G-N20 RTCP | adopted through GI-1…3, IPMX reports only | `mtl_rtcp.h` | 7 |
| G-N21 ST 2022-5 FEC | **not adopted**: the Node omits the FEC parameter set | — | — |
| G-N22 audio remap | **later** (`MTL_LATER`); the Node remaps in its own code | `mtl_audio_remap()` | later |
| G-N23 RX unicast | adopted as a rule: RX `ip` is the port's own address or all zero; `source_filter` checks the sender | `mtl_flow` comment | 1 |
| G-N24 address change | adopted | `MTL_EVENT_PORT_ADDRESS` (28) | 7 |
| G-N25 past instant | adopted: a past instant means now; no horizon limit for updates | `mtl_session_update` comment | 2 |

### 14.2 IPMX gaps (I1)

| Gap | Disposition | Where | Phase |
|---|---|---|---|
| GI-1 sender reports | adopted, IPMX mode only, scheduled per essence | `rtcp.sr`, `rtcp.cname`, `rtcp.dst_port` | 7 |
| GI-2 Info Block | adopted, reduced: the library builds the essence block; no builder function, no getter; per-unit blocks append without a version change | `mtl_rtcp_set_info()`, `MTL_META_RTCP_MIB`, `MTL_EVENT_RTCP_INFO` (29) | 7 |
| GI-3 RX reports, sender time | adopted, plus `mtl_rtcp_mib_next()` | `mtl_rtcp_read()`, `MTL_WAIT_RTCP`, `MTL_UNITF_SENDER_TIME`, `MTL_MEDIACLK_AUTO`, `rtcp.rx` | 7 |
| GI-4 async media mode | adopted, with the k rule and inline processors | `MTL_MEDIA_SENDER`, `MTL_SUBMIT_SENDER_TIME`, `MTL_INFO_MEDIACLK_SENDER` | 7 |
| GI-5 free run, AUTO at runtime | adopted, with a bounded slew | `MTL_TIME_SOURCE_FREERUN`, `time.fallback`, `time.freerun_slew_ppm` | 7 |
| GI-6 IPMX sender type | **replaced**: the profile gives N the TR-10-1 CMAX and VRX | `session.profile`, `video.vtotal`, `video.htotal` | 7 |
| GI-7 any raster | adopted (contract and engine fix) | `mtl_raster` comment | 7 |
| GI-8 VBR | adopted as an option; H.26x packetisation **later** | `cvideo.max_bitrate_bps` | 7 |
| GI-9 header extensions on RX | adopted (engine fix, SF-68) | `MTL_PKTE_HDR_EXT` | 7 |
| GI-10 PEP | adopted, parameters as options | `mtl_crypto.h`, `crypto.*`, `NO_KEY`, `MTL_EVENT_KEY_NEEDED` | 7, after the spike |
| GI-11 HDCP cipher plugin | **later**; packet units until then | — | later |
| GI-12 IGMPv2 | adopted, merged with K-REQ-20 | `port.igmp_version` | 7 |
| GI-13 link offset while active | adopted | `rx.link_offset_ns` R, `MTL_LINK_OFFSET_AUTO` | 7 |
| GI-14 InfoFrames first | adopted: shared queue, the video's RTP and media time | `tx.precede` | 7 |
| GI-15 FEC Profile A | **later** | — | later |
| GI-16 profile | adopted | `session.profile`, `instance.profile` | 7 |
| GI-17 keys | adopted | `tx.f2f_pp_ns`, `rx.sender_rate_ppb`, `rx.rtcp_info_version`, `rx.crypto_auth_fail` | 7 |

Data-path cost per gap: GI-1 O(1) from a template, ≈ 100 ns per frame; GI-2 none (a template swap);
GI-3 none on the system queue, ≈ 50 ns per report on a session queue; GI-4, GI-5, GI-6 (computed at
create), GI-7, GI-8, GI-12 and GI-16 none; GI-9 one branch per packet; GI-10 8 or 20 B of extension
per packet written on the tasklet; GI-13 gauges from existing per-unit times; GI-14 one enqueue per
frame on the video tasklet; GI-15 XOR into two accumulators per packet, ≈ 1 % of a core at 1080p60
**[inferred]**; GI-17 counters on existing paths. None changes a pinned-core rule.

### 14.3 Conflicts

Every conflict of N1 §6 and I1 §7 is resolved:

- C-N1 none (the resource is the session); C-N2 G-N3; C-N3 G-N5; C-N4 G-N4 and G-N6; C-N5 G-N2; C-N6
  G-N1; C-N7 mute keeps sessions RUNNING, so `when` is honoured; C-N8 the Node's baseline; C-N9
  G-N11; C-N10 G-N18; C-N11 the Node sets the source port, never MULTI; C-N12 an explicit
  `flows[1].port`; C-N13 G-N9; C-N14 `caps.pacing_required`.
- C-I1 R5; C-I2 SENDER mode; C-I5 GI-5; C-I6 the profile; C-I7 GI-13; C-I8 the `rtx.*` rename; C-I9
  keys through `mtl_crypto_set_key()`; C-I10 meta records back to back; C-I11 NG3 holds; C-I13 the
  profile; C-I3 (the TX grid snaps NEAREST) SENDER mode never snaps, NEAREST stays the default for
  sync sources; C-I4 compliance is judged against `session.profile`; C-I12 the GI-9 engine fix.
- C-I14 (an HDMI source changing format mid-stream): stop, update, start, then
  `mtl_rtcp_set_info()`; the short gap is accepted, and G-N6 later removes it for RX.

## 15. Requirement index

"v1": met by the ported API (Phases 1–6); "later": beyond the Phase 7 design; "outside MTL": the
Node or application. Full rows: [requirements.md](requirements.md) §6 (N-REQ) and §7 (I-REQ).

### 15.1 NMOS (N-REQ-1…60)

| IDs | Requirement | Pri | Where |
|---|---|---|---|
| 1, 3, 4, 6 | raster and exact rate; sampling and depth; audio rate, depth, channels, ptime; granted per-leg port, SSRC, PT, UDP ports, MACs | MUST | v1: `video.raster`, `video.format`, `audio.*`, `mtl_session_info.leg[]` |
| 2 | colorimetry, TCS, range | MUST | Phase 7: `video.colorimetry`, `tcs`, `range` |
| 5 | media type and format | MUST | v1: `essence`; the ST 2110-41 type is unregistered (outside MTL, Q-NMOS-6) |
| 7–9 | TP, TROFF, CMAX, TSMODE, TSDELAY; MAXUDP; wire and payload bit rate | MUST | v1: `info.*`; Phase 7: `info.troffset_default`, `max_udp_bytes`, `wire_kbps`, `payload_kbps` |
| 10–13 | grandmaster and domain per port; lock, traceability, accuracy; the same with ptp4l; grandmaster change | MUST, SHOULD | v1: `time.grandmaster_id`, `time.ptp_domain`, `time.state`; Phases 1–2: `mtl_time_set_reference()`; Phase 7: `time.gm_*`, `info.ts_refclk.*{leg}`, `MTL_EVENT_GRANDMASTER` |
| 14 | port MAC and port count | MUST | Phase 7: `mtl_port_spec.mac`, `instance.port_count` |
| 15 | LLDP | MAY | later (G-N14) |
| 16, 17 | ARP on media ports; leg to interface | MUST | v1: built-in ARP, `info.leg[].port`, `mtl_port_get_spec()` |
| 18–20, 24, 35 | every leg at one instant, all or nothing; planned and applied instant; answer an immediate activation only after it applied; per-leg enable; a past time means now | MUST | Phase 2: `mtl_session_update`, `planned_tai_ns`, `status.update_*`, `MTL_EVENT_UPDATE`, `MTL_UPDATE_LEGS` |
| 21, 22, 25, 26, 30 | re-apply; cancel; `master_enable`; exist before any connection; one leg and two legs without stop | MUST | Phase 7: `MTL_UPDATE_REAPPLY`, `parts` 0, mute, reserved legs (Phase 2 meanwhile: stop and start) |
| 29, 31, 33, 34, 38 | both legs on one interface; port change while running; validate staged parameters; bound double bandwidth; SDP offsets at activation | SHOULD | Phase 7: explicit `flows[1].port`, make before break, `MTL_UPDATE_DRY_RUN`, `rx.join_lead_ns`, R options at the boundary |
| 23 | re-staging while scheduled | MUST | outside MTL: the Node answers 423 |
| 27, 28 | unicast RX with a source check; SSM, one filter per leg | MUST | v1: the `mtl_flow` rule (G-N23), `source_filter` |
| 32 | a new received format at an activation | SHOULD | later (G-N6); the A/B swap meanwhile |
| 36, 37 | resolve `auto`; the instance TAI clock | MUST | outside MTL (resolution); v1: granted values, `mtl_time_now()` |
| 39, 40 | render and parse an SDP | SHOULD | Phase 7: `mtl_sdp.h` |
| 41, 42, 44–48 | link state; RX signal, loss, redundancy; stream validity; TX errors; missing essence; PTP state per port; cheap counters | MUST, SHOULD | v1: §7 keys and events, `mtl_stat_read()`; the link monitor in Phase 2 |
| 43 | late packets and units | SHOULD | v1: `rx.pkts_stale`; Phase 7: `leg.pkts_late`, `rx.units_late_presentation` |
| 49–52, 54 | stable identity; dry run with capacity; no silent pacing fallback; reconfigure keeping identity; receive without an SDP | MUST, SHOULD | v1: `sc.name`, `mtl_session_query()`, `caps.pacing_required`, `MTL_UPDATE_MEDIA` (CREATED, STOPPED), `video.detect` |
| 53 | mute on a constraint violation | MUST | outside MTL (policy) over the Phase 7 mute |
| 55 | IS-09 domain and announce timeout | SHOULD | v1: `time.ptp_domain`; Phase 7: `time.ptp_announce_timeout` |
| 56, 57 | audio remap; ANC DID/SDID seen | MAY | later: `mtl_audio_remap()`; Phase 7: `anc.did_sdid_seen` |
| 58 | IPv6 media | MAY | outside MTL: the Node constrains to IPv4 |
| 59, 60 | ST 2022-5 FEC and RFC 3550 RTCP; port address change | MAY | RTCP and `MTL_EVENT_PORT_ADDRESS` Phase 7; FEC not adopted |

### 15.2 IPMX (I-REQ-1…76)

| IDs | Requirement | Level | Where |
|---|---|---|---|
| 1–3 | with and without a common clock; free-running internal clock; PTP when present | MUST | v1: PHC, PTP_BUILTIN; Phase 7: FREERUN, AUTO at runtime |
| 4, 5 | BMCA follower; a PTP leader | MUST, SHOULD | outside MTL: ptp4l (Q-I-7) |
| 6, 7, 44 | TR-10-1 CMAX; IPMX VRX; CBR compressed with NCM | MUST | v1: `MTL_CVIDEO_CBR`; Phase 7: the profile, `video.vtotal` |
| 8, 13, 30, 31, 36–38 | audio timing; non-baseband RTP; receiver formats; packing; interlace; audio rates and channels | MUST | v1 |
| 9–12 | async and sync sources; media clock locked to the source; first RTP from the internal clock; RTP and NTP at VSYNC | MUST | v1 for sync sources; Phase 7: `MTL_MEDIA_SENDER` |
| 14–22 | reports to port +1; Info Block; NTP format; per-essence schedule; CNAME; DSCP; tolerate other RTCP | MUST | Phase 7: `mtl_rtcp.h` (SENDER audio open, §16) |
| 23 | a sender never joins its own source | MUST | v1: TX does not join |
| 24 | inline processors keep the input's timing | MUST | Phase 7: `MTL_SUBMIT_SENDER_TIME` |
| 25, 35 | SDP with the IPMX items; SDP, NMOS and reports consistent | MUST | Phase 7: `mtl_sdp_render()`, block version, `MTL_EVENT_RTCP_INFO` (measured values as text, §16) |
| 26 | recover async timing at a baseband output | SHOULD | Phase 7: `mtl_rtcp_read()`, `rx.sender_rate_ppb`; the output clock is outside MTL |
| 27 | link offset while active | MUST if supported | Phase 7: `rx.link_offset_ns` R |
| 28, 66 | UDP port rules; address ranges and default group | MUST | outside MTL; checked under the profile |
| 29 | UDP size with extensions | MUST | v1: `session.max_udp_payload`; Phase 7: sizing for extensions |
| 32 | any resolution and rate | MUST | Phase 7: GI-7 engine fix |
| 33, 34, 39–41 | MIBs 0x0001, 0x0002, 0x0004 and their defaults; ANC with reports | MUST | v1: the essences; Phase 7: library-built MIBs |
| 42, 43 | InfoFrames on +3, with the video's RTP, before the video | MUST if used | v1: fast metadata; Phase 7: `tx.precede` (SENDER video open, §16) |
| 45, 49, 76 | MIBs 0x0003, 0x0005, 0x0008, 0x0006 per field | MUST, MAY | Phase 7: application bytes, `MTL_META_RTCP_MIB` |
| 46 | JPEG XS per RFC 9134 | MUST if used | v1: codec plugin |
| 47, 48 | VBR compressed; H.264 and H.265 packetisation | MUST if used | Phase 7: `cvideo.max_bitrate_bps`; later: H.26x (packet units meanwhile) |
| 50, 51 | FEC Profile A; tolerate FEC streams | MAY, MUST | later (GI-15); v1: flows filter by UDP port |
| 52–58 | PEP: AES-128-CTR, clear headers, CTR extensions, MAC modes, RTP_KV, both legs, MIB 0x0011 | MUST if PEP | Phase 7: `mtl_crypto.h` (privacy_version open, §16) |
| 59, 62 | PSK, KDF, ECDH; HKEP | MUST if used | outside MTL |
| 60, 61 | HDCP extension; RX watches streamCtr | MUST if HDCP | later: cipher plugin; v1 packet units; Phase 7: GI-9 |
| 63 | DHCP by default | MUST | v1: `port.dhcp`, zero `sip` |
| 64 | DSCP AF42, AF41, EF | MUST | Phase 7: the profile (EF for PTP open, §16) |
| 65 | IGMPv3 SSM and IGMPv2 | MUST | v1: SSM; Phase 7: `port.igmp_version` |
| 67 | frame-to-frame interval ≤ 2 ms | MUST | v1: pacing; Phase 7: `tx.f2f_pp_ns` |
| 68–70 | sync method from the SDP; alignment; PTP state in the description | MUST, SHOULD | v1: `rx.mediaclk` DIRECT and SENDER, `mtl_rx_align()`, `MTL_EVENT_TIME_STATE`; Phase 7: AUTO, SENDER_TIME, the Info Block |
| 71 | in-band NMOS and HKEP on the media port | MUST if both bands | v1: `port.virtio_user`, kernel backends (Q-I-9 open) |
| 72–74 | unicast with reports; one network; off-subnet unicast | MUST, MAY, SHOULD | v1: unicast, `flows[1]` optional, `mtl_port_spec.gateway`; reports Phase 7 |
| 75 | USB over IP | MAY | outside MTL |

## 16. Open items for Phase 7

**The decision.** M17 accepts or narrows this design (accept; or NMOS only, IPMX later). Its
sub-questions Q-NI-1…10 all recommend yes: `mtl_sdp.h`; `mtl_rtcp.h` (Phase 7, D-98); the
`mtl_crypto.h` header, implemented after the spike; `rtx.*`; mute over a scheduled stop; both legs on
one port; audio at a packet boundary; `session.profile` as the one switch; no IPMX sender type
unless the VSF needs a distinct TP; RX REAPPLY without a leave ([decisions.md](decisions.md)).

**Gaps the verification left** (RV; checked against the headers on 2026-10-02):

| Item | What is missing |
|---|---|
| RN-17, RN-18 SDP | `a=privacy` needs key_generator, key_version and key_id: no render input, no parse output. `a=infoframe` is not rendered. Measured values, profile, level, PAR and DID_SDID exist only as `fmtp_extra` text |
| RN-20 audio reports | a report every int(10 ms / ptime) packets falls mid-unit for SENDER audio; the NTP of the packet it precedes is undefined, and there is no `unit_samples` alignment rule |
| RN-21 privacy block | 0x0011 needs privacy_version; no option carries it or the key identity |
| RN-24 time steps | units in flight across an AUTO time step are not addressed |
| RN-26, R-1 InfoFrames | the video and InfoFrame updates are two calls with one `when`, not atomic. A SENDER video cannot be a `tx.precede` target, so an async HDMI source cannot carry InfoFrames: allow it, or state it unsupported |
| RN-29 ts-refclk | `ts_refclk[64]` holds the 64-byte wire field with no room for the NUL: say NUL-padded, or limit to 63 |
| RN-30 profile table | EF for PTP, and DHCP DNS and domain for in-band discovery, are missing |
| RN-9, RN-15 | the call returns no seq (the Node serialises updates); PAR stays in `fmtp_extra` |
| RN §5 jxsv | ST 2110-22 `sampling` and `depth` have no source when `cvideo.app_format` is 0 (the application gives the codestream); they come only through `fmtp_extra` |
| RN §5 parse TP | parse does not map `TP=` to `video.sender_type`, so an RX cannot check the sender type it receives **[inferred]** |
| RN §5 ANC | ST 2110-40 `DID_SDID` and `VPID_Code` render and parse only as `fmtp_extra` text; the interlace field order (`interlaced_tff` / `_bff`) has no field in `mtl_raster`, so RX detection cannot report it |

Since the verification, the headers resolved: render while muted (the legs as configured),
`mtl_index_at()` on SENDER sessions (`-MTL_EINVAL`) and their `media_index` (k), a `when` before T0
in ARMED (means T0), and colorimetry at an update's boundary.

**Spikes and questions for others:**

- PEP cost spike: cycles per byte (ipsec-mb or OpenSSL) on the target CPUs.
- Label `MTL_TIME_SOURCE_PTP_BUILTIN` non-compliant under the IPMX profile until it runs BMCA.
- Ask the VSF whether InfoFrame and FEC streams need reports (Q-I-2), and the TR-10-13 authors
  whether a partial slice consumes a counter (Q-I-12). Verify in-band control through
  `port.virtio_user` with an IPMX controller (Q-I-9).
- Link offset without a shared clock: map sender time to local time by the minimum (arrival −
  sender time) observed (Q-I-6). Sample `MTL_LINK_OFFSET_AUTO` at activation and hold it, so
  presentation latency does not wander.
- Validate a reserved leg's port when it is enabled, not at create (a one-port instance).
- ST 2110-41 has no NMOS media type: publish `urn:x-nmos:format:data` with a vendor value and flag
  it to AMWA (Q-NMOS-6). `interlace_mode` field order is the application's.
