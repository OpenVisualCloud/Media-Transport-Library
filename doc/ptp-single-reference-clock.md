# One reference clock for the acceptance test fleet

Design proposal for slaving every CI runner's system clock to a single PTP
grandmaster, so that the pytest acceptance suite, the gtest suite, and the
netsniff-ng packet capture all derive their timestamps from one reference.

Status: **proposal**. Nothing described here is deployed. It is written up because
the arrangement it replaces is correct today only by coincidence, and that
coincidence is not recorded anywhere a person configuring a new runner would look.
One prerequisite measurement has been taken on an idle E810 runner and is reported
in §4; it adjusted no clock and changed the recommended port.

Scope note: this describes a lab topology, not a library change. The only code it
touches is `tests/acceptance/conftest.py`, and it touches it by **deleting**
(§5). Host names and addresses are deliberately generalised — substitute your own.

---

## 1. Why any of this matters

EBU LIST's ST 2110-21 analysis produces a measurement called `packet_ts_vs_rtp_ts`,
with a compliance window of **[0, 1 ms]**. It is the difference between two
independently produced timestamps:

| Quantity | Produced by | Read from |
|---|---|---|
| The **RTP timestamp inside each packet** | MTL's transmitter, when it builds the packet | MTL's time source |
| The **pcap timestamp of that packet** | the capture NIC, as the packet arrives on the wire | the capture port's PTP hardware clock (PHC) |

Those are two different clocks in two different places. The measurement is only
meaningful if they sit on the same timescale. If they are one second apart, EBU
reports a one-second error and calls the stream non-compliant — even when the
on-wire pacing is perfect.

This is not hypothetical. In a nightly run that prompted this note,
`packet_ts_vs_rtp_ts` came back at **-73.014 s** while MTL's own PTP was locked to
**144 ns**. Right pacing, wrong yardstick: the grandmaster's PHC had been re-seeded
by a driver reload and was free-running 73.195 s away from UTC, and the capture
inherited that.

So the whole problem reduces to one requirement:

> **The capture PHC and MTL's time source must be the same clock, or agree to well
> under 1 ms.**

Everything below is about how to guarantee that structurally instead of by
convention.

## 2. How netsniff-ng timestamps packets

This is the part that is easy to get wrong, because netsniff-ng looks like an
ordinary userspace capture tool but is not timestamping in userspace.

`tests/acceptance/create_pcap_file/netsniff.py:186-198` builds the command line:

```text
sudo netsniff-ng --silent --in <capture iface> --out '<file>.pcap' -T 0xa1b23c4d ...
```

Two things matter:

* **netsniff-ng requests raw hardware RX timestamps by default.** The timestamp
  written for each packet is taken by the NIC as the frame arrives on the wire —
  not by the kernel when the packet is delivered, and not from `CLOCK_REALTIME`.
  This is deliberate and load-bearing: software timestamps are smeared by RX
  interrupt coalescing, which destroys the packet-spacing measurements
  (`2110_21_cinst`, VRX) that these tests exist to make.
* **`-T 0xa1b23c4d`** selects the nanosecond pcap magic, so those timestamps
  survive at full resolution. The usual microsecond magic (`0xa1b2c3d4`) would
  truncate them.

The consequence, and the answer to "how does the capture get synchronised":
**netsniff-ng itself is never synchronised, and cannot be.** It has no clock. It
copies whatever value the capture port's PHC hands it. Synchronising the capture
therefore means synchronising *that PHC* — a hardware clock on the NIC, steered by
a daemon on the host, with netsniff-ng as a passive consumer.

An empirical confirmation that these really are hardware timestamps: in the
diagnosis above, the measured offset tracked the **PHC**, not the host wall clock.
Had netsniff-ng been recording software timestamps, disciplining the PHC would have
had no observable effect at all.

### Which PHC

`_select_sniff_interface_name()` resolves the capture interface from
`capture_cfg.sniff_pci_device` (a `vendor:device` pair). On a two-port E810 this
lands on the second port.

**On an E810, both ports share a single PHC.** Verified on two runners with
different card slots: the two `ice` ports enumerate to the same `/dev/ptp*` index
in each case.

This single fact drives the whole design. Any daemon that steers the capture PHC is
steering the clock MTL uses for pacing, and vice versa. **That clock must have
exactly one writer at any moment.**

## 3. The timestamp chain as it stands today

There are two regimes, selected by `@pytest.mark.ptp` (`tests/acceptance/pytest.ini:16`).

### Regime A — the default, `enable_ptp=False` (almost every test)

```text
  upstream NTP
       |
       v
  host CLOCK_REALTIME  --(+ kernel TAI offset)-->  host CLOCK_TAI
       |                                                |
       | phc2sys -s CLOCK_REALTIME -O <tai_offset>      | RxTxApp / FFmpeg read
       v                                                v  CLOCK_TAI for RTP ts
  E810 shared PHC                                   RTP timestamps
       |
       v
  netsniff-ng hardware RX timestamps  -->  pcap  -->  EBU LIST
```

`_start_capture_phc_sync()` (`conftest.py:441`) starts
`phc2sys -s CLOCK_REALTIME -c <iface> -O <tai_utc_offset> -S 0.001 -m` and **blocks
until the offset is in tolerance** (`_PHC_SYNC_THRESHOLD_NS = 2000`, 30 s timeout).
The TAI-UTC offset is read live from the host (`_host_tai_utc_offset()`,
`conftest.py:409`) rather than assumed, because the transmitters read `CLOCK_TAI`
and that offset is host-wide state.

This regime is **self-consistent per host**: both ends of the comparison derive from
that host's own wall clock. It does not involve the grandmaster at all. This is why
non-PTP tests pass even when the grandmaster is wrong.

### Regime B — `@pytest.mark.ptp`, `enable_ptp=True`

Here MTL's internal PTP wants to own the capture PHC: `dev_start_timesync()`
(`lib/src/dev/mt_dev.c:848`) seeds it from the system clock via
`rte_eth_timesync_enable()`, and the PTP tasklet steers it toward the grandmaster
with `rte_eth_timesync_adjust_time()`.

```text
  grandmaster (fabric PHC)
       |
       | PTP over the media fabric
       v
  MTL internal PTP  -->  steers the E810 shared PHC
       |                                    |
       v                                    v
  RTP timestamps                     netsniff-ng hardware RX timestamps
                                            |
                                            v
                                     pcap --> EBU LIST
```

When MTL is the only writer, the capture reads the very clock MTL is steering, so
the pcap and the RTP timestamps agree *by construction* — whatever the grandmaster's
own absolute offset happens to be. `pcap_capture` (`conftest.py:1268`) relies on
this and skips the capture phc2sys for these tests.

**As the tree stands, MTL is not the only writer.** `ptp_sync` (`conftest.py:538`)
starts `ptp4l -i <capture_iface> -s -m -2` for exactly these tests, on exactly that
PHC. Two servos then apply the full correction independently: one steps the PHC to
the grandmaster, MTL re-seeds the same PHC from the system clock and applies its own
correction on top. A separate change removes the harness-started `ptp4l` and leaves
regime B with no host daemon at all on that PHC. **This proposal assumes that fix is
in place**; it is a prerequisite, not part of the design.

### Why today's arrangement is fragile

Once that conflict is removed, the two regimes produce the same numbers — but only
because of two independent accidents:

1. **The grandmaster is configured on UTC** (`phc2sys … -O 0`). This is not the
   ST 2110 convention; a spec-conformant grandmaster serves **TAI**. It works only
   because MTL ignores `currentUtcOffset` when computing its clock value: at
   `lib/src/mt_ptp.c:1498`, `spec.tv_sec -= ptp->master_utc_offset;` carries the
   comment `/* display with utc offset */`. The offset reaches the log line and
   nothing else, so `mt_get_ptp_time()` returns raw grandmaster time.
2. **The kernel TAI-UTC offset is 0 on every runner checked**, so
   `CLOCK_TAI == CLOCK_REALTIME == UTC`. That is true only because no leapsecond
   file is loaded.

Change either one silently — load a leapfile, or "correct" the grandmaster to
`-O 37` to match its own announced `utc_offset 37` — and regime B diverges from
regime A by 37 seconds, with the failure appearing as a compliance verdict rather
than an error. **That is the real motivation for this proposal:** not that the
current setup is broken, but that its correctness rests on undocumented
coincidence.

## 4. Proposed: one reference, structurally

Make every runner's `CLOCK_REALTIME` a PTP slave of one grandmaster. Then both
regimes, both suites, and the capture all descend from one clock, and agreement is a
property of the topology rather than a coincidence of configuration.

The obstacle is §2's single-writer rule: each runner has exactly one E810 whose two
ports share one PHC. There is no second fabric PHC to borrow, so whatever
disciplines the fabric PHC is disciplining the clock the capture reads and — in
regime B — the clock MTL steers.

### Which interface carries the sync

**Option A — fabric `ice` port, hardware timestamping, PHC as the master. Recommended.**

This is the textbook linuxptp deployment and the standard ST 2110 node model:
`ptp4l -s` slaves the NIC PHC to the grandmaster with hardware timestamps, and
`phc2sys` copies the PHC into `CLOCK_REALTIME`. The data flow is the **reverse** of
today's:

```text
today:     CLOCK_REALTIME --phc2sys--> E810 PHC        (system clock is the master)
option A:  E810 PHC --phc2sys--> CLOCK_REALTIME        (the PHC is the master)
```

Reversing it dissolves the two-writer conflict that otherwise looks inherent. There
is exactly one writer on the PHC — `ptp4l` — and phc2sys only *reads* it. And
because the capture port's PHC is now grandmaster-locked in its own right, the
capture is traceable to the reference directly rather than via the host clock.
Concretely this design:

* puts the capture on the grandmaster with **hardware timestamps end to end**,
  sub-microsecond, no software timestamping anywhere;
* **deletes** `_start_capture_phc_sync()` and `_host_tai_utc_offset()` from the
  critical path. The capture PHC no longer has to be chased onto `CLOCK_TAI` by a
  per-test daemon, and no TAI-offset arithmetic is needed, because the PHC is
  already on the reference timescale. That is a net *simplification* of the harness;
* satisfies the 5 µs bound in gtest `Misc.ptp`, making `--ptp` gtest runs viable for
  the first time (§8.3);
* is what a real ST 2110 node looks like, so the suite exercises a realistic
  configuration rather than a lab-only one.

**Its one cost is regime B.** MTL's internal PTP wants to steer the same PHC, so
`ptp4l` and MTL cannot both run. That affects two test functions, but they are the
only end-to-end coverage of `MTL_FLAG_PTP_ENABLE` — a real library feature, used by
deployments that do not run linuxptp. Dropping them is not acceptable.

The reconciliation is a **per-test handover**, which is structurally what the harness
already does: `ptp_sync` suppresses host PTP daemons for `@pytest.mark.ptp` tests and
hands the PHC to MTL. The only change is that it would stop a host *service*
(`systemctl stop ptp4l-slave phc2sys-slave`) rather than a test-started process, and
restart it in teardown. Two things make that acceptable:

* Regime B tests do not depend on `CLOCK_REALTIME` at all — MTL is on the
  grandmaster and the capture reads the PHC MTL steers — so the host clock
  free-running for the duration of one test costs nothing. With its learned
  frequency correction it drifts microseconds to low milliseconds over a few
  minutes.
* `systemctl` is *more* recoverable than the current `sudo pkill`, not less: systemd
  knows the unit is stopped, and a crashed run that never restores it is repaired by
  the next `systemctl start`, by a `Restart=` policy, or by a host health check.
  Today's leaked-daemon failure mode (see `_reap_ptp_daemons`, `conftest.py:381`) is
  strictly worse — a leaked root daemon on a shared PHC has caused an `ice`-driver
  use-after-free in `ptp_clock_index()`.

Port choice: put `ptp4l` on the **TX port**, not the capture port. Because both ports
share one PHC this makes no difference to what gets disciplined — `ptp4l` on either
port steers the clock the capture reads — but `ptp4l` and netsniff-ng **cannot share a
port**. See the measurement below.

**Option B — fabric `ice` port, software timestamping. Fallback if the handover is rejected.**

With `time_stamping software` (`ptp4l -S`) there is no PHC in the loop at all: the
local clock is the system clock and ptp4l adjusts it directly. phc2sys(8) states the
division explicitly — "In hardware time stamping mode, ptp4l announces use of PTP
time scale and PHC is used for the stamps… Time offset between these two is
maintained by phc2sys" — phc2sys exists to bridge PHC and system clock in *hardware*
mode; in software mode there is nothing to bridge. `ethtool -T` confirms the
capability on these NICs (`software-transmit`, `software-receive`,
`software-system-clock`).

This satisfies two things at once: PTP runs on the **media fabric**, over the same
switch path and from the same grandmaster the tests use, and **nothing contends for
the E810 PHC** — so regime B keeps working exactly as it does today, MTL owning that
clock outright. It is the only fabric option compatible with regime B unchanged.

Do not confuse this with the **capture's** timestamps, which stay hardware in every
option. Two independent mechanisms share the name:

| | What is timestamped | By what | Used for |
|---|---|---|---|
| Capture | every media packet, on the wire | E810 PHC, **hardware** | pcap → EBU `Cinst`, VRX, `packet_ts_vs_rtp_ts` |
| PTP sync | ptp4l's own Sync/Delay_Req | PHC (hw) or kernel (sw) | measuring the offset used to discipline a clock |

Option B changes only the second. netsniff-ng keeps taking hardware on-wire
timestamps at full nanosecond resolution.

**The sync error is common mode for every compliance measurement.** In regime A both
`packet_ts_vs_rtp_ts` operands descend from the same host clock — the capture PHC via
phc2sys, the RTP timestamps via `CLOCK_TAI` — so any offset `ptp4l -S` carries
against the grandmaster subtracts out. In regime B both descend from the
grandmaster. And `Cinst`/VRX depend only on relative packet spacing inside one
capture, i.e. on PHC *rate*, so they are indifferent to absolute offset entirely.
This is why tens of microseconds on the sync path costs nothing for any capture
test.

The exception is any *differential* check between the two timescales. There is
exactly one in the tree: gtest `Misc.ptp` compares `mtl_ptp_read_time()` against
`CLOCK_REALTIME` with a 5 µs limit, which software timestamping would not satisfy —
see §8.3.

Two practical notes. Software timestamping costs accuracy — tens of microseconds
rather than sub-microsecond — which is still one to two orders of magnitude inside
the 1 ms window that actually matters. And software RX timestamps are taken in the
kernel path, so they degrade under load: prefer the **TX port**, whose kernel RX
queue is quiet, over the capture port, which is saturated by netsniff-ng during a
2160p119 test. The servo's time constant is long relative to a test, so a few tens
of seconds of noisy samples should not move the system clock meaningfully — but this
is the one thing to measure before committing (§7). The port-sharing measurement
below points the same way for an independent reason, so under every option `ptp4l`
belongs on the TX port.

Note also that in software mode ptp4l uses the **UTC** time scale rather than
announcing PTP time scale, which lines up with a grandmaster already on UTC (§6.2).

**Option C — management NIC, hardware timestamping. Fallback.**

Each host also has a second, kernel-managed NIC with its own PHC and hardware
timestamping, on the management network, that MTL never touches and that is not
bound to DPDK. Using it buys sub-microsecond hardware timestamping on a link that is
quiet during tests, at the cost of being off the fabric under test, needing a second
grandmaster instance on the grandmaster host's management port, and putting PTP
multicast on a corporate segment (§6.1). Keep it in reserve for hosts where fabric
software timestamping proves too jittery, where the `ice` PF cannot be used, or for a
host that must satisfy `Misc.ptp` without giving up the regime-B handover.

**Summary.**

| | Interface | Timestamps | Capture traceable to GM | Regime B | Verdict |
|---|---|---|---|---|---|
| **A** | `ice` (fabric) | hardware, sub-µs | directly, PHC is GM-locked | per-test handover | **Recommended** |
| **B** | `ice` (fabric) | software, tens of µs | via the host clock | unchanged, no handover | Fallback |
| **C** | management | hardware, sub-µs | via the host clock | unchanged, no handover | Fallback |

All three give one reference with no clock having two writers. A is the only one
that is hardware end to end and the only one that simplifies the harness; its price
is the per-test handover. B and C avoid the handover at the cost of an extra
conversion stage between the grandmaster and the capture.

### Measured: ptp4l and netsniff-ng cannot share a port

Measured on an idle E810 runner with `ptp4l 4.0`, using `free_running 1` so no clock
was adjusted — the daemon only reports the offset it measures:

```text
[global]
clientOnly 1
free_running 1
time_stamping hardware
```

**The fabric itself is healthy.** The grandmaster was discovered and selected from
both `ice` ports, and path delay was 470–605 ns with no visible asymmetry — a single
clean switch hop. PTP over the media fabric is viable; nothing here argues for
option C.

**But hardware PTP and the capture cannot share a port.** With `ptp4l` on the capture
port, starting netsniff-ng on that same port knocked it out within ~100 ms, twice, in
separate runs:

```text
ptp4l[...]: timed out while polling for tx timestamp
ptp4l[...]: increasing tx_timestamp_timeout may correct this issue, but it is
            likely caused by a driver bug
ptp4l[...]: port 1 (ice1): send delay request failed
ptp4l[...]: UNCALIBRATED to FAULTY on FAULT_DETECTED (FT_UNSPECIFIED)
```

Recovery took 16 s (`fault_reset_interval`), during which the PHC is undisciplined.
With `ptp4l` on the **TX port** and netsniff-ng on the capture port, zero faults, and
the capture was unaffected — so the fix is to separate them, which costs nothing
because both ports share the PHC being disciplined.

**The cause is not a driver bug, despite what the message guesses.** Sampling the
interface's timestamp configuration across the transition shows netsniff-ng switching
TX timestamping off underneath ptp4l:

```text
t=10  ptp4l alone        : tx_type 1  rx_filter 1
t=11  netsniff-ng starts : tx_type 0  rx_filter 1   <- clobbered
      ptp4l FAULTY ~100 ms later
t=40  both gone          : tx_type 1  rx_filter 1   <- ptp4l re-set it on recovery
```

`SIOCSHWTSTAMP` configures timestamping per **interface**, not per socket. netsniff-ng
needs only RX stamps, so it sets `tx_type = HWTSTAMP_TX_OFF` and turns off exactly
what `ptp4l` needs for its own Delay_Req. Last writer wins. `ptp4l` is then polling
for a timestamp the NIC will never produce, which is why the remedy the message
suggests does not work: `tx_timestamp_timeout` at 1 ms, 10 ms and 100 ms all fault
identically, with one timeout each. A longer timeout cannot help when the answer is
never coming.

Two consequences worth recording. First, no driver or firmware update will fix this —
it is shared per-netdev state, so **any** process that configures hardware
timestamping on the capture port will do the same thing to `ptp4l`. Second, this is
why today's arrangement has never hit it: `phc2sys` disciplines a PHC through
`clock_adjtime` on `/dev/ptp*` and never calls `SIOCSHWTSTAMP` at all, so the
existing per-test capture `phc2sys` coexists with netsniff-ng without contest. The
constraint is specific to daemons that use *socket* timestamping, which means
`ptp4l`.

Note also what did *not* break: the RX filter, which was the risk anticipated here.
`ethtool -T` offers only `none` and `all`, both parties want `all`, and the filter
stayed at `all` throughout. The earlier reasoning that "hardware stamps are taken at
the MAC regardless of kernel congestion" is true but was beside the point — the
failure is in configuration ownership, not in timestamp accuracy under load.

**An incidental finding that supports the whole proposal.** The runner's capture PHC,
with nothing disciplining it, sat **2.1–2.9 ms away from the grandmaster** — outside
the [0, 1 ms] compliance window on its own. That is the state a capture inherits
whenever no daemon has run, and it is what today's per-test `phc2sys` is quietly
papering over.

Relative frequency was not stable across the session: about −79 ppb early, then
+2.4 to +2.9 ppm decaying over the following minutes. A decaying frequency offset is
the signature of a servo converging *somewhere*, and since nothing was adjusting the
runner's clock, the likely explanation is the grandmaster PHC itself being slewed by
its own `phc2sys` chasing a slewing `CLOCK_REALTIME`. Measurements taken only at the
runner cannot separate runner drift from grandmaster slew, so this needs a
simultaneous reading at the grandmaster before any drift figure is quoted.

Still outstanding: the same measurement **under load**. It requires a 2160p119 case
running, which means a CI dispatch or a built tree outside the runner's `_work`
directory. The coexistence result above is the prerequisite for doing it safely — it
is now known that a measuring `ptp4l` on the TX port will not disturb a live job's
capture.

### Clock ownership under the proposal

| Clock | Single writer | Source |
|---|---|---|
| grandmaster host `CLOCK_REALTIME` | ntpsec | upstream NTP |
| grandmaster host fabric PHC | `phc2sys -s CLOCK_REALTIME -c <fabric iface>` | grandmaster host `CLOCK_REALTIME` |
| runner E810 shared PHC | `ptp4l -s -i <tx iface>` (hardware) — **or** MTL, during a regime-B test | grandmaster, over the media fabric |
| runner `CLOCK_REALTIME` | `phc2sys -s <tx iface> -c CLOCK_REALTIME` (reads the PHC, never writes it) | runner E810 PHC |

Under the option B fallback the last two rows become
`runner CLOCK_REALTIME ← ptp4l -S -s -i <tx iface>` with no PHC in the loop, and the
E810 PHC keeps today's arrangement (per-test capture phc2sys in regime A, MTL in
regime B). Under option C they become `runner mgmt PHC ← ptp4l -s`,
`runner CLOCK_REALTIME ← phc2sys -c CLOCK_REALTIME`, plus a second phc2sys instance
on the grandmaster host's management PHC.

### The capture chain afterwards

Regime A — the capture PHC is grandmaster-locked in its own right, and the host
clock is derived *from* it. No per-test phc2sys, no TAI-offset arithmetic:

```text
  grandmaster host CLOCK_REALTIME  (the one reference)
       |
       v
   grandmaster  --PTP, hardware ts, media fabric-->  ptp4l -s on TX port
                                                          |
                                                 E810 shared PHC
                                                     /        \
                                  phc2sys -s        /          \  netsniff-ng
                                                   v            v  hw RX ts
                                        runner CLOCK_REALTIME   pcap
                                                   |
                                              CLOCK_TAI
                                                   |
                                                   v
                                           RTP timestamps
```

Regime B — for the `@pytest.mark.ptp` tests the harness stops ptp4l and phc2sys and
MTL takes the PHC over. The host clock free-runs for the test's duration, which
nothing in these tests reads:

```text
  grandmaster host CLOCK_REALTIME --> grandmaster --> MTL internal PTP
                                                          |
                                            steers E810 shared PHC
                                                          |
                                         netsniff-ng hw RX ts --> pcap
```

In both regimes netsniff-ng's behaviour is unchanged and unchangeable: it reports the
E810 PHC. What changes is that the PHC is now grandmaster-locked either by ptp4l or
by MTL, so the pcap is traceable to the reference in both, and the RTP timestamps
derive from the same PHC in both.

## 5. What it costs

Stated plainly, because these are real:

* **Grandmaster error is exported into the host wall clock.** Today a wrong
  grandmaster breaks two test files. Under this design it moves every runner's
  `CLOCK_REALTIME`, which affects journald ordering, TLS validity windows and CI
  runner token refresh — things outside the test suite. Note the inverse, which is a
  genuine benefit: because both ends of the EBU comparison descend from the same
  clock, the *suite* becomes immune to grandmaster absolute error. Compliance
  measures relative consistency.
* **ntpsec must stop writing `CLOCK_REALTIME` on the runners.** Two writers on the
  system clock is the same class of bug as two writers on a PHC, with a larger blast
  radius. The recommendation is to keep ntpsec running with `noselect` on all
  servers so it keeps *measuring* without steering; `ntpq -p` then provides an
  independent cross-check (§6).
* **The grandmaster host becomes a time dependency, not only a storage one.** In
  this fleet it already serves the media corpus over NFS to every runner, so it is
  already a hard dependency — if it fails, the suite stops regardless. The
  difference is in failure *character*, not availability: NFS fails loud and
  fail-stop (`Media file not present on <host>` → SKIPPED, obvious within a minute),
  whereas a clock fails silent and fail-wrong — you still get a compliance verdict,
  it is just measured against the wrong epoch. That argues for monitoring, which is
  why the guards in §6 are not optional.
* **Loss of an independent reference per host.** Today each runner's clock is its own
  witness. Afterwards a fleet-wide time error has no dissenting voice unless ntpsec
  is retained as a monitor.

What it does **not** cost: coverage. Regime A still exercises MTL's default non-PTP
code path reading `CLOCK_TAI`. Only the provenance of that clock changes, not the
code path under test.

### Daemon inventory, before and after

A single reference clock does not mean a single daemon. Each runner still needs a
local PTP client, because "synchronised to the grandmaster" is a continuous
measurement, not a state a host can be put into once. What the design removes is not
the daemons but the *harness's* ownership of them:

| Per runner | Today | Under option A |
|---|---|---|
| `ptp4l` | started by `ptp_sync` per `@pytest.mark.ptp` test, on the shared PHC | `ptp4l-slave.service`, persistent, `-s -i <tx iface>` |
| `phc2sys` | started by `_start_capture_phc_sync()` per capturing test, `sudo pkill`ed after | `phc2sys-slave.service`, persistent, `-s <tx iface> -c CLOCK_REALTIME` |
| `ntpd` | steers `CLOCK_REALTIME` | runs with `noselect` — measures, does not steer |
| harness-started clock daemons | 1–2 per test, plus leaks | **none**, except the regime-B handover |

So the count of persistent units goes *up* by one and the count of transient ones
goes to zero. That is the trade, and it is the right way round: a per-test daemon
under `sudo pkill` cleanup is the mechanism behind both the two-writer conflict of
§3 and the leaked `phc2sys` processes that `_reap_ptp_daemons` exists to clean up. A
systemd unit has one lifecycle, one owner, and `systemctl status` as its witness.

Neither runner daemon can simply be dropped:

* **`ptp4l` is what makes the PHC grandmaster-locked.** Without it the PHC is only
  disciplined from below (today's `phc2sys -s CLOCK_REALTIME`, i.e. options B and C)
  or not at all, which is the free-run described in §1.
* **`phc2sys` is load-bearing for the EBU verdict, not a convenience.** In regime A
  MTL times RTP off `CLOCK_TAI`, and `packet_ts_vs_rtp_ts` compares that against
  pcap timestamps taken from the PHC. `phc2sys` is the only link that keeps the two
  operands on one clock. Remove it and the check compares a grandmaster-locked PHC
  against an NTP-steered system clock. The magnitude matters less than the
  structure: the two operands stop being common mode, against a [0, 1 ms] bound.

The only genuinely daemon-free arrangement on a runner is `enable_ptp=True`
everywhere, letting MTL's internal PTP be the sole clock discipline. It is worse on
three counts: the PHC is steered only while an MTL session is live and free-runs
between tests; `CLOCK_REALTIME` goes unsynchronised entirely, taking journald, CI
token refresh and TLS with it; and every test would then depend on the library
feature under test, which is circular — with MTL's `currentUtcOffset` handling still
display-only at `lib/src/mt_ptp.c:1498`.

On the grandmaster host the `ptp4l`/`phc2sys` master pair stays as-is under every
option. It *is* the single clock.

### Harness changes

Under option A the harness gets **smaller**, which is unusual for a change of this
kind and is worth weighing. Deleted from `tests/acceptance/conftest.py`:

* `_start_capture_phc_sync()` and `_wait_phc_sync_converged()` — there is no
  per-test daemon to start or wait for, because the capture PHC is already on the
  reference timescale when the test begins.
* `_host_tai_utc_offset()`. The `-O <tai_utc_offset>` arithmetic exists only to
  chase the PHC onto `CLOCK_TAI`; once the PHC is the master, the offset is applied
  once, on the host, by the system phc2sys.
* The `_PHC_SYNC_THRESHOLD_NS` / `_PHC_SYNC_TIMEOUT_SEC` constants and the
  `sync_phc` branch of `pcap_capture`.

`ptp_sync` keeps its shape and its reason for existing, but stops the host units for
`@pytest.mark.ptp` tests and restarts them in teardown. `_reap_ptp_daemons` stays as
the belt-and-braces path.

Under the option B or C fallbacks the harness is instead unchanged: the per-test
phc2sys keeps running exactly as today, and only the provenance of `CLOCK_REALTIME`
changes.

## 6. Failure modes and guards

**Holdover is not a cliff.** If the grandmaster's phc2sys dies after converging, its
PHC retains the learned frequency correction and keeps running at approximately the
right rate — sub-ppm to a few ppm, tens of milliseconds per day. Slaves follow it
gently. This is not the urgent case.

**Cold start is the real hazard.** A reboot or driver reload re-seeds the PHC with no
correction applied, which is precisely how the 73.195 s offset in §1 arrived. Slaves
configured to step would step onto the wrong value. Two mitigations:

1. Discipline the grandmaster PHC from an NTP-fed `CLOCK_REALTIME` at boot, so the
   window in which it is free-running is seconds rather than indefinite.
2. **Order the units correctly.** `phc2sys` does not need `ptp4l` — it sources from
   `CLOCK_REALTIME` — so the master `ptp4l` should order itself `After=` the
   grandmaster `phc2sys`, not the reverse. Get this backwards and the grandmaster
   *announces* before its PHC is disciplined, which is exactly the window a slave
   will step into.

**Guards to add:**

* A status row in `.github/scripts/acceptance_setup.sh` asserting: grandmaster
  offset < 1 µs (`phc_ctl <fabric iface> cmp`), local `ptp4l` offset in tolerance,
  and `|CLOCK_REALTIME − ntpsec estimate| < 100 ms`. This converts silent
  fail-wrong into loud fail-stop, which is the only objection to this design that
  survives scrutiny.
* Enforce **`enable_ptp` implies `@pytest.mark.ptp`** at collection time. Nothing
  enforces it today; the pairing merely happens to hold. One existing case sets
  `enable_ptp=True` outside the marker and is safe only because it requests no
  `pcap_capture`. The marker is the sole mechanism suppressing the capture phc2sys,
  so getting this wrong reproduces the two-writer bug with a multi-second symptom
  and no obvious cause.
* Make the grandmaster's announced clock quality honest. A grandmaster disciplined
  from NTP should not advertise `clockClass 248` (free-running). linuxptp slaves do
  not gate on `clockClass`, so this is documentation rather than protection — but it
  currently misdescribes the fabric.
* Assert that **`ptp4l` is not bound to the capture interface.** This is the one
  configuration mistake in this design that produces a silent, intermittent fault
  rather than an error: `ptp4l` runs fine until a capture starts, then drops into
  FAULTY for 16 s, and the PHC is undisciplined for exactly the window the test is
  measuring. A one-line check comparing `ptp4l`'s `-i` argument against the resolved
  sniff interface catches it at setup time. See the measurement in §4.

### 6.1 PTP multicast on a shared management LAN

Option C puts `224.0.1.129` traffic on the corporate management segment, through a
switch that is almost certainly not PTP-aware. Functionally that is fine — no
transparent clock means tens of microseconds of queueing asymmetry, against a 1 ms
compliance window — but emitting PTP on a shared enterprise segment may need the
network owners' agreement, and may be filtered. Fallback: unicast via
`unicast_master_table`, no multicast at all.

### 6.2 Timescale, decided once

Either stay all-UTC (`-O 0`) while MTL treats `currentUtcOffset` as log cosmetics,
or fix `lib/src/mt_ptp.c:1498` to apply the offset to the clock value and move the
fabric to TAI as the standard requires. Under this topology that becomes a single
decision in a single place instead of a convention silently replicated across every
host.

The MTL fix is the principled enabler and is worth making regardless of what any one
lab does: against real broadcast infrastructure, which serves TAI, MTL's clock
currently lands 37 s off UTC while reporting itself locked.

## 7. Suggested rollout order

Each step is independently testable and independently revertible.

0. **Measure before committing** — `ptp4l -m` with `free_running 1`, which adjusts no
   clock. **Done idle on one E810 runner; see §4.** Outcome: the fabric is healthy
   (path delay 470–605 ns, grandmaster selected from both ports), and `ptp4l` must run
   on the TX port because it cannot share a port with netsniff-ng. Still to do: the
   same measurement while a 2160p119 case runs, confirming the servo stays well inside
   the 1 ms window under load. That needs a CI dispatch or a built tree outside the
   runner's `_work` directory — never a pytest run inside `_work`, which would kill a
   subsequent job's `RxTxApp`. If the fabric itself proves unusable under load, fall
   back to option B or C.
1. **Grandmaster host only, additive:** fix the unit ordering so its PHC is
   disciplined before it announces (§6). Nothing slaves to it yet, so no runner
   behaviour changes. Option C additionally needs a second `ptp4l` (`serverOnly`, on
   the management port) and a phc2sys for that PHC; options A and B do not.
2. **One runner:** slave a single runner — `ptp4l -s -i <tx iface>` on the
   fabric plus `phc2sys -s <tx iface> -c CLOCK_REALTIME`, both as systemd units
   so the handover can stop and restart them, and ntpsec to `noselect`. Add the
   `ptp_sync` handover and drop `_start_capture_phc_sync` on a branch only. Confirm
   `CLOCK_REALTIME` tracks the grandmaster and that `|CLOCK_REALTIME − ntpsec|`
   stays small. Then run the full `tests/single/st20p` set on that runner and compare
   verdicts against a runner still on the old scheme. This is the real test: the
   fleet running both designs side by side on identical tests.
3. **Add the status guard and the `enable_ptp`/marker check** (§6), before step 4, so
   a regression during the rollout is named rather than diagnosed.
4. **Remaining runners**, one at a time.
5. **Optional, later:** revisit the timescale question and the MTL `currentUtcOffset`
   fix (§6.2).

Per-host prerequisite for every runner added: confirm the `ice` ports' shared PHC
index, confirm a spare kernel-managed PHC exists if option C is wanted, and record
which DPDK VFs are bound so the PF is known not to be in use by the data plane.

## 8. Scope: what is affected

The change is to `CLOCK_REALTIME`, which is host-wide. So the blast radius is "every
runner that runs any suite", but the *outcomes* that can change are a much smaller
set:

* **Directly affected** — the test's verdict depends on the capture PHC agreeing
  with MTL's time source. This is exactly the set that requests the `pcap_capture`
  fixture, because that is what produces an EBU compliance verdict.
* **Indirectly affected** — the test reads the wall clock but compares only against
  itself (MTL transmits and MTL receives, in one process, off one clock). The
  provenance of the clock changes; no assertion does.

### 8.1 Runners

Workflow legs are labelled by NIC family, not by hostname
(`runs-on: ${{ matrix.nic }}`):

| Label | Suites that target it | Capture-capable |
|---|---|---|
| `e810` | smoke, nightly (14 dirs), gtest | yes |
| `e830` | smoke, nightly, gtest | yes |
| `e835` | smoke, nightly, gtest | yes |
| `i225` | smoke only (`smoke-low-bandwidth`), optional/non-blocking | **no** — sets `no_capture: '1'` |
| `[self-hosted, perf, sut]` | `perf-pytest` (nightly cron) | not in the capture set |
| `ubuntu-22.04` | unit tests, report aggregation | GitHub-hosted, no NIC, unaffected |

The `i225` leg has no third port to sniff with, so it produces no compliance verdict
and is not in the directly-affected set — only the wall-clock set. Enumerating which
physical host carries which label needs repository admin access
(`gh api .../actions/runners`); labels are not recorded in a runner's local
`.runner` file, which holds only `agentName` and `poolName`.

### 8.2 pytest

Of 96 collected test functions under `tests/single/`, **29 request `pcap_capture`**:
1 smoke, 28 nightly, 1 neither. Two are `@pytest.mark.ptp` (regime B).

**All of the smoke suite on capture-capable NICs is in this set.** Note that `smoke`
is applied per *parameter* in four files, not only as a function decorator, so a
decorator-only scan undercounts it. Every file carrying a smoke parameter also
requests `pcap_capture`.

Nightly is a matrix over `dir`; **5 of the 14 dirs** contain directly-affected
tests, so 15 of 42 nightly legs. `st20p` contributes 18 of the 28 and is split
across three rows below only for readability:

| Nightly dir | Directly-affected test functions |
|---|---|
| `st20p` (format/transport) | `test_st20p_fps`, `test_st20p_resolutions`, `test_st20p_multicast`, `test_st20p_interlace`, `test_st20p_packing`, `test_st20p_pacing`, `test_st20p_redundant`, `test_st20p_input_format`, `test_st20p_transport_format`, `test_st20p_packing_transport_format`, `test_st20p_422p10le` |
| `st20p` (conversion) | `test_st20p_convert_on_rx`, `test_st20p_tx_rx_conversion` |
| `st20p` (pacing way) | `test_st20p_pacing_way_load`, `test_st20p_pacing_way_auto`, `test_st20p_pacing_way_x_pacing`, `test_st20p_pacing_way_sd_downgrade`, **`test_st20p_pacing_way_phc`** (`ptp`) |
| `st30p` | `test_st30p_format` (**smoke**), `test_st30p_integrity`, `test_st30p_channel`, `test_st30p_ptime`, `test_st30p_sampling`, `test_st30p_multicast` |
| `st40p` | `test_st40p_multicast_with_compliance` |
| `rx_timing` | `test_rx_timing_video_replicas_refactored`, `test_rx_timing_video_video_format_refactored` |
| `xdp` | `test_xdp_mode_refactored` |

Not affected, because they request no capture and so have no verdict that depends on
clock agreement: `cross_app`, `gstreamer`, `kernel_socket`, `rss_mode`, `st22p`,
`st41`, `udp`, `virtio_user` — 8 of 14 nightly dirs, 24 of 42 legs.

The `ptp` nightly dir is a special case: its only capture test,
`test_st20_interfaces_mix_refactored`, carries `xfail`/`ptp`/`refactored` but **not**
`nightly`, so `-m nightly` deselects it. It runs only under an explicit path or
marker selection.

The remaining 67 functions are indirectly affected: they read `CLOCK_TAI` for RTP
timestamps and validate inside MTL against the same clock, so they are
self-consistent whatever the clock's provenance. No expected verdict change. The
performance suite is in this group — none of the 29 capture tests live under
`tests/single/performance/`.

### 8.3 gtest — no outcome change today, one coverage gain available

`gtest-bare-metal` runs on `e810`, `e830` and `e835` via `.github/scripts/gtest.sh`,
which does not pass `--ptp`. Every time-dependent assertion in KahawaiTest is
self-referential: TX stamps with `mtl_ptp_read_time()` and RX compares against the
same clock in the same process. So **no gtest case changes outcome** under this
design. Two cases deserve naming anyway:

* **`Misc.ptp`** (`tests/integration_tests/tests.cpp:532-553`) asserts
  `|mtl_ptp_read_time() − CLOCK_REALTIME| < 5 µs`, five times over 2 ms intervals.
  Without `--ptp` both sides read `CLOCK_REALTIME`
  (`tests/integration_tests/tests.cpp:377-382`, installed as `ptp_get_time_fn` at
  `:408`), so it is trivially 0 and always passes. **With `--ptp` it demands that the
  host clock and the grandmaster agree within 5 µs** — which today they do not, since
  each runner is ntpsec-disciplined independently of the grandmaster
  (millisecond-class divergence at best). This is the only *differential* check
  between the two timescales in the tree, so it is the one place where the sync
  path's own accuracy matters instead of cancelling (§4). That decides something
  about the option choice: 5 µs is tighter than option B's software timestamping can
  deliver, so **the hardware options (A or C) would make this assertion hold and
  option B would not.** Under option A there is a wrinkle: a `--ptp` gtest run puts
  MTL on the grandmaster while `ptp4l` holds the PHC, which is the regime-B conflict,
  so such a run needs the same handover the pytest `ptp` tests get. If running gtest
  with `--ptp` is not a goal, the case is unaffected either way, because CI never
  passes it.
* **`st10_timestamp_test`** (`tests/integration_tests/tests.cpp:586`) asserts
  `EXPECT_GT(ptp2, ptp1)` across a 100 µs sleep — plain monotonicity. A backward step
  of `CLOCK_REALTIME` inside that window fails it. phc2sys steps only at startup
  (`first_step_threshold` defaults to 20 µs) and slews thereafter, so the operational
  guard is: **never start or restart a time daemon while a suite is running.**
  Sequence daemon changes against the runner being idle.

The unit suite (`unit_tests.yml`, GitHub-hosted) has no NIC and no clock dependency,
and is unaffected.

## 9. Reference points in the tree

| What | Where |
|---|---|
| Capture PHC discipline (regime A) | `tests/acceptance/conftest.py:441` |
| Live TAI-UTC offset, and why it is read not assumed | `tests/acceptance/conftest.py:409` |
| Daemon handling for `@pytest.mark.ptp` | `tests/acceptance/conftest.py:538`, `:1268` |
| Daemon reaping, and why `process.kill()` is not enough | `tests/acceptance/conftest.py:381` |
| netsniff-ng hardware timestamps and the ns pcap magic | `tests/acceptance/create_pcap_file/netsniff.py:186-198` |
| MTL seeds the PHC from the system clock | `lib/src/dev/mt_dev.c:848` |
| MTL uses `currentUtcOffset` for display only | `lib/src/mt_ptp.c:1498` |
| gtest reads `CLOCK_REALTIME` unless `--ptp` | `tests/integration_tests/tests.cpp:213`, `:377-382` |
| The `ptp` marker's contract | `tests/acceptance/pytest.ini:16` |
