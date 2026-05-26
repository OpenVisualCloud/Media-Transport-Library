---
description: "Running, debugging, and interpreting MTL integration tests (gtest / KahawaiTest). Covers test binaries, CLI flags, test suites, pacing modes, and failure patterns."
applyTo: "tests/integration_tests/**,.github/scripts/gtest.sh"
---

# MTL Integration Tests (gtest) — Agent Instructions

## Binaries

| Binary | Location | Purpose |
|--------|----------|---------|
| `KahawaiTest` | `build/tests/KahawaiTest` | Main integration tests |
| `KahawaiUfdTest` | `build/tests/KahawaiUfdTest` | User-frame-driver tests |
| `KahawaiUplTest` | `build/tests/KahawaiUplTest` | UDP preload tests |

## CLI Flags

```text
--p_port <BDF>          Primary (producer) port
--r_port <BDF>          Receiver port
--auto_start_stop       Auto start/stop sessions (always use)
--pacing_way <mode>     auto (default, uses RL) | tsc (software fallback)
--dma_dev <p>,<r>       DMA devices for DMA-accelerated tests
--rss_mode <mode>       l3_l4 for RSS tests
--p_sip <IP>            Primary station IP (auto-generated if omitted)
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

Requires 4 VF ports. Run serially with a cooldown (10s) between processes:

```bash
TEST_PORT_1=... TEST_PORT_2=... TEST_PORT_3=... TEST_PORT_4=... \
  bash tests/integration_tests/noctx/run.sh
```

`run.sh` enumerates `NoCtxTest.*` via `--gtest_list_tests` and spawns one
process per test. The MCP tool `run_noctx_tests(gtest_filter=...)` does the
same enumeration + one-process-per-test loop; it accepts filters that resolve
to many cases (e.g. `*nonsplit*`, `NoCtxTest.st40i_*`) and reports per-test
pass/fail.

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

## Validation Tests (pytest)

For the pytest-based validation framework under `tests/validation/tests/single/`, see
`.github/instructions/mtl-validation-tests.instructions.md`.
