# MTL Unit Tests

> **In-process tests for the Media Transport Library that find real bugs without requiring a NIC, hugepages, root, or PTP hardware.**

[![ASan](https://img.shields.io/badge/ASan-libmtl%20only-blue)](#quick-start)
[![No hardware](https://img.shields.io/badge/hardware-not%20required-success)](#what-makes-this-different)

---

## What this is

A `gtest` binary (`UnitTest`) that drives MTL's RX session and pipeline code
paths **directly** — using real DPDK mbufs from a no-hugepage EAL — and
asserts on production behaviour. The suite covers ST 2110-20 (video),
ST 2110-30 (audio), ST 2110-40 (ancillary), and the ST 2110-40 pipeline. Tests
run on any developer laptop in a fraction of a second.

TX packet building/queueing, DMA, kernel-socket / AF_XDP backends, and
multi-process scenarios are out of scope — see
[`tests/integration_tests/`](../integration_tests/) and
[`tests/acceptance/`](../acceptance/) for those. TX epoch/pacing *math*
(`session/st20_tx_harness.c`) is covered here.

## What makes this different

| Property            | Value                                                       |
| ------------------- | ----------------------------------------------------------- |
| Hardware required   | **None** — no NIC, no PTP clock, no hugepages               |
| Privileges required | **None** — runs as a regular user                           |
| Memory checking     | **Partial** — `libmtl.so` only, not this binary; see below  |
| Code under test     | The **production `.c` files** (not a separate test build)   |
| Determinism         | Synthetic input; some cases spawn threads                   |
| Isolation           | One `ut_eal_init()` per process; per-test ring/state reset  |

If a test fails it's a real defect, unless a case with a wall-clock budget ran
over it under load — an overrun that reproduces on an idle box is a defect. Only the
eight `*Concurrency*` suites, `St20PipelineTxBlocking`, and `FfmpegMtlCommonTest.ConcurrentGetsCreateOneSharedHandle` carry a budget;
the exception reaches no other case. There are no flaky network-timing tests in this binary.

## Quick start

```bash
./build.sh unit
```

This configures `build_unit/` with `-Denable_unit_tests=true`, builds it, and
runs `tests/unit/UnitTest`.

AddressSanitizer covers this tier only in part. `./build.sh debug unit` and
`MTL_BUILD_ENABLE_ASAN=true ./build.sh unit` set `-Denable_asan=true`, which `lib/meson.build`
adds to the `libmtl.so` compile arguments only. Both commands configure and build cleanly, but
the resulting suite is not green: `St22PipelineConcurrency.RxSingleProducerMultiConsumerNoDeadlock`
takes a SEGV and the sanitizer aborts the process. gtest prints no summary, and the binary exits
nonzero part-way through the 513 cases — one observed run stopped after 482 of them had reported
`OK`. `build.sh` runs under `set -e`, so the command dies there with a raw sanitizer backtrace.
To compare the ASan and non-ASan runs, exclude the suite: `--gtest_filter=-St22PipelineConcurrency.*` runs
the other 511 of the 513 cases and all pass. The crash needs `enable_asan` — the same case
passes 3/3 without it. Before filing a defect against `lib/`, read the allocator paragraph
below; no causal link between the two is established. Tracked as T-61.

Coverage also splits: no `UnitTest` object gets `-fsanitize=address`, so the 17 production `.c`
files this suite `#include`s into harness `.c` files are unchecked. The 237 shadowed symbols
therefore resolve to uninstrumented code — 231 from the 16 `lib/` copies plus 6 harness stubs.
`ecosystem/ffmpeg_plugin/mtl_common.c` is the one file of the 17 that contributes none of the
231. Shadowing needs a `libmtl.so` symbol, and nothing `mtl_common.c` itself defines is in
`libmtl.so`. `ffmpeg/mtl_common_harness.c` still shadows and defines 4 of the 6 stubs.
`pipeline/st30p_tx_harness.c` defines the other 2.

`libmtl.so` *is* instrumented, so the 153 symbols `UnitTest` resolves from it are checked —
that covers any file no harness includes, such as `st2110/st_ancillary.c`. `build.sh` preloads
the ASan runtime so the instrumented `libmtl.so` does not fault on an init-order interposition.

Every symbol count above comes from `nm` over the built tree:

```sh
cd build_unit
nm --defined-only tests/unit/UnitTest.p/*.o | grep -oP '^\S+ [A-Z] \K\S+' | sort -u > own
nm -D --defined-only lib/libmtl.so | grep -oP '^\S+ [A-Z] \K\S+' | sort -u > dso
nm -u tests/unit/UnitTest.p/*.o | grep -oP '^ +U \K\S+' | sort -u > undef
grep -rhoP '^#include "\K[^"]+\.c(?=")' ../tests/unit | grep -v '^\.\.' |
  sed 's|/|_|g; s|^|lib/libmtl.so.p/src_|; s|$|.o|' | sort -u | xargs nm --defined-only |
  grep -oP '^\S+ [A-Z] \K\S+' | sort -u > copies
comm -12 own dso > shadowed
wc -l < shadowed                             # 237 shadowed
comm -12 shadowed copies | wc -l             # 231 from the 16 lib/ copies
comm -23 shadowed copies | wc -l             # 6 harness stubs; drop wc -l to name them
comm -12 undef dso | comm -23 - own | wc -l  # 153 resolved from the DSO
```

Dropping the `comm -23 - own` step gives 195, counting symbols a harness copy already defines.

The same option also forks an allocator API. `-DMTL_HAS_ASAN` rides in `mtl_c_args` next to the
sanitizer flag, and `lib/src/mt_mem.h` keys on it to switch `mt_rte_zmalloc_socket()` and its
siblings from `static inline` DPDK wrappers to extern tracked allocators. The harness copies
never see the macro, so they compile the `static inline` branch, which emits no global symbol —
the two implementations name one set of allocators but never collide at link time, and no linker
flag is involved. For ASan over library code, use
[`tests/integration_tests/`](../integration_tests/).

To configure/build/run manually, or to filter tests (see below):

```bash
# 1. configure (one-time)
meson setup build_unit -Denable_unit_tests=true

# 2. build
ninja -C build_unit

# 3. run (an enable_asan build needs the runtime preloaded — see Troubleshooting)
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

### ST20 continuation bounds regression

`St20RxHeaderValidationTest.FinalRowContinuationPastFrameDropped` feeds the
production RFC 4175 frame handler a continuation SRD whose second row begins
past its frame buffer. It needs no NIC, root, hugepages, or packet injector.

```bash
meson setup build_unit -Denable_unit_tests=true
ninja -C build_unit tests/unit/UnitTest
./build_unit/tests/unit/UnitTest \
    --gtest_filter='St20RxHeaderValidationTest.FinalRowContinuationPastFrameDropped'
```

The test geometry is two 40-byte rows with a padded 48-byte stride, making an
80-byte frame. It sends row 1 with a 12-byte first SRD and a 12-byte
continuation. The original offset check accepts the first copy, while the
second copy would begin at byte 96. Correct behavior is a negative return and
one `stat_pkts_offset_dropped` increment. Before the continuation bound check,
this test fails with `rc` equal to `0` and the drop counter equal to `0`.

`St20RxHeaderValidationTest.PaddedLineContinuationAccepted` is the companion
positive test. It verifies a valid padded continuation remains accepted.

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

| Symptom                                                    | Likely cause                                                                                   | Fix                                                                                                     |
| ---------------------------------------------------------- | ---------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------- |
| `ASan runtime does not come first in initial library list` | An `enable_asan=true` `libmtl.so`, launched without `LD_PRELOAD=libasan.so`                    | Launch through `./build.sh debug unit` or `MTL_BUILD_ENABLE_ASAN=true ./build.sh unit`; both preload it |
| `libasan: failed to find runtime library`                  | Wrong libasan major (e.g. .so.5 vs .so.6)                                                      | `cc -print-file-name=libasan.so` gives the right path                                                   |
| Linker: multiple definition of `<symbol>`                  | Two harness objects in `unit_sources` each `#include` a production `.c` defining that symbol    | Rename it in one of them with `#define`/`#undef` around the `#include`, as `pipeline/st22p_harness.c` does |
| Test passes alone, fails in suite                          | Shared ring or session state not drained                                                       | Add cleanup to fixture `TearDown()`; never rely on test order                                           |
| New test green but suite total didn't grow                 | The `.cpp` was not added to `unit_sources`                                                     | Add to [`tests/unit/meson.build`](meson.build)                                                          |
