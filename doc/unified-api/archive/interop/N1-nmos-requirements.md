# N1 — What AMWA NMOS needs from the unified MTL API

| | |
|---|---|
| Status | Requirements study for maintainer review, 2026-10-01. Nothing is implemented; the API is the revision-4 design ([REVISION-4.md](../REVISION-4.md), headers in [sketch/](../../sketch/include/mtl/experimental/)). The gaps' proposals are the study's; what was adopted, and in which form, is [17 §6](../17-nmos-and-ipmx.md) |
| Question | "We also want to work with NMOS and IPMX; make sure the proposed API suits them." This document covers NMOS. A companion IPMX document in this folder covers IPMX; §2.8 lists where IPMX builds on NMOS so the two connect |
| Scope | What an NMOS Node (Devices, Senders, Receivers) built on MTL needs from the *transport library*. The NMOS HTTP/WebSocket APIs, mDNS, registration, JSON and UUIDs are the Node's job and stay out of MTL |
| Method | The AMWA repositories were cloned on 2026-10-01 (IS-04 `v1.3.x`, IS-05 `v1.1.x` and `v1.2.x`, IS-08, IS-09, IS-11, IS-12, MS-05-02, BCP-002-01/02, BCP-003-01, BCP-004-01/02, BCP-005-01, BCP-006-01..04, BCP-007-01, BCP-008-01/02, the NMOS Parameter Registers, the Control Feature Sets). SMPTE ST 2110-10/-20/-21:2022 were read from the PDFs in the repository root |
| Labels | **[verified]**: I read the text (spec page, JSON schema or RAML). **[inferred]**: from knowledge of the spec or a summary, not re-read for this study |
| Revision-4 symbols | `mtl.h` unless another header is named; stats keys are the [08 §R4](../08-observability.md) catalogue |

## 1. Summary

An NMOS Node needs four things from its transport: (a) the **values** it must publish in IS-04 and in the SDP
(format, raster, rate, colorimetry, sender type, TROFF, CMAX, bit rate, ports, MACs, PTP grandmaster, lock state);
(b) **IS-05 activation**: change both ST 2022-7 legs at one TAI instant or now, all or nothing, enable and disable
legs and the whole sender or receiver, and say when the change actually took effect; (c) **monitoring** for BCP-008
(link, connection or transmission, synchronisation, stream or essence health, lost, late and error counters);
(d) **capability answers** for BCP-004 and IS-11 constraint sets (can this session be created, with what result).

Revision 4 already carries most of it. `mtl_session_update()` with `MTL_UPDATE_FLOWS | MTL_UPDATE_LEGS` and
`struct mtl_when` is exactly the IS-05 shape (one call, both legs, `NOW` or `AT_TAI`, all or nothing, a pending
update replaced by the next). `mtl_session_info.leg[]`, the `info.*` keys and `mtl_port_get_spec()` give the
granted SSRC, ports, MACs, TP, TROFF, CMAX and TSMODE. The stats registry, `MTL_EVENT_LEG_STATE`, `PORT_LINK`,
`TIME_STATE`, `RX_SIGNAL` and `RX_FORMAT` cover BCP-008 almost line by line. `mtl_session_query()` answers
"could this be created" for capability sets.

The gaps (§4) are small in code and large in consequence:

1. **Activation outcome** (G-N1): IS-05 must return `activation_time` (the instant it *will* or *did* switch) and
   answer an immediate activation only after it is applied. The update call returns when the change is posted;
   nothing reports the planned or applied instant. Proposed: `mtl_session_get_update()` and `MTL_EVENT_UPDATE`.
2. **`master_enable = false` and unconfigured legs** (G-N3, G-N4): the design forbids disabling the last leg and
   requires an address on every leg, so a Node can neither mute a sender at a scheduled instant nor create a
   receiver at boot before it has a stream. Proposed: all legs may be disabled (mute), and a disabled leg may be
   unaddressed.
3. **Re-activation and cancel** (G-N2, G-N8): IS-05 requires an identical re-activation to re-apply (IGMP leave and
   join) and allows a scheduled activation to be cancelled; the design treats an identical leg as "unchanged" and
   has no cancel. Proposed: `MTL_UPDATE_REAPPLY`, `MTL_UPDATE_CANCEL`.
4. **Colorimetry, TCS and RANGE** (G-N10): mandatory in every ST 2110-20/-22 SDP and in the IS-04 Flow, absent from
   `mtl_video_config`. Proposed: three typed fields.
5. **Time reference metadata** (G-N11, G-N12): `ts-refclk` needs the grandmaster *and* "traceable"; IS-04 needs
   `locked` and `traceable`; BCP-008 needs grandmaster changes. With ptp4l disciplining the PHC, MTL knows none of
   them. Proposed: a few `time.*` keys, a GM-change event, and `mtl_time_set_reference()` for external PTP.
6. **Port identity and wire bit rate** (G-N13, G-N15): IS-04 `interfaces[].port_id` must be the MAC, and BCP-006-01
   and ST 2110-22 need the bit rate including RTP overhead. Proposed: `mac[6]` in the port spec, `info.wire_kbps`.
7. **An SDP helper** (G-N17): one optional `mtl_sdp.h` (render from a session, parse into a config) removes the
   largest amount of duplicated, error-prone Node code. It was already proposed as Q-MODE-6.

Three design rules conflict with NMOS behaviour (§6): port changes and adding/removing `flows[1]` need STOPPED, media
changes need STOPPED (so a receiver activation with a new SDP format cannot be scheduled), and per-session counters
cannot be reset (BCP-008 resets on activation; a Node baseline solves it). None needs a redesign.

## 2. The NMOS specifications and what each needs from the transport

### 2.1 Map

| Spec (version read) | URL | What it needs from MTL | Weight |
|---|---|---|---|
| IS-04 Discovery and Registration (v1.3.x) [verified] | <https://specs.amwa.tv/is-04/releases/v1.3.3/> | Flow, Sender, Receiver and Node attributes only the transport knows; change notifications to bump `version` | high |
| IS-05 Connection Management (v1.1, v1.2.0; RTP parts identical) [verified] | <https://specs.amwa.tv/is-05/releases/v1.2.0/> | per-leg RTP transport parameters, `master_enable`, immediate and scheduled activation, `activation_time`, SDP | high |
| IS-07 Event and Tally (v1.0.1) [inferred] | <https://specs.amwa.tv/is-07/> | nothing: events go over WebSocket/MQTT, not RTP; a Node may derive tally from session state | none |
| IS-08 Audio Channel Mapping (v1.0.1) [verified, behaviour] | <https://specs.amwa.tv/is-08/releases/v1.0.1/docs/Behaviour.html> | channel routing inputs → outputs, possibly scheduled; MTL only needs to let the app build units per media index | low |
| IS-09 System Parameters (v1.0.0) [verified, schema] | <https://specs.amwa.tv/is-09/releases/v1.0.0/> | `ptp.domain_number`, `ptp.announce_receipt_timeout` at Node start-up; syslog is the Node's | low |
| IS-10 Authorization, BCP-003-01 TLS [inferred] | <https://specs.amwa.tv/is-10/>, <https://specs.amwa.tv/bcp-003-01/> | nothing (control-plane security) | none |
| IS-11 Stream Compatibility Management (v1.0.0) [verified, server behaviour] | <https://specs.amwa.tv/is-11/releases/v1.0.0/docs/Behaviour_-_Server_Side.html> | sender reconfiguration to satisfy active constraints, sender inactive on violation, receiver stream compliance | medium |
| IS-12 Control Protocol + MS-05-02 Framework (v1.0.x) [verified, monitoring models] | <https://specs.amwa.tv/is-12/>, <https://specs.amwa.tv/ms-05-02/> | the device model is the Node's; its monitor objects read MTL state (BCP-008) | via BCP-008 |
| IS-13 Annotation (work in progress) [inferred] | <https://specs.amwa.tv/is-13/> | nothing (writable labels) | none |
| BCP-002-01/02 Grouping, asset info [inferred] | <https://specs.amwa.tv/bcp-002-01/> | nothing (tags) | none |
| BCP-004-01 Receiver Capabilities (v1.0.0) [verified] | <https://specs.amwa.tv/bcp-004-01/releases/v1.0.0/docs/Receiver_Capabilities.html> | which streams a receiver can consume, as constraint sets; a precise "can MTL receive X" | medium |
| BCP-004-02 Sender Capabilities (v1.0.0) [verified] | <https://specs.amwa.tv/bcp-004-02/releases/v1.0.0/docs/Sender_Capabilities.html> | "a Sender MUST not produce a stream that is incompatible with the advertised Sender Capabilities" | medium |
| BCP-005-01 EDID to Receiver Caps (v1.0.0) [verified, title] | <https://specs.amwa.tv/bcp-005-01/> | nothing (EDID is HDMI/DP side; IPMX). PTP clock info is not a BCP: it is IS-04 `clocks` (§2.2) | none |
| BCP-006-01 JPEG XS (v1.0.0) [verified] | <https://specs.amwa.tv/bcp-006-01/releases/v1.0.0/docs/NMOS_With_JPEG_XS.html> | profile, level, sublevel, colour, bit rate with and without RTP overhead, packetisation mode, TP | high for ST 2110-22 |
| BCP-006-02/-03 H.264/H.265 (work in progress), BCP-006-04 MPEG-TS (v1.0.0) [inferred] | <https://specs.amwa.tv/bcp-006-04/> | as BCP-006-01 for their codecs; MPEG-TS over RTP fits the generic RTP essence | low |
| BCP-007-01 NDI (work in progress) [verified, title] | <https://github.com/AMWA-TV/bcp-007-01> | not applicable: NDI is not an RTP transport MTL carries | none |
| BCP-008-01/02 Receiver and Sender Status Monitoring (v1.0.0) [verified] | <https://specs.amwa.tv/bcp-008-01/releases/v1.0.0/docs/Overview.html>, <https://specs.amwa.tv/bcp-008-02/releases/v1.0.0/docs/Overview.html> | link, connection/transmission, synchronisation, stream/essence status, lost/late/error counters | high |
| NMOS Parameter Registers (main) [verified] | <https://specs.amwa.tv/nmos-parameter-registers/> | the value vocabularies: capabilities, flow and sender attributes, `ext_` transport parameters | reference |

### 2.2 IS-04: the attributes a Node publishes that only the transport knows

Schemas: <https://specs.amwa.tv/is-04/releases/v1.3.3/APIs/schemas/> [verified]; behaviour: [Behaviour: Nodes](https://specs.amwa.tv/is-04/releases/v1.3.3/docs/Behaviour_-_Nodes.html) [verified].

| Resource.attribute | Rule | Source of the value in revision 4 |
|---|---|---|
| Flow `grain_rate` | frame rate for video (`flow_core.json`) | `sc.video.raster.fps` (exact rational) |
| Flow (video) `frame_width`, `frame_height`, `interlace_mode` (`progressive`, `interlaced_tff`, `interlaced_bff`, `interlaced_psf`) | required: width, height, colorspace (`flow_video.json`) | `raster.width/height`, `raster.scan` (`MTL_PROGRESSIVE/INTERLACED/PSF`); field order is not in the config (app) |
| Flow (video) `colorspace`, `transfer_characteristic` | values of the ST 2110-20 `colorimetry` and `TCS` parameters (register) | **gap G-N10** |
| Flow (video/raw) `components[]` (name, width, height, bit_depth) | required (`flow_video_raw.json`) | derivable from `video.format` + raster (`mtl_format_names().sdp`, `mtl_format_pgroup`) |
| Flow `media_type` | `video/raw`, `video/jxsv`, `video/H264`, `audio/L24`, `audio/L16`, `video/smpte291` (registers; IS-04 schemas) | from `sc.essence` and the format; ST 2110-41 has **no registered media type** yet [verified by search of the registers] |
| Flow (coded) `profile`, `level`, `sublevel`, `bit_rate` (register) | BCP-006-01 MUST | profile/level/sublevel: app or codec plugin; codestream rate: `info.codestream_bytes` × rate; see G-N15 |
| Flow (audio) `sample_rate`, `bit_depth`; Source `channels[]` | required | `audio.sample_rate`, `audio.format`, `audio.channels`; channel labels are the app's |
| Flow (data) `DID_SDID[]` | optional (`flow_sdianc_data.json`) | app on TX; RX observed set: G-N19 (MAY) |
| Sender `transport` | `urn:x-nmos:transport:rtp[.mcast/.ucast]` (Transports register) | from the destination address |
| Sender `manifest_href` | SHOULD serve the SDP; `version` MUST change when its content changes; MAY 404 while inactive | SDP from §2.6; content changes on activation, GM change, format change |
| Sender/Receiver `interface_bindings` | one interface per leg; the same interface listed twice when both legs use it | `info.leg[i].port` → the Node's interface name |
| Sender/Receiver `subscription.active` | MUST be true exactly when configured to send/receive | session RUNNING and not muted (G-N3) |
| Sender `bit_rate`, `st2110_21_sender_type`, `packet_transmission_mode` (register) | BCP-006-01 MUST for -22 | **G-N15**; `info.sender_type`; option `cvideo.pack` |
| Receiver `caps.media_types`, `caps.constraint_sets` (BCP-004-01) | MUST reflect capability changes by bumping `caps.version` | `mtl_session_query()` per candidate set (§2.5) |
| Node `clocks[]`: `ptp` {`name`, `ref_type`, `traceable`, `version` = `IEEE1588-2008`, `gmid`, `locked`} or `internal` | all required (`clock_ptp.json`) | `time.grandmaster_id`, `time.state` (LOCKED) per port; `traceable`: **G-N11** |
| Node `interfaces[]`: `name`, `port_id` (MUST be a MAC), `chassis_id` (LLDP, may be null), `attached_network_device` (LLDP received) | "require that interfaces implement ARP at a minimum, and ideally LLDP" (`node.json`) | name from `mtl_port_spec.name`; MAC: **G-N13**; LLDP: **G-N14** (MTL answers ARP today) |
| every resource `version` | a TAI `<s>:<ns>` updated on every change; IS-05: MUST increment on every activation, even of the same parameters [verified] | `mtl_time_now()`; the trigger is G-N1's applied event |

### 2.3 IS-05: connection management

Sources [verified]: `sender_transport_params_rtp.json`, `receiver_transport_params_rtp.json`, `activation-*.json`,
`ConnectionAPI.raml`, [Behaviour](https://specs.amwa.tv/is-05/releases/v1.2.0/docs/Behaviour.html),
[Behaviour: RTP Transport Type](https://specs.amwa.tv/is-05/releases/v1.2.0/docs/Behaviour_-_RTP_Transport_Type.html),
[Interoperability: IS-04](https://specs.amwa.tv/is-05/releases/v1.2.0/docs/Interoperability_-_IS-04.html).

**Parameter sets** (RTP Behaviour §Parameter Sets): endpoints MUST support the core set and MAY support multicast,
FEC and RTCP sets, each all or nothing. Sender core: `source_ip`, `destination_ip`, `source_port`,
`destination_port`, `rtp_enabled`. Receiver core: `source_ip`, `interface_ip`, `destination_port`, `rtp_enabled`;
plus `multicast_ip` if it can do multicast. One parameter object per leg: one entry without ST 2022-7, two with it.

| IS-05 parameter (per leg) | `auto` default (schema) | Revision-4 mapping |
|---|---|---|
| TX `source_ip` | "the sender should establish for itself which interface" | `flows[i].port` (the port owns one IP: `mtl_port_get_spec().sip`) |
| TX `destination_ip` | sender selects a multicast address (MADCAP, ZMAAP, ...) | `flows[i].ip`; allocation is the Node's |
| TX `source_port` | 5004 | `flows[i].udp_src_port` (MTL's own default is "= udp_port"; the Node resolves `auto` explicitly); granted `info.leg[i].udp_src_port` |
| TX `destination_port` | 5004 | `flows[i].udp_port` |
| TX/RX `rtp_enabled` | — | bit i of `legs_disabled` (`MTL_UPDATE_LEGS`) |
| RX `multicast_ip` (null = unicast) | — | `flows[i].ip` = the group; unicast: see G-N23 |
| RX `source_ip` (unicast source, or SSM filter; null = any) | — | `flows[i].source_filter` |
| RX `interface_ip` | multicast: receiver chooses (routing); unicast: undefined, controller supplies | `flows[i].port`; the Node maps an address to a port with `mtl_port_find()` |
| RX `destination_port` | 5004 | `flows[i].udp_port` |
| `fec_*` (ST 2022-5), `rtcp_*` | — | not supported: the Node omits both sets (G-N20, G-N21) |
| `ext_*` (registered externally) | — | IPMX `ext_link_offset_delay` → option `rx.link_offset_ns`; `ext_privacy_*` → IPMX document |

**Activation** [verified, RAML and schemas]:

- `activate_immediate`: "the API should only return a response once the new transport parameters have been applied
  to the underlying sender", with `activation_time` = "the time the activation actually occurred as an absolute TAI
  timestamp" (200).
- `activate_scheduled_absolute` / `_relative`: "when internal clock >= requested_time" (or receipt + requested). The
  202 response carries `activation_time` = "the absolute TAI time the parameters will actually transition ... may
  differ ... for example, at a frame boundary or end of GOP".
- A PATCH with no activation, or one that cancels a scheduled activation, returns 200.
- "On activation all instances of `auto` must be resolved into the actual values ... If there is an error condition
  that means `auto` cannot be resolved, the active transport parameters must not change, and the underlying sender
  must continue as before." This is the all-or-nothing rule of `mtl_session_update()`.
- "If an explicit activation is performed ... the API MUST request a re-application of settings ... whether the
  setting have changed or not ... an explicit IGMP leave and join" (Behaviour §Re-Activating).
- Scheduled activations exist to synchronise salvos across devices; they are "not intended ... for scheduling
  activations far in the future" (Behaviour §Scheduled Activations). On an error between scheduling and activation,
  `/active` SHOULD reflect reality, e.g. `master_enable = false` if the sender stopped.
- `master_enable = false`: "senders should not transmit any media streams"; `rtp_enabled` disables individual legs.
- Loss of packets MUST NOT be reported through `/active` (Behaviour §Connection Status): that is BCP-008's job.

**ST 2022-7 rules** [verified]: a request MUST carry as many legs as the constraints; an unchanged leg is `{}`. A
two-leg receiver given a one-leg SDP SHOULD set leg 2 `rtp_enabled = false`; a one-leg receiver given a 2022-7 SDP
SHOULD join the first leg. The first SDP stream is path 1. RFC 7104 forms: separate destination addresses (two
m-lines, `a=group:DUP`), separate source addresses (one m-line, two sources, `a=ssrc-group:DUP`), temporal
redundancy (same source and destination, `duplication-delay`; ST 2110-10 §8.5 forbids identical source *and*
destination, so a Node constrains it out).

**Transport file on receivers** [verified]: with an SDP in the PATCH the receiver SHOULD use its media information;
where SDP and `transport_params` in the same PATCH contradict, the parameters win.

**Constraints** [verified, RAML]: per leg; `auto` must be supported where the schema allows it; senders and
receivers SHOULD offer an enum of interface addresses. MTL-derived constraints: interfaces = ports (one IPv4 address
each, IPv6 reserved), legs bound to distinct ports by default, no FEC, no RTCP.

### 2.4 IS-08, IS-09, IS-11, IS-12

- **IS-08** [verified, behaviour]: maps input channels to output channels, with `reordering` and `block_size`
  capability flags and activations that can be scheduled. The transport only sends the channels it is given; a Node
  remaps in its own copy into the TX unit and switches maps at the media index of the activation instant
  (`mtl_index_at()`). A remap in MTL's copy path would save a copy in RX→TX gateways (G-N22, MAY).
- **IS-09** [verified, `global.json`]: required `ptp.domain_number` (0–127) and `ptp.announce_receipt_timeout`
  (2–10), read at Node start-up. Built-in PTP: option `time.ptp_domain` (C); announce timeout: no option (G-N18).
  With ptp4l (`MTL_TIME_SOURCE_PHC`) both are ptp4l's configuration.
- **IS-11** [verified, server side]: a Sender's active constraints start empty; when violated, "the Sender MUST become
  inactive. An inactive Sender in this state MUST NOT allow activations". Sender states `unconstrained`,
  `constrained`, `active_constraints_violation`, `no_essence`, `awaiting_essence`; receiver states
  `compliant_stream`, `non_compliant_stream` (SHOULD become inactive), `unknown` (no transport file). Transport
  needs: reconfigure a sender to a new format (`MTL_UPDATE_MEDIA` in STOPPED), mute it (G-N3), detect what a
  receiver actually gets (`MTL_DETECT_ON`, `MTL_EVENT_RX_FORMAT`, `rx.detected.*`). EDID is the Node's (IPMX).
- **IS-12 / MS-05-02** [verified, Control Feature Sets `monitoring/models`]: `NcStatusMonitor` (`overallStatus`,
  `statusReportingDelay` default 3 s), `NcReceiverMonitor` and `NcSenderMonitor` are Node objects; their values come
  from MTL (§5.5).

### 2.5 BCP-004-01/-02 and BCP-006-01

- **BCP-004-01** [verified]: a receiver lists `constraint_sets` (any one satisfied = compatible) over the Capabilities
  register: `media_type`, `grain_rate`, `frame_width`, `frame_height`, `interlace_mode`, `colorspace`,
  `transfer_characteristic`, `color_sampling`, `component_depth`, `channel_count`, `sample_rate`, `sample_depth`,
  `bit_rate`, `profile`, `level`, `sublevel`, `event_type` and transport caps `packet_time`, `max_packet_time`,
  `st2110_21_sender_type`, `packet_transmission_mode`, `bit_rate`, `hkep`, `privacy`, `usb_class` [verified,
  register]. It MUST update `caps.version` when its capabilities change (e.g. capacity). The transport answer is a dry
  run per candidate set: `mtl_session_query(mt, &sc, MTL_QUERY_CHECK_CAPACITY, ...)`. No enumeration API is needed;
  the formats are a closed enum (`mtl_format_names()` iterates them).
- **BCP-004-02** [verified]: the same for senders, plus "A Sender MUST not produce a stream that is incompatible with
  the advertised Sender Capabilities". For MTL this means a granted pacing class and sender type must not degrade
  silently: the Node sets `caps.pacing_required` or re-publishes on `MTL_EVENT_PACING_CHANGED`.
- **BCP-006-01** [verified]: Flow MUST carry `video/jxsv`, `components`, `profile`, `level`, `sublevel`, `bit_rate`
  (codestream); Sender MUST carry `bit_rate` "including the RTP transport overhead", `packet_transmission_mode` for
  slice mode, `st2110_21_sender_type` if ST 2110-22 compliant; SDP per RFC 9134 with `sampling`, `depth`, `width`,
  `height`, `exactframerate`, `colorimetry`, `TCS` in all cases, and `interlace`/`segmented` as appropriate.

### 2.6 SDP: what a Node writes for senders and reads for receivers

ST 2110-10 §8.1 [verified]: "Devices which contain one or more Senders shall construct one SDP session description ...
for each RTP stream (or redundant pair of streams) ... made available through a management API"; receivers "shall
provide a capability to ingest and act upon an SDP". IS-05 serves it at the sender `/transportfile` and accepts it in
a receiver's `transport_file`.

| SDP element | Rule | Value from revision 4 |
|---|---|---|
| `c=`, `m=<media> <port> RTP/AVP <pt>` per leg; `a=source-filter: incl` | RFC 4566, RFC 4570; -10 §8.4 SHOULD [verified] | `flows[i].ip`, `udp_port`, `info.leg[i].payload_type`; source = port `sip` |
| `a=group:DUP <mid1> <mid2>`, `a=mid:` | -10 §8.5 [verified]; RFC 7104 | two legs → two m-lines (separate destination addresses) |
| `a=ts-refclk:ptp=IEEE1588-2008:<gmid>:<domain>` or `:traceable`, or `localmac=<mac>` | -10 §8.2 MUST; traceable only when the GM's `timeTraceable` is set *and* `clockAccuracy` ≤ 250 ns [verified] | `info.ts_refclk.*`, `time.grandmaster_id`, `time.ptp_domain`; traceable and accuracy: **G-N11**; localmac: `info.leg[i].src_mac` |
| `a=mediaclk:direct=0` (or `sender`) | -10 §8.3 MUST; offset zero [verified] | implied for TX (RTP = floor(M × rate)); RX: options `rx.mediaclk`, `rx.rtp_offset` |
| `TSMODE`, `TSDELAY` | -10 §8.7 SHOULD [verified] | `info.tsmode`, `info.tsdelay_ns` |
| `MAXUDP` | -10 §8.6: required above the standard UDP size limit [verified] | option `session.max_udp_payload`; granted value: **G-N15** |
| video `a=rtpmap:<pt> raw/90000`; fmtp `sampling`, `depth`, `width`, `height`, `exactframerate`, `colorimetry`, `PM`, `SSN` | -20 §7.1–7.2 required [verified] | `video.format` → `mtl_format_names().sdp`; raster; `packing` (GPM and GPM_SL → `2110GPM`, BPM → `2110BPM`); colorimetry **G-N10**; SSN from colorimetry/TCS |
| fmtp `interlace`, `segmented`, `TCS`, `RANGE`, `PAR` | -20 §7.3 when not default [verified] | `raster.scan`; TCS, RANGE, PAR **G-N10** |
| fmtp `TP`, `TROFF`, `CMAX` | -21 §8.1 TP required; TROFF required when ≠ TRODEFAULT (§6.2); CMAX optional [verified] | `info.sender_type`, `info.troffset_ns`, `info.cmax`; "is it the default?": G-N15 |
| ST 2110-22 `jxsv/90000`, `packetmode`, `transmode`, `profile`, `level`, `sublevel`, `b=AS` | RFC 9134, BCP-006-01 [verified for BCP-006-01]; `b=AS` [inferred] | `cvideo.*`, option `cvideo.pack`; profile/level/sublevel app or plugin; `b=AS` **G-N15** |
| audio `L24/48000/<ch>`, `L16`, `AM824`; `a=ptime`; `channel-order=SMPTE2110.(...)` | ST 2110-30/-31 [inferred] | `audio.format`, `sample_rate`, `channels`, `samples_per_packet`; channel order app |
| ANC `smpte291/90000`; `DID_SDID`, `VPID_Code`, `exactframerate`, `TM`, `SSN` | ST 2110-40:2023 [verified in [research/12 §5](../research/12-st2110-timing-standards.md)] | `anc.video.fps` (0 until start when taken from a start), `info.anc_tm`; DID/SDID app |
| fast metadata (ST 2110-41) | rate and DIT signalled in SDP [inferred] | `fastmeta.*`; no NMOS media type yet (Q-NMOS-6) |
| ST 2022-6 `SMPTE2022-6/27000000` | [inferred] | `rtp.encoding`, `rtp.clock_rate` |

Receivers parse the same lines into `flows[]` (leg per m-line or per source of an `ssrc-group:DUP`), the payload
type check, the source filter, the essence member, `rx.rtp_offset` and `rx.mediaclk`.

### 2.7 BCP-008-01/-02: monitoring

[verified] Both BCPs require IS-12 and MS-05-02. Domains and states: `linkStatus` (AllUp, SomeDown, AllDown), receiver
`connectionStatus` and sender `transmissionStatus` (Inactive, Healthy, PartiallyHealthy, Unhealthy),
`externalSynchronizationStatus` (NotUsed, Healthy, PartiallyHealthy, Unhealthy) with `synchronizationSourceId`,
receiver `streamStatus` and sender `essenceStatus`; transition counters; methods `GetLostPacketCounters`,
`GetLatePacketCounters` (receiver), `GetTransmissionErrorCounters` (sender); counters and messages reset on every
activation by default (`autoResetCountersAndMessages`); a 3 s `statusReportingDelay` hysteresis towards healthier
states, none towards worse; deactivation goes straight to Inactive. "Late packets are packets that arrived but
arrived too late to be usable by presentation time"; implementations that cannot count individual late packets
"MUST at the very least increment every time the presentation is affected". A sync source change MUST cause a
temporary PartiallyHealthy. §5.5 maps every property.

### 2.8 Where IPMX extends NMOS (link to the IPMX document)

| IPMX addition on top of NMOS | NMOS hook it uses | Transport consequence |
|---|---|---|
| link offset as a controllable receiver attribute (VSF TR-10-1 §11.2, cited [verified] in [06 §11.4](../06-timing-pacing-and-sync.md)) | IS-05 `ext_link_offset_delay` (µs; `auto` = the minimum achievable) [verified, register] | option `rx.link_offset_ns` must be settable at activation, and MTL must report the minimum achievable value |
| privacy encryption (TR-10-13) [inferred] | `ext_privacy_*` transport parameters, Sender attribute `privacy`, cap `urn:x-nmos:cap:transport:privacy` [verified, registers] | payload encryption inside the transport: an IPMX-document gap |
| HDCP key exchange (TR-10-5) [inferred] | Sender attribute `hkep`, cap `transport:hkep` [verified, registers] | same |
| asynchronous senders, PTP optional, RTCP sender reports [inferred] | SDP `a=mediaclk:sender`, IS-05 `rtcp_*` parameter set | RFC 3550 RTCP SR/RR (G-N20) becomes a MUST for IPMX |
| EDID and stream compatibility (IS-11, BCP-005-01) [verified, titles] | IS-11 | as §2.4; no further transport need |
| USB (TR-10-14) [inferred] | Source `usb_devices`, cap `usb_class` [verified, registers] | out of MTL's scope unless carried as a generic RTP essence |

## 3. Requirements

Priority is what MTL needs for an NMOS Node to be conformant: MUST (the Node cannot meet a normative NMOS or SMPTE
rule without it), SHOULD (the Node can work around it at a cost), MAY. Coverage names the revision-4 symbol, or GAP
with the §4 entry.

| ID | Requirement | Source | Pri | Revision-4 coverage |
|---|---|---|---|---|
| N-REQ-1 | Report frame width, height, scan and exact rational rate of every video session | IS-04 `flow_video.json`, `flow_core.json` | MUST | `mtl_session_get_config()`: `video.raster` |
| N-REQ-2 | Carry and report colorimetry, transfer characteristic and range | IS-04 Flow `colorspace` (required), ST 2110-20 §7.2/7.3, BCP-006-01 | MUST | GAP G-N10 |
| N-REQ-3 | Report sampling and bit depth (components) of the transport format | IS-04 `flow_video_raw.json`, -20 §7.4 | MUST | `video.format`; `mtl_format_names()`, `mtl_format_pgroup()` |
| N-REQ-4 | Report audio sample rate, bit depth, channel count, packet time | IS-04 `flow_audio*.json`; BCP-004 `packet_time` | MUST | `sc.audio.*` |
| N-REQ-5 | Report the media type and format of each essence | IS-04 Flow `media_type` | MUST | `sc.essence` + format; ST 2110-41 type open (Q-NMOS-6) |
| N-REQ-6 | Report the granted per-leg port, SSRC, PT, UDP source and destination ports, source and destination MACs | IS-05 `/active`, SDP | MUST | `mtl_session_info.leg[]` |
| N-REQ-7 | Report TP, TROFF (and whether it is TRODEFAULT), CMAX, TSMODE, TSDELAY | ST 2110-21 §6.2/§8, -10 §8.7 | MUST | `info.sender_type`, `info.troffset_ns`, `info.cmax`, `info.tsmode`, `info.tsdelay_ns`; default flag GAP G-N15 |
| N-REQ-8 | Report the maximum UDP datagram size in use | ST 2110-10 §8.6 (MAXUDP) | MUST above 1460 B | GAP G-N15 (option is an input only) |
| N-REQ-9 | Report bit rate including RTP/UDP/IP overhead, and payload bit rate | BCP-006-01 Sender/Flow `bit_rate`; `b=AS` [inferred] | MUST for -22 | GAP G-N15 (`mtl_video_bandwidth()` covers raw video only) |
| N-REQ-10 | Report the PTP grandmaster identity and domain in use, per port | ST 2110-10 §8.2; IS-04 `clock_ptp.json` `gmid` | MUST | `time.grandmaster_id`, `time.ptp_domain` (port keys); `info.ts_refclk.*` |
| N-REQ-11 | Report lock state and traceability (GM `timeTraceable`, `clockAccuracy`) | IS-04 `locked`, `traceable`; -10 §8.2 traceable form | MUST | `time.state`; traceable/accuracy GAP G-N11 |
| N-REQ-12 | Report time metadata also when ptp4l/phc2sys discipline the PHC | -10 §8.2 (any PTP setup) | MUST | GAP G-N11 (`mtl_time_set_reference()`) |
| N-REQ-13 | Signal grandmaster changes | BCP-008 `synchronizationSourceId`; SDP and Sender `version` change | SHOULD | GAP G-N12 (polling `time.grandmaster_id` works) |
| N-REQ-14 | Report each port's MAC address and the port count | IS-04 `interfaces[].port_id` MUST be a MAC | MUST | GAP G-N13 |
| N-REQ-15 | Send LLDP and report the LLDP neighbour on DPDK-owned ports | IS-04 `interfaces` "ideally LLDP"; `attached_network_device` | MAY | GAP G-N14 |
| N-REQ-16 | Answer ARP on media ports | IS-04 `node.json` "ARP at a minimum" | MUST | built-in ARP (today's behaviour) |
| N-REQ-17 | Map a leg to its interface | IS-04 `interface_bindings` | MUST | `info.leg[i].port`, `mtl_port_get_spec()` |
| N-REQ-18 | Change destinations, sources, ports and PT of every leg at one TAI instant or now, all or nothing | IS-05 activation; RAML `/active` "must not change" on error | MUST | `mtl_session_update(MTL_UPDATE_FLOWS, &when)`; ex13 |
| N-REQ-19 | Report the instant an activation will take effect (scheduled) and did take effect (immediate) | IS-05 RAML 200/202 `activation_time` | MUST | GAP G-N1 |
| N-REQ-20 | Let the Node answer an immediate activation only after it is applied | IS-05 RAML 200 | MUST | GAP G-N1 (update returns when posted) |
| N-REQ-21 | Re-apply identical parameters on re-activation (IGMP leave and join) | IS-05 Behaviour §Re-Activating | MUST | GAP G-N2 (09 §7.1: an identical leg is unchanged) |
| N-REQ-22 | Cancel a pending scheduled activation | IS-05 RAML 200 "an activation has been cancelled" | MUST | GAP G-N8 (replace-only) |
| N-REQ-23 | Replace a pending scheduled activation | IS-05 (re-staging) | MUST | `mtl_session_update()` replaces the pending one (09 §7.1) |
| N-REQ-24 | Enable and disable each leg, also at a scheduled instant and together with a flow change | IS-05 `rtp_enabled` | MUST | `MTL_UPDATE_LEGS` (+ `MTL_UPDATE_FLOWS`, one `when`) |
| N-REQ-25 | Stop and resume transmission or reception as a whole (`master_enable`), immediately or at a scheduled instant | IS-05 `master_enable` | MUST | partly: `mtl_session_stop()` has no `when`; last leg cannot be disabled; GAP G-N3 |
| N-REQ-26 | Exist before any connection: a receiver with no address, a sender with no destination yet | IS-04 resources at boot; IS-05 null `multicast_ip` | MUST | GAP G-N4 (`flows[].ip` required) |
| N-REQ-27 | Receive unicast (no group) and filter on the unicast source | IS-05 receiver `multicast_ip = null`, `source_ip` | MUST | `mtl_flow.ip` "group or unicast"; rule undefined, GAP G-N23 |
| N-REQ-28 | Receive SSM with one source filter per leg; send IGMPv3 | IS-05 `source_ip`; RFC 4570 | MUST | `mtl_flow.source_filter` (must filter on DPDK, 09 §1.1 #1239) |
| N-REQ-29 | Two legs on one interface (interface listed twice) | IS-04 `interface_bindings`; RFC 7104 separate source/destination | SHOULD | unclear: `flows[i].port = 0` means "port i"; Q-NMOS-3 |
| N-REQ-30 | Accept a one-leg configuration on a two-leg session and back, without stop | IS-05 2022-7 rules | MUST | `legs_disabled`, if G-N4 lets the unused leg be unaddressed |
| N-REQ-31 | Change the interface (port) of a running session | IS-05 `source_ip`/`interface_ip` change | SHOULD | conflict: needs STOPPED (C-N3); GAP G-N5 |
| N-REQ-32 | Change the received format at an activation (new SDP) | IS-05 receiver `transport_file` | SHOULD | `MTL_UPDATE_MEDIA` in STOPPED only (C-N4); recipe §5.4; GAP G-N6 |
| N-REQ-33 | Validate staged parameters without preparing them | IS-05 PATCH 400 for invalid staging | SHOULD | GAP G-N7 (`mtl_session_query()` covers create, not update) |
| N-REQ-34 | Bound the double bandwidth of a far-ahead scheduled receiver switch | IS-05 scheduled; link capacity | SHOULD | GAP G-N9 (joins are sent at the call) |
| N-REQ-35 | Treat a scheduled time in the past as "now"; accept any future time | IS-05 "when internal clock >= requested_time" | MUST | unstated; G-N25 |
| N-REQ-36 | Resolve `auto` and report the resolved values | IS-05 RAML `/active` | MUST | the Node resolves; MTL reports granted values (`info.leg[]`, `mtl_port_get_spec()`) |
| N-REQ-37 | Read the instance TAI clock for versions and relative activations | IS-04 `version`, IS-05 relative | MUST | `mtl_time_now()` |
| N-REQ-38 | Apply SDP `mediaclk:direct=<offset>` and `mediaclk:sender` at activation | IS-05 transport file; -10 §8.3 | SHOULD | options `rx.rtp_offset`, `rx.mediaclk` are S (STOPPED); GAP G-N18 |
| N-REQ-39 | Generate a complete ST 2110 SDP from a session | -10 §8.1, IS-05 `/transportfile`, IS-04 `manifest_href` | SHOULD (MUST for the Node) | values only; GAP G-N17 (`mtl_sdp.h`) |
| N-REQ-40 | Parse an SDP into a session configuration (legs, PT, filters, essence) | -10 §8.1, IS-05 `transport_file` | SHOULD | GAP G-N17 |
| N-REQ-41 | Report per-port link state and changes | BCP-008 `linkStatus` | MUST | `port.link_up`, `MTL_EVENT_PORT_LINK`, `MTL_EVENT_LEG_STATE` (link monitor: Phase 2, 09 §7.2) |
| N-REQ-42 | RX: report packets arriving, loss per leg and after 2022-7 merge, use of redundancy | BCP-008-01 `connectionStatus`, `GetLostPacketCounters` | MUST | `MTL_STATUS_RX_SIGNAL`, `MTL_EVENT_RX_SIGNAL`, `leg.pkts_lost{leg}`, `rx.pkts_lost_est`, `rx.units_used_redundancy`, `rx.units_incomplete_*` |
| N-REQ-43 | RX: count packets that arrived too late, and units late for presentation | BCP-008-01 `GetLatePacketCounters` | SHOULD | `rx.pkts_stale`, `rx.units_stale`; per-leg and presentation counters GAP G-N16 |
| N-REQ-44 | RX: report stream validity (PT, SSRC, length, format vs. expectation) | BCP-008-01 `streamStatus`; IS-11 `non_compliant_stream` | MUST | `rx.pkts_rejected{cause}`, `MTL_STATUS_FORMAT_CHANGED`, `MTL_EVENT_RX_FORMAT`, `rx.detected.*`; ST 2110-21 `tp.*` |
| N-REQ-45 | TX: report recoverable and unrecoverable transmission errors | BCP-008-02 `transmissionStatus`, `GetTransmissionErrorCounters` | MUST | `tx.units_late`, `tx.units_dropped{reason}`, `tx.units_failed`, `leg.pkts_skipped`, `port.tx_errors`, `MTL_STATUS_PACING_DOWNGRADED`, `MTL_EVENT_TIMING_INFEASIBLE`, flow state `WAITING_NEIGHBOUR` |
| N-REQ-46 | TX: report missing essence | BCP-008-02 `essenceStatus`; IS-11 `no_essence` | SHOULD | `MTL_EVENT_TX_UNDERRUN`, `tx.slots_empty` |
| N-REQ-47 | Report per-port PTP state for multi-interface sync health | BCP-008 `externalSynchronizationStatus` PartiallyHealthy | MUST | `time.state`, `time.offset_ns` per port object; `MTL_EVENT_TIME_STATE` |
| N-REQ-48 | Read counters cheaply and consistently, any time | BCP-008 counters reset on activation (Node baseline) | MUST | `mtl_stat_read()` DP, one snapshot |
| N-REQ-49 | Stable session identity across reconfiguration, a 36-character UUID as name | BCP-008 touchpoints 1:1 for the resource's life | MUST | `sc.name` (64 B, unique), `MTL_UPDATE_MEDIA` keeps identity |
| N-REQ-50 | Dry-run any candidate configuration with capacity | BCP-004-01/-02 constraint sets, IS-11 | MUST | `mtl_session_query(MTL_QUERY_CHECK_CAPACITY)` |
| N-REQ-51 | Never degrade a published sender type or pacing silently | BCP-004-02 "MUST not produce ... incompatible" | MUST | option `caps.pacing_required`, `MTL_EVENT_PACING_CHANGED` |
| N-REQ-52 | Reconfigure a sender's format keeping its identity | IS-11 active constraints | MUST | `MTL_UPDATE_MEDIA` (CREATED/STOPPED) |
| N-REQ-53 | Mute a sender on constraint violation and refuse activation while violated | IS-11 "MUST become inactive" | MUST | Node policy over G-N3 |
| N-REQ-54 | Receive without an SDP (transport parameters only) | IS-05 receivers; IS-11 `unknown` | SHOULD | `video.detect = MTL_DETECT_ON` |
| N-REQ-55 | PTP domain and announce timeout from IS-09 at start-up | IS-09 `global.json` | SHOULD | option `time.ptp_domain`; announce timeout GAP G-N18 |
| N-REQ-56 | Remap or select audio channels when building TX units | IS-08 | MAY | app copy; GAP G-N22 |
| N-REQ-57 | Report the ANC DID/SDID seen on a received stream | IS-04 Flow `DID_SDID` (receivers forwarding) | MAY | GAP G-N19 |
| N-REQ-58 | IPv6 media addresses | IS-05 schemas allow IPv6 | MAY | reserved (`ip_family = 6`); the Node constrains to IPv4 |
| N-REQ-59 | ST 2022-5 FEC and RFC 3550 RTCP | IS-05 optional parameter sets | MAY (IPMX: RTCP MUST) | not supported; G-N20, G-N21 |
| N-REQ-60 | Report a port address change (DHCP renew) | SDP `c=`/`source-filter`, BCP-008 connection status | MAY | `mtl_port_get_spec()` polled; GAP G-N24 |

## 4. Gaps and proposed API changes

Each change is the minimum that closes the gap. All are additive (reserved fields, new keys, new bits), so they fit
the R3 versioning rule and change no size check except where a new struct is added.

| Gap | Header | Proposed symbol | Contract (one line) | Closes |
|---|---|---|---|---|
| G-N1 | `mtl.h` | `struct mtl_update_status`; `mtl_session_get_update(s, &us, size)` | the last update: `seq`, `state` (NONE, PENDING, APPLIED, FAILED, REPLACED, CANCELLED), `requested`, `planned_tai_ns` (the unit boundary it will switch on, known when the call returns), `applied_tai_ns`, `first_media_index`, `first_rtp[2]`, `reason`. CP | N-REQ-19/20 |
| G-N1 | `mtl_queue.h` | `MTL_EVENT_UPDATE = 23` | posted when an update leaves PENDING; `new_value` = state, `value[0]` = applied (or failed) TAI, `value[1]` = update seq; also on `mtl_session_wait(MTL_WAIT_EVENTS)` | N-REQ-20 |
| G-N2 | `mtl.h` | `MTL_UPDATE_REAPPLY 0x10u` | with FLOWS or LEGS: every enabled leg is re-applied even if identical (RX: IGMP leave + join; TX: neighbour re-resolved, header templates rebuilt), at `when` | N-REQ-21 |
| G-N3 | `mtl.h` | relax the `legs_disabled` rule | every leg may be disabled ("muted"): TX sends nothing and consumes units as `tx.units_suppressed`; RX leaves its groups and delivers nothing; the session stays RUNNING; `status.flags` gains `MTL_STATUS_MUTED 0x10u` | N-REQ-25, -53 |
| G-N4 | `mtl.h` | relax `mtl_flow` "ip required" | a leg whose bit is set in `legs_disabled` may have `ip` all zero and `udp_port` 0 (reserved, unaddressed); enabling it needs an address in the same update (`FLOWS \| LEGS`); `flows[1]` then exists by `legs_reserved` bit, see below | N-REQ-26, -30 |
| G-N4 | `mtl.h` | `uint32_t legs_reserved` (from `reserved[]` of `mtl_session_config`) | bit per leg that exists even without a `udp_port`; 0 = today's rule ("a leg exists when its udp_port is not 0") | N-REQ-26, -30 |
| G-N5 | `mtl.h` | allow `port` changes in `MTL_UPDATE_FLOWS` while RUNNING | make before break: the prepare step reserves queue and rule on the new port; `-MTL_ENOSPC` (CAPACITY_*) if it cannot, `-MTL_EBUSY` (PORT_CHANGE_NEEDS_STOP) where a backend cannot | N-REQ-31 |
| G-N6 | `mtl.h` | `MTL_UPDATE_MEDIA` with `when` on RX (later phase) | an RX media change at a unit boundary: the new layout from the first unit with media time ≥ t; same pool if it fits, else `-MTL_EBUSY`; until then, recipe §5.4 | N-REQ-32 |
| G-N7 | `mtl.h` | `MTL_UPDATE_DRY_RUN 0x20u` | validates `sc` against the session (create rules, capacity of a port change) and returns what update would, without preparing or changing anything | N-REQ-33 |
| G-N8 | `mtl.h` | `MTL_UPDATE_CANCEL 0x40u` | cancels a PENDING update (its resources released, state CANCELLED); `sc` may be NULL; `-MTL_EAGAIN` if none is pending (or already committing) | N-REQ-22 |
| G-N9 | `mtl_options.h` | `MTL_OPT_RX_JOIN_LEAD_NS = 213` `"rx.join_lead_ns"` (R) | RX `AT_TAI` updates send new joins at max(call, t − lead); absent = at the call | N-REQ-34 |
| G-N10 | `mtl.h` | `colorimetry`, `tcs`, `range` (uint8 each, from `video.reserved0`/`reserved`; same in `mtl_cvideo_config`) + `enum mtl_colorimetry`, `enum mtl_tcs`, `enum mtl_range` | 0 = unspecified (SDP omits or writes the default); values exactly the ST 2110-20 §7.5/§7.6/§7.3 lists; the library uses them for conversion matrices and SDP, never on the wire | N-REQ-2 |
| G-N11 | `mtl_observe.h` keys | port `time.gm_traceable` G, `time.gm_clock_class` G, `time.gm_clock_accuracy` G, `time.gm_priority1` G; session `info.ts_refclk.*` become G with `{leg=N}` | the values -10 §8.2 and IS-04 `clock_ptp` need; GM-derived keys change at runtime, so they cannot be CONST | N-REQ-11 |
| G-N11 | `mtl_sync.h` | `struct mtl_time_reference` {gmid[8], domain, traceable, clock_class, clock_accuracy, locked}; `mtl_time_set_reference(mt, port, &ref)` (input struct with `struct_size`) | for `MTL_TIME_SOURCE_PHC`/`CLOCK_TAI`/`USER`: the app (reading ptp4l through pmc) supplies what MTL cannot see; the `time.*` keys and `ts_refclk` report it. CP | N-REQ-12 |
| G-N12 | `mtl_queue.h` | `MTL_EVENT_TIME_SOURCE = 24` | port; grandmaster changed (or became unknown): `value[0]` new GM ID, `value[1]` old | N-REQ-13 |
| G-N13 | `mtl.h` / `mtl_observe.h` | `uint8_t mac[6]` in `mtl_port_spec` (output of `mtl_port_get_spec()`, must be zero on input), key `instance.port_count` C | the port's own MAC as used on the wire | N-REQ-14 |
| G-N14 | `mtl_options.h`, `mtl_observe.h` | options `port.lldp` (bool, per port), `port.lldp_chassis_id` (str); `mtl_port_get_neighbor(mt, port, &n, size)` | MTL sends LLDP on ports it owns and keeps the last neighbour TLVs (chassis ID, port ID) received; DPDK only (kernel ports have lldpd) | N-REQ-15 |
| G-N15 | `mtl_observe.h` keys | `info.wire_kbps{leg}` C, `info.payload_kbps` C, `info.max_udp_bytes` C, `info.troffset_default` C (bool) | kbps rounded up, as the NMOS register defines `bit_rate`; payload = codestream or essence bytes per second | N-REQ-7/8/9 |
| G-N16 | `mtl_observe.h` keys | `leg.pkts_late{leg}`, `rx.units_late_presentation` | packets that arrived after their unit was delivered or flushed, per leg; units delivered after `presentation_tai_ns` | N-REQ-43 |
| G-N17 | new `mtl_sdp.h` | §4.1 | SDP render and parse, built only on public calls (like `mtl_util.h`) | N-REQ-39/40 |
| G-N18 | `mtl_options.h` | `rx.rtp_offset`, `rx.mediaclk` become R (applied at the next update's boundary); `MTL_OPT_PTP_ANNOUNCE_TIMEOUT = 2207` `"time.ptp_announce_timeout"` (C) | an activation can carry them; IS-09 for built-in PTP | N-REQ-38, -55 |
| G-N19 | `mtl_observe.h` keys | `anc.did_sdid_seen` (H-like set of up to 16 DID/SDID pairs) | RX ANC sessions report the distinct DID/SDID pairs received | N-REQ-57 |
| G-N20 | — | RFC 3550 RTCP SR/RR (not today's NACK retransmission `rtcp.*`) | NMOS: MAY; IPMX: MUST; scope in the IPMX document | N-REQ-59 |
| G-N21 | — | ST 2022-5 FEC | not proposed; the Node omits the FEC set | N-REQ-59 |
| G-N22 | `mtl_convert.h` | `mtl_audio_remap(dst, src, map, ...)` | channel selection and reordering in one copy (DPC); IS-08 maps are applied by the app at a media index | N-REQ-56 |
| G-N23 | `mtl.h` (comment) | RX unicast rule | RX `flows[i].ip` = the port's own address (or all zero: the port's address) means unicast; `source_filter` then checks the source address; a group address means multicast | N-REQ-27 |
| G-N24 | `mtl_queue.h` | `MTL_EVENT_PORT_ADDRESS = 25` | port; the granted address changed (DHCP); re-read `mtl_port_get_spec()` | N-REQ-60 |
| G-N25 | `mtl.h` (comment) | `mtl_when` rule for updates | `AT_TAI` in the past = `NOW` (no `-MTL_ERANGE`); no horizon limit for updates; `AT_INDEX` likewise | N-REQ-35 |

Proposed declarations for the core additions (sketch; sizes to be checked with `sketch/check.sh`):

```c
/* mtl.h */
#define MTL_UPDATE_REAPPLY 0x10u /* re-apply identical legs: IGMP leave + join, re-ARP (IS-05) */
#define MTL_UPDATE_DRY_RUN 0x20u /* validate only; nothing prepared or changed */
#define MTL_UPDATE_CANCEL 0x40u  /* cancel the pending update; sc may be NULL */
#define MTL_STATUS_MUTED 0x10u   /* every leg is admin-disabled; RUNNING, nothing on the wire */

enum mtl_update_state {
  MTL_UPDATE_NONE = 0, MTL_UPDATE_PENDING = 1, MTL_UPDATE_APPLIED = 2,
  MTL_UPDATE_FAILED = 3, MTL_UPDATE_REPLACED = 4, MTL_UPDATE_CANCELLED = 5,
};
struct mtl_update_status {     /* output; the size argument versions it */
  uint64_t seq;                /* per session, +1 per accepted update call */
  uint32_t state;              /* enum mtl_update_state */
  uint32_t reason;             /* FAILED: mtl_reasons.h */
  uint64_t parts;              /* MTL_UPDATE_* of that call */
  struct mtl_when requested;
  int64_t planned_tai_ns;      /* media time of the boundary it switches on (IS-05 202) */
  int64_t applied_tai_ns;      /* when it switched (IS-05 200); INT64_MIN until then */
  int64_t first_media_index;   /* TX: first unit on the new state */
  uint32_t first_rtp[MTL_MAX_LEGS]; /* RX: RTP of the first unit accepted on each new flow */
};
MTL_API_CP int mtl_session_get_update(mtl_session_h s, struct mtl_update_status* us, size_t size);

enum mtl_colorimetry { MTL_COLOR_UNSPECIFIED = 0, MTL_COLOR_BT601 = 1, MTL_COLOR_BT709 = 2,
  MTL_COLOR_BT2020 = 3, MTL_COLOR_BT2100 = 4, MTL_COLOR_ST2065_1 = 5, MTL_COLOR_ST2065_3 = 6,
  MTL_COLOR_XYZ = 7, MTL_COLOR_ALPHA = 8 };
enum mtl_tcs { MTL_TCS_DEFAULT = 0 /* SDR */, MTL_TCS_SDR = 1, MTL_TCS_PQ = 2, MTL_TCS_HLG = 3,
  MTL_TCS_LINEAR = 4, MTL_TCS_BT2100LINPQ = 5, MTL_TCS_BT2100LINHLG = 6, MTL_TCS_ST2065_1 = 7,
  MTL_TCS_ST428_1 = 8, MTL_TCS_DENSITY = 9, MTL_TCS_ST2115LOGS3 = 10, MTL_TCS_UNSPECIFIED = 11 };
enum mtl_range { MTL_RANGE_DEFAULT = 0 /* NARROW */, MTL_RANGE_NARROW = 1, MTL_RANGE_FULLPROTECT = 2,
  MTL_RANGE_FULL = 3 };

/* mtl_sync.h */
struct mtl_time_reference {
  uint32_t struct_size;
  uint8_t gmid[8];          /* EUI-64 clockIdentity */
  uint8_t domain;
  uint8_t traceable;        /* GM timeTraceable */
  uint8_t clock_class;
  uint8_t clock_accuracy;   /* IEEE 1588 enumeration; 0x22 = 250 ns */
  uint32_t locked;          /* 1 = the PHC follows this GM */
  uint64_t reserved[2];
};
MTL_API_CP int mtl_time_set_reference(mtl_instance_h mt, uint32_t port,
                                      const struct mtl_time_reference* ref);
```

### 4.1 Helper layer: `mtl_sdp.h`

Q-MODE-6 proposed "values only in v1, an L4 parse helper later". For NMOS the render side matters as much as the
parse side: every Sender has to produce a correct SDP on every activation and every grandmaster change, and every
Node would otherwise write the same 300 lines with the same mistakes (missing `TROFF` when it is not the default,
`exactframerate` not reduced, `traceable` claimed without the accuracy check). Like `mtl_util.h`, the helper is built
only on public calls and allocates nothing; NMOS JSON stays outside MTL.

```c
/* mtl_sdp.h — SDP for ST 2110 / ST 2022-6 sessions (RFC 4566, 4570, 7104, 7273). */
struct mtl_sdp_meta {             /* what MTL does not know; all optional */
  uint32_t struct_size;
  uint32_t sdp_flags;             /* MTL_SDP_TRACEABLE_IF_ALLOWED, MTL_SDP_NO_SOURCE_FILTER, ... */
  const char* session_name;       /* s= ; NULL = the session name */
  uint64_t session_id, session_version; /* o= ; version 0 = the update seq */
  const char* mid[MTL_MAX_LEGS];  /* a=mid; NULL = "primary", "secondary" */
  const char* channel_order;      /* audio: "SMPTE2110.(ST,ST)" */
  const char* jxsv_profile, *jxsv_level, *jxsv_sublevel;
  const char* anc_did_sdid;       /* "{0x61,0x02},{0x41,0x05}" */
  uint32_t anc_vpid_code;
  uint32_t par_num, par_den;      /* PAR; 0 = 1:1 */
  uint64_t reserved[4];
};
/* Writes the SDP of a created session as it is now (granted values, active flows, time reference):
   the session's info, config and keys only. Length written (without NUL), -MTL_ENOSPC if cap is short,
   -MTL_EAGAIN while a value it needs is not yet known (ANC raster before start). CP. */
MTL_API_CP int mtl_sdp_render(mtl_session_h s, const struct mtl_sdp_meta* MTL_NULLABLE meta,
                              char* buf, size_t cap);
/* Parses an SDP into sc (direction left as given; flows, payload types, source filters, the essence
   member, rx.rtp_offset / rx.mediaclk into spec_options) and meta (strings point into sdp). Legs from
   a=group:DUP (two m-lines) or a=ssrc-group:DUP (one m-line, two sources). Returns the leg count.
   Unknown attributes are ignored; a malformed required one is -MTL_EINVAL naming it. CP. */
MTL_API_CP int mtl_sdp_parse(const char* sdp, size_t len, struct mtl_session_config* sc,
                             struct mtl_sdp_meta* MTL_NULLABLE meta);
```

Two cautions from S5 still hold: the SDP values are keys of a *created* session (a dry run does not produce them),
and the parser must never be needed by MTL itself (NG3: MTL does not require SDP for audio, ANC, tests).

## 5. Lifecycle mapping recipes

### 5.1 Mapping decisions

| NMOS concept | MTL action | Why |
|---|---|---|
| Node | one instance (`mtl_instance_open`); the NMOS APIs on a management NIC, not an MTL port | MTL owns the media ports through DPDK; mDNS and HTTP need kernel networking |
| `clocks[0]` | one PTP clock for the instance timescale; per-port `time.*` feed sync health | each leg's PHC is compensated into one instance timescale (06 §2.3) |
| `interfaces[i]` | MTL port i: `mtl_port_get_spec()` (+ `mac`, G-N13) | `port_id` must be a MAC |
| Sender / Receiver | one session each, created at boot with all legs reserved and disabled (G-N3, G-N4), started, muted | activations become boundary updates: no allocation at activation, exact scheduled timing |
| Sender/Receiver UUID | `sc.name` = the UUID | stable across `MTL_UPDATE_MEDIA`; BCP-008 touchpoints are 1:1 for the resource's life |
| `master_enable` | all legs enabled per `rtp_enabled` vs. all disabled (mute) | atomic with the flow change, schedulable (G-N3); `mtl_session_stop()` only for format changes |
| `rtp_enabled[i]` | bit i of `legs_disabled` | 1:1 |
| transport parameters | `flows[i]` (§2.3 table) | 1:1, auto resolved by the Node |
| format change (IS-11, new SDP) | stop → `MTL_UPDATE_MEDIA` → start (TX: `when` = activation; RX: A/B swap, §5.4) | media changes need STOPPED (C-N4) |
| re-activation of the same parameters | `MTL_UPDATE_REAPPLY` (G-N2) | IS-05 MUST |
| without G-N3/G-N4 | create the session at the first activation, `mtl_session_close()` on `master_enable = false` | works, but activation then costs a create (allocations, rules, queues) and a scheduled one must create early and `start(when)` |

### 5.2 Boot and IS-04 publishing

```c
mtl_instance_open(NULL, &params, &mt);                    /* ports from config or MTL_PORTS */
stat_get(MTL_OBJ_OF_INSTANCE(mt), "instance.port_count", &np);   /* G-N13 */
for (p = 0; p < np; p++) {
  mtl_port_get_spec(mt, p, &ps);                          /* name, sip, mac (G-N13) */
  node.interfaces[p] = { name: ps.name, port_id: mac_str(ps.mac), chassis_id: lldp_or_null(p) };
}
node.clocks[0] = ptp_clock(mt);                           /* time.grandmaster_id, time.state, time.gm_traceable (G-N11) */
q = mtl_queue_create(mt, {.subscribe = MTL_SUB_PORT | MTL_SUB_TIME | MTL_SUB_SESSIONS});
for each configured sender or receiver r {
  MTL_INIT(&sc); fill direction, essence, essence member (+ colorimetry/tcs/range, G-N10);
  strcpy(sc.name, r.uuid);
  sc.legs_reserved = r.legs == 2 ? 0x3 : 0x1;  sc.legs_disabled = sc.legs_reserved;   /* G-N4, G-N3 */
  opts += caps.pacing_required = 1;                      /* BCP-004-02: never degrade silently */
  mtl_session_create(mt, &sc, &r.s); mtl_queue_bind(q, r.s, MTL_BIND_EVENTS);
  mtl_session_start(&r.s, 1, NULL, NULL);                /* RUNNING, muted: nothing on the wire */
  publish IS-04 Source/Flow/Sender or Receiver with subscription.active = false, version = now_tai();
}
/* Flow: grain_rate = raster.fps, frame_* = raster, interlace_mode = scan, colorspace = colorimetry,
   components from mtl_format_names(video.format).sdp, bit_rate from info.payload_kbps (G-N15).
   Sender: interface_bindings[i] = interfaces[info.leg[i].port].name, bit_rate = info.wire_kbps,
   st2110_21_sender_type = info.sender_type, manifest_href -> mtl_sdp_render(s, &meta, ...) (G-N17). */
```

### 5.3 IS-05 PATCH and activation

```c
int on_patch(res r, patch p) {                 /* p already merged into r.staged; legs = r.legs */
  struct mtl_session_config sc; MTL_INIT(&sc); mtl_session_get_config(r.s, &sc);
  if (p.transport_file) mtl_sdp_parse(p.sdp, len, &sc_from_sdp, &meta);   /* G-N17; params win */
  for (i = 0; i < legs; i++) {                 /* resolve auto; the Node owns the policy */
    flow_from_params(&sc.flows[i], &r.staged.tp[i]);    /* source_ip/interface_ip -> port, ports 5004 */
    set_bit(&sc.legs_disabled, i, !(r.staged.master_enable && r.staged.tp[i].rtp_enabled));
  }
  if (media_differs(&sc, &current)) return format_change(r, &sc, p.activation);    /* §5.4 */
  parts = MTL_UPDATE_FLOWS | MTL_UPDATE_LEGS;
  if (mtl_session_update(r.s, &sc, parts | MTL_UPDATE_DRY_RUN, NULL) < 0) return 400; /* G-N7 */
  if (!p.activation.mode) return 200;          /* staging only */
  if (p.activation.mode == NULL_CANCEL) { mtl_session_update(r.s, NULL, MTL_UPDATE_CANCEL, NULL); return 200; } /* G-N8 */
  struct mtl_when w = {MTL_NOW};
  if (p.activation.mode == ABSOLUTE) w = (struct mtl_when){MTL_AT_TAI, 0, tai_ns(p.requested)};
  if (p.activation.mode == RELATIVE) { mtl_time_now(mt, &now); w = (struct mtl_when){MTL_AT_TAI, 0, now + rel_ns(p.requested)}; }
  if (mtl_session_update(r.s, &sc, parts | MTL_UPDATE_REAPPLY, &w) < 0) return 500; /* active unchanged */
  mtl_session_get_update(r.s, &us, sizeof us);                                      /* G-N1 */
  if (w.kind == MTL_NOW) {
    wait_event(r.s, MTL_EVENT_UPDATE, us.seq, timeout = 1 s);                        /* applied */
    mtl_session_get_update(r.s, &us, sizeof us);
    commit_active(r, us.applied_tai_ns); return 200 with activation_time = us.applied_tai_ns;
  }
  return 202 with activation_time = us.planned_tai_ns;   /* commit_active() runs on MTL_EVENT_UPDATE */
}
void commit_active(res r, int64_t t) {          /* also on the scheduled MTL_EVENT_UPDATE */
  r.active = r.staged (auto resolved: info.leg[i].udp_src_port, port sip, chosen group);
  r.subscription.active = !(status.flags & MTL_STATUS_MUTED);
  regenerate SDP; bump IS-04 version (TAI); monitor_on_activation(r, t);           /* §5.5 */
}
/* On MTL_EVENT_UPDATE with FAILED: /active keeps the old values; if the session went to ERROR,
   active.master_enable = false (IS-05 Behaviour §Scheduled Activations). */
```

Timing: a TX update switches on the first unit whose media time is ≥ t (09 §7.1), so `planned_tai_ns` is t rounded
up to the next frame or field (≤ 16.7 ms at 59.94p) and all legs switch on the same unit. RX switches by the media
time derived from RTP, so it is exact to the unit. An immediate activation is applied within one unit period plus
the command acknowledgement (bounded by `instance.cmd_ack_timeout_ns`, 100 ms). IS-05 sets no accuracy figure
[verified]; a controller compares `activation_time` across devices, which is why G-N1 reports the boundary actually
used. Joins and ARP for the new flows start at the call (09 §7.1), so a Node should call `mtl_session_update()` as
soon as the PATCH arrives; G-N9 bounds the double bandwidth if the time is far ahead.

### 5.4 Receiver activation with a new format (A/B swap)

```c
int format_change(res r, struct mtl_session_config* sc, activation a) {
  if (a.mode == IMMEDIATE) {                     /* simplest: stop, update, start */
    mtl_session_stop(&r.s, 1, MTL_STOP_FLUSH, MTL_MS(100));
    mtl_session_update(r.s, sc, MTL_UPDATE_MEDIA | MTL_UPDATE_FLOWS | MTL_UPDATE_LEGS, NULL);
    mtl_session_start(&r.s, 1, NULL, NULL);      /* identity, stats and monitors survive */
    return 200;
  }
  /* scheduled at t: a second session takes over at t, the old one is closed after t */
  snprintf(sc->name, sizeof sc->name, "%s.b", r.uuid);
  if (mtl_session_create(mt, sc, &nb) < 0) return 500;     /* capacity: ENOSPC + reason */
  struct mtl_when w = {MTL_AT_TAI, 0, t};
  mtl_session_start(&nb, 1, &w, NULL);           /* RX: delivers units with media time >= t */
  at(t, { mute(r.s); swap(r.s, nb); mtl_session_close(old, MTL_MS(200)); });
  return 202 with activation_time = t;
}
```

A sender format change (IS-11) is simpler: stop, `MTL_UPDATE_MEDIA`, then `mtl_session_start(&s, 1, &when, NULL)`
with the activation time, because a TX start takes `AT_TAI`.

### 5.5 BCP-008 monitoring

The Node keeps one monitor per session. It lists the schema once (`mtl_stat_list`), reads all values every 100–250 ms
with `mtl_stat_read()` (DP, one snapshot), and reacts to events from the shared queue. MTL counters are cumulative
(08 §2.3); "reset on activation" is a baseline the monitor takes when the update is applied.

```c
void monitor_on_activation(mon m, int64_t t) {   /* MTL_EVENT_UPDATE APPLIED (G-N1) */
  if (m.auto_reset) { stat_read_all(m.s, m.baseline); clear messages and transition counters; }
  m.state = all domains Healthy; m.hold_until = t + m.status_reporting_delay;     /* default 3 s */
}
void monitor_tick(mon m) {                        /* every 100-250 ms and on each event */
  if (status.flags & MTL_STATUS_MUTED) { set all to Inactive / NotUsed immediately; return; }
  link   = count legs with port.link_up == 0 among enabled legs -> AllUp / SomeDown / AllDown;
  sync   = per leg port: time.state LOCKED on all -> Healthy, on some -> PartiallyHealthy, none -> Unhealthy;
           time source internal -> NotUsed; MTL_EVENT_TIME_SOURCE (G-N12) -> PartiallyHealthy for one delay;
  delta  = stat_read_all(m.s) - m.last;  apply hysteresis: worse now, better after the delay held;
}
```

| BCP-008 property or method | Computed from (revision 4, plus gaps) |
|---|---|
| `linkStatus` (both) | `port.link_up` of each enabled leg's port; `MTL_EVENT_PORT_LINK`, `MTL_EVENT_LEG_STATE`; message names the interfaces |
| `connectionStatus` Healthy | RX signal (`MTL_STATUS_RX_SIGNAL`) and Δ`rx.units_incomplete_*` = Δ`rx.units_used_redundancy` = 0 |
| `connectionStatus` PartiallyHealthy | Δ`rx.units_used_redundancy` > 0 or Δ`leg.pkts_lost{leg}` > 0 with complete units (2022-7 recovered) |
| `connectionStatus` Unhealthy | no signal (`MTL_EVENT_RX_SIGNAL` 0), `JOIN_FAILED`, Δ`rx.units_incomplete_delivered` + `_discarded` > 0, Δ`rx.pkts_lost_est` > 0, late (below) |
| `GetLostPacketCounters` | `leg.pkts_lost{leg=0,1}`, `rx.pkts_lost_est` (after merge), minus baseline |
| `GetLatePacketCounters` | `rx.pkts_stale`, `rx.units_stale`; per leg `leg.pkts_late{leg}` and `rx.units_late_presentation` (G-N16) |
| `streamStatus` | Δ`rx.pkts_rejected{cause=pt,ssrc,len,interlace}`, `MTL_STATUS_FORMAT_CHANGED`, `rx.detected.*` vs. the SDP; optional `tp.fail` with `rx.timing_parser` (PartiallyHealthy) |
| `transmissionStatus` PartiallyHealthy | Δ`tx.units_late`, `MTL_STATUS_PACING_DOWNGRADED`, `MTL_STATUS_TIMING_WARNING`, one leg `WAITING_NEIGHBOUR` or skipped (Δ`leg.pkts_skipped`) |
| `transmissionStatus` Unhealthy | Δ`tx.units_dropped{reason}`, Δ`tx.units_failed`, every leg `WAITING_NEIGHBOUR`, session ERROR, Δ`port.tx_errors` |
| `GetTransmissionErrorCounters` | `tx.units_dropped{reason=...}`, `tx.units_late`, `tx.units_failed`, `leg.pkts_skipped{leg}`, `port.tx_errors`, `tx.build_overrun` |
| `essenceStatus` | Unhealthy on `MTL_EVENT_TX_UNDERRUN` / Δ`tx.slots_empty` > 0; content checks (black, silence) are the app's |
| `externalSynchronizationStatus`, `synchronizationSourceId` | per port `time.state`, `time.grandmaster_id` ("<gmid> on <interface>"), `MTL_EVENT_TIME_STATE`, `MTL_EVENT_TIME_SOURCE` (G-N12) |
| `overallStatus` | the least healthy domain; Inactive when muted or stopped |

## 6. Conflicts with the current design

| ID | Design today | NMOS behaviour | Resolution |
|---|---|---|---|
| C-N1 | `mtl_session_update()` is all or nothing for the whole session; fields outside `parts` must match | IS-05 PATCHes one resource with every leg (`{}` for unchanged legs) and resolves everything or nothing ("active ... must not change") | no conflict: the IS-05 unit of atomicity is the resource, which is the session; a Node sends `FLOWS \| LEGS` together |
| C-N2 | "Disabling the last enabled leg is `-MTL_EINVAL` (use stop)" (09 §7.2); `mtl_session_stop()` takes no `when` | `master_enable = false` and `rtp_enabled = false` on every leg are legal, scheduled or immediate, together with a parameter change | G-N3: allow every leg disabled (mute) |
| C-N3 | port changes in `MTL_UPDATE_FLOWS` need STOPPED (`PORT_CHANGE_NEEDS_STOP`) | `source_ip`/`interface_ip` may change in any activation | G-N5; until then the Node stops, updates and starts (not schedulable), or swaps sessions as §5.4 |
| C-N4 | adding or removing `flows[1]`, and `MTL_UPDATE_MEDIA`, need STOPPED | a 2022-7 receiver alternates between one-leg and two-leg SDPs; a new SDP may change the format at a scheduled time | legs: create both at boot (G-N4), toggle with `legs_disabled`; media: §5.4 now, G-N6 later |
| C-N5 | "a leg identical to its current flow is unchanged" (09 §7.1) | re-activation MUST re-apply (IGMP leave and join) | G-N2 `MTL_UPDATE_REAPPLY` |
| C-N6 | the update call "returns once the update is prepared and posted"; `FLOW_STATE` carries a media index, no instant | 200 only after the change is applied, with the TAI instant; 202 with the planned instant | G-N1 |
| C-N7 | in CREATED and STOPPED an update applies at once "with NOW semantics", ignoring `when` | a scheduled activation of an inactive resource must wait for t | with G-N3 sessions stay RUNNING (muted), so `when` is honoured; otherwise update now and `start(when)` |
| C-N8 | counters are cumulative, never reset (08 §2.3) | BCP-008 resets counters on activation and on `ResetCountersAndMessages` | keep the MTL rule; the Node subtracts a baseline taken on `MTL_EVENT_UPDATE` (G-N1) |
| C-N9 | `info.ts_refclk.*` are CONST keys | the grandmaster can change at runtime and the SDP must follow (and the Sender `version` with it) | G-N11: gauges, per leg |
| C-N10 | `rx.rtp_offset`, `rx.mediaclk` change only in STOPPED (S) | they come from the SDP of an activation | G-N18: R, applied at the update boundary |
| C-N11 | TX `udp_src_port` 0 = `udp_port`; option `session.src_port_mode` may be RANDOM or MULTI | IS-05 `source_port` is one value, `auto` = 5004 | the Node always sets the source port; MULTI cannot be represented in IS-05 and must not be used by an NMOS sender |
| C-N12 | `flows[i].port = 0` means "leg i on port i" | `interface_bindings` may list one interface twice (both legs on one NIC) | Q-NMOS-3 |
| C-N13 | joins for a scheduled RX update are sent at the call | a salvo scheduled seconds ahead would carry old and new streams on the link meanwhile | G-N9 |
| C-N14 | a pacing class can fall back (unless `caps.pacing_required`) | BCP-004-02: never produce a stream incompatible with the advertised caps; SDP `TP` must stay true | the Node sets `caps.pacing_required = 1`, or re-publishes on `MTL_EVENT_PACING_CHANGED` |

## 7. Open questions

| ID | Question | Recommendation |
|---|---|---|
| Q-NMOS-1 | Mute (G-N3) or a scheduled `mtl_session_stop(..., when)`? | mute: it composes with `FLOWS` in one atomic update and keeps the session's resources, so re-enable is a boundary command; stop stays for format changes |
| Q-NMOS-2 | What is `planned_tai_ns` for TX: the media time of the first switched unit, or its first packet's launch (media time + TROFFSET)? | the media time (the unit boundary); IS-05 compares devices by it, and launch differs per sender type |
| Q-NMOS-3 | May both legs use one port (`interface_bindings` listing one interface twice; RFC 7104 separate addresses on one NIC)? | allow it with an explicit `flows[1].port = 1` (port 0 + 1); ST 2110-10 §8.5 still forbids identical source and destination |
| Q-NMOS-4 | Audio updates: switch at a unit (default 10 ms) or a packet (1 ms) boundary? | packet boundary, so a salvo of video and audio to one t lands within one ptime |
| Q-NMOS-5 | Colorimetry, TCS and RANGE as typed fields (G-N10) or options? | typed: they are SDP-mandatory, IS-04-required and select conversion matrices; three bytes in reserved space |
| Q-NMOS-6 | ST 2110-41 has no NMOS media type or flow schema yet. Publish fast metadata as `urn:x-nmos:format:data` with an unregistered `media_type`? | yes, as a vendor value until AMWA registers one; flag to AMWA [inferred: no work item seen in the registers] |
| Q-NMOS-7 | `mtl_sdp.h` in v1 (render and parse), or only values (Q-MODE-6 default)? | render and parse in v1, as an optional helper over public calls; NMOS and IPMX Nodes both need it |
| Q-NMOS-8 | LLDP on DPDK ports (G-N14): MTL feature or the Node's (raw packets through packet mode)? | MAY; defer: `chassis_id = null` is allowed, and topology tools mostly use the switch side |
| Q-NMOS-9 | Should MTL count BCP-008 status transitions itself? | no: hysteresis, reporting delay and messages are Node policy; MTL gives counters and events |
| Q-NMOS-10 | A timed close of the old session in the A/B swap (§5.4) runs on a Node timer; is a `MTL_AT_TAI` stop worth adding for RX? | only if G-N6 is rejected; with G-N6 the swap disappears |
| Q-NMOS-11 | RTCP (RFC 3550): NMOS treats it as optional, IPMX needs sender reports. One RTCP design for both? | decide in the IPMX document; keep today's NACK retransmission (`rtcp.*`) clearly separate in naming |
