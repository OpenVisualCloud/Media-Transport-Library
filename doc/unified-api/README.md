# Unified MTL API

| | |
|---|---|
| Status | Design baseline, 2026-10-02. Nothing is implemented. Seventeen maintainer decisions are open ([decisions.md](decisions.md)) |
| Normative | the headers in [sketch/include/mtl/experimental/](sketch/include/mtl/experimental/); where a document and a header disagree, the header is right |
| Baseline | `main` @ `545a266a` |
| Goal of this branch | implement the API for ST 2110-20 first, move RxTxApp and the gtests onto it, and run the nightly with it ([implementation-plan.md](implementation-plan.md)) |

This directory designs a new public API for the Media Transport Library. One session type
serves every ST 2110 essence: video, compressed video, audio, ANC, fast metadata and generic
RTP. One unit struct moves frames, rows or packets in and out. Every submitted unit gets
exactly one result. Media time drives RTP exactly, and A/V/ANC stay in sync by construction.
No application code runs on the pinned cores, and the API is safe to run in a Kubernetes pod.
It is built over today's engines, which are fixed first so that legacy users benefit too.

A program includes one header, `mtl.h` (32 functions). Sixteen optional headers add one job
each, for 125 functions in all (and 14 reserved for later phases). Configuration is typed
fields only, with enums that follow the legacy ones, so porting an `ops` struct is a
field-by-field copy.

## The documents

| Read | When you want to | Contents |
|---|---|---|
| [concepts.md](concepts.md) | learn the API in one sitting | the model, the first sender and receiver, the life of a unit, results and errors, timing and memory in one page each, a glossary |
| [examples.md](examples.md) | see real code | every example of `sketch/examples/`, copied verbatim and compiled by `check.sh`, plus the header map |
| [diagrams.md](diagrams.md) | see it drawn | a picture per example, and the architecture, lifecycle, data path, waking, memory, timing, results, shutdown and NMOS pictures, with an index of every diagram in the set |
| [contract.md](contract.md) | know exactly what a call does | the normative behaviour by area: rules R1–R8, instance, lifecycle and states, data path, results, waiting, errors, memory, events, stats, options, packet units, shutdown and health |
| [timing.md](timing.md) | get time right | clocks and time sources, timelines, media time and RTP, launch time and pacing, source kinds, late policy, A/V/ANC sync, RX timing |
| [engine.md](engine.md) | build it | how the API sits on today's engines in `lib/`: layers, the slot interface, engine hooks, threading internals, the engine changes, known defects with `path:line` |
| [implementation-plan.md](implementation-plan.md) | plan or review the work | ST20 first, then RxTxApp, the gtests and the nightly; milestones, tests, risks, effort |
| [migration.md](migration.md) | port an application or a plugin | the call map, the field map of every legacy `ops` struct, enum tables, coexistence, hiding the legacy headers |
| [deployment.md](deployment.md) | run it | security surfaces, containers, Kubernetes pods, crash safety, probes |
| [nmos-ipmx.md](nmos-ipmx.md) | plan NMOS and IPMX support (Phase 7, later) | the IS-05 contract, SDP, RTCP sender reports, timing without PTP, encryption |
| [decisions.md](decisions.md) | decide, or check what was decided | the open maintainer decisions M1–M17, every decision D-01…D-98 and the open issues OI-n |
| [presentation/slides.md](presentation/slides.md) | present it | a slide deck (Marp) |
| [questions.md](questions.md) | see why a default was chosen | every question the design raised (Q-*), with its options, the default or answer, its status and where it is applied |
| [requirements.md](requirements.md) | check what must hold and where it is met | goals and non-goals, every requirement catalogue (R-*, K-REQ, N-REQ, I-REQ, R-PKT) and the guarantees that test them |
| [coverage.md](coverage.md) | check that nothing of today's API is lost | every legacy capability (U-001…U-418) with its unified home and phase, the revision-4 coverage check, hiding the session headers, sample friction |
| [research.md](research.md) | know how today's library behaves | verified facts about `lib/` and its users with `path:line`, the consumer survey, PR #1610 |
| [prior-art.md](prior-art.md) | see what the outside world does | ST 2110 and AES67 timing facts, libfabric, Rivermax, media-I/O APIs, Kubernetes and shutdown prior art, the bibliography |
| [history.md](history.md) | trace how the design got here | revisions r1–r4, rejected alternatives, the simplification studies, every review finding and its outcome |

The design history before this distillation is in git at commit 15a27bb6
(`git show 15a27bb6:doc/unified-api/archive/README.md`).

## Reading the IDs

| ID | Means | Defined in |
|---|---|---|
| R1–R8 | the rules every call follows | `mtl.h`, [contract.md](contract.md) §1 |
| M1–M17 | maintainer decisions | [decisions.md](decisions.md) |
| milestone M0–M6 | the steps of this branch (written "milestone Mn"; implementation-plan.md writes them bare and says so) | [implementation-plan.md](implementation-plan.md) |
| D-01…D-98 | decisions, proposed or awaiting a maintainer decision | [decisions.md](decisions.md) |
| OI-n | open issues found while distilling, for the maintainer | [decisions.md](decisions.md) |
| Q-xxx-n | the research questions behind a decision | [questions.md](questions.md) |
| G-01…G-112, G-PKT-n | guarantees, each with its test | [implementation-plan.md](implementation-plan.md) §8, [requirements.md](requirements.md) §4 |
| R-xxx-n, R-PKT-n, NG1… | requirements and non-goals | [implementation-plan.md](implementation-plan.md) §8.0, [requirements.md](requirements.md) §2–§3, [concepts.md](concepts.md) §1.4 |
| E1–E13, R1, R2, MF1–MF10 (memory), EK1–EK21 | engine changes | [engine.md](engine.md) |
| SF-n, SP-n, DD-n, H-K-n | defects and drift in today's code; Kubernetes hazards | [engine.md](engine.md) |
| S0–S8 (spikes) | measurements before code | [implementation-plan.md](implementation-plan.md) §3.3 |
| H1–H10, W0–W3, PE1–PE9 | pinned-core rules, wake-up mechanisms, packet-engine items | [engine.md](engine.md) |
| P1–P9, GO-1…GO-10 | personas and goals | [concepts.md](concepts.md), [requirements.md](requirements.md) §2.1 |
| S1–S9 (studies), K1–K3, N1, I1 | studies | [history.md](history.md) §5 (S2–S6), [coverage.md](coverage.md) (S1, S7, S9), [contract.md](contract.md) §13 (S8), [research.md](research.md) and [prior-art.md](prior-art.md) (K1–K3), [requirements.md](requirements.md) §5–§7 (K1, N1, I1) |
| C1–C5, R2, RK-n, RN-n, RA-n, RV-n | reviews and every finding with its outcome | [history.md](history.md) §6 |
| U-001…U-418, F-01…F-26, H-01…H-19, CUT-1…CUT-8 | legacy capabilities, sample friction, pre-hide gaps, proposed cuts | [coverage.md](coverage.md) |
| K-REQ-n, N-REQ-n, I-REQ-n, G-Nn, GI-n | Kubernetes, NMOS and IPMX requirements and gaps | [requirements.md](requirements.md) §5–§7, [deployment.md](deployment.md), [nmos-ipmx.md](nmos-ipmx.md) |
| Phase 0…7 | the roadmap; Phase 7 (NMOS extras, SDP, IPMX, PEP) is later | [implementation-plan.md](implementation-plan.md) §2, §6 |

## The sketch

`sketch/` holds the normative headers and the examples, which compile. Run the check after
any change:

```bash
doc/unified-api/sketch/check.sh       # compiles every header and example, C99 and C++17, gcc and clang
python3 doc/unified-api/sketch/gen_api_doc.py   # regenerates examples.md from sketch/examples.md.in
```

[sketch/README.md](sketch/README.md) says what the check covers. A change to the design that
adds or renames a symbol is complete only when it is in a header and `check.sh` passes.
Functions of later phases, and the types only they use, sit under `MTL_LATER`; the values,
flags, options, events and fields the other headers carry are declared now, so the ABI freeze
does not block them.

## How this was made

The design went through four revisions, a simplification pass and two addenda, with 13
research notes on today's code and on prior art (libfabric, Rivermax, DeckLink, DPDK, VFIO,
GStreamer, FFmpeg), the SMPTE, AMWA and VSF specifications, and twelve reviews. That material
was then folded into the documents above: the normative behaviour into contract.md, timing.md
and engine.md, and everything else, every question, requirement, legacy capability, code fact,
outside fact and review finding, into the six reference files (questions.md to history.md),
compressed, with its `[verified] path:line` citations pinned to `545a266a`; nothing needs the
older text any more.
