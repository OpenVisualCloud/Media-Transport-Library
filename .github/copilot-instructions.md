# Media Transport Library (MTL) - Copilot Instructions

## ⚠️ MANDATORY AGENT WORKFLOW — READ THIS FIRST

This file and `.github/copilot-docs/` are **persistent long-term memory** for agents. Follow these rules on every task:

### Before Starting
1. **Read this file fully** before writing or modifying any code.
2. **Read the relevant deep-dive files** based on your task area (see routing table below). When in doubt, read all of them.
3. **Do not guess** about patterns, conventions, or internal APIs — check these files first.

### While Working
4. **Fix doc inconsistencies on sight.** If anything in these files is outdated vs the actual codebase — fix it immediately.
5. **Save new knowledge.** Write non-obvious discoveries (invariants, gotchas, debugging techniques) into the correct copilot-docs file.
6. **Keep it compressed.** Bullet points over prose. Only store what saves a future agent significant time.
7. **Save conclusions, not descriptions.** Design decisions, non-obvious connections, invariants, gotchas, mental models — not struct fields or constants obvious from headers.

### File Routing Guide

All deep-dive files live in: `.github/copilot-docs/`

| Task area | File to read |
|-----------|-------------|
| Threads, schedulers, tasklets, lcore, sleep | `threading-and-scheduler.md` |
| malloc, free, mempools, frames, DMA, NUMA, ASAN | `memory-management.md` |
| Locks, mutexes, spinlocks, atomics, races | `concurrency-and-locking.md` |
| Pacing, PTP, TSC, rate limiter, latency | `pacing-timing-performance.md` |
| Session create/destroy, pipelines, TX/RX data flow, RTCP, ST2110-41 | `session-lifecycle-dataflow.md` |
| DPDK APIs, mbufs, queues, flow rules, device init, header-split | `dpdk-usage-patterns.md` |
| End-to-end flow, design patterns, naming, coding rules, config | `architecture-and-design-philosophy.md` |
| Tests, gtest, pytest, fuzz, RxTxApp, CI | `testing.md` |
| USDT tracepoints | `pacing-timing-performance.md` |

---

## Overview

MTL implements SMPTE ST 2110 for media transport over IP. DPDK-based with HW pacing (Intel E810). Supports ST2110-20 (video), ST2110-22 (compressed video), ST2110-30 (audio), ST2110-40 (ancillary), ST2110-41 (fast metadata).

**DeepWiki**: <https://deepwiki.com/OpenVisualCloud/Media-Transport-Library>

## Build & Format
```bash
./build.sh              # Release build
./build.sh debug        # Debug build (-O0 -g)
./build.sh debugonly    # Debug build without ASAN (faster than debug)
./format-coding.sh      # Format code (requires clang-format-14)
```

**Always** run `./build.sh` to verify compilation and `./format-coding.sh` to fix formatting before committing. CI rejects improperly formatted code.

## Commit Messages
Format: `<Type>: <description>` — Type is **capitalized**. See `doc/coding_standard.md`.

| Type | Use |
|------|-----|
| `Feat` / `Add` | New feature |
| `Fix` | Bugfix |
| `Refactor` | Code change (no bugfix/feature) |
| `Docs` | Documentation only |
| `Test` | Tests |
| `Perf` | Performance improvement |
| `Build` | Build system/dependencies |
| `Ci` | CI configuration |
| `Style` | Formatting (no code change) |

## Smoke Test (Run After Every Code Change)

After modifying code, **always** run these steps to verify nothing is broken. The entire sequence takes ~2 seconds on a no-op build.

### Prerequisites (one-time)
```bash
# Allocate hugepages (needed for MTL init, persists until reboot)
sudo sysctl vm.nr_hugepages=2048
```

### Step 1 — Build
```bash
ninja -C build && ninja -C build/tests
```
- No-op: ~0.03s. Full rebuild: ~60s.
- If build fails, fix the error before proceeding.

### Step 2 — Run gtest smoke via kernel:lo
```bash
sudo ./build/tests/KahawaiTest --p_port kernel:lo --r_port kernel:lo \
  --gtest_filter="Misc.version:Misc.memcpy:Cvt.simd_level:\
Cvt.rfc4175_422be10_to_yuv422p10le:Cvt.rfc4175_422be10_to_yuv422p10le_scalar:\
Main.start_stop_single:Main.bandwidth:Main.st20_frame_size:\
Main.fmt_equal_transport:Main.frame_api:Main.size_page_align:\
St20p.tx_create_free_single:St20p.rx_create_free_single:\
St22p.tx_create_free_single:St22p.rx_create_free_single:\
St30p.tx_create_free_single:St30p.rx_create_free_single" \
  --log_level error
```
- Runs 17 tests in ~1.3s. No NIC hardware required.
- Covers: version check, memcpy, SIMD pixel conversion, MTL init/start/stop lifecycle, bandwidth calc,
  frame size calc, format validation, page alignment, **pipeline session create/free for st20p, st22p, st30p (TX + RX)**.
- Expected output: `[  PASSED  ] 17 tests.`
- **Any failure = your change broke something. Fix it before committing.**

### What the tests cover

| Suite | Tests | What it validates |
|-------|-------|-------------------|
| Misc | version, memcpy | Library version API, optimized memcpy |
| Cvt | simd_level, 2× rfc4175→yuv422p10le | SIMD detection, pixel format conversion (SIMD + scalar) |
| Main | start_stop_single, bandwidth, st20_frame_size, fmt_equal_transport, frame_api, size_page_align | Full MTL init→start→stop→uninit lifecycle, ST2110-20 bandwidth/frame calculations, format helpers, page alignment |
| St20p | tx_create_free_single, rx_create_free_single | ST2110-20 pipeline session create→free (video TX + RX) |
| St22p | tx_create_free_single, rx_create_free_single | ST2110-22 pipeline session create→free (compressed video TX + RX) |
| St30p | tx_create_free_single, rx_create_free_single | ST2110-30 pipeline session create→free (audio TX + RX) |

### Troubleshooting
- **`hugepage_get failed`**: Run `sudo sysctl vm.nr_hugepages=2048`
- **`permission denied`**: Run with `sudo`
- **Build dir missing**: Run `./build.sh` once to create the meson build directory

### Step 3 (optional) — Targeted data-path tests

If your changes touch a specific media type, also run the matching digest test. These actually **transmit and receive frames** through the full pipeline over kernel:lo, verifying data integrity end-to-end.

| If you changed… | Run this test | Time |
|------------------|--------------|------|
| Video (st2110-20/22, pipeline, TX/RX video, pacing, converter) | `St20p.digest_1080p_s1` | ~11s |
| Audio (st2110-30, pipeline, TX/RX audio) | `St30p.digest_s3` | ~10s |
| Both / unsure | `St20p.digest_1080p_s1:St30p.digest_s3` | ~21s |

```bash
# Example: video change — run video digest
sudo ./build/tests/KahawaiTest --p_port kernel:lo --r_port kernel:lo \
  --gtest_filter="St20p.digest_1080p_s1" --log_level error

# Example: audio change — run audio digest
sudo ./build/tests/KahawaiTest --p_port kernel:lo --r_port kernel:lo \
  --gtest_filter="St30p.digest_s3" --log_level error
```
- These tests send real frames through the TX→RX pipeline and verify SHA digest matches.
- Expected output: `[  PASSED  ] 1 test.` (or 2 tests if running both)
- **Note**: Output to file recommended since these take >10s: append `> /tmp/digest_test.log 2>&1`
