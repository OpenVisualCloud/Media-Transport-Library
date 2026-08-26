---
description: "Running, debugging, and interpreting MTL integration tests (gtest / KahawaiTest). Covers test binaries, CLI flags, test suites, pacing modes, and failure patterns."
applyTo: "tests/integration_tests/**,.github/scripts/gtest.sh"
---

# MTL Integration Tests (gtest) — Agent Instructions

## Binaries

| Binary | Location | Purpose |
|--------|----------|---------|
| `KahawaiTest` | `build/tests/KahawaiTest` | Main integration tests |

## CLI Flags

```text
--p_port <BDF>          Primary (producer) port
--r_port <BDF>          Receiver port
--auto_start_stop       Auto start/stop sessions (always use)
--pacing_way <mode>     auto | rl | tsn | tsc | ptp | be. Default auto, which
                        picks RL only when the driver advertises TM
                        (lib/src/dev/mt_dev.c:1452-1461) and TSC otherwise
--dma_dev <p>,<r>       DMA devices for DMA-accelerated tests
--rss_mode <mode>       l3_l4 for RSS tests
--p_sip <IP>            Primary station IP (auto-generated if omitted)
--log_level <level>     debug | info | notice | warning | error. The binary
                        defaults to error, which mutes the `dpdk version:`
                        banner; notice is the quietest level that still prints it
--no_ctx_tests          Select the NoCtxTest suite and skip the shared mtl_init
--port_list <BDFs>      Comma-separated ports, up to MTL_PORT_MAX. Any suite
                        accepts it; NoCtxTest uses it instead of --p_port/--r_port
--gtest_filter=<pat>    Filter tests (supports wildcards)
--gtest_list_tests      List tests without running
```

## Test Suite Map

| Filter | Coverage | Typical Duration |
|--------|----------|-----------------|
| `St20p*` | ST2110-20 pipeline (video) — most comprehensive | ~3 min |
| `St20_tx*` | ST2110-20 raw TX | ~2 min |
| `St20_rx*` | ST2110-20 raw RX | ~2 min |
| `St22*` | ST2110-22 compressed video | ~1 min |
| `St30*` | ST2110-30 audio | ~1 min |
| `St40*` | ST2110-40 ancillary | ~30s |
| `St41*` | ST2110-41 fast metadata | ~30s |
| `Misc*` | Miscellaneous / utility | ~30s |
| `Sch*` | Scheduler tests | ~1 min |
| `Dma*` | DMA engine tests (need --dma_dev) | ~1 min |
| `Cvt*` | Color conversion tests | ~30s |

## Pipeline TX Flag Semantics

- `*_TX_FLAG_DROP_WHEN_LATE` requires `*_TX_FLAG_USER_PACING`; use a TAI frame timestamp for deterministic late-frame checks.
- A dropped frame triggers `notify_frame_done` with `ST_FRAME_STATUS_DROPPED` and also triggers `notify_frame_late`; the callbacks are not mutually exclusive.
- Buffer reclamation does not prove callback completion; synchronize callback observations independently.
- `*_TX_FLAG_BLOCK_GET` timeouts apply to each `get_frame` wait and must exceed any deliberately scheduled future timestamp.
- Every frame returned by `get_frame` remains application-owned until it is submitted or released with the matching `put_frame_abort` API.

## Quick Run (MCP tool preferred)

```bash
# Via MCP tool — handles port discovery and structured output:
run_gtest(gtest_filter="St20p*")
run_gtest(gtest_filter="St30*")

# Via terminal (supports extra flags like --pacing_way tsc):
sudo ./build/tests/KahawaiTest \
  --p_port 0000:c9:01.0 --r_port 0000:c9:01.1 \
  --auto_start_stop --gtest_filter="St20p*"
```

## CI Script

`.github/scripts/gtest.sh` provides full CI orchestration:
- Auto-discovers VF ports and DMA devices
- Shards `St20_rx*` and `St20_tx*` into 2 parts for parallelism
- Randomizes station IPs
- Retries failed tests (MAX_RETRIES=2, RETRY_DELAY=20s)
- TEST_CASE_TIMEOUT=1800s per test case

Key env vars: `TEST_PORT_1..4`, `TEST_DMA_PORT_P`, `TEST_DMA_PORT_R`, `NIGHTLY=1`

## Noctx Tests

Separate `NoCtxTest.*` suite that needs isolated `mtl_init`/`mtl_uninit`. DPDK
EAL cannot be re-initialised inside a single process, so **each test case must
run in its own KahawaiTest process**. Never pass a filter that matches multiple
NoCtxTest cases to a single `KahawaiTest` invocation — the second case will
fail with `dev_eal_init, eal not support re-init`.

`run.sh` takes 4 VF ports (`TEST_PORT_1..4`) and waits 20s between processes.
`run_pf.sh` takes 2 PF ports (`TEST_PF_PORT_1/2`) for the `_pf_` cases, whose
TSN/launch-time pacing offload no VF driver advertises, and waits 10s:

```bash
TEST_PORT_1=... TEST_PORT_2=... TEST_PORT_3=... TEST_PORT_4=... \
  bash tests/integration_tests/noctx/run.sh
```

`run.sh` enumerates `NoCtxTest.*` via `--gtest_list_tests` and spawns one
process per test. The MCP tool `run_noctx_tests(gtest_filter=...)` does the
same enumeration + one-process-per-test loop; it accepts filters that resolve
to many cases (e.g. `*nonsplit*`, `NoCtxTest.st40i_*`) and reports per-test
pass/fail. `run_noctx_pf_tests` covers the `_pf_` cases `run_noctx_tests`
excludes. Each tool's `cooldown_seconds` default matches the script it mirrors —
20 for `run.sh`, 10 for `run_pf.sh`.

The argv both tools build, their listing parser and their sudo/exit-code
handling are pinned by a no-NIC, no-subprocess unittest suite. Run it after any
edit to `.github/mcp/mtl_mcp_server.py`:

```bash
.github/mcp/.venv/bin/python -m unittest discover -s .github/mcp -v
```

## Interpreting Results

### Normal warnings (not failures)
- `Error: RX_VIDEO_SESSION: unrecovered pkts N` — packet loss under test load, expected in non-dedicated environments
- `Error: DEV: Status: rx_hw_dropped_packets N` — NIC hardware drops, normal at high rates

### Actual failures
- **SEGFAULT in `iavf_tm_node_add`** → Stock kernel ICE driver. Fix: `ice_driver_rebuild` + re-create VFs.
- **`librte_*.so not found`** → Run `sudo ldconfig` or rebuild DPDK.
- **Permission denied on CMakeCache.txt** → `mtl_clean_rebuild`.
- **Test timeout** → Check if MtlManager is running. Check NUMA locality.

### Pacing modes
- **`auto`** (default): Uses hardware rate limiter (RL) on E810/E830 with patched ICE. Best accuracy.
- **`tsc`**: Software TSC-based pacing. Works without patched ICE. Use to isolate pacing bugs.
- If tests pass with `tsc` but fail with `auto` → ICE driver or NIC firmware issue.

## 4-Port / Redundancy Tests

Tests named `*redundant*` need 4 ports. The test skips gracefully if only 2 ports are provided:
```text
TestBody, need 4 ports for redundant test, skipping
```

## Retry Strategy

- Some tests are flaky under ASAN builds — retry once before reporting failure.
- CI uses MAX_RETRIES=2 with RETRY_DELAY=20s.
- If a test fails consistently across retries, it's a real issue.

## Acceptance Tests (pytest)

For the pytest-based acceptance_tests framework under `tests/acceptance/tests/single/`, see
`.github/instructions/mtl-acceptance-tests.instructions.md`.
