# DPDK 26.07 move — progress report, 2026-08-25

A record of the round that produced the `dpdk-26.07` branch. The work list is
[tasks.md](tasks.md). The source record is [upstreaming.md](upstreaming.md). This file tells a
new reader what the round did, how it measured, and what it got wrong about itself.

Prose is Simplified Technical English.

## 0. How to read every fact in this file

**Three states disagree in this round, and each fact below names one.** A claim can be true of
the commit graph, true of the index, or true only of the working tree. Most of the repairs this
round produced are **in the working tree and nowhere else**, so a sentence that says "fixed"
without naming a state is wrong for anybody who checks the branch out. Therefore every factual
sentence in §1 to §6 says `at HEAD`, `in the index`, or `in the working tree`, or prints the
command that re-derives it. §7 is outside that rule: it is a historical log, and each figure in
it was true when the correction was recorded.

`HEAD` when this file was written is `9ad1f428`. Re-derive the shape of the tree with:

```sh
git rev-parse --short HEAD
git status --porcelain                # the dirty state; every count in it moves hourly
git diff --cached --name-status -M    # what is actually staged
```

**Every transcript below was taken with GNU grep 3.11 at `/usr/bin/grep`**, because a predicate's
output depends on which `grep` runs it — in an agent shell on this host the bare `grep` is a shell
function that shims to ugrep 7.8.4.

**This file asserts no task status.** Statuses change while it is being read, and `tasks.md`
holds vocabulary that no fixed three-way summary can express — `OPEN`, `BLOCKED`,
`IN PROGRESS`, `DO NOT START`, `HALF DONE`, `PHASE 1 DONE, 2 to 7 OPEN`. Enumerate them
yourself, and treat the answer as the authority over anything below:

```sh
grep -nE '^1\. \[.\] \*\*T-[0-9]+\*\*' tasks.md
```

**This file cites `tasks.md` and `upstreaming.md` by grep predicate or by section, never by line
number.** Both are edited continuously, and a line citation into either rots within the hour.
While this revision was being written, most of the `tasks.md` line numbers taken at the start of
it had already moved. A citation that names what to search for survives that; one that names a
line does not.

## 1. Scope and decisions

The round replaced an upstreaming effort with a version bump. Eight decisions were locked with
the user on 2026-08-24 and are recorded in the `## DECISIONS` table of `tasks.md`. Four of them
shape everything else:

- **D1** — MTL sends nothing to `dev@dpdk.org`. Eight upstreaming tasks were cancelled.
- **D2** — target DPDK 26.07.
- **D3** — a patch stays only when the v26.07 source proves the change is absent. Nothing is
  kept "just in case".
- **D7** — the burst-size patch `0003` is dropped, and an `rl_burst_size` field in
  `struct mtl_port_init_params` replaces it.

The patch count is **5 dropped, 11 kept, 0 open**. The 11 are the same 11 at HEAD and in the
working tree — `find patches/dpdk/26.07 -name '*.patch' | wc -l` gives 11, and
`git ls-tree -r --name-only HEAD patches/dpdk/26.07 | wc -l` gives 11 — but their **bytes
differ**, and §3 says how.

## 2. Method

Every task ran the six-gate test-first loop in
[.github/copilot-instructions.md](.github/copilot-instructions.md), numbered 0 to 6:
knowledge, failing test, implement, green build, adversarial review, hardware.
`mtl-orchestrator` held the work list and fired Gates 5 and 6. `mtl-developer` owned Gates 0 to
4. `mtl-reviewer` gave the Gate 5 verdict and had no exemption.

Two rules govern the round and both were learned the expensive way:

1. **State the predicate and the command that enumerates it, never the count.** Each time a
   count was written next to a rule, the count was the part that was wrong. §0 applies this
   rule to this file.
2. **An acceptance test can reward the defect it is meant to catch.** A test that only checked
   for the absence of a placeholder paid for an invented author name, and an earlier pass paid
   it.

A third rule came out of the patch work and is worth as much as either: **a zero exit status is
not evidence that a patch applied cleanly.** GNU `patch` 2.7.6 recovers a stale hunk header by
searching for the context at an offset and still returns 0. The signal is the **absence of an
`offset` line**, not the return code. §3 has the measurement.

Gate 5 was not a formality. `tasks.md` records T-04's Gate 5 as closing
`after 4 passes` — `grep -n 'after 4 passes' tasks.md` — and every pass but the last found a
real defect. The pass tallies this file previously gave for T-01, T-02, T-08, T-09 and T-10 are
**not corroborated anywhere in the tree**: those task records left `tasks.md`, and every hit of
`grep -rnoE '[0-9]+ passes' tasks.md upstreaming.md` belongs to another task. Treat the tallies
as lost, not as measured.

## 3. What the round landed, and where

`tasks.md`'s own preamble records the round of 2026-08-24 to 2026-08-25 as closing six tasks —
T-01, T-02, T-08, T-09, T-10 and T-33. Find it with `grep -n 'tasks closed there' tasks.md`.
Each row below names the commit that carries it. Re-derive any row with
`git log --oneline -1 -- <path>`.

| Task | Commit | Result |
|---|---|---|
| T-01 | none — evidence only | The drop list is proved against a real v26.07 tree: `upstreaming.md` §3 tables one source grep per drop candidate, each row ending in a `Measured result`, and one `patch -p1 --dry-run` per patch in a second table whose rows end in a `Verdict`. Read the rows with `sed -n '/^## 3\./,/^## 4\./p' upstreaming.md`. |
| T-02 | `675833f1` | `patches/dpdk/26.07/` holds the 11 final files. See the application measurement below — the "0 fuzz, 0 rejects" this file used to claim was true and misleading. |
| T-08 | `675833f1` | The carried patches have real authorship metadata. **9 of 10** header-bearing files commit under 5 real author names — see below. |
| T-09 | `9332b1fe` | `lib/src/mt_pcap.h` records the pcapng break at the guard, which is where the capture is lost. |
| T-10 | `7ea1f062` | CI and the documentation read the pinned DPDK version for the four locations T-10 owned. It did **not** clear the tree — see below. |
| T-33 | none found | The T-08 record was condensed and its durable findings routed to owners. The line figures this file used to give, 356 to 59, appear **nowhere** in `tasks.md` or `upstreaming.md`; `grep -rn '356\b' tasks.md upstreaming.md` returns nothing. Unverifiable. What would settle it is the pre-condensation blob, which no commit in this branch carries. |

### T-02 — the measurement that "0 fuzz, 0 rejects" hid

Nine flat patches, applied to a pristine `v26.07.zip` tree with GNU `patch` 2.7.6. `patch -p1`
returned **0 for all nine at HEAD**, and that told nobody anything:

```text
$ patch -p1 < <each HEAD patch>   # applied in order 0001 to 0009 into one tree, not independently
0001-Change-to-enable-PTP.patch rc=0 offsetlines=3
  Hunk #1 succeeded at 2023 (offset 51 lines).
  Hunk #2 succeeded at 2389 (offset 51 lines).
  Hunk #3 succeeded at 2879 (offset 44 lines).
0002-iavf-disable-runtime-queue.patch rc=0 offsetlines=1
  Hunk #1 succeeded at 1160 (offset 20 lines).
0006-net-ice-fix-read-clock-to-use-PHC-hardware-time.patch rc=0 offsetlines=1
  Hunk #1 succeeded at 7426 (offset 106 lines).
0007-net-ice-fix-TxPP-launch-time-encoding-for-19-bit-HW-f.patch rc=0 offsetlines=1
  Hunk #1 succeeded at 3081 (offset -1 lines).
0009-net-ice-always-init-PHC-owner.patch rc=0 offsetlines=2
  Hunk #1 succeeded at 7157 (offset 106 lines).
  Hunk #2 succeeded at 7234 (offset 106 lines).
=== TOTAL offset lines at HEAD: 8 ===
```

**At HEAD and in the index: 8 offset hunks across 5 of the 9 files.** The index carries the
same patch bytes as HEAD — the only staged change under `patches/dpdk/` is 4 `R100` renames,
which by definition change no content.

**In the working tree: 0 offset lines.** The same loop over the working-tree patches gives
`rc=0 offsetlines=0` for all nine. That repair is **unstaged and uncommitted.**

**The order is part of the measurement.** `git am` and
[script/build_dpdk.sh](script/build_dpdk.sh) apply the set in order into one tree, so the
figures above are the ones a real flow produces. Apply each patch to its own pristine tree
instead and the HEAD set still gives 8 offset lines, but `0007` lands at 3083 with offset 1, and
the working-tree set gives 1 offset line, not 0. `patch -p1` and `git apply -v` agree in both
readings. Prove the repair:

```sh
diff <(git show HEAD:patches/dpdk/26.07/0001-Change-to-enable-PTP.patch | grep '^@@') \
     <(grep '^@@' patches/dpdk/26.07/0001-Change-to-enable-PTP.patch)
# HEAD @@ -1972,8 +1972,7 @@   worktree @@ -2023,8 +2023,7 @@
```

The command above shows the shift in all three of `0001`'s hunk headers. The other recomputed
headers are `0002` (1140 to 1160), `0006` (7320 to 7426), `0007` (3082 to 3081) and `0009`, whose
two hunks move 7051 to 7157 and 7081 to 7187 — the same 106-line shift as `0006`.

Two mechanism facts belong with this, because both cost a measurement:

- **GNU `patch` defaults to `--backup-if-mismatch`, and the backup is per file, not per hunk.**
  The 8 offset hunks at HEAD touch 3 distinct source files, so plain `patch -p1` over the HEAD
  set leaves exactly 3: `drivers/net/intel/iavf/iavf_ethdev.c.orig`,
  `drivers/net/intel/ice/ice_ethdev.c.orig`, `drivers/net/intel/ice/ice_rxtx.c.orig`. Over the
  working-tree set it leaves 0. So the honest acceptance predicate is **0 `offset` lines under
  `git apply -v` and 0 `.orig` files under `patch -p1`** — never the return code.
- **A pristine-tree guard must say `-type f`.** Without it, `find dpdk-26.07 -newer v26.07.zip`
  reports **7** paths on a freshly unzipped, untouched tree, all of them symlinks whose mtime
  `unzip` cannot set. `find … -newer v26.07.zip -type f | wc -l` gives 0, and
  `find … -newer v26.07.zip -type l | wc -l` gives 7. An earlier count of 8 that blamed a
  directory does not reproduce; re-measured on 2026-08-25 there are 7, and 0 directories.

### T-08 — the tight authorship figure is 9 of 10, and 2 of the 5 names are unproved

`upstreaming.md` §8 sets the header-bearing set at **ten** files: the 9 flat patches plus
`hdr_split/0001`. Find the sentence with `grep -n 'ten files, not nine' upstreaming.md`. Both at
HEAD and in the working tree:

```text
$ grep -h '^From: ' patches/dpdk/26.07/*.patch patches/dpdk/26.07/hdr_split/*.patch | sort -u
From: Dawid Wesierski <dawid.wesierski@intel.com>
From: Frank Du <frank.du@intel.com>
From: Marek Kasiewicz <marek.kasiewicz@intel.com>
From: MTL Contributor <noreply@example.com>
From: Ric Li <ming3.li@intel.com>
From: Soumyadeep Hore <soumyadeep.hore@intel.com>
```

Six values: 5 real names and 1 deliberate placeholder, in `0009`. So **9 of 10 under 5 real
names**. "Real author names" still overstates what was proved: the T-27 record in `tasks.md`
holds two of those attributions as unverified reattributions — `0001-Change-to-enable-PTP` and
`hdr_split/0001` were each reattributed twice through the version directories, and the task
that owns them is open. Find the record with `grep -n 'reattributed twice' tasks.md`, which
matches at HEAD as well as in the working tree.

### T-10 — the claim was bounded to four locations, not to the tree

`upstreaming.md` §9 tables the version literals location by location, and marks each row
**Closed**, **Open**, or **deliberate** and load-bearing. That is the claim T-10 supports — read
the rows with `sed -n '/^## 9\./,/^## 10\./p' upstreaming.md`. The tree is **not** free of DPDK
version literals in the working tree:

```text
$ grep -rnE '\b2[0-9]\.(03|05|07|08|11)\b' --include='*.md' --include='*.yml' --include='*.sh' . \
    | grep -vE '(^|/)(patches/|tasks\.md|upstreaming\.md|report-dpdk|tests/acceptance/venv/)' \
    | cut -d: -f1 | sort | uniq -c | sort -rn
      8 ./CHANGELOG.md
      4 ./doc/design.md
      3 ./script/build_dpdk.sh
      3 ./doc/build_WIN.md
      1 ./script/check_dpdk_patches.sh
      1 ./.github/workflows/msys2_build.yml
      1 ./.github/legacy/msys2_ffmpeg.yml
      1 ./.github/legacy/msys2_build.yml
      1 ./.github/legacy/codeql.yml
      1 ./.github/claude/CLAUDE.md
      1 ./doc/sdm_appliance.md
      1 ./doc/experimental/header_split.md
      1 ./doc/coding_standard.md
      1 ./doc/build.md
      1 ./doc/asan.md
=== total lines: 29 ===
```

**GNU grep 3.11 produced that run on 2026-08-25, and the total moves.** The row labels carry the
`./` prefix that GNU grep adds, and an untracked file counts, so a new one changes the figure —
`./script/check_dpdk_patches.sh` is untracked today. Run the command; do not quote the total.
`.github/workflows/msys2_build.yml:46` carries
`dpdk: [25.03, 23.11]`, which `upstreaming.md` §9 marks **Open** — a live exception, not
a closed row. Nothing asserts documentation literals against `versions.env`, and a task in
`tasks.md` is open for exactly that, so no absolute claim belongs here.

Filter the exclusions **on the path**, as above. Excluding on the whole line, as an earlier
sweep did, silently drops any hit whose text happens to mention `tasks.md` or `upstreaming.md`
— that is how `.github/claude/CLAUDE.md:24` went uncounted.

The `(^|/)` anchor is load-bearing for the same reason. An earlier sweep anchored on `^\./` and
so depended on the `./` prefix. `ugrep` writes no such prefix, so every exclusion matches
nothing, the total rises by an order of magnitude, and no error is printed. The `(^|/)` form
gives the same total under both greps.

### The partial items

- **T-04's code half** closed on Gate 5 with 0 blockers, in `b1d78d49`. The `rl_burst_size`
  field, its EAL devarg path and 5 unit tests are at HEAD, and the T-04 record in `tasks.md`
  reports `UnitTest --gtest_filter='MtDevDevargs*'` giving `[  PASSED  ] 5 tests.` with 4 of the
  5 failing first — `grep -n 'MtDevDevargs' tasks.md`.
- **T-05** captured 2 of its 3 baseline runs on real hardware: `auto` pacing gives
  `[ PASSED ] 42 tests.` and `tsc` pacing gives 41 passed with 1 known failure. The baseline is
  outside the tree, in `/home/labrat/mtl/baseline-26.03/`, so no rebuild can overwrite it.
  These two tallies rest on the `tasks.md` record and are not otherwise reproducible here —
  `grep -n '2 of 3 captured' tasks.md`, which matches at HEAD as well as in the working tree.
- **T-34** fixed the cause of the dead MCP servers — an unpinned `mcp[cli]` dependency that
  resolved to 2.x, which removed the `mcp.server.fastmcp` module both servers import. `326e3fcc`
  carries the pin and the two wrapper probes, and **those three files** are clean in the index
  and clean in the working tree. Other files under `.github/mcp/` are dirty; the claim is about
  the three files only. They are not called verified: the only proof available in that session
  was read off a dead MCP connection. A new untracked `.github/mcp/test_mtl_mcp_server.py` sits
  beside them in the working tree.

## 4. What is staged, and what is only in the working tree

**Almost every repair described above is in the working tree and in no commit.** Rebuild this
list before you trust it:

```text
$ git diff --cached --name-status -M      # git separates these columns with a tab
M      .gitignore
R100   patches/dpdk/26.03/0012-net-ice-e830-use-direct-MMIO-for-PHC-update.patch   patches/dpdk/26.03/0012-net-ice-e830-use-direct-MMIO-for-PHY-timer-command.patch
R100   patches/dpdk/26.03/0013-net-ice-always-init-PHC-owner.patch   patches/dpdk/26.03/0013-net-ice-init-PHC-owner-when-enabling-timesync-on-a-n.patch
R100   patches/dpdk/26.07/0008-net-ice-e830-use-direct-MMIO-for-PHC-update.patch   patches/dpdk/26.07/0008-net-ice-e830-use-direct-MMIO-for-PHY-timer-command.patch
R100   patches/dpdk/26.07/0009-net-ice-always-init-PHC-owner.patch   patches/dpdk/26.07/0009-net-ice-init-PHC-owner-when-enabling-timesync-on-a-n.patch
M      report-dpdk-26.07.md
M      tasks.md
```

So the index holds exactly three things: 4 `R100` renames that give `0012`/`0013` and
`0008`/`0009` their real upstream subjects, a `.gitignore` block that ignores the three
acceptance A/B install trees (`.local_install`, `local_install_old/`, `local_install_new/`), and
this file plus the work list.

Nothing else is staged. Check any single path with
`git diff --cached --quiet -- <path>; echo $?` — 0 means not staged.

| Change | State | How to see it |
|---|---|---|
| 26.07 `@@` headers recomputed, 8 offset hunks to 0 | working tree only | `git diff -- patches/dpdk/26.07/` |
| 26.03 and 26.07 `From:`/`Subject:`/`Signed-off-by:` metadata rewrite | working tree only; 0 `index`-line changes under 26.03 | `git diff -U0 -- patches/dpdk/26.03/ \| grep -c '^+index '` gives 0 |
| 24 `windows/` stubs turned back into symlinks | working tree only, as git typechanges | `git diff --name-status -M -- patches/dpdk/ \| grep -c '^T'` gives 24 |
| `ICE_REPO` removed from `versions.env` | working tree only | `git show HEAD:versions.env \| grep -n ICE_REPO` gives line 14; `grep -rn ICE_REPO versions.env` gives nothing |
| msys2 uses `git apply` for `windows/*.patch` instead of `git am` | working tree only | `git diff -- .github/workflows/msys2_build.yml` |
| `base_build.yml` builds the Rust `no_std` example | working tree only | `grep -n 'no_std' .github/workflows/base_build.yml` matches; the same grep on `git show HEAD:…` gives nothing |
| `.github/workflows/unit_tests.yml`, a `unit-tests` job | **untracked** | `git ls-files --others --exclude-standard \| grep unit_tests` |
| `script/check_dpdk_patches.sh` | **untracked** | `git ls-files --others --exclude-standard \| grep check_dpdk` |

Everything the previous revision of this section listed as "in this commit" was **already
committed** before this file was written — `include/mtl_api.h`, `lib/src/dev/mt_dev.{c,h}`,
`lib/src/mt_main.c`, `tests/unit/dev/mt_dev_devargs_test.cpp` and
`rust/imtl-sys/examples/no_std.rs` in `b1d78d49`; `lib/src/mt_pcap.h` in `9332b1fe`;
`doc/build.md`, `doc/build_WIN.md`, `doc/experimental/header_split.md` and
`.github/workflows/validation-tests.yml` in `7ea1f062`; the three `.github/mcp/` files in
`326e3fcc`. Three of them — `rust/imtl-sys/examples/no_std.rs`, `doc/build.md` and
`doc/build_WIN.md` — are dirty again in the working tree for unrelated work.

**`patches/dpdk/26.07/` is still inert at HEAD and in the index.**
[script/build_dpdk.sh](script/build_dpdk.sh) globs `patches/dpdk/"$DPDK_VER"/*.patch`, and
`versions.env:1` pins `DPDK_VER=26.03` at HEAD, in the index and in the working tree, so
nothing reaches those files until T-03 changes that one value. Landing them cannot affect this
host or anybody's running test.

## 5. Where the work stopped, and why

`tasks.md` carries a `## READY NOW` section whose lead line opens "No host, no decision, no
MCP. …" and then names the tasks — `grep -n 'READY NOW' tasks.md`. So unblocked work remains.
What needs the host needs one of two things this session cannot supply.

- **A session restart.** `mtl-system-admin` is the only agent allowed to configure the host or
  to run `KahawaiTest`, and it is MCP-only. Claude Code negotiates MCP connections once, at
  session start, so this session holds a dead connection for both servers whatever is on disk.
  T-34 holds the cause and the remedy.
- **The host chain, in this order:** T-03 bumps `versions.env` and installs 26.07, T-35 gives a
  shipped binary a way to set `rl_burst_size`, T-06 measures both on real hardware, T-07 runs
  the acceptance smoke suite. T-06 is Gate 6 for T-03 and for T-04.

Three measurements fix that order and are worth reading before the chain runs.

**1. The installed 26.03 ice PMD does not know the `rl_burst_size` key.** Its `PMD_INFO_STRING`
enumerates **ten** devarg keys, and `rl_burst_size` is not one of them:

```text
$ strings -a /usr/local/lib/x86_64-linux-gnu/dpdk/pmds-26.1/librte_net_ice.so.26 \
    | grep -o 'hw_debug_mask=.*link_state_on_close=<down|up|initial>' | head -1
hw_debug_mask=0xXXXproto_xtr=[queue:]<vlan|ipv4|ipv6|ipv6_flow|tcp|ip_offset>safe-mode-support=<0|1>default-mac-disable=<0|1>ddp_pkg_file=</path/to/file>ddp_load_sched_topo=<0|1>tm_sched_levels=<N>source-prune=<0|1>rx_low_latency=<0|1>link_state_on_close=<down|up|initial>

$ strings -a … | grep -c '^rl_burst_size$'
0
$ strings -a … | grep 'rl_burst'
ice_cfg_rl_burst_size
```

The only match is `ice_cfg_rl_burst_size`, an internal base-driver symbol and not a devarg. The
same result holds for all three real ice PMD binaries on this host: `/usr/local`'s two copies
and the sibling checkout's
`/home/labrat/mtl/Media-Transport-Library/.local_install/dpdk/…/pmds-26.1/`. So a run today
returns an unknown-key probe failure, which proves nothing. T-03 must come first, and T-35 must
come before T-06.

**2. The loader serves a different checkout's DPDK.** `/etc/ld.so.conf.d/mtl_local.conf` puts
the sibling checkout `/home/labrat/mtl/Media-Transport-Library/.local_install/dpdk/lib/…` ahead
of `/usr/local`, and both export soname `librte_eal.so.26`. So installing 26.07 into
`/usr/local` does not by itself change what a test loads, and Gate 6 could pass while measuring
the old DPDK. Every recorded run must carry `--log_level notice` and prove `dpdk version:` from
inside the process.

**3. The installed minor does not match the pinned minor.** `versions.env:1-2` composes
`26.03.91_mtl_` from `DPDK_VER=26.03` and `DPDK_MTL_MINOR_VER=91`. What is installed reports
90:

```text
$ PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig pkg-config --modversion libdpdk
26.03.90_mtl_
```

A live task owns this. It is recorded here only so that a Gate 6 run does not read `26.03.9x`
and assume the pin was honoured.

## 6. What the verification found beyond the plan

Most open work in `tasks.md` was **found by a verification pass, not planned**. There is exactly
one import, T-37, taken from
`/home/labrat/notes/todo.md` where it is tracked as SDBQ-3799 — `grep -n 'Imported' tasks.md`
returns that single line, so any later import is not yet marked as one. Enumerate the current
set with the grep in §0 and do not trust a total written here. Most of these are defects in the
carried patch set or in the round's own record, and only a re-measurement could find them.

The ones that matter outside this move:

- **The unit tier.** The suite is **513 tests in 65 suites**, not 508. The T-19 record in
  `tasks.md` derives it — "T-04 added 5 tests… 508 + 5 = 513" — and a `--gtest_list_tests` run in
  the same record reports `65 suites, 513 tests`. Enumerate every place the figure appears with
  `grep -n '513' tasks.md`. The same
  record reports the abort as fixed on 2026-08-25 with
  `513 tests from 65 test suites ran. [ PASSED ] 513 tests.` The cause was a test reaching
  `rte_eal_init()`, where the EAL panicked on a double-registered tailq. **A workflow does now
  run the tier, but only in the working tree:** `.github/workflows/unit_tests.yml` is
  **untracked** and defines a `unit-tests` job at `:26` that runs `./build.sh unit` at `:72`.
  `grep -rln 'build.sh unit' .github/workflows/` matches that one file and nothing else, and
  `git ls-files .github/workflows/ | xargs grep -l 'build.sh unit'` returns nothing, so at HEAD
  no workflow runs the tier — which is why the abort hid. Search for `build.sh unit`, not for
  `build_unit` or `UnitTest`: neither string appears in the file.
- **The `windows/` stubs.** 24 files under `patches/dpdk/*/windows/` are symlinks that a
  `core.symlinks=false` checkout materialized as text. In the working tree they are real
  symlinks; at HEAD and in the index they are still `100644` text. The census:
  `find patches/dpdk -type l | wc -l` gives **83**;
  `git ls-tree -r HEAD patches/dpdk | awk '$1=="120000"' | wc -l` gives **59**;
  `git diff --name-status -M -- patches/dpdk/ | grep -c '^T'` gives **24**; and 59 + 24 = 83.
  Across `patches/` the figures are **88 = 64 + 24**, the extra 5 being the pre-existing relative
  symlinks in `patches/ice_drv/` that the T-37 record describes —
  `find patches/ice_drv -type l | wc -l` gives 5. Repository-wide is a third pair of figures, not
  either of these: `git ls-tree -r HEAD | awk '$1=="120000"' | wc -l` and
  `find . -path ./.git -prune -o -type l -print | wc -l`.
- **Stale `index` lines.** The 26.07 half is recomputed **in the working tree only**: 14 `index`
  lines across 8 files differ from HEAD, in `0001`, `0002`, `0005`, `0006`, `0007`, `0008`,
  `0009` and `hdr_split/0001`, which carries 7 of the 14. Re-derive it with
  `git diff HEAD -U0 -M -- patches/dpdk/26.07/ | grep -c '^+index '`. Ask git for the whole
  directory, not for one file at a time: `0008` and `0009` are two of the 4 staged `R100`
  renames, so a loop keyed on the HEAD filenames misses both and reports 12 across 6.
  The 26.03 half is untouched —
  `git diff -U0 -- patches/dpdk/26.03/ | grep -c '^+index '` gives 0 — and it is
  **unverifiable on this host and will stay so.** No pristine DPDK 26.03 tree exists anywhere
  here: every DPDK source tree found under `/home/labrat` and `/tmp` reports `26.07.0`,
  `26.07.0_mtl_`, `23.11.0` or one `26.01.0.DEV`, and `find / -name 'v26.03.zip'` returns
  nothing. One authorized download of `v26.03.zip` would settle it. Plain `git am` ignores
  `index` lines, so nothing breaks today; `git am -3` cannot work.
- **The msys2 Windows workflow.** It cannot pass, and the decisive reason is that **the job
  never runs**: `.github/workflows/msys2_build.yml:22` gates on
  `steps.filter.outputs.msys2_build`, and `.github/path_filters.yml` defines no such key —
  `grep -c msys2 .github/path_filters.yml` gives 0, and the keys it does define are `src`,
  `build`, `docker`, `ecosystem`, `ice_build`, `ubuntu_build` and `linux_tests`. The second fault
  is the `git am` on `windows/*.patch`, fixed in the working tree only, at `:136`.
  The symlink shape is **not** a fault in CI. A `Convert patches for DPDK` step dereferences the
  HEAD shape for both the flat glob and `windows/` at `msys2_build.yml:99-104`, before any
  `git am` runs, and that step is identical at HEAD and in the working tree. The gap is in the
  manual flow: the conversion exists in the working tree at `doc/build_WIN.md:75-106`, while
  `git show HEAD:doc/build_WIN.md` goes straight to `git am` at `:82`. So at HEAD the documented
  manual flow is the only path that lacks the conversion. Do not add a second conversion step to
  the workflow, and do not commit the dereferenced tree: `doc/build_WIN.md` records that a commit
  replaces each symlink with a large file, and that `core.symlinks=false` writes the patch body as
  the link target. That warning is working-tree only —
  `grep -n 'Do not commit the result' doc/build_WIN.md` matches, and the same grep against
  `git show HEAD:doc/build_WIN.md` is empty. No tool and no CI job here finds either result. None
  of the reasons is the version pin.
- **The Rust `no_std` example.** `.github/workflows/base_build.yml` gained a
  `Build the Rust no_std example` step **in the working tree**. `grep -n 'no_std'` finds it there,
  and the same grep against `git show HEAD:.github/workflows/base_build.yml` returns nothing. That
  file is itself under edit, so read the two states and do not quote a line number. The example
  itself, `rust/imtl-sys/examples/no_std.rs`, is dirty.

**Some tasks need a person, not a command.** `tasks.md` carries a `## BLOCKED ON A PERSON`
section that lists the blocked tasks, and inside it a `### THE ONE ROUND TO ASK` sub-section that
composes the questions covering the highest-leverage decisions; it defers T-28 and T-31 to a
second round because both are cheap either way and gate only T-27. Read that section for the
current set and its options rather than trusting a list here, because it has already been
recomposed once:

```sh
sed -n "/^## BLOCKED ON A PERSON/,/^## READY NOW/p" tasks.md
```

## 7. Corrections the round made to its own record

The orchestrator numbered its own errors as the round found them. On 2026-08-25 the round's
numbered list moved here. **`tasks.md` still carries later per-task corrections beside the tasks
that produced them**, so the earlier claim that it no longer carries any was wrong. Count them
yourself, because the number moves:

```sh
grep -c 'Corrected\|corrected\|was wrong\|premise was wrong' tasks.md
```

Among those hits are "This task's premise was wrong twice, and both corrections matter" and
"4 premises in this task were false. Corrected here, do not re-derive them." So the division is:
this section holds the **round's** numbered corrections, and `tasks.md` holds each task's own.

The numbering below is the original numbering and has gaps, because 12, 15, 16, 17 and 20 went
with the records of the tasks that closed and left `tasks.md`. The four `—` rows were never
numbered. **Every correction was found by a subagent that refused a figure, or by a
re-measurement.** That is the point of recording them: the review caught them, not the author.

| # | The claim | The correction |
|---|---|---|
| 13 | One `./checkpatch.sh` run rewrote 5 Markdown files. | The only evidence was mtime, and `markdownlint-fix` moves the mtime of every Markdown file it reads. A re-run on a clean tree gave exit 0 and `md5sum -c` OK for all 5. |
| 14 | — | A concurrency error, not a fact error. The orchestrator edited `tasks.md` while a developer ran `./checkpatch.sh`. The developer saw the file change under its own lint run and correctly reported an unknown writer. |
| 18 | The T-05 baseline is in `/home/labrat/baseline-26.03/`. | That path does not exist. It is `/home/labrat/mtl/baseline-26.03/`. A record that names the wrong path is worth less than no record, because the reader concludes the work was never done. |
| 19 | — | The orchestrator offered `mtl-system-admin` a Bash fallback for T-05 step 3. The agent declined, and it was right: the MCP layer encodes the guardrails for `sudo`, `bind_pmd` and a driver restore on a shared test bed. |
| 21 | The devarg truncation worst case is 63 + 26 + 1 bytes. | `strlen(",rl_burst_size=")` is 15 and `UINT32_MAX` is 10 digits. |
| 22 | A mutation experiment was confounded by out-of-bounds vdev writes. | `dev_eal_init` is `static` with 1 caller, `mt_dev_init` (`lib/src/dev/mt_dev.c:2134`), that no unit test reaches. Those writes are dead at runtime, and the claim had no build log behind it. |
| 23 | Two harness line numbers. | Both wrong. |
| 24 | An off-by-one in `upstreaming.md`. | The correction was off by one itself. `lib/src/mt_pcap.h` closes the `#else` arm at `#endif` on line 57, and line 59 is the header guard. |
| 25 | The Rust edit was 1 line because the new field takes tail padding. | Padding is why the ABI holds, not why the edit was 1 line. `no_std.rs` builds the struct as a literal, and a Rust `#[repr(C)]` literal is exhaustive, so any new field forces a line there whatever the layout does. |
| — | T-29 waits on "whether 26.07 supersedes 26.03", which is decision D6. | D6 is the branch choice. **D5** settles T-29: `patches/dpdk/26.03/` stays in the tree, so the task is actionable now. |
| — | T-28's second file is `patches/dpdk/21.08/0010-Change-to-enable-PTP.patch`. | That file does not exist. It is `0010-Add-init-time-to-sync-PHY-timer-with-primary-timer.patch`. `grep -rln` over `patches/` gives 2 files, so only the filename was wrong. |
| — | The patchwork links in `upstreaming.md` §2 name 26.03 `0009` and `0010`. | They use 26.03 numbering and name `0005` and `0006`. Gate 5 pass three inherited the same mis-pairing and asked to restore a `Cc: stable@dpdk.org` on the never-submitted pair. The ruling was not flipped a third time on evidence that had not changed. |
| — | `hdr_split/0001` and `windows/0001` have stale index lines. | Both measurements applied the 2 optional directories to 1 tree, and no documented flow does that. Each optional patch has exactly 1 real pre-image: the 9 flat patches. T-21 records the rule. |

Two further facts came out of the same class of mistake and now govern how the work runs.
`tasks.md` records both under `## RULES`, in `### How the work runs`.

1. **The Markdown hooks do change bytes, but only where a rule is really broken.**
   `markdownlint` collapsed a code span in the orchestrator's own prose and turned the sentence
   into nonsense. `textlint` rewrote `id` to `ID` inside a sentence another agent had written
   minutes earlier. So hash the file, then diff. Do not read the mtime and guess.
2. **Gate 5 overruled its own earlier instruction once, in the developer's favour.** Pass 3
   asked for a C comment that cited line numbers in **`upstreaming.md` §6**, the burst-size
   section — `grep -n '^## 6\.' upstreaming.md`, not this file's own §6. Pass 4 required the
   comment to name the vdev branches by devarg prefix
   instead, because the diff had moved 3 of those citations by inserting 9 lines. A review
   instruction can rot the same way a citation can.
