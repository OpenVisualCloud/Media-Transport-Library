---
name: mtl-write-test
description: Author a new MTL test. Use when adding coverage for a bug fix, new feature, regression, or behavior change. Teaches how to pick the right tier (unit / integration / NoCtx / pytest single-host / pytest dual-host) and how to write a test that will survive review. Does NOT cover running existing tests, debugging an existing test failure, host setup, or build infrastructure — see the linked instructions for those.
---

# Write a new MTL test

The hard part of testing MTL is **picking the right tier** and **asserting the right thing**.

## 1. The five tiers

| Tier | Binary | Hardware | Can observe |
|---|---|---|---|
| **Unit gtest** — `tests/unit/` | `UnitTest` (built with `-Denable_unit_tests=true`; via `./build.sh unit`) | None — runs as regular user under ASan | RX-side session logic fed synthetic mbufs at the RFC 4175 / RTP payload layer: frame assembly, bitmap dedup, redundancy merge, stat counters, pipeline accounting |
| **Integration gtest** — `tests/integration_tests/` | `KahawaiTest` | Real VFs (≥2; ≥4 for redundant cases) | Full session lifecycle, TX+RX through the NIC, every pacing mode, multi-session, callbacks at real timing, SHA digests, DMA, RSS, kernel-socket, AF_XDP, virtio-user |
| **NoCtx gtest** — `tests/integration_tests/noctx/` | `KahawaiTest --no_ctx` | Real VFs (per-case; redundant cases require ≥4) | Same as integration plus behaviors that depend on `mtl_init()` flags/callbacks the shared test context cannot supply. **One case per process** (DPDK EAL cannot re-init) — runner script enforces this |
| **pytest single-host** — `tests/validation/tests/single/` | RxTxApp / FFmpeg / GStreamer driven through pytest | Full host: VFs, MtlManager, NFS media, plugins, venv, SSH-to-localhost | End-to-end app behavior on one host: CLI flags, config files, real media bytes, PTP convergence |
| **pytest dual-host** — `tests/validation/tests/dual/` | same apps, two hosts | Two hosts wired together | Real two-machine traffic: cross-host pacing, real switch behavior, no loopback shortcuts |

Per-tier docs you must read before authoring: [tests/unit/README.md](tests/unit/README.md), [tests/integration_tests/noctx/noctx.md](tests/integration_tests/noctx/noctx.md), [tests/validation/README.md](tests/validation/README.md).

## 2. Picking the tier — disjoint rules, stop at the first **yes**

1. Two physical hosts required? → **pytest dual-host**.
2. Real app binary, CLI flag, config file, or media file on disk required? → **pytest single-host**.
3. `mtl_init()` flags or callbacks the shared context cannot supply (user pacing/timestamp, simulated packet loss, fake PTP)? → **NoCtx**.
4. Packet on the wire, scheduler running, qualitative pacing behavior (modes, switches, fallbacks — *not* sub-microsecond accuracy, which is pytest+PTP), real-time callbacks, DMA, RSS, kernel-socket, AF_XDP, virtio-user, or 2+ ports interacting? → **integration**.
5. Reproducible by feeding synthetic mbufs to a single RX session handler with no scheduler? → **unit**.

Trap: a library bug whose easiest reproducer is RxTxApp still belongs in a **gtest** — pytest is for app behavior, not library bug isolation.

## 3. Universal quality rules

- **One requirement per test.** The test name encodes the requirement; `test1`, `basic`, `it_works` are rejects. *Why:* a failing test must point at exactly one broken behavior.
- **Assert observable behavior through the public API or harness.** Never cast handles to `impl` structs; never read private fields.
- **Always assert.** Every test ends in at least one `EXPECT_*` / `ASSERT_*` / `assert`. *Why:* without an assertion, a regression that silently no-ops the feature still passes.
- **Reverse the creation order on teardown; check every `_free >= 0`.** *Why:* in a long-running gtest binary a leaked `_free` leaves named DPDK mempools/rings that subsequent cases collide on or exhaust the hugepage pool against.
- **Deterministic — no `sleep` to wait for "probably done".** Use the tier's wait primitive (synchronous in unit; an explicit, commented usleep budget in integration; the framework-owned `test_time` fixture in pytest). *Why:* `sleep` is flaky under load and impossible to debug.
- **Self-contained.** A single test must pass in isolation, in any order, repeated.
- **Fails before fix (regression tests).** Run against the unpatched code first and confirm it fails for the right reason; mention the failure mode in the commit. *Why:* a test that passes both before and after proves nothing.
- **No dead test code.** No commented-out cases, no `DISABLED_` without an issue link, no `if (0)` blocks.

## 4. Tier-specific rules

### Unit
- AddressSanitizer is preloaded on every run (`LD_PRELOAD=libasan.so.<major>` — current major in [tests/unit/README.md](tests/unit/README.md)). Any leak fails the suite.
- Drain shared state in `TearDown()` — the harness exposes a process-global ring; entries left in it cause "passes alone, fails in suite".
- Synthetic packets only. If you need a real capture, you are in the wrong tier.
- EAL init is idempotent but global: first `ut*_init()` configures DPDK with `--no-huge --no-pci --vdev=net_null0`; later calls cannot reconfigure it.

### Integration & NoCtx
- **Gate every case on the hardware it needs** (`num_ports`, DMA availability, NIC family, pacing mode). Integration idiom: `if (…) { info(...); return; }`. NoCtx pre-checks: `throw std::runtime_error(...)`. `GTEST_SKIP` is reserved for runtime non-determinism inside a test body (e.g. an RX frame that did not arrive), not for pre-checks.
- **NoCtx: one case per process.** *Why:* DPDK EAL cannot re-initialise inside the same PID; the fixture refuses and the runner enforces it — never try to "fix" this.
- **Pacing.** MTL exposes several modes (AUTO, RL, TSC, PTP, TSN, BE, …). If the behavior depends on a mode, set it explicitly or parametrize.
- **Bound the run.** Pick the shortest duration that surfaces the behavior, and write the math in a comment (e.g. "2 s × 60 fps ≈ 120 frames; expected drop < 1 % surfaces in N frames").

### pytest (single-host and dual-host)
- **No `time.sleep` in tests.** Run duration is owned by the `test_time` fixture and the app harness. Framework code may sleep for synchronisation; test bodies may not.
- **Parametrize via `@pytest.mark.parametrize` and fixtures, not Python loops.** Use `indirect=["media_file"]` and supply `ids=` for readable case names. *Why:* a Python loop produces one opaque pass/fail; parametrize produces one rerunnable case per ID.
- **Source media from `tests/validation/mtl_engine/media_files.py`** (`yuv_files`, `yuv_files_422rfc10`, `audio_files`, `anc_files`, …) via the `media_file` fixture. Never hard-code an NFS or developer-home path; mount points are per-host in `topology_config.yaml` / `test_config.yaml`.
- **Do not edit the top-level `tests/validation/conftest.py`, `common/`, or `mtl_engine/`** — those are framework. A **per-category `conftest.py` next to your tests is fine** (examples: `tests/single/xdp/conftest.py`, `tests/dual/performance/conftest.py`).
- **Check SSH return codes.** All commands run over SSH-to-localhost even on a single host; pass `expected_return_codes=None` and `pytest.skip(...)` on a missing optional resource.
- **No build registration.** pytest auto-discovers `test_*.py`; if you are editing a meson file for a pytest, you are in the wrong file.

## 5. The authoring loop

1. **Pick a neighbour test in the same directory** — closest to your behavior first, else the most recently modified file. Avoid `DISABLED_` cases and any file noticeably older than its siblings. **Copy its conventions, including its marker set** — markers gate CI selection ([tests/validation/pytest.ini](tests/validation/pytest.ini)) and an unmarked or wrongly-marked pytest runs in no job.
2. **Modify assertions only** until the test encodes your one requirement.
3. **Register in the build** for gtest tiers (add to the relevant `*_sources` list in the nearest `meson.build`). pytest needs no registration.
4. **Build:** `./build.sh` for integration binaries; `./build.sh unit` for the unit suite (builds `build_unit/` and runs `UnitTest`). See [`/mtl-build`](.github/skills/mtl-build/SKILL.md).
5. **Run** with the binary / pytest invocation from the tier's auto-loaded instructions.
6. **Confirm** the test fails before the fix (regression) or passes deterministically across 3+ consecutive runs (new coverage).

## Related

- Run an existing test → [.github/instructions/mtl-gtest.instructions.md](.github/instructions/mtl-gtest.instructions.md) (gtest) or [.github/instructions/mtl-validation-tests.instructions.md](.github/instructions/mtl-validation-tests.instructions.md) (pytest).
- Build / format → [`/mtl-build`](.github/skills/mtl-build/SKILL.md).
- Host setup → MTL System Admin agent (gtest) or `.github/scripts/validation_setup.sh`/`mtl-validation-setup` MCP tools directly (pytest, no dedicated agent).
- TDD discipline + implementation → MTL Developer (TDD) agent (this skill is loaded by Gate 2 of its six-gate loop). Adversarial review → MTL Reviewer agent.
