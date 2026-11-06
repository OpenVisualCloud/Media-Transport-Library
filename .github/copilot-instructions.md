# Media Transport Library (MTL): review instructions

These instructions are for Copilot code review of MTL pull requests.

## Overview

MTL implements SMPTE ST 2110 for professional media transport over IP. It is
DPDK-based, with hardware pacing on Intel E810/E830. It supports ST 2110-20
(video), -22 (compressed video), -30 (audio), -40 (ancillary data) and -41
(fast metadata, an opt-in mode of the -40 sessions).

## Production-quality bar

MTL is a production repository that clients see.

- Performance and maintainability first: the simplest design that is fast and
  easy to understand. No patch around a problem.
- Minimal diffs: no speculative code, convenience wrappers, or refactors that
  the change does not need.
- No dead code: no commented-out code, unused helpers, or placeholder stubs.
- No unrelated changes to formatting, comments, or names.
- Each patch is self-contained: it builds, it is tested, and it is small
  enough to review at one time.

## Rules to check

- C99 in `lib/`, plus C11 `<stdatomic.h>` atomics. C++ only in tests.
- Prefixes: `mt_` core internals, `mtl_` public core API,
  `st_`/`st20_`/`st22_`/`st30_`/`st40_` media APIs,
  `st20p_`/`st22p_`/`st30p_`/`st40p_` pipeline APIs.
- Two-world rule: the data plane (tasklets, hugepage memory, lock-free rings)
  never calls `malloc`, `pthread_mutex`, `sleep`, or an INFO-level log. A
  tasklet never blocks.
- Lock order: the manager mutex, then the session spinlock, never the reverse.
  Session migration: the target manager mutex, then the source manager mutex.
- Errors: 0 on success, a negative value on error; free resources in the
  reverse order of allocation.
- DPDK memory: `mt_rte_zmalloc_socket(sz, mt_socket_id(impl, port))`; the
  control-plane heap uses `mt_zmalloc()`.
- TX rings: `rte_ring_sp_enqueue_bulk()` (all or nothing). A short
  `rte_eth_tx_burst()` keeps the rest in `inflight[]`: never discard, never
  retry in a loop.
- Logs: `dbg()`, `info()`, `warn()`, `err()`. Never `printf`.
- Format: `./format-coding.sh`, with the tool versions of
  `.pre-commit-config.yaml`.
- Scripts: `script/` holds only the user tools. Each script there sources
  `common.sh` and has `show_help()` and `main()`; a file that cannot follow
  that format does not go in `script/`. CI scripts, workflows and `ci:` tasks stay
  simple, and a CI job never sets a host setting (`doc/ci_runner_setup.md`).
- Commit messages: section 7 of `doc/coding_standard.md`.
- A change of behavior comes with a test at the cheapest tier that can find
  the fault: unit, integration (`KahawaiTest`), or pytest.
- TX pacing gets a strict review: the `*pacing*`, `*_rl_*` and epoch code of
  `lib/src/st2110/st_tx_video_session.c`, `st_video_transmitter.c`,
  `st_tx_audio_session.c` and `st_tx_ancillary_session.c`, and the rate limit
  of `lib/src/dev/mt_dev.c`. The change comes with a unit test in
  `tests/unit/`, and the packets stay inside the timing of SMPTE ST 2110-21
  §6 and §7.1 for the sender type (ST 2110-40 §6 for ancillary data). A
  missing test, or a timing that the review cannot confirm, is a blocker.
