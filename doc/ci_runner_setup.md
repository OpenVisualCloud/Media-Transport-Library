# Self-hosted runner setup for MTL CI

MTL's hardware jobs run on self-hosted bare-metal runners that own the NIC they
test. This page describes the contract between those hosts and the workflows.

## Jobs install nothing onto the host

A CI job never installs onto a runner. `apt`, a kernel module, a DMA binding, the
media share: each mutates a host that other jobs share, races with those jobs,
and hides drift in the host image — a runner missing a package looks healthy for
months because every job silently repairs it, until one job repairs it
differently.

So the host image carries those dependencies and the job only checks for them:

| Job step                            | What it does                                    |
| ----------------------------------- | ----------------------------------------------- |
| `task ci:pytest-setup -- ensure`    | Uses the acceptance virtualenv, building it the first time on a host |

The acceptance virtualenv is the one exception, and the reason is that it is not
host state. It is built from `tests/acceptance/requirements.txt` in the checkout
that is about to run, it lives in the runner user's cache rather than on the
system, and it is identical for every job on the host — a cache, not something a
host image can be out of date about. `ensure` treats it as one: it builds it when
there is none, rebuilds it when `requirements.txt` no longer matches the hash
recorded inside it, and otherwise costs a `pytest --version`. Both events print a
line naming the reason, so a host whose cache keeps disappearing says so in the
log instead of looking like a host that never needed one.

Two jobs on the same host can reach that step together, so the build takes an
`flock` on the virtualenv and the loser keeps the winner's work. `verify` — the
pure check, which installs nothing and fails with the command that provisions —
is what `Provision runner` and a human ask.

A missing dependency fails the job with the command that provisions it. The
provisioning commands still exist and are meant to be run by hand on the host:

```bash
task ci:pytest-setup -- install         # ~/.cache/mtl-ci/acceptance-venv
```

On a fleet whose machines are not reachable from every desk that maintains
them, the runner itself is the way in: the `Provision runner` workflow
(`.github/workflows/provision-runner.yml`) runs those same commands on the host
carrying a given NIC label.

```bash
gh workflow run provision-runner.yml -f nic=e810   # then e830, e835, ...
```

It is `workflow_dispatch` only and installing is all it does, so it does not
weaken the rule above — that rule is about a *test* job repairing its host
silently, hiding drift and racing with everything else on the machine. This is
the opposite: it says which host it touched, and it ends with `verify`, so a green
run means the leg that was failing for a missing prerequisite will not fail for
that reason again.

`workflow_dispatch` only works from the default branch, though, so a fleet whose
hosts are missing something a branch has just started asking for cannot be
provisioned from that branch. Until this file is on `main`, provisioning the
acceptance virtualenv there is one SSH session per host — which is exactly why
the virtualenv step became `ensure`.

The acceptance virtualenv lives outside the workspace, because a cache kept
inside it is not one: every hardware job starts with `actions/checkout`, whose
default clean is `git clean -ffdx`, and `tests/acceptance/.venv` is gitignored —
so `-x` deletes it before the step that uses it runs, and a virtualenv built
there is built again for every job. `MTL_CI_VENV` names another location; an existing
in-workspace virtualenv is still used where one is present, under either
`tests/acceptance/.venv` or the `tests/acceptance/venv` that
`setup_acceptance.sh` writes, so a developer machine needs no second copy.
Accepting both names is not tidiness: `verify` used to look only for the
dotted one, so a host provisioned by `setup_acceptance.sh` had a working
virtualenv that the job called missing.

## Host configuration instead of repository secrets

Lab facts live on the runner, in `/etc/mtl-ci/runner.env`, not in GitHub
secrets. Point `MTL_CI_RUNNER_ENV` elsewhere to test the mechanism locally.

The shadow/SUT host pair used by `perf-pytest.yml` and the optional shadow sync
in `custom-pytest.yml` reads `/etc/mtl-ci/shadow-host`, a file defining `IP` and
`USER`.

Only credentials for genuinely external services remain repository secrets:
`COVERITY_EMAIL`, `COVERITY_TOKEN`, and the built-in `GITHUB_TOKEN`.

## NIC labels

A runner label is a claim about hardware, so it is verified rather than trusted:
`task ci:pytest-setup -- pci` resolves the label against `lspci` and fails with
the list of Intel network devices actually present when the card is not there.

| Label  | Device IDs (8086:)        | Notes                                     |
| ------ | ------------------------- | ----------------------------------------- |
| `e810` | 1592, 1593, 159b          | ICE                                       |
| `e830` | 12d2, 12d3                | ICE                                       |
| `e825` | 579d, 579e                | ICE                                       |
| `e835` | 1249, 124a                | ICE                                       |
| `i225` | 15f2, 15f3, 15f8, 0d9f, 3100 | IGC, 2.5 GbE, no SR-IOV                |
| `i226` | 125b, 125c, 125d, 3102    | IGC, 2.5 GbE, no SR-IOV                   |

### Datapath follows the card, not the job

A DPDK port belongs to one process, so a test that transmits and receives needs
two of them. The i225/i226 cards have no virtual functions to hand out, so
`task ci:pytest-setup -- pci` resolves the datapath from the ports it actually
finds instead of the job declaring one:

| Ports found        | Datapath | How the tests attach                     |
| ------------------ | -------- | ---------------------------------------- |
| SR-IOV card        | `VF`     | two VFs of one PF                        |
| two ports, no VFs  | `PF`     | the two PFs                              |
| one port, no VFs   | `KERNEL` | MTL's kernel socket, `kernel:<ifname>`   |

`KERNEL` is the only datapath a single-port card has: transmitter and receiver
are two sockets on the one interface. It is slower than DPDK, so it is never
chosen for a card with two ports. The resolved value is exported as
`INTERFACE_TYPE` and reaches the framework through `test_config.yaml`.

### The label also decides whether ICE is needed

Only the E8xx family is served by the Kahawai ICE driver. `task ci:ice-required`
answers for the label in `NIC`, and `.github/actions/validate-host` skips both
the ICE cache restore and the driver alignment when the answer is `false` — an
i225 runner neither needs a kernel module built for a card it does not have, nor
should fail because that artifact was built for a different kernel. An unlabeled
runner (the performance rig) is assumed to need it, which is what every job did
before this existed.

#### On an E8xx host the Kahawai driver is not optional

The in-tree `ice` of a distribution kernel loads, brings the link up and carries
traffic, so a host that never had the Kahawai module aligned looks healthy. It
is not: the VF rate limiter MTL paces with is a capability the PF grants, and
only the Kahawai build offers it. Under the in-tree driver the VF negotiates no
QoS capability, `vf->qos_cap` in DPDK's iavf PMD stays NULL, and MTL's TM pacing
path — `dev_if_init_pacing` selecting `ST21_TX_PACING_WAY_RL`, then
`dev_rl_init_nonleaf_nodes` calling `rte_tm_node_add` — dereferences it. The
symptom is a `SIGSEGV` in `iavf_tm_node_add` a minute or two into the first test,
right after `dev_if_init_pacing(0), try rl as drv support TM` in the log, with
nothing tying it to the driver. `rte_tm_capabilities_get` is no help as a
pre-check: the iavf implementation reads the same pointer in its first
instructions.

So on the fleet the alignment is a job step (`sudo -E env -u BASH_XTRACEFD
"$TASK_BIN" ci:activate-ice`, idempotent, a no-op when the running module is
already the cached one).

#### A capture leg needs both ports cabled

`gen_config.py` takes the sniff device from the second `--pci_device` entry, which
on an E8xx runner is the card's second port, and the acceptance framework builds
one VF per port: on a two-port E830 the transmitter lands on a VF of port 0 and
the receiver on a VF of port 1. Both therefore need a link. With only the first
port cabled, MTL reports `dev_detect_link(1), link not connected for
0000:<bus>:11.0` and `mt_dev_create` fails with `-5` before any traffic; with the
first port cabled but nothing on the second, traffic flows and the capture stays
empty, which EBU LIST returns as a report with `total_streams: 0` and the suite
reads as non-compliant. Neither is an MTL fault, and neither is visible from the
label — a runner advertising `e830` has to be cabled port to port as well as
carrying the card.

### The i225 leg of the smoke suite

2.5 Gbps of link means only the `low_bandwidth` subset fits: ST 2110-22, ST
2110-20 at up to 1080p29, ST 2110-30 and ST 2110-40. That subset is the `i225`
leg of the `smoke-tests-bare-metal` matrix, running concurrently with the E8xx
legs. The card has no third port to sniff with, so the leg sets `no_capture` and
carries no EBU compliance verdict, and it is `continue-on-error` while the
platform is being brought up.

The label was served by nothing until `mtl-runner-12` came online on 2026-08-21,
so the leg was queued-and-cancelled on every run before that and looked like a
failure.
[`i225_leg_analysis.md`](i225_leg_analysis.md) has that history, the one API field
that distinguishes "no runner has this label" from "the fleet is busy", and what
the first real run failed on.

Turning capture on for this leg takes more than dropping `no_capture`, and the
failure mode if you only drop it is total: `gen_config.py` derives the sniff
device from the *second* `--pci_device` entry, a single-port card has only one, so
neither branch runs and no `capture_cfg` is written at all — which the
`pcap_capture` fixture reads as "compliance is required and this host is
misconfigured" and fails every test that uses the fixture. What is missing is a
way for `gen_config.py` to say "capture on the DUT interface itself". That is
sound on this card specifically, because a single-port i225 runs MTL's
kernel-socket datapath, where the traffic stays visible to `AF_PACKET` instead of
being taken over by DPDK, and `conftest._select_sniff_interface` already falls
back to the DUT interface when no sniff device is named.

## A DPDK artifact remembers the path it was built at

The build job installs DPDK under its own workspace, and meson bakes that prefix
into the install: `libdpdk.pc` records it, and `librte_eal` records
`RTE_EAL_PMD_PATH`, `<prefix>/lib/<arch>/dpdk/pmds-<abi>`, as the one directory
EAL loads drivers from. There is no environment override for it, and MTL builds a
fixed EAL argv, so there is no `-d` either.

When the runner that builds and the runner that tests keep their workspace at the
same path, this is invisible. When they do not — a different `_work` root, or a
local run that builds in a container and tests on the host that owns the card —
no driver registers and the first failure is inside `mtl_init`:

```text
Error: mt_mempool_create_by_ops(1), fail(Invalid argument) for T_P0_SYS, n 2047
Error: mtl_init, st dev if init fail -12
```

The mempool, not the NIC, because the mempool ops MTL asks for (`stack`) ship as
a plugin like every driver. `task ci:configure-host -- dpdk-plugins` makes the
baked path resolve to the drivers that were actually restored, and
`.github/actions/validate-host` runs it right after the restore. It is a symlink,
so it also serves the acceptance tests, which reach the host over SSH and inherit
none of the job's environment.

## The plugin registry has to be a file, for the same reason

MTL loads its ST 2110-22 codec plugins from a JSON registry, and finds it in one of
two ways (`lib/src/mt_config.c`): `KAHAWAI_CFG_PATH` if set, otherwise the
cwd-relative literal `kahawai.json`. `task ci:configure-host -- registry` renders
the registry correctly — `.github/workflows/kahawai_template.json` with the
JPEG-XS plugin of *this* run's cache substituted in — but exporting
`KAHAWAI_CFG_PATH` through `GITHUB_ENV` only reaches the steps of the job. The
apps do not run in a step. They run over SSH and then under sudo, inheriting
nothing, so they took the fallback: `kahawai.json` relative to the cwd of an SSH
session, which is the login user's home directory.

That made JPEG-XS depend on a file nobody tracks. Where a human had put one there,
st22p passed; where none existed, or where the one that did was still the tracked
repository `kahawai.json` with its `st22_svt_jpegxs` entries at `"enabled": 0`, the
plugin never registered and every JPEG-XS case failed the same way:

```text
Warn: st_plugin_register, dlopen /usr/local/lib64/libst_plugin_st22_sample.so fail
Error: st22_get_encoder, fail to get, input fmt: YUV422PLANAR10LE, output fmt: JPEGXS_CODESTREAM
Error: st22p_tx_create(0), get encoder fail -22
```

RxTxApp then exits 251 and the FFmpeg leg writes an empty file. The stage now
installs the rendered registry in the login user's home under both names the
library will look for, `kahawai.json` and `.kahawai.json`, and keeps the export for
the in-step consumers. A host needs no hand-made registry, and one that has an old
one is corrected on every run.

## The EBU LIST compliance analyser

The acceptance framework does not judge ST 2110 compliance itself. The
`pcap_capture` fixture records the stream with `netsniff-ng`, posts the pcap to an
EBU LIST instance, and reads the verdict out of the returned report
(`tests/acceptance/mtl_engine/pcap_compliance.py`). A host with no reachable
analyser therefore produces no verdict, and says so late, inside a test.

`task ci:ebu-list -- verify` checks it up front: that `netsniff-ng` is installed
and can be run under passwordless sudo, and that the instance in `EBU_IP` returns
a token for `EBU_USER`/`EBU_PASSWORD`. Like every other CI-facing check it installs
nothing and fails with the command that provisions.

It separates absence from misconfiguration, because only one of the two is the
leg's problem. With `EBU_IP` unset there is nothing to configure wrongly:
`pytest-setup.sh` leaves the `ebu_server` block out of the generated
configuration and the suite runs without a compliance verdict, still
transmitting, receiving and comparing every frame. The check says so on the run's
summary and lets the leg proceed — a check added to stop a late failure inside a
test must not become an earlier failure of its own. With `EBU_IP` set but the
credentials missing, or the instance not answering, the analyser would reject
every upload minutes into the run, and that fails the leg.

Set `MTL_CI_REQUIRE_COMPLIANCE=1` in `runner.env` on a host whose analyser is
deployed. Absence then fails the leg on that host, so a stack that stops
answering is a red leg rather than a run that silently stopped judging
compliance.

EBU LIST is a Docker Compose stack, deployed from the `ebu-list` directory of the
internal `Media-Transport-Library-Devtools` repository. Clone it **outside** the
MTL checkout so it never appears in `git status`:

```bash
gh repo clone intel-sandbox/Media-Transport-Library-Devtools ~/mtl/devtools
cd ~/mtl/devtools/ebu-list
cp .env.template .env            # then set EBU_LIST_USERNAME/PASSWORD
task ci:ebu-list -- up           # docker compose up -d, from the MTL checkout
```

The stack publishes port 80 through an nginx proxy, so a host running its own
analyser sets `EBU_IP=127.0.0.1`. `EBU_LIST_DIR` in `runner.env` points
`task ci:ebu-list` at the clone.

pi-list has no self-service registration, so the account named by `EBU_USER` has
to be created once against the running instance — the web UI has no sign-up form
either, and the API endpoint is `POST /user/register`, not the `/auth/register`
that its sibling `/auth/login` suggests:

```bash
curl --noproxy '*' -X POST -H 'Content-Type: application/json' \
  -d '{"username": "gta", "password": "..."}' http://127.0.0.1/user/register
```

Use `--noproxy` (or `no_proxy`) for anything aimed at the analyser: a lab host
exports `http_proxy` for internet access, and without it an upload to a lab
address is handed to a proxy that cannot route there. The Python client avoids
the same trap by setting `session.trust_env = False`.

### Proving the chain without a working transmitter

`task ci:ebu-list -- verify` proves the analyser answers, not that a capture off
this host's wire reaches it and comes back judged. To prove the whole chain when
MTL itself cannot transmit, replay a known-good ST 2110 capture onto the interface
and capture it back. Public vendor samples live in the
[`ST2110_pcap_zoo`](https://github.com/NEOAdvancedTechnology/ST2110_pcap_zoo)
repository; this needs `tcpreplay`, which is not otherwise a host prerequisite.

```bash
gh api repos/NEOAdvancedTechnology/ST2110_pcap_zoo/contents/ST2110-20_720p_59_94_color_bars.pcap \
  -H 'Accept: application/vnd.github.raw' >/tmp/st2110.pcap
sudo netsniff-ng --silent --in "${IFACE}" --out /tmp/replay.pcap -T 0xa1b23c4d \
  --num 10001 'udp and dst 239.0.0.1' &
sudo tcpreplay --preload-pcap --intf1="${IFACE}" /tmp/st2110.pcap
python3 tests/acceptance/compliance/upload_pcap.py --ip "${EBU_IP}" \
  --user "${EBU_USER}" --password "${EBU_PASSWORD}" --pcap /tmp/replay.pcap
```

A working chain reports `video_streams: 1` and a `media_specific` block carrying
the raster, sampling and depth that were on the wire. Do not expect a *compliant*
verdict from a replay: `tcpreplay` does not reproduce the sender's ST 2110-21
pacing, and the vendor samples are real senders rather than ideal ones, so the
timing checks legitimately fail. `media_type: video` plus a correct
`media_specific` is what this test is for.

### Capture needs privilege, and it fails quietly without it

`netsniff-ng` opens a `PF_PACKET` socket and raises
`/proc/sys/net/core/{r,w}mem_max`, so unprivileged it prints `Permission denied`
and `Creation of PF socket failed` — and then **exits 0**, which made an SSH
session running as the ordinary lab user look like it had captured. It runs under
`sudo`, so the account the tests run as needs passwordless sudo.

File capabilities are not an alternative: `setcap cap_net_raw,cap_net_admin+ep`
gets past the socket, but netsniff-ng then fails on `ioprio_set` for a realtime
I/O class, which wants `CAP_SYS_ADMIN`. Granting that to a binary is no better
than running it as root.

Two consequences of capturing as root are handled in the framework, and are worth
knowing when reading it: the pcap belongs to root, so it is removed with
`sudo rm` (the default `pcap_dir` is sticky `/tmp`, where the test account cannot
unlink a root-owned file); and because `sudo` does not pass signals to its child,
the capture is reaped with `sudo pkill` on the argv rather than through the
process handle, the same way `conftest._reap_ptp_daemons` reaps ptp4l.

## The DMA channels the job serves itself

`sudo task ci:bind-test-ports` serves two DMA channels on the test card's NUMA
node, and it takes them from wherever they are: a channel already on vfio-pci is
left as it is, a channel with no driver is bound, and a channel `idxd` holds is
taken from it. All three are the same `dpdk-devbind.py -b vfio-pci <bdf>` call.

Taking them is the whole point, because a stock host serves none. Every DSA
device comes up on `idxd`, so a step that refused to touch those refused every
host in the fleet — and it refused the leg, not just the DMA cases, which cost
three gtest legs a round for nothing.

Two facts make the runtime version work, and neither needs a reboot:

- `vfio-pci` carries a denylist that includes Intel DSA (`8086:0b25`), and a bind
  against a denylisted device ends in `Cannot bind to driver vfio-pci: [Errno
  22]` with `exists in vfio-pci device denylist, driver probing disallowed` in
  `dmesg`. `disable_denylist=1` turns it off, and the parameter is `0444` once
  the module is loaded — so the job *reloads* the module, which is allowed:
  nothing holds `vfio_pci` open between jobs, and the ports and channels this
  step binds are all bound after the reload.
- `idxd` releases a DSA device on request. On the EMR host this was proven on
  (kernel 6.8) `dpdk-devbind.py -b vfio-pci` moved one over with `dmesg` reading
  `device denylist disabled - allowing device 8086:0b25`, and all 17 `Dma` gtest
  cases then passed on it. A driver that wedges instead is caught by the timeout
  every NIC operation in that script runs under, and reported as a host fault.

`dpdk-devbind.py --status-dev dma` is the check: two entries reading
`drv=vfio-pci` on the same NUMA node as the test card. A host that ends up with
fewer runs the suite anyway — the cases that copy with DMA ask the library for a
channel and report themselves skipped when there is none, so the alternative is
running nothing at all. The step says so on the run summary, and a host that is
meant to serve channels sets `MTL_CI_REQUIRE_DMA=1` in `runner.env` to make the
shortfall a failure again.

The node the channels are on is a requirement and not a preference. MTL grants a
session a channel of the port's own socket and no other
([`dma.md` section 3.4](dma.md#34-dma-socket)), while the suite's
`st_test_dma_available` only counts the channels that registered — so a channel
from another node is worse than no channel at all: the DMA cases neither skip
themselves nor offload, they check the offload path's expectations against a
plain `memcpy`. Both halves of the job therefore take the ports' own node and
nothing else. An E810 host whose card sits on NUMA 2 while its channels sit on 0
and 1 failed `St20_rx.digest_ooo_slice_4320p` with 143 incomplete frames against
a limit of 16 for exactly this reason, and now runs the leg without DMA offload
instead.

A platform that lists no DMA device at all needs its DSA or CBDMA engines
enabled in the BIOS first. A host where something else keeps `vfio_pci` loaded,
so the reload cannot happen, is the one case left for the boot-time version:

```bash
echo 'options vfio-pci disable_denylist=1' | sudo tee /etc/modprobe.d/vfio-pci.conf
```

One thing the step does *not* do is unbind a channel afterwards. `idxd` is a
kernel accelerator driver that nothing else on a test host uses, and leaving the
two channels on vfio-pci is what makes the next job's preparation a no-op.

## Hugepages are reserved by the job, not by the image

`bind-test-ports` also reserves 2048 × 2 MB hugepages (`MIN_HUGEPAGES`) when the
host has fewer, because every process the gtest suite starts is a DPDK process
and EAL stops on `Cannot get hugepage information` without them — several steps
later, in words about DPDK rather than about the host. A reboot clears the
reservation, so this is exactly the kind of state that is missing on a host
nobody has touched since one.

It is raised, never lowered: a host may have reserved more for something else,
and this suite is not the one to take them back. What it cannot do is defragment
memory — a kernel that serves fewer pages than it was asked for says so in the
step's log, and the fix is to free memory or reboot.

## The VFIO group nodes are handed to the test account by the suite

A VF is bound to `vfio-pci` by root, and the `/dev/vfio/<group>` node the kernel
creates with it is `0600 root:root` unless the host carries the udev rule from
[`run.md` section 3.1](run.md#31-allow-current-user-to-access-devvfio-devices).
The acceptance suite reaches the DUT over SSH as an unprivileged account, so on a
host without that rule every case dies in EAL with

```text
EAL: Cannot open /dev/vfio/468: Permission denied
PCI_BUS: Requested device 0000:38:01.0 cannot be used
```

which reads as a binding problem and is a file mode. Three E8xx hosts of the
fleet lost a whole smoke leg to it in one round while a fourth, which happens to
carry the rule, passed.

`Nicctl.grant_vfio_access` now hands those nodes to the account the suite
connects as, in the same session that created the VFs. The udev rule is still
worth having — it is the general answer for anyone running MTL by hand — but it
is no longer a prerequisite for a CI host, and it could not be a complete one
anyway: the node is recreated with root ownership every time the VFs are.

## Host image prerequisites the jobs check but cannot fix

Because jobs install nothing, a gap in the host image surfaces as a failed check
with the command that closes it. `task ebpf:check` is that check for the build
toolchain — headers, tools and kernel configuration alike — and it names the
package to install for each thing it finds missing, so run it on a new host
before wiring it into the fleet.

It answers for two different consumers, so it takes a scope. `task ebpf:check`
is everything needed to build and run the eBPF/XDP paths, which is what to run
before `task ebpf:install`. `task ebpf:check-build` is the subset a plain MTL
build consumes: the `libelf` and `zlib` that `libdpdk.pc` names in
`Requires.private`, and `make`. The build job runs the latter, because it builds
DPDK, MTL and the plugins and never builds xdp-tools — held to the full set it
failed on `cap-ng.h`, a header nothing it compiles includes.

Three prerequisites are easy to miss:

- `libelf-dev`. `.local_install/dpdk`'s `libdpdk.pc` lists `libelf` in
  `Requires.private`, so `pkg-config --exists libdpdk` — and therefore every
  build against the restored DPDK — fails without it, even though DPDK itself is
  already built.
- `libcap-ng-dev`. `xdp-tools` links `libcap-ng` to drop capabilities, so the
  manager's eBPF/XDP objects do not build without it. A host provisioned before
  the eBPF path was wired into the manager will have every other prerequisite
  and still fail `task ebpf:check` on this one; the full apt line for the XDP
  toolchain is in [`xdp.md`](xdp.md). A host that only builds MTL does not need
  it, which is what the build scope above is for.
- `python3-venv`. Debian and Ubuntu keep the venv module out of `python3`, and
  `python3 -m venv` stops on "ensurepip is not available". A host that has
  `virtualenv` or `uv` instead is fine — both carry their own pip and the job
  uses whichever it finds, which is still the host as it is and not a job
  installing anything. A host with none of the three is told which package to
  install, before it leaves a half-built directory behind. This is what the i225
  leg failed on twelve seconds into its first run on a new host.
- **an SSH login to itself.** The framework talks to the DUT over SSH even when
  the DUT is the runner: `gen_config.py` writes `ip_address: 127.0.0.1` with
  `connection_type: SSHConnection`, and mfd_connect opens a paramiko session
  before the first case. So the test account needs its own public key in its own
  `authorized_keys` (`ssh-keygen -t ed25519 -N '' -f ~/.ssh/id_ed25519`, then
  `cat ~/.ssh/id_ed25519.pub >> ~/.ssh/authorized_keys` with `~/.ssh` at mode
  700 and the file at 600 — sshd's `StrictModes` ignores a key that anyone else
  in the group could have written). Not `ssh-copy-id`: it authenticates before
  it copies, and a fleet host that offers publickey only cannot let it in to do
  the work, which is the very failure being fixed. A host that keeps
  its key elsewhere sets `RUNNER_SSH_KEY`. Worth checking deliberately, because
  the failure names the wrong thing: paramiko is handed the key *and* the empty
  password `gen_config.py` always writes, and it tries the password last, so a key
  it cannot use is reported two minutes in as
  `BadAuthenticationType: allowed types: ['publickey']` — a password problem on a
  host that has no password. `task ci:pytest-setup -- connection` is one second of
  `ssh` instead, in its own step, and prints what `ssh` said.
- the media assets. The tests read ST 2110 source files from `media_path`
  (`/mnt/media` by default), normally the lab's NFS share. `task ci:media-assets
  -- list` reports which files a host is missing, `-- verify` turns that into a
  verdict — which is what the test jobs run, because a case whose file is absent
  skips and a host that lost the share otherwise reports a green leg having
  transmitted nothing — and `task ci:media-assets -- generate` synthesises
  stand-ins of the right geometry and format for a host that has no share,
  enough to exercise the suite and not a substitute for the real content.

## A red gate that means the fleet was busy

The bare-metal test workflows do not build their own artifacts: `pr-gate` waits
for the `build` check on the same commit, and the smoke and gtest legs only start
once it is green. That wait is deliberately patient, because the queue in front
of the build host is routinely longer than the build itself.

It is not infinite, and the two ways it can end read differently on purpose. A
build that ran and failed fails the gate with its conclusion. A build that never
left the queue fails it with the number of minutes it sat there and the sentence
that this is fleet availability rather than a result — no runner online for the
build host, and a commit that was therefore never tested. Re-run the gate once a
runner is back; there is nothing to fix in the change.

Worth knowing when reading such a run: a queued job holds no runner, so the leg
that never started is invisible in the Actions UI beyond its own spinner, and
`gh run view <id> --json jobs` reports `runnerName` empty for it. The way to see
which host carries a label is the jobs API of the last run that did get picked
up.
