# Archive: the design history of the unified API

| | |
|---|---|
| Status | Not maintained. Kept unchanged as the record behind the documents in [../](../README.md) |
| Date | moved here 2026-10-02 |
| Baseline | every `path:line` citation is pinned to `main` @ `545a266a` |

The maintained documents distil this material: [concepts](../concepts.md),
[contract](../contract.md), [timing](../timing.md), [engine](../engine.md),
[implementation plan](../implementation-plan.md), [migration](../migration.md),
[deployment](../deployment.md), [NMOS and IPMX](../nmos-ipmx.md) and
[decisions](../decisions.md), with [diagrams](../diagrams.md) and [examples](../examples.md).
Where they and a file here disagree, they and the headers in
[../sketch/](../sketch/README.md) are right. The documents 01–15 below use revision-3 API
names; [REVISION-4.md §6](REVISION-4.md#6-revision-3--revision-4-names) maps them to the
revision-4 headers.

## Design documents (revision 3, then revision 4 and its addenda)

| File | What it holds |
|---|---|
| [README-revision-4.md](README-revision-4.md) | the previous entry page and document map |
| [00-summary.md](00-summary.md) | the executive summary of revision 4 |
| [01-goals-and-requirements.md](01-goals-and-requirements.md) | goals, personas, requirement IDs (R-…), success criteria |
| [02-architecture.md](02-architecture.md) | layers, the pinned-core rules, the slot interface, the incremental alternative costed |
| [03-object-model-and-lifecycle.md](03-object-model-and-lifecycle.md) | objects, handles, session and lease state machines, destroy, the instance merge table |
| [04-threading-and-execution.md](04-threading-and-execution.md) | execution contexts, call classes, commands, queues, waking, progress, thread safety |
| [05-memory-and-buffers.md](05-memory-and-buffers.md) | regions, buffers, pools, data paths, holds, engine fixes M1–M10 |
| [06-timing-pacing-and-sync.md](06-timing-pacing-and-sync.md) | clocks, timelines, media and launch time, RTP, late policy, A/V sync, RX timing, E-changes |
| [07-completions-events-and-errors.md](07-completions-events-and-errors.md) | result records, events, error codes and reasons |
| [08-observability.md](08-observability.md) | stats schema and the revision-4 key catalogue (§R4) |
| [09-media-modes-and-backends.md](09-media-modes-and-backends.md) | units per essence, flows, legs, backends, capabilities, defaults |
| [10-api-sketch.md](10-api-sketch.md) | the revision-4 examples page as it was (now [../examples.md](../examples.md)) |
| [11-abi-compatibility-and-migration.md](11-abi-compatibility-and-migration.md) | ABI strategy, bindings, framework recipes, the legacy field map (§R4) |
| [12-familiarity-libfabric-and-rivermax.md](12-familiarity-libfabric-and-rivermax.md) | concept map for libfabric and Rivermax users |
| [13-guarantees-and-tests.md](13-guarantees-and-tests.md) | normative guarantees G-…, contract tests, fault matrix, performance budgets |
| [14-implementation-roadmap.md](14-implementation-roadmap.md) | phases 0–7, engines track, effort, staffing, risks |
| [15-security-and-deployment.md](15-security-and-deployment.md) | security surfaces, containers, hugepages, Windows, release policy |
| [16-kubernetes-and-crash-safety.md](16-kubernetes-and-crash-safety.md) | the Kubernetes addendum: shutdown, crash contract, health, CPUs, time, privileges, EK1–EK21 |
| [17-nmos-and-ipmx.md](17-nmos-and-ipmx.md) | the NMOS and IPMX addendum and every gap's disposition |
| [REVISION-4.md](REVISION-4.md) | what revision 4 changed and why, its decisions, the r3 → r4 name map, the addenda, typed configuration |
| [LIST-OF-CHANGES.md](LIST-OF-CHANGES.md) | every change against today's API, in bullets |
| [LEARN.md](LEARN.md) | the 30-minute tour of revision 4 |
| [DECISIONS.md](DECISIONS.md), [OPEN-QUESTIONS.md](OPEN-QUESTIONS.md) | the full decision log and all 170-odd questions with their proposed defaults |
| [side-findings.md](side-findings.md) | defects and documentation drift found in today's code (SF-, DD-) |
| [samples/](samples/README.md) | the examples as pictures |
| [r3-sketch/](r3-sketch/README.md) | the revision-3 single header and its examples |

## Research notes (today's code and prior art)

| File | Topic |
|---|---|
| [research/00-pr1610-design-review.md](research/00-pr1610-design-review.md) | the maintainer's review of PR #1610 |
| [research/01-pr1610-analysis.md](research/01-pr1610-analysis.md) | PR #1610 in full |
| [research/02-current-api-survey.md](research/02-current-api-survey.md) | today's public API and its flaws |
| [research/03-scheduler-threading.md](research/03-scheduler-threading.md) | schedulers, tasklets, threads |
| [research/04-memory-buffers.md](research/04-memory-buffers.md) | memory, buffers, DMA, external frames |
| [research/05-timing-pacing.md](research/05-timing-pacing.md) | timing, pacing, timestamps today |
| [research/06-observability.md](research/06-observability.md) | stats, events, errors today |
| [research/07-modes-matrix.md](research/07-modes-matrix.md) | every operating mode, media type and backend |
| [research/08-consumers-ecosystem.md](research/08-consumers-ecosystem.md) | how RxTxApp, plugins and bindings use the API |
| [research/09-libfabric.md](research/09-libfabric.md), [research/10-rivermax.md](research/10-rivermax.md), [research/11-media-io-prior-art.md](research/11-media-io-prior-art.md) | prior art |
| [research/12-st2110-timing-standards.md](research/12-st2110-timing-standards.md) | the ST 2110 timing model |
| [research/13-lifecycle-errors-abi.md](research/13-lifecycle-errors-abi.md) | lifecycle, recovery, errors, ABI, packaging |

## Studies and reviews

| Folder | Contents |
|---|---|
| [simplification/](simplification/) | S1–S9: coverage inventory, object model, configuration, data path, observability, a minimal alternative, samples friction, RTP passthrough, hiding the session headers; the revision-4 coverage check and header review |
| [kubernetes/](kubernetes/) | K1 the Kubernetes runtime, K2 an audit of today's code (hazards H-K-), K3 shutdown prior art |
| [interop/](interop/) | N1 NMOS requirements (N-REQ-), I1 IPMX requirements (I-REQ-) |
| [reviews/](reviews/) | C1–C4 (revision 1), the response to C5 (revision 2; review C5 itself is not in the repository, so links to it from this archive do not resolve), R2 verification, RK, RN, RA and their response, RV verification |
