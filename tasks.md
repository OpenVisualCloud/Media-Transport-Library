# Tasks

Work list for the DPDK 26.07 move. One task per `##` heading. Status is one of
`OPEN`, `IN PROGRESS`, `BLOCKED`, `DONE`. Keep every note to one line — the history
belongs in `git log`.

The source record is [upstreaming.md](upstreaming.md). Read the section a task names
before you start the task.

Prose in this file is Simplified Technical English. Use the `mtl-ste-writing` skill.

## Decisions — locked with the user, 2026-08-24

| # | Decision | Consequence |
|---|---|---|
| D1 | **MTL sends nothing upstream.** No post to `dev@dpdk.org`, no repost, no maintainer ping. | Every upstreaming task is cancelled. See `## Cancelled`. Nothing in this list waits on a person outside this machine. |
| D2 | **Target DPDK 26.07.** | 5 of 16 patches are dropped. 11 are kept and renumbered into `patches/dpdk/26.07/`. |
| D3 | **Drop everything 26.07 covers, keep nothing "just in case".** | A patch stays only when the v26.07 source proves it is absent. T-01 supplies that proof. |
| D4 | **Every behaviour change gets a test at the cheapest tier that can catch it.** | Unit for string and logic changes, integration for pacing and PTP, acceptance for the end-to-end path. Use the `mtl-write-test` skill to pick the tier. |
| D5 | **`patches/dpdk/26.03/` stays in the tree.** | The maint branches and a rollback need it. The bump adds a directory; it does not move one. |
| D6 | **Branch `dpdk-26.07`.** | Off `checkpath` HEAD. `origin/dev` is shared and stays untouched. |

## Order of work

1. **T-01** and **T-05** run at the same time. T-01 reads a downloaded source tree.
   T-05 needs the hardware and must capture the baseline **before** the bump.
2. **T-10** can also run now. It touches only CI and documentation, and it does not
   build, so it does not race `build/` with T-01.
3. **T-02** needs T-01. **T-03** needs T-02. **T-04** decides before T-03 lands.
4. **T-06** needs T-03 and T-05. **T-07** needs T-03.
5. **T-08** and **T-09** are small and independent. Fit them anywhere.
6. **T-11** and **T-12** are the long tail. They are the only way to shrink the patch
   set below 11, and neither is a 26.07 task.

Only one `mtl-developer` builds at a time — one `build/` tree, one `/usr/local`. Only
one `mtl-system-admin` runs at a time — one set of VFs, one MtlManager.

## T-01 Prove the drop list against a real DPDK 26.07 tree — OPEN

- **Owner:** mtl-developer
- **Ref:** upstreaming.md §3
- **Files:** upstreaming.md (§2 and §3 only). Read-only elsewhere.
- **Acceptance:** the five greps in §3 pasted with their output, plus
  `patch -p1 --dry-run` output for all 16 patches against an unpacked `dpdk-26.07`.
  The 5 dropped patches must fail the dry run, because 26.07 already applies them.
  The 11 kept patches must pass it.
- **Test tier:** none. No MTL code changes.
- **Gates:** 2 exempt (documentation); 5 required; 6 exempt (no data-plane change).
- **Note:** the six upstream commit hashes in §2 were read from a DPDK git tree at
  `/home/labrat/dev1/dpdk` that no longer exists, so they are a record and not a
  measurement. No DPDK git tree exists on this host, and `script/build_dpdk.sh`
  downloads a tarball, so `git merge-base` cannot answer the question. Check the
  source. A patch that fails `--dry-run` is not proof on its own — a context change
  fails the same way — so pair each dry-run with its grep.

## T-02 Create `patches/dpdk/26.07/` with the 11 kept patches — OPEN

- **Owner:** mtl-developer
- **Needs:** T-01
- **Ref:** upstreaming.md §2 and §4
- **Files:** `patches/dpdk/26.07/` (new), `patches/dpdk/26.07/hdr_split/`,
  `patches/dpdk/26.07/windows/`
- **Acceptance:** `for p in patches/dpdk/26.07/*.patch; do patch -p1 --dry-run -i $p; done`
  clean on a fresh `dpdk-26.07` tree, then the same for `hdr_split/` and `windows/`.
- **Test tier:** none. Patch files only.
- **Gates:** 2 exempt (no MTL code); 5 required; 6 exempt.
- **Note:** renumber `0004→0001`, `0005→0002`, `0006→0003`, `0007→0004`, `0009→0005`,
  `0010→0006`, `0011→0007`, `0012→0008`, `0013→0009`; keep `hdr_split/0001` and
  `windows/0001` under their current names. `script/build_dpdk.sh:98` applies a flat
  `*.patch` glob, so name order is apply order, and the subdirectories are applied by
  hand. Copy the files; do not move them (D5).

## T-03 Bump `versions.env` to DPDK 26.07 — OPEN

- **Owner:** mtl-developer
- **Needs:** T-02, and the T-04 decision
- **Ref:** upstreaming.md §2
- **Files:** [versions.env](versions.env), [script/build_dpdk.sh](script/build_dpdk.sh)
  if the version gate needs it
- **Acceptance:** `./script/build_dpdk.sh -f`, then `pkg-config --modversion libdpdk`
  reports `26.07.<minor>_mtl_`, then `./build.sh` green, then `./build.sh unit` green.
- **Test tier:** unit — the whole suite must stay green across the bump.
- **Gates:** 2 exempt (no product code); 5 required; 6 required, and T-06 is that gate.
- **Note:** reset `DPDK_MTL_MINOR_VER`. The `mtl_tag_since="26.03"` gate in
  `dpdk_is_installed()` at `script/build_dpdk.sh:57-70` still passes for 26.07 through
  `sort -V`, so it needs no edit — confirm that, do not assume it. This host currently
  reports `26.03.90_mtl_` while `versions.env` pins minor `91`, so the installed DPDK
  is already stale and a rebuild is forced either way. **ACTION ON HOSTS** — every test
  host needs the new DPDK before T-06 and T-07.

## T-04 Decide whether MTL needs the ice `rl_burst_size` devarg — OPEN

- **Owner:** mtl-planner first, then mtl-developer if the answer is yes
- **Ref:** upstreaming.md §6
- **Files:** [lib/src/dev/mt_dev.c:309-392](lib/src/dev/mt_dev.c),
  `tests/unit/dev/mt_dev_harness.{c,h}`, a new `tests/unit/dev/mt_dev_devargs_test.cpp`,
  `tests/unit/meson.build`
- **Acceptance:** part one is a written answer with evidence. Part two, only if needed:
  `./build_unit/tests/unit/UnitTest --gtest_filter='MtDevDevargs*'`.
- **Test tier:** unit — pin the built `-a <BDF>` string for both DPDK versions.
- **Gates:** 2 required if code lands; 5 required; 6 required if code lands (pacing).
- **Note:** part one answers one question — does MTL ever drive an ice **PF** with
  hardware rate-limit pacing? `patches/ice_drv/2.6.6/0002-*` already sets the 2 KB
  scheduler burst in the kernel module, and the kernel PF is what programs a VF rate
  limiter, so the dropped DPDK patch `0003` may change nothing in the normal VF
  deployment. Do not add the devarg on the strength of the old "pacing regresses
  silently" claim; §6 shows why that claim is narrower than it read. If code lands,
  `dev_eal_init()` is static and builds the string inline at `mt_dev.c:388`, so split
  the builder into its own function first — that split is what makes the unit test
  possible.

## T-05 Capture the 26.03 hardware baseline — OPEN

- **Owner:** mtl-system-admin
- **Ref:** upstreaming.md §6
- **Files:** none. Output goes in the note and in a file the note names.
- **Acceptance:** `sudo ./build/tests/KahawaiTest --p_port <bdf> --r_port <bdf>
  --auto_start_stop --gtest_filter='St20p*'` pass output, the same run with
  `--pacing_way tsc`, and the ST 2110-21 narrow-sender pacing numbers.
- **Test tier:** integration.
- **Gates:** none — this task **is** the Gate 6 baseline for T-03.
- **Note:** must run **before** T-03, because after the bump there is no 26.03 to
  measure. Ask the user before touching the host: VF create or destroy, driver
  rebuild, hugepage change, or MtlManager restart can kill somebody else's live test.
  **ACTION ON HOSTS**

## T-06 Verify the bump on real hardware — OPEN

- **Owner:** mtl-system-admin
- **Needs:** T-03, T-05
- **Ref:** upstreaming.md §6
- **Acceptance:** the full `KahawaiTest` suite, plus the two filtered runs from T-05,
  with pacing and PTP numbers inside the T-05 baseline.
- **Test tier:** integration.
- **Gates:** this is Gate 6 for T-03 and for T-04.
- **Note:** `NoCtxTest.*` needs one process per case — use
  `tests/integration_tests/noctx/run.sh`, never a filter that matches several.
  **ACTION ON HOSTS**

## T-07 Run the acceptance smoke suite on 26.07 — OPEN

- **Owner:** orchestrator, per `.github/instructions/mtl-acceptance-tests.instructions.md`
- **Needs:** T-03
- **Files:** none. Never edit `conftest.py`, `common/` or `mtl_engine/` to pass a test.
- **Acceptance:** `cd tests/acceptance && sudo -E ./venv/bin/python3 -m pytest
  --topology_config=configs/topology_config.yaml
  --test_config=configs/test_config.yaml -m smoke -v`
- **Test tier:** acceptance.
- **Gates:** 5 not applicable (no diff); this is the end-to-end proof of T-03.
- **Note:** the acceptance suite reads `.local_install/mtl/bin/`, a different install
  tree from the one gtest uses, so it needs its own rebuild against the new DPDK —
  `MTL_INSTALL_PREFIX=$PWD/.local_install ./build.sh`. Most cases need `/mnt/media`
  mounted. **ACTION ON HOSTS**

## T-08 Give the carried patches real authorship metadata — OPEN

- **Owner:** mtl-developer
- **Needs:** T-02
- **Ref:** upstreaming.md §8
- **Files:** the new `0007`, `0008` and `0009` in `patches/dpdk/26.07/`
- **Acceptance:** `grep -rn "noreply@example.com\|0000000000000000" patches/dpdk/26.07/`
  returns nothing, and `git am` of the set into a scratch `dpdk-26.07` clone produces
  real authors.
- **Test tier:** none. Patch metadata only.
- **Gates:** 2 exempt; 5 required; 6 exempt.
- **Note:** this still matters with no upstreaming, because `patch -p1` ignores `From:`
  but `git am` does not, and three places tell the reader to use `git am`. Remove the
  `Cc: stable@dpdk.org` lines rather than adding more — they mean nothing in a tree
  that never posts. Replace the all-zero `Fixes:` hashes with real ones or delete the
  tag; do not invent a hash, which is how the fabricated blob hash in `0011` got there.

## T-09 Record the pcapng break at the call site — OPEN

- **Owner:** mtl-developer
- **Ref:** upstreaming.md §7
- **Files:** [lib/src/mt_pcap.c:85](lib/src/mt_pcap.c)
- **Acceptance:** `./build.sh` green; the comment names the symbol, the accepted
  upstream signature, and the patch that supplies the current one.
- **Test tier:** none. Comment only.
- **Gates:** 2 exempt (comment); 5 required; 6 exempt.
- **Note:** `rte_pcapng_copy_ts()` exists only in MTL patch `0006`. Upstream accepted a
  different shape — `rte_pcapng_copy()` with an extra `uint64_t timestamp` — and that
  shape is **not** in 26.07, so 26.07 is safe and MTL keeps `0006` unchanged. The build
  breaks on the DPDK release that applies the accepted patch. Four lines of comment
  turn that future failure into a five-second diagnosis.

## T-10 Make CI and the documentation read the pinned DPDK version — OPEN

- **Owner:** mtl-developer
- **Ref:** upstreaming.md §9
- **Files:** [.github/workflows/validation-tests.yml:109](.github/workflows/validation-tests.yml),
  [.github/workflows/msys2_build.yml:46](.github/workflows/msys2_build.yml),
  [doc/build.md:150](doc/build.md), [doc/build_WIN.md:76](doc/build_WIN.md),
  [doc/design.md:671](doc/design.md),
  [doc/experimental/header_split.md:16](doc/experimental/header_split.md)
- **Acceptance:** `./checkpatch.sh` clean, and no literal DPDK version outside
  [versions.env](versions.env) and `patches/dpdk/*/` —
  `grep -rn "2[0-9]\.[0-9][0-9]" --include=*.yml --include=*.md` reviewed by hand.
- **Test tier:** none. CI and documentation.
- **Gates:** 2 exempt; 5 required; 6 exempt.
- **Note:** CI validation pins `25.11` while `versions.env` pins `26.03`, so it builds
  a different DPDK from the shipped one and passes only because `patches/dpdk/25.11/`
  still exists. Read `versions.env` in the workflow instead of editing six numbers,
  or the same drift returns at the next bump. The msys2 matrix pins `[25.03, 23.11]`
  and needs a decision, not a bump: say which versions Windows still supports.

## T-11 Move the Rx path to `RTE_ETH_RX_OFFLOAD_TIMESTAMP` and delete `0004` — OPEN

- **Owner:** mtl-planner first, then mtl-developer
- **Ref:** upstreaming.md §10
- **Acceptance:** PTP locks with an **unpatched** DPDK, and the patch is gone from
  `patches/dpdk/26.07/`. Integration run proves PTP lock and timestamp accuracy.
- **Test tier:** unit for the offload-request path, integration for PTP lock.
- **Gates:** 2 required; 5 required; 6 required (PTP, timestamps).
- **Note:** `0004` drops the PTP ptype filter, so the driver marks every packet as a
  PTP packet — upstream called that incorrect and we agreed. The offload flag
  timestamps every packet without the false mark, and it was tested during the review.
  Not a 26.07 task. Low priority, high value.

## T-12 Move header split to `RTE_PKTMBUF_POOL_F_PINNED_EXT_BUF` and delete `hdr_split/0001` — OPEN

- **Owner:** mtl-planner first, then mtl-developer
- **Ref:** upstreaming.md §10
- **Files:** `ST20_RX_FLAG_HDR_SPLIT`, `mt_if_hdr_split_pool()`,
  [doc/experimental/header_split.md](doc/experimental/header_split.md)
- **Acceptance:** zero-copy Rx works with an unpatched ethdev.
- **Test tier:** unit for the pool flag, integration for zero-copy Rx.
- **Gates:** 2 required; 5 required; 6 required (Rx data path).
- **Note:** this patch changes `lib/ethdev/rte_ethdev.{c,h}` and `ethdev_driver.h`, so
  it conflicts on every DPDK bump and it is the most invasive patch MTL carries.
  Buffer split plus a pinned external buffer pool covers the same case. Not a 26.07
  task. Lowest priority, largest change.

## Cancelled — 2026-08-24, decision D1

Recorded so that a missing task does not read as an oversight.

| Was | Why it is gone |
|---|---|
| Fix the inverted guard in patch `0002` | The bump drops `0002`, and 26.07 carries the fix. Do not backport it. upstreaming.md §5 |
| Repost the iavf runtime-queue-setup patch as v7 | D1. MTL keeps the patch as the new `0002`. |
| Ping Stephen Hemminger on the pcapng v6 patch | D1. MTL keeps the patch as the new `0003`. T-09 records the future break. |
| Ask Soumyadeep Hore about patches `0009`–`0011` | D1. MTL keeps all three. |
| Send `0009`–`0011` as a series to `next-net-intel` | D1. |
| Send the testpmd `create pinned-rxpool` patch standalone | D1. The file lived in `/home/labrat/dev1/dpdk`, which no longer exists. |
| Regenerate patch `0006` from the accepted upstream v6 file | D1, and the same dead path. 26.07 does not carry the accepted shape, so nothing breaks. |
| Decide a route for patches `0012` and `0013` | D1 answers it: MTL keeps both. T-08 fixes their metadata. |

## Done

Nothing yet.
