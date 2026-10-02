# RA — API coherence review of the Kubernetes (K) and NMOS/IPMX (N) additions

| | |
|---|---|
| Status | Review, 2026-10-01. Read-only: no other file was changed |
| Scope | `sketch/include/mtl/experimental/*.h` (17 headers), `sketch/examples/ex01_tx_video.c`, `ex11_signal.c`, `ex13_nmos_switch.c`, `examples_cpp.cpp` |
| Version reviewed | the untracked files of 2026-10-01 after addenda K ([16](../16-kubernetes-and-crash-safety.md)) and N ([17](../17-nmos-and-ipmx.md)) |
| Checked against | mtl.h R1–R8, DECISIONS D-22/D-33/D-73/D-74/D-80/D-89…D-96, REVISION-4, 03 §3.4, 09 §7.2 and §9.3, 16, 17, ST 2110-20:2022 §7.2–§7.6, [R4-header-review](../simplification/R4-header-review.md) |
| Owner's goal | very modular and extensible, as simple as possible, the include file as lean as possible, every use case MTL covers today |
| Severity scale | Blocker (does not compile or cannot be implemented as written), Major (an implementer must guess, or a stated use case fails), Minor (inconsistency, avoidable surface), Nit |

## 1. Summary

`check.sh` is green: `OK (198 compile runs, compilers: gcc clang g++ clang++)`, 139 functions (mtl.h 33), 6 under `MTL_LATER`. That green hides one Blocker. `check.sh` compiles each header in its own translation unit, and two of the new names collide when a program includes `mtl_options.h` and `mtl_rtcp.h` together (RA-1).

| Severity | Count |
|---|---|
| Blocker | 1 |
| Major | 13 |
| Minor | 21 |
| Nit | 6 |

The Majors cluster in four places:

- **Zero defaults and profiles.** IPMX turns some zero values into profile defaults, so `MTL_SENDER_N` and DSCP CS0 can no longer be requested (RA-2). Colorimetry 0 renders a non-compliant SDP (RA-14).
- **The IS-05 contract.** CANCEL misuses `-MTL_EAGAIN` and hides "too late" (RA-3). Reserved legs contradict the leg rules (RA-6). Options cannot switch with the flow (RA-7). ex13 has three bugs that a Node copying it would ship (RA-11).
- **Shutdown.** Shared-instance handles are undefined (RA-4). The fate of memory under stale leases is described four different ways (RA-5). FLUSH and abort leave the partial unit undefined (RA-12). The close outcomes have gaps (RA-13). Readiness never turns true for a PTP-less IPMX pod (RA-10).
- **SDP and crypto.** `mtl_sdp_parse` returns unterminated strings (RA-8). The crypto extension block cannot be read back, so `mtl_sdp_render` cannot be "built only on public calls" (RA-9).

The biggest simplification keeps every capability: make the PEP crypto parameters options (none of them is secret). That deletes `struct mtl_ext_hdr`, `enum mtl_ext_kind` and the extension chain from `mtl.h`, and fixes RA-9 (§5 E9). With the cuts of §6, the surface goes from 139 to 136 functions and from 17 to 16 headers, and `mtl.h` sheds about 20 lines.

## 2. Findings

Locations are `file:line` in `sketch/include/mtl/experimental/` unless they name an example or a document. "E*n*" points to the exact edit in §5. Findings marked "(§2.1)" have their full reasoning below the table.

| ID | Sev | Location | Finding | Fix |
|---|---|---|---|---|
| RA-1 | Blocker | mtl_options.h:239, mtl_rtcp.h:73 | `enum mtl_rtcp_sr` and `struct mtl_rtcp_sr` collide; a TU including both headers fails to compile (§2.1) | E1; all-headers TU in check.sh (RA-33) |
| RA-2 | Major | mtl.h:484, 534-539; 17 §3.1 | profile defaults reuse 0: under IPMX, `MTL_SENDER_N` and DSCP CS0 cannot be requested (§2.1) | E2 |
| RA-3 | Major | mtl.h:852-853 | CANCEL returns `-MTL_EAGAIN` for both "none pending" and "too late" (§2.1) | E3 |
| RA-4 | Major | mtl.h:442-444, 452-462 | shared instance: does each open get its own handle? "Always consumes" is unverifiable otherwise (§2.1) | E4 |
| RA-5 | Major | mtl.h:457-459; mtl_observe.h:259; 16 §2 | memory under leases after instance close is described four ways; release on a closed session undefined (§2.1) | E5 |
| RA-6 | Major | mtl.h:476-479, 687, 690-692, 842-845; 09 §7.2, §9.3 | reserved legs let LEGS add a leg while RUNNING; 09 contradicts the header; mute pacing undefined (§2.1) | E6 |
| RA-7 | Major | mtl.h:855-856; mtl_options.h:49-52; 17 §2.2 | `rx.rtp_offset`/`rx.mediaclk` cannot change at the update's boundary as 17 promises (§2.1) | E7 |
| RA-8 | Major | mtl_sdp.h:25-26, 47-48 | parse returns `meta` strings into non-terminated text, with no length (§2.1) | E8 |
| RA-9 | Major | mtl.h:703, 714-723; mtl_crypto.h:41; mtl_sdp.h:7 | the crypto block cannot be read back, so render cannot be "built only on public calls" (§2.1) | E9: crypto parameters as options |
| RA-10 | Major | mtl_observe.h:203-205; 16 §8; 17 §2.6 | readiness never becomes true without PTP; one bad stream takes the Node out (§2.1) | E10 |
| RA-11 | Major | ex13_nmos_switch.c:140-171 | flow_parse wipes ssrc/dscp/pt; split updates replace each other; `applied_at` never ends (§2.1) | §4.3 |
| RA-12 | Major | mtl.h:293, 453-454, 467-470; 03 §3.4 | FLUSH's "partly sent is finished" and abort's cut unit have no defined status or reason (§2.1) | E12 |
| RA-13 | Major | mtl.h:452-463; mtl_observe.h:262-268 | close outcomes incomplete: stuck threads, library-thread callers, retirement after 1, other handles (§2.1) | E13 |
| RA-14 | Major | mtl_format.h:51-53; mtl.h:552 | "colorimetry 0: the SDP omits it" violates ST 2110-20:2022 §7.2 (required parameter) | E14 |
| RA-15 | Minor | mtl_observe.h:238-248 | `MTL_SHUTDOWN_*` flags and outcomes share a prefix and overlap (`QUIESCED == ALL_REFERENCES`); `outcome` repeats the return; RETIRED for a non-last drop | E15 |
| RA-16 | Nit | mtl.h:841-853, 897-904 | parts, modifiers and `MTL_UPDATE_STATE_*` share `MTL_UPDATE_`; STATE_APPLIED (2) OR'ed into parts is LEGS | accept; drop CANCEL (E3) |
| RA-17 | Minor | mtl_sdp.h:39; mtl_sync.h:72-74 | `-MTL_EAGAIN` outside R2 (render "value unknown", `mtl_index_at` "T0 unresolved"): nothing is armed | `-MTL_EBUSY`, reason `WRONG_STATE` |
| RA-18 | Minor | mtl.h:393; mtl_observe.h:278 | `mtl_port_get_spec` has no size argument (R3) and writes an input struct; non-zero `mac` on input is `-MTL_EINVAL` by R3 | `mac` "ignored on input"; add `size_t size` |
| RA-19 | Minor | mtl_options.h:70, 72-87, 155, 216 | 2028 profile sits in the pods range (16 claims 2020–2028); 306/310-314/320-324/340 in "queues"; 2208 claimed by 16 and 17 | profile → 2040; RTX/RTCP/crypto → 1000-1099 |
| RA-20 | Minor | mtl_rtcp.h:28; mtl.h:1085-1089 | `MTL_WAIT_RTCP` (0x20) lives outside the core wait list; the next core bit collides silently | list it in mtl.h's wait block |
| RA-21 | Minor | mtl_rtcp.h:44-47; mtl.h:998 | a per-frame MIB record bumps the Info Block version and posts `RTCP_INFO` every frame; RX and audio meaning unstated | per-unit records never bump the version; TX only |
| RA-22 | Minor | mtl.h:988-990, 1000-1006 | "at most one record per kind" forbids two tagged USER records (HDR + timecode) | "one per (kind, tag)" |
| RA-23 | Minor | mtl.h:261-270; mtl_sync.h:6-11 | SENDER mode: k, result indices, AT_INDEX, `mtl_index_at`, `tx.precede`, fast sources all undefined | E23 |
| RA-24 | Minor | mtl.h:324-330, 412-421; mtl_queue.h:55 | "FREERUN" is a state, a source and a fallback; the state under FREERUN/SYSTEM_TAI is unstated; `MTL_EVENT_TIME_SOURCE` means "grandmaster changed" | `MTL_EVENT_GRANDMASTER`; state rule (E10) |
| RA-25 | Minor | ex11_signal.c:69-77, 116-125 | `r` read uninitialised; timeout negative for grace < 2 s; `g_signals++` races across threads; flags 0 on a shared instance | §4.2 |
| RA-26 | Minor | mtl_util.h:6-9, 47-55 | `mtl_port_specs_from_network_status` does file I/O and JSON parsing, against the header's "built only on public calls" | cut to a sample or `MTL_LATER` |
| RA-27 | Minor | mtl_convert.h:56-62 | `mtl_audio_remap`: new capability, ten-line loop, wrong prefix, no return value, aliasing unstated | cut, or `MTL_LATER` |
| RA-28 | Minor | mtl_rtcp.h:51-66 | `mtl_rtcp_mib_build` rebuilds from a config what the session already holds | E28: the library builds the essence MIB |
| RA-29 | Minor | mtl.h:557-559, 584 | `vtotal`, `htotal`, `max_bitrate_bps` are IPMX tuning with derived defaults (D-73: options) | E29 |
| RA-30 | Minor | DECISIONS D-80; mtl.h:905-922 | status is 112 B, D-80 says 80 B; `update_planned_tai_ns` repeats the call's output | drop the field (104 B); update D-80 |
| RA-31 | Minor | mtl.h:854-865 | `planned_tai_ns` in CREATED/STOPPED/ARMED; DRY_RUN's effect on status, seq and pending updates; what `update_seq` counts | E3 text |
| RA-32 | Minor | mtl.h:376-379, 437-441 | `env:` ports read the environment in a set-uid process, where `MTL_PORTS` is ignored | `secure_getenv`; `-MTL_EINVAL` there |
| RA-33 | Minor | sketch/check.sh | no TU includes every header, so RA-1 passed | §7 probe |
| RA-34 | Minor | mtl.h:452-456 against 872 | instance close FLUSHes, session close DRAINs; closing only the instance loses queued units silently | say so in the close comment |
| RA-35 | Minor | mtl.h:50-54, 457, 873 | "their close then returns 0" contradicts R4 (stale → `-MTL_EBADF`) and "never close twice" | extend R4 (E5) |
| RA-36 | Minor | mtl_observe.h:232-236 | health as DP needs type-stable memory against a racing close and a seqlock copy of `detail[96]`; neither is stated | "a full snapshot or `-MTL_EBADF`, never torn" |
| RA-37 | Nit | mtl.h:65 | comment line has no leading asterisk | reflow |
| RA-38 | Nit | ex01_tx_video.c:40-45, 52 | `k` advances on `-MTL_EAGAIN`; the "tx" label also covers an open failure | count after submit; label "mtl" |
| RA-39 | Nit | mtl_crypto.h:32-40; mtl.h:552-554 | crypto mode values lack the `MTL_CRYPTO_` prefix; the new enum fields are `uint8_t`, every other one `uint32_t` | rename; note `uint8_t` in R3 |
| RA-40 | Nit | 16 §7 | "budget 18 on an untrusted VF" but "the 17th group fails" | state the reserved entries, or "the 19th" |
| RA-41 | Nit | mtl_util.h:41-45; mtl_rtcp.h:89-92 | pure parsers have mixed classes: flow/raster parse CP; fps parse, `mib_next`, `rfc8331_*` AS | one rule: pure and allocation-free = AS |

### 2.1 Details of the Blocker and the Majors

**RA-1.** `enum mtl_rtcp_sr` (mtl_options.h:239) and `struct mtl_rtcp_sr` (mtl_rtcp.h:73, 87, 96) share C's tag namespace. A TU that includes all 17 headers fails with gcc: "'mtl_rtcp_sr' defined as wrong kind of tag". `check.sh` stays green because it compiles every header in its own TU.

The two families also share the `MTL_RTCP_SR_` prefix with overlapping values: `MTL_RTCP_SR_OFF == MTL_RTCP_SR_INFO == 1`.

**RA-2.** 17 §3.1 lets the profile decide what a zero field means, but in two places 0 is also a real value:

- `enum mtl_sender_type`: `MTL_SENDER_N = 0`. Under IPMX, `sender_type = 0` means IPMX, so narrow cannot be requested.
- `mtl_flow.dscp` says "0 = CS0". Under IPMX, 0 means AF41/AF42, so the header comment is wrong and CS0 cannot be requested.

R3's zero-default rule holds only if 0 is never also a real choice. Fix E2. Add `MTL_FLOWF_DSCP_LITERAL` only if CS0 under IPMX is a use case.

**RA-3.** CANCEL returns `-MTL_EAGAIN` both when nothing is pending and when the update is already committing. R2 reserves `-MTL_EAGAIN` for data calls and says it arms a wait target; here nothing is armed.

Worse, the two cases have opposite answers for the Node: with IS-05 `activation.mode = null`, "too late" means the activation will still happen and must be reported. Fix E3: `parts` 0 cancels (no flag); 0 = cancelled, 1 = none pending, `-MTL_EBUSY` with a new reason `UPDATE_COMMITTING` = too late.

**RA-4.** For `MTL_INSTANCE_SHARED` it is not said whether each open returns its own handle value. If the value is shared:

- "always consumes mt" cannot be checked;
- R4's "a closed handle is never reissued" and "stale → `-MTL_EBADF`" cannot hold;
- a component that closes twice silently drops another component's reference.

Fix E4: one reference handle per open.

**RA-5.** Memory under a lease after instance close:

- mtl.h:458-459: "freed when they are returned, or at exit";
- the report, `bytes_kept_until_exit`: "never freed";
- 16 §2.3: "a long-lived process returns its leases, and the memory is freed then";
- 16 §2.4 and D-89: "stays mapped until exit".

Returning such a lease needs a session handle that the instance closed. R4 then gives `-MTL_EBADF`, 16 §2.1 says "0 or `-MTL_ESTALE`", and mtl.h says nothing. Fix E5: release returns 0, a slot is freed after its last lease (16 §2.3), and `bytes_kept_until_exit` counts only leases never returned. Put the exception in R4.

**RA-6.** A set `legs_disabled` bit creates a reserved leg, so `MTL_UPDATE_LEGS` on a RUNNING session can add a leg. Yet FLOWS says that the set of legs changes only when STOPPED, and 17 §2.2 says "fixed at create". Further gaps:

- Bits ≥ `MTL_MAX_LEGS` are not rejected.
- "All set = muted" can only mean "all existing legs".
- mtl.h:687 ("flows[1] with a udp_port is the 2022-7 leg") ignores reserved legs.
- 09 §7.2 still says that disabling the last enabled leg is `-MTL_EINVAL`. 09 §9.3 still requires `flows[0].ip` and `udp_port`.
- Mute does not say whether muted TX units retire at their slot time. If they do not, a library-pool loop spins at full CPU, and with results off nobody sees the DROPPED units.

Fix E6.

**RA-7.** 17 §2.2 (last row) says that the new SDP's `mediaclk:direct=` (`rx.rtp_offset`, `rx.mediaclk`) changes "at the update's boundary". The header says update ignores options and that `mtl_set_option()` applies R keys "at the next unit boundary", so the flow switch and the offset change land on different units.

`mtl_sdp_parse` writes these keys into `spec_options`, which update then ignores. Fix E7.

**RA-8.** `mtl_sdp_parse(sdp, len, ...)` takes text that need not be NUL-terminated, and fills `meta` strings "pointing into the parsed text". They have no length and no terminator, so `printf("%s", meta.session_name)` reads past the field. Their lifetime is also tied to the caller's buffer. Fix E8: fixed `char` arrays, copied and terminated.

**RA-9.** Crypto uses the extension chain, but `mtl_session_get_config()` cannot return `next`. So:

- `mtl_sdp_render`, "built only on public calls" (mtl_sdp.h:7), cannot learn the PEP attributes it promises to render (extmap IDs, scheme, mode);
- `mtl_sdp_parse` cannot produce the block for a receiver;
- it is unstated whether `next` is compared in a get_config → update round-trip.

The chain has exactly one kind. Fix E9: the PEP parameters become options 341-348 (none is secret; keys stay in `mtl_crypto_set_key`). Delete `mtl_ext_hdr` and `mtl_ext_kind`, and return `next` to "reserved, NULL" (D-33).

**RA-10.** Readiness is too broad:

- `TIME_UNLOCKED` stays set forever under FREERUN, SYSTEM_TAI and the FREERUN fallback, so a pod without PTP never becomes ready, although 17 §4 says such a pod "is an IPMX sender ... and not an error".
- `SESSION_ERROR` and `FLOW_PENDING` (for example a TX whose unicast receiver is down) take the whole pod out of service.
- 17 §2.6 uses readiness as the gate for NMOS registration, so one bad stream unregisters the Node.

Fix E10.

**RA-11.** ex13 has three bugs:

- (a) `mtl_flow_parse` zero-fills the flow (mtl_util.h:39-40). That wipes port, dscp, ttl, payload_type, flow_flags and ssrc, and ssrc 0 means a new random SSRC on every activation.
- (b) The destination change and the enable/disable are two updates. "A new update replaces a pending one", so a scheduled PATCH that has both loses the first. Enabling a reserved leg also needs FLOWS and LEGS in one call (17 §2.2).
- (c) `applied_at` returns `-MTL_EAGAIN` although nothing is armed. It polls forever on FAILED, REPLACED and CANCELLED, and reports whichever update came last, since it has no seq.

Fix §4.3.

**RA-12.** `MTL_STOP_FLUSH` says "a unit partly sent is finished". 03 §3.4 says only that "packets already handed to the NIC complete", and is silent on the rest of the unit. The finished unit's result status (ON_TIME/LATE, or FLUSHED) is unstated. The unit that `mtl_instance_abort` cuts has no defined status, reason or flag, and there is no reason code for abort. Fix E12, plus a row in 03 §3.4.

**RA-13.** The close outcomes have four gaps:

- (a) "1" requires that no library thread runs application code. A sink stuck past the deadline (`threads_unjoined`) is then neither 1 nor `-MTL_ETIMEDOUT`.
- (b) "Always consumes" contradicts "any thread but a library thread". It is unstated what a dispatch or log thread gets (`-MTL_EDEADLK`?) and whether mt is then consumed; the same holds for `mtl_instance_close`.
- (c) After 1, retirement cannot be observed: there is no handle and the queues are closed. It is also unstated whether the same ports can be reopened in this process.
- (d) Only sessions are said to be closed by it, not queues, timelines or regions.

Fix E13.

**RA-14.** ST 2110-20:2022 §7.2 lists colorimetry among the required parameters, so "colorimetry 0 = unspecified, the SDP omits it" renders a non-compliant SDP. UNSPECIFIED is a permitted value (§7.5), so render must write it. RANGE=FULLPROTECT is not allowed with BT2100 (§7.3) and must be rejected at create. Fix E14.

## 3. Answers to the five questions

### 3.1 Consistency with R1–R8 and with each other

**Return conventions (R1, R2).**

- Consistent: `mtl_instance_get_health` (flags ≥ 0), `mtl_sdp_render` (length), `mtl_sdp_parse` (leg count), `mtl_rtcp_read` (1 or `-MTL_EAGAIN`), `mtl_rtcp_mib_next` (1, 0 at the end), `mtl_port_specs_from_network_status` (count), `mtl_instance_close` (0/1/`-MTL_ETIMEDOUT`).
- Not consistent: CANCEL (RA-3), render (RA-17), `mtl_index_at` (RA-17, older), and ex13's own `applied_at` (RA-11).
- Unstated: the result of `mtl_audio_remap` (RA-27).

**R3: `struct_size` against size arguments.**

- Correct inputs with `struct_size`: `mtl_rtcp_info`, `mtl_rtcp_media_desc`, `mtl_time_reference`, `mtl_ext_hdr` / `mtl_ext_crypto` (`MTL_INIT` works because `hdr` is at offset 0).
- Correct outputs with no `struct_size` and a size argument: `mtl_rtcp_sr`, `mtl_health`, `mtl_shutdown_report`, `mtl_session_status`.
- `mtl_sdp_meta` is read by render and written by parse, so it falls under R3's "written and read back" rule: acceptable.
- The one violation is `mtl_port_spec` used as an output with no size argument (RA-18).

**R6: call classes.**

| Call | Declared | Verdict |
|---|---|---|
| `mtl_instance_get_health` | DP | Right, if the handle table is type-stable and the record is published, not formatted at call time (RA-36). The loop age is a TSC difference, with no syscall. |
| `mtl_rtcp_read` | WT | Right: a data read with arming (`MTL_WAIT_RTCP`). The Info Block copy is bounded (< 1500 B), like reap's record copy, so it needs no DPC. |
| `mtl_sdp_render`, `mtl_sdp_parse` | CP | Right. Render calls CP getters; parse is pure and could be AS (RA-41), but CP does no harm. |
| `mtl_audio_remap` | DPC | Right as a class: work in the caller, no instance, like `mtl_convert_am824_*`. The finding is about scope (RA-27). |
| `mtl_rtcp_mib_build`, `mtl_rtcp_mib_next` | AS | Right (pure). |
| `mtl_crypto_set_key`, `mtl_crypto_clear_keys`, `mtl_time_set_reference`, `mtl_instance_shutdown`, `mtl_port_specs_from_network_status` | CP | Right (file I/O for the last). |
| `mtl_instance_interrupt` / `mtl_instance_abort` | AS | Right. ex11 relies on it. |
| `mtl_tx_submit` / `mtl_rx_dequeue` under crypto | "become DPC" | Acceptable: a class that depends on configuration, as with packet units. Say it in R6 once, not per header. |

**Handle consumption.**

- `mtl_instance_close` and `mtl_instance_shutdown` "always consume", which conflicts with the refusal on library threads (RA-13 b) and is undefined for shared references (RA-4).
- A session closed by the instance, then closed by the application, contradicts R4 (RA-35).

**Naming prefixes.**

- Clean: `mtl_rtcp_*`, `mtl_crypto_*`, `mtl_sdp_*`, `mtl_time_set_reference`, `mtl_instance_get_health`, `mtl_instance_shutdown`.
- Off: `mtl_audio_remap` in `mtl_convert.h` (RA-27), and the crypto mode values (RA-39).

**Enum and flag collisions.**

| Pair | Problem |
|---|---|
| `enum mtl_rtcp_sr` / `struct mtl_rtcp_sr`; `MTL_RTCP_SR_OFF..IPMX` (1-3) / `MTL_RTCP_SR_INFO`, `_CHANGED` (1, 2) | a compile error, and one prefix for two families (RA-1) |
| `MTL_SHUTDOWN_ALL_REFERENCES`, `_DRAIN` (1, 2) / `MTL_SHUTDOWN_RETIRED..NOT_QUIESCED` (0-2) | one prefix, overlapping values (RA-15) |
| `MTL_UPDATE_*` parts and modifiers / `MTL_UPDATE_STATE_*` | one prefix (RA-16, Nit) |
| `MTL_TIME_FREERUN` (state) / `MTL_TIME_SOURCE_FREERUN` / `MTL_FALLBACK_FREERUN` | three meanings of one word (RA-24) |
| `MTL_SENDER_*` (sender type) / `MTL_MEDIA_SENDER` / `MTL_MEDIACLK_SENDER` / `MTL_INFO_MEDIACLK_SENDER` | readable once the mediaclk names say "mediaclk"; acceptable |
| `MTL_IGMP_V2 = 2`, `MTL_IGMP_V3 = 3` | fine: the values are versions, 0 is still "absent", and nothing else uses the prefix |
| `MTL_CRYPTO_IN_PLACE` (flag) / `MTL_CRYPTO_PEP_RTP` (enum), both 1 | Nit (RA-39) |

**Option key numbering.**

- No numeric collision.
- Ranges out of their documented groups: RA-19.
- Every new key has a name, a default and a C/S/R class. Naming drift (`MTL_OPT_RTX` = "rtx.enable", `MTL_OPT_IGMP_REPORT_INTERVAL_MS` = "port.igmp_report_ms", `MTL_OPT_CPU_SHARED_POLICY` = "instance.cpu_shared") follows earlier precedent: Nit.

**Event numbering.**

- 23-29 are contiguous and collide with nothing.
- `MTL_SUB_*` lists the new instance events: PORT_REMOVED and PORT_ADDRESS under PORT, TIME_SOURCE under TIME, SCHED_STALLED and HEALTH under INSTANCE.
- UPDATE and RTCP_INFO are session events, which reach a queue through `MTL_BIND_EVENTS`: correct.
- The name TIME_SOURCE is misleading (RA-24).

**Reason groups.**

- Correct: 10-11 in lifecycle, 114-115 in device, 600-614 a new and documented environment group, 519 NO_KEY among the TX outcomes.
- 112-113 (scheduler and worker stalls) are not device reasons, but SCHED_OVERLOAD (111) already set that precedent: Nit.
- Missing: `UPDATE_COMMITTING` (12, E3) and `ABORTED` (520, E12).

**Zero defaults (R3, the D-22 rule kept by D-74).**

- Correct:
  - `tcs` 0 = SDR (§7.6 default) and `range` 0 = NARROW (§7.3 default).
  - `vtotal`, `htotal` and `max_bitrate_bps` 0 = derived.
  - `legs_disabled` 0 = all enabled.
  - `MTL_MEDIA_SENDER`, `MTL_TIME_SOURCE_FREERUN` and every new option enum are non-zero.
  - The output enums (`mtl_update_state` NONE = 0, `mtl_phase`) are fine.
- Wrong: colorimetry 0 is a permitted value, but the "SDP omits it" rule is not (RA-14).
- Broken by the profile: `sender_type` and `dscp` (RA-2).
- Unstated: whether `mtl_ext_crypto.scheme`, `mode` and `ext_id_full` (0 is not a valid RFC 8285 ID) are required, and whether an all-zero `iv` is accepted (moot with E9).

**Size checks.** Every new struct has one, and the lint passes. What is missing is a test that the headers compile together (RA-33).

### 3.2 Leanness

What landed in `mtl.h` today, and where it belongs:

| Addition | Verdict |
|---|---|
| R8, `MTL_INSTANCE_PREFLIGHT`, `env:` ports, the new close contract | Core: every program in a pod needs them. Keep. |
| `mtl_port_spec.mac` | Keep, but "ignored on input" (RA-18). Alternatively the stat `port.mac`, but the field is cheap. |
| `MTL_TIME_SOURCE_FREERUN`, `MTL_MEDIA_SENDER`, `MTL_SENDER_IPMX`, `MTL_INFO_MEDIACLK_SENDER`, `MTL_UNITF_SENDER_TIME`, `MTL_STATUS_MUTED` | One value each, in enums and flag sets that are core. Keep. |
| colorimetry, tcs, range | Keep in core: every ST 2110-20 sender needs them for its SDP and IS-04, and conversion needs them for its matrices. |
| `vtotal`, `htotal`, `max_bitrate_bps` | Move to options (RA-29). They are IPMX tuning with derived defaults. |
| Reserved legs, mute, `REAPPLY`, `DRY_RUN`, `planned_tai_ns` | The IS-05 contract on a core call. Keep; fix the rules (RA-6, RA-31). |
| `MTL_UPDATE_CANCEL` | Drop: `parts` 0 cancels (E3). |
| `status.update_*` and `enum mtl_update_state` | Keep four fields; drop `update_planned_tai_ns` (RA-30). A separate getter would add a function to save 24 bytes in an output struct, which is the wrong trade. |
| Meta records chain, `MTL_META_RTCP_MIB` | Keep: one enum value, and the chain makes the layout explicit. Fix RA-21 and RA-22. |
| `next`, `struct mtl_ext_hdr`, `enum mtl_ext_kind` | Remove from `mtl.h` (E9). Two extension mechanisms (options and a chain) for one user is not lean. `next` stays as the D-33 reserved pointer. |

Functions that could be dropped or merged:

- **`mtl_instance_shutdown` against `mtl_instance_close`.**
  - A merge is possible: the DRAIN flag as an option `instance.close_drain` (R), and the summary in `mtl_last_error().detail` after close. But `mtl_last_error` is valid only after a failure (R1/D-84), so 0 and 1 would need a new rule, and `MTL_SHUTDOWN_ALL_REFERENCES` has no good option form.
  - Recommendation: keep it. It is one function in an optional header, and the termination message is a real K-REQ.
  - Slim it instead: drop `outcome` and its enum (RA-15).
- **`mtl_rtcp_mib_build`.** Drop (E28). **`mtl_rtcp_mib_next`**: keep, since RX needs to iterate MIBs and it is pure.
- **`mtl_port_specs_from_network_status`.** Cut from the library (RA-26).
- **`mtl_audio_remap`.** Cut, or move under `MTL_LATER` (RA-27).

**Three new headers, or merged?**

- `mtl_sdp.h` → `mtl_util.h`. Both carry the same contract ("built only on public calls, allocates nothing"), which is exactly `mtl_util.h`'s job.
- `mtl_rtcp.h` and `mtl_crypto.h` stay separate.
  - RTCP has an RFC 3550 mode that is not IPMX-only.
  - Crypto has its own security review, key-handling rules and implementation phase (Q-NI-3).
  - A combined `mtl_ipmx.h` would pull key handling into every RTCP user, and RTCP into every non-IPMX crypto user (HDCP packet units).

**Function counts.** `check.sh` per header; "before" is today's count minus the 13 functions that 16 and 17 list (126 in total, as 17 §1 says).

| Header | Before K+N | Now | Proposed |
|---|---|---|---|
| mtl.h | 33 | 33 | 33 |
| mtl_observe.h | 14 | 16 | 16 |
| mtl_util.h | 14 | 15 | 16 (−network-status, +2 SDP) |
| mtl_sync.h | 14 | 15 | 15 |
| mtl_convert.h | 3 | 4 | 3 |
| mtl_rtcp.h | — | 4 | 3 |
| mtl_crypto.h | — | 2 | 2 |
| mtl_sdp.h | — | 2 | merged into mtl_util.h |
| the other 9 headers | 48 | 48 | 48 |
| **total** | **126** | **139** | **136** (+2 under `MTL_LATER`: 8) |
| headers | 14 | 17 | 16 |

`mtl.h` stays at 33 functions. It loses about 20 lines (`mtl_ext_hdr`, `mtl_ext_kind`, CANCEL, `vtotal`/`htotal`/`max_bitrate_bps`, `update_planned_tai_ns`) and gains about 10 lines of contract text (E3, E5, E6, E13). Lines are not the measure; types and names a basic sender sees are, and those go down by two types and four fields.

### 3.3 Ambiguities and contradictions an implementer hits

| Question | Answer in the files | Finding |
|---|---|---|
| "Always consumes mt" against shared instances | undefined handle identity | RA-4 |
| "1: quiesced": how is retirement observed later? Is there a wait? | no handle, no event and no wait. Fine if the rule is "exit", but long-lived hosts (plugins) need "ports reopenable after 1" | RA-13 c |
| Memory under leases after 1 | four different answers | RA-5 |
| Mute against results-off sessions | units "DROPPED/LEG_DISABLED", but with results off nobody sees them, and it is unstated whether slots retire at their slot time (back-pressure) or at once (spin) | RA-6 |
| Reserved legs against "a leg exists when udp_port != 0" (09 §7, §9.3) | 09 contradicts the header in two places | RA-6 |
| FLUSH "a unit partly sent is finished" against 03 §3.4 and `mtl_instance_abort` | the rest of the unit, its status, and abort's cut unit are undefined | RA-12 |
| `planned_tai_ns` for CREATED and STOPPED sessions | undefined (no unit boundary exists) | RA-31, E3 |
| DRY_RUN return values | "validate, reserve nothing" only; no word on status, seq, replacement, or `planned_tai_ns` | RA-31, E3 |
| CANCEL with `sc` NULL against `parts` semantics | CANCEL is a bit inside `parts`; CANCEL together with FLOWS is undefined | RA-3, E3 |
| Meta records "at most one per kind" against ANC | ANC is fine (one record, count packets); USER tags are not | RA-22 |
| SENDER mode against `mtl_session_start`, timelines and `media_index` | undefined | RA-23 |
| FREERUN against the `MTL_TIMEF` flags | VALID\|ESTIMATED always, `TXR_ESTIMATED` on every result, but the state name is unstated and readiness never comes | RA-10, RA-24 |
| Options at update time | ignored, which contradicts 17 §2.2 | RA-7 |
| `next` in get_config and update | undefined | RA-9 |

### 3.4 The examples

See §4. ex01 is correct and minimal (one Nit, RA-38). examples_cpp.cpp is correct: `held_ = ret == -MTL_EAGAIN` matches mtl.h:1026-1028, and the reap loop handles a negative `n`. ex11 has four small defects (RA-25). ex13 has three real bugs (RA-11).

### 3.5 Simplifications and cuts

§5 gives the edits and §6 the cut list.

## 4. Examples

### 4.1 ex01_tx_video.c

It is correct. `mtl_session_close` before `mtl_instance_close` is the right order, because session close drains and instance close flushes (RA-34). Nit RA-38: count frames only when one is submitted.

### 4.2 ex11_signal.c

```c
static void on_term(int sig) {
  (void)sig;
  mtl_instance_interrupt(g_mt, 1); /* idempotent; every wait returns -MTL_ECANCELED */
  if (g_signals) mtl_instance_abort(g_mt); /* the second signal: stop at the next packet */
  g_signals = 1;
}
/* install_handlers(): also block SIGTERM and SIGINT (pthread_sigmask) before creating the
   workers, so only the main thread runs the handler. */

int shutdown_all(mtl_instance_h mt, int64_t grace_ns) {
  struct mtl_shutdown_report r;
  int64_t budget = grace_ns > MTL_SEC(3) ? grace_ns - MTL_SEC(2) : MTL_SEC(1);
  memset(&r, 0, sizeof(r)); /* a call that fails early may fill nothing */
  int ret = mtl_instance_shutdown(mt, MTL_SHUTDOWN_ALL_REFERENCES, budget, &r, sizeof(r));
  ...
```

`MTL_SHUTDOWN_ALL_REFERENCES` belongs there because the example is "the application that owns main()" (16 §2.1). The probe handler is right as written, once E10 narrows `MTL_HEALTH_READINESS`.

### 4.3 ex13_nmos_switch.c

One IS-05 PATCH is one update. Only the addresses come from the parsed flow, and the applied-state helper tracks its own update:

```c
/* One activation: new destinations (NULL keeps a leg) and rtp_enabled per leg, at `when`
   (NULL = activate_immediate). Identical legs are re-applied, as IS-05 asks. */
int activate(mtl_session_h s, const char* const dest[MTL_MAX_LEGS], uint32_t legs_disabled,
             const struct mtl_when* when, int64_t* planned_tai_ns, uint64_t* seq) {
  struct mtl_session_config sc;
  struct mtl_session_status st;
  MTL_INIT(&sc);
  int ret = mtl_session_get_config(s, &sc);
  for (int i = 0; ret >= 0 && i < MTL_MAX_LEGS; i++) {
    struct mtl_flow f;
    if (!dest[i]) continue;
    ret = mtl_flow_parse(dest[i], &f); /* zero-fills f: copy only what IS-05 changed */
    if (ret < 0) break;
    memcpy(sc.flows[i].ip, f.ip, sizeof(f.ip));
    memcpy(sc.flows[i].source_filter, f.source_filter, sizeof(f.source_filter));
    sc.flows[i].udp_port = f.udp_port;
  }
  sc.legs_disabled = legs_disabled; /* all set: master_enable = false (muted) */
  if (ret >= 0)
    ret = mtl_session_update(s, &sc, MTL_UPDATE_FLOWS | MTL_UPDATE_LEGS | MTL_UPDATE_REAPPLY,
                             when, planned_tai_ns);
  if (ret >= 0) ret = mtl_session_get_status(s, &st, sizeof(st));
  if (ret >= 0) *seq = st.update_seq; /* the Node serialises updates per session */
  return ret < 0 ? ex_fail("activate", ret) : 0;
}

/* 1: switched at *tai_ns (the 200 response); 0: still pending; -MTL_ECANCELED: replaced or
   cancelled; -MTL_EIO: failed (status.update_reason). */
int applied_at(mtl_session_h s, uint64_t seq, int64_t* tai_ns) {
  struct mtl_session_status st;
  int ret = mtl_session_get_status(s, &st, sizeof(st));
  if (ret < 0) return ret;
  if (st.update_seq != seq || st.update_state == MTL_UPDATE_STATE_REPLACED ||
      st.update_state == MTL_UPDATE_STATE_CANCELLED)
    return -MTL_ECANCELED;
  if (st.update_state == MTL_UPDATE_STATE_FAILED) return -MTL_EIO;
  if (st.update_state != MTL_UPDATE_STATE_APPLIED) return 0;
  *tai_ns = st.update_applied_tai_ns;
  return 1;
}
```

`set_enabled` disappears into `activate`. This sketch needs `<string.h>`, and Lint 4 requires the same text in 10-api-sketch.md.

### 4.4 examples_cpp.cpp

It is correct and idiomatic. No change.

## 5. Exact header edits

Each edit keeps every capability. The sizes stay as they are unless an edit says otherwise.

**E1 (RA-1), mtl_rtcp.h and mtl_options.h.**

```c
/* mtl_options.h:239 */
enum mtl_rtcp_sr_mode { MTL_RTCP_SR_OFF = 1, MTL_RTCP_SR_RFC3550 = 2, MTL_RTCP_SR_IPMX = 3 };
/* mtl_rtcp.h:70-88, 96 */
#define MTL_RTCP_RPT_INFO 0x1u    /* an Info Block was present (copied into buf) */
#define MTL_RTCP_RPT_CHANGED 0x2u /* its version differs from the previous report's */
struct mtl_rtcp_report { /* fields unchanged; flags: MTL_RTCP_RPT_* */ };
MTL_API_WT int mtl_rtcp_read(mtl_session_h s, struct mtl_rtcp_report* rpt, size_t rpt_size,
                             void* MTL_NULLABLE buf, size_t cap, int64_t timeout_ns);
MTL_SIZE_CHECK(mtl_rtcp_report, 48);
```

**E2 (RA-2), mtl.h:484, 534-539.**

```c
  uint8_t dscp; /* 0 = the profile's default (ST2110: CS0; IPMX: AF41 audio, AF42 others) */

enum mtl_sender_type { /* ST 2110-21 */
  MTL_SENDER_DEFAULT = 0, /* by profile: ST2110 N, IPMX the TR-10-1 schedule */
  MTL_SENDER_N = 1,
  MTL_SENDER_NL = 2,
  MTL_SENDER_W = 3,
  MTL_SENDER_IPMX = 4, /* TR-10-1 gapped schedule, CMAX from the packet count, IPMX VRX */
};
```

**E3 (RA-3, RA-16, RA-31), mtl.h:848-865 and mtl_reasons.h.**

```c
/* Modifiers (in parts) */
#define MTL_UPDATE_REAPPLY 0x10u /* re-apply identical enabled legs: RX leave + join, TX
                                    neighbour and headers again (IS-05 re-activation) */
#define MTL_UPDATE_DRY_RUN 0x20u /* validate and plan: planned_tai_ns is filled; nothing is
                                    reserved, posted or replaced; status is untouched */
/* ... A new update replaces a pending one (REPLACED). parts 0 with sc and when NULL cancels
   the pending one: 0 cancelled (CANCELLED), 1 none pending, -MTL_EBUSY (UPDATE_COMMITTING)
   when it is already committing and will apply. update_seq counts updates that were posted
   (not dry runs, not cancels). In CREATED and STOPPED the change applies during the call,
   *planned_tai_ns = INT64_MIN and update_state = APPLIED with applied INT64_MIN; in ARMED
   a `when` before T0 means T0. CP. */
/* mtl_reasons.h, lifecycle */
  MTL_REASON_UPDATE_COMMITTING = 12, /* cancel too late: the update applies */
```

Delete `MTL_UPDATE_CANCEL`.

**E4 (RA-4), mtl.h:442-444.**

```c
   MTL_INSTANCE_SHARED: the first open creates the process-wide instance, later ones join
   it; each open returns its own reference handle (MTL_SAME is false between two), and
   every call accepts any live reference. ...
```

**E5 (RA-5, RA-35), mtl.h R4 and close.**

```c
 * R4 ... A stale or foreign handle fails with -MTL_EBADF, a lease already returned with
 *     -MTL_ESTALE; on a session the instance closed, close returns 0 and a lease's release
 *     returns 0.
/* close: ... 1: quiesced ... but leases or regions are still referenced; a slot's memory is
   freed after its last lease is returned, and leases never returned keep their memory
   mapped until exit (bytes_kept_until_exit). */
```

**E6 (RA-6), mtl.h:476-479, 687, 690-692, 842-845.**

```c
/* One leg's network flow. The legs of a session are fixed at create (STOPPED: FLOWS or LEGS
   may change the set): a leg exists when its udp_port is not 0 or its legs_disabled bit is
   set (a reserved leg, all zero until an update gives it an address together with LEGS). */
  struct mtl_flow flows[MTL_MAX_LEGS]; /* flows[1] that exists is the ST 2022-7 leg */
  uint32_t legs_disabled; /* bit per existing leg, bits >= MTL_MAX_LEGS -MTL_EINVAL: admin
                             down, reserved; every existing leg set = muted: RUNNING, TX units
                             retire at their slot as DROPPED (LEG_DISABLED), RX groups left */
#define MTL_UPDATE_LEGS 0x2u /* sc->legs_disabled; a bit for a leg that does not exist is
                                -MTL_EINVAL unless STOPPED */
```

Delete 09 §7.2's "disabling the last enabled leg is `-MTL_EINVAL`", and let 09 §9.3 except reserved legs.

**E7 (RA-7), mtl.h:855-856.**

```c
   differs). Options in sc->options and sc->spec_options whose key may change while running
   (R) apply at the same boundary as FLOWS; any other key there must equal its current
   value (-MTL_EBUSY, OPTION_STATE). direction, essence and unit never change. ...
```

**E8 (RA-8), mtl_sdp.h:25-37.**

```c
/* What MTL does not know; all optional ("" = default). Render reads it; parse fills it,
   every string copied and NUL-terminated (truncated: -MTL_ENOSPC naming the field). */
struct mtl_sdp_meta {
  uint32_t struct_size;
  uint32_t sdp_flags;       /* MTL_SDP_* */
  uint64_t session_id;      /* o=; 0 = derived from the session's creation time */
  uint64_t session_version; /* o=; 0 = status.update_seq */
  char session_name[64];    /* s=; "" = the session name */
  char mid[MTL_MAX_LEGS][16]; /* a=mid; "" = "primary", "secondary" */
  char channel_order[64];   /* audio */
  char fmtp_extra[128];     /* appended to a=fmtp: profile, level, PAR, DID_SDID */
  uint64_t reserved[4];
};
MTL_SIZE_CHECK(mtl_sdp_meta, 344);
```

**E9 (RA-9), mtl.h:703, 714-723, mtl_crypto.h, mtl_options.h.**

```c
/* mtl.h: delete enum mtl_ext_kind, struct mtl_ext_hdr and its size check */
  const void* MTL_NULLABLE next; /* reserved, NULL */
/* mtl_options.h, payload encryption (mtl_crypto.h); none of these is secret */
  MTL_OPT_CRYPTO_SCHEME = 341,     /* "crypto.scheme" MTL_CRYPTO_PEP_* (absent = no crypto) C */
  MTL_OPT_CRYPTO_MODE = 342,       /* "crypto.mode" MTL_CRYPTO_AES* (AES128_CTR) C */
  MTL_OPT_CRYPTO_EXT_ID_FULL = 343, /* "crypto.ext_id_full" RFC 8285 ID 1-14, required C */
  MTL_OPT_CRYPTO_EXT_ID_SHORT = 344, /* "crypto.ext_id_short" RFC 8285 ID 1-14, required C */
  MTL_OPT_CRYPTO_CLEAR_BYTES = 345, /* "crypto.clear_bytes" (the essence's payload header) C */
  MTL_OPT_CRYPTO_IV = 346,         /* "crypto.iv" the 64-bit base IV, required C */
  MTL_OPT_CRYPTO_SUBSTREAM = 347,  /* "crypto.substream" 0..1023 (0) C */
  MTL_OPT_CRYPTO_IN_PLACE = 348,   /* "crypto.in_place" bool (0) C */
/* mtl_crypto.h: delete struct mtl_ext_crypto, MTL_CRYPTO_IN_PLACE and the size check; keep
   the two enums (values MTL_CRYPTO_AES128_CTR, ..., RA-39) and the two key calls. */
```

`mtl_sdp_parse` then writes the PEP parameters into `spec_options`, and render reads them with `mtl_get_option`. The "built only on public calls" claim becomes true.

**E10 (RA-10, RA-24), mtl_observe.h:199-208 and mtl.h:323.**

```c
#define MTL_HEALTH_TIME_UNLOCKED 0x400u /* the time base is ACQUIRING or LOST (FREERUN and
                                           HOLDOVER are ready: IPMX runs without PTP) */
#define MTL_HEALTH_SESSION_ERROR 0x10000u /* informational: a started session is in ERROR */
#define MTL_HEALTH_FLOW_PENDING 0x20000u  /* informational: an enabled leg is not resolved */
#define MTL_HEALTH_READINESS 0xffffu       /* the liveness bits and 0x100-0x8000 */
/* mtl.h, enum mtl_time_state: FREERUN = the source is FREERUN or SYSTEM_TAI, or AUTO fell
   back to free run; LOCKED only for PTP_BUILTIN, PHC, CLOCK_TAI and USER sources. */
/* mtl_queue.h:55 */
  MTL_EVENT_GRANDMASTER = 27, /* port; value[0] new clockIdentity, [1] old */
```

The flags field stays `uint32_t`. The return value is an `int`, and the bits stay below 2^31.

**E12 (RA-12), mtl.h:293, 467-470, mtl_reasons.h.**

```c
  MTL_STOP_FLUSH = 1, /* queued units become FLUSHED (STOP_FLUSH); a unit whose first packet
                         left is sent to its end at its pace and gets its normal status */
/* abort: ... every TX session stops at its next packet: the cut unit is FLUSHED (ABORTED)
   with MTL_TXR_PKT_SHORT, queued units FLUSHED (ABORTED). ... AS. */
  MTL_REASON_ABORTED = 520, /* mtl_instance_abort() */
```

**E13 (RA-13), mtl.h:452-463.**

```c
/* ... 0: retired. 1: quiesced: no device can reach any memory, every library thread is
   joined or stuck in application code past the deadline (counted, its memory kept), the
   ports are released and may be opened again, but leases or regions are still referenced.
   -MTL_ETIMEDOUT: a port could not be quiesced. Sessions, queues and timelines still open
   are closed by it, and a later close of their handles returns 0. From a library thread
   (dispatch, log sink, codec): -MTL_EDEADLK and mt is not consumed. ... CP. */
```

**E14 (RA-14), mtl_format.h:51-53.**

```c
/* ST 2110-20 colorimetry, TCS and RANGE (§7.5, §7.6, §7.3) ...: SDP and conversion only.
   0 = colorimetry UNSPECIFIED (rendered, as §7.2 requires it), TCS SDR, RANGE NARROW;
   FULLPROTECT with BT2100 is -MTL_EINVAL. */
```

**E15 (RA-15), mtl_observe.h:240-261.** Delete `enum mtl_shutdown_outcome`. In `struct mtl_shutdown_report`, replace `uint32_t outcome;` with `uint32_t references_left; /* > 0: only this reference was dropped */`. The size does not change.

**E23 (RA-23), mtl.h:266-269.**

```c
  /* unit.media_tai_ns is the source's own sampling instant, never snapped: an async source
     (IPMX mediaclk:sender). k counts units from the first after start (media_index of the
     results); RTP = RTP0 + floor(k x period x rate), RTP0 = floor(M0 x rate); launch = M +
     min_tx_delay_ns, the gapped model over the nominal period; a unit that would overlap
     the previous one is DROPPED (WOULD_OVERLAP). AT_INDEX, RX_BY_INDEX targets, tx.precede
     and mtl_index_at are -MTL_EINVAL on such a session (17 §4). */
```

**E28 (RA-28), mtl_rtcp.h:32-66.** Delete `struct mtl_rtcp_media_desc` and `mtl_rtcp_mib_build`. Into `struct mtl_rtcp_info` (replacing `reserved0` and two reserved words; 184 B kept) go:

```c
  uint32_t par_num, par_den;              /* pixel aspect ratio; 0 = 1:1 */
  uint64_t measured_rate_milli;            /* 0 = nominal */
  const char* MTL_NULLABLE channel_order;  /* audio: "SMPTE2110.(ST,ST)" */
#define MTL_RTCP_MIB_APP_ONLY 0x4u /* send only the application's MIBs (default: the library's
                                      essence MIB first, then mib) */
```

**E29 (RA-29), mtl.h:557-559, 584 and mtl_options.h.**

```c
  MTL_OPT_VIDEO_VTOTAL = 505,  /* "video.vtotal" total lines (the ST 2110-21 default) C */
  MTL_OPT_VIDEO_HTOTAL = 506,  /* "video.htotal" total pixels per line (derived) C */
  MTL_OPT_CVIDEO_MAX_BITRATE = 515, /* "cvideo.max_bitrate_bps" VBR_MAX ceiling (from
                                       codestream_bytes) C */
```

In `mtl_video_config` the fields become `uint64_t reserved[4];` after `troffset_ns` (96 B kept). In `mtl_cvideo_config`, `max_bitrate_bps` becomes reserved (96 B kept).

## 6. Cut, or move under `MTL_LATER`

| Item | Action | Why it costs no use case of today |
|---|---|---|
| `mtl_port_specs_from_network_status` | cut to a sample (`samples/k8s_ports.c`) or `MTL_LATER` | new; `env:` + `MTL_PORTS` cover the device, and the IP comes from the pod's own config |
| `mtl_audio_remap`, `MTL_AUDIO_SILENT` | cut or `MTL_LATER` | new; MTL has no channel map today, and IS-08 is Node logic |
| `mtl_rtcp_mib_build`, `struct mtl_rtcp_media_desc` | delete (E28) | the library builds the same block from the session |
| `struct mtl_ext_hdr`, `enum mtl_ext_kind`, `struct mtl_ext_crypto` | delete (E9) | the same parameters as options |
| `MTL_UPDATE_CANCEL` | delete (E3) | `parts` 0 |
| `enum mtl_shutdown_outcome` | delete (E15) | the return value |
| `status.update_planned_tai_ns` | delete (RA-30) | the call returns it |
| `mtl_sdp.h` | merge into `mtl_util.h` | the same contract |

Keep `mtl_instance_shutdown`, `mtl_rtcp_mib_next`, `mtl_time_set_reference` and both crypto key calls.

## 7. Probe for check.sh (RA-33)

```bash
# Lint 5: every header together, in both languages, in reverse order too.
all="$(for h in "${headers[@]}"; do echo "#include <mtl/experimental/$h>"; done)"
run "gcc C99 all headers" sh -c "echo '$all' | gcc ${cflags[*]} -pedantic -x c -"
run "g++ C++17 all headers" sh -c "echo '$all' | g++ ${cxxflags[*]} -pedantic -x c++ -"
run "gcc C99 all headers reversed" sh -c "echo '$all' | tac | gcc ${cflags[*]} -pedantic -x c -"
```

On today's files, the first line reproduces RA-1:

```text
mtl_rtcp.h:73:8: error: 'mtl_rtcp_sr' defined as wrong kind of tag
mtl_options.h:239:6: note: 'mtl_rtcp_sr' has a previous declaration here
```
