# DPDK patch set analysis — what 26.07 covers and what MTL keeps

Snapshot date: **2026-08-24**

| Item | Value |
|---|---|
| MTL pinned DPDK | **26.03** — [versions.env:1](versions.env) (`DPDK_VER=26.03`, `DPDK_MTL_MINOR_VER=91`) |
| DPDK installed on this host | `26.03.90_mtl_` — `pkg-config --modversion libdpdk` |
| Target DPDK | **26.07** |
| Patch set today | 16 files in [patches/dpdk/26.03/](patches/dpdk/26.03/) |
| Patch set after the bump | 11 files in `patches/dpdk/26.07/` — final, because `0003` is dropped (§6) |

This file is the source record for the work list in [tasks.md](tasks.md). Read the
section that a task names before you start the task.

## 1. Decision

**MTL sends nothing upstream.** The team decided this on 2026-08-24. Every action
that needed a post to `dev@dpdk.org`, a repost, or a maintainer reply is cancelled.

What replaces it:

1. Move MTL to DPDK 26.07.
2. Drop every patch that 26.07 already carries.
3. Keep every patch that 26.07 does not carry, unless 26.07 reaches the same effect at
   runtime and MTL can be changed to drive it from `lib/`. That exception is granted to
   `0003` and to nothing else — §6. It is not a standing test that a later reader may
   apply on his own judgement: using it on any other patch, at this bump or at a future
   one, is a new decision and must be recorded here as one. Renumber the set.
4. Add a test at the cheapest tier for each change that can alter behaviour.

The upstream review history stays in this file for one reason only: it is the
evidence for the drop list and for the two patches that MTL keeps forever. It is
not a plan of action any more.

## 2. Patch set today

`patch -p1` applies the flat glob `patches/dpdk/$DPDK_VER/*.patch` —
[script/build_dpdk.sh:98](script/build_dpdk.sh). The `hdr_split/` and `windows/`
subdirectories are applied by hand — `hdr_split/` per
[doc/experimental/header_split.md:21](doc/experimental/header_split.md), `windows/` per
[doc/build_WIN.md:86](doc/build_WIN.md), which is the only `windows/` flow that works. The
second one, [.github/workflows/msys2_build.yml:136](.github/workflows/msys2_build.yml),
cannot apply the file it names — §8.

| # | Patch file | Upstream state | Action on 26.07 |
|---|---|---|---|
| 0001 | [e810-set-max-ring-desc-to-max-allowed-by-hardware](patches/dpdk/26.03/0001-e810-set-max-ring-desc-to-max-allowed-by-hardware.patch) | Merged `03bee932e9` | **Drop** |
| 0002 | [net-iavf-refine-queue-rate-limit-configure](patches/dpdk/26.03/0002-net-iavf-refine-queue-rate-limit-configure.patch) | Merged `e04c0fa68c`, with a logic fix (§5) | **Drop** |
| 0003 | [ice-set-ICE_SCHED_DFLT_BURST_SIZE-to-2048](patches/dpdk/26.03/0003-ice-set-ICE_SCHED_DFLT_BURST_SIZE-to-2048.patch) | Superseded by the `rl_burst_size` devarg, `b3f2afb3b7` + `74dc5eb5c7` | **Drop.** 26.07 does not carry the patch itself, so MTL replaces it with an `rl_burst_size` field — §6 |
| 0004 | [Change-to-enable-PTP](patches/dpdk/26.03/0004-Change-to-enable-PTP.patch) | Rejected on correctness | Keep → new `0001` |
| 0005 | [iavf-disable-runtime-queue](patches/dpdk/26.03/0005-iavf-disable-runtime-queue.patch) | Acked, never applied | Keep → new `0002` |
| 0006 | [pcapng-add-user-timestamp-support](patches/dpdk/26.03/0006-pcapng-add-user-timestamp-support.patch) | Approved, never applied | Keep → new `0003`, see §7 |
| 0007 | [config-add-mtl-version-to-version-string](patches/dpdk/26.03/0007-config-add-mtl-version-to-version-string.patch) | MTL-local by design | Keep → new `0004`, refreshed — §3 |
| 0008 | [net-iavf-fix-large-VF-IRQ-mapping](patches/dpdk/26.03/0008-net-iavf-fix-large-VF-IRQ-mapping.patch) | Merged `cc58d28b10`, by Anatoly Burakov | **Drop** |
| 0009 | [net-ice-fix-TxPP-timer-association-in-txtime-context](patches/dpdk/26.03/0009-net-ice-fix-TxPP-timer-association-in-txtime-context.patch) | Never submitted | Keep → new `0005` |
| 0010 | [net-ice-fix-read-clock-to-use-PHC-hardware-time](patches/dpdk/26.03/0010-net-ice-fix-read-clock-to-use-PHC-hardware-time.patch) | Never submitted | Keep → new `0006` |
| 0011 | [net-ice-fix-TxPP-launch-time-encoding-for-19-bit-HW-field](patches/dpdk/26.03/0011-net-ice-fix-TxPP-launch-time-encoding-for-19-bit-HW-f.patch) | Never submitted | Keep → new `0007`, see §8 |
| 0012 | [net-ice-e830-use-direct-MMIO-for-PHC-update](patches/dpdk/26.03/0012-net-ice-e830-use-direct-MMIO-for-PHC-update.patch) | Never submitted | Keep → new `0008`, see §8 |
| 0013 | [net-ice-always-init-PHC-owner](patches/dpdk/26.03/0013-net-ice-always-init-PHC-owner.patch) | Never submitted | Keep → new `0009`, see §8 |
| 0014 | [net-ice-gate-send-on-timestamp-offload-to-e830](patches/dpdk/26.03/0014-net-ice-gate-send-on-timestamp-offload-to-e830.patch) | Merged `b87947ed19`, written twice | **Drop** |
| — | [hdr_split/0001-net-intel-ice-support-hdr-split-mbuf-callback](patches/dpdk/26.03/hdr_split/0001-net-intel-ice-support-hdr-split-mbuf-callback.patch) | Rejected, API "not necessary" | Keep, unchanged name, §10 |
| — | [windows/0001](patches/dpdk/26.03/windows/0001.patch) | Never submitted | Keep, unchanged name, refreshed and lost 2 hunks — §3 |

The "Action on 26.07" column records the mapping T-02 executed. `patches/dpdk/26.07/`
carries the 11 kept files and none of the 5 dropped ones. That directory is untracked, so
this is a working-tree fact. Two rows still defer work: `0003` defers its replacement field
to T-04 (§6), and `0007` defers the version-string conflict to T-03 (§3).

Count: 5 dropped, 11 kept. 16 rows in, 11 files out — §6 closed the last row.

## 3. Verification status of the drop list — read this before you drop anything

The six upstream commit hashes above come from an earlier session. That session read
a DPDK git tree at `/home/labrat/dev1/dpdk`. **That path does not exist on this host
any more, and no DPDK git tree exists here.** So the hashes are a record, not a
measurement you can repeat today.

`script/build_dpdk.sh` downloads a **ZIP archive**, not a git clone
([script/build_dpdk.sh:90-97](script/build_dpdk.sh)), so `git merge-base` cannot
answer "is commit X in v26.07". Check the source instead.

**Task T-01 did this on 2026-08-24.** It unpacked
`https://github.com/DPDK/dpdk/archive/refs/tags/v26.07.zip`, which gives a tree whose
`VERSION` file reads `26.07.0`. It then ran one grep per drop candidate and one
`patch -p1 --dry-run` per patch, for all 16 patches. The measured result follows. It
replaces the earlier estimate. Nobody needs to repeat the greps.

| Drop | Evidence to find in the v26.07 source | File in the DPDK tree | Measured result |
|---|---|---|---|
| 0001 | `IAVF_MAX_RING_DESC` is `(8192 - 32)`, not `4096` | `drivers/net/intel/iavf/iavf_rxtx.h` | **Found.** Line 19 reads `(8192 - 32)`. The earlier filename `iavf/iavf.h` was wrong — the symbol sits in `iavf_rxtx.h` in both 26.03 and 26.07 |
| 0002 | the guard reads `(vf->tm_conf.nb_tc_node != 1 \|\| vf->qos_cap->num_elem != 1)` and sits below the `VIRTCHNL_VF_OFFLOAD_QOS` check | `drivers/net/intel/iavf/iavf_tm.c` | **Found.** Line 825 carries the `\|\|` form. The capability check sits above it at line 813 |
| 0003 | the string `rl_burst_size` is present | `drivers/net/intel/ice/ice_ethdev.c` | **Found.** Line 45 defines the devarg key. `ice_parse_devargs()` parses it with `rte_kvargs_process` at line 2502, and `ice_dev_init()` acts on the parsed `ad->devargs.rl_burst_size` at lines 2727-2728. But `ICE_SCHED_DFLT_BURST_SIZE` still reads `(15 * 1024)`, so 26.07 does not carry MTL `0003` itself |
| 0008 | `chunk_sz` no longer counts the inline queue vector | `drivers/net/intel/iavf/iavf_vchnl.c` | **Found.** Line 1627 reads `sizeof(struct virtchnl_queue_vector) * (chunk_sz - 1)` |
| 0014 | the offload advertisement is gated on `hw->phy_model == ICE_PHY_E830` | `drivers/net/intel/ice/ice_ethdev.c` | **Found.** Line 4668 gates `RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP` on the E830 model |

A patch also proves itself: `patch -p1 --dry-run` on the v26.07 tree fails on an
already-applied patch. A dry-run failure alone is not proof, because a context
change also fails. Use both signals. Every dry run below ran against the pristine
tree, one patch at a time.

| Patch | Dry run on v26.07 | Verdict |
|---|---|---|
| 0001 | **Fail** — "Reversed (or previously applied) patch detected" | Drop confirmed |
| 0002 | **Fail** — 3 of 3 hunks failed | Drop confirmed |
| 0003 | **Pass** — "Hunk #1 succeeded at 1100 (offset 13 lines)." | Drop — §6. A pass here does not mean keep |
| 0004 | Pass, offset 44-51 lines | Keep confirmed |
| 0005 | Pass, offset 20 lines | Keep confirmed |
| 0006 | Pass, no offset | Keep confirmed |
| 0007 | **Fail** — the `VERSION` hunk failed. The `config/meson.build` hunk passed | Keep. T-02 refreshed it and shipped it as 26.07 `0004` |
| 0008 | **Fail** — "Reversed (or previously applied) patch detected" | Drop confirmed |
| 0009 | Pass, no offset | Keep confirmed |
| 0010 | Pass, offset 106 lines | Keep confirmed |
| 0011 | Pass, offset 1 line | Keep confirmed |
| 0012 | Pass. `patch` also warns "unexpectedly ends in middle of line", because the patch file has no final newline | Keep confirmed |
| 0013 | Pass, offset 106 lines | Keep confirmed |
| 0014 | **Fail** — "Reversed (or previously applied) patch detected" | Drop confirmed |
| hdr_split/0001 | Pass, offsets from -410 to 53 lines | Keep confirmed |
| windows/0001 | **Fail** — 2 hunks on `app/test/meson.build` failed, and 2 hunks on `pcap_osdep_windows.c` are already applied. The other 7 files passed, but hunk #2 on `lib/eal/windows/eal.c` needed fuzz 2 | Keep. T-02 refreshed it and shipped it as 26.07 `windows/0001` |

Totals: 10 pass, 6 fail, against an outcome of 11 keep and 5 drop.

The 9 kept patches that pass on their own also apply as a set. A real
`patch -p1` of `0004`, `0005`, `0006`, `0009`–`0013` and `hdr_split/0001`, in that
order, on one copy of the tree, gave no conflict.

Three results differed from the plan when T-01 measured them. Only `windows/0001` is closed
as a §3 divergence, and §8 keeps that file open for a separate defect. `0003` shipped no
hunks at all and defers the replacement field to T-04. `0007` shipped refreshed hunks and
defers the version-string conflict to T-03. Each bullet keeps
its evidence — for `0007` and `windows/0001` it records why the shipped hunks look the way
they do, and for `0003` it is the only record of a row that shipped nothing.

* **`0003` passes the dry run and is dropped anyway.** The grep is the only evidence for
  this row. Upstream superseded MTL `0003` rather than merged it. It added the
  `rl_burst_size` devarg instead, so `ICE_SCHED_DFLT_BURST_SIZE` in
  `drivers/net/intel/ice/base/ice_type.h` still reads `(15 * 1024)` in 26.07. The drop
  therefore also drops the 2 KB compile-time default, and MTL replaces it from its
  own side — §6. The rule "a dropped patch fails the dry run" does not hold for a patch
  that upstream superseded rather than merged.
* **`0007` failed on context, and the fix left a version-string conflict for T-03.** The
  26.03 patch rewrites `VERSION` from `26.03.0` to `26.03.91_mtl_`. The 26.07 file reads
  `26.07.0`, so both the context line and the target string were wrong for this release.
  T-02 refreshed the patch as
  [0004](patches/dpdk/26.07/0004-config-add-mtl-version-to-version-string.patch), which
  writes the literal `26.07.0_mtl_`. **The earlier instruction to derive that string from
  `DPDK_VER` and `DPDK_MTL_MINOR_VER` is withdrawn** — the shipped patch hardcodes it, and
  nothing in the patch reads [versions.env](versions.env). The two sides still disagree
  today: [versions.env:2](versions.env) reads `DPDK_MTL_MINOR_VER=91`, while
  [script/build_dpdk.sh:66](script/build_dpdk.sh) tests the installed `pkg-config`
  version against the composed prefix `${DPDK_VER}.${DPDK_MTL_MINOR_VER}_mtl_`. With
  `DPDK_VER=26.07` that asks for `26.07.91_mtl_`, and the patched build produces
  `26.07.0_mtl_`. `dpdk_is_installed()` therefore never matches, and every
  `build_dpdk.sh` invocation rebuilds DPDK in full instead of skipping. Task **T-03**
  owns the reconciliation, by moving `DPDK_MTL_MINOR_VER` to `0`.
* **`windows/0001` failed for two different reasons at once.** The
  `app/test/meson.build` hunks comment out the two `test_pcapng.c` entries. 26.07
  changed the first line and added a sibling line after the second. Line 139 now reads
  `'test_pcapng.c': ['net_null', 'net', 'ethdev', 'pcapng', 'bus_vdev'],`, so the
  dependency list gained `net_null` and reordered `net` against `ethdev`. Line 224
  `'test_pcapng.c': ['pcap'],` is byte-identical to the patch target, and that hunk
  failed on the following context instead — the new line 225
  `'test_pmd_pcap.c': ['pcap'],`. Both failures were context drift, and T-02 redid the
  hunks against 26.07. The `pcap_osdep_windows.c` hunks deleted a trailing `\n` from four
  `PMD_LOG` calls, and 26.07 had already dropped those newlines, so T-02 removed those 2
  hunks. Both outcomes are visible in the shipped file:
  `grep -c pcap_osdep_windows patches/dpdk/26.07/windows/0001.patch` returns 0, and the
  `app/test/meson.build` hunk carries the `net_null` form of line 139.

## 4. What MTL keeps, and why

Four groups. Numbers below are 26.03 numbers. The last group also uses 26.07 numbers, and
marks each one.

**Rejected on design (2 patches).** Only a change inside `lib/` removes these. See
§10.

* `0004` drops the PTP ptype filter, so the driver marks every packet as a PTP
  packet. Bruce Richardson called that incorrect, and we agreed
  ([review](https://inbox.dpdk.org/dev/akPjFNVasdvxrAwu@bricha3-mobl1.ger.corp.intel.com)).
* `hdr_split/0001` adds an ethdev callback API. Thomas Monjalon refused it, because
  `RTE_PKTMBUF_POOL_F_PINNED_EXT_BUF` plus buffer split covers the same case
  ([review](https://inbox.dpdk.org/dev/S2CuFF0sSGW_kdQEDd06JQ@monjalon.net)).
  This patch also changes `lib/ethdev/rte_ethdev.{c,h}` and `ethdev_driver.h`, so it
  conflicts on every DPDK bump. It is the most invasive patch MTL carries.

**Accepted upstream but never applied (2 patches).** Patchwork marks both `accepted`.
Neither is in `v26.07`. MTL must keep both.

* `0005` — patchwork [166691](https://patches.dpdk.org/project/dpdk/patch/20260713094240.1721105-3-dawid.wesierski@intel.com/).
* `0006` — patchwork [166396](https://patches.dpdk.org/project/dpdk/patch/20260629093942.983145-1-dawid.wesierski@intel.com/). See §7.

**MTL-local by design (2 patches).** `0007` adds the MTL version to the DPDK version
string, which [script/build_dpdk.sh:57-70](script/build_dpdk.sh) then reads to decide
whether a rebuild is needed. `windows/0001` carries the Windows fixups.

**Never submitted (5 patches).** `0009`–`0013` are genuine `net/ice` E830 and TxPP
fixes, renumbered `0005`–`0009` in the 26.07 set. They stay out of tree now that MTL
sends nothing upstream. Their author metadata is settled except in one file. Enumerate it
with `grep -nE '^(From:|Signed-off-by:)' patches/dpdk/26.07/000[5-9]*.patch`: 26.07 `0005`
and `0006` name Soumyadeep Hore on both lines, 26.07 `0007` and `0008` name Marek Kasiewicz
on both. Only 26.07 [0009](patches/dpdk/26.07/0009-net-ice-always-init-PHC-owner.patch) still
reads `MTL Contributor <noreply@example.com>`, as `From:` and as `Signed-off-by:`. §8
records why the placeholder stays for now, and task **T-31** owns it.

## 5. MTL `0002` carries an inverted guard — the bump deletes the bug

[patches/dpdk/26.03/0002-net-iavf-refine-queue-rate-limit-configure.patch](patches/dpdk/26.03/0002-net-iavf-refine-queue-rate-limit-configure.patch):

```c
if (vf->tm_conf.nb_tc_node != 1 &&
    vf->qos_cap->num_elem != 1 &&
    adapter->stopped != 1) {
```

Upstream `e04c0fa68c`:

```c
if ((vf->tm_conf.nb_tc_node != 1 || vf->qos_cap->num_elem != 1) &&
    adapter->stopped != 1) {
```

Two defects, both fixed upstream:

* `&&` where `||` belongs. The MTL version demands a stopped port only when **both**
  counts differ from 1. So `nb_tc_node == 1` with `num_elem == 5` on a running port
  passes a guard that must stop it.
* The MTL version reads `vf->qos_cap` above the `VIRTCHNL_VF_OFFLOAD_QOS` capability
  check. Upstream moved the guard below that check, so the pointer is known valid.

**No work item.** An earlier plan had a task to backport the fix into the MTL copy of
`0002`. The bump drops `0002`, so the fix arrives with 26.07. Do not backport it.
This is recorded so that the missing task does not look like an oversight.

## 6. Burst size — `0003` is dropped, and T-04 carries the burst from `lib/`

**Decision, 2026-08-24.** DPDK patch `0003` is dropped. MTL carries the burst size as an
`rl_burst_size` field in `struct mtl_port_init_params`, which `dev_eal_init()` appends as
the `rl_burst_size` devarg on the port BDF. The field is opt-in, and a caller sets it
only for a port that DPDK drives as an ice **PF**: the devarg must never reach a VF,
because `iavf` rejects an unknown key and the probe fails `-EINVAL`. The 26.07 set is
therefore 11 files, and that count is final. Task **T-04** owns the field and its unit
test.

The rest of this section is the evidence behind that decision. MTL patches two burst-size
defaults, not one:

| Patch | Target | Who uses it |
|---|---|---|
| [patches/dpdk/26.03/0003-ice-set-ICE_SCHED_DFLT_BURST_SIZE-to-2048.patch](patches/dpdk/26.03/0003-ice-set-ICE_SCHED_DFLT_BURST_SIZE-to-2048.patch) | `drivers/net/intel/ice/base/ice_type.h` in DPDK | the DPDK `ice` PMD, and only when DPDK drives the ice **PF** |
| [patches/ice_drv/2.6.6/0002-ice-reduce-TX-scheduler-default-burst-size-to-2-KB.patch](patches/ice_drv/2.6.6/0002-ice-reduce-TX-scheduler-default-burst-size-to-2-KB.patch) | `ICE_SCHED_DFLT_BURST_SIZE` in the kernel ICE module | the kernel PF, which programs every VF rate limiter |

MTL sets its rate limit through `rte_tm` on the port it owns —
`dev_rl_shaper_add()` at [lib/src/dev/mt_dev.c:599](lib/src/dev/mt_dev.c). In the
normal deployment that port is a **VF**, so `iavf` sends a virtchnl message and the
**kernel** PF programs the scheduler. The kernel patch supplies the 2 KB burst in
that path. In that path the DPDK patch does not take part.

So the earlier claim — "dropping `0003` silently regresses narrow-sender pacing" —
holds only for a deployment where DPDK owns the ice PF. That deployment is real, which is
why the field is needed, and it is narrow, which is why the field is opt-in.

The build site for the devarg is
[lib/src/dev/mt_dev.c:398](lib/src/dev/mt_dev.c):

```c
dev_build_pci_devarg(p, i, port_param, sizeof(port_params[i]));
```

That call is the only place MTL builds the `-a <BDF>` argument for an
`MTL_PMD_DPDK_USER` port. It sits inside `static int dev_eal_init()` at
[lib/src/dev/mt_dev.c:320](lib/src/dev/mt_dev.c), which no unit test can enter because
it runs `rte_eal_init()`. The string builder is therefore split out as
`dev_build_pci_devarg()` at [mt_dev.c:310](lib/src/dev/mt_dev.c), and the unit test
calls that directly.

Upstream behaviour of the devarg, for reference: `-a 80:00.0,rl_burst_size=2048`.
Default `0` keeps the 15 KB hardware default. An out-of-range value is a hard
failure — `ice_cfg_rl_burst_size()` fails in `ice_dev_init()`, and the probe returns
`-EINVAL`. This is the only item in the whole effort that got a 26.07 release note.

**Task T-04 part one — measured evidence, 2026-08-24.** Part one read the MTL tree and
the pristine v26.07 tree. It sharpens the VF-path analysis above. MTL ships,
documents and tests a configuration where DPDK drives an ice **PF**, so the DPDK patch
is not irrelevant. Every citation below was checked against the tree.

| Finding | Evidence |
|---|---|
| The `net_ice` driver entry is a PF entry that asks for TM rate limiting | [lib/src/dev/mt_dev.c:29-35](lib/src/dev/mt_dev.c) declares `.port_type = MT_PORT_PF` with `.rl_type = MT_RL_TYPE_TM`. Compare `net_iavf` at [mt_dev.c:42-49](lib/src/dev/mt_dev.c), which declares `MT_PORT_VF` and the same `rl_type` |
| `AUTO` pacing selects RL on a PF | The `ST21_TX_PACING_WAY_AUTO` block at [mt_dev.c:1452-1462](lib/src/dev/mt_dev.c) tests `rl_type == MT_RL_TYPE_TM` at line 1454 and nothing else. No PF or VF condition takes part |
| The PF TM hierarchy is deliberate code, not an accident | [mt_dev.c:556-557](lib/src/dev/mt_dev.c) sets `ST_TM_NONLEAF_NODES_NUM_PF 7` against `..._VF 2`. `MT_DRV_IAVF` branches at [:579](lib/src/dev/mt_dev.c), [:671](lib/src/dev/mt_dev.c), [:739](lib/src/dev/mt_dev.c) and [:1479](lib/src/dev/mt_dev.c) choose between the two shapes |
| Nothing rejects a PF BDF | `lib/` holds no PCI device-id table. The `bind_pmd` command at [script/nicctl.sh:216-224](script/nicctl.sh) checks neither vendor nor VF presence |
| A PF-with-RL case runs today | [tests/acceptance/tests/single/st20p/test_pacing_way.py:50-51](tests/acceptance/tests/single/st20p/test_pacing_way.py) crosses `interface_type` in `("VF", "PF")` with `pacing_way` in `("auto", "rl", ...)`. The suite carries `@pytest.mark.nightly` at line 55 |
| In 26.07 the PF path still reads the compile-time burst default | `drivers/net/intel/ice/base/ice_type.h:1103` still defines `ICE_SCHED_DFLT_BURST_SIZE` as `(15 * 1024)`. `ice_init_hw()`, which spans `drivers/net/intel/ice/base/ice_common.c:1049-1212`, applies it at lines 1160-1161. `ice_sched.c:4132` then copies `hw->max_burst_size` into every rate-limit profile |
| The VF path does not read it | `drivers/net/intel/iavf/` holds no `burst_size` reference at all. This confirms the VF analysis above |
| `rte_tm` offers no runtime route to the burst | `drivers/net/intel/ice/ice_tm.c:316-327` and `drivers/net/intel/iavf/iavf_tm.c:492-503` both reject `committed.size` and `peak.size` with `-EINVAL`. MTL cannot set the burst through the shaper profile it already builds |
| The devarg must never reach a VF | `drivers/net/intel/iavf/iavf_ethdev.c:2476-2480` runs `rte_kvargs_parse` against `iavf_valid_args` at `:53-63`, which holds no `rl_burst_size` key. An unknown key fails the probe with `-EINVAL` |
| The per-port devarg field is new | `dev_eal_init()` builds the whole EAL argv itself. `struct mtl_port_init_params` at [include/mtl_api.h:542-558](include/mtl_api.h) carried only `flags` and `socket_id` until this work appended `rl_burst_size` |

**The new field is ABI-neutral in size.** `uint64_t flags` sits at offset 0 and
`int socket_id` at offset 8, which left 4 bytes of tail padding. `uint32_t rl_burst_size`
lands in that padding at offset 12, so `sizeof(struct mtl_port_init_params)` stays 16 and
no member of `port_params[MTL_PORT_MAX]` at
[include/mtl_api.h:733](include/mtl_api.h) shifts. bindgen therefore regenerates the same
layout and needs no version dance. The one added line at
[rust/imtl-sys/examples/no_std.rs:16](rust/imtl-sys/examples/no_std.rs) is a separate fact
and not a consequence of that: it is an example rather than the binding, and Rust struct
literals are exhaustive, so any new field forces a line there whatever the layout does. Every
downstream Rust caller that builds this struct without `..Default::default()` needs one too.

Three points bound what the evidence carries into the decision.

1. A PF-with-rate-limit configuration exists in the tree, so dropping `0003` is not a
   no-op. The `rl_burst_size` field is what makes it one.
2. One hardware measurement must still confirm that MTL's hard-coded 7-level PF TM
   hierarchy commits. 26.07 validates node depth against `hw->num_tx_sched_layers` at
   `drivers/net/intel/ice/ice_tm.c:509`. If the hardware rejects MTL's tree, the port
   falls back to TSC. The warning reads "fallback to tsc as rl init fail" at
   [mt_dev.c:1487](lib/src/dev/mt_dev.c). The burst size then stops mattering, which
   bounds how much the field can buy — it does not reopen the drop.
3. The field wins over the two alternatives that were weighed against it, a sysfs probe
   and keeping `0003` as a 12th carried patch. A carried patch pays a rebase on every
   DPDK bump for a default that upstream now exposes at runtime.

**Testing gap.** [.github/scripts/gtest.sh:107](.github/scripts/gtest.sh) and
[gtest.sh:114-116](.github/scripts/gtest.sh) are the only lines that pass
`--pacing_way`, and they pass `auto` and `tsc` only. No gtest passes `rl`, so no gtest
can catch a PF rate-limit burst regression.

## 7. pcapng — 26.07 is safe, the next bump is not

The 26.03 number `0006` is
[0003](patches/dpdk/26.07/0003-pcapng-add-user-timestamp-support.patch) in 26.07. This
section calls it `0006` where it records review history, and `0003` where it records the
shipped file.

[lib/src/mt_pcap.c:85](lib/src/mt_pcap.c) calls `rte_pcapng_copy_ts()`. That symbol
exists only in MTL patch `0006`. Upstream rejected that shape twice and accepted a
different one: `rte_pcapng_copy()` gains a `uint64_t timestamp` parameter through ABI
versioning, plus a new `rte_pcapng_tsc_to_ns()` helper.

The accepted version is **not in `v26.07`**. So 26.07 is not a build break, and MTL keeps
`0003`'s diff hunks unchanged — they are byte-identical to the 26.03 ancestor's. Its mail
headers did change. 26.03 line 2 read `From: "Kasiewicz, Marek" <marek.kasiewicz@intel.com>`
over the single trailer `Signed-off-by: Kasiewicz, Marek <marek.kasiewicz@intel.com>`; 26.07
`0003` line 2 reads `From: Frank Du <frank.du@intel.com>` over
`Signed-off-by: Frank Du <frank.du@intel.com>` and
`Signed-off-by: Marek Kasiewicz <marek.kasiewicz@intel.com>` — §8 records why. The break
arrives with the DPDK release that applies the v6 patch.

**The symptom is not a compile error.**
[lib/src/mt_pcap.h:34-57](lib/src/mt_pcap.h) is the `#else` arm of
`#ifdef MTL_DPDK_HAS_PCAPNG_TS`. It supplies stubs that return `NULL` and `-ENOTSUP`, and
[lib/src/mt_pcap.c:10](lib/src/mt_pcap.c) wraps the real code in `#ifdef MT_HAS_PCAPNG_TS`.
So MTL compiles against an unpatched DPDK, and pcap capture degrades at runtime instead.
Each open logs `no pcap support for this build` and returns `NULL`. An earlier symptom can
also appear: [script/build_dpdk.sh:99](script/build_dpdk.sh) applies the set with
`patch -p1`, and that step aborts when the patch stops applying. That script is the only
caller that applies the DPDK set with `patch`. The four by-hand flows use `git am`
([doc/build.md:155](doc/build.md), [doc/build_WIN.md:82](doc/build_WIN.md),
[doc/experimental/header_split.md:20](doc/experimental/header_split.md),
[.github/workflows/msys2_build.yml:135](.github/workflows/msys2_build.yml)), so a reader who
follows them sees the runtime symptom only. Both symptoms are a forecast, not a
measurement. On v26.07 today `0006` still applies clean with no offset — §3.

All six `patches/dpdk/*/` copies of this patch add `MTL_DPDK_HAS_PCAPNG_TS` in the same
`rte_pcapng.h` hunk that declares `rte_pcapng_copy_ts()` — line 96 of the shipped
[26.07 `0003`](patches/dpdk/26.07/0003-pcapng-add-user-timestamp-support.patch).
The define arrives inside the installed `rte_pcapng.h`, not through a meson feature
check and not through a `-D` flag. So the define always travels with the symbol.

Task **T-09** records this as a watch item with the exact symptom, so the next person
does not debug it from zero. That watch item also landed in the code:
[lib/src/mt_pcap.h:13-16](lib/src/mt_pcap.h) carries a comment that names both patch shapes
and points back at this section.

## 8. Carried patch metadata — what the 26.07 set says now

The 26.03 numbers `0011`, `0012` and `0013` are `0007`, `0008` and `0009` in 26.07.

Line 1 of every header-bearing file in the set is now
`From nobody Mon Sep 17 00:00:00 2001`. That is the nine flat patches plus
[hdr_split/0001](patches/dpdk/26.07/hdr_split/0001-net-intel-ice-support-hdr-split-mbuf-callback.patch)
— ten files, not nine. No line-1 hash was worth keeping: a `git format-patch` ID means
nothing after a rebase, and `head -1` on the 26.03 ancestors of `0007`, `0008` and `0009`
returns `a1b2c3d4e5f60718293a4b5c6d7e8f9011223344`,
`0000000000000000000000000000000000000002` and 40 zeros. `From nobody` is git's own
canonical placeholder: it asserts nothing, and the `git am` run at the end of this section
proves it is accepted.
[windows/0001.patch](patches/dpdk/26.07/windows/0001.patch) starts at `diff --git` with no
mail header, so the rule does not reach it. That shape also decides which flow can apply the
file: `git apply --check` exits 0, and `git am` exits 128 with "Patch format detection
failed." So [doc/build_WIN.md:86](doc/build_WIN.md) works and
[.github/workflows/msys2_build.yml:136](.github/workflows/msys2_build.yml) cannot. This is no
live CI break — the defect is inherited from 26.03, and the msys2 matrix is `[25.03, 23.11]`
([msys2_build.yml:46](.github/workflows/msys2_build.yml)), so CI never applies the 26.07 set.
Task **T-30** owns the file and the call site.

Three files were singled out in earlier rounds. Of the three, only `0009` has a known defect
past line 1. Confirm each with `grep -nE '^(From:|Signed-off-by:)'`:

* [0007](patches/dpdk/26.07/0007-net-ice-fix-TxPP-launch-time-encoding-for-19-bit-HW-f.patch)
  — its `From:` names Marek Kasiewicz. Nothing else in its header is known wrong, but its
  `Fixes: 0b6ff09a1f19` on line 30 is not cleared either. §3 records that no DPDK git tree
  exists on this host, so that hash, the same hash on `0005:15`, and `327fe144ca39` on
  `0006:16` are all unverified here. None of the three hashes is claimed wrong. `0b6ff09a1f19`
  appears independently in two patches, which is weak evidence that it is real.
* [0008](patches/dpdk/26.07/0008-net-ice-e830-use-direct-MMIO-for-PHC-update.patch)
  — clean past line 1. It carries no `Fixes:` line that could be wrong.
* [0009](patches/dpdk/26.07/0009-net-ice-always-init-PHC-owner.patch)
  — `From: MTL Contributor <noreply@example.com>` on line 2, and the same placeholder as
  `Signed-off-by:` on line 30. This is the only file in the set with a placeholder
  identity.

`0009` keeps its placeholder author because no author is recoverable.
`ice_timesync_find_src_tmr_owner` is absent from a pristine 26.07 tree (`grep -rq` exits
1), the body carries no attribution tag (`grep -cE '^(Fixes|Cc|Reviewed-by|Acked-by):'`
returns 0), and the sole commit that added the file
(`git log --all --diff-filter=A -- 'patches/dpdk/*always-init-PHC-owner.patch'` returns
`168b785a`) bundles the committer's own patches with other people's: 26.03 `0011` and
`0012` name the committer Marek Kasiewicz as author, while `0009` and `0010` name
Soumyadeep Hore. Committer identity is therefore evidence for neither, and no `From:` may
be taken from the author of the commit that added a patch file.

`0009` also keeps line 30. `From:` is an attribution, but `Signed-off-by:` is a DCO
certification, and a visible placeholder certifies nothing while a real name would.
Deleting the line would leave a DPDK patch with no sign-off at all, so the line stays and
task **T-31** carries the decision, which needs a person who can certify the change.

§7 quotes `0003`'s header change; this is why it is legitimate. The 26.03 ancestor's
`"Kasiewicz, Marek"` was wrong: the 23.11 copy of the same patch carries the original
author, and
`git show '659ebc82:patches/dpdk/23.11/0007-pcapng-add-user-timestamp-support.patch' | head -6`
prints `From: Frank Du <frank.du@intel.com>` with a matching `Signed-off-by:`. `0003` took
that `Signed-off-by: Frank Du` from the 23.11 original and kept Marek Kasiewicz's, normalized,
for the rebase — two DCO trailers where 26.03 carried one. Adding a named person's sign-off is
legitimate here only because the content he signed is the content shipped: the 23.11 patch
already carries `rte_pcapng_copy_ts`, the commented-out `uint64_t cycles, timestamp;` and the
`#if 0` block, and `0003`'s diff hunks are byte-identical to its 26.03 ancestor's.

The `index <pre>..<post>` lines across this set are not maintained and must not be relied
on. `windows/` and `hdr_split/` are never applied to the same tree, so each one is measured
after the nine flat patches and nothing else. Compare each `<pre>` blob with
`git hash-object` against the tree the patch meets — a flat patch after its predecessors,
an optional patch after the nine flat patches: `0001`, `0002`, `0005`, `0006`, `0007` and
`0009` are each stale in their one line, and `hdr_split/0001` is stale in all seven of its
own — 13 stale lines in seven of the 11 patch files, naming eight distinct DPDK source
files. `windows/0001` is right in all eight of its own and `0004` in both of its own. The
remaining two, `0003` and `0008`, carry no `index` line at all. Plain `patch -p1` and plain
`git am` ignore these lines, so the cost is nil today; `git am -3` does not, and task
**T-21** owns that recovery path.

Header lines are the whole of the metadata work, plus two bytes in `0008`, both inherited
defects of 26.03 `0012`: it gained the final newline that `0012` lacked, which clears the
`patch` warning §3 records, and its git signature separator regained the trailing space
after the two dashes that the other eight flat patches already had. Check that separator
with `grep -c '^-- $'`, which now returns 1 for all nine. No diff hunk changed as part of the
metadata work. The bodies that do differ from 26.03 — `0004` and `windows/0001` — differ
because the 26.07 rebase regenerated them.

Why any of this matters in a tree that never posts upstream: `patch -p1` ignores the
`From:` line, `git am` does not, and four documented flows apply the flat glob with `git am` —
[.github/workflows/msys2_build.yml:135](.github/workflows/msys2_build.yml),
[doc/build.md:155](doc/build.md), [doc/build_WIN.md:82](doc/build_WIN.md) and
[doc/experimental/header_split.md:20](doc/experimental/header_split.md). `0009` therefore
still commits as `MTL Contributor <noreply@example.com>` in every tree built that way, and
task **T-31** carries that open item. The other eight flat patches now commit under a real
author — `git am` of the eight in order names Marek Kasiewicz, Ric Li, Frank Du, Dawid
Wesierski and Soumyadeep Hore. Drop the `Cc: stable@dpdk.org` lines instead of adding more —
they are meaningless in a tree that never posts.

## 9. Stale DPDK version references across the tree

**Dated snapshot: 2026-08-24.** This round closed four of the six rows. Re-grep a row
before you trust it. Tasks **T-10** and **T-16** in [tasks.md](tasks.md) read this table.

| Location | Said | State after this round |
|---|---|---|
| `.github/workflows/validation-tests.yml` | `DPDK_VERSION: '25.11'` | **Closed.** `grep 25.11` returns nothing |
| [.github/workflows/msys2_build.yml:46](.github/workflows/msys2_build.yml) | matrix `[25.03, 23.11]` | **Open.** Still two hard-coded versions |
| `doc/build.md` | `patches/dpdk/25.11/` | **Closed.** The path reads `patches/dpdk/${DPDK_VER}/`. `grep 25.11` returns nothing |
| `doc/build_WIN.md` | `patches/dpdk/25.11/` | **Closed.** Same change. `grep 25.11` returns nothing |
| [doc/design.md:671](doc/design.md) | pin `DPDK_VER=25.11` | **Not stale — deliberate exception.** [doc/design.md:664](doc/design.md) and [:670](doc/design.md) mark the pin a deliberate Ubuntu 22.04 override |
| `doc/experimental/header_split.md` | `patches/dpdk/23.03/` | **Closed.** `grep -o '2[0-9]\.[0-9][0-9]'` returns 1 literal, the `23.03` verification pin, on line 11 |

One row stays open. Task **T-10** removes the duplication instead of editing the
numbers.

## 10. The two patches that only `lib/` work can remove

Neither is a 26.07 task. Both are the only route to a smaller patch set after 26.07.

**`0004` → `RTE_ETH_RX_OFFLOAD_TIMESTAMP`.** The offload flag timestamps every packet
and does not add the false PTP mark. Bruce named it in review, we tested it during
the review, and it worked: *"I am testing the 'proper' way and it seems to be
working, will drop this patch from the series."*
([thread](https://inbox.dpdk.org/dev/20260703200128.1364461-1-dawid.wesierski@intel.com))

**`hdr_split/0001` → `RTE_PKTMBUF_POOL_F_PINNED_EXT_BUF` plus buffer split.** The MTL
side is `ST20_RX_FLAG_HDR_SPLIT`, `mt_if_hdr_split_pool()` and
[doc/experimental/header_split.md](doc/experimental/header_split.md).

## 11. Why these patches will never be upstream

Kept as a short note, because it explains §4 and §10. It is not guidance for a post
any more.

* **Bruce Richardson** (`net/intel`) prefers runtime capability negotiation over a
  device argument, and a hard probe failure over a log line. He rejected `0004` on
  correctness.
* **Stephen Hemminger** (`lib/pcapng`) treats ABI stability as non-negotiable and
  refuses a `_ts` or `_ex` sibling function where symbol versioning works. That is
  why MTL's `0006` shape was rejected and the accepted shape differs.
* **Thomas Monjalon** (`lib/ethdev`) refuses a new API when a generic mechanism
  already covers the case. That is why `hdr_split/0001` was refused.

## 12. Series timeline — provenance only

Kept because it is the only record of which patchwork series produced the outcomes in
§2. This file was untracked until now, so git history holds nothing earlier. Every
series was posted by Dawid Wesierski to the `dpdk` patchwork project.

| Date | Series | Patchwork | Result |
|---|---|---|---|
| 2026-06-08 | v1 `intel network and pcapng updates` (7) | [38373](https://patches.dpdk.org/project/dpdk/list/?series=38373) | changes-requested |
| 2026-06-18 | v2 `Intel network drivers enhancements` (7) | [38487](https://patches.dpdk.org/project/dpdk/list/?series=38487) | changes-requested |
| 2026-06-18 | v3 `pcapng: add user-supplied timestamp support` (1) | [38486](https://patches.dpdk.org/project/dpdk/list/?series=38486) | changes-requested |
| 2026-06-23 | v4 pcapng | [38546](https://patches.dpdk.org/project/dpdk/list/?series=38546) | changes-requested |
| 2026-06-24 | v5 pcapng | [38573](https://patches.dpdk.org/project/dpdk/list/?series=38573) | superseded |
| 2026-06-29 | v6 pcapng | [38615](https://patches.dpdk.org/project/dpdk/list/?series=38615) | accepted, never applied |
| 2026-06-30 | v3 `Intel network drivers enhancements` (6) | [38623](https://patches.dpdk.org/project/dpdk/list/?series=38623) | 3 accepted, 3 changes-requested |
| 2026-07-03 | v4 `Intel network drivers enhancements` (5) | [38651](https://patches.dpdk.org/project/dpdk/list/?series=38651) | changes-requested |
| 2026-07-08 | v5 `net/iavf,ice: fix runtime queue setup race` (2) | [38706](https://patches.dpdk.org/project/dpdk/list/?series=38706) | changes-requested |
| 2026-07-13 | v6 `Intel network driver enhancements` (2) | [38723](https://patches.dpdk.org/project/dpdk/list/?series=38723) | 1 applied, 1 accepted, never applied |

Two traps in this table. First, the series that merged carries the subject prefix
`[PATCH v3 x/6]`, but the local files were named `v4-*`; patchwork numbering is
authoritative. Second, those local files lived in `/home/labrat/dev1/dpdk`, which
**no longer exists**. Nothing in [tasks.md](tasks.md) may depend on that path.
