# RN — Adversarial review: NMOS and IPMX suitability of revision 4 (addendum N)

| | |
|---|---|
| Status | Review for maintainer decision M17, 2026-10-01 |
| Scope | [17-nmos-and-ipmx.md](../17-nmos-and-ipmx.md) and the header changes it lists (`mtl.h`, `mtl_rtcp.h`, `mtl_crypto.h`, `mtl_sdp.h`, `mtl_sync.h`, `mtl_format.h`, `mtl_convert.h`, `mtl_options.h`, `mtl_queue.h`, `mtl_packet.h`), `ex13_nmos_switch.c`, against [N1](../interop/N1-nmos-requirements.md) and [I1](../interop/I1-ipmx-requirements.md) |
| Rule | The header wins over prose (sketch/README.md). Where a header comment and 09 disagree, the finding says so |
| Sources (AMWA) | IS-05 v1.1.x (`Behaviour.md`, RTP Behaviour, Interop IS-04, RAML, activation schemas), IS-04 v1.3.x schemas and `Behaviour - Nodes.md`, BCP-008-01/-02 (in `/tmp/nmos`); nmos-testing `IS05Utils.py`, `IS0501Test.py`, `IS0502Test.py`, `Config.py` (master, fetched 2026-10-01) |
| Sources (SMPTE, VSF) | ST 2110-10/-20/-21:2022 (repository PDFs); VSF TR-10-0, -1, -2, -3, -4, -5v2, -7, -8, -9v2, -10, -11, -13v2, -15-1, -16 (static.vsf.tv PDFs, fetched 2026-10-01) |
| Labels | **[verified: source]** = I read the text cited; **[inferred]** = reasoning, not checked against a text |
| Check | `sketch/check.sh` passes (198 compile runs, gcc, clang, g++, clang++) [verified: run 2026-10-01]; every size check in the new headers matches a hand count |

## 1. Verdict

The shape is right: one atomic `mtl_session_update` per IS-05 resource, a planned instant returned by
the call, mute as "every leg disabled", reserved legs, and IPMX timing as an exception to D-09 rather
than a second timing model. The design is not yet one a conformant Node can build on, for four reasons:

1. **An update only completes when media flows** (RN-1). An idle or muted sender never reaches
   APPLIED, so the Node has no `activation_time` for its 200 response. The IS-05 testing tool
   activates every sender and receiver without checking for media [inferred from `IS05Utils.py`].
2. **The header contradicts 09 on neighbour resolution** (RN-2). The header text makes an activation
   to an unreachable unicast host fail. The testing tool activates senders to 192.0.2.1.
3. **What an activation carries besides flows is not atomic with it** (RN-3, RN-5, RN-7). That covers
   SDP `mediaclk`, the link offset, RTCP settings, and every PEP parameter. Under IPMX PEP, a receiver
   cannot switch to another encrypted sender without being re-created.
4. **The example teaches the wrong pattern** (RN-4). It issues one IS-05 PATCH as two updates, and the
   second replaces the first.

The IPMX side also has a key-reuse hazard (RN-6), an inline-processor clock-domain error (RN-8), and
SDP and MIB helpers that cannot yet produce what TR-10 requires (RN-17, RN-18, RN-21).

## 2. Findings

| ID | Sev | Location | Finding | Fix |
|---|---|---|---|---|
| RN-1 | Critical | `mtl.h:854-865`; 09 §7.1 lines 446-457 | TX NOW/AT_TAI switches on "the first unit picked up / whose media time ≥ t". An idle or muted sender never reaches APPLIED, so no 200 can be sent. RX AT_TAI is tied to the first unit ≥ t | Switch on the slot boundary by the clock, unit or not. RX APPLIED at max(t, ack) whatever the traffic. Say it in the header |
| RN-2 | High | `mtl.h:854` vs 09 §7.1 line 440; `ex13:17` | The header says "neighbours resolved before commit"; 09 says resolution is "started, not awaited". Under the header, an activation to 192.0.2.1 (nmos-testing default, IS0502 test_10/11) fails | Adopt 09: the leg waits in WAITING_NEIGHBOUR and the update still applies. No gateway off-subnet = a flow state, not EINVAL |
| RN-3 | High | `mtl.h:856`; options 201-203, 213, 320-324 | The update ignores options. `rx.mediaclk`, `rx.rtp_offset`, `rx.link_offset_ns` and `rtcp.*` change only through `mtl_set_option` at the "next unit", never at `when`. 17's C-N10 claim has no mechanism | Add part `MTL_UPDATE_OPTIONS`: R keys in `sc` options apply at the update boundary. C and S keys return EBUSY |
| RN-4 | High | `ex13_nmos_switch.c` | One PATCH becomes two updates. The LEGS one, built from `get_config` (the active config), REPLACES the pending FLOWS one. `set_enabled` edits `sc` before checking `ret`, omits REAPPLY. `applied_at` spins on FAILED etc. | One `activate(s, flows, legs_disabled, when)` with `FLOWS\|LEGS\|REAPPLY`. Report terminal states. Say `get_config` is active |
| RN-5 | High | `mtl.h:715`; `mtl_crypto.h` | `mtl_ext_crypto` (iv, mode, scheme, extension IDs) is read at create only. These values belong to each sender (TR-10-13 §13). A receiver cannot connect to another PEP sender without close and create, which breaks the "create at boot, mute" recipe | Allow an `MTL_UPDATE_EXT` part while muted or STOPPED, applied at `when` |
| RN-6 | High | `mtl_crypto.h:55` | "The counter restarting at 0" on every `set_key`. Re-installing the same key (re-activation, stop and start) reuses iv'‖ctr under one key, which TR-10-13 §15 forbids | Restart ctr only for key bytes this session never used. Otherwise continue or return EEXIST. The app must change `key_generator` per boot |
| RN-7 | High | `mtl_crypto.h:53-58` | RX ignores `when`: the RTP scheme uses "the latest" key, so at a scheduled switch old-flow packets are decrypted with the new key. With RTP_KV, an unknown key_version is silent loss with no event | An RX key with `when` applies to units whose media time is ≥ t (the RX update rule). Add event and stat `KEY_NEEDED{version}` |
| RN-8 | High | `mtl.h:266-269`; I1 §4.3 | The inline-processor recipe (SENDER, `media_tai_ns` = the input's SENDER_TIME) computes launch = M + delay on the upstream clock, not the instance clock: TOO_LATE or past the horizon. TR-10-1 §9 is a MUST | Add `MTL_SUBMIT_SENDER_TIME`: SR NTP and RTP from the input, launch from local arrival or submit plus the delay |
| RN-9 | Medium | `mtl.h:917-921` | The status keeps only the last call, and the call returns no seq, so a Node cannot match an event to its call. DRY_RUN/CANCEL effects on seq and state are unspecified. DRY_RUN reserves nothing, so its yes can still end in ENOSPC | Return `{seq, planned}`. DRY_RUN never touches the status. CANCEL marks the pending seq CANCELLED. Document ENOSPC |
| RN-10 | Medium | 17 §2.2 (master_enable row); N1 §5.3 `commit_active` | Deriving `subscription.active` and BCP-008 Inactive from `MTL_STATUS_MUTED` conflates master_enable = false with "every `rtp_enabled` false". IS-05 says these come from `master_enable`, and IS0502 checks it | State that the Node keeps `master_enable` itself. MUTED is a transport state only |
| RN-11 | Medium | 17 §2.2 ("a new staged activation replacing a pending one"); N1 N-REQ-23 | IS-05 returns 423 while a scheduled activation is pending. `mode: null` cancels, and may be combined with staging but not with a new activation. REPLACED is not an IS-05 path | Correct the table: the Node returns 423, and REPLACED is for non-NMOS callers |
| RN-12 | Medium | `mtl.h:849-850` | On RX, REAPPLY does leave + join of an identical group at t, on both 2022-7 legs at once: a guaranteed hit on every re-activation. IS-05 only *suggests* leave and join; the MUST is re-application | RX REAPPLY re-sends the report and re-arms the rules without a leave, staggering legs. Leave + join behind an option |
| RN-13 | Medium | 09 §7.1-7.2; 07 §3.2 | 09 still says the last leg cannot be disabled, a port change needs a reconfigure, re-staging replaces, and the status exposes `first_media_index`/`first_rtp`. 07's event table lacks events 26-29. Nothing is marked superseded | Mark each passage "superseded by 17 §2.2 / D-93" |
| RN-14 | Medium | `mtl_format.h:53` | "Colorimetry: unspecified, the SDP omits it". ST 2110-20 §7.2 makes `colorimetry` required, and IS-04 `colorspace` is required | Render `colorimetry=UNSPECIFIED`. Better: make it required on TX video and cvideo (IPMX default BT709, TR-10-9 §20) |
| RN-15 | Medium | `mtl.h:552-554`, 581 | Colorimetry, TCS and range are metadata, "never on the wire", yet changing them is `MTL_UPDATE_MEDIA` (STOPPED only). An RX activation to a BT.2020 sender forces a stop or an A/B swap | Allow the three bytes (and PAR) under FLOWS at the boundary when no conversion depends on them |
| RN-16 | Medium | `mtl_sdp.h:24` | `MTL_SDP_ONE_MLINE` renders `a=ssrc-group:DUP`. ST 2110-10 §8.5 requires the session-level `a=group:DUP` for *both* RFC 7104 forms | Drop the render flag and keep parsing that form. Render separate source addresses as two m-lines |
| RN-17 | Medium | `mtl_sdp.h:27-37` (render) | Render cannot write `a=privacy` key_generator/key_version/key_id, `measuredpixclk`/`measuredsamplerate`, `a=infoframe`, or the IPMX TP; jxsv profile/level and ANC DID_SDID only as free text. `o=` version is unchanged on a GM or RTCP_INFO change | Add typed meta fields (or a key=value list). Bump the `o=` version on TIME_SOURCE and RTCP_INFO too |
| RN-18 | Medium | `mtl_sdp.h:42-48` (parse) | Parse outputs no IPMX keyword (TR-10-9 §11 uses it to decide), no ts-refclk, no `a=rtcp`, no `a=privacy`/`a=extmap` (nothing to write into: `next` needs storage), no PAR, profile, level or DID_SDID. Legs missing from the SDP are not specified | Add meta outputs. Parse zeroes `flows[n..]` and sets their `legs_disabled` bits (RN-19) |
| RN-19 | Medium | `mtl_sdp.h:42-48` | Parsing a one-leg SDP into a config from `get_config` may leave a stale enabled `flows[1]`, and the receiver then merges an old stream (IS-05 RTP §2022-7) | Specify zeroing and disabling of legs beyond the parsed count |
| RN-20 | Medium | `mtl_rtcp.h:7-11`; 17 §3.2 | Only video and audio schedules are named. ANC (TR-10-1 §8.9.2: one SR per new RTP timestamp, before its first packet) is a MUST and missing; cvideo, fastmeta unstated. "Per 10 ms" ≠ N = int(10 ms/ptime) packets from the first | Schedule per essence. SENDER audio: `unit_samples` a multiple of N × spp, or define a mid-unit SR's NTP |
| RN-21 | Medium | `mtl_rtcp.h:50-62` | "One MIB builder for every essence" cannot build 0x0008 (T, P, Ppih, Plev), 0x0003 (sampling and depth; cvideo has no `htotal`/`vtotal`), 0x0011 (privacy_version) or 0x0006 | Build only 0x0001, 0x0002, 0x0004 and 0x0011 (add privacy_version to the descriptor), and take the rest as application bytes; or add the fields |
| RN-22 | Medium | `mtl_rtcp.h:44-48` | A per-unit `MTL_META_RTCP_MIB` *replaces* all MIBs, and any byte change bumps the version and posts RTCP_INFO ("re-render SDP"). Dynamic HDR per field (TR-10-16 §7) bumps and posts every frame | Per-unit records *append* (HDR after the colorimetry MIB). Only `set_info` changes the version and posts RTCP_INFO |
| RN-23 | Medium | `mtl.h:538`, 557 | `MTL_SENDER_IPMX` has no TP value: TR-10 examples use `TP=2110TPN`, and 2110TPW requires a linear PRS (ST 2110-21 §7.1.4). `vtotal` 0 = "the ST 2110-21 default" is undefined outside the BT rasters (§6.3) | Drop the type (Type N + `vtotal` meets TR-10-1 §8.1) or define its TP. `vtotal` 0 = the 2110-21 value, else height (TR-10-9 §10) |
| RN-24 | Medium | `mtl.h:266-269` | SENDER mode leaves `k` undefined. After a missing VSYNC, RTP moves one frame while NTP moves two, so receivers' ΔRTP/ΔNTP glitches. Q-I-3's DISCONTINUITY re-anchor is not in the header, nor are units in flight across an AUTO step | k = `media_index` if set, else round((M − M0)/period). DISCONTINUITY re-anchors RTP0. In-flight units are rebased by a step |
| RN-25 | Medium | 09 §7.1 lines 451-457 | An RX AT_TAI switch by RTP-derived media time means nothing for `mediaclk:sender` or an unlocked or foreign clock (IPMX without PTP) | In SENDER or ESTIMATED mode, classify packets by local arrival time on the instance clock. Say so |
| RN-26 | Medium | `mtl_options.h:45` | `tx.precede` (S) orders enqueue only; separate queues or RL shaping lose the wire order. InfoFrame RTP must equal the video's (TR-10-10 §6, §9), which SENDER mode won't give. A video activation also moves the +3 stream: two non-atomic updates | Make it C, shared queue. Copy the preceded unit's RTP and M. Update with its video. Render `a=infoframe` |
| RN-27 | Medium | `mtl.h:419-421` | FREERUN is never stepped or slewed, so it drifts from the Node and controller clocks (10 ppm ≈ 0.86 s/day) [inferred]. Absolute activations miss: nmos-testing allows 0.1 s [verified: `IS05Utils.py` `MAX_TIME_SYNC_OFFSET`] | Add an option for a frequency-only, ppm-bounded slew to CLOCK_TAI/NTP; or document `mtl_time_convert` for IS-05 times |
| RN-28 | Low | `mtl.h:852`; `mtl_sdp.h:39` | `-MTL_EAGAIN` from CP calls (CANCEL, render) contradicts R2 (data calls that arm a wait) | CANCEL with nothing pending returns 0 (IS-05 `mode: null` answers 200 anyway). Render returns `-MTL_EBUSY` |
| RN-29 | Low | `mtl_rtcp.h:37-38` | `ts_refclk[96]` and `mediaclk[32]`, but the wire fields are 64 and 12 bytes (TR-10-1 §8.7) | Size them 64 and 12, or return `-MTL_EINVAL` beyond 64 and 12 |
| RN-30 | Low | 17 §3.1 profile table | TR-10-9 §16: InfoFrame, FEC and HDCP streams take the associated stream's DSCP, and PTP takes EF. Under IPMX, CS0 cannot be expressed (0 = AF42). Also missing: DHCP default, mandatory source filter (`MTL_SDP_NO_SOURCE_FILTER`), DHCP DNS/domain for in-band discovery | Complete the table. Add a flow flag for an explicit DSCP. List the profile's checks |
| RN-31 | Low | `mtl_rtcp.h:28`; `mtl.h:325` vs 419; `mtl_options.h:155`; 17 §1 | `MTL_WAIT_RTCP` is a core wait bit defined in an optional header. TIME_FREERUN (a state) and TIME_SOURCE_FREERUN share a name. Key 2028 sits in the 2000-2019 block. "13 new functions": 17's headers add 10, and 3 come from 16 | Move the bit to `mtl.h`, rename the state, move the key, correct the count |
| RN-32 | Low | `mtl.h:690-692` | Muted TX units are DROPPED/LEG_DISABLED and so fall into `tx.units_dropped`, which the BCP-008 mapping (17 §2.5, N1 §5.5) reads as Unhealthy. Whether SRs go out while muted, and what render does then, is unspecified | Count them in `tx.units_muted` only. No SR while muted. Render returns `-MTL_EBUSY` |
| RN-33 | Low | `mtl.h:862`; options 320, 322 | An update in CREATED or STOPPED applies at once, so `planned_tai_ns` is undefined there. A pending AT_TAI across a time step is undefined. `rtcp.sr` (C) and `rtcp.dst_port` (S) cannot follow the IS-05 RTCP set | planned = the applied instant. Pending updates fail with TIME_STEP. A Node constrains `rtcp_*` to the fixed values |
| RN-34 | Low | `mtl_crypto.h`, `mtl_rtcp.h`, options 320/323 | Lean-API trims: `clear_keys` = `set_key(NULL)`; `mib_build` and `mib_next` are pure byte helpers (`mtl_util.h`); the RFC 3550 SR mode and `rtcp.sr_interval_ms` serve no NMOS or IPMX MUST | Merge or drop them (§8) |

## 3. Q1 — IS-05 correctness

**What holds** [verified: `ConnectionAPI.raml` lines 237-284; `activation-response-schema.json`]:

- 200 vs 202. For an immediate activation, the response comes "only once the new transport parameters
  have been applied", with the time it occurred. For a scheduled one, 202 carries "the absolute TAI
  time the parameters will actually transition ... may differ ... at a frame boundary". Returning
  `planned_tai_ns` from the call (202), and `update_applied_tai_ns` with `MTL_EVENT_UPDATE` (200), is
  the right shape. A past instant meaning now matches "when internal clock >= requested_time".
- `transport_params` with `auto`: the Node resolves them and reads back the granted values
  (`info.leg[].udp_src_port`, ssrc, MACs; `mtl_port_get_spec().sip`). IS-05 source_port `auto` is
  5004 [verified: `sender_transport_params_rtp.json`], while MTL's default is `udp_src_port =
  udp_port`. The Node must therefore always write it, as C-N11 says.
- Two-leg receiver given a one-leg SDP: IS-05 says "SHOULD set `rtp_enabled` in the second set ... to
  false" [verified: RTP Behaviour §2022-7]. A reserved and disabled leg 1 covers this, subject to RN-19.
  Temporal redundancy (identical source and destination) is constrained out by ST 2110-10 §8.5
  [verified].
- `receiver_id` and `sender_id` are Node state only. Nothing in MTL is needed or affected
  [verified: IS-05 Interop IS-04 §Identifying Active Connections].
- staged vs active: Node state; MTL holds only the active configuration (RN-4 shows why that must be
  documented).

**What a conformant Node still cannot do, or what nmos-testing would fail:**

- **RN-1.** `IS05Utils._check_perform_activation` checks `/active` once (`maxTries = 1`) right after
  an immediate activation, and up to three times, `API_PROCESSING_TIMEOUT` (1 s) apart, after a
  scheduled one [verified: `IS05Utils.py`, `Config.py`]. The suite runs against Nodes that usually have
  no essence and no stream. A sender that is not submitting units never reaches APPLIED under 09 §7.1
  ("from the first unit picked up"). The receiver side has a fallback for removing old rules
  (t + flush offset + skew budget), but nothing says when the update counts as applied.
- **RN-2.** `subscribe_resource(..., multicast=False)` sets `destination_ip = 192.0.2.1` and
  activates at once [verified: `IS05Utils.py`, `Config.py` `UNICAST_STREAM_TARGET`]. Under the header's
  "neighbours resolved before commit", that update fails, and the Node answers 500.
- **RN-3, RN-5, RN-7.** IS-05 activates the whole parameter set, including `ext_*` and the SDP's media
  information, at one instant. Options and extension blocks are outside the update.
- **RN-11.** While a scheduled activation is pending, the resource is locked: 423 [verified: RAML
  lines 279-284]. The Node must implement the lock itself, and 17's "REPLACED" row is not IS-05.
- **Re-activation.** The MUST is "request a re-application", and leave/join is only "suggested"
  [verified: `Behaviour.md` §Re-Activating]. REAPPLY's leave + join on RX is stricter than needed and
  costs a hit (RN-12). `version` must still increment on every re-activation [verified: Interop IS-04
  §Version Increments]. That is the Node's job, triggered by `MTL_EVENT_UPDATE`.
- **master_enable vs rtp_enabled.** Folding both into `legs_disabled` is fine for the transport. The
  Node must still keep `master_enable` itself, because IS-04 `subscription.active` and BCP-008
  "deactivation" are defined by it [verified: IS-04 `Behaviour - Nodes.md`; BCP-008-01 §Deactivating]
  (RN-10).
- **Port change while running.** Make-before-break is correct. `-MTL_EBUSY` on a backend that cannot
  do it is acceptable only if the Node advertises a constraint of one `interface_ip` per leg on that
  backend [inferred].
- **Join lead.** `max(call, t − lead)` is fine. Note that a deferred join cannot fail the update
  synchronously, so JOIN_FAILED arrives after the 202. That is IS-05-correct, since packet loss must not
  show in `/active` [verified: `Behaviour.md` §Connection Status].
- **Reserved legs.** The rule (a set `legs_disabled` bit on an unaddressed leg) works. On a one-port
  instance, though, a reserved leg 1 with `port = 0` means "port 1", which does not exist. Validate a
  reserved leg's port when it is enabled, not at create [inferred].

## 4. Q2 — IS-04 and BCP-008 coverage

Every IS-04 *required* attribute can be filled from public values [verified: `required` lists of
`node.json`, `sender.json`, `receiver_core.json`, `flow_video*.json`, `flow_audio_raw.json`,
`clock_ptp.json`]. The exceptions and caveats:

| Attribute | Status |
|---|---|
| Flow `colorspace` (required) | from `video.colorimetry`; 0 must map to `UNSPECIFIED`, and the SDP must say so too (RN-14) |
| Flow `interlace_mode` `interlaced_tff`/`_bff` | not in `mtl_raster` (`MTL_INTERLACED` has no field order): application. Acceptable, but RX detection cannot report it [inferred] |
| Flow (coded) `profile`, `level`, `sublevel` (BCP-006-01 MUST) | application or codec plugin; not in `mtl_cvideo_config` |
| Flow (data) `DID_SDID` on TX | application; RX `anc.did_sdid_seen` covers forwarding |
| Sender `interface_bindings`, Node `interfaces[].port_id` | `info.leg[].port`, `mtl_port_spec.mac` OK. Whether `mtl_port_get_spec().sip` returns the *granted* (DHCP) address is unstated |
| Node `clocks[]` `traceable`, `gmid`, `locked` | `time.gm_traceable`, `time.grandmaster_id`, `time.state`, or `mtl_time_set_reference` OK |
| ST 2110-41 Flow `media_type` | still unregistered [verified: grep of nmos-parameter-registers @ 2026-09-25]; Q-NMOS-6 stands |
| `subscription.active` | Node from `master_enable`, not from MUTED (RN-10) |

BCP-008-01/-02 can be computed from the counters and events in 08 R4.5 [verified: 08 lines 460-479;
BCP-008 Overview §Link, §Connection, §Synchronization, §Stream, §Transmission, §Essence]. Remaining
issues: muted units pollute the error counters (RN-32); "reset on activation" needs a reliable
activation marker per call (RN-9); and in SENDER-clock receivers `externalSynchronizationStatus` should
be NotUsed, which the Node can derive from `rx.mediaclk` [inferred].

## 5. Q3 — SDP coverage

| Item | Render | Parse |
|---|---|---|
| ST 2110-20 required fmtp (sampling, depth, width, height, exactframerate, colorimetry, PM, SSN) | yes, but colorimetry must not be omitted (RN-14) | yes |
| -20 defaults (interlace, segmented, TCS, RANGE, MAXUDP, PAR) | PAR only through `fmtp_extra` | PAR has no output (RN-18) |
| -21 TP, TROFF, CMAX | yes; TP for `MTL_SENDER_IPMX` undefined (RN-23) | TP is not mapped to `sender_type` for RX checks [inferred] |
| -22 jxsv (packetmode, transmode, profile, level, sublevel, sampling, depth, `b=AS`) | sampling and depth have no source when `app_format = 0`; profile, level and sublevel free text only | no typed output |
| -30/-31 (L16, L24, AM824, ptime, channel-order) | yes (`channel_order`) | yes |
| -40 (DID_SDID, VPID_Code, exactframerate, TM, SSN) | DID_SDID and VPID free text only | no output |
| -41 (DIT, K) | yes from `fastmeta.*` [inferred] | yes [inferred] |
| ST 2022-6 | `rtp.encoding`, `clock_rate` | yes |
| 2022-7 `group:DUP` (two m-lines) | yes | yes |
| 2022-7 `ssrc-group:DUP` (one m-line) | must not be rendered (RN-16) | yes |
| IPMX keyword, `measuredpixclk`, `htotal`, `vtotal`, `measuredsamplerate` | keyword under the profile [inferred]; measured values have no input (RN-17) | keyword has no output (RN-18) |
| PEP `a=privacy`, `a=extmap` | extmap yes; privacy is missing key_generator, key_version and key_id | nowhere to put them (RN-18) |
| `a=rtcp` (RFC 3605) | yes [inferred from "RTCP attributes"] | not mapped (RN-18) |
| `a=infoframe` (TR-10-10 §8) | no (RN-26) | no |
| `a=ts-refclk`, `a=mediaclk` | yes | mediaclk yes; ts-refclk has no output |

`mtl_sdp_meta` is not enough for IPMX. Either add typed fields (measured clock, privacy triple, IPMX
flag, ts-refclk out, rtcp port out), or give it one `key=value` list in each direction. A single list
keeps the struct stable and covers BCP-006-02/-03 later [inferred].

## 6. Q4 — IPMX

- **SR placement.** Video per frame or field, "before the first video media packet ... but after the
  first video media packet of the previous frame" [verified: TR-10-1 §8.8.2]. Enqueueing ahead of the
  unit's first packet on the same queue satisfies this. ANC per new RTP timestamp [verified: §8.9.2]
  is **not covered** (RN-20). Audio is "first packet and every N packets", N = int(10 ms/ptime),
  "before the packet containing the associated RTP timestamp" [verified: §8.10.1]. That is not "per
  10 ms of packets" when units and N disagree.
- **Info Block.** The tag, length, version, 64-byte ts-refclk and 12-byte mediaclk fields
  [verified: §8.7] mean the header string sizes are wrong (RN-29). The rule that "block version
  increments whenever the associated media stream changes and requires the IPMX Info Block content to
  be updated" fits poorly with per-unit replacement (RN-22).
- **MIB builder.** 0x0001 is buildable from the config plus PAR and the measured clock
  [verified: TR-10-2 §10 layout]. 0x0008 needs T, P, Ppih and Plev [verified: TR-10-15-1 §9]; 0x0011
  needs privacy_version [verified: TR-10-13 §22]; 0x0006 is application data [verified: TR-10-16 §7].
  None of these are buildable (RN-21).
- **SENDER mode math.** RTP0 = floor(M0 × rate) and RTP = RTP0 + floor(k × period × rate) give video
  +1501.5 per frame at 59.94, and audio +1 per sample (the period is one sample). The math is correct,
  but `k` and the dropout and DISCONTINUITY behaviour are undefined (RN-24). The inline-processor use
  computes launch on the wrong clock (RN-8).
- **FREERUN and AUTO.** These match TR-10-1 §7.1 ("free running Internal Clock") [verified]. Missing:
  drift against controllers (RN-27); units and pending updates across the step (RN-24, RN-33). BMCA
  (§7.2 MUST) still needs ptp4l (Q-I-7), so `PTP_BUILTIN` must be labelled non-compliant under the IPMX
  profile [inferred].
- **`MTL_SENDER_IPMX`.** TR-10-1 §8.1 defines the IPMX CMAX as "the maximum allowed value" and allows
  a Type N CMAX [verified]. Its example signals `TP=2110TPN` [verified: TR-10-1 §10.2]. A separate type
  without a TP value is a liability (RN-23).
- **IGMPv2.** `port.igmp_version` V2/V3 satisfies "support V2 ... user mechanism for selecting"
  [verified: TR-10-9 §17].
- **DSCP via the profile.** Incomplete (RN-30).
- **`tx.precede`.** Ordering only. RTP equality, the +3 port, the same destination IP, and atomic
  activation are not covered (RN-26) [verified: TR-10-10 §5, §6, §9].
- **Link offset while active.** R option plus min/max gauges: OK for TR-10-8 §8 constraints. But the
  activation value is not atomic (RN-3), and AUTO "takes the measured minimum" must be sampled at
  activation and then held, not track continuously, or presentation latency wanders. TR-10-8 sets it
  "on activation" [verified].
- **PEP.** The per-packet counter is consistent with TR-10-13 §20.2: slices of 16 bytes, a partial
  slice counted, Full extension on the first packet of a frame, field, slice or audio packet and on
  every non-A/V packet [verified]. The library should state the ANC/fastmeta "Full on every packet"
  rule. 2022-7 with the same ciphertext: OK (§13 "legs shall use the same parameters" [verified]). MAC
  modes on the copy path: OK. Key rotation: TX OK; RX broken (RN-7); key reuse (RN-6); per-sender
  parameters immutable (RN-5). ECDH modes are only a key-derivation difference, so the cipher enum is
  sufficient [verified: §20 mode list].
- **HDCP.** Packet units with vendor code fit TR-10-5. An HDCP receiver must also use packet units,
  because frame units deliver ciphertext pixels and no extension values (streamCtr, I-REQ-61).
  Document that [inferred].

**IPMX MUSTs (I1 §3) still not met:** I-REQ-18 (ANC SR, RN-20); I-REQ-24 (inline processors,
RN-8); I-REQ-25 (measured values in the SDP, RN-17); I-REQ-43 (InfoFrames before video and
RTP-matched, RN-26); I-REQ-45/-58 (MIBs 0x0008 and 0x0011, RN-21); I-REQ-52..57 when PEP is
implemented (RN-5, RN-6, RN-7); I-REQ-64 (DSCP for InfoFrames and PTP, RN-30); I-REQ-27 "if
supported" (atomic link offset at activation, RN-3). I-REQ-4 (BMCA) is met only with ptp4l.

## 7. Q5 — Coherence and dispositions

- **D-09.** The SENDER exception is stated in the enum comment and in D-95. Coherent.
- **Zero defaults (D-74/D-22).** The profile changing what 0 means is stated, but it makes CS0 and
  "TCS SDR, explicitly" inexpressible. For TCS that is harmless; for DSCP see RN-30.
- **R2.** CP calls return `-MTL_EAGAIN` (RN-28).
- **R3.** `mtl_ext_hdr` and the unknown-kind error are consistent. `mtl_sdp_meta`, used as parse
  output with `struct_size`, falls under R3's "written and read back" clause. OK.
- **R5.** Rewritten and consistent with `MTL_UNITF_SENDER_TIME`. R7: the key exception is justified
  (C-I9).
- **09 §7 and 07.** Not updated or marked superseded (RN-13). 09 §7.1's "first_media_index /
  first_rtp in `mtl_session_get_status()`" contradicts the header, which has no such fields.
- **06.** SENDER launch = M + delay bypasses the TVD/TROFFSET grid of 06 §4.3. C-I3 says so.
- **Dispositions in 17 §6.** Every claimed symbol exists with the claimed meaning: options 113,
  201-203 R, 213, 306, 310-314 `rtx.*`, 320-324, 340, 2028, 2124, 2208, 2209; events 26-29; reasons
  `LEG_DISABLED`, `PORT_CHANGE_NEEDS_STOP`, `NO_KEY`; keys in 08 R4.5 [verified: grep of headers and
  08]. Inaccurate claims:
  - C-N10 and G-N18 ("at the update's boundary"): not true (RN-3);
  - 17 §2.2 REPLACED as the IS-05 re-staging: not true (RN-11);
  - G-N10 colorimetry "SDP omits": wrong (RN-14);
  - GI-2 "one MIB builder for every essence": overstated (RN-21);
  - the function count (RN-31).
- **ex13** is not correct IS-05 usage (RN-4). It also never shows the immediate path (NOW and wait for
  APPLIED).

## 8. Q6 — Simplicity

What a Node really needs from MTL is: the update contract (one call, no new function); colorimetry
bytes; `mtl_time_set_reference`; the SR sender and receiver; the cipher; and the counters. Proposed
trims, which keep the module split:

| Item | Keep / change |
|---|---|
| `mtl_sdp.h` (2 functions) | keep as its own header: it is the largest saving for every Node. Fix RN-16 to RN-19 instead of adding functions |
| `mtl_rtcp.h` (4 functions) | keep `mtl_rtcp_set_info` and `mtl_rtcp_read`. Move `mtl_rtcp_mib_build`/`mib_next` (pure, AS) to `mtl_util.h`, or drop `mib_build` in favour of application bytes (Q-I-1's own answer) |
| RFC 3550 SR mode, `rtcp.sr_interval_ms` | drop: no NMOS or IPMX MUST needs it; `rtcp.sr` becomes a bool |
| `mtl_crypto_clear_keys` | merge into `mtl_crypto_set_key(s, v, NULL, 0, when)` |
| `MTL_SENDER_IPMX` | drop unless a TP value is defined (RN-23) |
| `session.profile` + `instance.profile` | keep one switch; the session key is enough if the instance key only seeds it. The real cost is the hidden default matrix, which the profile table must list completely (RN-30) |
| `MTL_UPDATE_DRY_RUN` | keep (cheap), but specify that it never touches the status (RN-9) |
| `MTL_WAIT_RTCP` | move to `mtl.h` beside the other wait bits (RN-31) |

Net: two functions fewer (`clear_keys`, and `mib_build` if application bytes are taken), two moved
to `mtl_util.h`, and no header beyond the three proposed. RN-3 and RN-5 add one part bit each, not
a function.
