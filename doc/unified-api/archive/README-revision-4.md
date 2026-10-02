# Unified MTL session API — plan, design and research

| | |
|---|---|
| Status | Draft for maintainer review — revision 4 (reviews C1–C5, then a simplification pass), with addenda for Kubernetes and for NMOS/IPMX. Nothing here is implemented or decided |
| Date | 2026-10-01 |
| Baseline | `main` @ `545a266a`; PR #1610 @ `14a1f80c`; PR #1770 (open) @ `74b9991d` |
| Supersedes | nothing yet; intended to replace PR #1610's `doc/new_API/` once decisions are made |

This directory plans a new public API for the Media Transport Library. It keeps PR #1610's
idea — one session for every ST 2110 essence — and redesigns the contract for buffers,
completion, timing, threading, lifecycle and ABI. It is based on the maintainer's review
of PR #1610, on 13 research notes, and on five adversarial reviews.

**Revision 4** is a simplification pass ([REVISION-4.md](REVISION-4.md)): the include file a
program needs is `mtl.h` with 32 functions, optional headers add one job each, one typed config
and one unit struct serve every essence, rare knobs are named options, RTP passthrough is a unit
kind of the high-level API, and the legacy session headers get a plan to become non-public
without losing any capability. Nine studies and two reviews behind it are in
[simplification/](simplification/); the revision-3 header is archived in
[archive/r3-sketch/](r3-sketch/).

**Addenda to revision 4** (2026-10-01). [16](16-kubernetes-and-crash-safety.md) makes the
library safe in a Kubernetes pod: a bounded, network-first shutdown, crash-only rules,
health for probes, CPUs and time from the pod. [17](17-nmos-and-ipmx.md) makes the API fit
NMOS and IPMX: an IS-05 activation contract, SDP, RTCP sender reports, timing without PTP
and payload encryption. Both rest on studies ([kubernetes/](kubernetes/), [interop/](interop/))
and three reviews ([response](reviews/RKNA-response.md)). `mtl.h` has 32 functions; 16
optional headers bring the total to 125, plus 14 reserved for later phases. Configuration is
typed only, with enums that follow the legacy ones, and NMOS extras, SDP, IPMX and PEP are a
later phase: today's functionality is ported first ([REVISION-4 §9](REVISION-4.md)).

**Revision 3** answered review C5. The biggest changes: the header is now a real file set
that compiles (`sketch/`); the RX half has a timing model and engine hooks; the plan
starts with engine fixes that reach legacy users and a Phase 0.5 that ships the A/V answer
on the legacy API; a null backend, a test clock and fault injection ship with Phase 1;
the operator surface (atomic flow activation, leg admin state, names, enumeration,
capacity) is designed; security and deployment have their own document; and the decision
package is down to ten items. Cross-cutting names and rules are in
[reviews/C5-response.md](reviews/C5-response.md) Part A.

## Start here

| If you want to … | Read |
|---|---|
| see what revision 4 changed and why | [REVISION-4.md](REVISION-4.md) |
| see the proposal in pictures | [samples/diagrams.md](samples/diagrams.md) (a picture per example, plus lifecycle, timing and errors) and [samples/README.md](samples/README.md) |
| know every change in bullet points | [LIST-OF-CHANGES.md](LIST-OF-CHANGES.md) |
| learn it in 30 minutes | [LEARN.md](LEARN.md) |
| present it to others | [presentation/slides.md](../presentation/slides.md) (Marp; renders as Markdown on GitHub) |
| decide | [00-summary.md](00-summary.md), then [OPEN-QUESTIONS.md](OPEN-QUESTIONS.md) §The decisions (M1–M17) |
| read the API | [10-api-sketch.md](10-api-sketch.md) and the headers in [sketch/](../sketch/README.md) |
| see how review C5 was answered | [reviews/C5-response.md](reviews/C5-response.md) |

## How to read it

1. [00-summary.md](00-summary.md) — the whole proposal on two pages, and the seventeen
   decisions.
2. [OPEN-QUESTIONS.md](OPEN-QUESTIONS.md) — the decisions with options and a line for your
   answer; every other question has a proposed default you can object to.
3. The design documents in order, or only the ones you care about:

| # | Document | What it decides |
|---|---|---|
| 01 | [goals-and-requirements](01-goals-and-requirements.md) | why (and the surface counted honestly), personas, goals, non-goals, requirement IDs (R-…), success criteria in user terms |
| 02 | [architecture](02-architecture.md) | layers, the pinned-core rules, data and control flow, the slot interface, the incremental alternative costed |
| 03 | [object-model-and-lifecycle](03-object-model-and-lifecycle.md) | objects, handles and leases, session and lease state machines, groups, destroy, the instance and its merge table |
| 04 | [threading-and-execution](04-threading-and-execution.md) | execution contexts, call classes, commands, queues, waking, progress, thread-safety table |
| 05 | [memory-and-buffers](05-memory-and-buffers.md) | regions, buffers, pools, data paths, holds, when a buffer is reusable |
| 06 | [timing-pacing-and-sync](06-timing-pacing-and-sync.md) | clocks, timelines, media vs launch time, RTP, late policy, A/V sync, progressive, RX timing |
| 07 | [completions-events-and-errors](07-completions-events-and-errors.md) | CQ and EQ, result records, capacity by construction, error codes and reasons |
| 08 | [observability](08-observability.md) | stats schema, gauges, histograms, status and info getters, names, enumeration, capacity, tracing, logging |
| 09 | [media-modes-and-backends](09-media-modes-and-backends.md) | per-essence units, flows and `update_flows`, feature coverage, backends (incl. null), capability negotiation, the defaults table |
| 10 | [api-sketch](10-api-sketch.md) | the header set in `sketch/` described, and worked examples (incl. the A/V-from-one-file case) |
| 11 | [abi-compatibility-and-migration](11-abi-compatibility-and-migration.md) | ABI strategy, where a knob lives, bindings, coexistence, framework recipes, PR #1610's fate |
| 12 | [familiarity-libfabric-and-rivermax](12-familiarity-libfabric-and-rivermax.md) | concept map for libfabric and Rivermax users |
| 13 | [guarantees-and-tests](13-guarantees-and-tests.md) | normative guarantees (G-…) marked P or BE, their contract tests, the fault matrix, performance budgets, the test substrate |
| 14 | [implementation-roadmap](14-implementation-roadmap.md) | Phases 0, 0.5, engines first, 1–6; spikes; effort and staffing; success criteria; deprecation policy; risks |
| 15 | [security-and-deployment](15-security-and-deployment.md) | imported memory and the IOMMU, wait objects, the waker, handles, debug-API gating, MtlManager trust, containers, hugepages, Windows |
| 16 | [kubernetes-and-crash-safety](16-kubernetes-and-crash-safety.md) | bounded network-first shutdown, what holds after SIGKILL, device removal, CPUs, time in a pod, fail-fast open, health and probes, privileges and a pod spec, engine fixes EK1–EK21 |
| 17 | [nmos-and-ipmx](17-nmos-and-ipmx.md) | IS-04 values, the IS-05 activation contract, SDP, BCP-008, the IPMX profile, RTCP sender reports, timing without PTP, PEP encryption, every gap's disposition |
| — | [REVISION-4.md](REVISION-4.md) | what revision 4 changed, its decisions D-71…D-88, and the revision-3 → revision-4 name map for documents 01–15 |
| — | [sketch/](../sketch/) | the normative header set (`include/mtl/experimental/mtl.h` and 16 optional headers), 13 examples in C99 and one in C++17, and `check.sh`, which compiles them; if prose and a header disagree, the header wins |
| — | [kubernetes/](kubernetes/), [interop/](interop/) | the studies behind 16 and 17: K1 runtime facts, K2 code audit, K3 shutdown prior art; N1 NMOS and I1 IPMX requirements |
| — | [simplification/](simplification/) | the revision-4 studies S1–S9 (coverage inventory, object model, configuration, data path, observability, a minimal alternative, samples audit, RTP passthrough, hiding the session headers) and the reviews of the result |
| — | [DECISIONS.md](DECISIONS.md) | decision log (proposed defaults and the maintainer's answers) |
| — | [side-findings.md](side-findings.md) | bugs and documentation drift found on the way, each with its status against open PR #1770 |

## Design reviews

The first draft was reviewed by four independent critics; revision 2 applied their
findings. A fifth, adversarial user and feasibility review drives revision 3 (summary in
[00 §What the reviews changed](00-summary.md)).

| Review | Lens |
|---|---|
| [C1](reviews/C1-realtime-feasibility.md) | real-time and implementation feasibility on today's engines |
| [C2](reviews/C2-timing-standards.md) | timing, pacing and standards correctness (numbers recomputed exactly) |
| [C3](reviews/C3-usability-personas.md) | developer experience, role-playing every persona |
| [C4](reviews/C4-consistency-audit.md) | cross-document consistency, traceability, research and mode coverage, citations |
| [C5](reviews/C5-adversarial-user-review.md) | adversarial users (sample author, plugin maintainer, operator, validation), header as compiled, plan size and sequencing |
| [C5 response](reviews/C5-response.md) | the designer's verdict on every C5 finding and the cross-cutting resolutions of revision 3 |
| [RK](reviews/RK-kubernetes-review.md), [RN](reviews/RN-nmos-ipmx-review.md), [RA](reviews/RA-api-coherence-review.md), [response](reviews/RKNA-response.md) | the addenda: Kubernetes safety, NMOS/IPMX suitability, API coherence and leanness |
| [R2 verification](reviews/R2-verification.md), [citation re-pin log](reviews/R2-citation-repin-log.md) | how revision 2 was checked |

## Research notes

Each note labels its claims **[verified]** (read in code or primary source, with
`path:line` or a URL), **[inferred]**, or **[unknown]**, and ends with its own open
questions (merged into [OPEN-QUESTIONS.md](OPEN-QUESTIONS.md)). The design documents cite
them as `[Rnn §x]`.

| Note | Topic |
|---|---|
| [R00](research/00-pr1610-design-review.md) | the maintainer's consolidated review of PR #1610 (input, unchanged) |
| [R01](research/01-pr1610-analysis.md) | PR #1610 read commit by commit; review claims verified; rebase assessment |
| [R02](research/02-current-api-survey.md) | today's public API: inventory, patterns, flaws |
| [R03](research/03-scheduler-threading.md) | scheduler, tasklets, execution contexts, locks, wake-ups |
| [R04](research/04-memory-buffers.md) | memory modes, frame lifecycles, copies, DMA mapping, GPU |
| [R05](research/05-timing-pacing.md) | pacing modes, timestamps, epochs, lateness, RTP derivation, A/V today |
| [R06](research/06-observability.md) | stats, events, errors, diagnostics today |
| [R07](research/07-modes-matrix.md) | every media type, mode and backend |
| [R08](research/08-consumers-ecosystem.md) | how FFmpeg, GStreamer, OBS, bindings, samples and external users use the API |
| [R09](research/09-libfabric.md) | libfabric in depth and its mapping to MTL |
| [R10](research/10-rivermax.md) | NVIDIA Rivermax in depth and its mapping to MTL |
| [R11](research/11-media-io-prior-art.md) | DeckLink, AJA, GStreamer, io_uring, Vulkan, audio APIs, DPDK, NDI, SRT, WebRTC |
| [R12](research/12-st2110-timing-standards.md) | the ST 2110 / ST 2059 / ST 2022-7 timing model from the primary texts |
| [R13](research/13-lifecycle-errors-abi.md) | lifecycle, recovery, error model, ABI, build and bindings constraints |

## Conventions

- IDs: goals `GO-n` and requirements `R-AREA-n` (01), guarantees `G-nn` (13), decisions
  `D-nn` (DECISIONS), questions `Q-AREA-n` (OPEN-QUESTIONS), side findings
  `SF/SP/DD/SC-nn`, review findings `C1 #n` / `C2 #n` / `C3 Px-n` / `C4 C-nn` / `C5 §x.y`.
  IDs are stable: retired items keep their number.
- Revision markers: **r2**, **r3** and **r4** flag text changed in that revision. Documents
  01–15 carry revision-3 API names under a revision-4 banner; [REVISION-4.md §6](REVISION-4.md#6-revision-3--revision-4-names)
  maps them.
- Code names in the design (`mtl_tx_submit`, `struct mtl_time`, …) are proposals, not an
  API; the header set in [sketch/](../sketch/) collects them and is normative for names and
  shapes, and [10-api-sketch.md](10-api-sketch.md) describes it.
- Citations of MTL code are `path:line` at `545a266a` unless stated otherwise; `git show
  545a266a:<path>` must show the cited text there.
- `doc/user-pacing-timestamp-contract.md` ("the contract") is an untracked working draft,
  not part of `545a266a`.
