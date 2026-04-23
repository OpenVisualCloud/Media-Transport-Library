# MTL Unit Tests

> **In-process tests for the Media Transport Library that find real bugs without requiring a NIC, hugepages, root, or PTP hardware.**

[![ASan](https://img.shields.io/badge/ASan-required-blue)](#quick-start)
[![No hardware](https://img.shields.io/badge/hardware-not%20required-success)](#what-makes-this-different)

---

## What this is

A `gtest` binary (`UnitTest`) that drives MTL's RX session and pipeline code
paths **directly** — using real DPDK mbufs from a no-hugepage EAL — and
asserts on production behaviour. The suite covers ST 2110-20 (video),
ST 2110-30 (audio), ST 2110-40 (ancillary), and the ST 2110-40 pipeline. Tests
run on any developer laptop in a fraction of a second.

TX paths, DMA, kernel-socket / AF_XDP backends, and multi-process scenarios
are out of scope — see [`tests/integration_tests/`](../integration_tests/) and
[`tests/validation/`](../validation/) for those.

## What makes this different

| Property            | Value                                                       |
| ------------------- | ----------------------------------------------------------- |
| Hardware required   | **None** — no NIC, no PTP clock, no hugepages               |
| Privileges required | **None** — runs as a regular user                           |
| Memory checking     | **AddressSanitizer** preloaded on every run                 |
| Code under test     | The **production `.c` files** (not a separate test build)   |
| Determinism         | **Single-threaded**, no timers, no I/O                      |
| Isolation           | One `mtl_init` per process; per-test ring/state reset       |

If a test fails it's a real defect — there are no flaky network-timing tests
in this binary.

## Quick start

```bash
# 1. configure (one-time)
meson setup build_unit -Denable_unit_tests=true

# 2. build
ninja -C build_unit

# 3. run
LD_PRELOAD=/lib/x86_64-linux-gnu/libasan.so.6 \
    ./build_unit/tests/unit/UnitTest
```

Filter to a single suite or test:

```bash
# one suite
./build_unit/tests/unit/UnitTest --gtest_filter='St40RxRedundancyTest.*'

# one test
./build_unit/tests/unit/UnitTest \
    --gtest_filter='St20RxErrPacketsTest.WrongPtCountedAsErr'

# everything related to err_packets across all media
./build_unit/tests/unit/UnitTest --gtest_filter='*ErrPackets*'

# list everything without running
./build_unit/tests/unit/UnitTest --gtest_list_tests
```

> **Why `LD_PRELOAD=libasan.so`?** `libmtl.so` is built with AddressSanitizer
> when `enable_unit_tests=true`. Preloading the runtime first prevents
> init-order interposition issues between gtest, libstdc++, and libasan.

## Test layout

Tests are organised per-media. Each session type has its own subdirectory
under `session/`, with per-feature files inside:

- `session/st20/` — ST 2110-20 (video)
- `session/st30/` — ST 2110-30 (audio)
- `session/st40/` — ST 2110-40 (ancillary)
- `pipeline/`     — pipeline layer (frame assembly above the session filter)

The shared per-media gtest fixture lives in `<media>/<media>_rx_test_base.h`;
each per-feature file derives a thin subclass so `--gtest_filter` selects only
that feature's tests.

For the always-current list of suites and tests:

```bash
./build_unit/tests/unit/UnitTest --gtest_list_tests
```

## Troubleshooting

| Symptom                                   | Likely cause                                                                                  | Fix                                                                                |
| ----------------------------------------- | --------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| `AddressSanitizer:DEADLYSIGNAL` at startup| `LD_PRELOAD=libasan.so.6` not set                                                             | Use the launch command shown above                                                 |
| `libasan: failed to find runtime library` | Wrong libasan major (e.g. .so.5 vs .so.6)                                                     | `dpkg -L libasan6 \| grep libasan.so` to locate the right path                     |
| Linker: multiple definition of `<symbol>` | New `.c` file in `unit_sources` is missing the `#undef MTL_HAS_USDT` or wrong `#include` order| Match an existing harness `.c` exactly                                             |
| Test passes alone, fails in suite         | Shared ring or session state not drained                                                      | Add cleanup to fixture `TearDown()`; never rely on test order                      |
| `EAL: cannot init memory` on second run   | Stale shared-memory file under `/var/run/dpdk`                                                | `--no-shconf --in-memory` are already set; delete `/dev/hugepages/rtemap_*` if any leaked |
| New test green but suite total didn't grow| The `.cpp` was not added to `unit_sources`                                                    | Add to [`tests/unit/meson.build`](meson.build)                                     |
