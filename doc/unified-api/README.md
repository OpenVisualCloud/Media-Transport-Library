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
today's engines through the callbacks they already make ([core.md](core.md)). A program
includes `mtl.h`; fourteen optional headers add what a kind of program needs, one of them
(`mtl_sdp.h`) for the companion library libmtl_sdp. Verbs that several objects share (`mtl_close`, `mtl_interrupt`, `mtl_wait`,
`mtl_reap`, …) are one exported function each over `struct mtl_object`, and
`mtl_release(s, lease)` is one for TX and RX leases, all with typed inline wrappers. Configuration is typed fields, with enums that follow the legacy ones.

## 1. For the implementing session

The work is done by a main session that plans and integrates, and by the repository's subagents:
`mtl-developer` writes code and tests for one task (Gates 0–4), `mtl-reviewer` reviews every saved
diff (Gate 5), `mtl-system-admin` runs KahawaiTest on VFs (Gate 6); pytest runs from the main
session. Their rules are in the repository's `CLAUDE.md` and `.github/claude/agents/`.

**Read first, in this order** (about 43 k tokens in all, at 4 bytes a token):

1. this README §1 and [ms1-status.md](ms1-status.md) (where the work stands);
2. [implementation-plan.md](implementation-plan.md) §1.1, §1.2, §1.3, §2.1, §2.2, §3, §4, §5.1,
   §5.2, §5.6, §5.8, §8.4: the assumptions, milestones, non-goals, the architecture,
   prerequisites, where the code goes, MS1 with its tasks and reading lists, the gates, the exit
   criteria, how the work runs and the budgets;
3. [core.md](core.md) §1–§3 and [engine.md](engine.md) §1.2, §1.4: the layers and the seams, the
   binding ops and the RX shapes, a unit through the core, the slot table and admission; the
   execution contexts and how the core keeps the pinned-core rules;
4. [`mtl.h`](sketch/include/mtl/experimental/mtl.h) lines 1–124: the top comment with the rules
   R1–R8 every call follows.

engine.md §2 and core.md §8 (where each MS1 task starts), the plan's scope map (§2.3), test pool
(§5.3), RxTxApp port (§5.4), smoke set (§5.5) and risks (§5.7), and the rest of the headers are
read per task.

Then load the rest **by section, per task**: implementation-plan.md §5.2 says what each task's
agent reads. Do not load whole documents into one context; the large ones are references.

**Rules that hold for every task:**

- The header wins. A change that adds or renames a public symbol is complete only when the header
  has it and `doc/unified-api/sketch/check.sh` (later the same check over `include/`) passes;
  `python3 doc/unified-api/sketch/gen_api_doc.py` regenerates [examples.md](examples.md).
- One source per table. The reasons, the option keys, the codes per call, the session states
  and calls, the stats keys and the availability of every declared value are data: each has one
  `.def` file in the library tree (README §4), from which the library builds its tables, the
  tests build theirs, and `sketch/gen_api_doc.py` writes the header enum blocks and the document
  tables between `BEGIN TABLE` and `END TABLE` markers. Edit the `.def`, never a marked region;
  `check.sh` fails when a region differs from a regeneration. Before its task creates the
  `.def`, a marked region is the design, kept by hand. **A table without markers is prose: no
  build, check or test reads its cells** (a check may grep a document for a name).
- One home per number. A load-bearing number (a bound, a timeout, a cap, a layout) is stated in
  its home only: the header for a public value, `st_core.h` for an internal layout (until C0, the
  D-row or core.md, as `sketch/numbers.txt` names), its D-row for a decided bound or process rule. Elsewhere state the rule in words and
  link the home; `sketch/numbers.txt` lists the numbers and `check.sh` enforces it.
- The repository's two-world rule and tasklet rules apply to the core and the bindings: a binding
  runs on a tasklet under the session spinlock and is wait-free (core.md §1, engine.md §1).
- The legacy gate (implementation-plan.md §8.5) passes at every milestone: legacy users see only
  bugfixes.
- The acceptance engine parses log lines that libmtl and RxTxApp print; the unified path prints
  libmtl's from one file of the API shell, `lib/src/unified/compat_log.c` (core.md §1; D-110,
  D-177).
- `tests/integration_tests/` (`KahawaiTest`) and `tests/tools/RxTxApp/` are frozen: new-API tests
  go into their copies, `UnifiedKahawaiTest` and `UnifiedRxTxApp`, and both stacks run until the
  legacy API is removed; then task F2-1 moves the session-level suites, the engine's internal
  test, into the unified harness, deletes the rest and gives the unified binaries the old names
  (implementation-plan.md §4.1).
- Commits stay under the commit cap of D-107 (mechanical copies and deletions exempt), tests
  first; a task is one review unit, one commit or a short series of them (implementation-plan.md
  §5.8), through `mtl-reviewer`; the maintainer reviews at most four review units a week, so ask
  at the checkpoints of §5.2 and not between.
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
2. Then, in calendar order: P0 (the tooling and the histogram), S0 (baseline nights), T1, H1b
   and E1 (two series), C0 (the maintainer's first checkpoint); M0 before week 2.

The order, the gates and how the work runs are in implementation-plan.md §5.2 and §5.8; the state
of every task and the standing instructions are in [ms1-status.md](ms1-status.md).

## 2. The documents

| Document | For | Contents |
|---|---|---|
| [concepts.md](concepts.md) | users, everyone | the model, the first sender and receiver, the life of a unit, results and errors, timing and memory in one page each, a glossary, FAQ |
| [examples.md](examples.md) | users | every example of `sketch/examples/`, copied verbatim and compiled, and the header map |
| [implementation-plan.md](implementation-plan.md) | implementers | milestones MS1–MS7, MS1 tasks with reading lists, tests, guarantees G-xx, risks, calendar |
| [core.md](core.md) | implementers | the core and its seams: the layers and the end state, the binding ops, the two RX shapes, the bindings per essence, the instance context, extending the core, the slot table, admission, results and the handle table, commands, waking, close, the core's files |
| [engine.md](engine.md) | implementers | today's engines under the core: the pinned-core rules, where each part starts in `lib/`, the engine accessor header, the published time base, the stalled-queue path, recovery, the engine change list, known defects with `path:line` |
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
| [design/](design/wait-tests.md) | implementers | the wait protocol's pause-hook tests (WH, WC, QW) and the model `final_model.py` that C1w, C1q1 and C1q2 copy; the provide model that MS2b copies |

## 3. Reading the IDs

| ID | Means | Defined in |
|---|---|---|
| MS1…MS7, Phase 7 | milestones | [implementation-plan.md](implementation-plan.md) §1.2 |
| P0, M0, S0, T1, H1b, E1 (E1a–E1d), C0, C1a, C1w, C1b, A1, C2, B1, B2, A2a, A2c, I1, R1, P1, A2b, B3, X | the tasks of MS1 | [implementation-plan.md](implementation-plan.md) §5.2 |
| C-FPS, C-GRANT, C-BRIDGE, C1q1, C1q2 (MS2a), N1–N8, N6a, N6b (MS4a2) | later tasks named by the designs | [implementation-plan.md](implementation-plan.md) §6 |
| RB0–RB2, F2-1 | the st20p re-base tasks (MS2b); the F+2 harness task | [implementation-plan.md](implementation-plan.md) §6.1, §6.6 |
| R1–R8 | the rules every call follows | `mtl.h`, [contract.md](contract.md) §1 |
| D-01…D-207 | design decisions and their reason | [decisions.md](decisions.md) §2 |
| OI-n | open implementation issues, each with its rule and milestone | [decisions.md](decisions.md) §5 |
| G-01…G-142, G-PKT-n | guarantees, each with its test | [implementation-plan.md](implementation-plan.md) §8, [requirements.md](requirements.md) §4 |
| R-xxx-n, R-PKT-n, K-REQ-n, N-REQ-n, I-REQ-n | requirements | [requirements.md](requirements.md) |
| GO-n, NG-n, P1–P9 | goals, non-goals, personas | [requirements.md](requirements.md) §2 |
| E1–E17 (E5 split into E5a, E5b), R1, R2, MF1–MF10, EK1–EK21, PE1–PE9 | engine changes | [engine.md](engine.md) §6 |
| SF-n, SP-n, DD-n, H-K-n | defects and drift in today's code; pod hazards | [engine.md](engine.md) §7 |
| H1–H10, W0–W2 and the notifier contingency | pinned-core hazards, wake-up mechanisms | [engine.md](engine.md) §1, [core.md](core.md) §6 |
| S0, S1, S4–S8 | measurements (spikes) | [engine.md](engine.md) §6.1, [implementation-plan.md](implementation-plan.md) §3.3 |
| WH2–WH21, WC1–WC5, QW1–QW22, ST1–ST9, SR1–SR3, SR6 | the pause-hook tests of the wait protocol, the S1 thresholds and decision rules | [implementation-plan.md](implementation-plan.md) §8.2, [engine.md](engine.md) §6.1 |
| U-001…U-420, H-01…H-19, CUT-n | legacy capabilities, pre-hide gaps, the cut list | [coverage.md](coverage.md) |
| LB-n | the legacy-behaviour ledger | [migration.md](migration.md) §6.5 |
| NM-*, IX-* | the NMOS and IPMX features MTL provides | [nmos-ipmx.md](nmos-ipmx.md) §3 |
| NX-n | proposals that make the NMOS integration easier | [nmos-ipmx.md](nmos-ipmx.md) §6 |
| G-Nn, GI-n, C-Nn, C-In | NMOS and IPMX gap dispositions and conflicts | [nmos-ipmx.md](nmos-ipmx.md) §21 |
| ex01…ex24 | the examples | [examples.md](examples.md) |

Some letters are reused: R1 and R2 are contract rules and also engine items (R1 also a task), H1 is
a pinned-core hazard while H1b is a task, E1 is a task and an engine change, S0 a task and a spike,
P1 a task and a persona (P1–P9), and G-Nn is an NMOS gap, not a guarantee G-xx. N1–N8 are MS4a2
tasks, while core.md §6's N0' and N1–N9 are steps of INTERRUPT. The document in the third column
settles which one a text means.

## 4. The sketch

`sketch/` holds the normative headers and the examples, which compile. Run the check after any
change:

```bash
doc/unified-api/sketch/check.sh                 # every header and example, C99 and C++17, gcc and clang
python3 doc/unified-api/sketch/gen_api_doc.py   # regenerates examples.md and every marked table and enum block
```

[sketch/README.md](sketch/README.md) says what the check covers. Functions of Phase 7, and the
types only they use, sit under `MTL_LATER`, which only the design checks define; every other
function is exported in the milestone its tag names, in that milestone's open version node (it
grows task by task and is sealed at the exit and at every release, G-51), and a call to a function above `MTL_LEVEL` fails to
compile naming its milestone (OI-64); a known value not built yet returns `-MTL_ENOTSUP` with `MTL_REASON_NOT_IMPLEMENTED`
(D-106). The library and its version node: [migration.md](migration.md) §7.2.

The data tables and their homes:

| `.def` | Home | Generates | From task |
|---|---|---|---|
| reasons | `lib/src/unified/reasons.def` | `mtl_reasons.h` enum; [contract.md](contract.md) §8.3 | H1b |
| option keys | `lib/src/unified/mtl_options.def` | `mtl_options.h` key enum; contract.md §12.6 | H1b |
| availability of values | `lib/src/unified/availability.def` | — (`st_avail_check`, `Api.not_implemented`) | H1b |
| codes per call | `lib/src/unified/codes_per_call.def` | contract.md §8.4 | H1b |
| session states and calls | `lib/src/st2110/core/st_core_states.def` | contract.md §4.1 picture, §4.2 table | C1a |
| stats keys | `lib/src/unified/stats_keys.def` | contract.md §11.3 | A2b |
| G-114 exemptions | `app/sample/g114_exempt.txt` | — | P1 |
