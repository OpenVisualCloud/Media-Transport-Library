# DPDK patch set analysis — what 26.07 covers and what MTL keeps

Snapshot date: **2026-08-24**

| Item | Value |
|---|---|
| MTL pinned DPDK | **26.03** — [versions.env:1](versions.env) (`DPDK_VER=26.03`, `DPDK_MTL_MINOR_VER=91`) |
| DPDK installed on this host | `26.03.90_mtl_` — `pkg-config --modversion libdpdk` |
| Target DPDK | **26.07** |
| Patch set today | 16 files in [patches/dpdk/26.03/](patches/dpdk/26.03/) |
| Patch set after the bump | 11 files in `patches/dpdk/26.07/` |

This file is the source record for the work list in [tasks.md](tasks.md). Read the
section that a task names before you start the task.

## 1. Decision

**MTL sends nothing upstream.** The team decided this on 2026-08-24. Every action
that needed a post to `dev@dpdk.org`, a repost, or a maintainer reply is cancelled.

What replaces it:

1. Move MTL to DPDK 26.07.
2. Drop every patch that 26.07 already carries.
3. Keep every patch that 26.07 does not carry. Renumber the set.
4. Add a test at the cheapest tier for each change that can alter behaviour.

The upstream review history stays in this file for one reason only: it is the
evidence for the drop list and for the two patches that MTL keeps forever. It is
not a plan of action any more.

## 2. Patch set today

`patch -p1` applies the flat glob `patches/dpdk/$DPDK_VER/*.patch` —
[script/build_dpdk.sh:98](script/build_dpdk.sh). The `hdr_split/` and `windows/`
subdirectories are applied by hand, per [doc/build.md](doc/build.md) and
[.github/workflows/msys2_build.yml:135](.github/workflows/msys2_build.yml).

| # | Patch file | Upstream state | Action on 26.07 |
|---|---|---|---|
| 0001 | [e810-set-max-ring-desc-to-max-allowed-by-hardware](patches/dpdk/26.03/0001-e810-set-max-ring-desc-to-max-allowed-by-hardware.patch) | Merged `03bee932e9` | **Drop** |
| 0002 | [net-iavf-refine-queue-rate-limit-configure](patches/dpdk/26.03/0002-net-iavf-refine-queue-rate-limit-configure.patch) | Merged `e04c0fa68c`, with a logic fix (§5) | **Drop** |
| 0003 | [ice-set-ICE_SCHED_DFLT_BURST_SIZE-to-2048](patches/dpdk/26.03/0003-ice-set-ICE_SCHED_DFLT_BURST_SIZE-to-2048.patch) | Superseded by the `rl_burst_size` devarg, `b3f2afb3b7` + `74dc5eb5c7` | **Drop**, see §6 first |
| 0004 | [Change-to-enable-PTP](patches/dpdk/26.03/0004-Change-to-enable-PTP.patch) | Rejected on correctness | Keep → new `0001` |
| 0005 | [iavf-disable-runtime-queue](patches/dpdk/26.03/0005-iavf-disable-runtime-queue.patch) | Acked, never applied | Keep → new `0002` |
| 0006 | [pcapng-add-user-timestamp-support](patches/dpdk/26.03/0006-pcapng-add-user-timestamp-support.patch) | Approved, never applied | Keep → new `0003`, see §7 |
| 0007 | [config-add-mtl-version-to-version-string](patches/dpdk/26.03/0007-config-add-mtl-version-to-version-string.patch) | MTL-local by design | Keep → new `0004` |
| 0008 | [net-iavf-fix-large-VF-IRQ-mapping](patches/dpdk/26.03/0008-net-iavf-fix-large-VF-IRQ-mapping.patch) | Merged `cc58d28b10`, by Anatoly Burakov | **Drop** |
| 0009 | [net-ice-fix-TxPP-timer-association-in-txtime-context](patches/dpdk/26.03/0009-net-ice-fix-TxPP-timer-association-in-txtime-context.patch) | Never submitted | Keep → new `0005` |
| 0010 | [net-ice-fix-read-clock-to-use-PHC-hardware-time](patches/dpdk/26.03/0010-net-ice-fix-read-clock-to-use-PHC-hardware-time.patch) | Never submitted | Keep → new `0006` |
| 0011 | [net-ice-fix-TxPP-launch-time-encoding-for-19-bit-HW-field](patches/dpdk/26.03/0011-net-ice-fix-TxPP-launch-time-encoding-for-19-bit-HW-f.patch) | Never submitted | Keep → new `0007`, see §8 |
| 0012 | [net-ice-e830-use-direct-MMIO-for-PHC-update](patches/dpdk/26.03/0012-net-ice-e830-use-direct-MMIO-for-PHC-update.patch) | Never submitted | Keep → new `0008`, see §8 |
| 0013 | [net-ice-always-init-PHC-owner](patches/dpdk/26.03/0013-net-ice-always-init-PHC-owner.patch) | Never submitted | Keep → new `0009`, see §8 |
| 0014 | [net-ice-gate-send-on-timestamp-offload-to-e830](patches/dpdk/26.03/0014-net-ice-gate-send-on-timestamp-offload-to-e830.patch) | Merged `b87947ed19`, written twice | **Drop** |
| — | [hdr_split/0001-net-intel-ice-support-hdr-split-mbuf-callback](patches/dpdk/26.03/hdr_split/0001-net-intel-ice-support-hdr-split-mbuf-callback.patch) | Rejected, API "not necessary" | Keep, unchanged name, §10 |
| — | [windows/0001](patches/dpdk/26.03/windows/0001.patch) | Never submitted | Keep, unchanged name |

Count: 5 dropped, 11 kept.

## 3. Verification status of the drop list — read this before you drop anything

The six upstream commit hashes above come from an earlier session. That session read
a DPDK git tree at `/home/labrat/dev1/dpdk`. **That path does not exist on this host
any more, and no DPDK git tree exists here.** So the hashes are a record, not a
measurement you can repeat today.

`script/build_dpdk.sh` downloads a **tarball**, not a git clone
([script/build_dpdk.sh:90-97](script/build_dpdk.sh)), so `git merge-base` cannot
answer "is commit X in v26.07". Check the source instead. Task **T-01** does this.
Each drop needs one grep against the unpacked `dpdk-26.07` tree:

| Drop | Evidence to find in the v26.07 source | File in the DPDK tree |
|---|---|---|
| 0001 | `IAVF_MAX_RING_DESC` is `(8192 - 32)`, not `4096` | `drivers/net/intel/iavf/iavf.h` |
| 0002 | the guard reads `(vf->tm_conf.nb_tc_node != 1 \|\| vf->qos_cap->num_elem != 1)` and sits below the `VIRTCHNL_VF_OFFLOAD_QOS` check | `drivers/net/intel/iavf/iavf_tm.c` |
| 0003 | the string `rl_burst_size` is present | `drivers/net/intel/ice/ice_ethdev.c` |
| 0008 | `chunk_sz` no longer counts the inline queue vector | `drivers/net/intel/iavf/iavf_vchnl.c` |
| 0014 | the offload advertisement is gated on `hw->phy_model == ICE_PHY_E830` | `drivers/net/intel/ice/ice_ethdev.c` |

A patch also proves itself: `patch -p1 --dry-run` on the v26.07 tree fails on an
already-applied patch. A dry-run failure alone is not proof, because a context
change also fails. Use both signals.

## 4. What MTL keeps, and why

Four groups.

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
fixes. They stay out of tree now that MTL sends nothing upstream. Their metadata is
still wrong for a patch MTL carries — see §8.

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

## 6. Burst size — the DPDK patch is not the one that matters

MTL patches two burst-size defaults, not one:

| Patch | Target | Who uses it |
|---|---|---|
| [patches/dpdk/26.03/0003-ice-set-ICE_SCHED_DFLT_BURST_SIZE-to-2048.patch](patches/dpdk/26.03/0003-ice-set-ICE_SCHED_DFLT_BURST_SIZE-to-2048.patch) | `drivers/net/intel/ice/base/ice_type.h` in DPDK | the DPDK `ice` PMD, and only when DPDK drives the ice **PF** |
| [patches/ice_drv/2.6.6/0002-ice-reduce-TX-scheduler-default-burst-size-to-2-KB.patch](patches/ice_drv/2.6.6/0002-ice-reduce-TX-scheduler-default-burst-size-to-2-KB.patch) | `ICE_SCHED_DFLT_BURST_SIZE` in the kernel ICE module | the kernel PF, which programs every VF rate limiter |

MTL sets its rate limit through `rte_tm` on the port it owns —
`dev_rl_shaper_add()` at [lib/src/dev/mt_dev.c:589](lib/src/dev/mt_dev.c). In the
normal deployment that port is a **VF**, so `iavf` sends a virtchnl message and the
**kernel** PF programs the scheduler. The kernel patch supplies the 2 KB burst in
that path. The DPDK patch does not take part.

So the earlier claim — "dropping `0003` silently regresses narrow-sender pacing" —
holds only for a deployment where DPDK owns the ice PF. Task **T-04** measures which
case is real before any code is written. Do not add the devarg on the strength of the
old claim.

If the devarg is needed, the build site is
[lib/src/dev/mt_dev.c:388](lib/src/dev/mt_dev.c):

```c
snprintf(port_param, 2 * MTL_PORT_MAX_LEN, "%s", p->port[i]);
```

That line is the only place MTL builds the `-a <BDF>` argument for an
`MTL_PMD_DPDK_USER` port. It sits inside `static int dev_eal_init()` at
[lib/src/dev/mt_dev.c:309](lib/src/dev/mt_dev.c), so a unit test needs the string
builder split out into its own function first.

Upstream behaviour of the devarg, for reference: `-a 80:00.0,rl_burst_size=2048`.
Default `0` keeps the 15 KB hardware default. An out-of-range value is a hard
failure — `ice_cfg_rl_burst_size()` fails in `ice_dev_init()`, and the probe returns
`-EINVAL`. This is the only item in the whole effort that got a 26.07 release note.

## 7. pcapng — 26.07 is safe, the next bump is not

[lib/src/mt_pcap.c:85](lib/src/mt_pcap.c) calls `rte_pcapng_copy_ts()`. That symbol
exists only in MTL patch `0006`. Upstream rejected that shape twice and accepted a
different one: `rte_pcapng_copy()` gains a `uint64_t timestamp` parameter through ABI
versioning, plus a new `rte_pcapng_tsc_to_ns()` helper.

The accepted version is **not in `v26.07`**. So 26.07 is not a build break, and MTL
keeps `0006` as it is. The break arrives with the DPDK release that applies the v6
patch. When that happens, `mt_pcap.c` stops compiling.

Task **T-09** records this as a watch item with the exact symptom, so the next person
does not debug it from zero. No code changes now.

## 8. Carried patches with placeholder metadata

Three patches MTL keeps carry metadata that is wrong:

* [0011](patches/dpdk/26.03/0011-net-ice-fix-TxPP-launch-time-encoding-for-19-bit-HW-f.patch) — a fabricated blob hash.
* [0012](patches/dpdk/26.03/0012-net-ice-e830-use-direct-MMIO-for-PHC-update.patch) — `From: MTL Contributor <noreply@example.com>`, all-zero commit hash, no `Fixes:`.
* [0013](patches/dpdk/26.03/0013-net-ice-always-init-PHC-owner.patch) — the same defects.

This still matters with no upstreaming, for one reason: `patch -p1` ignores the
`From:` line, but `git am` does not.
[.github/workflows/msys2_build.yml:135](.github/workflows/msys2_build.yml),
[doc/build.md:150](doc/build.md) and
[doc/build_WIN.md:76](doc/build_WIN.md) all tell the reader to use `git am`. So
`MTL Contributor <noreply@example.com>` becomes a real commit author in every DPDK
tree built that way, and an all-zero `Fixes:` hash points at nothing. Task **T-08**
fixes the metadata. Drop the `Cc: stable@dpdk.org` lines instead of adding more —
they are meaningless in a tree that never posts.

## 9. Stale DPDK version references across the tree

Found by grep, all of them older than the pinned 26.03:

| Location | Says | Should say |
|---|---|---|
| [.github/workflows/validation-tests.yml:109](.github/workflows/validation-tests.yml) | `DPDK_VERSION: '25.11'` | read [versions.env](versions.env) |
| [.github/workflows/msys2_build.yml:46](.github/workflows/msys2_build.yml) | matrix `[25.03, 23.11]` | the pinned version |
| [doc/build.md:150](doc/build.md) | `patches/dpdk/25.11/` | the pinned version |
| [doc/build_WIN.md:76](doc/build_WIN.md) | `patches/dpdk/25.11/` | the pinned version |
| [doc/design.md:671](doc/design.md) | pin `DPDK_VER=25.11` | the pinned version |
| [doc/experimental/header_split.md:16](doc/experimental/header_split.md) | `patches/dpdk/23.03/` | the pinned version |

CI validation therefore builds a **different** DPDK from the one `versions.env` pins,
and it passes only because `patches/dpdk/25.11/` still exists. Task **T-10** removes
the duplication instead of editing six numbers.

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
