# RL pre-arm: a stall before the pad burst delays packet 0

This analysis applies to the RL frame start of `video_trs_rl_pre_arm()`
([design.md 4.3.2](design.md#432-st2110-21-pacing)). It explains a single-leg frame
shift that a wire capture shows with 720p60 sessions, and the unit test
`St20TxRlWarmUpTest.LaunchStaysOnTargetWhenTheFirstPadBurstStalls` that pins it.

## 1. Fault

`video_trs_rl_pre_arm()` reads the TSC, plans the pad train from that time, and then
queues the pads (`lib/src/st2110/st_video_transmitter.c`):

```c
drained = target_tsc - gap - credit;            /* plan anchored at the gap read */
...
long double start = mt_get_tsc(impl) - credit;
if (drained < start) { ... drained = start; }   /* shaper idle: re-anchor */
...
tx = video_trs_burst_pad(impl, s, s_port, pads, n);  /* doorbell: pads reach the NIC */
```

The plan supposes that the NIC gets the pads at the time of the TSC read. If the
thread stops between the read and the doorbell (an interrupt, a softirq, a
preemption), the pads go into the NIC queue later than the plan. After
`video_trs_rl_pre_arm()` returns `true`, packet 0 goes into the NIC ring at once,
about 480 µs before its target at 720p60, behind the pads. Thus packet 0 leaves late by
the full stall.

- The `drained < start` check finds a stall only if it is longer than the pads
  already queued (32 pads, about 280 µs at 720p60). A shorter stall stays in the plan.
- MTL counts nothing: no refused pad, no `stat_trans_recalculate_warmup`, no
  `stat_trans_troffset_mismatch`.
- The frame-start tolerance of design.md 4.3.2 is correct for a scheduler that
  **reaches** the pre-arm late: the plan then starts from a later gap read. For a stall
  **inside** the pre-arm call, the tolerance is zero.
- Without a pad plan (`video_trs_rl_warm_up()` and `ST_TX_VIDEO_RL_STATE_WAIT_TARGET`,
  as on main), the transmitter keeps packet 0 until its target. A stall in the warm-up
  costs pads, not the time of packet 0.

## 2. Unit test

`tests/unit/session/st20_tx/rl_warm_up_test.cpp`,
`LaunchStaysOnTargetWhenTheFirstPadBurstStalls`. The harness knob
`ut_trs_set_burst_stall()` moves the mock clock forward inside one burst call, as an
interrupt before the doorbell does. The shaper model then takes the pads at the later
time. The test asks for the design contract: packet 0 leaves in
[target, target + one pad).

```bash
./build.sh unit
./build_unit/tests/unit/UnitTest --gtest_filter='St20TxRlWarmUpTest.LaunchStaysOnTargetWhenTheFirstPadBurstStalls'
```

Result with the current code (1080p59.94 shaper, one pad = 3.7 µs):

| Stall in the first pad burst | Packet 0 after target |
|---|---|
| 1 µs | in one pad (passes) |
| 10 µs | 12.2 µs |
| 50 µs | 52.2 µs |
| 150 µs | 119.0 µs (the `drained < start` re-anchor removes a part) |

The test fails until the pre-arm handles a stall before the doorbell.

## 3. Hardware evidence

Setup: one RxTxApp, 2 st20p TX sessions 720p60 (and one 1080p59.94 run), RL pacing on
one E830 VF (iavf), no RX process. The E810 port of a second host NIC captures the
headers with hardware timestamps through the switch; its PHC follows
`CLOCK_REALTIME`, the time MTL uses without PTP. USDT probes in the transmitter give,
per frame, the gap read, the pads queued, the planned launch delay ("slack"), and the
time packet 0 goes into the NIC ring. bpftrace also records each context switch of the
scheduler thread and each interrupt of 1 µs or more on its CPU. "Isolated" runs use
`tests/tools/isolate/isolate.sh` with `MTL_ISOLATE=require`. Each run is 120 s (the
not-isolated runs: 240 s).

A debug build busy-waits a set time in session 0, once each 10 frames, after the TSC
read and before the first pad burst. The main build waits at the same point of
`video_trs_rl_warm_up()`.

| Case | With pre-arm (this branch) | Without pre-arm (main) |
|---|---|---|
| 50 µs stall, isolated | packet 0 +49.4 µs | +0.0 µs |
| 150 µs stall, isolated | packet 0 +149.5 µs, all 718 stalled frames are isolated single-leg shifts | +0.2 µs, no shift |
| No stall, not isolated, 240 s | 33 isolated single-leg shifts of 52-272 µs. In 31 of them, the scheduler thread is switched out (a `kworker` 35-105 µs) or interrupted inside the pre-arm call | no isolated shift; 10 late frames, both sessions together, preempted at the target |
| No stall, isolated | packet 0 +7.4 µs (median) when the 1 ms tick is inside the pre-arm call, +0 µs when not | no effect |

Packet 0 on the wire minus its planned launch, session 0, isolated, no stall:

| Interrupt inside the pre-arm call | Frames | Median | Max |
|---|---|---|---|
| no | 5497 | 2.0 µs | 4.0 µs |
| yes | 1687 | 9.4 µs | 15.4 µs |

## 4. Why 720p60 shows it and 1080p59.94 does not

- The test host has a 1 ms tick (`CONFIG_HZ=1000`) on each CPU, also in an isolated
  partition without `nohz_full=`. A tick takes 2-10 µs.
- At 720p60 the frame grid repeats each 3 frames (50 ms, 50 ticks). On the test host,
  one of the 3 phases puts a tick at the start of the pre-arm call: the tick is in 1687
  of 2395 pre-arm calls of that phase, and in none of the other two phases.
- Without isolation, the tick also wakes a `kworker` that preempts the scheduler thread.
  That gives the large shifts.
- At 1080p59.94 the frame grid drifts against the tick (60 phases in 1001 ms). Only 1-2
  of 7177 pre-arm calls get an interrupt. The NoCtx case
  `st20p_redundant_1080p59_s8_equal_vrx_margin` uses this format.
- The phase of the tick against `CLOCK_REALTIME` is a property of the host and of the
  boot. The fault does not depend on it: a stall in the call at any rate and format moves
  packet 0.

A second, rarer effect has the same root: after a large shift, the late frame was still
in the NIC queue when the next pre-arm took the shaper as idle, and the next packet 0
was 104 µs late with no stall of its own.

## 5. Open items

- One of the 33 shifts without isolation has a 139 µs pre-arm call with no context
  switch and no traced interrupt. The SMI count did not change. The cause of that stall
  is not known.
- No hardware run used ST 2022-7. Each leg has its own pre-arm call, so a stall moves
  only one leg: the "single-leg" shift.

## 6. Directions for a fix

1. Read the TSC again after each pad burst, and plan the next burst from the later of
   `drained` and the new time minus `credit`. A stall before a doorbell then gives
   fewer pads. A stall in the last burst is not covered.
2. After the pad train, compare the time of the last doorbell with the plan. If it is
   later by more than one pad, keep packet 0 out of the ring and finish with the
   one-pad-at-a-time warm-up of `video_trs_rl_warm_up()`.
3. Before the pre-arm, check that the previous frame and its pads have left the shaper.
