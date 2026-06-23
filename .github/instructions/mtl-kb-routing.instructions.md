---
name: 'MTL Knowledge Base Reference'
description: 'Routes agents to the correct section of the MTL knowledge base based on the code being edited. Consult the linked section before making non-trivial changes.'
applyTo: 'lib/**,app/**,include/**,plugins/**,ecosystem/**,tests/**'
---

# MTL Knowledge Base Reference

The [MTL knowledge base](../copilot-docs/mtl-knowledge-base.md) is the comprehensive architecture and domain reference. Consult the relevant section before making non-trivial changes. Fix any inconsistencies you find.

## When to Read Which Section

| You are editing... | Read this §section |
|---|---|
| Design patterns, naming, end-to-end flow | [§1 Architecture & Design Philosophy](../copilot-docs/mtl-knowledge-base.md) |
| Scheduler, tasklet, lcore, thread code | [§2 Threading & Scheduler](../copilot-docs/mtl-knowledge-base.md) |
| Allocation, mempool, frame buffer, DMA code | [§3 Memory Management](../copilot-docs/mtl-knowledge-base.md) |
| Mutex, spinlock, atomic, session access code | [§4 Concurrency & Locking](../copilot-docs/mtl-knowledge-base.md) |
| Pacing, PTP, TSC, rate limiter, USDT | [§5 Pacing, Timing & Performance](../copilot-docs/mtl-knowledge-base.md) |
| Session create/destroy, pipeline, TX/RX data flow, RTCP | [§6 Session Lifecycle & Data Flow](../copilot-docs/mtl-knowledge-base.md) |
| DPDK queues, mbufs, flow rules, device init | [§7 DPDK Usage Patterns](../copilot-docs/mtl-knowledge-base.md) |
| Tests, gtest suites, fuzz targets, CI | [§8 Testing](../copilot-docs/mtl-knowledge-base.md) |

## Knowledge Base Workflow

- **Before non-trivial changes**: Read the relevant §section to understand existing patterns
- **Fix inconsistencies on sight**: If the KB is outdated vs actual code, update it immediately
- **Save new knowledge**: Write non-obvious discoveries into the KB under the appropriate §section
- **Keep it compressed**: Bullet points over prose. Tables over paragraphs. Only store what saves future agents significant time.
