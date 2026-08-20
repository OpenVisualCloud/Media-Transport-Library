# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Media Transport Library (MTL) — a software SMPTE ST 2110 stack for high-throughput,
low-latency media over IP. C99 library core built on DPDK, with hardware pacing on Intel
E810/E830 NICs. Supports ST2110-20 (uncompressed video), -22 (compressed/JPEG-XS),
-30 (audio), -40 (ancillary), -41 (fast metadata), plus ST2022-7 redundancy.

Additional deliverables in-tree: sample apps (`app/`), FFmpeg/GStreamer/OBS plugins
(`ecosystem/`), codec plugins (`plugins/`), `MtlManager` daemon (`manager/`),
LD_PRELOAD UDP shim (`ld_preload/`), Python and Rust bindings (`python/`, `rust/`).

## Existing agent documentation — read this first

This repository already carries a deep, maintained agent knowledge base. Prefer it over
re-deriving things from source:

| Doc | Use when |
|---|---|
| `.github/copilot-docs/mtl-knowledge-base.md` | Architecture reference (§1 design, §2 scheduler, §3 memory, §4 locking, §5 pacing, §6 session lifecycle, §7 DPDK patterns, §8 testing). Read the relevant § before any non-trivial library change. |
| `.github/instructions/mtl-c-coding.instructions.md` | Mandatory C rules — naming, memory, locking, tasklet constraints, error handling. |
| `.github/instructions/mtl-gtest.instructions.md` | Running/debugging `KahawaiTest`, suite map, pacing modes, failure signatures. |
| `.github/instructions/mtl-acceptance-*.instructions.md` | pytest E2E suite: running, authoring, engine internals, host setup. |
| `.github/instructions/mtl-system-setup.instructions.md` | Hugepages, VFs, ICE driver, MtlManager. |
| `.github/copilot-instructions.md` | Quality bar and the six-gate TDD loop the Copilot/subagent workflow uses. |
| `doc/design.md`, `doc/build.md`, `doc/run.md` | Upstream user-facing design/build/run guides. |

If the knowledge base disagrees with the code, fix the knowledge base in the same change.

## Native tooling in this repository

The Copilot workflow above has been ported to Claude Code equivalents, so the same agents,
skills, and MCP servers are usable directly.

Everything lives in `.github/claude/`, next to the Copilot originals it mirrors. Claude Code
only discovers config at fixed paths, so three tracked symlinks bridge the two:

| Discovery path | Real file |
|---|---|
| `CLAUDE.md` | `.github/claude/CLAUDE.md` (this file) |
| `.claude/` | `.github/claude/` (agents, skills, settings) |
| `.mcp.json` | `.github/claude/mcp.json` |

Edit the file under `.github/claude/`, never the symlink. Personal, machine-local overrides go in
`.github/claude/settings.local.json`, which stays gitignored.

**Subagents** (`.github/claude/agents/`) — invoke with the Agent tool:

| Agent | Use for |
|---|---|
| `mtl-developer` | Any code change to `lib/`, `include/`, `app/`, `plugins/`, `ecosystem/`, `tests/unit/`, `tests/integration_tests/`. Owns Gates 0–4 of the TDD loop (knowledge → failing test → implement → green build) in one context window. Also owns building and unit gtest. |
| `mtl-reviewer` | Adversarial read-only review of a saved diff. Gate 5 — no exemption. Refuses if `git diff` is empty. Give it scope + one-line intent; do not paste the diff. |
| `mtl-system-admin` | Host setup (hugepages, VFs, ICE, MtlManager) and running `KahawaiTest` on real VFs. MCP-only, never shell. Gate 6 for data-plane changes. |
| `mtl-planner` | Multi-subsystem work where ownership isn't obvious. Plans only — never implements. |

Built-in `Explore` covers read-only Q&A and code archaeology; use it for fan-out reads instead of
burning the specialist agents' context.

**Skills** (`.github/claude/skills/`) — `mtl-build`, `mtl-write-test`, `mtl-commit`. Each is
itself a symlink to `.github/skills/<name>/`, whose `SKILL.md` frontmatter is already valid for
both Copilot and Claude Code. So there is exactly one copy of every skill and it cannot drift,
and relative links inside a skill body resolve against `.github/skills/<name>/`.

**MCP servers** (`.github/claude/mcp.json`, enabled in `.github/claude/settings.json`) —
`mtl-system-setup` and `mtl-acceptance-setup`, both backed by `.github/mcp/`. Their tools are
deferred: call `ToolSearch` before invoking `mcp__mtl-system-setup__*` etc. The wrapper scripts
create and populate `.github/mcp/.venv` on first launch.

**Path-scoped context** — `lib/`, `tests/unit/`, `tests/integration_tests/`, and
`tests/acceptance/` each carry a small `CLAUDE.md` that imports the matching
`.github/instructions/*.md`. That reproduces the Copilot `applyTo:` auto-attach, so the C rules
or the gtest/pytest instructions load when you actually work in those trees.

One difference from the Copilot version worth knowing: `mtl-system-admin`'s "MCP tools only, never
a shell" rule is enforced by its prompt rather than by withholding the Bash tool, because MCP
wildcards in a subagent's `tools:` list aren't reliably supported. If you see it reach for Bash,
that's a bug in the agent's behavior, not permission to allow it.

## Build

```bash
./build.sh                  # release: lib -> app -> tests -> plugins -> ld_preload -> manager -> RxTxApp, each installed
./build.sh debug            # debug + ASan
./build.sh debugonly        # debug, no ASan
./build.sh unit             # configure build_unit/, build and RUN the unit gtest suite, then exit
./build.sh enable_fuzzing   # add libFuzzer harnesses
MTL_INSTALL_PREFIX=$PWD/.local_install ./build.sh   # local prefix install
```

`build.sh` is a sequence of `meson setup` + `ninja` + `ninja install` per component; each
subdirectory (`app/`, `tests/`, `plugins/`, `manager/`, `tests/tools/RxTxApp/`) is its own
meson project. Incremental library-only rebuild: `ninja -C build`.

Two install trees exist and are **not** interchangeable:

* `build/` + `/usr/local` — what gtest / `KahawaiTest` uses.
* `.local_install/mtl/bin/{MtlManager,RxTxApp}` — what the pytest acceptance suite uses
  (`tests/acceptance/mtl_engine/const.py` hardcodes `PREFIX = ".local_install"`).

DPDK must be built and installed first (patched from `patches/`); pinned dependency
versions live in `versions.env` (DPDK, ICE, JPEG-XS, FFmpeg, xdp-tools, libbpf).

## Format and lint

```bash
./checkpatch.sh             # verify everything — what CI and the git hooks run
./checkpatch.sh --staged    # verify staged files only
./format-coding.sh          # apply every autofix
./format-coding.sh --check  # preview the blast radius, then restore the tree
```

`.pre-commit-config.yaml` is the **single source of truth** for which tool, which version,
which arguments and which files; rule content lives in `.github/linters/` (plus
`.clang-format` at the root, which must stay a real file — clang-format searches upward, and
a symlink materializes as a text file on Windows). `checkpatch.sh`, the git hooks and
`.github/workflows/linter.yml` all run that one hook list and define no rule of their own.
Adding a linter or changing a rule means editing that config and nowhere else.

`pre-commit` installs the pinned clang-format 22.1.8, shfmt, shellcheck, markdownlint,
textlint, yamllint, actionlint and gitleaks itself — plus its own Node — so none of them
need to be on `PATH`; do not `apt install clang-format-22`. Bootstrap with `./checkpatch.sh --bootstrap`, install
the hooks with `./checkpatch.sh --install-hooks`. On a PEP 668 host (Fedora, Arch,
Debian 12+) `--bootstrap` cannot use pip and prints the distribution package instead.

Four CI checks are not yet reproduced locally (`BASH_EXEC`, dotenv-linter, ESLint over
`*.ts`, rustfmt/clippy) and run in the `residual-linters` job. See
[doc/coding_standard.md](../../doc/coding_standard.md) for the parity table and the rationale
behind every pin and omission.

## Tests

Three tiers, in increasing cost:

**Unit (`tests/unit/`, no NIC, no root)** — gtest against internal functions, compiled with
`-Denable_unit_tests=true` which exposes internals. This is the only tier that runs
anywhere:

```bash
./build.sh unit                                  # build + run all
./build_unit/tests/unit/UnitTest --gtest_filter='St40*'   # single test/suite after a build
```

**Integration (`tests/integration_tests/`, real VFs, root)** — `KahawaiTest`, one binary,
one global `mtl_init()` for the whole run:

```bash
sudo ./build/tests/KahawaiTest --p_port 0000:c9:01.0 --r_port 0000:c9:01.1 \
  --auto_start_stop --gtest_filter='St20p*'
sudo ./build/tests/KahawaiTest ... --pacing_way tsc --gtest_filter='St20p*'   # software pacing fallback
sudo -E .github/scripts/gtest.sh                 # full CI orchestration (port discovery, sharding, retries)
```

`--level all` runs the non-mandatory cases; CI runs mandatory only. `NoCtxTest.*` cases each
need their own process (DPDK EAL cannot re-init) — use `tests/integration_tests/noctx/run.sh`,
never a filter matching several of them.

**Acceptance (`tests/acceptance/`, pytest, root, SSH-to-localhost)** — E2E through RxTxApp /
FFmpeg / GStreamer; does not call the C API:

```bash
cd tests/acceptance
sudo -E ./venv/bin/python3 -m pytest \
  --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml \
  -m smoke -v
```

Always the venv python (`sudo python3` lacks `pytest_mfd_config`), always both config flags.
Host prep: `.github/scripts/acceptance_setup.sh` (interactive, or `--auto`). Most cases need
`/mnt/media` mounted over NFS. Never edit `conftest.py`, `common/`, or `mtl_engine/` to make a
test pass — fix the environment or config.

**Fuzz (`tests/fuzz/`)** — single-packet libFuzzer harnesses over RX parsers; see
`doc/fuzzing.md`.

## Runtime setup (needed for anything beyond unit tests)

```bash
sudo ./script/nicctl.sh create_vf 0000:af:00.0   # create + bind VFs to DPDK PMD
sudo sysctl -w vm.nr_hugepages=2048              # lost on reboot
sudo MtlManager                                  # lcore/queue arbitration daemon
```

`script/build_ice_driver.sh` builds the patched ICE module required for hardware rate-limit
pacing. A SEGFAULT in `iavf_tm_node_add` means the stock ICE driver is loaded.

## Architecture essentials

**Two-world rule.** Data plane (hugepage memory, `rte_spinlock_t`, lock-free rings, zero-copy,
polling tasklets) must never call control-plane code: no `malloc`, no `pthread_mutex`, no
`sleep`, no INFO-level logging. Control plane (session create/destroy, config, stats) may
block. Violating this is the single most common way to break pacing.

**Cooperative tasklets, not threads.** A scheduler thread calls tasklet functions in a tight
loop; a tasklet returns positive for "had work", 0 for "idle", and must never block — one
blocked tasklet starves every other tasklet on that scheduler. Lcore mode (CPU-pinned) is
default; `MTL_FLAG_TASKLET_THREAD` switches to pthreads for containers. Sessions are assigned
to schedulers by weight in 1080p-equivalents; `MT_MAX_SCH_NUM = 18`.

**Three API layers.** Pipeline (`st20p_tx_get_frame`/`put_frame` — most apps; handles
conversion and frame lifecycle) wraps Session (`st20_tx_create` + callbacks — direct
frame/slice/RTP control) wraps Transport (internal: packet build, pacing, NIC). Pipeline code
lives in `lib/src/st2110/pipeline/`, session code in `lib/src/st2110/`.

**Layout.**

* `include/` — the entire public API (`mtl_api.h`, `st20_api.h`, `st_pipeline_api.h`, …).
  Internal headers never go here.
* `lib/src/mt_*.c` — core, non-media: scheduler (`mt_sch.c`), config, PTP, DMA, flow rules,
  mcast/IGMP, stats, RTCP, instance/manager IPC.
* `lib/src/dev/` — device layer: DPDK PMD (`mt_dev.c`) and AF_XDP (`mt_af_xdp.c`).
* `lib/src/datapath/` — the virtual data-path backend abstraction: queues, shared queues,
  shared RSS, kernel-socket path. This is what lets one packet TX/RX interface serve
  DPDK PMD, kernel socket, and AF_XDP.
* `lib/src/st2110/` — per-media TX/RX sessions plus builder/transmitter split for video,
  SIMD color conversion (`st_avx2.c`, `st_avx512*.c`), RX timing parser.
* `lib/windows/` — Windows shims.

**Video vs everything else.** Video is ~4500 packets/frame with µs pacing, slot-based
reassembly, bitmap dedup, DMA offload, and a separate builder tasklet feeding a transmitter
tasklet through an `rte_ring`. Audio/ancillary/fast-metadata are 1–8 packets/frame, per-frame
pacing, single tasklet. Adding a new ST2110-xx type means copying the video session pattern
and simplifying it — don't invent a new shape.

**Prefixes** (enforced): `mt_` core internals, `mtl_` public core API, `st_`/`st20_`/`st22_`/
`st30_`/`st40_`/`st41_` media session APIs, `st20p_`/`st22p_`/`st30p_` pipeline APIs,
`tv_`/`rv_` TX/RX video internals, `tx_audio_session_`/`rx_audio_session_` etc. for the
simpler media types.

## Conventions that bite

* C99 only in `lib/`. C++ only in `tests/`.
* Return 0 on success, negative on error; free resources in reverse allocation order.
* `dbg()` / `info()` / `warn()` / `err()` — never `printf`.
* DPDK allocations via `mt_rte_zmalloc()` with an explicit `socket_id` from
  `mt_socket_id(impl, port)`; NUMA mismatch roughly doubles DMA latency.
* Lock order is manager mutex → session spinlock, never the reverse.
* TX rings use `rte_ring_sp_enqueue_bulk()` (all-or-nothing); a partial `_burst()` enqueue
  breaks RTP sequence and pacing integrity.
* Short `rte_eth_tx_burst()` → stash the remainder in `inflight[]` and return; never discard,
  never retry in a loop.
* RX frame buffers must be zero-initialized — gaps from lost packets must read as zeros.
* Mempool names need the `recovery_idx` suffix to stay unique across recovery cycles.

## Repository hygiene

Minimal diffs; no speculative helpers, no commented-out code, no reformatting lines you
aren't functionally changing. `./format-coding.sh` then `./build.sh` before proposing a
change; add or extend a test at the cheapest tier that can catch the bug.

A new lint or formatting rule goes in `.pre-commit-config.yaml` and nowhere else — not in a
workflow, not in a script, not in a document. Anything else is drift by construction.
