# MTL Unit Tests

> **Per-function unit tests for the Media Transport Library — no libmtl link, no DPDK runtime, no hardware.**

---

## What this is

A collection of tiny `gtest` binaries — one per function (or small cluster
of related functions) — that exercise individual `static` / `static inline`
helpers from the st2110 stack in isolation.

Each module:

1. has a thin **wrapper TU** (`wrap_*.c`) that `#include`s the production
   `.c` file directly and exposes the function under test through an
   `extern "C"` bridge with primitive-typed arguments,
2. has a **`*_test.cpp`** that links against gtest and calls the bridge,
3. is linked with `-ffunction-sections -fdata-sections -Wl,--gc-sections`
   so every production function and global the bridge does not transitively
   reach is dropped at link time.

The result: a 5000-line production TU compiles into a binary of a few
hundred KB, with no `libmtl.so` and no DPDK runtime calls — even though
the production source `#include`s DPDK headers freely.

### Why a separate tier (vs. component tests)

| | `tests/unit/` | [`tests/component/`](../component/) |
|---|---|---|
| Links libmtl + DPDK | no | yes |
| Brings up EAL / hugepages | no | yes |
| Build/relink time | sub-second | many seconds |
| Failure points at | one function | "something in the pipeline" |
| Good for | pure helpers, table lookups, branch coverage | session state machines, end-to-end behaviour |

Use unit tests when the behaviour you want to lock in is a property of
*one* function and can be expressed in plain values. Use component tests
when the bug only appears after a sequence of session events.

## Layout

```
tests/unit/
├── meson.build              # central registry: every module is one entry
├── README.md                # this file
├── fff.h                    # vendored at configure (gitignored)
├── common/
│   ├── shim_mt_log.h        # turns dbg/info/err/warn into no-ops + neutralises USDT
│   └── fff_globals.cpp      # DEFINE_FFF_GLOBALS (kept around for future fakes)
└── <module>/
    ├── wrap_<file>.c        # #include "../../../lib/src/.../<file>.c" + bridge
    └── <module>_test.cpp    # gtest cases calling the bridge
```

There are **no per-subdir `meson.build` files**. To add a module, drop two
files under `tests/unit/<name>/` and add one entry to the `unit_modules`
dict in [`meson.build`](meson.build).

## Modules at a glance

| Module | Production fn(s) under test | What it pins |
|---|---|---|
| `tp_stat_aggregate` | `rv_tp_slot_parse_result` | running min/max aggregation invariants (currently pins two known A1 bugs) |
| `tp_calc_avg` | `rv_tp_calculate_avg` | float widening of large int sums |
| `detector_fps` | `rv_detector_calculate_fps` | cadence table + ±1-tick jitter tolerance + dual-delta cases (P59_94, P23_98) |
| `detector_dimension` | `rv_detector_calculate_dimension` | progressive vs. interlaced resolution tables |
| `detector_n_packet` | `rv_detector_calculate_n_packet` | success/mismatch branches |
| `detector_packing` | `rv_detector_calculate_packing` | priority order: BPM > single-line > GPM |
| `rfc4175_rtp_seq_id` | `rfc4175_rtp_seq_id` | 16-bit base/ext bit-pack, no-bleed on wrap |
| `tai_from_frame_count` | `tai_from_frame_count`, `transmission_start_time`, `tv_rl_bps` | nextafter precision near 2^53; rate-limit feed properties (linearity, ratio-equivalence, SD-only reactive correction) |
| `anc_pacing_time` | `tx_ancillary_pacing_time(_stamp)`, `tx_fastmetadata_pacing_time(_stamp)` | nextafter parity between ancillary and fast-metadata sessions |
| `transmitter_burst_fail` | `video_trs_burst_fail` | hang-detection escalation state machine (off-by-one threshold, observation-window reset) |
| `frame_payload_cross_page` | `tv_frame_payload_cross_page` | DMA page-crossing detection across IOVA-discontiguous page tables |

## Quick start

```bash
# Configure (downloads tests/unit/fff.h on first run via curl)
meson setup build_unit -Denable_unit_tests=true -Denable_component_tests=false

# Build + run everything
meson test -C build_unit

# Run a single module
meson test -C build_unit unit/detector_fps -v

# Or invoke the binary directly with gtest filters
./build_unit/tests/unit/unit_detector_fps --gtest_filter='DetectorFps.*'
```

> **Offline?** The configure step shells out to `curl` to fetch `fff.h`.
> Drop a copy at `tests/unit/fff.h` before configuring and the download
> is skipped. (FFF is not actually used by any current module — it is
> linked as a placeholder for future fake-needing modules.)

## How it works

```mermaid
flowchart LR
    subgraph "build-time TU"
      W["wrap_*.c<br>(C, extern \"C\" bridge)"] -->|#include| P["lib/src/.../prod.c<br>(unmodified)"]
    end
    T["module_test.cpp<br>(gtest, primitive args)"] --> W
    L["-Wl,--gc-sections"] -.->|"drops everything<br>not reached from bridge"| W
```

Three pieces collaborate:

1. **`-include shim_mt_log.h`** is forced on every C source. The shim
   defines the include-guards of `mt_log.h` and `mt_usdt.h` and provides
   no-op `dbg/info/warn/err/...` and `MT_USDT_*` macros, so the production
   `.c` compiles without trying to format log lines or fire USDT probes.
2. **`#include "<prod.c>"` from `wrap_*.c`** brings the production code
   into the test TU verbatim. All `static` helpers become reachable from
   the bridge functions defined right after the include.
3. **`-ffunction-sections -fdata-sections -Wl,--gc-sections`** tells the
   linker to discard every function and global not transitively referenced
   from `main()` (which gtest provides). Result: unreached helpers,
   stat structs, and the externs they would have called against libmtl
   are all dropped — the binary links without `libmtl.so`.

When this isn't enough (e.g. you need to intercept a function the bridge
*does* reach), use `#define foo my_foo` before the production `#include`,
or drop in an FFF fake — both `fff.h` and `DEFINE_FFF_GLOBALS` are wired
in already.

## How to add a module

1. Pick a target function. Anything reachable from one production `.c`,
   ideally with branches or arithmetic worth pinning. If a function is a
   pure 2-line wrapper, prefer testing its caller instead.
2. Create `tests/unit/<name>/wrap_<file>.c`:
   ```c
   #include <stdint.h>
   #include "../../../lib/src/<path>/<file>.c"

   /* Bridge: take primitive args, build the struct internally,
    * call the static helper, return primitives. */
   uint64_t test_<thing>(...) { ... }
   ```
3. Create `tests/unit/<name>/<name>_test.cpp` with gtest cases that call
   the bridge through an `extern "C"` block.
4. Add an entry to the `unit_modules` dict in
   [`meson.build`](meson.build):
   ```meson
   '<name>' : [
     '<name>/wrap_<file>.c',
     '<name>/<name>_test.cpp',
   ],
   ```
5. `meson compile -C build_unit && meson test -C build_unit unit/<name>`.

If the link fails with `undefined reference to <symbol>` it means the
bridge transitively reaches code that calls `<symbol>` and `--gc-sections`
could not drop it. Either narrow the bridge so the unreachable path is
gone, or `#define <symbol>` to a stub before the production `#include`.

## Conventions

- **Tests describe invariants, not bug IDs.** Name a test by the property
  it locks in (`Height576UsesPalRatioNotNtscRatio`), not by the ticket
  number.
- **Avoid magic-number oracles.** Prefer property tests
  (linearity, monotonicity, ratio-equivalence, branch selection) over
  hand-computed exact answers. See `tai_from_frame_count/` for an
  example: `tv_rl_bps` is covered by linearity + ratio-equivalence
  + branch-coverage tests, not by a single "expected = 331_776_000" line.
- **Keep the bridge dumb.** It should construct the production struct
  from primitives, call the helper, return primitives. No gtest, no
  C++ in the wrapper TU.
- **One `EXPECT_*` per behavioural claim** — when one fails, the name
  alone tells you which property regressed.
- **Tests intentionally pinned to known bugs are KEEP, not BROKEN.** When
  a test exists to fail until a fix lands, label it in a comment so the
  next reader doesn't "fix" the test instead of the production code
  (e.g. `tp_stat_aggregate`).

## What is *not* covered here

- Anything that needs real DPDK mbufs, rings, or the EAL → use
  [`tests/component/`](../component/).
- Session-accumulated state (slot machines, bitmap reconstruction,
  redundancy filter) → ditto.
- TX-side pacing accuracy, real-time behaviour, multi-process semantics
  → [`tests/validation/`](../validation/).

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `fatal error: fff.h: No such file` | Configure was offline and `curl` failed | Drop `fff.h` into `tests/unit/` and `meson setup --reconfigure build_unit` |
| `undefined reference to <symbol>` at link | Bridge transitively reaches code that calls `<symbol>`; `--gc-sections` could not drop it | Narrow the bridge, or `#define <symbol> my_stub` before the production `#include` |
| Test passes alone, fails in suite | A static global in the production TU carries state between tests | Reset the relevant struct in the bridge or in `SetUp()` |
| Configure re-downloads `fff.h` every time | Empty file from a failed download | `rm tests/unit/fff.h && meson setup --reconfigure build_unit` |
| `redefinition of 'foo'` when `#define`-stubbing | The production header was included before your `#define` | Move the `#define` after `mt_main.h` (or whichever header originally declared `foo`) and before the production `#include` |

## Contributing

- Run [`format-coding.sh`](../../format-coding.sh) before submitting.
- Commit messages follow the project standard: `Test: <imperative description>`.
- A new module should build and run in well under a second; if it
  doesn't, the bridge is reaching too much production code.
