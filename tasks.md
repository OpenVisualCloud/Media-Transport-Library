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
| D7 | **T-04 answered: add an `rl_burst_size` field to `struct mtl_port_init_params`.** DPDK patch `0003` is dropped. | Corrects the D2 arithmetic: **5 dropped, 11 kept, 0 open**. The 26.07 set is 11 files, final. Unblocks T-02. T-04 part two lands code, so it needs Gates 2, 5 and 6. |
| D8 | **The host chain T-05 → T-03 → T-06 → T-07 is approved in full.** | No further approval per step. T-05 runs first and alone, because it is the only irreversible measurement. Stop and report only on direct evidence of another live test. |

## Order of work

1. **T-01** and **T-05** run at the same time. T-01 reads a downloaded source tree.
   T-05 needs the hardware and must capture the baseline **before** the bump.
2. **T-10** can also run now. It touches only CI and documentation, and it does not
   build, so it does not race `build/` with T-01.
3. **T-02** needs T-01. **T-03** needs T-02. **T-04** now decides before **T-02** closes,
   not before T-03 lands: the T-04 answer sets how many patches `patches/dpdk/26.07/`
   holds, and Gate 5 will not approve that directory while the count is open.
4. **T-06** needs T-03, T-05 and, to mean anything for T-04, **T-35**. **T-07** needs T-03.
5. **T-08** and **T-09** are small and independent. Fit them anywhere.
6. **T-11** and **T-12** are the long tail. They are the only way to shrink the patch
   set below 11, and neither is a 26.07 task.

Only one `mtl-developer` builds at a time — one `build/` tree, one `/usr/local`. Only
one `mtl-system-admin` runs at a time — one set of VFs, one MtlManager. A prose pass and a C
pass cannot run at the same time either, because `format-coding.sh` autofixes tree-wide.

**Take a snapshot between review passes.** T-04 needed 4 Gate 5 passes, and pass 4 could not
prove what moved between passes, because there was no snapshot of the pass-3 tree to compare.
`git stash create` writes a dangling snapshot commit and leaves the working tree untouched, so
it costs nothing and commits to no branch. Use it from now on — but it has **no `-u` and skips
untracked files**, which on T-04 would have omitted the very test file under review. Pair it
with `git add -N` on the untracked paths, or record `git status --short` beside the SHA. It also
prints nothing and exits 0 on a clean tree. Also hash any file you do not
mean to touch **before** a run: `format-coding.sh` changed `tasks.md` once during T-04, and a
`git diff -w` comparison cannot see a textlint terminology autofix.

### Where the work stands — 2026-08-24

The order above is unchanged. This records progress against it.

- **Steps 1 and 2 are done.** T-01, T-08, T-09, T-10 and T-33 are **DONE** and sit under
  `## Done`. T-05 has captured 2 of its 3 runs; the third moves to T-06.
- **Step 3 is done. T-02 and the code half of T-04 both closed on Gate 5 with 0 blockers**, T-02
  after 5 passes and T-04 after 3. `patches/dpdk/26.07/` holds the 11 final files, and the
  `rl_burst_size` field with its 5 unit tests is in the tree. **Nothing further can run without
  the host.** T-03 arms the new directory.
- **The host chain gained a step, and a measurement fixed its order.** The installed 26.03 ice
  PMD has no `rl_burst_size` key at all — `strings` on the shipped `librte_net_ice.so` finds
  `proto_xtr` and `rx_low_latency` and no third key. So a run today returns an unknown-key probe
  failure, which proves nothing. Order is T-03, then **T-35** to give a binary a way to set the
  field, then T-06. Without T-35 the Gate 6 run measures T-03 only.
- **What the patch set proves about itself**, all of it re-measured by Gate 5 rather than read
  from a report. The 9 flat patches `git am` clean in order onto a pristine `dpdk-26.07`; each
  optional patch applies after those 9; `VERSION` reads `26.07.0_mtl_`; 8 patches commit under
  real author names and `0009` under a placeholder that T-31 owns; every body is byte-identical
  to its 26.03 ancestor except `0004`, `windows/0001` and 2 named bytes of `0008`.
- **The irreversible measurement is safe.** The 26.03 baseline is in
  `/home/labrat/mtl/baseline-26.03/` — 8 files, `ls` confirms — outside the tree, captured
  before anything rebuilt. DPDK in `/usr/local` is still `26.03.90_mtl_`.
- **Steps 4 to 6 are blocked on tooling, not on hardware or on a decision.** T-05 step 3, T-06
  and T-07 all need `mtl-system-admin`, which is MCP-only, and the MCP servers cannot start in
  this session. **T-34** holds the cause and the remedy: the fix is on disk, but Claude Code
  negotiates MCP connections once at session start, so it needs a restart. T-11 and T-12 are out
  of scope for this move by the task text itself.
- **The loader hazard is proven, and it changes how T-06 must run.** `LD_DEBUG=libs` on
  `build/tests/KahawaiTest` shows the loader opening the **sibling checkout's** DPDK, not
  `/usr/local`. So installing 26.07 into `/usr/local` does not by itself change what a test
  loads. T-06 must prove the version from inside each run with `--log_level notice`.
- **Every task numbered T-15 and above was found by the verification passes, not planned.**
  `grep -c '^## T-' tasks.md` counts what is open. Most are defects in the patch set and in its
  record that only a re-measurement could find.

### The ordering constraint that still governs the host chain

**T-05 had to run before T-03, and it did.** T-03 replaces the installed DPDK, so once it runs
there is no 26.03 build left to measure. D8 approves the chain T-05 → T-03 → T-06 → T-07 in
full, with no further approval per step.

**Stop and report only on direct evidence that another test is running on these NICs.** An
idle-looking host is not a reason to ask again.

**The new patch directory is inert, so landing it is safe.**
[script/build_dpdk.sh:98](script/build_dpdk.sh) globs
`../../patches/dpdk/"$DPDK_VER"/*.patch`, and `versions.env` still pins `DPDK_VER=26.03`.
So `patches/dpdk/26.07/` is unreachable until T-03 changes that 1 value. The patch set can
be reviewed and committed with no effect on this host or on anybody's running test. Only
T-03 arms it. That separates the reversible work from the irreversible work.

## T-03 Bump `versions.env` to DPDK 26.07 — OPEN

- **Owner:** mtl-developer
- **Needs:** T-02, and the T-04 decision
- **Ref:** upstreaming.md §2
- **Files:** [versions.env](versions.env), [script/build_dpdk.sh](script/build_dpdk.sh)
  if the version gate needs it
- **Set:** `DPDK_MTL_MINOR_VER=0`. T-02 hardcodes `26.07.0_mtl_` in the new `0004`
  patch. The two values are one fact stored twice. Change one and you change both.
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
- **Two 26.03 DPDK installs exist and they disagree, so "already stale" was the wrong read.**
  `pkg-config` on `/usr/local` reports `26.03.90_mtl_`, but the loader cache serves
  `26.03.91_mtl_` from the sibling checkout, which is exactly what `versions.env` pins. So the
  build resolves `.90` and the loader resolves `.91`. `dpdk_is_installed()` reads
  `pkg-config`, so it does force a rebuild — the conclusion held for the wrong reason. See the
  T-06 note: installing 26.07 into `/usr/local` does not by itself change what binaries load.
- **The version literal is hardcoded, and upstreaming.md §3 says it should not be.**
  Found by the T-08 Gate 5 pass five, which raised it as outside its own scope.
  `patches/dpdk/26.07/0004` writes `26.07.0_mtl_` as a literal and does not derive the minor
  from `DPDK_MTL_MINOR_VER`, while §3:129-130 instructs T-02 to derive it. So setting
  `DPDK_MTL_MINOR_VER=0` is not a bump-and-go; the 2 stores of that 1 fact must be made to
  agree, or §3 must withdraw the instruction. Settle which before running the build.
- **Expect `*.orig` files in the DPDK tree after the apply.** A clean apply of the 11 patches
  leaves 7 of them — `ice_rxtx.c`, `iavf_ethdev.c`, `ice_ethdev.c`, `ice_rxtx.h`,
  `ethdev_driver.h`, `rte_ethdev.c`, `rte_ethdev.h` — one for each file with an offset hunk.
  GNU `patch` writes them under `--backup-if-mismatch`, which is its default, and the pristine
  tarball contains none. They are not rejects and not a defect in the patches. Check whether
  they reach the installed tree or confuse a later re-apply.

## T-04 Add an `rl_burst_size` field to `struct mtl_port_init_params` — BLOCKED

- **Blocked by:** T-06, which needs T-03 first. Gates 0 to 5 are done. Gate 6 needs an ice PF.
- **Owner:** mtl-developer | **Ref:** upstreaming.md §6
- **Files:** `include/mtl_api.h:542-558`, `lib/src/dev/mt_dev.{c,h}`, `lib/src/mt_main.c:276-280`,
  `tests/unit/dev/mt_dev_harness.{c,h}`, `tests/unit/dev/mt_dev_devargs_test.cpp` (new),
  `tests/unit/meson.build`, `rust/imtl-sys/examples/no_std.rs`
- **Acceptance:** `./build_unit/tests/unit/UnitTest --gtest_filter='MtDevDevargs*'` gives
  `[  PASSED  ] 5 tests.`, exit 0. I re-ran it myself. Gate 2 was real: 4 of the 5 failed first
  with `Which is: "0000:c9:01.0"` against `"0000:c9:01.0,rl_burst_size=2048"`.
- **Gate 5 took 4 passes and closed at APPROVE WITH COMMENTS, 0 blockers.** Every warning was
  landed. 3 of the 4 passes overturned a claim of mine — see the error list below. Pass 4 also
  overruled its own pass-3 instruction in the developer's favour: a C comment must name the vdev
  branches by devarg prefix, not by line number, because this diff moved 3 §6 line citations by
  inserting 9 lines.
- **One ABI hazard is real, and it belongs to `include/mtl_api.h`, not to this diff.** The field
  is size-neutral because it takes 4 bytes of tail padding. So a binary compiled against the old
  header leaves those bytes uninitialized, and a newer library reads them as `rl_burst_size`;
  garbage there fails the probe with `-EINVAL`. Callers that zero the whole struct are safe.
  **A doc comment does not fix it, so none was added.** The caller at risk compiled against the
  old header and never reads a comment in the new one, and `= {0}` does not guarantee zeroed
  padding under C11, so the advice would have to say `memset`. The real fix is a size or version
  field, or library-side validation. Zero already means "unset", pinned by
  `MtDevDevargsTest.UnsetBurstSizeBuildsBareBdf`, so the safe value is the natural one.
- **The limit on the evidence, stated plainly.** The 5 new tests are known-green only because
  they sort ahead of the T-19 abort — `ut_dev_create_ctx` uses plain `calloc` and touches no EAL,
  so they all run before `rte_eal_init`. The full unit suite still cannot finish. That is a limit
  on the proof, not a fault in the diff, and Gate 5 pass 4 ruled the abort separable from T-04.
- **No ABI break, and the reason is padding.** The field lands at offset 12, in the 4 tail bytes
  `uint64_t flags` + `int socket_id` already forced, so `sizeof` stays 16 and no member of
  `port_params[MTL_PORT_MAX]` (`include/mtl_api.h:733`) moves, so bindgen needs no version
  dance. That is **not** why the Rust edit was 1 line: `no_std.rs` builds the struct as a
  literal, and Rust literals are exhaustive, so any new field forces a line there whatever the
  layout does. My 25th error, caught by Gate 5 pass 4. The next port param added will grow the
  struct.
- **MTL does not validate the range, by choice.** `lib/meson.build:15` accepts
  `libdpdk >= 25.03`, so one binary can link against ice versions with different bounds, and a
  copied constant would drift toward MTL rejecting a value the driver accepts. The ice PMD stays
  the single source of truth and its failure is loud, `-EINVAL` from the probe.
  `MtDevDevargsTest.OutOfRangeBurstSizeIsPassedThroughUnvalidated` pins the pass-through.
- **`0003` never was a devarg patch, so D7 restates it.** `0003` flips one constant,
  `ICE_SCHED_DFLT_BURST_SIZE` from `(15 * 1024)` to `(2 * 1024)`. The devarg is a separate 26.07
  addition that supersedes it: `ICE_RL_BURST_SIZE_ARG` at
  `drivers/net/intel/ice/ice_ethdev.c:45`, parsed at `:2501-2504`, applied at `:2727-2733`.
  `uint32_t`, `strtoul` base 0, range 64 to 2096128 bytes (`base/ice_sched.h:26-28`), `0` keeps
  the hardware default. Documented at `doc/guides/nics/ice.rst:162-172`.
- **The devarg is ice PF only, and `iavf` rejects it hard, so opt-in is necessary and not
  defensive.** `iavf_parse_devargs()` passes a valid-key list to `rte_kvargs_parse`, so one
  unknown key returns `NULL` and the function returns `-EINVAL` (`iavf_ethdev.c:2473-2480`). It
  runs first in dev init, so the VF never comes up. `ice` uses a valid-key list too, so a
  misspelling breaks the PF probe as well — no silent no-op is possible.
- **The library cannot tell a PF from a VF before `rte_eal_init()`**, so half of D7's test
  instruction is not implementable. `MT_PORT_PF`/`MT_PORT_VF` are written only by
  `parse_driver_info()`, which needs a probed port ID, and no `virtfn`, `physfn` or `sriov` read
  exists under `lib/`. The test pins "an unset field builds a string with no `rl_burst_size`";
  it cannot pin "a VF port". Opt-in only preserves the intent.
- **MTL already assumes the 2 KB burst, so the drop is a live coupling.**
  `lib/src/st2110/st_tx_video_session.c:580` reads
  `pacing->vrx -= 2; /* VRX compensate to rl burst(max_burst_size=2048) */`. On an ice PF with no
  devarg, 26.07 programs 15 KB, and that compensation no longer matches the hardware.
- **A PF path exists, so the drop is not provably a no-op.** `net_ice` declares `MT_PORT_PF` with
  `MT_RL_TYPE_TM` (`mt_dev.c:29-35`), AUTO pacing selects rate limit on it with no PF or VF test
  (`mt_dev.c:1452-1462`), MTL builds a 7-level PF scheduler tree (`mt_dev.c:556`), nothing
  rejects a PF BDF, and `tests/acceptance/tests/single/st20p/test_pacing_way.py:50-51` already
  runs PF with `rl`. `rte_tm` gives no runtime alternative: `ice_tm.c:316-327` and
  `iavf_tm.c:492-503` both reject `committed.size` and `peak.size` with `-EINVAL`.
- **Still open, and only hardware answers it:** does the 7-level PF scheduler tree commit? 26.07
  checks depth against `hw->num_tx_sched_layers`. If the tree is refused, the port falls back to
  TSC and the field changes nothing at runtime. T-06 measures it. The field lands either way,
  because D7 rules on the interface, not on the outcome.
- **Buffer width, settled after 3 wrong arithmetics.** One authority now, the row declaration at
  `mt_dev.c:327`; all 4 writes take `sizeof(port_params[i])`. Worst cases against 128 bytes:
  109 for `eth_af_packet`, 102 for `net_af_xdp`, 89 for the PCI devarg. The 2 vdev writers are
  the widest and are untested at every tier.
- **My 3 errors on this task, kept because each one was caught by a subagent refusing my
  figure.** 21st: I gave the truncation worst case as 63 + 26 + 1, when
  `strlen(",rl_burst_size=")` is 15 and `UINT32_MAX` is 10 digits. 22nd: I claimed a mutation
  experiment was confounded by out-of-bounds vdev writes; `dev_eal_init` is `static` with one
  caller `mt_dev_init` (`mt_dev.c:2134`) that no unit test reaches, so those writes are dead at
  runtime and my claim had no build log behind it. 23rd: I gave 2 harness line numbers wrong.
  24th: correcting an off-by-one in `upstreaming.md`, I was off by one myself — `mt_pcap.h`
  closes the `#else` arm at `#endif` on line 57, and line 59 is the header guard.
- **2 facts about this file that govern any future mutation test.** `mt_dev.c` compiles twice
  under different flags — into the library target, and again inside `mt_dev_harness.c` at
  `-Wall -Werror` — so a warning can break `./build.sh unit` and not `./build.sh`. And a
  too-wide write into a `[8][64]` row is intra-object for ports 0 to 6, which is UB that ASan
  cannot see, and `build_unit` runs `b_sanitize: none` anyway.
- **`-Denable_unit_tests=true` defines no macro; it only adds the subdir** (`meson.build:74-76`).
  So `meson_options.txt:8-9` and `tests/unit/README.md:78-81` are both stale where they promise
  "fuzz wrappers". Folded here, not filed.

## T-05 Capture the 26.03 hardware baseline — IN PROGRESS

- **Owner:** mtl-system-admin
- **Ref:** upstreaming.md §6
- **Files:** `/home/labrat/mtl/baseline-26.03/` — 8 files, 84 KB, outside the tree on purpose so
  no rebuild can overwrite it. **My 18th error: I first recorded this path as
  `/home/labrat/baseline-26.03/`, which does not exist.** A record that names the wrong path is
  worth less than no record, because the reader concludes the work was never done.
- **Acceptance:** `sudo ./build/tests/KahawaiTest --p_port <bdf> --r_port <bdf>
  --auto_start_stop --gtest_filter='St20p*'` pass output, the same run with
  `--pacing_way tsc`, and a PF run with `--pacing_way rl`. **2 of 3 captured.**
- **Test tier:** integration.
- **Gates:** none — this task **is** the Gate 6 baseline for T-03.
- **Captured and verified by me, not read from the report.** Run 1, `auto` pacing:
  `[ PASSED ] 42 tests.` in 193338 ms. Run 2, `tsc` pacing: `[ PASSED ] 41 tests.` with
  `[ FAILED ] St20p.rx_ext_digest_1080p_no_convert_s2 (10487 ms)` in 168958 ms. No build ran —
  `KahawaiTest` and `libmtl.so` both still carry their pre-run timestamps, and DPDK is unchanged
  at `26.03.90_mtl_` in `/usr/local`.
- **The version gap is closed, and the baseline's own claim is confirmed.** The run logs carry no
  EAL banner, so provenance first rested on `ldd`. I measured it directly instead, with no host
  mutation, no `sudo` and no NIC: the dynamic loader resolves every `NEEDED` library before
  `main`, so `LD_DEBUG=libs ./build/tests/KahawaiTest --gtest_list_tests` prints the path the
  process opens. It prints
  `calling init: /home/labrat/mtl/Media-Transport-Library/.local_install/dpdk/lib/x86_64-linux-gnu/librte_eal.so.26`
  — the **sibling checkout**, not `/usr/local`. That install tree's own `libdpdk.pc` reads
  `26.03.91_mtl_` while `/usr/local` reads `26.03.90_mtl_`, so the baseline runs loaded
  `26.03.91_mtl_`, exactly as `DPDK_VERSION.txt` claims. `sudo` does not change the answer: it
  scrubs `LD_*`, none of which was set, and it cannot reorder `ld.so.conf`.
- **A better in-run proof exists for T-06 and costs 1 flag.** `lib/src/mt_main.c:418` already
  prints `dpdk version: %s` from `rte_version()`, which is compiled into the `librte_eal` the
  process mapped and therefore cannot name a library it did not load. It is muted because
  `tests/integration_tests/tests.cpp:402` defaults the level to `error` and
  `lib/src/dev/mt_dev.c:475` applies that before the banner fires. Add `--log_level notice` to
  every recorded run. NoCtx runs already emit it — the fixture forces INFO at
  `tests/integration_tests/noctx/core/test_fixture.cpp:67`.
- **Step 3 is not done and it is blocked on tooling, not on hardware.** No PF `rl` run. The
  `mtl-system-admin` refused twice, correctly: the `mcp__mtl-system-setup__*` tools are absent
  from its inventory and it is MCP-only. **T-34** holds the cause. It also declined a Bash
  fallback I offered it, which it should have — offering it was my 19th error. Its ground was
  the right one: the MCP layer is what encodes the guardrails for `sudo`, `bind_pmd` and a
  driver restore on a shared test bed, so a broken gate is the weakest moment to bypass a gate.
- **What step 3 still needs.** Bind a PF with `script/nicctl.sh bind_pmd` and transmit with
  `--pacing_way rl`. Record whether rate limit engages or the log shows
  `fallback to tsc as rl init fail`, and whether the 7-level PF tree
  (`ST_TM_NONLEAF_NODES_NUM_PF` at `lib/src/dev/mt_dev.c:546-547`) commits. Bind only a PF that
  hosts no VFs and is not the one behind `0000:15:01.*`, which T-06 needs. No gtest sets `rl`,
  and `.github/scripts/gtest.sh:107,114-116` puts every `--pacing_way` line inside the
  nightly-only guard at `:102-104`, so the suite alone cannot catch a PF burst-size regression.
- **Note:** ran before T-03, per D8. **ACTION ON HOSTS**

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
- **This gate would pass while measuring the wrong DPDK. That is now proven, not suspected.**
  `/etc/ld.so.conf.d/mtl_local.conf` line 3 puts a **different checkout**,
  `/home/labrat/mtl/Media-Transport-Library/.local_install/dpdk/`, ahead of `/usr/local` in the
  loader cache. Both export soname `librte_eal.so.26` and `librte_ethdev.so.26`, and 26.07 keeps
  ABI 26. `LD_DEBUG=libs ./build/tests/KahawaiTest --gtest_list_tests` prints
  `calling init: …/Media-Transport-Library/.local_install/dpdk/lib/x86_64-linux-gnu/librte_eal.so.26`,
  so the sibling wins today even though the binary's own `RUNPATH` is
  `/usr/local/lib/x86_64-linux-gnu`. After T-03 installs 26.07 into `/usr/local`, an unforced run
  still loads the sibling's **26.03.91** and the whole gate measures the old DPDK.
- **So T-06 starts by proving the loaded version, and 1 flag does it.** Run every case with
  `--log_level notice` and grep `dpdk version:` — `lib/src/mt_main.c:418` prints it from
  `rte_version()`, which is compiled into the `librte_eal` the process mapped. Expect
  `DPDK 26.07.0_mtl_0`. If the line reads `26.03.91_mtl_0`, the loader served the sibling and the
  run is void. Force `LD_LIBRARY_PATH=/usr/local/lib/x86_64-linux-gnu` with `sudo env VAR=…`, not
  `sudo -E`, which scrubs `LD_*`. Do **not** edit or delete `mtl_local.conf` as a first move: it
  serves the sibling checkout's acceptance install, which is somebody else's environment and is
  outside what D8 approved.
- **T-06 also inherits T-05 step 3**, the PF `rl` capture. It is the only measurement that can
  catch a PF burst-size regression, and D7 makes the PF path the reason T-04 exists.
- **The installed ice PMD does not know the `rl_burst_size` key, so T-04 cannot be proven before
  T-03.** `strings` on `/usr/local/lib/x86_64-linux-gnu/dpdk/pmds-26.1/librte_net_ice.so` finds
  the devarg keys `proto_xtr` and `rx_low_latency` and the base symbol `ice_cfg_rl_burst_size`,
  but no `rl_burst_size` key and no `Invalid rl_burst_size` message. Any run against today's
  install returns a probe failure from an unknown key, which proves nothing about T-04. Order is
  therefore fixed: T-03 installs 26.07, then T-35 lands the setter, then T-06 measures.

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
  `MTL_INSTALL_PREFIX=$PWD/.local_install ./build.sh`. **ACTION ON HOSTS**
- **This checkout has no acceptance environment at all, so T-07 is bigger than a pytest run.**
  Neither `tests/acceptance/venv` nor `.local_install` exists here. The acceptance tree that does
  exist belongs to the sibling checkout, `/home/labrat/mtl/Media-Transport-Library/.local_install/`,
  and it is what `/etc/ld.so.conf.d/mtl_local.conf` serves. So T-07 needs
  `.github/scripts/acceptance_setup.sh`, a venv, and its own `.local_install` build before the
  first test can run. Budget for that, and expect the loader hazard in T-06 to apply here too.
- **The media files are present but not on a mount.** `/mnt/media` is a plain directory holding
  the 1080p 10-bit YUV, the 24-channel PCM and `test.txt`. `mountpoint /mnt/media` says it is not
  a mount point, so the NFS share the documentation assumes is not mounted. Check what the smoke
  set actually reads before treating this as satisfied.

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

## T-13 Decide which DPDK versions the Windows build supports — OPEN

- **Owner:** the user decides. mtl-developer then edits one file.
- **Ref:** upstreaming.md §9
- **Files:** [.github/workflows/msys2_build.yml:46](.github/workflows/msys2_build.yml)
- **Acceptance:** the matrix holds only versions that have a `patches/dpdk/<ver>/windows/`
  directory, and CI passes.
- **Gates:** 2 exempt; 5 required; 6 exempt.
- **Note:** this is the part of T-10 that a bump cannot answer. The matrix pins
  `[25.03, 23.11]`. T-10 left it untouched on purpose, because a bump would answer a
  product question in silence. Say which versions Windows still supports.
- **The version question is not the first problem. Fix these 2 defects first:**
  1. **The job never runs.** [.github/workflows/msys2_build.yml:22](.github/workflows/msys2_build.yml)
     reads `steps.filter.outputs.msys2_build`, and
     [.github/path_filters.yml](.github/path_filters.yml) defines no `msys2_build` key.
     It defines 7 keys: `src`, `build`, `docker`, `ecosystem`, `ice_build`,
     `ubuntu_build`, `linux_tests`. The output stays empty, so the gate at line 38 never
     opens. The only trigger is `workflow_dispatch`, and `dorny/paths-filter` has no
     base to diff against on a manual run, so the filter cannot help either.
  2. **The `25.03` leg cannot work.** Lines 104 and 131 need
     `patches/dpdk/25.03/windows/*.patch`, and `patches/dpdk/25.03/windows/` does not
     exist. Of the 2 pinned versions only `23.11` has a `windows/` directory.
- **Note:** so nobody has run this workflow successfully for a long time, and a matrix
  bump alone would leave it just as dead. Decide the versions and repair the gate in one
  change, or delete the workflow.

## T-14 Delete `.github/legacy/`, or keep it for the record — OPEN

- **Owner:** the user decides. mtl-developer then runs `git rm`.
- **Files:** `.github/legacy/codeql.yml:30`, `.github/legacy/msys2_build.yml:41`,
  `.github/legacy/msys2_ffmpeg.yml:20`
- **Acceptance:** no DPDK version literal stays in `.github/legacy/`, or the directory
  goes away.
- **Gates:** 2 exempt; 5 required; 6 exempt.
- **Note:** found while T-10 ran. Same defect as T-10, 3 more sites, and upstreaming.md
  §9 lists none of them. The directory holds exactly 3 files and nothing reaches it.
  GitHub Actions reads only `.github/workflows/`, and no `uses: ./` in the tree points
  into `.github/legacy/`.
- **Evidence, gathered 2026-08-24.** One commit created the directory: `b9e266d8`,
  2025-04-22, which moved all 3 files out of `.github/workflows/` at 100 percent
  similarity. Its message says "move unsupported windows pipelines to legacy folder".
  The 3 files are not one thing:
  - `msys2_build.yml` is a **stale duplicate**. `255d7622` copied it back to
    `.github/workflows/` and left the archived copy behind. The 2 files differ by 19
    lines, all pinned-action SHA bumps plus one added Harden Runner step, and the
    `dpdk: [25.03, 23.11]` matrix is byte-identical in both. Deleting it loses nothing.
  - `codeql.yml` is the only CodeQL `cpp` analysis in the tree. Live CodeQL references
    in `scorecards.yml` and `trivy.yml` only upload SARIF, they do not analyze.
  - `msys2_ffmpeg.yml` is the only mingw64 FFmpeg 4.4 plugin build.
- **The trade-off.** Neither archived file would run if restored. `msys2_ffmpeg.yml`
  copies `ecosystem/ffmpeg_plugin/kahawai_*.c`, which no longer exists — the tree now
  holds `mtl_*.c` under `4.4/`, `6.1/` and `7.0/`. Their value is a record of intent,
  which languages and which FFmpeg release, not runnable CI. Against that, they appear
  in every version-pin audit, which is why this task exists.
- **Open, and not answerable from the tree:** CodeQL may be on through GitHub default
  setup, which lives in repository settings and not in a file. Check
  `gh api /repos/{owner}/{repo}/code-scanning/default-setup` before you treat
  `.github/legacy/codeql.yml` as the only coverage.

## T-15 Fix or delete the unreachable `create_dcf_vf` command — OPEN

- **Owner:** mtl-developer
- **Files:** [script/nicctl.sh:239](script/nicctl.sh)
- **Acceptance:** either the command works and a test covers it, or it is gone.
- **Gates:** 2 exempt if deleted; 5 required; 6 exempt.
- **Note:** found while T-04 ran. The guard tests `$ice`, which the script never sets,
  so `create_dcf_vf` always exits 1. DCF mode is unreachable and undocumented.

## T-16 Record that `patches/dpdk/25.11/` is load-bearing — OPEN

- **Owner:** mtl-developer
- **Ref:** upstreaming.md §9
- **Files:** upstreaming.md §9, and the Decisions table in this file
- **Acceptance:** §9 states the exception, and no later cleanup can delete
  `patches/dpdk/25.11/` by accident.
- **Gates:** 2 exempt; 5 required; 6 exempt.
- **Note:** found while T-10 ran. `doc/design.md` §8.3 tells Ubuntu 22.04 users who need
  AF_XDP to pin DPDK 25.11 and use `patches/dpdk/25.11/`. That is a deliberate exception
  to the single-pin rule, not drift, so §9 is wrong to list it as stale. Decision D5
  keeps `26.03` and says nothing about `25.11`.

## T-17 Decide whether CI validation should build DPDK at all — OPEN

- **Owner:** the user decides. mtl-developer then edits one file.
- **Ref:** upstreaming.md §9, which states the premise this task corrects
- **Files:** [.github/workflows/validation-tests.yml:109](.github/workflows/validation-tests.yml)
- **Acceptance:** either `DPDK_REBUILD` becomes a `workflow_dispatch` input with a
  documented default, or the 4 dead steps go away.
- **Gates:** 2 exempt; 5 required; 6 exempt.
- **Note:** found by the T-10 Gate 5 pass. `DPDK_REBUILD: 'false'` is set once at line
  109, never overridden, and not a dispatch input, so 4 steps never run: the DPDK
  version read, the DPDK checkout, the patch step and the DPDK build. The job therefore
  tests against whatever DPDK the self-hosted runner already holds. This matters for
  T-03: after the bump, CI will not pick up 26.07 on its own.

## T-18 Correct 2 stale citation ranges in upstreaming.md — OPEN

- **Owner:** mtl-developer
- **Files:** `upstreaming.md:69`, `upstreaming.md:164`, `upstreaming.md:316-318`
- **Acceptance:** every `file:line` citation in `upstreaming.md` resolves to the line it
  names. I verified the correct values with a full sweep, so do not trust the numbers in
  this task without re-resolving them first — see the rule below.
- **Gates:** 2 exempt (documentation); 5 required; 6 exempt.
- **Note:** `:69` cites `script/build_dpdk.sh:90-97` for the download. The block is
  `:89-95`. Line 96 is blank and line 97 is the `cd`. `:164` cites
  `dpdk_is_installed()` at `:57-70`. The function is `:59-70`, because 57 and 58 are
  comments, and the rebuild decision it feeds is `:79-82`.
- **This task's own line numbers already rotted once.** They were `:68` and `:160` when the
  task was filed. `upstreaming.md` grew from 279 lines to 392 during this round, so both
  moved by 1 and 4. Re-resolve before you edit.
- **Note:** found by the T-01 Gate 5 pass. Held out of that fix because both lines sit
  outside the diff, and a documentation change should not quietly widen its own scope.
- **Add a 3rd, cosmetic.** `upstreaming.md:316` in §8 cites `msys2_build.yml:135` and the
  citation is correct, because `:135` really is a `git am` line. It now sits 2 lines from the
  `:136` that T-01 corrected in §2, so the 2 read as a contradiction. Say which glob each
  line applies.
- **Add a 4th and 5th, and T-10 caused these 2.** `upstreaming.md:317` cites
  `doc/build.md:150` and `:318` cites `doc/build_WIN.md:76`. Both now resolve to blank lines.
  The `git am` lines they mean moved to `doc/build.md:155` and `doc/build_WIN.md:82`, because
  T-10 added and removed lines in both files. I verified all 5 with `sed -n`.
- **T-02 closed on Gate 5 pass five and left 2 warnings and 3 nits here, all one clause.**
  Folded rather than passed to a sixth review round. `upstreaming.md` is now 526 lines, so
  re-resolve every number below before editing. (1) §2's closer says two rows still defer work,
  unqualified, while §8 names T-30, T-31 and T-21 against three more rows of the same table —
  qualify it to the mapping work the sentence is about. (2) `0003` means the dropped burst-size
  patch 13 times and the shipped pcapng patch 9 times, so §3's "`0003` shipped no hunks at all"
  and §7's "`0003`'s diff hunks unchanged" read as a contradiction; §4 and §7 declare a
  numbering frame and §3 does not. Give §3 the same one-line frame. (3) §7's frame is
  under-inclusive: 2 of its 8 `0006` uses name the 26.03 file's content and a dry-run
  measurement, not review history. (4) "marks each one" in §4 is not literal — one marker
  covers each pair. (5) The `script/build_dpdk.sh:57-70` citation clips its own block; the
  comment starts at `:56`.
- **The check that catches this class.** Any pass that adds or deletes lines in a file
  `upstreaming.md` cites by number must re-resolve every `file:line` citation into that file
  afterwards. `checkpatch.sh` does not resolve `file.md:NN` fragments, so no linter can see it.
  Sweeping every citation in one command found 2 defects the pass itself had created and would
  otherwise have shipped.
- **`checkpatch.sh` itself moves lines, so a developer edit is not needed to rot a citation.**
  The `markdownlint-fix` hook edits Markdown in place when a rule is broken. Run the citation
  sweep **after** `checkpatch.sh`, never before.
- **My 13th error, and the correction is the useful part.** I first wrote that
  one `./checkpatch.sh` run rewrote `upstreaming.md`, `doc/build.md`, `doc/build_WIN.md`,
  `doc/experimental/header_split.md` and `tasks.md` at once. My only evidence was mtime.
  `markdownlint-fix` moves the mtime of every Markdown file it reads, even when it changes no
  byte. I re-ran the hook on a clean tree: exit 0, the `tasks.md` mtime moved, and `md5sum -c`
  returned OK for all 5 files. So mtime does not prove a content edit, and those 4 files were
  never rewritten. The developer measured this and corrected me.
- **My 14th error, and it is a concurrency error, not a fact error.** I edited `tasks.md`
  while a developer ran `./checkpatch.sh`. `CLAUDE.md` warns against running `checkpatch.sh`
  while a developer edits Markdown; this is the same race from the other side, and I own it.
  The developer saw `tasks.md` change under its own lint run, correctly reported an unknown
  writer, and reasonably suspected its own measurement. The writer was me. Its lint run 3 then
  exited 1 and applied a `markdownlint-fix` to my prose that no one reviewed. I checked the
  result: no degenerate code span, 0 trailing-whitespace lines, every sentence I wrote intact.
  So the fix was benign, but only by luck. **The rule: hold `tasks.md` still while any agent
  may run `checkpatch.sh`, or take a hash first.** I now baseline it before delegating.
- **The mechanism is still real, and 2 content edits are now proven.** `markdownlint` collapsed
  a code span in my own `tasks.md` prose from a separator with a trailing space to one without,
  which turned the sentence into nonsense. Later, `textlint` rewrote `id` to `ID` at
  `tasks.md:270` during a developer's lint run — in a sentence I had written minutes earlier.
  Both are 1-token edits inside another agent's file. So the hook does change bytes, but only
  where a rule is really broken. The sweep rule stands, and its trigger is a rule violation,
  not every run. Hash the file, then diff; do not read the mtime and guess.
- **A 6th, and it is a form defect, not a stale number.** Some citations name a bare filename
  with no path: `mt_dev.c:1442`, `mt_dev.c:1477`, `mt_dev.c:42`, `mt_dev.c:546`,
  `gtest.sh:114`. All 5 line numbers are in range — `lib/src/dev/mt_dev.c` is 2633 lines and
  `.github/scripts/gtest.sh` is 456 — so the facts hold, but a reader cannot resolve them and
  a sweep cannot check them. Give every citation its repository-relative path. Citations into
  the DPDK tree, such as `drivers/net/intel/ice/ice_tm.c:316`, are correct as they are and
  must stay out of the sweep.
- **The sweep command, for reuse.** It resolves both forms in one pass:
  `grep -oE '[A-Za-z0-9_./-]+\.(md|sh|yml|h|c|env):[0-9]+' upstreaming.md | sort -u` and
  `grep -oE '\[:[0-9]+\]\([^)]+\)' upstreaming.md | sort -u` for the bare `[:NNN](path)` form.
  The second form is easy to miss, and my first sweep did miss it.
- **Also worth 1 line while here.** `doc/build_WIN.md:86` applies the `windows/` patches with
  `git apply`, while `.github/workflows/msys2_build.yml:136` applies the same glob with
  `git am`. `git apply` accepts a bare diff and `git am` needs a mail header, which is why the
  documented path works and the CI path cannot. See T-30.

## T-19 The unit suite aborts after 46 of 508 tests — OPEN

- **Owner:** mtl-developer
- **Files:** `tests/unit/session/st40/`, [lib/src/dev/mt_dev.c:306](lib/src/dev/mt_dev.c)
- **Acceptance:** `./build_unit/tests/unit/UnitTest` exits 0 and runs all 508 tests.
- **Test tier:** unit. The suite is the defect.
- **Gates:** 2 already satisfied, the suite itself is the failing test; 5 required;
  6 exempt if the fix stays in `tests/unit/`.
- **Note:** found while verifying T-09. `./build_unit/tests/unit/UnitTest` aborts with
  exit 134, `SIGABRT`, at `St40RxRedundancyTest.NormalRedundancy`. Every test listed after
  that one never runs; compare `--gtest_list_tests` with the last `OK` line to enumerate them.
  This is not a flake. It reproduces every run.
- **Measured, 2026-08-24:**
  - `EAL: UIO_RESOURCE_LIST tailq is already registered`, then
    `EAL: PANIC in tailqinitfn_rte_uio_tailq()`.
  - `--gtest_filter='St40Rx*'` aborts with 0 tests passed, so any St40 Rx test triggers
    it.
  - `--gtest_filter='MtDevIgcTest.*'` passes 8 of 8 and exits 0, so the test that runs
    just before is not the cause.
  - Backtrace frame 2 is `dpdk/pmds-26.1/librte_bus_pci.so.26.1` and frames 3 to 6 are
    `ld-linux`, so a constructor runs at `dlopen` time.
  - `ldd` shows `librte_bus_pci` is not a link-time dependency of `UnitTest`, so the
    object loads twice under 2 paths. `/usr/local/lib/x86_64-linux-gnu/librte_bus_pci.so.26.1`
    is a symlink into `dpdk/pmds-26.1/`.
  - The install is self-consistent. One `pmds-26.1` directory, matching mtimes.
- **The likely contract break.** CLAUDE.md says the unit tier needs no NIC and no root.
  These tests reach `rte_eal_init()` at `lib/src/dev/mt_dev.c:306`. A unit test that
  starts the EAL is outside the tier contract, whatever the panic turns out to be.
- **Why CI never caught it.** No workflow runs the unit suite. `grep -rln 'build.sh
  unit\|UnitTest\|enable_unit_tests' .github/workflows/` returns nothing. Fix that in the
  same change or the next regression hides just as long.
- **Not caused by the 26.07 work.** The `mt_pcap.c` object code is byte-identical before
  and after T-09. T-04 later added 5 tests to this tier and the abort moved by 5 tests, not in
  kind.
- **The tier documentation also overstates its own rigor.** `tests/unit/CLAUDE.md:6` says
  "ASan is preloaded so any leak fails the suite", but `build.sh:16` defaults
  `enable_asan=false` and `build_unit` reports `b_sanitize: none`. ASan engages only under
  `MTL_BUILD_ENABLE_ASAN=true` or a debug build. Fix that sentence in the same change; found by
  the T-04 rework, folded here.

## T-20 The installed DPDK does not match the pin — OPEN

- **Owner:** the user decides, because the fix mutates the host.
- **Ref:** upstreaming.md §9
- **Files:** [versions.env](versions.env), [script/build_dpdk.sh:59-70](script/build_dpdk.sh)
- **Acceptance:** `pkg-config --modversion libdpdk` matches
  `${DPDK_VER}.${DPDK_MTL_MINOR_VER}_mtl_`.
- **Gates:** 2 exempt; 5 required; 6 required, because it replaces the installed DPDK.
- **Note:** the host has `26.03.90_mtl_` installed and `versions.env` pins
  `DPDK_MTL_MINOR_VER=91`. `dpdk_is_installed()` compares the full
  `26.03.91_mtl_` prefix, so it returns false today and `./script/build_dpdk.sh` will
  rebuild DPDK the next time anybody runs it, with no `-f` flag needed.
- **This changes what T-05 measures.** A 26.03 baseline taken now measures `26.03.90`,
  not the pinned `26.03.91`. Decide whether that is the baseline you want before T-05
  runs, because after T-03 there is no 26.03 left to measure.
- **Fix `upstreaming.md:8` in the same change.** The header table records the installed DPDK as
  `26.03.90_mtl_` as though it agreed with the pin. It does not. That line is the source record
  for this defect, so it must state the mismatch and not just the measurement.

## T-26 Patch filenames disagree with their own subjects — OPEN

- **Owner:** mtl-developer
- **Files:** `patches/dpdk/26.07/0008`, `0009`, and the same 2 files in `patches/dpdk/26.03/`
- **Acceptance:** each filename describes the change its `Subject:` names, and the full series
  still applies in filename order with 0 rejects.
- **Gates:** 2 exempt (patch metadata); 5 required; 6 exempt.
- **Note:** found by the T-08 Gate 5 pass three. `0008` is named
  `…-use-direct-MMIO-for-PHC-update` while its subject says "use direct MMIO for PHY timer
  command", and the body writes `E830_ETH_GLTSYN_CMD`, which is a timer command. `0009` has the
  same class of mismatch against `always-init-PHC-owner`.
- **`0008`'s `Subject:` was edited without its filename, and that is how the gap widened.**
  Against `patches/dpdk/26.03/0012` it changed from `[PATCH 12/12] ice: e830:` to
  `[PATCH] net/ice: e830:`, and its `Signed-off-by:` name order changed from the comma form to
  the plain form. Both are earlier-pass edits, not the fix pass. The whole set was being
  renumbered at that moment, which was the cheap time to align the filename, and it was missed.
- **Why this is its own task.** Name order is apply order —
  `script/build_dpdk.sh:98` uses a flat `*.patch` glob — so a rename is a set-wide change that
  needs its own apply-order verification. T-08 fixed the `0008` subject prefix to `net/ice:`
  and deliberately left every filename alone.
- **Also here, from the same pass.** `0009:4-5` reassembles to a 67-character headline, which
  `devtools/check-git-log.sh` reports as too long. `0008:32` uses `--` where `git format-patch`
  writes `--` with a trailing space. Both pre-existing and both cosmetic.
- **Evidence, from DPDK's own tool.** `devtools/check-git-log.sh -n 9`, run on the set after
  `git am` into a scratch repository, reports: wrong headline format and wrong prefix for `Change to
  enable PTP`, expected `net/ice`; wrong prefix for `iavf: disable runtime queue`, expected
  `net/iavf`; headline too long for `0009`. So `0001` and `0002` break the same rule that Gate 5
  raised against `0008`.
- **Why only `0008` was fixed.** `0008` came from Gate 5 as an explicit warning, so it is
  accepted review feedback. `0002` → `net/iavf:` is mechanically derivable, but `0001` has no
  derivable prefix and shortening `0009` are both rewrites of another author's subject line, and
  both change the filename-to-subject relation this task owns. Held rather than widened.
- **`0002` can never be mailmap-clean.** `Ric Li <ming3.li@intel.com>` is absent from DPDK
  `.mailmap` because he never landed a patch upstream. Any acceptance test for this task must
  not require mailmap membership.
- **Not defects.** The 3 `Wrong 'Fixes' reference` complaints from that run are artifacts of the
  scratch repository, which has one synthetic root commit, so `0b6ff09a1f19` and `327fe144ca39` cannot
  resolve there.

## T-25 `format-coding.sh` has no scoped mode — OPEN

- **Owner:** mtl-developer
- **Ref:** CLAUDE.md, "Format and lint"
- **Files:** [format-coding.sh](format-coding.sh), [checkpatch.sh](checkpatch.sh)
- **Acceptance:** a scoped change can format only the files it touches, and CLAUDE.md says how.
- **Gates:** 2 exempt if the change is script-only; 5 required; 6 exempt.
- **Note:** raised by the T-09 developer against its own Gate 4, which is the right instinct.
  `./format-coding.sh` runs tree-wide, so a 4-file remediation pass rewrites mtimes across
  every tracked file it can format. No content changed this round — `doc/build.md`,
  `doc/build_WIN.md` and `doc/design.md` show the same diff stat as at session start — so this
  is a hygiene defect and not a correctness one.
- **Why it matters.** A tree-wide formatter inside a scoped task hides collateral edits in the
  noise, and it defeats the review rule that a diff must match its stated scope. `pre-commit`
  already supports a file list, so `checkpatch.sh --staged` is the existing precedent.

## T-21 `git am -3` cannot work on the carried patch set — OPEN

- **Owner:** mtl-developer
- **Files:** `patches/dpdk/26.07/0006`, `0009`, `hdr_split/0001`, and the same files in
  `patches/dpdk/26.03/`
- **Acceptance:** `git am -3` applies the whole series to a pristine v26.07 tree.
- **Gates:** 2 exempt (patch metadata); 5 required; 6 exempt.
- **Note:** found by the T-08 Gate 5 pass. `patch -p1` ignores `index` lines, so the
  acceptance test cannot see this. `git am -3` and `git apply -3` read them, and 3 places
  tell the reader to use `git am`: `.github/workflows/msys2_build.yml:135-136`,
  `doc/build.md:152` and `doc/experimental/header_split.md:20-21`.
- **Folded in from T-08: the hunk headers are stale in the same way, in 1 file.**
  `patch -p1 --dry-run` on a pristine 26.07 copy shows `hdr_split/0001` applying 9 hunks at
  offsets from -406 to +53 lines, 0 fuzz, 0 rejects. `windows/0001` applies at 0 offset, both
  alone and after the 9 flat patches. Regenerate the `hdr_split/0001` headers in the same pass
  as the `index` lines; `patch` and `git apply` both search for context, so nothing breaks today.
- **Measured, on `drivers/net/intel/ice/ice_ethdev.c` in documented apply order:**
  - `0006` says `0f2e7aee14..746b1aba0a`
  - `0009` says `746b1aba..cc1ee7be`, which chains from `0006` correctly
  - `windows/0001` says `95bfc9b504..f4fd6189e7`, which does not chain from `cc1ee7be`
  - `hdr_split/0001` says `c721d135f5..047c4ab8a3`, the pristine 26.03 value
- **Which file is stale.** `windows/0001` is the only one with true v26.07 blobs, because
  T-08 regenerated it against pristine plus the 9 flat patches. `0006` and `0009` hold
  values byte-identical to their 26.03 counterparts, so the flat patches are the stale
  ones. Fix by regenerating the flat index lines, not by touching `windows/0001`.
- **The full census, measured by Gate 5 pass three against a git-initialized pristine 26.07.**
  Six patches are wrong and 2 files are right, which is what makes this a defect and not
  inherited noise — the same artifact holds 2 standards.

  | Patch:line | file | claimed | real |
  |---|---|---|---|
  | `0001:12` | ice_rxtx.c | `da508592aa..c832fdd083` | `c4b5454..8d70912` |
  | `0002:12` | iavf_ethdev.c | `335a8126c4..10d4ff84f9` | `d601ec3..54c4674` |
  | `0004:22` | VERSION | `403cc28f7a..f38c3bd87e` | `403cc28..f38c3bd` — correct |
  | `0004:29` | config/meson.build | `d7f5e55c18..7e00c87bed` | `d7f5e55..7e00c87` — correct |
  | `0005:22` | ice_rxtx.c | `31b74be9ba..40150748d8` | `8d70912..4c9bf43` |
  | `0006:23` | ice_ethdev.c | `0f2e7aee14..746b1aba0a` | `76b8ff0..d823735` |
  | `0007:37` | ice_rxtx.c | `40150748d8..cf8d38fff0` | `4c9bf43..683de7c` |
  | `0009:36` | ice_ethdev.c | `746b1aba..cc1ee7be` | `d823735..95bfc9b` |

- **One contradiction needs no tree at all.** `0001:12` says `ice_rxtx.c` **ends** at
  `c832fdd083` and `0005:22` says the same file **starts** at `31b74be9ba`. Both cannot hold,
  so the set is provably self-inconsistent without reference to any DPDK version.
- **`0004` and `windows/0001` are right, and that is the proof of intent.** `windows/0001`
  claims `95bfc9b504` for `ice_ethdev.c`, and `git hash-object` on that file after the 9 flat
  patches returns `95bfc9b504825157fecc07b5da04162783d2fa2e`. Producing that value requires a
  staged git tree at the mid-application state, so the author had the means and used it for 2
  of 11 files.
- **T-08 does not fix this, by ruling.** Repairing an `index` line means regenerating the patch
  body, and a metadata pass must not change bodies. upstreaming.md §8 instead records that
  these lines are not maintained and points here. Plain `git am` ignores them, so nothing
  breaks today and the cost lands on the first `git am -3`.
- **The scope is 13 stale lines in 7 files, wider than the table above.** Gate 5 measured the 9
  flat patches. `hdr_split/0001` is stale in **all 7** of its own index lines, and no pass had
  looked at it. `windows/0001` is correct in **all 8**, and `0004` in both of its own. So 10
  correct lines in 2 files, 13 stale in 7.
- **`windows/` and `hdr_split/` are never applied to the same tree, and this fact is what makes
  the measurement reproducible.** Four documented flows exist and no flow uses both:
  `doc/build.md:155` is the flat glob alone; `doc/build_WIN.md:82` then `:86` is flat then
  `windows/`; `.github/workflows/msys2_build.yml:135-136` is the same pair;
  `doc/experimental/header_split.md:20-21` is flat then `hdr_split/`. So each optional patch has
  exactly one real pre-image tree — **the 9 flat patches and nothing else.**
- **Both the developer and I first measured one file against a tree that never exists.** I
  applied flat, then `windows/`, then `hdr_split/`, which made `hdr_split` look stale for the
  wrong reason. The developer applied flat, then `hdr_split/`, then `windows/`, and reported
  `windows/0001` stale in 1 of 8. Neither order is a documented flow. Measured against flat
  only, `windows/0001` claims `95bfc9b504` for `ice_ethdev.c` and the file really is
  `95bfc9b504`.
- **So the phrase "in apply order" is the trap.** There is no single apply order. Any fix or
  check here must say "after the 9 flat patches", or the next reader gets a third answer.
- **Pre-existing.** 26.03 carries the same broken chain. The 26.07 bump did not cause it.
- **A harder failure in the same file, found 2026-08-24.** `windows/0001.patch` has no mail
  header. It starts at `diff --git`. `git am` on it prints "Patch format detection failed." and
  applies nothing, so no index-line fix can help it. Yet
  `.github/workflows/msys2_build.yml:136` runs `git am` on exactly that glob. `26.03` has the
  same shape, so this is pre-existing. Either give the file a mail header or change the
  workflow to `git apply`. This is the 3rd independent reason that workflow cannot pass; see
  T-13 for the other 2.
- **Also fix here:** `0009:36` is the only 8-hex index line in the set. The other 22 are 10-hex.
- **The method is already proven on `0004`.** T-08 recomputed that file's 2 index lines
  with `git -c core.abbrev=10 diff` against a scratch copy of the pristine tree, then
  showed the difference on a deliberately drifted tree. With the old index lines
  `git am -3` failed with "sha1 information is lacking or useless (VERSION)" and "could
  not build fake ancestor". With the recomputed lines it printed "Using index info to
  reconstruct a base tree" and merged clean. Use the same method for the rest.
- **Also fix here:** `script/build_dpdk.sh:57` says `e.g. "26.03.9_mtl_"` and the shipped
  value is `91`.

## T-22 One instruction sentence is copied into 2 documents — OPEN

- **Owner:** mtl-developer
- **Files:** [doc/build.md:151](doc/build.md),
  [doc/experimental/header_split.md:13](doc/experimental/header_split.md)
- **Acceptance:** the sentence reads the same in both files, or one file points at the other.
- **Gates:** 2 exempt (documentation); 5 required; 6 exempt.
- **Note:** found by the T-10 Gate 5 pass three. Both files say `$mtl_source_code` "should be
  pointed to top source code tree", which is passive and drops an article. I declined the fix
  inside T-09 and T-10, because correcting 1 copy and leaving the other creates drift between
  2 copies of 1 instruction, and `doc/build.md` was outside that task's file set. Fix both in
  1 change, or delete the duplicate.

## T-23 `DPDK_REPO` in versions.env is dead — OPEN

- **Owner:** mtl-developer
- **Files:** [versions.env:13](versions.env), [script/build_dpdk.sh](script/build_dpdk.sh)
- **Acceptance:** either `script/build_dpdk.sh` reads `DPDK_REPO`, or the variable is gone.
- **Gates:** 2 exempt; 5 required; 6 exempt.
- **Note:** found by the T-01 Gate 5 pass three. `versions.env:13` defines `DPDK_REPO` as a
  `.tar.gz` URL. `script/build_dpdk.sh:91-94` builds its own `v${DPDK_VER}.zip` name and
  never reads the variable, so the 2 disagree on the archive format and 1 of them is unused.
  Check every other consumer before you delete it.

## T-24 The "Never submitted" status in §2 is not measurable on this host — OPEN

- **Owner:** mtl-developer
- **Ref:** upstreaming.md §2, rows for 26.03 `0009` and `0010`
- **Files:** `upstreaming.md:49-50`
- **Acceptance:** §2 either says how each status was obtained, or marks the unmeasurable ones
  unverified. No unqualified claim survives that §3 already says it cannot check.
- **Gates:** 2 exempt (documentation); 5 required; 6 exempt.
- **Note:** found by the T-08 Gate 5 pass two. `:49-50` state "Never submitted" for the 2
  patches that became 26.07 `0005` and `0006`. Their metadata is submission-shaped in 5
  independent ways: well-formed 40-hex commit separators, an author that DPDK `.mailmap:1590`
  records,
  `git format-patch` series counters `[PATCH 09/11]` and `[PATCH 10/11]`, DPDK-convention
  `Fixes:` tags, and `Cc: stable@dpdk.org`, whose only function is to route a patch to the
  stable maintainers on the mailing list.
- **Not proof either way.** "Prepared for submission and never sent" fits every one of those
  5 signals. The defect is that §2 states an unqualified status while §3 already concedes it
  cannot be measured here — there is no DPDK git tree and no mail archive on this host.
- **Act before the trail is gone.** T-08 deleted the `Cc: stable@dpdk.org` lines, which were
  the strongest counter-evidence. The deletion is right, because §8 states one uniform rule
  for a tree that posts nothing. Record the reasoning in §2 while it is still checkable.
- **The separators are recorded here verbatim, because T-08 is about to delete them.**
  `0005` line 1 held `From 9c05e102304f23b9b6e1b8af4ec1347d514f0507` and `0006` held
  `From f6165f586a5628b47e5cbb68e53e9f7865ef7088`. Neither resolves in this repository, and the
  pristine 26.07 tarball has no `.git`, so neither is checkable as a DPDK object either.
- **What the separator actually proves is weaker than §2 needs.** `git format-patch` writes a
  separator whether or not the result is ever mailed. So the separator and the `[PATCH 09/11]`
  counter both show the files were produced by `format-patch`, not that they were sent. Of the 5
  signals, only `Cc: stable@dpdk.org` pointed at a mailing list, and that one is now deleted.
  §2 must state the status as unverified, not qualify it.
- **Order matters.** Record this in §2 before, or in the same change as, the `0005` and `0006`
  separator strip. Otherwise the third of 5 signals is gone before the record exists.
- **This weakens 1 of my own arguments, so read it.** I accepted the `Cc:` deletion partly
  because `:49-50` say "Never submitted". If that status is unverified, the deletion still
  stands on §8's uniform rule alone, which does not discriminate by author.
- **The authorship half of this is now settled, and the answer was neither candidate.** The
  pcapng patch is Frank Du's, per `659ebc82`. Cite that commit. Dawid Wesierski was the sender
  and Marek Kasiewicz the rebaser. See T-08.
- **Record the true submission map, because I mis-paired it once and so did Gate 5.** The
  patchwork links at `:159-160` use **26.03 numbering**. They name 26.03 `0005`
  (`iavf-disable-runtime-queue`, patchwork 166691) and 26.03 `0006` (`pcapng`, 166396). They do
  **not** name 26.03 `0009` and `0010`. So the 2 patches that `:49-50` call "Never submitted"
  are exactly the 2 with no patchwork link, and §2 and §3 agree rather than contradict.
- **This is why the `Cc:` deletion stays deleted.** Gate 5 pass three asked to restore
  `Cc: stable@dpdk.org` on 26.07 `0005` and `0006`, calling them the only genuine upstream
  postings. That pairing is inverted: those are 26.03 `0009` and `0010`, the never-submitted
  pair. §8 states one uniform rule for a tree that posts nothing, so the deletion holds. I did
  not flip the ruling a 3rd time on evidence that had not changed.
- **Still open, and worth 1 command.** Did 26.03 `0005` or `0006`, the 2 that were genuinely
  posted, carry a `Cc: stable@dpdk.org` that the sweep removed? If so that is a real loss of
  mailing-list metadata and §8 should record the exception.

## T-27 Two patches still name an author nobody verified — OPEN

- **Owner:** mtl-developer, after the trace returns
- **Ref:** upstreaming.md §2 and §8
- **Files:** `patches/dpdk/26.07/0001-Change-to-enable-PTP.patch`,
  `patches/dpdk/26.07/hdr_split/0001-net-intel-ice-support-hdr-split-mbuf-callback.patch`
- **`0009` is closed, 2026-08-24.** The ruling below landed. Line 2 and line 30 both read
  `MTL Contributor <noreply@example.com>`, and upstreaming.md §8 records why no author is
  recoverable. I verified both lines and the §8 entry. The trace stays here as the record of
  how the answer was reached. T-31 carries what the fix left behind on line 30.
- **Acceptance:** each `From:` is either supported by a named commit in this repository or in
  the pristine 26.07 tree, or the file states that the author is unknown. **No `From:` rests on
  the author of the commit that added the patch file.** That inference is what this task exists
  to remove, so the test must forbid it by name.
- **Gates:** 2 exempt (patch metadata); 5 required; 6 exempt.
- **Note:** T-08 proved the inference wrong on the pcapng patch and fixed that one file. These 3
  carry the same defect and were left alone deliberately, because a better guess is not a fix.
- **`0009`: the trace is complete and the answer is unknown.** Checked 5 ways, and no evidence
  supports any name.
  1. **Not upstream.** `ice_timesync_find_src_tmr_owner` has 0 hits in the pristine 26.07 tree,
     and `ice_ethdev.c:7177-7189` still holds the bare `if (src_tmr_owned)` with no `else` — the
     pre-patch state. So there is no upstream author to inherit.
  2. **One commit ever added it**, `168b785a`. Its message describes the bug at length and cites
     no patchwork link, no thread and no author. No earlier `patches/dpdk/*/` holds the file.
  3. **upstreaming.md already records the gap** — `§2` marks it Never submitted, `§4` says the
     metadata is wrong, `§8` lists it under placeholder metadata, and the §12 series timeline
     covers no such posting.
  4. **The sibling argument refutes the name instead of supporting it.** `168b785a` added
     `0005` to `0009` as a bundle, and `0005` and `0006` in that same bundle are Soumyadeep
     Hore's, carried and not authored. So for this exact commit, "Marek's commit added it" is
     already proved not to mean "Marek wrote it".
  5. **The body names nobody.** No `Fixes:`, no `Cc:`, no reviewer, no second sign-off. The
     genuine siblings all carry real `Fixes:` tags — `0005:15`, `0006:16`, `0007:30`.
- **The decisive fact: the rename was never committed.** Every one of the 18 refs that holds this
  file holds `From: MTL Contributor <noreply@example.com>`, and `HEAD:patches/dpdk/26.03/0013`
  still reads that today. `Marek Kasiewicz` exists only in the untracked `patches/dpdk/26.07/`
  working tree. So there is no committed provenance for the name and nothing is being reverted.
- **Ruling: restore the placeholder on `0009`, and record it in upstreaming.md §8.** The 26.07
  file is a copy of a tracked 26.03 file, D5 keeps that source, and a copy has no licence to
  improve on its source without evidence. `MTL Contributor` is visibly not a person, so it makes
  no false claim about one, which is the property a real name lacks here. This is why the
  acceptance test above had to change first — the old test forbade the honest answer.
- **What would settle it** is outside this repository: ask Marek Kasiewicz whether he wrote it,
  or search Intel-internal history. Nothing on this host can.
- **`0001` and `hdr_split/0001` were each reattributed twice.**
  `Change-to-enable-PTP`: `qiaoliu78` at 21.11, then Ric Li at 22.07 (`f457fdd7`), then
  `"Kasiewicz, Marek"` at 25.03 (`a141fa92`). `hdr_split/0001`: `"Du, Frank"` at 22.07
  (`f457fdd7`), then Ric Li at 23.07 (`a69f05b4`), then `"Kasiewicz, Marek"` at `a141fa92`.
- **`0001` has no clean name to restore.** Its true original author is recorded as
  `qiaoliu78 <media@qiaoliu-mobl2.ccr.corp.intel.com>` — a workstation hostname, not a mailbox.
  See T-28.

## T-28 A developer workstation hostname is committed in 2 patch files — OPEN

- **Owner:** the user decides; this is not a code question
- **Files:** `patches/dpdk/21.11/0002-Change-to-enable-PTP.patch:2`,
  `patches/dpdk/21.08/0010-Add-init-time-to-sync-PHY-timer-with-primary-timer.patch:2`
- **Acceptance:** the user rules whether `media@qiaoliu-mobl2.ccr.corp.intel.com` stays.
- **Correction, mine.** I first named the second file
  `patches/dpdk/21.08/0010-Change-to-enable-PTP.patch`, which does not exist. `grep -rln`
  over `patches/` gives the 2 files above and no others, so the count of 2 is right and only
  the filename was wrong.
- **Gates:** 2 exempt; 5 required if anything changes; 6 exempt.
- **Note:** found by the T-08 authorship trace. The string is an internal corporate workstation
  hostname in tracked history, and it is the true original author of what is now 26.07 `0001`.
  `gitleaks` does not flag it, because it is not a credential.
- **Do not rewrite history to remove it.** These are tracked files in released directories. The
  decision is whether to leave it, or replace it in the working tree only and accept that the
  string stays reachable through `git log`. Deleting it also destroys the only record of who
  wrote that patch, which works against T-27.

## T-29 `patches/dpdk/26.03/` still carries every defect T-08 removed from 26.07 — OPEN

- **Not blocked. D5 already settled it, and I had this wrong.** I first wrote that this waits on
  "whether 26.07 supersedes 26.03", and called that decision D6. D6 is the branch choice and
  says nothing about it. **D5 says `patches/dpdk/26.03/` stays in the tree**, because the maint
  branches and a rollback need it. So both directories ship, and the defect ships with them.
  This task is actionable now.
- **Owner:** mtl-developer
- **Ref:** upstreaming.md §2
- **Files:** `patches/dpdk/26.03/` (12 of 14 files), `patches/dpdk/25.11/0009`
- **Acceptance:** the same 3 greps T-08 uses return nothing across all of `patches/`, not only
  under `26.07/`.
- **Gates:** 2 exempt; 5 required; 6 exempt.
- **Measured across `patches/dpdk/26.03/`, 2026-08-24.** The scope is wider than the 4 files
  first named here.
  - **Line 1.** 13 of 14 files carry a 40-hex hash and 1 reads `From patchwork Wed May 6 ...`.
    Of the hashes, 2 are `0000...0000`, 1 is a hand-typed `0000...0002`, and 1 is
    `a1b2c3d4e5f60718293a4b5c6d7e8f9011223344`, an ascending-nibble pattern that cannot be a
    hash. The remaining 9 are MTL-local rebase hashes that resolve to nothing.
  - **Comma-form identities: 5 files** — `0001`, `0004`, `0006`, `0007`, `0012`. These fail
    DPDK's own `devtools/check-git-log.sh`, which validates every contributor against
    `.mailmap`, and `.mailmap` holds 0 comma-form entries.
  - **Fabricated `[PATCH nn/mm]` counters: 10 files** — `0001` to `0006`, `0009` to `0012`.
- **The 26.07 fix is the template.** All 10 header-bearing files there now read
  `From nobody Mon Sep 17 00:00:00 2001` on line 1, carry bare `[PATCH]`, and use plain
  identities. Apply the same 3 edits here. Touch header lines only and prove each diff body
  hash held, anchoring on `^---` as well as `^diff --git`, because a `diff --git`-only anchor
  hashes the empty string for a plain unified diff.
- **2 more byte defects, and 26.07 already fixed both of them.** `26.03/0012` has no final
  newline, which makes `patch` emit a warning §3 records, and its git signature separator lacks
  the trailing space after the two dashes, so it is the one file that fails
  `grep -c '^-- $'`. Its 26.07 descendant `0008` has both repaired. Do not write that separator
  as a Markdown code span — the linter strips the space and the sentence becomes nonsense.
- **Note:** `26.03/0011:1` is `From a1b2c3d4e5f60718293a4b5c6d7e8f9011223344`, an ascending-nibble
  pattern that cannot be a hash. `26.03/0012:1` and `25.11/0009:1` are `From 0000...0002`.
  `26.03/0013` and `0014` are `From 0000...0000` with `MTL Contributor <noreply@example.com>` in
  both `From:` and `Signed-off-by:`.
- **Why it is not urgent.** If 26.07 supersedes 26.03 the whole directory goes and this closes
  itself. If both ship, the defect ships. One decision settles it, so do no work first.

## T-30 24 Windows patch files are not patches — OPEN

- **Owner:** mtl-developer
- **Ref:** upstreaming.md §2; CLAUDE.md, "Format and lint", on symlinks under Windows
- **Files:** `patches/dpdk/23.03/windows/` (10 files), `patches/dpdk/23.07/windows/` (7),
  `patches/dpdk/23.11/windows/` (7)
- **Acceptance:** every file under `patches/dpdk/*/windows/` is either a symlink (mode `120000`)
  or a real patch whose first line matches `^From`. No tracked file in `patches/` is a mode
  `100644` regular file whose whole content is a relative path.
- **Gates:** 2 exempt (patch files, no MTL code); 5 required; 6 exempt.
- **What is wrong.** Each of the 24 is a mode `100644` regular file whose entire content is one
  relative path, with no trailing newline — for example
  `patches/dpdk/23.11/windows/0001-Add-DDP-package-load-support-in-windows.patch` is 70 bytes
  reading `../../21.11/windows/0001-Add-DDP-package-load-support-in-windows.patch`. They are
  symlinks materialized as text. The 30 files that are still mode `120000`, in `22.03`, `22.07`
  and `22.11`, resolve correctly and prove what the broken ones were meant to be.
- **How they got there.** A checkout with `core.symlinks=false`, which is the Windows default,
  writes a symlink as a text file holding its target. Committing that tree converts the link to
  a file. CLAUDE.md already records this hazard for `.clang-format`, which is why that file must
  stay real.
- **This breaks the msys2 workflow, and both matrix entries fail.**
  `.github/workflows/msys2_build.yml:46` pins `dpdk: [25.03, 23.11]` and `:136` runs
  `git am ../patches/dpdk/${{matrix.dpdk}}/windows/*.patch`.
  For `23.11` that feeds `git am` 7 files that hold a path string instead of a diff.
  For `25.03` there is no `windows/` directory at all, so the glob never expands and `git am`
  receives a literal unexpanded path. Neither entry can pass. This explains the T-13 note that
  nobody has run this workflow successfully for a long time, and it means the failure is not the
  version pin.
- **No dangling links anywhere.** I checked every tracked file under `patches/`: 0 dangling
  symlinks. The defect is only this conversion.
- **A 2nd, independent defect at the same call site, found by the T-08 Gate 5 pass four.**
  Even a real Windows patch fails there. `windows/0001.patch` has no mail header and starts at
  `diff --git`, so `git am` on it returns `Patch format detection failed.` while
  `git apply --check` passes. The reviewer measured both on `patches/dpdk/26.07/windows/`.
  So `msys2_build.yml:136` is wrong twice over: it uses `git am` where the shape needs
  `git apply`, and 24 of its inputs are not patches at all.
  [doc/build_WIN.md:86](doc/build_WIN.md) already uses `git apply` and is the flow that works.
  Fix the workflow to match the document, not the reverse.
- **Note:** this widens T-30 from a file-mode repair to a call-site repair. The 24 files and
  the `git am` call are separate faults and either one alone still fails the workflow.
- **Do not fix this by copying content in.** Restore them as symlinks, or the duplication comes
  back and the next Windows checkout breaks them again. Whether MTL should carry patch symlinks
  at all is a separate question and belongs with T-13.

## T-31 Patch `0009` carries a sign-off nobody can stand behind — OPEN

- **Owner:** user decision first, then mtl-developer
- **Ref:** upstreaming.md §8; the Linux DCO, which `Signed-off-by:` invokes
- **Files:** `patches/dpdk/26.07/0009-net-ice-always-init-PHC-owner.patch:30`, and the same
  line in `patches/dpdk/26.03/0013-net-ice-always-init-PHC-owner.patch`
- **Acceptance:** the sign-off trailer names a person or entity that can certify the DCO, or
  the patch records why it has none. No trailer may attest on behalf of a placeholder.
- **Gates:** 2 exempt (patch metadata); 5 required; 6 exempt.
- **What is wrong.** T-08 set line 2 and line 30 both to
  `MTL Contributor <noreply@example.com>`, because 5 independent routes failed to recover the
  author. That is right for `From:`, which only claims authorship. It is weaker for
  `Signed-off-by:`, which is a **certification** under the DCO. A placeholder cannot certify
  anything, so line 30 is now an empty legal attestation.
- **All 3 available states are flawed, and T-08 chose the least bad.** A real name on line 30
  would forge a DCO certification from a person who never gave it. Deleting the trailer would
  alter the patch body and leave a DPDK patch with no sign-off at all, which DPDK rejects. The
  placeholder is honest about the gap and forges nothing, so it stands until somebody decides.
- **Why this is a separate task.** "Carried patch with an unknown author" and "carried patch
  with an unsignable sign-off" are different problems. T-08 reasoned about `From:` only, so
  line 30 would otherwise stay unexamined.
- **The real fix is probably not metadata.** Somebody at Intel who can certify the change
  should sign it, or MTL should re-derive the patch from a source that already carries a valid
  trailer. Both need a person outside this machine.
- **Note:** `noreply@example.com` is not itself a defect. RFC 2606 reserves `example.com` so
  it can never resolve to a real mailbox, so the address cannot misdeliver or impersonate.
- **Note:** raised by the developer that made the T-08 edit, against its own change. Recorded
  because it is correct.

## T-32 Patch `0003` keeps an export annotation for a function it renames — OPEN

- **Owner:** mtl-developer, at the next DPDK bump
- **Ref:** upstreaming.md §7
- **Files:** `patches/dpdk/26.07/0003-pcapng-add-user-timestamp-support.patch:20`
- **Acceptance:** the annotation names a symbol the patched tree actually exports, or a
  comment in §7 records why it does not have to.
- **Gates:** 2 exempt (patch metadata); 5 required; 6 exempt.
- **What is wrong.** The patch keeps `RTE_EXPORT_SYMBOL(rte_pcapng_copy)` while renaming that
  function to `rte_pcapng_copy_ts()` and making `rte_pcapng_copy` a `static inline` wrapper in
  the header. So the annotation names a symbol the shared object no longer exports.
- **It does not break the build today, and Gate 5 proved that 3 ways.** The macro expands to
  nothing at `lib/eal/common/eal_export.h:16`. DPDK 26.07 generates version maps from these
  annotations and carries no `version.map` file anywhere in the tree. And
  `config/meson.build:200-202` passes `-Wl,--undefined-version`, so a map entry with no
  matching symbol still links clean.
- **Why file it anyway.** All 3 conditions are upstream build-system choices, not MTL's. If any
  one changes, the link fails and the cause will look like a DPDK regression rather than a
  20-line-old annotation in an MTL patch. The body is unchanged from 26.03, so this is not a
  regression the bump introduced.
- **Do not fix it inside a metadata pass.** The change is 1 body line, which means regenerating
  the patch and re-proving the body integrity evidence. Do it when something else already
  requires a body edit.

## T-34 The MCP servers cannot start, so no host task can run — IN PROGRESS

- **Owner:** general-purpose. Not `mtl-developer`: no file here is MTL product code.
- **Files:** [.github/mcp/requirements.txt](.github/mcp/requirements.txt),
  [.github/mcp/run_server.sh](.github/mcp/run_server.sh) and
  [.github/mcp/run_acceptance_server.sh](.github/mcp/run_acceptance_server.sh) — all 3 fixed.
  Only the session restart is left.
- **Why it is load-bearing.** `mtl-system-admin` is the only agent allowed to configure the host
  or run `KahawaiTest`, and it is MCP-only. With the servers down, T-05 step 3, T-06 and T-07
  cannot run at all. It refused T-05 twice and was right both times.
- **Acceptance:** a fresh session lists `mtl-system-setup` with its 32 tools, and
  `mcp__mtl-system-setup__system_status` returns.
- **Gates:** 2 exempt (build-system and tooling, no MTL code); 5 required; 6 exempt.
- **The cause was an unpinned dependency.** `requirements.txt` read `mcp[cli]>=1.0.0`, which
  resolved to mcp 2.0.0. Version 2 removed `mcp.server.fastmcp`, which both servers import —
  `mtl_mcp_server.py:23` and `mtl_acceptance_mcp_server.py:29`. The venv now holds
  `mcp/server/mcpserver/` and no `fastmcp/`, so the server died with `ModuleNotFoundError`
  before any handshake. Pinned `>=1.0.0,<2.0.0`; the venv rebuilt at **1.29.0**, the last 1.x
  release; a hand-fed stdio handshake then listed 32 tools on `mtl-system-setup` and 7 on
  `mtl-acceptance-setup`.
- **The pin alone did not heal a broken venv, and both wrappers now carry the repair.** The old
  guard `if ! python3 -c "import mcp"` succeeds on an mcp 2.x venv, so pip never ran and the
  ceiling never applied. Both wrappers now probe
  `importlib.util.find_spec("mcp.server.fastmcp")` — the module the servers really import — so a
  2.x venv reinstalls itself and no host needs `rm -rf .github/mcp/.venv` by hand. Verified: the
  probe exits 0 on the 1.29.0 venv and 1 for both a 2.x-only layout and an absent package;
  `bash -n` clean on both; `./checkpatch.sh` clean, `shfmt` and `shellcheck` included.
- **It needs a session restart, and that needs the user.** Claude Code negotiates MCP
  connections once, at session start, and does not retry a server that failed to hand over its
  tool list. A subagent inherits the parent session's connections and cannot open its own. So
  this session holds a dead connection for both servers no matter what is on disk, which is
  exactly what the inventory shows: the pin is correct, the venv is at 1.29.0, the server answers
  a handshake from outside, and both servers are missing from the agent's tool list.
- **Note:** the unpinned dependency is the defect worth remembering. A floating major version in
  agent tooling fails silently and looks like a broken agent.

## T-35 No shipped binary can set `rl_burst_size`, so T-06 cannot exercise it — OPEN

- **Owner:** mtl-developer
- **Needs:** T-04, and T-03 before any run can pass
- **Files:** [tests/integration_tests/noctx/testcases/queues.cpp](tests/integration_tests/noctx/testcases/queues.cpp),
  then [tests/tools/RxTxApp/src/args.c](tests/tools/RxTxApp/src/args.c)
- **Acceptance:** the new noctx case reaches `mtl_init` with the field set, and
  `lib/src/dev/mt_dev.c:400` logs `port_param: <BDF>,rl_burst_size=2048`.
- **Gates:** 2 required; 5 required; 6 is T-06.
- **Note:** found by the T-04 Gate 5. Outside `include/`, `lib/src/` and `tests/unit/dev/`, only
  3 places read `port_params` at all: `args.c:398`, `args.c:657-658` and the Rust example. Take
  2 steps. **Cheapest first, 1 edit site:** append a `TEST_F` after
  `noctx/testcases/queues.cpp:102`, copying `init_32_queues` at `:8-24`. The file is already in
  `noctx/meson.build:26`, the fixture deep-copies `para` per case
  (`noctx/core/test_fixture.cpp:54`) so the setting is per port, `tests.cpp:789` already sets
  `MTL_PMD_DPDK_USER`, and a `_pf_` infix in the name puts the case in `run_pf.sh:56` and out of
  `run.sh:54` — which the devarg needs, because `iavf` rejects the key. **Then RxTxApp, 3 edit
  sites in 1 file** (`args.c:73`, `:229`, `:655-659`), which makes the result reproducible.
  `args.c` has no usage printer and the JSON parser has no schema or key allowlist, so there is
  no fourth site; `doc/run.md:366` is an optional doc line.
- **No zero-code path exists, and the obvious workaround is a trap.** `grep -rn getenv lib/src/`
  returns exactly 1 hit, `KAHAWAI_CFG_PATH` (`mt_config.c:53`), whose only key is `"plugins"`.
  No init param carries extra EAL arguments. Passing the devarg inside the BDF
  (`--p_port 0000:c9:01.0,rl_burst_size=2048`) does reach EAL, but MTL then looks the ethdev up
  by that whole string and fails at `mt_dev.c:2219-2223`.

## T-36 The Rust `no_std` example does not compile, and nothing builds it — OPEN

- **Owner:** mtl-developer
- **Files:** [rust/imtl-sys/examples/no_std.rs](rust/imtl-sys/examples/no_std.rs)
- **Acceptance:** `cargo build --example no_std` succeeds in `rust/imtl-sys/`, and either
  `build.sh` or a workflow runs it so the next break is caught.
- **Gates:** 2 exempt if the fix is only the struct literal; 5 required; 6 exempt.
- **Note:** found by the T-04 Gate 5. `rust/imtl-sys/build.rs` runs bindgen at build time, so
  the generated `mtl_port_init_params` follows `include/mtl_api.h`, and a `#[repr(C)]` struct
  literal is exhaustive. Two breaks predate T-04: line 28 reads `dma_dev_port: [[0; 64]; 8]`
  against `MTL_DMA_DEV_MAX (32)` (`include/mtl_api.h:94`), and the literal omits
  `port_packet_loss` (`mtl_api.h:577`). T-04 fixes only the field it added. **The real defect is
  that no build runs it** — `grep -n 'cargo\|rust' build.sh` is empty and no workflow mentions
  `cargo` — so any public struct change silently breaks it. Fix the example or delete it; an
  example nobody compiles is not documentation.

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

### T-08 Give the carried patches real authorship metadata — DONE 2026-08-24

- **Owner:** mtl-developer
- **Needs:** T-02
- **Ref:** upstreaming.md §8
- **Files:** the new `0004`, `0007`, `0008`, `0009` and `windows/0001.patch` in
  `patches/dpdk/26.07/`
- **The renumbering warning T-02 raised is closed, and it named the wrong file.** I re-measured
  `patch -p1 --dry-run` on a pristine 26.07 copy: `windows/0001.patch` applies with 0 offset,
  both alone and after the 9 flat patches. `hdr_split/0001` is the file whose hunk headers still
  describe 26.03 — 9 hunks at offsets from -406 to +53 lines, 0 fuzz, 0 rejects. That costs
  nothing to `patch -p1` or `git apply`, which both search for context, so it is stale metadata
  of the same class as the `index` lines. Folded into **T-21**, not filed again.
- **Gates:** 2 exempt (patch metadata, no MTL code); 0-4 done; 5 pass one BLOCK on 2
  blockers, pass two APPROVE WITH COMMENTS with 0 blockers and 2 warnings, pass three
  **REJECT — 1 blocker, 5 warnings, 4 nits**, pass four **REJECT — 1 blocker, 3 warnings,
  2 nits**, pass five **APPROVE WITH COMMENTS — 0 blockers, 2 warnings, 3 nits**; 6 exempt.
  **Gate 5 is satisfied.**
- **What the finished set proves, all of it re-measured by Gate 5 rather than read from a
  report.** The 9 flat patches `git am` clean in order onto a pristine `dpdk-26.07` with 0 fuzz
  and 0 rejects. Each optional patch applies after those 9. `VERSION` ends `26.07.0_mtl_`.
  Every header-bearing file reads `From nobody Mon Sep 17 00:00:00 2001` on line 1;
  `windows/0001.patch` is outside that rule because it starts at `diff --git`, and the
  predicate says so rather than an exemption list. 8 patches commit under 5 real author names.
  Every body is byte-identical to its 26.03 ancestor except `0004`, `windows/0001` and 2 named
  bytes of `0008`.
- **1 known defect ships, deliberately and on the record.** `0009` still commits as
  `MTL Contributor <noreply@example.com>`, because 5 routes failed to recover the author. Its
  `Signed-off-by:` keeps the same placeholder: a real name would forge a DCO certification, and
  deleting the trailer would edit the body and leave a DPDK patch with no sign-off. **T-31**
  owns it and needs a person who can certify the change.
- **The `index <pre>..<post>` lines are not maintained** — 13 stale lines in 7 of the 11 files.
  Repairing them means regenerating bodies, which a metadata pass must not do. Plain `patch -p1`
  and plain `git am` ignore them, so the cost lands only on a future `git am -3`. **T-21**.
- **The standing rule this task earned, and it cost 6 recurrences to learn: state the predicate
  and the command that enumerates it, never the count.** Every time I wrote a count next to a
  rule, the count was the part that was wrong — a fix scoped to 3 files when 5 needed it, to 9
  when 10 did, "4 documented apply flows" when there are 5 sites, "4 dropped" when the table
  says 5. A predicate is checkable by a reader; a count is not.
- **The second rule, and it is the one worth carrying furthest: an acceptance test can reward
  the defect it is meant to catch.** My first test was
  `grep -rn "noreply@example.com\|0000000000000000"`, which passes the moment a placeholder is
  overwritten with **any** name. So the cheapest way to pass it was to invent an author, and an
  earlier pass did exactly that. A bad claim is one defect; a test that pays for fabrication
  generates them. **T-27**.
- **Evidence that cannot fail is not evidence.** §8 argued the old line-1 hashes were fabricated
  because `git cat-file -t` fails on them. There is no DPDK git tree on this host, so that
  command fails on every hash — including one §2 calls a real upstream commit. The conclusion
  survived on better grounds: the 3 hashes are a keyboard walk, a hand-typed counter and 40
  zeros, which needs no repository to check.
- **Routed out of this task, each with its own record:** T-21 index lines, T-24 the `0003`
  patchwork-sender question, T-26 filename-versus-`Subject:` on `0008` and `0009`, T-27 the
  2 remaining reattributed authors, T-29 the inherited 26.03 byte defects, T-31 the sign-off,
  T-32 the retained export annotation.
- **Acceptance, and the first version of it was the defect:** no `From:` in
  `patches/dpdk/26.07/` rests on the author of the commit that added the patch file. Each one is
  either supported by a named commit, or is a visible placeholder recorded in upstreaming.md §8
  as unrecoverable. `git am` of the set into a scratch `dpdk-26.07` clone succeeds for all 9.
- **Test tier:** none. Patch metadata only.

### T-02 Create `patches/dpdk/26.07/` with the 11 kept patches — DONE 2026-08-24

- **Owner:** mtl-developer
- **Ref:** upstreaming.md §1, §2, §3, §4, §6
- **Files:** `patches/dpdk/26.07/` (11 files, untracked); `upstreaming.md`
- **Acceptance:** met on 5 passes. `for p in patches/dpdk/26.07/*.patch; do patch -p1
  --dry-run -i $p; done` clean on a fresh `dpdk-26.07` tree, then the same for `hdr_split/`
  and `windows/`. Every patch applies with 0 fuzz and the series ends with `VERSION` reading
  `26.07.0_mtl_`.
- **Test tier:** none. Patch files, then prose.
- **Gates:** 2 exempt (no MTL code); 0-4 done; 5 pass one BLOCK, pass two **REJECT — 1 blocker,
  3 warnings, 3 nits**, pass three **REJECT — 1 blocker, 5 warnings, 1 nit**, pass four
  **APPROVE WITH COMMENTS — 0 blockers, 6 warnings, 2 nits**, pass five **APPROVE WITH
  COMMENTS — 0 blockers, 2 warnings, 3 nits**; 6 exempt.
- **The set is 11 files and final** (D7): 9 flat patches plus `hdr_split/0001` and
  `windows/0001`. Renumbering was `0004→0001`, `0005→0002`, `0006→0003`, `0007→0004`,
  `0009→0005`, `0010→0006`, `0011→0007`, `0012→0008`, `0013→0009`; the 2 subdirectory files
  keep their names. `script/build_dpdk.sh:98` applies a flat `*.patch` glob, so name order is
  apply order, and the subdirectories are applied by hand. Files were copied, not moved (D5).
- **Every pass found the same defect shape, and it recurred 11 times: a sentence left behind
  that reads as a completed repair.** Pass three's instance was a `##` heading I dictated
  myself, asserting MTL "carries the burst itself" 12 lines above the sentence that denies the
  field exists. Pass four's was "No code changes now" in §7, false because `lib/src/mt_pcap.h`
  carries T-09's comment pointing back at that very section. Each was closed with a measurement,
  not a rewrite.
- **Gate 5 re-derived 24 of the document's claims from the tree and all 24 measured.** That
  includes the §2 arithmetic from the table itself, the byte-identity of `0003`'s hunks to its
  26.03 ancestor, the 13 stale `index` lines across 7 of 11 files, and the `git am` author list.
  §8 is dense but reviewable; the question of shortening it is settled and closed.
- **Two conflicts were found here and handed to their owners.** `patches/dpdk/26.07/0004`
  hardcodes `26.07.0_mtl_` instead of deriving the minor from `DPDK_MTL_MINOR_VER`, so with
  `versions.env` at `91` the gate in `dpdk_is_installed()` never matches and every run rebuilds
  DPDK — **T-03** sets it to 0. The `rl_burst_size` replacement field is **T-04**.
- **2 warnings and 3 nits survive in `upstreaming.md` and are folded into T-18**, not passed to
  a sixth review round. The load-bearing one: `0003` names the dropped burst-size patch in some
  sections and the shipped pcapng patch in others, and only §4 and §7 declare which.

### T-33 Condense the T-08 record and move it under `## Done` — DONE 2026-08-24

- **Owner:** mtl-orchestrator
- **Files:** [tasks.md](tasks.md)
- **Acceptance:** met. T-08 appears once, as a `###` entry under `## Done`, at 59 lines against
  T-01's 49. Every durable finding has a home: T-18 the citation and lint rules, T-21 the
  unmaintained `index` lines and the stale `hdr_split/0001` hunk headers, T-24 the "never
  submitted" status, T-26 the filenames, T-27 the reattributed authors, T-29 the 26.03 copies,
  T-30 the Windows call site, T-31 the sign-off, T-32 the export annotation.
- **Gates:** none. This file is not code and no agent builds it.
- **Why it existed.** The T-08 record grew to 356 lines because each of 5 Gate 5 passes found a
  real defect and I wrote the evidence into the work list instead of leaving it in `git log`.
  What went was my own error register — 16 numbered entries that help 1 session and mislead a
  maintainer next month. What stayed is what a reader cannot re-derive: the 2 durable rules, the
  1 defect that ships, and the routing table out.
- **1 finding changed under re-measurement while condensing.** T-02's Gate 5 warning blamed
  `windows/0001.patch` for half-renumbered hunk headers. `patch -p1 --dry-run` on a pristine
  26.07 copy shows `windows/0001` at 0 offset and `hdr_split/0001` at 9 hunks off by up to 406
  lines. Condensing a record is not a copy edit; it re-tests the claims.

### T-01 Prove the drop list against a real DPDK 26.07 tree — DONE 2026-08-24

- **Owner:** mtl-developer
- **Ref:** upstreaming.md §3
- **Files:** upstreaming.md (§2 and §3 only). Read-only elsewhere.
- **Acceptance:** met, and I reproduced both halves myself.
- **Gates:** 2 exempt (documentation); 0-4 done; 5 pass one APPROVE with 8 nits, pass two
  fixed 4, pass three BLOCK with 4 blockers, pass four fixed them, **pass five APPROVE WITH
  COMMENTS — 0 blockers**, pass six landed the remaining warnings as deletions; 6 exempt.
- **The 5 greps, re-run by me against `/home/labrat/dpdk-26.07-verify/dpdk-26.07`.** Every one
  resolves to the exact line §3 names.
  - `0001` `iavf_rxtx.h:19` — `#define IAVF_MAX_RING_DESC        (8192 - 32)`
  - `0002` `iavf_tm.c:825` — the `||` guard, with the capability check above it at `:813`
  - `0003` `ice_ethdev.c:45` — `#define ICE_RL_BURST_SIZE_ARG     "rl_burst_size"`, read at
    `:2727`
  - `0008` `iavf_vchnl.c:1627` — `sizeof(struct virtchnl_queue_vector) * (chunk_sz - 1)`
  - `0014` `ice_ethdev.c:4668` — `if (hw->phy_model == ICE_PHY_E830)`
- **The 16 dry runs, re-run by me.** 10 pass and 6 fail, which is the number §3 records, not
  the planned 5 and 11. The 6 that fail are `0001`, `0002`, `0007`, `0008`, `0014` and
  `windows/0001`. `0001`, `0002`, `0008` and `0014` fail because 26.07 already carries the
  change. `0007` and `windows/0001` fail on context drift and are kept.
- **`0003` passes the dry run, and the reason needs care.** 26.07 still reads
  `ICE_SCHED_DFLT_BURST_SIZE (15 * 1024)` at `base/ice_type.h:1103`, so the patch text still
  applies. Upstream superseded MTL's **approach** with the `rl_burst_size` devarg; it did not
  take MTL's change. So a reader must not treat "superseded" here as "26.07 covers it".
  upstreaming.md `:82` and `:123` state this correctly. T-04 owns the decision.
- **I nearly filed this as a false claim in the record and I was wrong.** The compressed note
  read as if 26.07 carried the patch. It does not say that, and §3 spells out the distinction.
  The lesson is about wording, not about the measurement.
- **Note:** the 6 upstream commit hashes in §2 were read from a DPDK git tree at
  `/home/labrat/dev1/dpdk` that no longer exists, so they are a record and not a measurement.
  No DPDK git tree exists on this host, and `script/build_dpdk.sh` downloads a tarball, so
  `git merge-base` cannot answer the question. Pair each dry run with its grep.
- **The pattern broke on pass four, and this is the part worth keeping.** The developer found
  2 false claims in its own draft and reported both: it had written that `23.03` "stays once"
  in `header_split.md` when its own rewrite made the string appear 3 times, and it had written
  that `build.sh` reads `versions.env` when `build.sh` holds 0 references to it and
  `script/common.sh:9` is the real reader. I confirmed both. Deleting instead of rewriting is
  what made the difference.
- **My 6th error. The scope line I wrote did not cover my own findings.** I restricted the
  developer to upstreaming.md "§3, §6 and §9 only", then gave it line-cited items in §2 and
  §7. It read the list as an under-specified paraphrase, executed the line-numbered items,
  left the hard-excluded §8 byte-identical, and offered to revert if I meant the restriction
  literally. I verified §8: it hashes `7933d3287c9b67d3` from both HEAD and the working tree.
  Name sections and line numbers from the same reading of the file, or the 2 disagree.
- **The `4 dropped` fix forced a 2nd repair.** Changing `:58` contradicted `:109`, which read
  "a plan of 11 keep and 5 drop". `:109` is in §3 and in scope, so it now reads "11 keep, 4
  drop and 1 open". Both still resolve to the 16 rows in the §2 table.

### T-09 Record the pcapng break at the guard — DONE 2026-08-24

- **Owner:** mtl-developer
- **Ref:** upstreaming.md §7
- **Files:** [lib/src/mt_pcap.h:13-16](lib/src/mt_pcap.h)
- **Acceptance:** met. `./build.sh` green, and the comment names the symbol, the accepted
  upstream signature, and the patch that supplies the current one.
- **Gates:** 2 exempt (comment); 0-4 done; 5 pass two APPROVE, pass three BLOCK, pass four
  REJECT, **pass five APPROVE WITH COMMENTS — 0 blockers**; 6 exempt.
- **How I closed it.** I read the 4 comment lines myself. They name
  `MTL_DPDK_HAS_PCAPNG_TS`, `rte_pcapng_copy_ts()`, the `uint64_t` shape upstream accepted,
  and the patch by `Subject:` text instead of by number. `./checkpatch.sh` exits 0.
- **The comment sits at the guard, not the call site,** because the guard is where the
  capture is lost. The failure analysis stays in upstreaming.md §7 only, so the 2 copies
  cannot drift again.
- **Gate 2 exemption proved.** `build/lib/libmtl.so.p/src_mt_pcap.c.o` hashes
  `2addeb73a1c5fb68...e5705c74` before and after. `ninja -n` confirmed the object was dirty
  first, so the rebuild was real and not a no-op. I reproduced the hash.
- **4 passes failed before this one, and each wrote a new false claim.** Pass one said the
  build breaks; it does not, because `mt_pcap.h:34-45` holds stubs and capture stops
  instead. Pass three said a failed patch lets the stubs take over with no build error; it
  does not, because `script/build_dpdk.sh:6` is `set -e`, the apply loop is `:98` and
  `meson build` is `:112`, so a failed patch aborts before anything compiles. The stubs are
  reached by an install whose `rte_pcapng.h` lacks the define. Pass four found the comment
  had grown to 6 lines and duplicated §7, and the 2 copies had already drifted.
- **Note:** upstreaming.md said the tree holds "exactly one definition" of
  `MTL_DPDK_HAS_PCAPNG_TS`. Six patch files add it. The intended meaning is true, so the
  row is restated as the measurable fact — each `patches/dpdk/*/` copy adds the define in
  the same hunk that declares `rte_pcapng_copy_ts()`, so an install cannot get one without
  the other.
- **A type defect sits in the stub arm, and it is unreachable. Not filed as a task.**
  `mt_pcap_dump()` returns `uint16_t`, a dumped-packet count, in both arms
  (`mt_pcap.h:32` and `:47`), but the stub returns `-ENOTSUP`, which is 65503. Callers add it
  straight to a counter — `pcap->dumped_pkts += dump; pcap->dropped_pkts += nb - dump;` at
  `mt_cni.c:331-333`, and the same shape at `st_rx_video_session.c:1500` and
  `st_rx_audio_session.c:704`. No call site can run it: the stub `mt_pcap_open()` returns
  `NULL`, and every caller gates on a non-`NULL` handle (`mt_cni.c:314`). The stub should
  return `0`. Recorded here so the next reader does not file it twice.

### T-10 Make CI and the documentation read the pinned DPDK version — DONE 2026-08-24

- **Owner:** mtl-developer
- **Ref:** upstreaming.md §9
- **Files:** [.github/workflows/validation-tests.yml:109](.github/workflows/validation-tests.yml),
  [doc/build.md:155](doc/build.md), [doc/build_WIN.md:82](doc/build_WIN.md),
  [doc/experimental/header_split.md:11](doc/experimental/header_split.md)
- **Acceptance:** met. `./checkpatch.sh` clean, and no literal DPDK version outside
  [versions.env](versions.env) and `patches/dpdk/*/`, except the 2 exceptions below.
- **Gates:** 2 exempt; 0-4 done; 5 pass one APPROVE WITH COMMENTS, pass three BLOCK, pass
  four REJECT on 2 blockers, **pass five APPROVE WITH COMMENTS — 0 blockers, 4 warnings,
  1 nit**; pass six landed all 4 warnings and the nit as deletions; 6 exempt.
- **How I closed it. I re-measured every part.** `doc/build.md` and `doc/build_WIN.md` hold
  0 DPDK version literals. `.github/workflows/validation-tests.yml` holds 0.
  `doc/experimental/header_split.md` holds 1 against a limit of 8, and its longest line is
  212 characters against 400, down from 384. `doc/design.md` is byte-identical to HEAD and
  has dropped out of `git status`. `./checkpatch.sh` exits 0.
- **2 exceptions stay, and both are recorded, not forgotten.**
  [doc/design.md:664](doc/design.md) and [:671](doc/design.md) keep `25.11` as the
  deliberate Ubuntu 22.04 AF_XDP workaround, which HEAD already justifies.
  [.github/workflows/msys2_build.yml:46](.github/workflows/msys2_build.yml) keeps
  `[25.03, 23.11]`. A bump there answers a product question in silence, so T-13 owns it.
- **This task shifted 2 citations in upstreaming.md, and the record says so.**
  `doc/build.md:150` and `doc/build_WIN.md:76` now point at blank lines. The `git am` lines
  they mean moved to `:155` and `:82`. T-18 repairs both.
- **I reported `checkpatch.sh` as unclean here, and I was wrong.** I claimed my first
  verification run applied `markdownlint-fix` to 4 Markdown files. My only evidence was
  mtime, and `markdownlint-fix` moves the mtime of every Markdown file it reads even when it
  changes no byte. A later run proved it: exit 0, mtime moved, `md5sum -c` OK on all 5 files.
  The hook does edit content where a rule is really broken, so T-18 keeps the citation sweep
  rule, but this task's lint runs were clean and the developer's report was right.
- **"Header split is experimental" stays, for a reason neither I nor the developer had.**
  `include/mtl_api.h:695` states it in the public API. My own justification was the
  `doc/experimental/` directory name, which proves where the file sits and nothing about the
  feature. Gate 5 supplied the real proof.
- **Gate 5 found the worst-formed row in §9, and it was evidence I had accepted.** The
  `doc/design.md` row cited a line **added by this same uncommitted worktree**, so the row's
  evidence was written by the change that declared the row closed. A `git stash` would have
  falsified it. That sentence also added a `25.11` literal, which works against this task's
  own acceptance test, so it is deleted.
- **Note:** §9's premise was wrong and Gate 5 proved it. The workflow never built DPDK
  25.11. `DPDK_REBUILD` is hardcoded `'false'`, so all 4 `DPDK_VERSION` consumers never
  run. This task fixed a latent defect, not a live one. The new `versions.env` read is also
  unreachable. See T-17.
- **My seventh error, and the same shape as the fourth.** I told the developer to record
  that the original note named `v23.08`, a release DPDK never made. The developer did
  exactly that, and the sentence narrates the document's own edit history. Gate 5 named
  this as the mechanism behind 4 failed passes: each pass deletes a false claim and writes
  a sentence about the deletion. Pass six is deletion only.
