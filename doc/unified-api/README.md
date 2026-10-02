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
| [decisions.md](decisions.md) | decide, or check what was decided | the open maintainer decisions M1–M17 and every decision D-01…D-98 |
| [presentation/slides.md](presentation/slides.md) | present it | a slide deck (Marp) |
| [archive/](archive/README.md) | trace a rule to its research | every earlier document, study and review, unchanged |

## Reading the IDs

| ID | Means | Defined in |
|---|---|---|
| R1–R8 | the rules every call follows | `mtl.h`, [contract.md](contract.md) §1 |
| M1–M17 | maintainer decisions | [decisions.md](decisions.md) |
| milestone M0–M6 | the steps of this branch (written "milestone Mn"; implementation-plan.md writes them bare and says so) | [implementation-plan.md](implementation-plan.md) |
| D-01…D-98 | decisions, proposed or awaiting a maintainer decision | [decisions.md](decisions.md) |
| OI-n | open issues found while distilling, for the maintainer | [decisions.md](decisions.md) |
| Q-xxx-n | the research questions behind a decision | [archive/OPEN-QUESTIONS.md](archive/OPEN-QUESTIONS.md) |
| G-01…G-112 | guarantees, each with its test | [implementation-plan.md](implementation-plan.md) §8, [archive/13](archive/13-guarantees-and-tests.md) |
| R-xxx-n, NG1… | requirements and non-goals | [concepts.md](concepts.md), [archive/01](archive/01-goals-and-requirements.md) |
| E1–E13, R1, R2, MF1–MF10 (memory), EK1–EK21 | engine changes | [engine.md](engine.md) |
| SF-n, SP-n, DD-n, H-K-n | defects and drift in today's code; Kubernetes hazards | [engine.md](engine.md) |
| S0–S8 (spikes) | measurements before code | [implementation-plan.md](implementation-plan.md) §3.3 |
| H1–H10, W0–W3, PE1–PE9 | pinned-core rules, wake-up mechanisms, packet-engine items | [engine.md](engine.md) |
| P1–P9, GO-1…GO-9 | personas and goals | [concepts.md](concepts.md) |
| S1–S9 (studies), K1–K3, N1, I1, C1–C5, RK, RN, RA, RV | studies and reviews | [archive/](archive/README.md) |
| K-REQ-n, N-REQ-n, I-REQ-n, G-Nn, GI-n | Kubernetes, NMOS and IPMX requirements and gaps | [deployment.md](deployment.md), [nmos-ipmx.md](nmos-ipmx.md) |
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
GStreamer, FFmpeg), the SMPTE, AMWA and VSF specifications, and twelve reviews. The
documents above distil that material; [archive/](archive/README.md) keeps all of it, with its
`[verified] path:line` citations pinned to `545a266a`.
