# 05 — Timing, pacing, timestamps, epochs, lateness: current state

| Field | Value |
|---|---|
| Topic | TX pacing modes, timestamp inputs/outputs, epoch selection, lateness, RTP derivation, A/V sync, latency/buffering |
| Revision | r1 |
| Date | 2026-09-29 |
| Code basis | `main` HEAD `545a266a`, read from `git archive HEAD` (the working tree carries unrelated local edits in `st_tx_video_session.c`, `mt_ptp.c` and others; all `path:line` below are HEAD lines) |
| Key inputs | `doc/user-pacing-timestamp-contract.md` (untracked proposal, "the contract"), `doc/triage/1722-rtp-level-limits-and-pacing.md`, KB §5, `doc/design.md` §4.3, §5.4, §6.6, §6.11, §8.2 |
| Evidence labels | **[verified]** = read in code at the cited line; **[inferred]** = follows from reading several places or from a numeric simulation, not run on hardware; **[unknown]** = not determined |

---

## 0. Summary (read this if nothing else)

1. There is **one** `timestamp` field per unit and it feeds **both** pacing (`*_USER_PACING`) and RTP (`*_USER_TIMESTAMP`). The two meanings are selected by flags, not by separate fields. The maintainer's "two timestamps" idea maps exactly onto the two internal consumers that already exist (`tv_pacing_required_tai()` and `tv_update_rtp_time_stamp()`) **[verified]**
   `lib/src/st2110/st_tx_video_session.c:1774`, `:762`.
2. **Default video RTP is the scheduled TX time of packet 0, not the frame's media time.** It sits TR_OFFSET − VRX·TRS after the N·T_FRAME grid: +56/57 ticks (≈0.63 ms) at 1080p59.94, +67 ticks at 1080p50 **[inferred, simulated]**. Default ST40 RTP sits **on** the grid (no TR_OFFSET) **[verified]** `st_tx_ancillary_session.c:416-428`.
   So default ST20 and ST40 of the same frame carry **different RTP values**, which breaks the ST 2110-40 "same RTP as the associated video" expectation unless the app sets `ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH` or USER_TIMESTAMP.
3. **The TX pacing decision is made when the builder tasklet pulls the frame** (`get_next_frame`), not when the app submits it. At that moment the session picks an epoch, and that choice is final. **"Late" is measured against the epoch boundary, not the frame's TX start.**
   A frame picked after its own start time but before the next epoch goes out immediately with no counter and no callback **[verified]** `st_tx_video_session.c:672-735`.
4. Late, early and invalid user timestamps are **never rejected**. They are counted (`stat_error_user_timestamp`) and then either sent ASAP, snapped, clamped, or **silently replaced by default pacing** (EXACT mode) **[verified]** `st_tx_video_session.c:1796-1805`. The contract's "reject explicitly, no silent fallback" is the opposite of what the code does today.
5. The default time source is **`CLOCK_REALTIME` (UTC), not TAI**, unless the built-in PTP is enabled or the app supplies `ptp_get_time_fn`. Every API still calls it "TAI" **[verified]** `lib/src/dev/mt_dev.c:2288-2289`, `lib/src/mt_main.h:1871-1876`. With built-in PTP, the source switches from UTC to the PHC mid-run when the master is first seen **[verified]**
   `lib/src/mt_ptp.c:1062-1067`. That is a ~37 s step **[inferred]**.
6. A/V sync today: the only exact recipe is **USER_TIMESTAMP on every essence**, which makes RTP exact and leaves TX to each session's own grid. USER_PACING alone snaps video to the nearest epoch (±T_FRAME/2) while audio honours the request almost exactly, so RTP-derived A/V offset is up to half a video frame plus TR_OFFSET.
   This is the error the maintainer describes **[verified + inferred]**. ST30P has no USER_TIMESTAMP flag at all, and ST40P silently ignores its USER_TIMESTAMP flag unless USER_PACING is also set **[verified]**.
7. Many timing flags and paths are silently ignored or downgraded (§9). The contract is a solid basis for **what the wire must look like**. It is thin on **API shape, clocks, RX, pipelines and multi-essence sync**, and some of its rules change behaviour that current tests pin (§8).

---

## 1. Pacing modes

### 1.1 Video (ST20/ST22) — `enum st21_tx_pacing_way` (`include/mtl_api.h:318-335`)

Pacing is chosen **per port at `mtl_init`** (`mtl_init_params.pacing`), copied into the session at create time **[verified]** `st_tx_video_session.c:3427-3435`. The session cannot request a mode.

| Way | Mechanism | Hardware / backend | Precision (claimed / observed) | Selection and fallback |
|---|---|---|---|---|
| `AUTO` (0) | Resolves at init to RL if driver `rl_type == MT_RL_TYPE_TM`, else TSC | — | — | `mt_dev.c:1468-1478` **[verified]** |
| `RL` | NIC TM shaper per queue at `tv_rl_bps()`; SW times packet 0 (TSC wait + warm-up pads), adds trained pads | `net_ice` PF, `net_iavf` VF (DPDK TM, `mt_dev.c:29-49`); AF_XDP on ice via sysfs `tx_maxrate` (`mt_af_xdp.c:277-291`) | KB: "sub-µs spacing". Packet-0 alignment is a TSC poll and warm-up can miss (`st_video_transmitter.c:121-139`) | See RL fallback list below |
| `TSC` | Transmitter polls TSC and releases a bulk of 4 when `mbuf.tsc <= now` | any backend | KB: scheduler poll adds 10–100 µs jitter. 3 packets of VRX budget consumed by bulk (`st_tx_video_session.c:578`) | Default when there is no TM. Forced when shared TX queue is used, **info log only, overrides an explicit RL/TSN** (`mt_dev.c:1460-1464`) |
| `TSC_NARROW` | TSC with `bulk = 1` | any | better than TSC **[unknown numbers]** | `st_tx_video_session.c:574-576` |
| `BE` | Packet 0 of a frame waits for TSC, the rest go at line rate (`bulk = 1`) | any | Frame start only. Not ST 2110-21 | `st_video_transmitter.c:377`, `:440` **[verified]** |
| `PTP` | Like TSC but polls `mt_get_ptp_time()` (PHC MMIO on PF) | PF + built-in PTP | Poll-bound, costlier per read **[inferred]** | `st_video_transmitter.c:561-650` |
| `TSN` | Per-packet launch time (`ptp_time_cursor`) in the descriptor dynfield; the NIC releases at PHC time | E830 PF (TxPP), igc queue 0; needs `MTL_FLAG_PTP_ENABLE`, not `PTP_SOURCE_TSC` | Hardware launch | Requirements are **explicit errors** at init (`mt_dev.c:1520-1533`); a launch time in the past is **not checked** (`st_video_transmitter.c:531-538`) |

RL fallback and downgrade paths **[verified]**:

- Explicit RL on a non-TM port: `-EINVAL` at init (`mt_dev.c:1481-1484`).
- AUTO + TM init fail: warn, then TSC (`mt_dev.c:1491-1515`).
- Per-session training fail: **silent** TSC, no log (`st_tx_video_session.c:535-542`).
- ST22 frame mode: **silent** TSC (`st_tx_video_session.c:3432-3434`).
- P/R pacing mismatch: both TSC, with a warning (`st_tx_video_session.c:545-552`).
- Runtime queue RL fail: err, then the **whole port** flips to TSC (`mt_dev.c:1650-1655`).
- NIC behaviour for a TSN launch time already in the past: **[unknown]**.

Other video pacing knobs:

- `st20_tx_ops.pacing` (`ST21_PACING_NARROW/WIDE/LINEAR`) changes only the start VRX (WIDE sets VRX to 80% of the packets in TR_OFFSET and disables warm-up). LINEAR has no special code path **[verified]** `st_tx_video_session.c:595-601`, grep finds no `ST21_PACING_LINEAR` use.
- `start_vrx`, `pad_interval`, `ST20_TX_FLAG_ENABLE_STATIC_PAD_P`, and `ST20_TX_FLAG_DISABLE_BULK` are exposed tuning knobs. They leak implementation into the public API.
- ST22: `vrx = 0`, `warm_pkts = 0`, `trs` recomputed per codestream (`st_tx_video_session.c:581-585`, `:750-758`). The code says "not sure the pacing for st22, none now".
- RTP-level ST20/ST22: always default pacing. USER_PACING is accepted and ignored (triage 1722 Finding A; `tv_sync_pacing(impl, s, 0, …)` at `st_tx_video_session.c:1410`, `:1484`).

### 1.2 Audio (ST30) — `enum st30_tx_pacing_way` (`include/st30_api.h:178-187`), per session

| Way | Mechanism | When selected |
|---|---|---|
| `TSC` | Builder stamps each packet with `tsc_time_cursor`. The transmitter holds it until reached, then bursts to a dedicated queue or to the shared audio transmitter ring (`st_tx_audio_session.c:1095-1176`) | Default |
| `RL` | Queue shaped to ~4× packet rate with 3 pad packets per audio packet, a warm-up/sync point every 10 ms, default accuracy target 40 µs (`rl_accuracy_ns`), and a user `rl_offset_ns` (`st_tx_audio_session.c:1315-1330`, `:1495-1560`) | AUTO: only if ptime < 0.5 ms **and** the port is RL. Explicit RL: only if ptime < 2 ms, otherwise **silently TSC** (`:2079-2112`) **[verified]** |
| `ST30_TX_FLAG_BUILD_PACING` | Waits in the builder as well (`:823-838`) | flag |

Under RL, packets between sync points follow the shaper rate, not per-packet user times **[inferred]**.

### 1.3 Ancillary (ST40) and fast metadata (ST41)

- TSC only. The builder waits in its own tasklet until `tsc_time_cursor` (`st_tx_ancillary_session.c:1016-1030`). There is no RL, TSN or model.
- **ST40 multi-packet units are spread evenly over the whole frame period**: `pkt_time = frame_time / total_pkts` (`st_tx_ancillary_session.c:1108`) **[verified]**. That bears no relation to ST 2110-40 LLTM/CTM windows (CTM allows 1 ms after the location-derived time; see contract Appendix D).
- ST41 TX has no EXACT mode and no `notify_frame_late` **[verified]** (grep).

---

## 2. Timestamp inputs and outputs

### 2.1 Clocks

| Clock | Source | Where used | Notes |
|---|---|---|---|
| "PTP"/"TAI" = `mt_get_ptp_time(impl, port)` → `inf->ptp_get_time_fn` | One of four sources, see the list below | All epoch math, RTP, launch time | Called TAI everywhere, but only TAI with built-in PTP (PHC carries the PTP timescale) or a correct user fn |
| TSC ns = `mt_get_tsc()` | `rte_get_tsc_cycles()` scaled in `double` (`mt_main.h:1850-1855`) | All TX release targets (`tsc_time_cursor`), stats | TAI→TSC mapping is taken once per frame (`cur_tsc + (start_tai − cur_tai)`, `st_tx_video_session.c:737-742`). No per-packet correction |
| `mtl_ptp_read_time()` | Cached: returns `ptp_usync + tsc_delta` if the last real read was < 10 ms ago (`mt_main.c:1100-1123`) | Public API, RX SW timestamps, `timestamp_last_pkt` | Cache fields updated non-atomically from any caller thread **[inferred race]**. Always reads port P |
| `mtl_ptp_read_time_raw()` | Direct `mt_get_ptp_time(P)` (`mt_main.c:1125-1135`) | Public API | |
| RX HW timestamp | `mbuf_hw_time_stamp()` = raw NIC RX ts, PHC-corrected (`mt_ptp.c:1623-1633`), with `MTL_FLAG_ENABLE_HW_TIMESTAMP` and NIC support. Otherwise `mtl_ptp_read_time()` at **processing** time, port P regardless of RX port (`mt_ptp.c:1635-1641`) | `timestamp_first_pkt`, timing parser | SW fallback includes RX burst and poll latency |

The "PTP"/"TAI" time function is selected as follows **[verified]**:

- Default: `ptp_from_real_time` = **`CLOCK_REALTIME` (UTC)** (`mt_dev.c:1597-1601`, `:2288-2289`).
- `MTL_FLAG_PTP_ENABLE`: switches to the PHC (`ptp_from_eth`) **once the master is initialized** (`mt_ptp.c:1062-1067`). Until then it is the UTC default.
- `ptp_get_time_fn`: app callback, invoked on **every** internal read (per frame, and per packet in PTP pacing) (`mt_dev.c:1603-1608`).
- `MTL_FLAG_PTP_SOURCE_TSC`: TSC plus a `CLOCK_REALTIME` base captured at init (`mt_dev.c:1610-1614`, `:2596-2597`).
- `doc/design.md` §5.4.3 documents the 37 s UTC/TAI issue only for user fns.

### 2.2 TX inputs per layer and media

| Media / layer | Field(s) | Pre-filled by lib | Units / `tfmt` | Who consumes it |
|---|---|---|---|---|
| ST20 session | `st20_tx_frame_meta.{tfmt,timestamp}` set by the app **inside the `get_next_frame` callback** | `tfmt = TAI`, `timestamp = tai(cur_epochs+1)`, `epoch = cur_epochs+1` (`st_tx_video_session.c:802-820`) | TAI ns or MEDIA_CLK (32-bit ticks in a u64) | USER_PACING: TAI only (MEDIA_CLK → err log + stat + default pacing, `:1787-1791`). USER_TIMESTAMP: both |
| ST20P / ST22P | `st_frame.{tfmt,timestamp}` at `put_frame` | — | same | Forwarded to the session only if USER_PACING or USER_TIMESTAMP (`st20_pipeline_tx.c:231-234`) |
| ST20 RTP level | RTP header `tmstamp` in the app's mbuf | — | 90 kHz ticks | Kept only with USER_TIMESTAMP, otherwise overwritten (`st_tx_video_session.c:1410-1427`) |
| ST30 session | `st30_tx_frame_meta.{tfmt,timestamp}` | `tai(packet epoch cur+1)` (`st_tx_audio_session.c:401-414`) | TAI or MEDIA_CLK (48/96/44.1 kHz) | Applies to **packet 0 of the buffer only** (§5) |
| ST30P | `st30_frame.{tfmt,timestamp}` | — | same | Forwarded **only with `ST30P_TX_FLAG_USER_PACING`**. ST30P has **no** USER_TIMESTAMP flag (`include/st30_pipeline_api.h:33-61`, `st30_pipeline_tx.c:197-201`, `:282`) |
| ST40 session | `st40_tx_frame_meta.{tfmt,timestamp,second_field}` | `tai(cur+1)` | TAI or MEDIA_CLK | USER_PACING (TAI only), EXACT, USER_TIMESTAMP |
| ST40P | `st40_frame_info.{tfmt,timestamp}` | — | same | Forwarded **only with `ST40P_TX_FLAG_USER_PACING`** (`st40_pipeline_tx.c:199-202`), although `ST40P_TX_FLAG_USER_TIMESTAMP` is mapped (`:308-309`) |
| ST41 session | `st41_tx_frame_meta.{tfmt,timestamp}` | `tai(cur+1)` | | See §9 F13/F14 |
| all sessions | `ops.rtp_timestamp_delta_us` (`int32`, µs) | — | µs, applied in TAI domain | RTP only, never TX |

### 2.3 TX outputs (`notify_frame_done` / pipeline done)

| Field | Meaning after TX | Evidence |
|---|---|---|
| `timestamp`, `tfmt = TAI` | The TAI instant the RTP was derived from (user ts + delta, epoch time, or scheduled start), **overwriting the app's input** | `st_tx_video_session.c:1981-1987`, `st_tx_audio_session.c:812-819`, `st_tx_ancillary_session.c:1003-1007` |
| `rtp_timestamp` | RTP ticks on the wire. For ST30: the RTP of the **last** packet built (the per-packet loop overwrites it) | `st_tx_audio_session.c:819`, test `st30_tx/pacing_test.cpp:159-161` |
| `epoch` | Chosen epoch index: frames (ST20/ST40, fields if interlaced) or **packets** (ST30) | `st_tx_video_session.c:1988`, `st_tx_audio_session.c:227` |
| **When "done" fires** | ST20 chain mode: when the NIC driver frees the last extbuf mbuf (lazy TX completion). No-chain mode: right after the frame is **built**, before the wire (`st_tx_video_session.c:2129-2134`). ST30: when the last packet is **built** (`st_tx_audio_session.c:916-935`) | "done" ≠ "on the wire" **[verified]**, jitter **[inferred]** |

No API exposes the **scheduled TX time** of packet 0 (`ptp_time_cursor`), except by coincidence in default mode where `timestamp` equals it.

### 2.4 RX outputs

| Field | Value | Evidence |
|---|---|---|
| `st20_rx_frame_meta.timestamp` / `rtp_timestamp`, `tfmt = MEDIA_CLK` | Raw 32-bit RTP | `st_rx_video_session.c:867-892` |
| `timestamp_first_pkt` | TAI ns of the packet that **created the slot**, i.e. the first to arrive with that RTP (any port, not necessarily packet 0 by sequence) | `st_rx_video_session.c:1293-1294` |
| `timestamp_last_pkt` | `mtl_ptp_read_time()` at notify time (SW, cached) | `:876` |
| `fpt` | `timestamp_first_pkt` − its own epoch start (always computed, `double`) | `:871-875` |
| pipeline `st_frame.receive_timestamp` | = `timestamp_first_pkt` (ST20P, ST30P, ST40P) | `st20_pipeline_rx.c:239-240`, `st30_pipeline_rx.c:116`, `st40_pipeline_rx.c:134` |
| ST30 RX | `timestamp` = RTP of the first packet that **opened** the RX buffer. RX buffer boundaries are set by whichever packet opens it and are unrelated to TX buffer boundaries | `st_rx_audio_session.c:283-288`, `:480-500` |
| timing parser (`*_TIMING_PARSER_META`) | `fpt`, `latency = first_pkt_time − RTP time`, `rtp_offset`, `rtp_ts_delta`, `vrx`, `cinst`, `ipt` | `st_rx_timing_parser.c:13-69` |

`st_frame.timestamp` therefore means "requested TAI or media clock" on TX input, "TAI basis of RTP" on TX done, and "raw RTP ticks" on RX. The meaning is carried only by `tfmt`.

### 2.5 Missing, zero, late, early, far-future user timestamps

| Case | ST20 USER_PACING | ST20 EXACT | ST30 USER_PACING | ST40 USER/EXACT | ST41 |
|---|---|---|---|---|---|
| `timestamp == 0` | default pacing, silent (`st_tx_video_session.c:1779-1785`) | default pacing + stat + **err log per frame** | default pacing, silent (`st_tx_audio_session.c:248`) | as ST20 | default |
| MEDIA_CLK | default + stat + err log per frame | same | same (`:250-255`) | same | **converted zero-based** (`st_tx_fastmetadata_session.c:252`, §9 F13) |
| in the past | snapped epoch used; start passed → **sent immediately**; stat only if the epoch is behind now (`:645-652`, `:731-735`) | **silent fallback to default pacing** + stat (`:1796-1805`) | ASAP, `stat_epoch_mismatch`, `notify_frame_late` (`:314-321`) | USER: as ST20. EXACT: fallback (`st_tx_ancillary_session.c:321-327`) | epoch mismatch, ASAP |
| < warm-up lead (RL) | n/a | fallback | n/a | not checked | — |
| > now + 1 s | stat only, **honoured**; the transmitter sees `delta > 1 s`, logs `err`, **sends immediately** (`st_video_transmitter.c:444-461`, `:311-336`) **[inferred effect]** | fallback | **clamped to now + 1 s** (`:304-309`) | USER: like ST20 (builder err + send, `st_tx_ancillary_session.c:1016-1029`) | — |
| two frames → same epoch | both get it: identical RTP, second frame back-to-back (no onward check in the user branch, `:644-656`) **[inferred]** | n/a (exact) | n/a | same as ST20 | — |

---

## 3. How TX picks the epoch and what "late" means

### 3.1 Video timeline (default pacing)

```text
  epoch N boundary = N*T_FRAME (T = field period if interlaced)
  |<------------------------------- T_FRAME ------------------------------------>|
  |                                                                              |
  N*T        start_N = N*T + TR_OFFSET - VRX*TRS                          (N+1)*T
  |------------------|====== ~total_pkts * TRS (active) ======|--- idle ---------|
                     ^ packet 0 target (tsc_time_cursor)
  RTP (default)   = round_tick(start_N)            <- not N*T
  RTP (EPOCH flag)= round(90k * N*T)
  builder decides N at get_next_frame(), roughly ring_count(<=512 pkts)*TRS before the previous frame ends
```

`calc_frame_count_since_epoch()` (`st_tx_video_session.c:637-690`), with `now_epoch = floor(now / T)` and `next = cur_epochs + 1`:

| Condition at pickup | Epoch chosen | Counters / callbacks | What happens on the wire |
|---|---|---|---|
| user `required_tai` (USER_PACING) | `round(required / T)` (half up) | `stat_error_user_timestamp` if the epoch is behind now's epoch or > 1 s ahead | See §2.5 |
| `now_epoch <= next`, onward ≤ 1 s | `next` | — | If `start_next < now` → **sent at once** (`time_to_tx_ns = 0`, `:731-732`), no counter |
| onward > 1 s (`max_onward_epochs`) | `now_epoch` | `stat_epoch_onward += onward` | resync |
| `now_epoch > next` ("late") | `now_epoch` | `stat_epoch_drop += now_epoch − next`, `notify_frame_late(skipped)` | RTP jumps. The frame goes out in the current epoch, immediately if its start has passed |
| interlaced, parity(epoch) ≠ `second_field`, not EXACT | `epoch + 1` | — | Field waits one field slot (`:709-713`, commit 7058bed0) |

Consequences:

- The "late" test uses the **epoch boundary**. A frame picked anywhere in `(start_N, (N+1)·T)` is late on the wire by up to ~one frame period, counted nowhere. `stat_epoch_mismatch` is never incremented for video, and `stat_epoch_troffset_mismatch` is declared, logged and never incremented **[verified]** (grep: no writers).
  KB §5 "TX: `stat_epoch_mismatch`, `stat_frame_late`" names a counter video never bumps and one that does not exist.
- In TSC mode the late frame's packets all have past targets, so they leave at line rate until caught up **[inferred]**. In RL mode the shaper keeps the spacing and the whole frame is simply shifted **[inferred]**.
- `stat_exceed_frame_time` counts frames whose **build** ended after packet 0's target (`:2135-2142`). That is a builder-side symptom, not wire lateness.
- The contract's "Normal = earliest regular slot that can still be scheduled" would pick N+2 in the silent-late case. The code prefers RTP continuity over ST 2110-21 conformance.

### 3.2 End-to-end lateness table per media

| Media / layer | Decision point | "Late" means | Action | App sees |
|---|---|---|---|---|
| ST20/ST22 session, default | builder pickup, once per frame | `now_epoch > cur+1` | skip epochs, send in the current one | `stat_epoch_drop`, `notify_frame_late(epochs_skipped)` |
| ST20 USER_PACING | pickup | snapped epoch < now's epoch | send ASAP | `stat_error_user_timestamp` |
| ST20 EXACT | pickup | `req < now`, `> now+1s`, `< warm-up lead` | **default pacing instead** | `stat_error_user_timestamp` |
| ST20P/ST22P `DROP_WHEN_LATE` (**needs USER_PACING, else silently off**) | `next_frame` (pickup) | `now ≥ ts + T_FRAME` | drop the frame, slot back to FREE | `notify_frame_done(status=DROPPED)`, `notify_frame_late(0)`, `stat_frames_dropped` (`st20_pipeline_tx.c:115-179`) |
| ST30 session, default | **every packet** | `now > slot(next)`: ≤ 10 ms → keep continuity (`stat_epoch_late`), > 10 ms → jump (`stat_epoch_drop`) | send ASAP | `stat_epoch_mismatch`, `notify_frame_late(late_ns/pkt_time)` **per packet** (`st_tx_audio_session.c:287-326`) |
| ST30 USER_PACING | packet 0 of the buffer (then chained) | `req < now` | ASAP; > 1 s clamped | as above |
| ST30P `DROP_WHEN_LATE` (+USER_PACING) | pickup | `now ≥ ts + buffer period` | drop | as ST20P (`st30_pipeline_tx.c:101-150`) |
| ST40 session | per unit | like ST20, plus `start < now` → `stat_epoch_mismatch` | ASAP | `stat_epoch_drop`, `notify_frame_late` |
| ST40P `DROP_WHEN_LATE` (+USER_PACING) | pickup | `now ≥ ts + T` | drop | as ST20P |
| ST41 | per unit | start passed | ASAP | `stat_epoch_mismatch` only, no callback |

`notify_frame_late(priv, uint64_t epoch_skipped)` therefore carries **three different units**: epochs skipped (ST20/ST40), packet-times late per packet (ST30), and a constant `0` (pipeline drop).

---

## 4. RTP timestamp derivation

All paths compute ticks from an **absolute** TAI value and truncate to 32 bits, so there is **no cumulative drift across frames** in any default path **[verified]**. The helper `st10_tai_to_media_clk()` rounds **to nearest, ties down**
(`lib/src/st2110/st_fmt.c:943-954`, `:986-993`), and unit tests pin that rounding (`tests/unit/session/st20_tx/rtp_timestamp_rounding_test.cpp`). ST 2110-10 §7.6.1 and the contract say **floor**.

| Media / mode | RTP = | Evidence |
|---|---|---|
| ST20 default | `round(90k · round_tick(N·T + TR_OFFSET − VRX·TRS))` | `st_tx_video_session.c:715-730`, `:790-797` |
| ST20 `RTP_TIMESTAMP_EPOCH` | `round(90k · nextafterl(N·T))` | `:790-792`, `:51-59` |
| ST20 USER_TIMESTAMP | `round(90k · (user_tai + delta))`. MEDIA_CLK is unwrapped against `ptp_time_cursor` then re-quantized (identity + delta) | `:769-785` |
| ST20 EXACT without USER_TIMESTAMP | `round(90k · required_tai)`, so **RTP moves with TX** | `:715-716`, `:793` |
| ST20 USER_PACING without USER_TIMESTAMP | snapped TX start. The app's timestamp is lost for RTP | as default |
| ST20 RTP level | app header if USER_TIMESTAMP, else as default | `:1410-1427` |
| ST30 default | `round(fs · nextafterl(E·T_pkt))`. Exact `E·spp` for integer-sample ptimes | `st_tx_audio_session.c:231-234`, `:376-397` |
| ST30 USER_PACING / USER_TIMESTAMP | `round(fs · (ts_pkt0 + n·T_pkt))`, see §5 | `:334-354` (**dead**, overwritten at `:395`) |
| ST40 default | `round(90k · round_tick(N·T))` (no TR_OFFSET) | `st_tx_ancillary_session.c:416-428`, `:451-468` |
| ST41 | `trunc(E · frame_time_sampling)` in `double` (a third rounding rule) | `st_tx_fastmetadata_session.c:230-237`, `:317-318` |

Simulation (C, replicating the HEAD formulas over 2×10⁶ frames at 2026 TAI; `vrx = 4`, 4320 packets, 1080-line TR_OFFSET) **[inferred]**:

| Rate | Default ST20: RTP − floor(N·T·90k) | Cadence | EPOCH flag vs ST10 floor |
|---|---|---|---|
| 59.94 | +56..+57 ticks | 1501/1502 alternating, exact | **differs by +1 on every second frame** (round half up vs floor) |
| 60 | +56 | 1500 | equal |
| 50 | +67 | 1800 | equal |
| 23.976 | +140..+141 | 3753/3754 (1:3) | differs on 50% of frames |

Other RTP observations:

- Interlaced: RTP is per field, and the field period is the epoch. At 1080i59.94 the EPOCH flag gives a second-field offset of +1502 where the contract (separately floored half frame) gives +1501 **[inferred from the same rounding]**.
- `frame_time` is a `double` ns value. Its error times ~10¹¹ frames at 2026 TAI is ~10² ns of absolute epoch error **[inferred]**. That is harmless for ticks except at exact tie points, which is why `nextafterl` exists (`st_tx_video_session.c:51-58`).
- `rtp_timestamp_delta_us` is `int32` µs, cast to `uint64` and added. A negative value works by modular wrap (`:766`). It is session-static and only µs-granular.
- Design doc §8.2 describes an RL "latency compensation" of RTP. In code the only such term is the `−VRX·TRS` in the start time (`:63-70`), and RTP follows the start time. The doc text is muddled **[inferred]**.

---

## 5. Audio: ptime, packets and buffers

- `samples_per_pkt` comes from `(ptime, sampling)` (`st_fmt.c:1093-1127`). `T_pkt` is a `double` ns value, non-integer for 333 µs, 1.09 ms, 0.14 ms and 0.09 ms.
- The buffer is split into packets as `st30_total_pkts = framebuff_size / pkt_len`. **Epoch = packet index since TAI 0** (`st_tx_audio_session.c:223-229`). Buffers carry no epoch of their own.
- The builder calls `sync_pacing` **per packet** (`calculate_time_cursor = true` after every packet, `:900`):
  - packet 0: `timestamp` = the buffer's `meta.timestamp`;
  - packet n > 0: `timestamp = ptp_time_cursor` (`:803-809`).
- **USER_PACING**: each buffer **re-anchors** to its own timestamp. Packets inside it chain at `ts + n·T_pkt`, and the cursor is truncated to `uint64` each step, so drift is ≤ 1 ns per packet, reset per buffer **[inferred]**. The contract's rule "later buffers must continue the first request's grid or be rejected" is **not** today's behaviour.
- **USER_TIMESTAMP without USER_PACING**: packet 0 carries the user RTP. Packets ≥ 1 use the cursor left by the *previous* packet's natural-cadence sync, so **their RTP lags their TX slot by one packet**. If the user timestamp equals the natural slot, packets 0 and 1 carry the same RTP **[inferred]**.
  The code and the test pin this as "falls back to natural cadence" (`tests/unit/session/st30_tx/pacing_test.cpp:185-223`; `include/st30_api.h:51-69` documents it).
- Default mode: continuous packet grid anchored at TAI 0. There is a 10 ms "late but keep continuity" window and a 1 s onward window (`:213-214`). The builder runs at most `fifo_size` (default 10 ms of packets) ahead (`:1930-1935`).
- RX does not preserve TX buffer boundaries (§2.4). An app cannot tell which TX buffer an RX buffer came from.
- At fractional video rates one video frame is 800.8 samples (48 kHz, 59.94). Fixed-size ST30/ST30P buffers therefore **cannot be "one video frame of audio"**. An app that wants per-video-frame audio submissions has to repack.

---

## 6. A/V sync today

ST 2110 carries sync in **RTP relative to a common PTP epoch**, not in wire time. A receiver aligns essences by converting RTP back to TAI. So the question is whether MTL lets an app put exact media times into RTP for every essence.

| Recipe | ST20 RTP | ST30 RTP | ST40 RTP | Residual A/V error in RTP |
|---|---|---|---|---|
| No flags | TX start = N·Tv + TRO − VRX·TRS | packet-grid time | N·Tv | The first video epoch vs the first audio packet depends on when each tasklet first pulled a buffer: up to 1 video frame, plus the TRO offset on video |
| USER_PACING on all (TAI) | snapped epoch start (±Tv/2 + TRO) | ≈ requested (exact TX) | snapped epoch (±Tv/2) | **up to Tv/2 + TRO** (the maintainer's "half a video frame") |
| USER_TIMESTAMP on all | exact user ts | exact user ts (packet 0; §5 caveat without USER_PACING) | exact user ts | ≤ 1 tick of rounding. TX still on each session's own grid |
| USER_TIMESTAMP + USER_PACING (+EXACT) | exact | exact | exact | exact RTP. TX exact under EXACT, snapped otherwise |
| Pipelines | ST20P: OK | **ST30P: USER_TIMESTAMP does not exist**, and USER_PACING gives RTP = request | **ST40P: USER_TIMESTAMP silently ignored without USER_PACING** | pipeline-dependent |

`tests/unit/session/multi_essence_sync_test.cpp` proves only that ST20/ST30/ST40 **sessions** map the same USER_TIMESTAMP input to the same TAI/RTP. Nothing covers pipelines, wire timing or cross-essence TX alignment **[verified]**.

What is missing for "automatic" multi-essence sync:

1. **No common media anchor object.** Each session anchors to TAI 0 plus its own first pickup. There is no "group T0" that video frame k (T0 + k·Tv) and audio sample s (T0 + s/fs) share.
2. **No separation of media time (RTP) and TX time.** Apps must set `timestamp = media time` and live with TX snapping, or hack a TX offset through `timestamp = media + L` with `rtp_timestamp_delta_us = −L` (µs, static).
3. **Default RTP semantics differ by essence** (video = TX time, ANC and audio = grid time).
4. **No sample-accurate audio start.** Buffers are fixed and per-packet, and a video grid start T0 = N·1001/60000 s is a fractional audio tick. Someone must define floor or round for the first sample.
5. **RX**: the app gets raw 32-bit RTP plus `receive_timestamp`. It must unwrap with `st10_media_clk_to_tai(receive_timestamp, rtp, rate)` itself. There is no presentation-time field or playout deadline, and no cross-session RX alignment.

---

## 7. Latency and buffering

### 7.1 TX queueing stages (video pipeline)

```text
app put_frame --> [framebuffs, FIFO by seq; framebuff_cnt]
              --> converter/plugin (optional) --> CONVERTED
              --> builder get_next_frame()          <-- EPOCH DECIDED HERE
              --> rte_ring (<=512 pkts ~ 1.9 ms @1080p60)
              --> transmitter (TSC/RL/TSN wait)
              --> NIC TX queue (nb_tx_desc, default 512; design.md 4.3.2)
              --> RL shaper --> wire
```

- Frames leave the CONVERTED queue one per epoch, so queue latency ≈ (frames waiting) × T. The misnamed `tx_st20p_newest_available()` actually returns the **oldest** (lowest seq) frame, i.e. FIFO (`st20_pipeline_tx.c:59-73`) **[verified]**.
- The builder pulls frame k+1 only when frame k's last bulk is in the ring. So "evaluation time" for k+1 is ~ring depth before k ends. That is internal state the app cannot observe; the contract's `tx_queue_available_time` is exactly this.
- Onward limit is 1 s (`max_onward_epochs`). There is no API to query "expected TX time if I put now" or queue depth.
- Audio: builder up to `fifo_size` (default 10 ms) ahead. The shared audio transmitter ring adds unpaced queueing after the TSC release **[inferred]**.

### 7.2 What an app can measure today

TX:

- `notify_frame_done` gives `timestamp` (TAI basis of RTP) and `epoch`, but done-time semantics vary (§2.3).
- `stat_max_next_frame_us`, `stat_max_notify_frame_us`, `stat_exceed_frame_time`, `stat_epoch_*`.
- The `ST_EVENT_VSYNC` event gives `{epoch, ptp, frame_time}` at each epoch (`include/st_api.h:196-203`).

RX:

- `receive_timestamp` (packet 0 arrival, approximately) minus the RTP→TAI of the frame gives the network plus sender offset. For MTL default video senders this includes the ~0.6 ms TRO offset.
- The frame is delivered on completion (last packet), so ≈ TRO + active time after the RTP instant.
- A frame whose tail was lost is delivered or recycled only when a **newer RTP timestamp evicts its slot**. That is the next frame's first packet with 1 slot, and later with 2 slots (redundant/RTCP) (KB §6 "Slot Assignment") **[inferred]**. There is no timeout flush.
- ST30 RX delivers when the buffer is full, or when a packet beyond it arrives (`st_rx_audio_session.c:491-519`).
- The timing parser `latency` field (`st_rx_timing_parser.c:36`) is the only built-in "wire vs RTP" metric, and it is video-only.

---

## 8. Assessment of `doc/user-pacing-timestamp-contract.md`

### 8.1 Solid

- **The central rule** "RTP describes content, pacing chooses TX, RX = TX + path" (lines 44-52). It is exactly what fixes §6, and today's code violates it in three places: default video RTP = TX time, EXACT moves RTP, and USER_PACING without USER_TIMESTAMP drops the media time.
- **No silent fallback, explicit rejection reasons** (lines 152-164, 417-422). Today every invalid case is counted-and-continued (§2.5).
- **Exact rational arithmetic from the original anchor, round once, no accumulated rounding** (lines 445-454). Current code already avoids cumulative drift but mixes three rounding rules (§4).
- **Per-packet oracle, extended sequence, "do not stop at packet 0"** (lines 314-322). Also the oracle must not infer anchors from the output (lines 301-303, 522-524).
- **Second interlaced field floored separately** (lines 514-520). This catches the +1502/+1501 difference.
- **State transitions**: rejected = nothing consumed. Accepted-then-dropped = RTP position consumed with a visible gap. Sequence numbers only for sent packets (lines 426-443).
- **The measurement boundary** (SFD, HW timestamps, path calibration) and "advertised profile per NIC × pacing combination" (lines 16-17, 239-257, 615-748) are honest about what a SW pacer can prove.

### 8.2 Questionable

| Point | Contract lines | Concern |
|---|---|---|
| Floor for RTP | 452, 502-505 | Correct per ST 2110-10 §7.6.1, but contradicts the round-to-nearest pinned by unit tests (`rtp_timestamp_rounding_test.cpp`). Changes every second 59.94 frame by 1 tick. Must be a deliberate migration |
| User pacing = "nearest slot, may be before request" | 107-113, 384 | For playout this allows TX **before** the requested time, which surprises users who read "timestamp" as "not before". Code snaps relative to the **epoch boundary** N·T, the contract relative to the **slot** (N·T + offset). They differ by TR_OFFSET |
| ST30 "later user buffer must equal the computed grid point or be rejected" | 601-602 | Too strict for live capture with its own audio clock or jittery app timestamps: a 1 ns mismatch rejects. Needs a tolerance, an explicit "continue grid, ignore ts" mode, or a re-anchor (discontinuity) event. Also contradicts today's per-buffer re-anchoring |
| 1 s horizon | 396-398 | Arbitrary. It limits deep pre-roll playout and it is cheap to make configurable. Today it is only a counter for USER_PACING |
| "Request evaluation time" | 357-371 | The implementation evaluates at builder pickup, asynchronously after `put`. The contract needs a public **asynchronous accept/reject channel** (event with reason plus the buffer returned), not "API or instrumentation" (line 414) |
| P10 ±10 µs | 247-257, 677-701 | Achievable on RL/TSN. TSC with 10–100 µs poll jitter cannot meet it. The profile must be per pacing way. BE, and shared-queue-forced TSC, need an explicit "no timing claim" profile |
| `receive_timestamp` = packet 0 by expected sequence | 222-231 | Workable for ST20 (SRD row 0/offset 0) but **undefined for ST30**, where RX units are not TX units (§2.4). For redundancy it needs the per-path value (line 674 mentions it) |
| ST40 location-derived planner (Appendix D) | 750-865 | Correct model, but MTL has no line-location scheduling today and spreads packets over T (`st_tx_ancillary_session.c:1108`). A large new feature disguised as a test contract |
| "RTP clock offset" / `rtp_anchor_ticks` as a free input | 215-216, 497 | ST 2110-10 requires zero offset to the PTP epoch for RTP-from-media-clock. The contract should say whether non-zero anchors are allowed at all, beyond `rtp_timestamp_delta_us` |

### 8.3 Missing (needed for a new API)

1. **Clock domain**: what "TAI" means when PTP is not locked (today UTC `CLOCK_REALTIME`). Behaviour on PTP lock, steps and holdover. Whether a clock step forces the "explicit stream reset". The UTC→PHC switch at first sync.
2. **API shape for two timestamps**: media time (→ RTP) versus requested TX time (→ pacing), their defaults, and how they relate (e.g. TX = media + latency, snapped to the essence grid).
3. **Multi-essence grouping**: a shared anchor T0 and sample/frame counters, so ST20/ST30/ST40 derive RTP from one timeline automatically.
4. **Pipeline layer**: conversion latency, queue depth, DROP_WHEN_LATE semantics (today `ts + T` grace versus EXACT's `< now`), and what "done" means (built, handed to NIC, or on wire).
5. **RX**: delivery deadline and timeouts, a presentation-time field (unwrapped TAI of RTP), incomplete-frame flush latency, per-path timestamps.
6. **ST22, ST41, and RTP-level** user pacing (feature request in triage 1722).
7. **Redundancy**: whether both ports share one TX target, and the allowed P/R skew.
8. **Observability**: reporting the chosen packet-0 target and the lateness per unit synchronously, instead of `stat_*` counters.

---

## 9. Flaws found (with evidence)

| # | Flaw | Evidence | Label |
|---|---|---|---|
| F1 | Default time source is UTC `CLOCK_REALTIME`, but it is labelled TAI everywhere | `mt_dev.c:2288-2289`, `:1597-1601`; `mtl_api.h:662-667` | verified |
| F2 | Built-in PTP switches the time function from UTC to PHC when the master is first seen, a ~37 s step for running sessions | `mt_ptp.c:1062-1067` | verified (step size inferred) |
| F3 | EXACT user pacing with an invalid timestamp **silently falls back to default pacing** | `st_tx_video_session.c:1796-1805`; `st_tx_ancillary_session.c:321-327` | verified |
| F4 | USER_PACING far-future (> 1 s) is honoured by the builder, then the transmitter logs `err` and sends at once (video TSC/RL/PTP, ANC builder) | `st_video_transmitter.c:444-461`, `:311-336`, `:630-647`; `st_tx_ancillary_session.c:1016-1029` | verified code / inferred effect |
| F5 | Video "late" detection ignores frames picked after their own start but before the next epoch: sent late, no counter. `stat_epoch_troffset_mismatch` has no writer, and video never writes `stat_epoch_mismatch` | `st_tx_video_session.c:672-690`, `:731-735`; grep | verified |
| F6 | USER_PACING (non-exact) has no onward/duplicate check: two frames can take the same epoch, giving duplicate RTP and back-to-back TX | `st_tx_video_session.c:644-656` | inferred |
| F7 | Default ST20 RTP ≠ default ST40 RTP for the same frame (TRO offset) | `st_tx_video_session.c:715-730` vs `st_tx_ancillary_session.c:416-428` | verified / simulated |
| F8 | EXACT without USER_TIMESTAMP puts the TX time into RTP (RTP moves with TX) | `st_tx_video_session.c:715-716`, `:793` | verified |
| F9 | ST30 USER_TIMESTAMP without USER_PACING: packets ≥ 1 carry the previous packet's slot time (1-packet RTP lag, possible duplicate) | `st_tx_audio_session.c:803-819`, `:385-392`; test `st30_tx/pacing_test.cpp:185-223` | inferred |
| F10 | ST30 sync_pacing computes `rtp_time_stamp` (incl. delta) and it is overwritten immediately (dead code) | `st_tx_audio_session.c:334-354` vs `:395` | verified |
| F11 | ST30P has no USER_TIMESTAMP. ST40P USER_TIMESTAMP is silently ignored unless USER_PACING is set | `st30_pipeline_api.h:33-61`; `st40_pipeline_tx.c:199-202`, `:308-309` | verified |
| F12 | `*_DROP_WHEN_LATE` is silently inert without `*_USER_PACING`. The doc says "when mtl reports late frames", which does not match the implementation | `st20_pipeline_tx.c:124-126`; `st_pipeline_api.h:461-466` | verified |
| F13 | ST41 USER_PACING with MEDIA_CLK uses zero-based `st10_media_clk_to_ns` (a 1970 time) instead of unwrap | `st_tx_fastmetadata_session.c:252` | verified |
| F14 | ST41 USER_TIMESTAMP checks `frame->ta_meta.tfmt` (the audio union member) instead of `tf_meta.tfmt`. The read lands in `tf_meta.epoch`'s low word, so the flag practically never applies. TAI user timestamps are always ignored | `st_tx_fastmetadata_session.c:769-772`; `st_header.h:154-161`; `st41_api.h:134-147`, `st30_api.h:278-300` | verified layout / inferred runtime |
| F15 | `st10_get_tai(MEDIA_CLK, …)` returns zero-based ns, not "ns since TAI epoch" as documented | `include/st_api.h:588-606` | verified |
| F16 | Three rounding rules for RTP (round-half-down helper plus `nextafterl`, double truncation in ST41, ST10 floor in doc and contract) | `st_fmt.c:943-993`; `st_tx_fastmetadata_session.c:230-237` | verified |
| F17 | `notify_frame_late` argument has three units (epochs, packet-times, 0), and fires per **packet** for ST30 | §3.2 | verified |
| F18 | `epoch` means frame, field, or **packet** index depending on media | `st_tx_audio_session.c:223-229` | verified |
| F19 | `st_frame.timestamp` is overwritten on TX done. Same field, three meanings (§2.4) | `st20_pipeline_tx.c:260-263` | verified |
| F20 | `notify_frame_done` fires at build time (no-chain ST20, ST30) or at lazy NIC free (chain), never "on wire" | `st_tx_video_session.c:2129-2134`, `:116-134`; `st_tx_audio_session.c:916-935` | verified / inferred jitter |
| F21 | Zero used as sentinel for "no timestamp", "unset epoch", "no RX timestamp" and "no target" | `st_tx_video_session.c:654`, `:1779`; `st_rx_timing_parser.c:20`; `st_tx_audio_session.c:992` | verified |
| F22 | Silent or downgraded pacing: RL training fail → TSC, no log; ST22 → TSC; shared txq overrides explicit RL/TSN (info only); ST30 RL with ptime ≥ 2 ms → TSC; runtime queue RL fail flips the **port** while the session keeps `RL` | `st_tx_video_session.c:535-542`, `:3432-3434`; `mt_dev.c:1460-1464`, `:1650-1655`; `st_tx_audio_session.c:2086` | verified (last effect inferred) |
| F23 | `mt_pacing_train_bps_result_search(impl, i, …)` passes the **session port index** where the physical port is expected (compare `:4270`) | `st_tx_video_session.c:2758` | verified |
| F24 | `err()` logged per frame in the tasklet for MEDIA_CLK/zero-ts misuse (log flood on the data plane) | `st_tx_video_session.c:1782`, `:1789`; `st_tx_audio_session.c:251`; `st_tx_ancillary_session.c:308`, `:315` | verified |
| F25 | ST40 multi-packet units are spaced `T/total_pkts` apart, unrelated to the LLTM/CTM windows | `st_tx_ancillary_session.c:1108` | verified |
| F26 | ST20P `newest_available()` returns the oldest frame (name inverted, behaviour FIFO) | `st20_pipeline_tx.c:59-73` | verified |
| F27 | `mtl_ptp_read_time()` cache is updated non-atomically from any caller thread | `mt_main.c:1110-1122` | inferred |
| F28 | RX `timestamp_first_pkt` is the first arrival (any port, any packet), SW fallback is processing time on port P, and an incomplete frame is flushed only by eviction | `st_rx_video_session.c:1293`; `mt_ptp.c:1635-1641` | verified / inferred |
| F29 | Docs drift: KB §5 names `stat_frame_late` (does not exist) and `stat_epoch_mismatch` for video (never written). `st_api.h:350` describes `stat_epoch_drop` as "epoch mismatch events". design.md §8.2's RL compensation text does not match the code | KB §5; `include/st_api.h:350-359` | verified |

---

## 10. Requirements distilled for the new API

1. Two per-unit times with separate validity (no zero sentinel): **media time** (drives RTP; TAI ns or rational media position) and **requested TX time** (drives pacing; optional).
2. A per-session **pacing policy enum** (`GRID`, `NEAREST_SLOT`, `NOT_BEFORE`, `EXACT`), with the rejection policy explicit (`REJECT`, `DROP`, `SEND_LATE`) and never silent.
3. A **synchronous or evented admission result** per unit: accepted packet-0 target, or rejected with a reason. The buffer comes back with a status.
4. **RTP from media time only**, a single documented rounding rule (floor per ST10), and the same rule for all essences. Default media time = the essence grid (N·T), never the TX time.
5. **Sync group / media anchor**: a shared T0 plus frame/sample counters for ST20/ST30/ST40, and a group latency budget L (TX = media + L, snapped per essence).
6. Sample-accurate audio submission (variable sample count, or a per-buffer sample index) so an audio buffer can match 800.8-sample video frames.
7. An explicit clock contract: time-source type exposed, lock state queryable, a step event, and a reset semantics on step.
8. The pacing way requested **per session**, the effective way reported back, and no silent downgrade. An advertised accuracy profile per effective way.
9. "Done" statuses that distinguish built, queued to NIC and (optionally, with TX HW timestamp) on wire. Lateness reported per unit with one unit of measure.
10. RX: presentation TAI (unwrapped RTP), first- and last-packet times with the path ID, and a flush deadline for incomplete units.

---

## Open questions for the maintainer

1. **Default video RTP: TX time or media grid?** Today default ST20 RTP = packet-0 TX time (≈ +0.6 ms at 1080p), while ST40/ST30 use grid time. That breaks ST20/ST40 RTP equality. Options: (a) make N·T_FRAME the default (today's `RTP_TIMESTAMP_EPOCH`) and drop the flag; (b) keep TX-time RTP as an opt-in "measured" mode; (c) keep as is.
   (a) matches ST 2110-10 §7.6.3 and the contract but changes the wire for every existing user.
2. **Rounding rule: floor or nearest?** The contract and ST10 say floor. Code and tests use round-half-down plus `nextafterl`, so 59.94/23.976 differ by one tick on half the frames. Options: switch everything to floor in the new API only, or everywhere. Interop impact is small but it is a visible wire change.
3. **Two timestamps: shape and defaults.** Options: (a) `media_time` + optional `tx_time` per unit; (b) `media_time` per unit + a session-level `tx_latency` (TX = media + L snapped to the grid); (c) both. (b) makes the A/V case trivial, while (a) serves exact scheduling. What should happen when only `tx_time` is given (RTP = grid, or RTP = tx_time)?
4. **Automatic multi-essence sync: library object or app convention?** A "sync group" with a shared T0 and latency L would remove the half-frame error without the app computing per-essence grids.
   Should the library own it (group create, add sessions, start at the aligned T0), or should the API only provide helpers (`anchor → media_time(frame k)`)? A library-owned group implies coordinated start across schedulers.
5. **Late/invalid policy per unit.** Today: counted and continued, with EXACT silently falling back. The contract says: reject. Options per session: `REJECT` (return the buffer with a status), `DROP` (consume the RTP slot, leave a gap), `SEND_LATE` (keep continuity). Which is the default for live, and which for playout?
6. **User-pacing snap semantics.** "Nearest slot" (can be before the request; contract) versus "first slot not before" versus "nearest epoch boundary" (code, off by TR_OFFSET). Which one should `NEAREST` mean, and do we need both?
7. **Audio grid continuity.** Should later ST30 buffers re-anchor to their own timestamps (today), continue the first grid ignoring timestamps, or be rejected on mismatch (contract)? Live sound-card capture drifts against PTP, so a strict rule forces resets. Is a tolerance or an explicit discontinuity flag acceptable?
8. **Clock contract.** Should MTL refuse to start timed sessions without TAI (PTP locked or user fn declared TAI), or keep UTC `CLOCK_REALTIME` as a labelled "free-running" source? How should a PTP step (UTC→PHC at first sync, or GM change) be surfaced: stream reset event, silent re-epoch (today), or session error?
9. **Where is the admission decision made and reported?** Today it happens at builder pickup, asynchronously after `put`. Should the new API decide at `put` (needs the queue model exposed), or report asynchronously with the buffer returned? The contract's determinism needs one of the two.
10. **Pacing way per session and silent downgrades.** Should sessions request a pacing way and fail if it is unavailable (breaking for AUTO users on shared queues/ST22), or keep port-level selection but report the effective way plus an accuracy profile? What happens to a port-wide RL → TSC flip that affects other sessions?
11. **Meaning of "frame done".** Built, handed to NIC, or on wire (needs TX HW timestamps; TSN/E830 could provide it)? Latency measurement and safe external-buffer reuse depend on it.
12. **ST40 timing model scope.** Is LLTM/CTM-conformant per-packet ANC scheduling (contract Appendix D) in scope for the redesign, or should ST40 stay "single slot per unit" with the model as a later feature? Today's spreading over T is non-conformant for multi-packet units.
13. **1 s horizon and pre-roll.** Keep a fixed 1 s maximum lead (contract, today's counters), make it configurable, or derive it from framebuffer depth? Deep-buffer playout needs more than 1 s.
14. **RX presentation contract.** Should RX units carry an unwrapped TAI media time plus per-path first/last arrival times and a flush deadline for incomplete frames (today eviction only)? This decides whether apps can do A/V sync and latency measurement without re-implementing ST10 unwrap logic.
