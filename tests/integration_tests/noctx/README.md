# NoCtx integration tests

`NoCtxTest.*` is the part of `KahawaiTest` where each test case owns its own
`mtl_init()` / `mtl_uninit()`. Use it when a test needs `mtl_init()` flags, a
PTP clock callback or a port layout that the shared integration-test context
cannot give it: user pacing, simulated packet loss, a fake PTP clock, NIC RX
timestamps, queue counts.

Library background: `.github/copilot-docs/mtl-knowledge-base.md` §5 (pacing,
epochs, RL warm-up), §7 (iavf RX timestamp workaround), §8 (testing). To add a
test, see `.github/skills/mtl-write-test/SKILL.md`.

## Running

With `--no_ctx_tests`, `tests.cpp` skips the global `mtl_init()`, and each case
sets its own flags and calls `mtl_init()` itself. DPDK EAL cannot be initialised
twice in one process, so **run one case per process**: a filter that matches two
NoCtx cases fails the second with `eal not support re-init`. The runners
enforce this.

| Runner | Selects | Ports | Pause |
|---|---|---|---|
| `run.sh` | `NoCtxTest.${NOCTX_FILTER}*`, excluding `*_pf_*` | `TEST_PORT_1..4`, all required | 20 s |
| `run_pf.sh` | `NoCtxTest.${NOCTX_FILTER}*_pf_*` | `TEST_PF_PORT_1..2`, PFs bound to `vfio-pci` | 10 s |
| MCP `run_noctx_tests` / `run_noctx_pf_tests` (`mtl-system-setup`) | `gtest_filter` / the `_pf_` cases; no `isolate.sh`, so `st30p_user_pacing` and `st20p_redundant_1080p59_s8_equal_vrx_margin` FAIL | 4 ports / 2 PFs, auto-discovered if not given | `cooldown_seconds`, default 10 s |

```bash
sudo -E tests/integration_tests/noctx/run.sh                      # every non-PF case
sudo -E NOCTX_FILTER=st20p_ tests/integration_tests/noctx/run.sh  # NoCtxTest.st20p_*
sudo -E tests/integration_tests/noctx/run_pf.sh                   # _pf_ cases only
```

The runners read the port variables from the environment or from `noctx.env`
next to the scripts (untracked, sourced but not exported). They list the cases
with `--gtest_list_tests`, run each `KahawaiTest --auto_start_stop
--no_ctx_tests` and write `${TMP_FOLDER:-/tmp}/noctx_<n>.xml`
(`noctx_pf_<n>.xml` for `run_pf.sh`). `run.sh` wraps only the cases in
`NOCTX_ISOLATE_CASES` (comma-separated, default: the seven strict tests listed
under [Requirements](#requirements-and-skipfail-policy)) in `tests/tools/isolate/isolate.sh`; the others, and every `run_pf.sh` case, run directly.
`EXIT_ON_FAILURE=0` continues past a failure and exits 1 at the end if any
case failed; `BUILD_PATH` overrides the binary, which defaults to
`.local_install/mtl/bin/KahawaiTest` if `.local_install` exists, else
`build/tests/KahawaiTest`. One case by hand:

```bash
sudo ./build/tests/KahawaiTest --auto_start_stop --no_ctx_tests \
  --port_list=0000:xx:xx.x,0000:yy:yy.y --gtest_filter=NoCtxTest.st20p_user_pacing
```

Code layout: `core/` holds the `NoCtxTest` fixture, the fake PTP clock and the
`FrameTestStrategy` base; `handlers/` creates the TX/RX pipeline sessions and
runs the frame threads; `strategies/` holds the per-frame hooks
(`txTestFrameModifier()` stamps a TX frame, `rxTestFrameModifier()` is the RX
oracle, reporting through `EXPECT_*`); `testcases/` holds the `TEST_F` cases.

## Requirements and SKIP/FAIL policy

A single-port session sends on `TEST_PORT_1` and receives on `TEST_PORT_2`;
redundant tests map sessions to ports by index.

| Tests | Requirement | If missing |
|---|---|---|
| Strict: `st20p_default_timestamps`, `st20p_user_pacing`, `st20p_user_pacing_offset_jitter`, `st20p_exact_user_pacing`, `st20p_user_pacing_interlaced`, `st30p_user_pacing`, and the next row | TX, RX on **different physical ports**; RX-port **PHC** read to ±1 µs; NIC RX timestamp on frame 0 | SKIP (topology), FAIL with `NOCTX_REQUIRE_STRICT=1`. SW RX time after frame 0: FAIL |
| `st20p_redundant_1080p59_s8_equal_vrx_margin` | 4 ports; strict topology on `TEST_PORT_1` -> `_2` (leg P) and `_3` -> `_4` (leg R); RX ports on one PHC; NIC RX timestamps ([oracle](#equal-vrx-margin-oracle)); ≥ 50 Gb/s links (e.g. 100 GbE E810/E830): the TX PF link carries 16 streams, ~42.5 Gb/s, so a 25 GbE pair fails | < 4 ports: FAIL. Topology: as the strict row. A software RX time: FAIL |
| `st30p_user_pacing`, `st20p_redundant_1080p59_s8_equal_vrx_margin` | An exclusive CPU partition: its own cgroup's `cpuset.cpus.partition` reads `isolated`. Checked after the strict topology, which may SKIP or FAIL the case first | FAIL, never SKIP |
| `st20p_redundant_latency_drops_even_odd`, `st30p_redundant_latency*` | 4 ports | The case errors (`std::runtime_error`) |
| `st20p_redundant_latency_drops_even_odd` | `MTL_SIMULATE_PACKET_DROPS` build (`./build.sh debug` / `debugonly`) | SKIP |
| `*_pf_*` (TSN launch-time pacing) | E830 PF ports (`run_pf.sh`); the device ID is not checked | Fails or hangs elsewhere |

`strictPacingTopologyError()` resolves both BDFs to their PF through sysfs
`physfn`. The RX PF must stay bound to its kernel driver so that
`ETHTOOL_GET_TS_INFO` finds its `/dev/ptpN` (a VF uses its PF's PHC; a
`vfio-pci` PF as RX always skips, or FAILs with `NOCTX_REQUIRE_STRICT=1`).
VFs of one PF are switched inside the NIC and get no RX timestamp, so on a host
with two cabled ports interleave the VFs: `TEST_PORT_1=<PF0 VF0>`,
`TEST_PORT_2=<PF1 VF0>`, `TEST_PORT_3=<PF0 VF1>`, `TEST_PORT_4=<PF1 VF1>`.

**CI.** `bind-test-ports` gives the chosen PF six VFs and the other port of the
same card two, when its link is up (`doc/ci_runner_setup.md`). For the NoCtx run
`.github/scripts/gtest.sh` interleaves them as above, for example
`15:01.0,15:11.0,15:01.1,15:11.1` on an E830 with PFs `15:00.0` and `15:00.1`.
Without VFs on the other port it passes the four VFs of one PF that the other
suites use. It sets `NOCTX_REQUIRE_STRICT=1`, so there the seven strict tests fail
instead of skipping, and `MTL_ISOLATE=require`, so they also fail without an
exclusive CPU partition.

**Policy.** A SKIP means the host cannot measure wire timing, never that pacing
passed; the message gives the reason. No NoCtx test skips because of CPU
isolation: `st30p_user_pacing` and `st20p_redundant_1080p59_s8_equal_vrx_margin`
FAIL without an exclusive partition, and the other strict tests run at any
isolation level and may fail their timing checks.

`initStrictPacingContext()` adds `MTL_FLAG_ENABLE_HW_TIMESTAMP` to the default
context, as NIC RX timestamps are the pacing oracle. It also clears
`MTL_FLAG_CNI_TASKLET` and sets `MTL_FLAG_RX_SEPARATE_VIDEO_LCORE`: the TX
tasklet launches each frame in software, so the CNI and RX video tasklets stay
off its scheduler.

## CPU isolation

`run.sh` runs each case in `NOCTX_ISOLATE_CASES` under
`tests/tools/isolate/isolate.sh` with `MTL_ISOLATE=try` (override it from the
environment) and `MTL_ISOLATE_PORTS` = `TEST_PORT_1..4`. The default cases are
the seven strict tests. The wrapper, its requirements and limits:
[tests/tools/isolate/README.md](../../tools/isolate/README.md).

## Timing model

`T` is the frame period (the field period for interlaced, where `fps` is the
field rate). `TR_offset` is the ST 2110-21 offset to the first active line,
`trs` the packet spacing, and `VRX` the packets sent ahead of `TR_offset`
(narrow VRX minus 4 for RL, minus `bulk − 1` for TSC). The tests read the live
values with `st20p_tx_get_pacing_params()` rather than hardcoding them. At
1080p25 BPM with RL: 4115 packets, `trs` ≈ 9.33 µs, `TR_offset` ≈ 1.529 ms,
`VRX` = 4.

- **Epoch grid.** The fake PTP clock is `CLOCK_MONOTONIC_RAW − start`, so TAI
  starts near 0 and epoch `k` is `k·T`. ST20 packet 0 of epoch `k` is at
  `k·T + TR_offset − VRX·trs`, later packets `+i·trs`.
- **RL warm-up.** With the hardware rate limiter, the transmitter queues pad
  packets (KB §5 Warm-Up Padding) from `warm_pkts·trs` before the target, then
  holds packet 0 until the target, so a late TX tasklet launches it late. A
  frame late within its epoch goes out late in it. A target already
  more than two `trs` late when the warm-up begins gets no pads
  (`stat_trans_troffset_mismatch`).
- **Pacing modes.** Default: the current epoch (ST40: the epoch itself; ST30:
  the next packet-time grid point).
  `USER_PACING`: the **nearest epoch** to `t_user` (ties to the later one),
  plus `TR_offset − VRX·trs` for ST20; a past or >1 s ahead request is still
  used and counts `stat_error_user_timestamp`; ST30 sends packet 0 of each
  buffer at `t_user`. `EXACT_USER_PACING` (ST20, ST40): packet 0 at `t_user`
  itself; an invalid request (zero, `MEDIA_CLK`, past, >1 s ahead, or for ST20
  closer than the warm-up) falls back to default pacing and counts
  `stat_error_user_timestamp`.
- **RTP clocks.** RTP is `st10_tai_to_media_clk()` of the packet-0 TX instant
  (the request itself in exact mode): 90 kHz for ST20/ST40, the sample rate
  (48 kHz) for ST30. Steps: 3600 per 1080p25 frame, 1800 per 1080i50 field,
  1500 at 60p, alternating 1501/1502 at 59.94p, 480 per 10 ms audio buffer.
  RTP is always compared by integer equality.
- **PHC → `CLOCK_MONOTONIC_RAW`.** `receive_timestamp` is the NIC timestamp
  of the packet that opened the RX slot, in the PHC domain. `RxPhcClock`
  reads the PHC once per frame and converts
  `rx_mono_raw = receive_timestamp − (phc_now − mono_raw_now)`. E830 gives
  `mono_raw_now` exactly (`PTP_SYS_OFFSET_PRECISE`). E810 has no
  cross-timestamp, so it is read with `PTP_SYS_OFFSET_EXTENDED` and
  `mono_raw_now` is known to ±u ≤ 1 µs; every check below must hold at both
  ends of ±u, and a PHC that cannot be read to ±1 µs fails. `phc_now` lags
  `receive_timestamp` by about a frame, so a PHC rate more than 5 ppm off
  `CLOCK_MONOTONIC_RAW` fails; each run prints it (`NoCtx RX PHC: rate …`).
- **Strict ST20p: launch −1/+10 µs, elapsed from frame 0 ±10 µs.** Packet 0
  of every frame `n` is checked twice. Launch: its RX time, mapped on to the
  fake PTP clock, against its planned launch,
  `−1 µs ≤ rx(n) − expected_tx(n) ≤ +10 µs`; it cannot arrive early, so 1 µs
  is PHC read error, and 10 µs covers path latency and launch jitter. Elapsed:
  `|(rx(n) − rx(0)) − (expected_tx(n) − expected_tx(0))| ≤ 10 µs` for `n ≥ 1`,
  where the clock origin, path delay, PHY latency and timestamp-point offset
  cancel but drift does not. 10 µs is just above one `trs` at 1080p25.
- **PHC-following PTP clock.** The library's RX timing parser places the
  epochs of `vrx_min` on the raw NIC timestamps, so a test that uses it as the
  oracle needs MTL's PTP time on the RX PHC. `PhcFollowingClock` makes it
  `PHC + K`: `now()` is `CLOCK_MONOTONIC_RAW + offset`, the offset read as in
  **PHC → `CLOCK_MONOTONIC_RAW`** above, once before `mtl_init()` and then
  every 10 ms by a thread started before it and pinned to the test's starting
  CPU, EAL's main lcore in the partition, which runs no scheduler. Each read
  sets the offset's slope so that it reaches the new reading 10 ms later, at
  most 100 ppm; each update can move `now()` by about 10–20 ns, either way.
  `now()` costs a vDSO clock read, as `FakePtpClockNow`, but a seqlock makes
  it spin while the follower is mid-update, so a follower preempted there
  stalls every caller, TX tasklets included. Each run prints
  `NoCtx PHC follower: …`.
- **NIC RX timestamp proof.** With the PHC in MTL's PTP domain, the parser's
  software fallback (the PTP time at processing) would look the same. So K is
  a whole number of frames, 60 at 59.94p (1.001 s): the TX epochs keep their
  phase on the PHC, and so do `vrx_min`, `fpt`, `cinst` and `ipt`. Only the
  RTP-based `rtp_offset` and `latency` shift, so the parser reports every
  frame as not compliant, which the test does not read. With NIC timestamps a
  frame's PTP now − `receive_timestamp` is K plus its delivery latency, and
  each leg's `rtp_offset` is shifted by K·90 kHz = 90090 ticks; without,
  neither is.
- **Partial 2022-7 legs.** The timing parser stops parsing a frame once it is
  complete, so the leg whose last packet arrives second misses its tail, and
  `vrx_min` over fewer packets can only be higher. A separate check bounds
  that tail; lateness confined to the last 4 or fewer packets of a leg is not
  timed.
- **ST30p user pacing: absolute ±40 µs.** The RX time is mapped on to the fake
  PTP clock and compared directly with `t_user`: `|rx − t_user| ≤ 40 µs`
  (80 µs for buffer 0). Path delay and conversion error count against it.

## Test catalogue

Default run length is 20 s, stopping at the first failure. ST20p is 1080p25
YUV 4:2:2 10-bit BPM, 3 buffers; ST30p is PCM16 48 kHz stereo, 1 ms packets,
10 ms buffers; ST40p is 60p. Every strict ST20p frame must also be `COMPLETE`
with the BPM packet count (`ceil(frame_bytes / 1260)`) and `timestamp ==
rtp_timestamp`; the user-paced tests request `t_user(n) = start + n·T`, with
`start` 800 ms ahead rounded up to `T`.

| Test | Proves | Oracle | Tolerance |
|---|---|---|---|
| `st20p_default_timestamps` | Default epoch pacing: one frame per epoch, no skipped slot, no drift | TX at `n·T` from frame 0; frame 0 RTP on the `k·T + TR_offset − VRX·trs` tick; RTP step exactly 3600 | −1/+10 µs launch, ±10 µs elapsed |
| `st20p_user_pacing` | An on-epoch request snaps to that epoch's packet-0 slot | TX `round(t_user/T)·T + TR_offset − VRX·trs`; `RTP == tick(TX)`; TX count == RX count | −1/+10 µs launch, ±10 µs elapsed |
| `st20p_user_pacing_offset_jitter` | Requests anywhere within ±T/2 snap to the epoch | As above, with `t_user` offsets `{0, .3, .1, −.49, .37, −.14, 0, .44}·T` | −1/+10 µs launch, ±10 µs elapsed |
| `st20p_exact_user_pacing` | Exact pacing launches packet 0 at the request, unsnapped | TX `t_user` (offsets −100 µs to +320 µs); `RTP == tick(t_user)`; no step check | −1/+10 µs launch, ±10 µs elapsed |
| `st20p_user_pacing_interlaced` | The user-pacing contract per field, 1080i50 | As `st20p_user_pacing` with `T` = 20 ms, step 1800, but a field whose nearest slot is off the ST 2110-21 frame grid (first field even, second odd) goes one slot later; `second_field` alternates from a first field | −1/+10 µs launch, ±10 µs elapsed |
| `st30p_user_pacing` | User-paced audio is stamped and sent at the request | `t_user(n) = 600 ms + n·10 ms` from PTP zero; `RTP == tick48k(t_user)`, step 480; TX count == RX count | ±40 µs absolute (80 µs buffer 0) |
| `st20p_redundant_1080p59_s8_equal_vrx_margin` | RL pacing does not depend on the TX scheduler a session lands on; not first-packet latency, which the rx_timing acceptance suite and the unit model cover | Equal VRX margins, see [below](#equal-vrx-margin-oracle) | 1 packet |
| `st30p_default_timestamps` | Default audio starts on the packet-time grid | Frame 0 RTP, as TAI, is a multiple of 1 ms and less than one buffer before its (software) RX time; step 480 | Exact |
| `st30p_redundant_latency`, `st30p_redundant_latency2` | ST 2022-7 audio merge with R 10 ms behind, and with P stopping after 10 s | `_latency`: packets on each port == TX ± 10 %. `_latency2`: packets on port 1 (R) == TX ± 10 %, port 0 > 0, accepted packets == TX ± 1 %. Both: loss ≤ 0.1 %, RX buffers == TX ± 1 % (100 RX buffers) | Counts |
| `st40p_user_pacing`, `st40p_user_pacing_59fps`, `st40p_user_pacing_offset_jitter` | ST40 user pacing snaps to the nearest epoch, at 60p and 59.94p (1501/1502 steps) | TX `round(t_user/T)·T`; `RTP == tick90k(TX)`; step from the planned grid | Software RX time in [0, +1 ms] |
| `st40p_exact_user_pacing` | Exact ST40 pacing sends at the request | TX `t_user`; `RTP == tick(t_user)`; no step check | Software RX time in [0, +1 ms] |
| Other NoCtx tests | ST20 2022-7 even/odd loss recovery, epoch recovery after a PTP step, TSN packet spread and epoch recovery (`_pf_`), ST20p TX multithread stability (default 1800 s), ST40 interlace/split/auto-detect, 32–128 and asymmetric queue init | Stats, counts, frame content | See each file |

### Equal VRX margin oracle

`st20p_redundant_1080p59_s8_equal_vrx_margin` runs 8 ST 2022-7 1080p59.94
sessions, all TX created first, so one TX scheduler holds ≥ 3 sessions and
≥ 2 schedulers are in use (else FAIL). It skips the start-up underflows of the
first 20 s after each session's first frame, measures for 30 s, NIC timed (see
[Timing model](#timing-model)), and checks:

- per port, each session's `vrx_min` mean less its lowest 1 % ≥ the best
  session's − 1 packet, over ≥ 80 % of the window's frames per session and port;
- ≤ 1 % of those frames with that leg finishing > 4 packets behind the other;
- median PTP now − `receive_timestamp` in (K, K + 2T), no leg's `rtp_offset`
  unshifted by K, and ≤ 1 % + 61 frames per leg (about 1 s, as the RTP wrap
  every ~13.25 h is not handled) with it off the K shift by ≥ T/2;
- the PHC follower: no read lagging by more than 5 µs, no gap of more than
  100 ms between reads, and ≥ 90 % of the 10 ms slots read.

The 1-packet tolerance is measured: with the RL pre-arm plan and the
redundant-leg rate fix, the best-minus-worst trimmed-mean `vrx_min` spread per
port was 0.15–0.26 packets over 3 runs (E830 VF TX/RX, and E810 RX); without
them it was 1.14–1.29 packets, the sessions on the busy TX scheduler failing
the margin check. Without them leg R also finished more than 4 packets behind
on 1799/1799 frames, as the redundant leg looked up the wrong trained rate.

## Failure signatures

| Symptom | Likely cause |
|---|---|
| Elapsed error jumps by ~2^32 ns (≈4.29 s), or "NIC RX timestamp moved backwards" | The iavf vector RX path dropped a descriptor timestamp and corrupted the 32→64-bit extension. Check the `nb_rx_desc` workaround in `dev_config_port()` is active (KB §7) |
| `TX_VIDEO_SESSION(...): transmitter mismatch troffset N` next to a late frame | The tasklet stalled over the RL warm-up window, so no pads were sent (`stat_trans_troffset_mismatch`). Look for a busy or shared lcore |
| `st30p_user_pacing` fails on a single late buffer | Kernel housekeeping or preemption of the TSC-paced audio scheduler. Check the case ran in an exclusive partition, and the work listed under [CPU isolation limits](../../tools/isolate/README.md#limits) that needs boot parameters |
| `st30p_user_pacing ... needs an exclusive CPU partition` | Not run under `tests/tools/isolate/isolate.sh` with an exclusive partition (check `NOCTX_ISOLATE_CASES`); see the wrapper's `WARNING` |
| `st30p_redundant_latency*` RX buffers short of TX; message shows `stat_slot_get_frame_fail` | Back-pressure: the RX consumer did not return buffers and the session refused packets |
| Error of about `T` from some frame on | A frame was lost or a slot skipped, so `idx_rx` no longer matches the TX index. Check `stat_epoch_drop` and RX loss |
| `median PTP now - receive_timestamp … is not in (K, +2T)`, or `frames timed with software RX times` | Below K: the RX port delivers no NIC RX timestamps and the parser used software times (`MTL_FLAG_ENABLE_HW_TIMESTAMP` is dropped silently when the port lacks the offload). Otherwise `PhcFollowingClock` lost the RX PHC |
| `MTL's PTP time did not follow the RX PHC` | Something (ptp4l, phc2sys) stepped the RX PF's PHC or steers it > 100 ppm off, the PHC reads failed or left gaps (E810 reads that miss ±1 µs), or the follower thread was starved of its CPU |
| `this leg finished more than 4 packets … behind the other` | TX paces the two 2022-7 legs of a session apart (for example one leg's queue on the wrong rate), the leg lost packets, or the RX tasklet stalled at the frame tail; the lagging leg's tail is not timed |
| `TX sessions per scheduler: … the check needs one scheduler with >= 3 sessions` | The scheduler quota or lcore count changed, so the 8 sessions no longer share TX schedulers and the VRX check would compare nothing |
| `dev_eal_init, eal not support re-init` | The filter matched more than one NoCtx case in one process |
| After a pass: `EAL: PANIC in eal_intr_thread_main(): Error adding fd N epoll_ctl, Bad file descriptor`, or SIGSEGV during `mt_dev_if_uinit` | The iavf close vs. interrupt callback race. Check the DPDK build carries [patch 0009](../../../patches/dpdk/26.07/0009-net-iavf-fix-interrupt-callback-race-on-close.md) |

For the TX statistics (`stat_epoch_drop`, `stat_epoch_onward`,
`stat_error_user_timestamp`) see `doc/stats_guide.md` and KB §5.
