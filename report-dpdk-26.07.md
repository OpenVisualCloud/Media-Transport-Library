# DPDK 26.07 move — progress report, 2026-08-25

A short record of the round that produced the `dpdk-26.07` branch. The work list is
[tasks.md](tasks.md). The source record is [upstreaming.md](upstreaming.md). This file
adds no facts; it tells a new reader what was done, how it was done, and where the work
stopped.

Prose is Simplified Technical English.

## 1. Scope and decisions

The round replaced an upstreaming effort with a version bump. Eight decisions were locked
with the user on 2026-08-24 and are recorded in the `## Decisions` table of `tasks.md`. The
three that shape everything else:

- **D1** — MTL sends nothing to `dev@dpdk.org`. Eight upstreaming tasks were cancelled.
- **D2** and **D3** — target DPDK 26.07. A patch stays only when the v26.07 source proves
  the change is absent. Nothing is kept "just in case".
- **D7** — the burst-size patch `0003` is dropped, and an `rl_burst_size` field in
  `struct mtl_port_init_params` replaces it.

The final patch count is **5 dropped, 11 kept, 0 open**.

## 2. Method

Every task ran the six-gate test-first loop in
[.github/copilot-instructions.md](.github/copilot-instructions.md): knowledge, failing
test, implement, green build, adversarial review, hardware. `mtl-orchestrator` held the
work list and fired Gates 5 and 6. `mtl-developer` owned Gates 0 to 4. `mtl-reviewer` gave
the Gate 5 verdict and had no exemption.

Two rules govern the round and both were learned the expensive way:

1. **State the predicate and the command that enumerates it, never the count.** Each time a
   count was written next to a rule, the count was the part that was wrong.
2. **An acceptance test can reward the defect it is meant to catch.** A test that only
   checked for the absence of a placeholder paid for an invented author name, and an earlier
   pass paid it.

Gate 5 was not a formality. T-01 took 6 passes, T-02 and T-08 took 5, T-09 and T-10 took 5,
T-04 took 4. Every pass but the last found a real defect, and most defects were false claims
in the record rather than faults in the code.

## 3. What is finished

Six tasks are **DONE** and sit under `## Done` in `tasks.md`.

| Task | Result |
|---|---|
| T-01 | The drop list is proved against a real v26.07 tree: 5 greps and 16 dry runs, 10 pass and 6 fail. |
| T-02 | `patches/dpdk/26.07/` holds the 11 final files. The 9 flat patches `git am` clean in order onto a pristine tree, with 0 fuzz and 0 rejects. |
| T-08 | The carried patches have real authorship metadata. 8 of 9 commit under 5 real author names. |
| T-09 | `lib/src/mt_pcap.h` records the pcapng break at the guard, which is where the capture is lost. |
| T-10 | CI and the documentation read the pinned DPDK version. No version literal stays outside `versions.env` and `patches/dpdk/*/`, except 2 recorded exceptions. |
| T-33 | The T-08 record was condensed from 356 lines to 59 and its durable findings were routed to owners. |

The code half of **T-04** also closed on Gate 5 with 0 blockers. The `rl_burst_size` field,
its EAL devarg path and 5 unit tests are in the tree, and
`UnitTest --gtest_filter='MtDevDevargs*'` reports `[  PASSED  ] 5 tests.`

**T-05** captured 2 of its 3 baseline runs on real hardware: `auto` pacing gives
`[ PASSED ] 42 tests.` and `tsc` pacing gives 41 passed with 1 known failure. The baseline
is outside the tree, in `/home/labrat/mtl/baseline-26.03/`, so no rebuild can overwrite it.

**T-34** fixed the cause of the dead MCP servers — an unpinned `mcp[cli]` dependency that
resolved to 2.x, which removed the `mcp.server.fastmcp` module both servers import. The pin
and both wrapper probes are on disk and verified.

## 4. What is in this commit

- `patches/dpdk/26.07/` — 11 new patch files. **The directory is inert.**
  [script/build_dpdk.sh](script/build_dpdk.sh) globs
  `patches/dpdk/"$DPDK_VER"/*.patch`, and `versions.env` still pins `DPDK_VER=26.03`, so
  nothing reaches these files until T-03 changes that 1 value. Landing them cannot affect
  this host or anybody's running test.
- `include/mtl_api.h`, `lib/src/dev/mt_dev.{c,h}`, `lib/src/mt_main.c` — the
  `rl_burst_size` field, the `dev_build_pci_devarg()` helper that appends the devarg, one
  named width constant that replaces 4 open-coded buffer sizes, and a warning when the field
  is set on a PMD that cannot carry it.
- `tests/unit/dev/mt_dev_devargs_test.cpp` and the harness and meson changes that expose it
  — 5 tests. Gate 2 was real: 4 of the 5 failed first.
- `rust/imtl-sys/examples/no_std.rs` — 1 line, because a `#[repr(C)]` struct literal in Rust
  is exhaustive.
- `lib/src/mt_pcap.h` — the T-09 comment.
- `doc/build.md`, `doc/build_WIN.md`, `doc/experimental/header_split.md`,
  `.github/workflows/validation-tests.yml` — the T-10 version-literal removal.
- `.github/mcp/requirements.txt`, `.github/mcp/run_server.sh`,
  `.github/mcp/run_acceptance_server.sh` — the T-34 pin and the 2 wrapper probes.
- `tasks.md` and `upstreaming.md` — the work list and the source record.

## 5. Where the work stopped, and why

**Everything that does not need the host is done.** What is left needs one of two things
this session cannot supply.

- **A session restart.** `mtl-system-admin` is the only agent allowed to configure the host
  or to run `KahawaiTest`, and it is MCP-only. Claude Code negotiates MCP connections once,
  at session start, so this session holds a dead connection for both servers whatever is on
  disk. T-34 holds the cause and the remedy.
- **The host chain, in this order:** T-03 bumps `versions.env` and installs 26.07, T-35
  gives a shipped binary a way to set `rl_burst_size`, T-06 measures both on real hardware,
  T-07 runs the acceptance smoke suite. T-06 is Gate 6 for T-03 and for T-04.

Two measurements fix that order and are worth reading before the chain runs:

1. **The installed 26.03 ice PMD does not know the `rl_burst_size` key.** `strings` on the
   shipped `librte_net_ice.so` finds `proto_xtr` and `rx_low_latency` and no third key. A
   run today returns an unknown-key probe failure, which proves nothing. So T-03 must come
   first, and T-35 must come before T-06.
2. **The loader serves a different checkout's DPDK.** `/etc/ld.so.conf.d/mtl_local.conf`
   puts a sibling checkout ahead of `/usr/local`, and both export soname
   `librte_eal.so.26`. So installing 26.07 into `/usr/local` does not by itself change what
   a test loads, and Gate 6 could pass while measuring the old DPDK. Every recorded run must
   carry `--log_level notice` and prove `dpdk version:` from inside the process.

## 6. What the verification found beyond the plan

`tasks.md` holds 36 tasks: 6 done, 1 blocked, 2 in progress, 27 open. **Every task numbered
T-11 and above was found by a verification pass, not planned.** Most are defects in the
carried patch set or in its own record, and only a re-measurement could find them. The ones
that matter outside this move:

- **T-19** — the unit suite aborts after 46 of 508 tests, because a test reaches
  `rte_eal_init()` and the EAL panics on a double-registered tailq. No workflow runs the
  unit suite at all, which is why this hid.
- **T-30** — 24 files under `patches/dpdk/*/windows/` are not patches. They are symlinks
  that a `core.symlinks=false` checkout materialized as text.
- **T-21** — 13 of 23 `index` lines across the patch set are stale, so `git am -3` cannot
  work. Plain `git am` ignores them, so nothing breaks today.
- **T-13** and **T-30** together prove the msys2 Windows workflow cannot pass for 3
  independent reasons, and none of them is the version pin.
- **T-36** — the Rust `no_std` example does not compile and no build runs it.

Five tasks need a person, not a command: T-13 and T-17 need a product decision, T-28 needs
a ruling on a workstation hostname in tracked history, T-31 needs somebody who can certify a
DCO sign-off, and T-27 needs an author that no route on this host can recover.
