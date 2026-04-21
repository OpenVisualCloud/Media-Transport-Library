# MTL Unit Tests

Lightweight, in-process tests that drive production MTL code paths
**without a NIC, hugepages, or `sudo`**. They build into a single gtest
binary, `tests/unit/UnitTest`, and run on any developer machine.

## Test philosophy

These tests are written to **find bugs**, not to mirror current behavior.
Concretely:

- Every assertion specifies what the library **must** do, with a comment
  explaining why an app would be broken if the assertion did not hold.
- When the lib does something illogical, the test **fails** — we fix the
  lib, not the test.
- Counters, statuses, and stat overlays are checked against invariants
  (e.g. `corrupted ≤ received`, `dropped == busy` at the same site), not
  against fixed magic numbers extracted from the source.
- "Sanity asserts" (`EXPECT_GT(x, 0)`) are added next to equality checks
  whenever the equality could be trivially satisfied with both sides zero.

If you find yourself weakening an assertion to make a test pass: stop,
re-read the lib, and decide whether the lib is wrong or your inputs are
wrong — but never silently delete the assertion.

## Suites

| Suite | Production source | Layer | What it tests |
|-------|-------------------|-------|---------------|
| `St20RedundancyTest` | `lib/src/st2110/st_rx_video_session.c` | session | RFC 4175 packet validation, slot bitmap, redundancy filtering |
| `St30RedundancyTest` | `lib/src/st2110/st_rx_audio_session.c` | session | Audio frame assembly, redundancy filtering, frame eviction |
| `St40RedundancyTest` | `lib/src/st2110/st_rx_ancillary_session.c` | session | RFC 8331 ANC validation, per-port sequence tracking, F-bit guard |
| `St40PipelineRxTest` | `lib/src/st2110/pipeline/st40_pipeline_rx.c` | pipeline | Frame assembly above the session redundancy filter, frame status (COMPLETE/CORRUPTED), pipeline-owned stat counters |

Each session suite calls a `fuzz_handle_pkt` wrapper exposed under
`MTL_ENABLE_FUZZING_ST*`. The pipeline suite drives the
`rx_st40p_rtp_ready()` tasklet callback directly via a mock
`packet_ring`.

## Build & run

```bash
# one-time
meson setup build -Denable_unit_tests=true

# build
ninja -C build

# run all
LD_PRELOAD=/lib/x86_64-linux-gnu/libasan.so.6 ./build/tests/unit/UnitTest

# run one suite
./build/tests/unit/UnitTest --gtest_filter='St40PipelineRxTest.*'

# run one test
./build/tests/unit/UnitTest --gtest_filter='St40PipelineRxTest.GetSessionStatsOverlay'
```

`LD_PRELOAD=libasan.so` is required because `libmtl.so` is built with
ASan; preloading the runtime first prevents init-order issues.

## Architecture

### File layout

```
tests/unit/
├── meson.build                # one executable, all sources listed
├── main.cpp                   # gtest entry point
├── README.md                  # this file
├── common/
│   ├── ut_common.h            # EAL init, mempool, ring factory
│   └── ut_common.c
├── session/                   # session-layer suites (raw RTP handler)
│   ├── st20_harness.{h,c}     ; st20_test.cpp
│   ├── st30_harness.{h,c}     ; st30_test.cpp
│   └── st40_harness.{h,c}     ; st40_test.cpp
└── pipeline/                  # pipeline-layer suites (frame assembly)
    ├── st40p_harness.{h,c}    ; st40p_test.cpp
```

### Per-suite shape

```
  *_test.cpp ──► *_harness.h (opaque C API) ──► *_harness.c ──► production .c
   (gtest)        ut*_feed_pkt(...)              builds mbuf,    rx_*_handle_pkt()
                  ut*_stat_*(...)                calls handler   or rx_*_rtp_ready()
```

### Why C/C++ split

Internal MTL headers use `new` as a struct field name (a C++ reserved
keyword). The harness `.c` files include those internal headers and
expose an **opaque context** (`ut_test_ctx*`, `ut40p_ctx*`, …) through a
pure-C API in the matching `.h` file. The gtest `.cpp` files only
include the harness `.h` and never touch internal MTL headers.

## How linking works

The unit test executable depends on `libmtl.so` (`mtl_internal_dep` →
`shared_library('mtl', …)` in `lib/meson.build`).  Each harness `.c`
**`#include`s the production `.c` it wants to test**, e.g.
`st40p_harness.c` does:

```c
#undef MTL_HAS_USDT          /* skip dtrace probe semaphores */
#include "st2110/pipeline/st40_pipeline_rx.c"
```

This makes every `static` function in the production unit (the actual
target of the test) callable from the harness. Every non-`static`
symbol in that file is **redefined** in the test executable, colliding
with the same symbol exported by `libmtl.so`.

The collision is resolved by the link flag declared in `meson.build`:

```meson
unit_link_args = ['-Wl,--allow-multiple-definition']
```

With this flag the linker keeps the **first** definition it sees, which
is always the one from the harness object file (test sources are listed
before `libmtl.so` on the link line). So:

- Functions defined in the included production `.c` → harness's copy is
  used (identical code, just re-emitted).
- Functions called by that `.c` but defined elsewhere in libmtl (e.g.
  `mt_get_log_global_level`) → resolve to libmtl normally.
- Functions the harness explicitly stubs (see below) → harness's stub
  wins because it is defined in an object that links before libmtl.

This is the **only** linkage trick — there is no `LD_PRELOAD` of fakes,
no symbol versioning, no `dlsym`.

## How stubs work

A stub is just a non-static function with the same signature as a
libmtl symbol, defined in a harness `.c`. Because of
`--allow-multiple-definition`, the harness wins.

Stubs exist for one of three reasons:

1. **Hardware paths** — the symbol would touch a NIC, hugepage, or PTP
   clock. Example: `mt_mbuf_time_stamp()` in `st40p_harness.c` returns
   `0` because there is no real RX timestamp.
2. **DTrace/USDT** — `MTL_HAS_USDT` is `#undef`'d before including the
   production `.c`, so no probe semaphore symbols are referenced.
3. **Logger boilerplate** — `mt_get_log_*` are pulled in from libmtl
   unchanged; we don't stub them because they're cheap and harmless.

### What is **not** stubbed (intentional)

The `st40p_harness.c` provides a real `rte_spinlock_t` inside a mock
`st_rx_ancillary_sessions_mgr` so that the libmtl
`st40_rx_get_session_stats()` call path runs end-to-end against the
mocked transport handle. This means the pipeline overlay test
exercises the full `st40p_rx_get_session_stats()` →
`st40_rx_get_session_stats()` → `memcpy(&port_user_stats)` → atomic
overlay path, not just the pipeline half.

## How DPDK is initialized

`common/ut_common.c::ut_init()` runs `rte_eal_init()` once with
`--no-huge --no-pci --vdev=net_null0`. A single shared `rte_mempool`
backs all mbuf allocations, and a single `rte_ring` factory hands out
named rings to harnesses that need them. Test fixtures drain rings in
`TearDown()`, so tests are independent.

## Adding a new test

1. Decide the **layer** (session vs pipeline) and reuse an existing
   harness if possible.
2. Write the test as `EXPECT`/`ASSERT` over **invariants**:
   - "Counter X must equal counter Y after operation Z" — not "X must
     equal 7".
   - Add a `<<` message explaining what app behavior the assertion
     guards.
3. If you need a new accessor on the harness, add it to the `.h` and
   implement it in the `.c`. Keep accessors thin.
4. If the production code reaches a libmtl symbol that crashes (NULL
   deref, missing HW), add a stub in the harness `.c` and document why
   in the comment above it.
5. Build and run with ASan (`LD_PRELOAD=libasan`). Any new test must
   pass under ASan with no leaks reported on the test allocations.

## Adding a new harness

1. Create `tests/unit/<layer>/<suite>_harness.{h,c}` and
   `<suite>_test.cpp`.
2. In the harness `.c`:
   - `#undef MTL_HAS_USDT`
   - `#include "common/ut_common.h"`
   - `#include "<path/to/production>.c"`
   - Declare `struct <suite>_ctx { … }` and include the harness `.h`.
3. Stub only the symbols that the test would otherwise crash on; trust
   `--allow-multiple-definition` for the rest.
4. Add the three new files to `unit_sources` in
   `tests/unit/meson.build`.
