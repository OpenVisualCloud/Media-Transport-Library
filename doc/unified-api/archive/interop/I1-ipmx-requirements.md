# I1 — What IPMX needs from the unified MTL API

| | |
|---|---|
| Status | Analysis for maintainer review, 2026-10-01. Nothing is implemented; every proposal below is a design change to the revision-4 headers in [sketch/include/mtl/experimental/](../../sketch/include/mtl/experimental/). What was adopted, and in which form, is [17 §6](../17-nmos-and-ipmx.md) |
| Question | "We also want to work with NMOS and IPMX; make sure the proposed API suits them." This document covers IPMX (VSF TR-10 and the AIMS profiles). [N1-nmos-requirements.md](N1-nmos-requirements.md) covers AMWA NMOS; §2.4 lists what N1 should also check |
| Sources | The TR-10 PDFs from <https://vsf.tv/technical-recommendations/> (list and status fetched 2026-10-01; texts read from `static.vsf.tv/download/technical_recommendations/VSF_TR-10-*.pdf`), AIMS PQCR v1.1 and profiles (<https://ipmx.io/technical-information/>), TR-10 TP-1 (2026-07-31), AMWA BCP-005-02/-03 (<https://specs.amwa.tv/bcp-005-02/>, <https://specs.amwa.tv/bcp-005-03/>) |
| Labels | [verified] = I read the text (TR section given); [verified, code] = read in this repository at HEAD `545a266a`; [inferred] = reasoning or general knowledge, not checked against a text |
| Names | Revision-4 symbols (`mtl.h`, `mtl_sync.h`, `mtl_options.h`, ...). New symbols are proposals; key and event numbers avoid those N1 proposes (event 23–25, option 213, 2207) |

## 1. Summary

IPMX is ST 2110 plus NMOS for Pro-AV: the TR-10 parts are written as differences from ST 2110 (TR-10-0 §4)
[verified]. What changes for a transport library:

1. **PTP is optional** (TR-10-1 §7). Without a grandmaster a sender keeps a free-running Internal Clock; with
   one it must use it. **Async media** (an HDMI source at its own rate) is first class: the media clock is
   frequency-locked to the source, not to the Internal Clock, and the SDP says `a=mediaclk:sender`
   (§8.3, §8.4, §10.5) [verified].
2. **RTCP Sender Reports are mandatory** for every IPMX sender, on UDP port media+1, with an **IPMX Info Block**
   (tag `0x5831`) that carries ts-refclk, mediaclk and per-essence Media Info Blocks. Video sends one per
   frame or field, before the frame's first packet; audio one per 10 ms (§8.7–8.10) [verified]. They are what
   lets a receiver without PTP recover the sender's clock and align its flows (§11, TR-10-9 §11).
3. **A different traffic model**: CMAX = max(16, Npkts/(21600·TFRAME)), a gapped read schedule with
   RACTIVE = height/vtotal, and a small VRX that drains from half full (TR-10-1 §8.1) [verified].
4. **Any resolution and any frame rate**; RGB 4:4:4 8-bit and YCbCr 4:2:2 10-bit are the receiver minimum
   (TR-10-2 §8, AIMS uncompressed profile §5–6) [verified].
5. **Optional per-packet encryption**: PEP (TR-10-13, AES-128-CTR mandatory when implemented) and HDCP 2.3 via
   HKEP (TR-10-5), both with an RTP header extension per packet and the payload header left in clear.
6. **Network**: unicast and multicast, IGMPv3 SSM **and** IGMPv2 with a user choice, IPMX DSCP defaults
   (AF42 video, AF41 audio), DHCP by default, in-band NMOS/HKEP on the media port (TR-10-9 §14–19) [verified].

Revision 4 already covers most essence-level needs: every essence (InfoFrames are ST 2110-41 fast metadata,
AES3 is AM824), rational frame rates in `mtl_raster`, RGB and 4:4:4 formats, unicast and SSM in `mtl_flow`,
IS-05-style atomic `mtl_session_update`, packet units for anything the library cannot packetise, and
`rx.mediaclk = sender` on receive. **The gaps are in timing, RTCP and crypto**: no RTCP SR (today's `rtcp.*`
options are a proprietary NACK retransmission), no media mode for async sources (every TX RTP is
`floor(M × R)` of an instance-clock media time), no runtime switch between free-run and PTP, no IPMX sender
type, no header-extension or cipher support, no IGMPv2, and a link offset that cannot change while running.
§4 proposes 17 additive changes (two optional headers, `mtl_rtcp.h` and `mtl_crypto.h`; the rest are enum
values, reserved-field uses and option keys). None changes a pinned-core rule: SR building is O(1) on the
tasklet from a prepared template, and the cipher runs in the caller (DPC) or on library crypto workers.

## 2. The IPMX specification set

### 2.1 The TR-10 parts

Status as listed on the VSF page on 2026-10-01 [verified]. "Needs" is what the part asks of the media transport.

| Part | Title, edition | Status | Needs from the transport |
|---|---|---|---|
| TR-10-0 | General Organization, 2026-07-07 | Final (informative) | Media Info Block type registry: 0x0001 video, 0x0002 PCM, 0x0003 CBR compressed, 0x0004 AES3, 0x0005 VBR compressed, 0x0008 JPEG XS, 0x0009 H.265, 0x000A H.264, 0x0010 HKEP, 0x0011 PEP (§5); TR-10-16 uses 0x0006 [verified] |
| TR-10-1 | System Timing and Definitions, 2024-02-23 | Final | with and without PTP, free-running Internal Clock, BMCA, follower-only (§7); IPMX NCM and VRX (§8.1); AES67 audio (§8.2); async and sync sources (§8.3–8.4); first RTP from the Internal Clock (§8.6); SR + Info Block on port +1, schedules (§8.7–8.10); inline processors (§9); SDP (§10); receiver timing, link offset (§11) [verified] |
| TR-10-2 | Uncompressed Active Video, 2024-02-23 | Final | ST 2110-20 §1–5, 6.1.2, 6.1.4, 6.1.5, 6.2, 6.3, 7 (§7); even UDP port > 1024; receivers support YCbCr 4:2:2/10 and RGB 4:4:4/8 (§8); 90 kHz (§9); Media Info Block 0x0001 with sampling, depth, packing, interlace, PAR, range, colorimetry, TCS, size, rate (22/10-bit rational), measured pixel clock, htotal, vtotal (§10) [verified] |
| TR-10-3 | PCM Digital Audio, 2024-02-23 | Final | AES67 and ST 2110-30 §1–5, 6.2.2; 48 kHz L16/L24 MUST, 44.1 kHz (L16) and 96 kHz (L24) SHOULD (§8); receive level A at any channel count (§10); MIB 0x0002 with measuredsamplerate and channel-order (§11) [verified] |
| TR-10-4 | SMPTE ST 291-1 Ancillary Data, 2023-04-14 | Draft | ST 2110-40 §5.1, 5.2, 5.5; 90 kHz; SR with Info Block, no MIB required (§7–10) [verified] |
| TR-10-5 | HDCP Key Exchange Protocol, 2026-02-17 v2 | Final | HKEP over TCP between Sender and Receiver (§9.3, §12); in every media packet an HDCP Full or Short IV Counters RTP header extension, first among extensions (§14); RTP header, extensions and payload header unencrypted (§15); ANC not encrypted; MIB 0x0010 (§16) [verified] |
| TR-10-6 | Forward Error Correction, 2023-08-07 | Draft | optional ST 2022-5 Profile A: column FEC L=2, D=16 above 32 packets/ms, 1×1 below; column FEC on port +2, row on +4; shaping, partial matrices at frame end and on timeout; receivers must tolerate FEC streams (§7) [verified] |
| TR-10-7 | Compressed Video, 2024-11-22 | Draft | VBR compressed video over the ST 2110-22 §6 payload; CMAX from the maximum rate; no VRX; `b=AS` maximum bit rate; MIB 0x0005 (§7–12) [verified] |
| TR-10-8 | NMOS Requirements, 2026-01-06 | Final | IS-04 1.3, IS-05 1.1, BCP-002, BCP-004-01/02, BCP-005-01, IS-08, IS-11 (§7); `ext_link_offset_delay` with min/max constraints and `auto` (§8) [verified]; NMOS side in N1 |
| TR-10-9 | System Environment and Device Behavior, 2025-05-13 v2 | Draft (PQCR: baseline) | SR + SDES CNAME compound (§8); non-baseband substitutes (§10); direct=0 and sender receivers using SRs (§11); frame interval ≤ 2 ms p-p over 2 s (§11.2); DHCP, DNS-SD, DSCP AF42/AF41/EF, IGMPv3 SSM and IGMPv2, address defaults, in-band control (§14–19) [verified] |
| TR-10-10 | HDMI InfoFrame Packet Transport, 2024-10-07 | Draft | ST 2110-41 with DIT 0x100100, K=0, port media+3, RTP of the video; one or more packets per frame/field sent before the video's first packet; Null InfoFrame block when empty; IS-05 `ext_infoframe_enabled` (§5–14) [verified] |
| TR-10-11 | Constant Bit-Rate Compressed Video, 2024-02-23 | Final | ST 2110-22 §4, §6; TR-10-1 NCM, no VRX; MIB 0x0003 = 0x0001 layout, more MIBs may follow (§7–12) [verified] |
| TR-10-12 | AES3 Transparent Transport, 2023-08-30 | Draft | ST 2110-31 §5.1, 5.3, 5.4, 6, 7; 48 kHz MUST, 44.1/96 MAY; MIB 0x0004 (§7–10) [verified] |
| TR-10-13 | Privacy Encryption Protocol (PEP), 2026-02-17 v2 | Final | PSK key derivation (§12); `a=privacy` and IS-05 `ext_privacy_*` (§13); AES-CTR or GCM (§15); RTP adaptation: CTR Full/Short header extensions, 16-byte slices, 64-bit counter, AES-128-CTR mandatory, CMAC-64 optional, RTP_KV key versions (§20); RTCP clear; MIB 0x0011 (§22) [verified] |
| TR-10-14 | IPMX USB, 2026-04-07 | Draft | USB over TCP control and data channels, PEP CMAC-AAD modes on TCP messages (§8, §12) [verified, sections read in part] |
| TR-10-15 Part 1 | JPEG-XS Codec Requirements, 2026-09-22 | Final | TR-10-11 transport, RFC 9134; High444.12 with 4:2:2/10 and RGB 4:4:4/8; sublevels above 4 bpp allowed; MIB 0x0008 (T, P, Ppih, Plev) after 0x0003 (§7–9) [verified] |
| TR-10-15 Part 2 / 3 | H.265 / H.264 Codec Requirements, 2026-06-04 | Draft | TR-10-7 transport; RFC 7798 / RFC 6184 packetisation with BCP-006-03/-02 limits, no PACI; `TP=2110TPW` (no gapped mode); HRD and SEI rules [verified, grep of the text] |
| TR-10-16 | HDR Info Block, 2025-11-18 | Draft | optional MIB 0x0006 after the colorimetry MIB; its N bit says "for the next field", so it can change per field (§7) [verified] |

Corrections to the parts as the task guessed them: TR-10-4 is ANC (InfoFrames are TR-10-10), TR-10-9 is the
device-behaviour part, TR-10-10 is InfoFrames (USB is TR-10-14), and PEP keys are not exchanged through IS-10:
they derive from pre-shared keys plus parameters carried in SDP and IS-05 transport parameters (TR-10-13 §3,
§13; BCP-005-03) [verified]. There are also TR-10-15 (codecs) and TR-10-16 (HDR).

### 2.2 Around the TRs

- **AIMS PQCR v1.1** (Aug 2026): baseline = TR-10-1, TR-10-8, TR-10-9; HDCP, InfoFrames, PEP, USB and HDR are
  optional capabilities, tested if declared. It requires SDP, NMOS and RTCP to describe the same stream and
  the block version to increment on change (§4.1.1) [verified].
- **AIMS profiles**: uncompressed (RGB 8-bit 4:4:4 or YCbCr 10-bit 4:2:2 for senders, both for receivers,
  resolution and frame rate "Undefined / Any"), PCM, JPEG XS, HEVC [verified, uncompressed read].
- **TR-10 TP-1** §13.3 checks the SR port (+1), DSCP, RTP/NTP pairing, Info Block fields, the per-frame and
  10 ms schedules and SR-before-packet order; §13.5 checks CMAX, VRX and the 2 ms frame interval [verified].

### 2.3 Where IPMX mandates NMOS behaviour

TR-10-8 makes NMOS mandatory, so every NMOS requirement in N1 is also an IPMX requirement. IPMX adds
transport-facing NMOS items: `ext_link_offset_delay` (TR-10-8 §8), `ext_infoframe_enabled` (TR-10-10 §10),
`ext_privacy_*` (TR-10-13 §13, BCP-005-03), the `hkep` Sender attribute and capability (BCP-005-02), and
consistency of SDP, NMOS and RTCP (PQCR §4.1.1).

### 2.4 What the NMOS document should also check

1. IPMX needs RTCP **SR only**: RR is "not required to be compliant" (TR-10-1 §8.7 note); N1's G-N20 says SR/RR.
2. IS-05 `rtcp_enabled`/`rtcp_destination_*` and `fec_*` parameters vs the fixed IPMX ports (+1, +2, +4, and +3
   for InfoFrames): reject other values, or map them to `rtcp.dst_port` (GI-1).
3. The IS-04 Flow/Sender `version` must change when the Info Block version changes (`MTL_EVENT_RTCP_INFO`, GI-2).
4. `ext_link_offset_delay` in µs, `auto`, min/max constraints only while active, 0 when inactive (GI-13).
5. `ext_privacy_*`: values fixed at activation (`master_enable` true), Receivers fail activation on an unknown
   `key_id` (BCP-005-03) → `mtl_crypto_set_key` before start or at the activation `when` (GI-10).
6. SDP content the Node writes: `IPMX` fmtp keyword, `measuredpixclk`/`htotal`/`vtotal`/`measuredsamplerate`,
   ts-refclk `localmac=` without PTP, `a=mediaclk:sender`, `a=extmap` for PEP/HDCP, `a=privacy`, `a=hkep`,
   `a=infoframe`, `FECPROFILE=profile-a`, `b=AS` for TR-10-7; N1's `mtl_sdp.h` (G-N17) should take them.
7. N1's G-N10 colorimetry/TCS/range fields are also the inputs of the IPMX video MIB (GI-2).
8. A sender whose clock source changes (TR-10-9 §12) changes SDP, NMOS and SR together (N1 G-N12 + GI-2).

## 3. Requirements

Level is the TR keyword; "MUST (if X)" applies when the optional feature X is implemented. Coverage names a
revision-4 symbol, PARTIAL with the missing piece, or GAP with the §4 item.

| ID | Requirement | Source | Level | Revision-4 coverage |
|---|---|---|---|---|
| I-REQ-1 | Work in networks with and without a Common Reference Clock | TR-10-1 §7 | MUST | PARTIAL: `MTL_TIME_SOURCE_AUTO` resolves once at open; GI-5 |
| I-REQ-2 | No PTP: the sender keeps a free-running Internal Clock | TR-10-1 §7.1 | MUST | PARTIAL: `MTL_TIME_SOURCE_SYSTEM_TAI` is ESTIMATED but follows NTP steps; GI-5 |
| I-REQ-3 | PTP present: the sender uses it; Internal Clock synchronised | TR-10-1 §7.2 | MUST | `MTL_TIME_SOURCE_PHC`, `_PTP_BUILTIN`; runtime switch GAP GI-5 |
| I-REQ-4 | BMCA per ST 2059-2 §6.2, `slaveOnly` TRUE by default, any `logMinDelayReqInterval` | TR-10-1 §7.2 | MUST | PHC with ptp4l: yes; built-in PTP takes the first Announce (`mt_ptp.c:1032`) [verified, code]: GI-5 (engine) |
| I-REQ-5 | A PTP leader that BMCA can elect | TR-10-1 §7.2 | SHOULD | out of scope (ptp4l) |
| I-REQ-6 | Video NCM with CMAX = max(16, int(Npkts/(21600·TFRAME))) | TR-10-1 §8.1 | MUST | GAP GI-6 (`MTL_SENDER_W` is linear, 06 §5.2) |
| I-REQ-7 | IPMX VRX: gapped, drains from VRXFULL/2 at Npkts/((height/vtotal)·TFRAME), VRXFULL = 2·CMAX | TR-10-1 §8.1 | MUST | GAP GI-6 (no `vtotal`) |
| I-REQ-8 | Audio sender timing per AES67 §7.5; 125 µs ptime for low latency | TR-10-1 §8.2 | MUST | `mtl_audio_config.samples_per_packet` (6 at 48 kHz) |
| I-REQ-9 | Senders support Async and Sync source media | TR-10-1 §8.3 | MUST | GAP GI-4 |
| I-REQ-10 | Media clock frequency-locked to the baseband source | TR-10-1 §8.4 | MUST | GAP GI-4 (INDEX/TAI tie RTP to the instance clock; NEAREST drops or repeats) |
| I-REQ-11 | RTP per ST 2110-10; first RTP taken from the Internal Clock | TR-10-1 §8.5–8.6 | MUST | sync: `floor(M × R)` on the epoch timeline (`mtl_sync.h`); async: GI-4 |
| I-REQ-12 | Baseband video: RTP sampled at VSYNC; SR NTP sampled at the same instant | TR-10-1 §8.8.1 | MUST | GI-4 (`unit.media_tai_ns` = VSYNC) + GI-1 |
| I-REQ-13 | Non-baseband video: RTP as ST 2110-10 | TR-10-1 §8.8.1 | MUST | `MTL_MEDIA_AUTO`/`INDEX`/`TAI` |
| I-REQ-14 | SR per RFC 3550 §6.4.1, to the media's destination IP, UDP port +1 | TR-10-1 §8.7 | MUST | GAP GI-1 (`rtcp.*` keys 310–314 are NACK retransmission) |
| I-REQ-15 | Every SR carries the IPMX Info Block: tag 0x5831, length, block version, ts-refclk (64 B), mediaclk (12 B), MIBs | TR-10-1 §8.7 | MUST | GAP GI-2 |
| I-REQ-16 | SR NTP = Internal Clock as PTP truncated timestamp (seconds low 32 bits, ns) matching the SR's RTP; SSRC = media SSRC; RC 0 | TR-10-1 §8.7 | MUST / SHOULD (RC) | GAP GI-1 (value = media time; R5 TAI ns is the PTP timescale) |
| I-REQ-17 | Video: one SR per frame (per field if interlaced, PsF as progressive), RTP = the frame's, sent before its first packet and after the previous frame's first | TR-10-1 §8.8.2 | MUST | GAP GI-1 |
| I-REQ-18 | ANC: one SR per new RTP timestamp, same placement | TR-10-1 §8.9.2 | MUST | GAP GI-1 |
| I-REQ-19 | Audio: SR for the first packet and every N = int(10 ms/ptime) packets, before that packet | TR-10-1 §8.10.1 | MUST | GAP GI-1 |
| I-REQ-20 | SR inside a compound packet, first, followed by SDES CNAME | TR-10-9 §8 | MUST | GAP GI-1 (`rtcp.cname`) |
| I-REQ-21 | RTCP DSCP = the stream's DSCP | TR-10-9 §16 | MUST | GI-1 contract (from `mtl_flow.dscp`) |
| I-REQ-22 | Receivers tolerate other RTCP packets; RR not needed | TR-10-1 §8.7 | MUST | today port+1 is not received; GI-3 parses SR and drops the rest |
| I-REQ-23 | With SSM a sender never joins INCLUDE its own source | TR-10-1 §8.7 | MUST | TX sessions do not join [inferred: joins are RX-only in `mt_mcast.c`]; stays so with GI-3 |
| I-REQ-24 | Inline processors keep the input's timing by default | TR-10-1 §9 | MUST | PARTIAL: `MTL_SUBMIT_RTP_TS` + `unit.hold`; SR NTP and refclk strings of the input GI-2, GI-4 |
| I-REQ-25 | SDP with `IPMX` keyword, measured values, ts-refclk (`localmac=` without PTP), mediaclk `direct=0` or `sender` | TR-10-1 §10 | MUST | app (NG3); inputs `mtl_session_info`, `info.ts_refclk.*`, `time.grandmaster_id`, `leg[].src_mac`; mediaclk GI-2 |
| I-REQ-26 | Receivers producing baseband recover async timing (frequency-locked output) | TR-10-1 §11.1 | SHOULD | GAP GI-3 |
| I-REQ-27 | Link Offset Delay controllable while active (`ext_link_offset_delay`, min/max, `auto`) | TR-10-1 §11.2, TR-10-8 §8 | MUST (if supported) | PARTIAL: `rx.link_offset_ns` is S (STOPPED only); GI-13 |
| I-REQ-28 | UDP destination port even and > 1024 (SHOULD > 5000); default 5004 | TR-10-2 §7, TR-10-9 §17 | MUST | app; checked under `session.profile=ipmx` GI-16 |
| I-REQ-29 | UDP size ≤ Standard UDP Size Limit, header extensions included | TR-10-2 §7 | MUST | `session.max_udp_payload`; GI-10 sizes payloads after extensions |
| I-REQ-30 | Receivers: YCbCr 4:2:2/10 and RGB 4:4:4/8; senders at least one | TR-10-2 §8, AIMS profile §6 | MUST | `MTL_YUV422_10`, `MTL_RGB_8` |
| I-REQ-31 | GPM/BPM packing, pgroups, 90 kHz, one RTP per frame or field | TR-10-2 §7, §9 | MUST | `mtl_video_config.packing`; 06 §4.5 |
| I-REQ-32 | Any resolution and any frame rate | AIMS uncompressed profile §5–6 | MUST | PARTIAL: `mtl_raster.fps` is rational, the engine has 11 `st_fps` values (`st_api.h:61`); GI-7 |
| I-REQ-33 | Video MIB 0x0001 incl. colorimetry, TCS, range, PAR, measured pixel clock, htotal, vtotal | TR-10-2 §10 | MUST | GAP GI-2 (N1 G-N10 adds colorimetry/TCS/range) |
| I-REQ-34 | Non-baseband substitutes: htotal = width, vtotal = height, pixclk = w·h·rate; measuredsamplerate = nominal | TR-10-9 §10 | MUST | GI-2 helper defaults |
| I-REQ-35 | SDP, NMOS and SR consistent; block version increments on change | PQCR §4.1.1, TP-1 §13.3 | MUST | GAP GI-2 |
| I-REQ-36 | Interlaced: RTP per field, I and S bits | TR-10-2 §9–10 | MUST | `MTL_INTERLACED`, `MTL_PSF` |
| I-REQ-37 | 48 kHz L16 or L24 (receivers both); 44.1 kHz L16, 96 kHz L24 | TR-10-3 §8 | MUST / SHOULD | `MTL_PCM16`, `MTL_PCM24`, `audio.sample_rate` |
| I-REQ-38 | Receive ST 2110-30 level A at any channel count | TR-10-3 §10 | MUST | `audio.channels`, 1 ms default ptime |
| I-REQ-39 | PCM MIB 0x0002 (rate, size, channels, ptime µs, measuredsamplerate, channel-order) | TR-10-3 §11 | MUST | GAP GI-2 |
| I-REQ-40 | AES3 per ST 2110-31, 48 kHz, MIB 0x0004 | TR-10-12 §7–10 | MUST (if AES3) | `MTL_AM824`; MIB GI-2 |
| I-REQ-41 | ANC per ST 2110-40 §5.1/5.2/5.5, 90 kHz, SR with Info Block | TR-10-4 §7–10 | MUST | `MTL_ANC`; SR GI-1 |
| I-REQ-42 | InfoFrames: ST 2110-41, DIT 0x100100, K=0, port media+3, RTP of the video | TR-10-10 §5–7 | MUST (if InfoFrames) | `MTL_FASTMETA`, `fastmeta.data_item_type`, `k_bit`, shared timeline |
| I-REQ-43 | ≥ 1 InfoFrame packet per frame/field, before the video's first packet; Null block when empty | TR-10-10 §9, §12 | MUST (if InfoFrames) | PARTIAL: no order across sessions; GI-14 |
| I-REQ-44 | CBR compressed per ST 2110-22 §4/§6, TR-10-1 NCM, no VRX | TR-10-11 §7–11 | MUST | `MTL_CVIDEO_CBR`; CMAX GI-6 |
| I-REQ-45 | MIB 0x0003, plus 0x0008 (T, P, Ppih, Plev) for JPEG XS | TR-10-11 §12, TR-10-15-1 §9 | MUST | GAP GI-2 |
| I-REQ-46 | JPEG XS per RFC 9134, High444.12, 4:2:2/10 and RGB 4:4:4/8 | TR-10-15-1 §7–8 | MUST (if JPEG XS) | `MTL_CODEC_JPEGXS` + plugin; slice mode reserved (`MTL_CVIDEO_PACK_SLICE`) |
| I-REQ-47 | VBR compressed: ST 2110-22 §6 payload, CMAX from MaxRate, `b=AS`, `TP=2110TPW` | TR-10-7 §8–11 | MUST (if VBR) | PARTIAL: `MTL_CVIDEO_VBR_MAX` flagged non-compliant, no max-rate field; GI-8 |
| I-REQ-48 | H.264/H.265 packetised per RFC 6184/7798, no PACI | TR-10-15-2/3 | MUST (if codec) | PARTIAL: `MTL_UNIT_PACKETS` only; GI-8 |
| I-REQ-49 | MIB 0x0005 (+ 0x0009/0x000A) | TR-10-7 §12 | MUST | GAP GI-2 |
| I-REQ-50 | FEC Profile A: column FEC on +2, row on +4, shaping, partial matrices | TR-10-6 §7 | MAY | GAP GI-15 (later) |
| I-REQ-51 | Receivers tolerate FEC streams | TR-10-6 §7 | MUST | yes: flows filter by UDP port |
| I-REQ-52 | PEP: AES-128-CTR mandatory; CTR/CMAC-64/ECDH variants optional | TR-10-13 §13, §20 | MUST (if PEP) | GAP GI-10 |
| I-REQ-53 | RTP header, extensions and payload header in clear; payload in 16-byte slices; IV = iv' ‖ ctr, ctr +1 per slice | TR-10-13 §20, §20.2 | MUST (if PEP) | GAP GI-10 |
| I-REQ-54 | CTR Full extension (key_version, 64-bit ctr) on each frame/field/slice/audio-packet start and on non-A/V packets, Short (24-bit) otherwise; gap < 2^24 | TR-10-13 §20.1–20.2 | MUST (if PEP) | GAP GI-10, GI-9 |
| I-REQ-55 | MAC modes: CMAC-64, mac-then-encrypt in the payload's last 8 bytes | TR-10-13 §15, §20.2 | MAY | GAP GI-10 (copy path) |
| I-REQ-56 | RTP_KV: key version changes at frame/field/GOP or audio-packet boundaries; ctr restarts per key | TR-10-13 §20.3 | MAY | GAP GI-10 (`when` on the key) |
| I-REQ-57 | Same PEP parameters on both ST 2022-7 legs; RTCP not encrypted | TR-10-13 §13, §20 | MUST (if PEP) | GI-10 encrypts once per unit; GI-1 sends RTCP clear |
| I-REQ-58 | PEP MIB 0x0011 (privacy_version, f_id, s_id) | TR-10-13 §22 | MUST (if PEP) | GAP GI-2 |
| I-REQ-59 | PSK store, KDF (CMAC / HMAC-SHA-512/256), ECDH, parameters in SDP and IS-05 | TR-10-13 §12–13; BCP-005-03 | MUST (if PEP) | application, control plane; GI-10 takes the derived key |
| I-REQ-60 | HDCP Full/Short IV Counters extension first in every packet (frz, streamCtr, inputCtr); headers clear | TR-10-5 §14–15 | MUST (if HDCP) | GAP GI-11; packet units can carry it today |
| I-REQ-61 | Receivers watch streamCtr and re-run HDCP on a change | TR-10-5 §14.1 | MUST (if HDCP) | GAP GI-9 (extension values on RX) |
| I-REQ-62 | HKEP over TCP, `a=hkep`, MIB 0x0010 | TR-10-5 §10–12, §16 | MUST (if HDCP) | application; MIB GI-2 |
| I-REQ-63 | DHCP by default on every interface, manual fallback | TR-10-9 §14 | MUST | `mtl_port_spec.sip` zero = DHCP (kernel), option `port.dhcp` (DPDK) |
| I-REQ-64 | DSCP defaults AF42 (video, ANC, compressed), AF41 (audio, AES3), EF (PTP); user-selectable | TR-10-9 §16 | MUST | `mtl_flow.dscp` (0 = CS0); defaults GI-16 |
| I-REQ-65 | IPv4 multicast; IGMPv3 SSM with the SDP source; IGMPv2 too; version user-selectable | TR-10-9 §17 | MUST | `mtl_flow.source_filter`; IGMPv2 GAP GI-12 (v3 reports only, `mt_mcast.c:215`) [verified, code] |
| I-REQ-66 | Address ranges; default group 239.S.C.D | TR-10-9 §17, §17.1 | MUST | application |
| I-REQ-67 | Frame-to-frame interval of first packets and of SRs: max − min ≤ 2 ms over 2 s | TR-10-9 §11.2 | MUST | pacing meets it; async units carry the app's jitter; gauge GI-17 |
| I-REQ-68 | Receivers pick the sync method from ts-refclk and mediaclk; support `direct=0` and `sender` | TR-10-9 §11, §11.1 | MUST | option `rx.mediaclk` DIRECT/SENDER; AUTO from the SR GI-3 |
| I-REQ-69 | Streams that share a reference clock are aligned by their timestamps | TR-10-9 §13 | MUST | `mtl_rx_align()`; sender-clock streams GI-3 |
| I-REQ-70 | Watch PTP state and update the clock source description | TR-10-9 §12 | SHOULD | `MTL_EVENT_TIME_STATE`, `time.grandmaster_id`; Info Block follows GI-2 |
| I-REQ-71 | In-band NMOS and HKEP on the media port (TCP, mDNS, DHCP reach the OS) | TR-10-9 §18–19 | MUST (if both bands) | PARTIAL: `port.virtio_user`, kernel backends; Q-I-9 |
| I-REQ-72 | Unicast streams; SR to the unicast destination | TR-10-1 §8.7, TR-10-5 §9.1 | MUST | `mtl_flow.ip` unicast with ARP or `MTL_FLOWF_USER_MAC`; SR GI-1 |
| I-REQ-73 | One network is the common case; ST 2022-7 optional | TR-10-1 §6 | MAY | `flows[1]` optional |
| I-REQ-74 | Off-subnet unicast through a gateway | [inferred] | SHOULD | `mtl_port_spec.gateway` |
| I-REQ-75 | USB over IP | TR-10-14 | MAY | out of scope: TCP, not a media flow |
| I-REQ-76 | HDR MIB 0x0006 after the colorimetry MIB, may change per field | TR-10-16 §7 | MAY | GAP GI-2 (per-unit MIB) |

## 4. Gaps and proposed API changes

Each change is additive. "DP cost" is what the change adds on a pinned core.

| Gap | Header | Proposed symbol | Contract (one line) | DP cost |
|---|---|---|---|---|
| GI-1 | new `mtl_rtcp.h`, `mtl_options.h` | `rtcp.sr` (320, enum OFF/RFC3550/IPMX), `rtcp.cname` (321, str), `rtcp.dst_port` (322; 0 = udp_port + 1), `rtcp.sr_interval_ms` (323) | TX sends SR + SDES CNAME per leg to the flow's IP and `dst_port`, same DSCP and TTL; IPMX adds the Info Block and the TR-10-1 schedule, queued before the unit's first packet | O(1) from a template: ≈ 100 ns per frame |
| GI-2 | `mtl_rtcp.h`, `mtl.h`, `mtl_queue.h` | `struct mtl_rtcp_info`, `mtl_rtcp_set_info()`, `mtl_rtcp_get_info()`, `MTL_META_RTCP_MIB = 3`, `MTL_EVENT_RTCP_INFO = 26`, MIB builders | the library writes tag, length, version, ts-refclk and mediaclk (AUTO unless set) and appends the app's MIBs; the version increments when the bytes change and the event is posted | none (template swap) |
| GI-3 | `mtl_rtcp.h`, `mtl.h`, `mtl_options.h` | `mtl_rtcp_read()`, `MTL_WAIT_RTCP 0x20u`, `MTL_UNITF_SENDER_TIME 0x800000u`, `MTL_MEDIACLK_AUTO = 3`, `rtcp.rx` (324, bool) | RX receives SRs on port+1, keeps the latest per leg, maps each unit's RTP to the sender's Internal Clock and sets `media_tai_ns` with SENDER_TIME | none on the system queue (default); ≈ 50 ns per SR on a session queue |
| GI-4 | `mtl.h` | `MTL_MEDIA_SENDER = 3` in `enum mtl_media_mode`; `MTL_INFO_MEDIACLK_SENDER 0x2u` | TX: `media_tai_ns` = sampling instant on the instance clock, never snapped; RTP = RTP0 + floor(k·P·R), RTP0 = floor(M0·R); launch = M + `min_tx_delay_ns`; SR NTP = M (§5) | none |
| GI-5 | `mtl.h`, `mtl_options.h` | `MTL_TIME_SOURCE_FREERUN = 6`; AUTO contract; `time.fallback` (2208, enum HOLDOVER/FREERUN) | FREERUN: TSC or undisciplined PHC seeded once from CLOCK_REALTIME + UTC offset, never stepped, ESTIMATED. AUTO: PTP while locked, else FREERUN from the last value, re-evaluated at runtime; switches post `TIME_STATE` (and `TIME_STEP`) | none |
| GI-6 | `mtl.h` | `MTL_SENDER_IPMX = 3`; `uint16_t vtotal, htotal` from `mtl_video_config.reserved[]` (N1 G-N10 uses `reserved0`) | gapped read schedule with RACTIVE = height/vtotal (0 = ST 2110-21 default), CMAX = max(16, int(Npkts/(21600·TFRAME))) or from `max_bitrate_bps` for cvideo VBR, IPMX VRX of TR-10-1 §8.1 | none: computed at create |
| GI-7 | `mtl.h` (contract), engine | text on `mtl_raster` | any width and height ≤ 32767 (RFC 4175 fields), any `fps` with num ≤ 4194303 and den ≤ 1023 (the MIB fields); the engine replaces the `st_fps` table with rationals | none |
| GI-8 | `mtl.h`, `mtl_options.h` | `uint64_t max_bitrate_bps` in `mtl_cvideo_config.reserved[0]`; later option `cvideo.packetization` (515, enum RFC9134/RFC6184/RFC7798) | VBR_MAX is compliant under `session.profile=ipmx` (TR-10-7); H.26x packetisation by the library is later; until then `MTL_UNIT_PACKETS` | none |
| GI-9 | engine; `mtl_packet.h` | RX honours X and CC bits; `MTL_PKTE_HDR_EXT 0x40u` | RX skips CSRCs and RFC 8285 extensions before the payload header; packet units keep `data` at the RTP header and flag extensions | +1 branch per packet |
| GI-10 | new `mtl_crypto.h` | `struct mtl_ext_crypto` (first `next` block), `mtl_crypto_set_key()`, `mtl_crypto_clear_keys()` | PEP RTP and RTP_KV: the library writes the CTR extensions and counters, encrypts in the caller at submit (DPC) or on crypto workers, decrypts at dequeue; keys are never options (§6) | tasklet: 8 or 20 B of extension per packet |
| GI-11 | `mtl_crypto.h`, later | `MTL_CRYPTO_EXTERNAL`, a cipher kind in `mtl_plugin.h` | HDCP through a vendor cipher plugin (keys never in MTL); until then packet units with app-written extension and payload | as GI-10 |
| GI-12 | `mtl_options.h` | `port.igmp_version` (2123, per port, enum AUTO/V2/V3) | AUTO = v3 with the RFC 3376 v2 compatibility mode after a v2 querier; V2 drops SSM (`source_filter` checked in software) | none |
| GI-13 | `mtl_options.h`, `mtl_observe.h` | `rx.link_offset_ns` becomes R; value `MTL_LINK_OFFSET_AUTO = -1`; gauges `rx.link_offset_min_ns`, `rx.link_offset_max_ns` | applied at the next unit; AUTO = the current minimum; min = high percentile of (unit complete − media time) + delivery; max = pool depth × unit period − one unit | gauges from existing per-unit times |
| GI-14 | `mtl_options.h` | `tx.precede` (113, str: a session name) | this session's unit k is enqueued on the named session's queue right before that session's unit k (same timeline): InfoFrames before video | one enqueue per frame on the video tasklet |
| GI-15 | `mtl_options.h`, later | `fec.profile` (330, enum NONE/IPMX_A) | ST 2022-5 column FEC per TR-10-6 on port +2 with its shaping and partial matrices; RX recovery | XOR into 2 accumulators per packet (≈ 1 % of a core at 1080p60 [inferred]) |
| GI-16 | `mtl_options.h` | `session.profile` (306, enum ST2110/IPMX), `instance.profile` (2021) | IPMX changes defaults only: `rtcp.sr=ipmx`, `rtcp.rx=1`, `MTL_SENDER_IPMX`, DSCP AF42/AF41, `rx.mediaclk=auto`, port checks, compliance labels | none |
| GI-17 | `mtl_observe.h` | `tx.f2f_pp_ns` G, `tx.rtcp_sr` C, `rx.rtcp_sr` C, `rx.rtcp_info_version` G, `rx.sender_rate_ppb` G, `rx.crypto_auth_fail` C | f2f = max − min of first-packet intervals over 2 s (TR-10-9 §11.2); sender rate from RTP vs arrival over a window | counters on existing paths |

### 4.1 `mtl_rtcp.h` (sketch)

```c
/* mtl_rtcp.h - RTCP sender reports and the IPMX Info Block (optional header). */
#define MTL_RTCP_REFCLK_APP 0x1u   /* use ts_refclk as given; default: from the time state */
#define MTL_RTCP_MEDIACLK_APP 0x2u /* use mediaclk as given; default: "direct=0" or "sender" */
struct mtl_rtcp_info {             /* input, and output of mtl_rtcp_get_info() */
  uint32_t struct_size;
  uint32_t flags;          /* MTL_RTCP_*_APP */
  char ts_refclk[64];      /* "ptp=IEEE1588-2008:ec-46-70-ff-fe-10-ff-b0:127", "localmac=..." */
  char mediaclk[12];       /* "direct=0", "sender" */
  uint32_t mib_bytes;      /* Media Info Blocks, each 32-bit aligned, in order */
  const void* MTL_NULLABLE mib;
  uint32_t version;        /* output: the block version on the wire */
  uint32_t reserved0;
  uint64_t reserved[4];
};
/* Applies from the next SR (TX). Unit meta MTL_META_RTCP_MIB replaces the MIBs for that unit's SR
   only (HDR per field, TR-10-16). The library increments the version when the bytes change and posts
   MTL_EVENT_RTCP_INFO (new_value = version). CP. */
MTL_API_CP int mtl_rtcp_set_info(mtl_session_h s, const struct mtl_rtcp_info* info);
MTL_API_CP int mtl_rtcp_get_info(mtl_session_h s, struct mtl_rtcp_info* out);

#define MTL_RTCP_SR_INFO 0x1u     /* an IPMX Info Block was present */
#define MTL_RTCP_SR_CHANGED 0x2u  /* its version differs from the previous SR's */
struct mtl_rtcp_sr {              /* output */
  uint32_t ssrc, rtp, packets, octets;
  int64_t sender_ns;              /* NTP field as PTP time: seconds unwrapped near now, plus ns */
  int64_t arrival_tai_ns;         /* local, software */
  uint32_t leg, flags, info_version, info_bytes; /* info_bytes copied into buf, up to cap */
};
/* The next received SR (oldest first; a full ring drops the oldest and counts it). DPC. */
MTL_API_DPC int mtl_rtcp_read(mtl_session_h s, struct mtl_rtcp_sr* sr, size_t sr_size,
                              void* MTL_NULLABLE buf, size_t cap, int64_t timeout_ns);
/* MIB builders (AS, pure): 0x0001/0x0003/0x0005 from a video or cvideo config plus the strings, 0x0002/
   0x0004 from an audio config; zero htotal/vtotal/pixclk/measured rate take the TR-10-9 §10 values. */
MTL_API_AS int mtl_rtcp_mib_video(const struct mtl_session_config* sc, uint16_t type,
                                  const struct mtl_rtcp_video_desc* d, void* buf, size_t cap);
MTL_API_AS int mtl_rtcp_mib_audio(const struct mtl_session_config* sc, uint16_t type,
                                  uint32_t measured_rate, const char* channel_order, void* buf,
                                  size_t cap);
MTL_API_AS int mtl_rtcp_info_parse(const void* blk, size_t len, struct mtl_rtcp_info* out,
                                   const void** mib, uint32_t* mib_bytes);
```

`struct mtl_rtcp_video_desc` holds what the session config does not: PAR, measured pixel clock, and the
colorimetry/TCS/range strings when N1's G-N10 enums are absent. The library never interprets MIB bytes it
did not build; it only checks 32-bit alignment and that SR + Info Block + SDES fit one datagram.

**Where SRs are built.** Off the tasklet, a control thread prepares the compound packet (SR header, Info
Block, SDES) into one of two template buffers and publishes it with a release store. The tasklet, when it
dequeues unit k, copies the template into an mbuf, writes NTP (from M), RTP, packet and octet counts, and
enqueues it ahead of the unit's first packet on the same queue, so the TR-10-1 order holds by queue order.
With rate-limit pacing the SR takes one packet slot in the shaped queue (≈ 1 µs at 1080p); it is not a
media packet for VRX. On ST 2022-7 each leg sends its own copy. The sender does not listen on port+1.

### 4.2 `mtl_crypto.h` (sketch)

```c
enum mtl_crypto_scheme { MTL_CRYPTO_PEP_RTP = 1, MTL_CRYPTO_PEP_RTP_KV = 2, MTL_CRYPTO_EXTERNAL = 3 };
enum mtl_crypto_mode {
  MTL_AES128_CTR = 1, MTL_AES256_CTR = 2,               /* AES-128-CTR is mandatory (TR-10-13 §20) */
  MTL_AES128_CTR_CMAC64 = 3, MTL_AES256_CTR_CMAC64 = 4, /* MAC modes: copy path, §6 */
  MTL_AES128_CTR_CMAC64_AAD = 5, MTL_AES256_CTR_CMAC64_AAD = 6,
};
#define MTL_CRYPTO_IN_PLACE 0x1u /* TX may overwrite app memory with ciphertext (attached pools) */
struct mtl_ext_crypto {          /* a mtl_session_config.next block, read at create */
  uint32_t struct_size;
  uint32_t kind;                 /* MTL_EXT_CRYPTO */
  const void* MTL_NULLABLE next;
  uint32_t scheme, mode, flags;
  uint8_t ext_id_full, ext_id_short; /* RFC 8285 IDs, as in a=extmap */
  uint16_t clear_bytes;          /* payload bytes in clear; 0 = the essence's payload header */
  uint8_t iv[8];                 /* base iv' (TR-10-13 §20) */
  uint32_t substream;            /* added to iv' (0..1023) */
  uint32_t reserved0;
  uint64_t reserved[4];
};
/* Install key `key_version` (16 or 32 bytes), copied into locked, non-dumpable memory. TX: used from
   the first unit at or after `when` (NULL = next unit); ctr restarts at 0. RX: kept with the current
   key and chosen per packet by the extension's key_version (RTP_KV) or by the latest set (RTP). The
   key is never readable back, logged or exported. CP. */
MTL_API_CP int mtl_crypto_set_key(mtl_session_h s, uint32_t key_version, const uint8_t* key,
                                  uint32_t key_bytes, const struct mtl_when* MTL_NULLABLE when);
MTL_API_CP int mtl_crypto_clear_keys(mtl_session_h s); /* zeroises; TX then fails units (NO_KEY) */
```

### 4.3 Smaller items

- **GI-4** sits beside the existing modes so `MTL_SUBMIT_RTP_TS` keeps working: an inline processor submits
  `unit.rtp` from the received unit and `media_tai_ns` = its `MTL_UNITF_SENDER_TIME` value, so the output
  SR keeps the input's time domain (TR-10-1 §9); it also copies the input's refclk and mediaclk strings with
  `MTL_RTCP_*_APP`. Late policy in SENDER mode: SEND_LATE within `tx.late_tolerance_ns`, then DROP.
- **GI-5** keeps `MTL_TIME_SOURCE_PTP_BUILTIN` but documents it as not IPMX-compliant until it runs BMCA
  over Announce messages (engine work); `MTL_TIME_SOURCE_PHC` with ptp4l in follower-only ST 2059-2 mode is
  the IPMX recipe. A PTP leader is outside MTL.
- **GI-9** is a bugfix with no API: the RX parsers read fixed offsets and the library never reads the X or
  CC bits (only TX writes them as 0, e.g. `st_tx_video_session.c:959-960`) [verified, code], so any stream
  with an HDCP or PEP extension, encrypted or not, would be mis-parsed.
- **GI-12**, **GI-16**: the zero-default rule (D-22) makes DSCP 0 = CS0; the profile option is the way to get
  IPMX defaults without per-field code in every application.

## 5. Timing without PTP

### 5.1 The IPMX clock model

TR-10-1 has four clocks [verified]: the **Common Reference Clock** (PTP, when present); each device's
**Internal Clock**, synchronised to it when present and free-running otherwise (§7); the **Media Clock** of
a stream, locked to the source's sample or frame rate (§8.4); and the **RTP Clock**, advanced by the media
clock and sampled for timestamps. At the first timestamp the RTP clock is set from the Internal Clock (§8.6),
so a sync source gives ST 2110-10 timestamps. An async source's RTP then advances at the source's rate,
which drifts against the Internal Clock; the SDP says `mediaclk:sender` (§10.5) and the SRs carry
(Internal Clock, RTP) pairs, sampled at the same VSYNC (§8.8.1) or audio sampling instant (§8.10.1).

How it differs from ST 2110: in ST 2110 every sender is PTP-locked and RTP = floor(TAI × rate) with zero
offset (ST 2110-10 §7.3), so a receiver aligns streams from any sender by RTP alone and paces from TAI.
IPMX receivers instead learn the sender's time from the SRs; streams from one sender align through its
Internal Clock even without PTP, and streams from different senders need a common clock (Appendix A) [verified].

### 5.2 Mapping onto revision 4

| Case | Time source | Media mode | Source kind | SDP mediaclk / ts-refclk | Revision 4 today |
|---|---|---|---|---|---|
| file playout or test pattern, PTP present | `PHC` or `PTP_BUILTIN` | `INDEX` or `AUTO` | PLAYBACK | `direct=0` / `ptp=...` | covered |
| file playout, no PTP (TR-10-9 §9: a sync sender) | `FREERUN` (GI-5) | `INDEX` or `AUTO` | PLAYBACK | `direct=0` / `localmac=` | covered with `SYSTEM_TAI` (but NTP steps); GI-5 |
| genlocked capture locked to PTP | `PHC` | `TAI` (NEAREST) or `INDEX` | CAPTURE, GATEWAY | `direct=0` / `ptp=...` | covered |
| HDMI capture at its own rate, PTP or not | any | `SENDER` (GI-4) | CAPTURE, GATEWAY | `sender` / `ptp=...` or `localmac=` | GAP: TAI + NEAREST drops or repeats a frame every 1/ppm frames |
| inline processor keeping input timing | any | `SENDER` + `MTL_SUBMIT_RTP_TS` | GATEWAY | as the input | PARTIAL (RTP yes, SR no) |

**Does the design support a free-running sender clock not tied to TAI?** For sync senders, yes in shape:
the instance clock is whatever the time source publishes, R5's "TAI ns" is then the Internal Clock with
`MTL_TIMEF_ESTIMATED`, and RTP = floor(M × R) on the epoch timeline is exactly TR-10-1 §8.6 for a sync
source. Two pieces are missing: a clock that does not step with NTP (`SYSTEM_TAI` follows `CLOCK_REALTIME`)
and the runtime switch between free-run and PTP (GI-5). For async sources, no: every TX RTP is derived from
an instance-clock media time, snapped to the nominal grid. A source at +50 ppm on a 59.94 session drifts
0.83 µs per frame, so NEAREST drops one frame every 1/50 ppm = 20 000 frames (5.6 min), and an audio source
at −1000 ppm gets 48 samples per second absorbed or inserted. IPMX avoids both by letting RTP follow the
source; that needs GI-4.

### 5.3 Worked example: an async HDMI source, no PTP

Sender: 1080p59.94 video and 48 kHz audio from one HDMI input, no grandmaster, `MTL_TIME_SOURCE_FREERUN`,
`media_mode = MTL_MEDIA_SENDER`, CAPTURE. The video source runs 50 ppm fast; the audio clock measures 47952 Hz
(TR-10-3 §12's example). Numbers computed exactly (rational arithmetic) [verified, computed].

1. First VSYNC, read on the Internal Clock: M0 = 1 700 000 000.123 456 789 s. RTP0 = floor(M0 × 90000) mod 2^32
   = 153 000 000 011 111 mod 2^32 = **380 025 703** (TR-10-1 §8.6).
2. Frame k's RTP = RTP0 + floor(k × 1501.5): 380 025 703, 380 027 204, 380 028 706, 380 030 207, ... (TP-1 §13.3
   expects increments of 1501.5 at 59.94). It does not depend on when the VSYNC happens: the RTP clock is
   locked to the source, one nominal frame of ticks per frame.
3. The VSYNCs come every 16.683 333 ms / (1 + 50 ppm) = 16 682 499 ns: M1 = …140 139 288 ns, M2 = …156 821 787 ns.
   The app submits frame k with `media_tai_ns = Mk`.
4. SR for frame k (before its first packet): SSRC, NTP MSW = 1 700 000 000 (seconds, low 32 bits), NTP LSW =
   140 139 288 (ns, PTP truncated format, not an NTP fraction) for k = 1, RTP = 380 027 204, packet and octet
   counts, then the Info Block with ts-refclk `localmac=<port MAC>` and mediaclk `sender`, the 0x0001 MIB
   (`measuredpixclk` 148 359 066 Hz = 148.5 MHz × 1000/1001 × (1 + 50 ppm), htotal 2200, vtotal 1125), and SDES CNAME.
5. First packet of frame k at Mk + `min_tx_delay_ns` (CAPTURE default one frame plus pick-up lead, or less
   with GATEWAY rows), then the IPMX gapped schedule at TRS = TFRAME × (1080/1125) / Npkts.
6. After 600 frames, SR600 has RTP 380 926 603 and NTP 1 700 000 010.132 956 314. A receiver computes
   ΔRTP/ΔNTP = 900 900 / 10.009 499 525 s = 90 004.5 Hz: the source runs +50 ppm against the sender's
   Internal Clock.
7. Audio, 125 µs ptime (6 samples per packet): first sample at M0 + 2 ms, RTP0 = floor(that × 48000) mod 2^32 =
   4 211 316 613. SRs every 80 packets: RTP +480 each, NTP +480/47952 s = +10 010 010 ns each
   (…125 456 789, …135 466 799, …145 476 809). The MIB says 48000 Hz nominal, measuredsamplerate 47952.

A PTP-locked sync sender at the same instant would stamp floor(N·TFRAME·90000) = 380 024 949 on the grid
frame N = 101 898 101 905; the async sender is 754 ticks (8.4 ms) off that grid, which is legal with
`mediaclk:sender`.

### 5.4 Receiver behaviour

- **Clock recovery** (TR-10-1 §11.1, SHOULD): from successive SRs a receiver gets the sender's media rate in
  sender-Internal-Clock terms (step 6); from RTP against arrival times it gets the rate against its own clock.
  A receiver driving HDMI out locks its output pixel clock to that (the app's job); MTL gives the SR records
  (`mtl_rtcp_read`), the gauge `rx.sender_rate_ppb` and, per unit, the sender time (`MTL_UNITF_SENDER_TIME`).
- **Buffering**: the IPMX VRX is 2·CMAX packets (32 for HD, Appendix A) and starts at half full, so a
  receiver needs no grid and no TAI; MTL's slot reassembly already works on arrival. Link offset needs care:
  without a shared clock, "media time + D_LO" can only be measured against the first-arrival mapping of
  sender time to local time (Q-I-6).
- **Choosing the method** (TR-10-9 §11.1): use the SRs when ts-refclk is `localmac=` or a PTP clock the
  receiver is not locked to; use PTP + SRs when both share the GM. `rx.mediaclk = auto` (GI-3) does that
  comparison from the Info Block and `time.grandmaster_id`.
- **Alignment** (TR-10-9 §13): streams whose ts-refclk is the same can be aligned by mapping each RTP to the
  sender clock; `mtl_rx_align()` works on `media_tai_ns`, so it works for SENDER_TIME units of one sender.

### 5.5 Clock source changes

When a grandmaster appears, the AUTO source of GI-5 locks to it and the Internal Clock steps by the offset;
`TIME_STATE` and `TIME_STEP` are posted, ts-refclk changes from `localmac=` to `ptp=...`, the Info Block
version increments and `MTL_EVENT_RTCP_INFO` tells the app to update SDP and NMOS (TR-10-9 §12, PQCR §4.1.1).
SENDER sessions keep their RTP (it follows the source); their SR NTP values jump. INDEX and AUTO sessions on
the epoch timeline follow their step policy (06 §2.4): the RTP moves to the PTP grid, which receivers see as
a discontinuity. Losing the grandmaster goes to HOLDOVER and then, with `time.fallback = freerun`, to FREERUN
without a step.

## 6. Encryption and HDCP: placement and cost

### 6.1 Where it sits in a packet

```text
| Eth 14 | IPv4 20 | UDP 8 | RTP 12, X=1 | RFC 8285 ext: 0xBEDE + len (4) + CTR Short (4) or Full (16) | payload header (clear) | payload: 16-byte slices, last may be partial | [MAC 8, MAC modes] |
```

The payload header stays in clear (RFC 4175 extended sequence and SRDs, RFC 9134's 4 bytes, RFC 8331's
header; audio has none) [verified, TR-10-13 §20]. Packet i of a unit gets ctr_i = ctr of its first slice;
ctr_(i+1) = ctr_i + ceil(len_i / 16) [inferred: TR-10-13 counts every slice, partial ones included]. The Full
extension (key_version and the 64-bit ctr) goes on the first packet of a frame, field, slice or audio packet;
the rest carry the low 24 bits. HDCP (TR-10-5 §14) uses the same structure with its own Full/Short IV
Counters (frz, streamCtr, inputCtr); PEP can encrypt an HDCP stream in place reusing inputCtr (TR-10-13 §20.4).

Consequences for the library: payload sizing must subtract the extension (8 B on most packets, 20 B on the
first) and, with MAC modes, 8 B more, from the 1460 B Standard UDP Size Limit; packets per frame grow by about
0.7 % at 1200 B payloads; ST 2022-7 legs carry identical ciphertext, so a unit is encrypted once.

### 6.2 Who does what

| Function | Library | Application or NMOS layer |
|---|---|---|
| PSK storage and provisioning, key_id, KDF (CMAC, HMAC-SHA-512/256), ECDH, iv and key_generator choice | no | yes (TR-10-13 §12, §17; BCP-005-03) |
| `a=privacy`, `a=extmap`, IS-05 `ext_privacy_*`, PEP MIB | values from `mtl_ext_crypto` | writes them (GI-2 builds the MIB) |
| install keys at activation, rotate at a boundary | `mtl_crypto_set_key(..., when)` | decides when (IS-05 activation, RTP_KV schedule) |
| write and parse the CTR extensions, counter bookkeeping, payload sizing | yes | no |
| AES-CTR (and CMAC) over payloads | yes, off the tasklet | or the app encrypts itself with packet units |
| HDCP: AKE, HKEP over TCP, session key, lc128 | no (licensed secrets) | vendor code; cipher via GI-11 or packet units |

### 6.3 Where the cipher runs

The pinned-core rule forbids per-byte work on tasklets: AES over a 5 MB frame is milliseconds of one core.

| Path | TX | RX |
|---|---|---|
| frame and row units, library pool | in the caller during `mtl_tx_submit` (DPC), in place, per packet region (the library knows the layout); rows: per submitted range | the tasklet lands ciphertext and stores each packet's ctr in the slot's packet table (8 B per packet); `mtl_rx_dequeue` decrypts in place in the caller (DPC), then zero-fills gaps; rows: in `mtl_rx_wait_rows` |
| app memory (attached pools) | copy to a library shadow slot while encrypting, unless `MTL_CRYPTO_IN_PLACE`; `MTL_SESSION_REQUIRE_DIRECT` without IN_PLACE fails at create | as library pools (ciphertext is in the app's slot until dequeue) |
| `unit.hold` (RX→TX zero copy) | copy path (in place would corrupt the shared RX slot) | — |
| packet units | per packet at submit (DPC); or the app encrypts and writes its own extension with `packet.set_fields` verbatim | `MTL_PKT_RX_LEND` + app decrypt, or library decrypt at dequeue |
| MAC modes | MAC computed and appended per packet: payload regions are no longer contiguous with the frame, so always the copy path | the 8 MAC bytes interleave with pixels: packets go through a staging copy; a failed MAC counts `rx.crypto_auth_fail` and the packet is treated as lost |
| offload | option `crypto.workers` (340): library threads encrypt between submit and pick-up, adding their time to `min_submit_lead_ns` | the same for decrypt before delivery |

Results and timing stay lossless (R-rules): a unit whose key is missing is DROPPED with reason `NO_KEY`, not
sent in clear; a RX packet that fails authentication is a lost packet, counted.

### 6.4 Cost

| Item | Cost [inferred unless noted] |
|---|---|
| AES-128-CTR with AES-NI / VAES | ≈ 1 / ≈ 0.3 cycle per byte per core |
| 1080p59.94 YCbCr 4:2:2 10-bit (5.18 MB per frame, 311 MB/s) [computed] | 0.03–0.10 of a 3 GHz core, TX or RX |
| 2160p60 YCbCr 4:2:2 10-bit (1.24 GB/s) / RGB 8-bit (1.49 GB/s) [computed] | 0.12–0.50 of a core |
| CMAC-64 modes | about the same again, serial within a packet (multi-buffer across packets) |
| tasklet, TX | extension write per packet (2 stores into the header mbuf), sizing at create |
| tasklet, RX | extension parse (+1 branch) and an 8 B ctr store per packet |
| wire | +8 B per packet (≈ 0.7 % at 1200 B), +20 B once per frame |

The caller's submit time grows by the cipher time, which moves `min_submit_lead_ns` for CAPTURE and GATEWAY
sources; `mtl_session_info` reports it. A spike should measure the real cycles per byte with DPDK's
ipsec-mb or OpenSSL on the target CPUs before the defaults are fixed.

### 6.5 Keys and rotation

PEP RTP: one key per activation; RTP_KV: `key_version` may increase by one at a frame, field or GOP boundary
(audio: packet), ctr restarts at 0 (TR-10-13 §20.3). `mtl_crypto_set_key(s, v + 1, key, 16, &when)` with
`MTL_AT_INDEX` gives an exact unit boundary; the tasklet switches with the unit, the caller-side cipher
picks the key by unit index, so no lock is shared. RX keeps the current and next key and selects by the
`dynamic_key_version` in the Full extension. Keys are not options (options are readable, R7): they live in
`mlock`ed, `MADV_DONTDUMP` memory, are zeroised on replace and close, and never reach logs, stats or capture
files (capture records ciphertext).

### 6.6 HDCP

HDCP 2.3's cipher is AES-128 in counter mode too, so the data path above fits it [inferred]. The key path does
not: the session key and lc128 are licensed secrets under DCP robustness rules, which an open-source library
should not hold [inferred]. Hence GI-11: a cipher plugin kind (vendor binary, keys inside it) called by the
library's crypto workers, and until then HDCP senders use packet units with the vendor encrypting payloads
and writing the HDCP extension. The library's part is header-extension tolerance and exposure on RX (GI-9),
so a receiver can watch streamCtr (TR-10-5 §14.1).

## 7. Conflicts with the current design

| ID | Current rule | IPMX need | Proposed resolution |
|---|---|---|---|
| C-I1 | R5: "Times are TAI ns" | the Internal Clock may be free-running, and RX sender times are another device's clock | keep `int64_t` ns; the instance clock is TAI when locked and ESTIMATED otherwise; sender-clock values carry `MTL_UNITF_SENDER_TIME` |
| C-I2 | D-09: RTP = floor(M × R) from media time, one rule for every essence | async RTP is frequency-locked to the source: RTP0 + count × ticks | SENDER mode is the one exception (GI-4); the other modes keep D-09 |
| C-I3 | TX grid: TVD = N·TFRAME + TRO, TAI units snapped NEAREST (06 §4.3) | async launch follows each VSYNC; no drop or repeat | SENDER mode launches from M; NEAREST stays the default for sync sources |
| C-I4 | `MTL_SUBMIT_EXACT` "non-compliant", `MTL_CVIDEO_VBR_MAX` "legacy, non-compliant", `MTL_INFO_NON_COMPLIANT` | free launch is the async model; VBR is TR-10-7's mode | compliance is reported against `session.profile` (GI-16) |
| C-I5 | `MTL_TIME_SOURCE_AUTO` chosen at open; `SYSTEM_TAI` follows NTP steps | switch free-run ↔ PTP at runtime; a free-running clock that does not step | GI-5 |
| C-I6 | sender types N, NL, W; W uses the linear read schedule (06 §5.2) | gapped schedule with W-sized CMAX and the IPMX VRX; receivers need not take linear senders | `MTL_SENDER_IPMX` (GI-6) |
| C-I7 | `rx.link_offset_ns` S (STOPPED only), no achievable range | change while active, `auto`, min/max constraints | GI-13 |
| C-I8 | options 310–314 `rtcp.*` = MTL's NACK retransmission (APP packets PT 204 named "IMTL", TX listening on dst_port+1, `mt_rtcp.c:393`, `st_tx_video_session.c:1041`) [verified, code] | RTCP means RFC 3550 SR on the same +1 port | rename the old keys `rtx.*`; SR and NACK share port+1, demultiplexed by PT |
| C-I9 | R7: every knob is an option | keys must not be readable | keys only through `mtl_crypto_set_key` |
| C-I10 | one meta kind per unit (`mtl_meta_hdr`) | per-frame MIB (HDR) and user meta together | allow two meta records per unit, or carry per-unit MIBs in a separate small area (Q-I-5) |
| C-I11 | NG3: no SDP in `lib/` | the SR Info Block is in-band SDP the library sends | the library emits bytes the app supplies (GI-2); NG3 holds |
| C-I12 | RX parses fixed RTP offsets | streams with header extensions or CSRCs | GI-9 engine fix |
| C-I13 | D-22 zero defaults: DSCP 0 = CS0, RTCP off | IPMX defaults AF42/AF41, SR on | `session.profile = ipmx` (GI-16) |
| C-I14 | `MTL_UPDATE_MEDIA` only in CREATED or STOPPED | an HDMI source changing format mid-stream (IS-11, EDID) bumps the block version | stop, update, start, `mtl_rtcp_set_info()`; a short gap is acceptable [inferred]; see N1 G-N6 |

## 8. Open questions

| ID | Question | Why it matters | Proposed answer |
|---|---|---|---|
| Q-I-1 | Library-built MIBs (needs colorimetry, TCS, range, PAR in the config) or app-supplied bytes? | consistency with SDP (PQCR) vs a smaller config | app bytes, with builders that read N1's G-N10 fields when they exist |
| Q-I-2 | Do TR-10-10 InfoFrame and TR-10-6 FEC streams need their own SRs? | TR-10-1 §8.7 says "IPMX Senders"; TR-10-10 is silent | ask VSF; default: no SR for FEC, SR without MIB for InfoFrames under the profile |
| Q-I-3 | SENDER mode after a source dropout: keep counting RTP or re-anchor RTP0? | receivers see either a gap or a jump | DISCONTINUITY re-anchors from the next M; otherwise one tick count per unit |
| Q-I-4 | May MTL hold HDCP keys at all? | DCP licensing and robustness rules | no; plugin or packet units (GI-11) |
| Q-I-5 | Per-unit MIB and user meta together | HDR plus app metadata on one frame | a second meta record per slot |
| Q-I-6 | Link offset without a shared clock | presentation = sender time + D_LO needs a local mapping | map sender time to local time by the minimum observed (arrival − sender time) and report it |
| Q-I-7 | Add BMCA to the built-in PTP client, or require ptp4l for IPMX? | TR-10-1 §7.2 MUST | require PHC + ptp4l in v1; BMCA later |
| Q-I-8 | RTCP receive path: a flow rule per session or the system queue? | flow-rule budget doubles per RX session | system (CNI) queue by default; session queue when hardware SR arrival times are needed |
| Q-I-9 | In-band control on a DPDK port: is `port.virtio_user` enough for TCP, mDNS and DHCP? | TR-10-9 §18–19 defaults | verify with an IPMX controller; mDNS needs 224.0.0.251 forwarded to the kernel |
| Q-I-10 | Should the library offer PEP key derivation helpers? | needs a crypto dependency for CMAC/HMAC/ECDH | no; the app uses OpenSSL; MTL takes derived keys |
| Q-I-11 | IS-05 `rtcp_destination_port` other than media+1 | IPMX fixes +1 | allow `rtcp.dst_port`, default +1, warn under the IPMX profile |
| Q-I-12 | Can the PEP counter rule (partial slices consume a counter) be confirmed with the TR-10-13 authors? | frame-wide vs per-packet encryption equivalence | ask; the library encrypts per packet region anyway |
