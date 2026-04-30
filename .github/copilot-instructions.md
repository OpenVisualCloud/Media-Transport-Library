# Media Transport Library (MTL) - Copilot Instructions

## ⚠️ MANDATORY AGENT WORKFLOW — READ THIS FIRST

This file and `.github/copilot-docs/mtl-knowledge-base.md` are **persistent long-term memory** for agents.

### Before Starting
1. **Read this file fully** before writing or modifying any code.
2. **Read `.github/copilot-docs/mtl-knowledge-base.md`** — it is the single consolidated reference covering architecture, threading, memory, concurrency, pacing, sessions, DPDK patterns, and testing. Read the relevant §sections for your task.
3. **Do not guess** about patterns, conventions, or internal APIs — check the knowledge base first.

### While Working
4. **Fix doc inconsistencies on sight.** If anything is outdated vs the actual codebase — fix it immediately.
5. **Save new knowledge.** Write non-obvious discoveries into `mtl-knowledge-base.md` under the appropriate §section.
6. **Keep it compressed.** Bullet points over prose. Tables over paragraphs. Only store what saves a future agent significant time.
7. **Save conclusions, not descriptions.** Design decisions, invariants, gotchas — not struct fields obvious from headers.

### §Section Quick Reference

All knowledge lives in: `.github/copilot-docs/mtl-knowledge-base.md`

| Task area | §Section |
|-----------|----------|
| Design patterns, naming, coding rules, end-to-end flow | §1 Architecture & Design Philosophy |
| Threads, schedulers, tasklets, lcore, sleep | §2 Threading & Scheduler |
| malloc, free, mempools, frames, DMA, NUMA | §3 Memory Management |
| Locks, mutexes, spinlocks, atomics, races | §4 Concurrency & Locking |
| Pacing, PTP, TSC, rate limiter, VRX, USDT | §5 Pacing, Timing & Performance |
| Session create/destroy, pipelines, TX/RX data flow, RTCP, slots | §6 Session Lifecycle & Data Flow |
| DPDK APIs, mbufs, queues, flow rules, device init, header-split | §7 DPDK Usage Patterns |
| Tests, gtest, pytest, fuzz, RxTxApp, CI | §8 Testing |

---

## Overview

MTL implements SMPTE ST 2110 for media transport over IP. DPDK-based with HW pacing (Intel E810). Supports ST2110-20 (video), ST2110-22 (compressed video), ST2110-30 (audio), ST2110-40 (ancillary), ST2110-41 (fast metadata).

## Build & Format
```bash
./build.sh              # Release build
./build.sh debug        # Debug build (-O0 -g)
./build.sh debugonly    # Debug build without ASAN (faster than debug)
./format-coding.sh      # Format code (requires clang-format-14)
```

**Always** run `./build.sh` to verify compilation and `./format-coding.sh` to fix formatting before committing. CI rejects improperly formatted code.
