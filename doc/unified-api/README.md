# Unified MTL API

| | |
|---|---|
| Status | Design complete; implementation starts with milestone MS1 ([implementation-plan.md](implementation-plan.md)). Nothing is implemented yet |
| Normative | the headers in [sketch/include/mtl/experimental/](sketch/include/mtl/experimental/) (they move to `include/mtl/experimental/` in task H1b of MS1); where a document and a header disagree, the header is right |
| Baseline | `main` @ `545a266a`; every `path:line` in these documents is pinned there |

The unified API is the one public API MTL converges on. One session type serves every ST 2110
essence: video, compressed video, audio, ANC, fast metadata and generic RTP. One unit struct
moves frames, rows (slice mode) or packets in and out. Every submitted unit gets exactly one
result. Media time drives RTP exactly, so audio, video and ANC stay in sync. No application code
runs on the pinned cores, and the library is safe to run in a Kubernetes pod. Today's session and
pipeline headers keep working during the transition and are hidden at the end (MS7).

Inside, one **core** (a slot table with its states, the order of results, waiting) sits under
both the new API and the legacy pipeline calls, and one **binding** per essence connects it to
today's engines through the callbacks they already make. A program includes `mtl.h`; thirteen
optional headers add one job each. Verbs that several objects share (`mtl_close`,
`mtl_interrupt`, `mtl_wait`, `mtl_reap`, `mtl_release`, …) are one exported function each, with
typed inline wrappers. Configuration is typed fields, with enums that follow the legacy ones.

## 1. For the implementing session

The work is done by a main session that plans and integrates, and by the repository's subagents:
`mtl-developer` writes code and tests for one task (Gates 0–4), `mtl-reviewer` reviews every saved
diff (Gate 5), `mtl-system-admin` runs KahawaiTest on VFs (Gate 6); pytest runs from the main
session. Their rules are in the repository's `CLAUDE.md` and `.github/claude/agents/`.

**Read first, in this order** (about 27 k tokens in all, at 4 bytes a token):

1. this README §1 and [ms1-status.md](ms1-status.md) (where the work stands);
2. [implementation-plan.md](implementation-plan.md) §1.1, §1.2, §1.3, §2.1, §2.2, §3, §4, §5.1,
   §5.2, §5.6, §5.8: the assumptions A1–A10, milestones, non-goals, the architecture,
   prerequisites, where the code goes, and MS1 with its tasks, the reading list per task, the
   gates, the exit criteria and how the work runs;
3. [engine.md](engine.md) §1, §2.2, §2.4, §3: the layers, the pinned-core rules, the core, the
   bindings and the slot table;
4. [`mtl.h`](sketch/include/mtl/experimental/mtl.h) lines 1–122: the top comment with the rules
   R1–R8 every call follows.

engine.md §4 (where each MS1 task starts in `lib/`), the plan's scope map (§2.3), test pool
(§5.3), RxTxApp port (§5.4), smoke set (§5.5) and risks (§5.7), and the rest of the headers are
read per task.

Then load the rest **by section, per task**: implementation-plan.md §5.2 says what each task's
agent reads. Do not load whole documents into one context; the large ones are references.

**Rules that hold for every task:**

- The header wins. A change that adds or renames a public symbol is complete only when the header
  has it and `doc/unified-api/sketch/check.sh` (later the same check over `include/`) passes;
  `python3 doc/unified-api/sketch/gen_api_doc.py` regenerates [examples.md](examples.md).
- The repository's two-world rule and tasklet rules apply to the core and the bindings: a binding
  runs on a tasklet under the session spinlock and is wait-free (engine.md §1, §2).
- The legacy gate (implementation-plan.md §8.5) passes at every milestone: legacy users see only
  bugfixes.
- The acceptance engine parses log lines that libmtl and RxTxApp print; the unified path prints
  them byte-identical (D-110).
- `tests/integration_tests/` (`KahawaiTest`) and `tests/tools/RxTxApp/` are frozen: new-API tests
  go into their copies, `UnifiedKahawaiTest` and `UnifiedRxTxApp`, and both stacks run until the
  legacy API is removed; then the pipeline-level cases and the legacy RxTxApp are deleted, and the
  session-level cases stay as the engine's internal test (implementation-plan.md §4.1).
- Commits stay under about 1.5 k changed lines (mechanical copies and deletions exempt), tests
  first, each through `mtl-reviewer`; the maintainer reviews at most four a week, so ask at the
  checkpoints of §5.2 and not between.
- `standards/` at the repository root holds the maintainer's copies of the SMPTE standards: read
  them to check a rule, cite clauses in the documents, never commit or quote them (they are
  copyrighted); [standards.md](standards.md) is the place for the facts the code must meet.
- Keep these documents true. When code settles an open issue ([decisions.md](decisions.md) §5) or
  contradicts a statement, fix the document in the same commit; a new design choice gets a D-row
  after the maintainer agrees (implementation-plan.md §5.8).

**Starting MS1:**

1. The doc set is committed on `unified-api-design` (the maintainer pushes it). `standards/` is in
   `.git/info/exclude` on the maintainer's machine; keep it out of every commit. Create the work
   branch `unified-api-ms1` from that commit and record it in [ms1-status.md](ms1-status.md).
2. Then, in calendar order: P0 (the tooling the gates need), S0 (baseline measurement), T1
   (pinning today's st20p behaviour with tests), H1b (the headers, the API shell inside libmtl and
   the run options) and E1 (the MS1 engine fixes), C0 (the core's internal header, the
   maintainer's first checkpoint).

The order, the gates and how the work runs are in implementation-plan.md §5.2 and §5.8; the state
of every task and the standing instructions are in [ms1-status.md](ms1-status.md).

## 2. The documents

| Document | For | Contents |
|---|---|---|
| [concepts.md](concepts.md) | users, everyone | the model, the first sender and receiver, the life of a unit, results and errors, timing and memory in one page each, a glossary, FAQ |
| [examples.md](examples.md) | users | every example of `sketch/examples/`, copied verbatim and compiled, and the header map |
| [implementation-plan.md](implementation-plan.md) | implementers | milestones MS1–MS7, MS1 tasks with reading lists, tests, guarantees G-xx, risks, calendar |
| [engine.md](engine.md) | implementers | the core and the bindings, the pinned-core rules, where each part lives in `lib/`, the engine change list, known defects with `path:line` |
| [contract.md](contract.md) | implementers, users | the normative behaviour of every call: rules R1–R8, instance, lifecycle, data path, results, waiting, errors, memory, events, stats, options, packet units, legs |
| [timing.md](timing.md) | implementers, users | clocks, media time and RTP, launch and pacing, late policy, A/V/ANC sync, RX timing, timing tests |
| [migration.md](migration.md) | users porting; the legacy wrappers | call map, field maps of every legacy `ops` struct, enum tables, coexistence, the library and ABI plan, hiding the legacy headers, plugins, bindings, per-consumer notes |
| [coverage.md](coverage.md) | implementers, maintainers | every legacy capability (U-001…U-420) with its unified home and milestone; the cut list; what blocks hiding the session headers |
| [legacy-internals.md](legacy-internals.md) | implementers | verified facts about today's `lib/` and its consumers: modes, lifecycle, threads and locks, memory, pacing and timestamps, stats, PR #1610: its pitfalls and what each MS1 task can reuse |
| [standards.md](standards.md) | implementers, testers | what ST 2110, ST 2022, AES67, VSF TR-10 and AMWA NMOS require, where MTL disagrees today, an arithmetic reference, the bibliography |
| [requirements.md](requirements.md) | maintainers | goals, non-goals, personas, the requirement catalogues (R-*, R-PKT, K-REQ, N-REQ, I-REQ) and the guarantee texts of later milestones |
| [decisions.md](decisions.md) | maintainers | one line of rationale per design decision D-xx, the defaults for ST20, the open implementation issues OI-n |
| [deployment.md](deployment.md) | operators | security surfaces, containers, Kubernetes, crash safety, probes, hugepages, Windows, release and deprecation policy |
| [nmos-ipmx.md](nmos-ipmx.md) | NMOS and IPMX products; Phase 7 | NMOS and IPMX in brief, every feature MTL provides for them (name, purpose, API, milestone), the open-source NMOS stacks, the integration, a demo plan; the Phase 7 design: the IS-05 contract, SDP, RTCP sender reports, timing without PTP, encryption |
| [presentation/slides.md](presentation/slides.md) | presenters | a talk on the design and the plan (Marp) |
| [sketch/](sketch/README.md) | everyone | the normative headers, the compiling examples, `check.sh` |
| [design/](design/wait-tests.md) | implementers | the wait protocol's pause-hook tests WH1–WH21 and the models (`design/models/`) that C1w and MS2b's provide tests copy |

## 3. Reading the IDs

| ID | Means | Defined in |
|---|---|---|
| MS1…MS7, Phase 7 | milestones | [implementation-plan.md](implementation-plan.md) §1.2 |
| P0, S0, T1, H1b, E1, C0, C1a, C1w, C1b, A1, C2, B1, B2, A2a, A2c, I1, R1, P1, A2b, B3, X | the tasks of MS1 | [implementation-plan.md](implementation-plan.md) §5.2 |
| C-FPS, C-GRANT, C-BRIDGE, C1h (MS2a), N1–N8, N6a, N6b (MS4a2) | later tasks named by the designs | [implementation-plan.md](implementation-plan.md) §6 |
| R1–R8 | the rules every call follows | `mtl.h`, [contract.md](contract.md) §1 |
| D-01…D-156 | design decisions and their reason | [decisions.md](decisions.md) §2 |
| OI-n | open implementation issues, each with its rule and milestone | [decisions.md](decisions.md) §5 |
| G-01…G-142, G-PKT-n | guarantees, each with its test | [implementation-plan.md](implementation-plan.md) §8, [requirements.md](requirements.md) §4 |
| R-xxx-n, R-PKT-n, K-REQ-n, N-REQ-n, I-REQ-n | requirements | [requirements.md](requirements.md) |
| GO-n, NG-n, P1–P9 | goals, non-goals, personas | [requirements.md](requirements.md) §2 |
| E1–E15 (E5 split into E5a, E5b), R1, R2, MF1–MF10, EK1–EK21, PE1–PE9 | engine changes | [engine.md](engine.md) §11 |
| SF-n, SP-n, DD-n, H-K-n | defects and drift in today's code; pod hazards | [engine.md](engine.md) §12 |
| H1–H10, W0–W3 | pinned-core hazards, wake-up mechanisms | [engine.md](engine.md) §2, §7 |
| S0, S1, S4–S8 | measurements (spikes) | [engine.md](engine.md) §11.1, [implementation-plan.md](implementation-plan.md) §3.3 |
| WH1–WH21, ST1–ST9, SR1–SR5 | the pause-hook tests of the wait protocol, the S1 thresholds and decision rules | [implementation-plan.md](implementation-plan.md) §8.2, [engine.md](engine.md) §11.1 |
| U-001…U-420, H-01…H-19, CUT-n | legacy capabilities, pre-hide gaps, the cut list | [coverage.md](coverage.md) |
| NM-*, IX-* | the NMOS and IPMX features MTL provides | [nmos-ipmx.md](nmos-ipmx.md) §3 |
| NX-n | proposals that make the NMOS integration easier | [nmos-ipmx.md](nmos-ipmx.md) §6 |
| G-Nn, GI-n, C-Nn, C-In | NMOS and IPMX gap dispositions and conflicts | [nmos-ipmx.md](nmos-ipmx.md) §21 |
| ex01…ex14 | the examples | [examples.md](examples.md) |

Some letters are reused: R1 and R2 are contract rules and also engine items (R1 also a task), H1 is
a pinned-core hazard while H1b is a task, E1 is a task and an engine change, S0 a task and a spike,
P1 a task and a persona (P1–P9), and G-Nn is an NMOS gap, not a guarantee G-xx. N1–N8 are MS4a2
tasks, while engine.md §7's N0' and N1–N12 are steps of INTERRUPT. The document in the third column
settles which one a text means.

## 4. The sketch

`sketch/` holds the normative headers and the examples, which compile. Run the check after any
change:

```bash
doc/unified-api/sketch/check.sh                 # every header and example, C99 and C++17, gcc and clang
python3 doc/unified-api/sketch/gen_api_doc.py   # regenerates examples.md from sketch/examples.md.in
```

[sketch/README.md](sketch/README.md) says what the check covers. Functions of Phase 7, and the
types only they use, sit under `MTL_LATER`, which only the design checks define; every other
function is exported in the milestone its tag names, in that milestone's version node (within a
milestone the node grows task by task, G-51), and a call to a function above `MTL_LEVEL` fails to
compile naming its milestone (OI-64); a known value not built yet returns `-MTL_ENOTSUP` with `MTL_REASON_NOT_IMPLEMENTED`
(D-106). The library and its version node: [migration.md](migration.md) §7.2.
