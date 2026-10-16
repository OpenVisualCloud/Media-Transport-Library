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

Inside, one **core** serves both the new API and the legacy pipeline calls. It owns the slot table
with its states, the order of results and waiting. One **binding** per essence connects the core to
today's engines through the callbacks they already make ([core.md](core.md)). A program
includes `mtl.h`; fourteen optional headers add what a kind of program needs, one of them
(`mtl_sdp.h`) for the companion library libmtl_sdp. Verbs that several objects share (`mtl_close`, `mtl_interrupt`, `mtl_wait`,
`mtl_reap`, …) are one exported function each over `struct mtl_object`, and
`mtl_release(s, lease)` is one for TX and RX leases, all with typed inline wrappers. Configuration is typed fields, with enums that follow the legacy ones.

## Start here

| You are | Read first | Then | Look up when needed |
|---|---|---|---|
| writing an application | [concepts.md](concepts.md) §2–§8 | [examples.md](examples.md): ex01, ex02, then "Find your use case" | [contract.md](contract.md) §1 (the rules), §5 (data path), §6 (results), §8 (errors); [timing.md](timing.md) §1, §2.1–§2.2, §4.1, §5.2, §6.3–§6.6, §7, §10, §11.1–§11.6 (its "Who reads what"); [deployment.md](deployment.md) before production |
| porting legacy code | [concepts.md](concepts.md) §12 | [migration.md](migration.md) §1, §2, then §4–§6 (fields, enums, coexistence) | [coverage.md](coverage.md) (find a capability); [examples.md](examples.md) §26 |
| writing a framework or codec plugin | [concepts.md](concepts.md) §2–§8 | [frameworks.md](frameworks.md); [examples.md](examples.md) §21–§23 | contract.md §3.8–§3.9 (query, latency), §7 (waiting), §9 (memory); timing.md §10.5 (live sinks); migration.md §7 (plugin ABI); [abi.md](abi.md) (versions) |
| operating MTL (pods, hosts) | [deployment.md](deployment.md) | contract.md §2.4–§2.7 (shutdown, health, device events), §11 (stats) | timing.md §2 (clock policy); abi.md §7 (release and deprecation) |
| building an NMOS or IPMX product | [nmos-ipmx.md](nmos-ipmx.md) §1–§7 | [examples.md](examples.md) §20 | nmos-ipmx.md §10–§23 (Phase 7) |
| the implementing session | §1 below; [autonomous-run.md](autonomous-run.md) | [run-status.md](run-status.md), [ms1-status.md](ms1-status.md) | [implementation-plan.md](implementation-plan.md) §5.2 (one card per task) |
| a task agent | your card in implementation-plan.md §5.2.2 | the sections it lists | [legacy-internals.md](legacy-internals.md) (today's code); [pr1610.md](pr1610.md); [standards.md](standards.md) (a clause) |
| testing | [guarantees.md](guarantees.md) | implementation-plan.md §8; [design/README.md](design/README.md) and design/wait-tests.md | the G-xx of the task |
| maintaining the design | [requirements.md](requirements.md) §2; [decisions.md](decisions.md) §1 and its topic index | decisions.md §2, §5; coverage.md | [abi.md](abi.md) |
| presenting | [presentation/slides.md](presentation/slides.md) | concepts.md | — |

Example IDs (ex21) and the section numbers of [examples.md](examples.md) (§23) differ: the IDs name the files in `sketch/examples/`, the sections count in the order of the page (the table at the head of examples.md maps one to the other). Here "exNN" is a file and "examples.md §n" a section.

## 1. For the implementing session

The work is done by a main session that plans and integrates, and by the repository's subagents:
`mtl-developer` writes code and tests for one task (Gates 0–4), `mtl-reviewer` reviews every saved
diff (Gate 5), `mtl-system-admin` runs KahawaiTest on VFs (Gate 6); pytest runs from the main
session. Their rules are in the repository's `CLAUDE.md` and `.github/claude/agents/`. The run
is autonomous, with no maintainer to ask: [autonomous-run.md](autonomous-run.md) gives the roles
(an orchestrator, one lead per milestone), what replaces the maintainer's approvals and
stop-and-ask, how a question or a plan bug is decided, and the git and push rules. Where it
differs from the process rules below or in the plan, it wins.

**Read first, in this order** (about 39 k tokens in all, at 4 bytes a token):

1. this README §1, [autonomous-run.md](autonomous-run.md), [run-status.md](run-status.md) and
   [ms1-status.md](ms1-status.md) (how the run works, where the work stands);
2. [implementation-plan.md](implementation-plan.md) §1.1, §1.2, §2.1, §2.2, §3, §4, §5.1, §5.2.1,
   §5.2.3, §5.6, §5.8, §8.4: the assumptions, milestones, the architecture, prerequisites,
   where the code goes, MS1's scope (its non-goals are implementation-plan.md §5.1 "Out"), the task table, the order and
   gates, the exit criteria, how the work runs and the budgets;
3. [core.md](core.md) §1–§3 and [engine.md](engine.md) §1.2, §1.4: the layers and the seams, the
   binding ops and the RX shapes, a unit through the core, the slot table and admission; the
   execution contexts and how the core keeps the pinned-core rules;
4. [`mtl.h`](sketch/include/mtl/experimental/mtl.h) lines 1–124: the rules R1–R8.

engine.md §2 and core.md §8 (where each MS1 task starts), coverage.md §6 (ST 2110-20 by
milestone), the plan's test pool (§5.3), RxTxApp port (§5.4), smoke set (§5.5) and risks (§5.7),
and the rest of the headers are read per task.

Then load the rest **by section, per task**: implementation-plan.md §5.2.2 says what each task's
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
  libmtl's from one file of the API shell, `lib/src/unified/compat_log.c` (core.md §1.2; D-110,
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

1. The doc set is committed on `unified-api-design`, and the work branch `unified-api-impl` starts
   from that commit (autonomous-run.md §5); record the commit in [run-status.md](run-status.md)
   and [ms1-status.md](ms1-status.md). `standards/` is in `.git/info/exclude` on the maintainer's
   machine; keep it out of every commit.
2. R0 and L0 first ([autonomous-run.md](autonomous-run.md) §1). Then, in the plan's order: P0
   (the tooling and the histogram), S0 (baseline nights), T1, H1b and E1 (two series), C0 and M0;
   autonomous-run.md §3 replaces the calendar and the maintainer's checkpoints.

The order, the gates and how the work runs are in implementation-plan.md §5.2.3 and §5.8; the state
of every task and the standing instructions are in [ms1-status.md](ms1-status.md).

## 2. The documents, by reader

| Document | For | Answers |
|---|---|---|
| **Users** | | |
| [concepts.md](concepts.md) | everyone | the model, the first sender and receiver, a unit's life, results, timing, memory |
| [examples.md](examples.md) | users | every example, compiled; generated by `sketch/gen_api_doc.py` (edit `sketch/examples.md.in`) |
| [contract.md](contract.md) | users, implementers | what every call does |
| [timing.md](timing.md) | users, implementers | clocks, media time and RTP, launch, late and underrun policy, sync, RX timing |
| [migration.md](migration.md) | porters, wrapper implementers | the call, field and enum maps; coexistence; plugin ABI; bindings |
| [frameworks.md](frameworks.md) | framework and plugin owners | GStreamer, FFmpeg and OBS on the unified API |
| **Implementers** | | |
| [autonomous-run.md](autonomous-run.md) | the implementing session | the autonomous run: roles, what replaces the maintainer, deciding questions and plan bugs, git, cost, resume |
| [run-status.md](run-status.md) | the implementing session; the maintainer after the run | the run's ledger: milestones, decisions taken in the run, items blocked outside it, the final report |
| [ms1-status.md](ms1-status.md) | the implementing session; the maintainer at checkpoints | the state of every MS1 task, standing instructions, open questions |
| [implementation-plan.md](implementation-plan.md) | implementers | milestones, MS1 task cards, tests, budgets, risks, calendar |
| [core.md](core.md) | implementers | the core and its seams: slot table, admission, results, handles, commands, close |
| [wait-protocol.md](wait-protocol.md) | implementers of C1w, C1q1, C1q2 | waking: the words, the procedures, queues, why it is correct |
| [engine.md](engine.md) | implementers | today's engines under the core, the change list, known defects |
| [guarantees.md](guarantees.md) | implementers, testers | every guarantee G-xx with its method, tier and milestone |
| [legacy-internals.md](legacy-internals.md) | implementers | verified facts about today's `lib/` |
| [pr1610.md](pr1610.md) | the MS1 tasks | PR #1610's pitfalls and what each task can reuse |
| [standards.md](standards.md) | implementers, testers | what the standards require, where MTL disagrees today |
| [design/README.md](design/README.md) | implementers | the design models and the task that copies each; the wait protocol's hook tests |
| **Maintainers** | | |
| [requirements.md](requirements.md) | maintainers, implementers | goals, non-goals, personas, the requirement catalogues and what closes each |
| [decisions.md](decisions.md) | maintainers, implementers | one line per decision D-xx, the defaults DF-n, the open issues OI-n |
| [coverage.md](coverage.md) | maintainers, implementers | every legacy capability with its home and milestone; the cut list |
| [abi.md](abi.md) | maintainers, packagers | the library, version nodes, sealing, hiding the legacy headers, release policy, Windows |
| **Operators and products** | | |
| [deployment.md](deployment.md) | operators | security, containers, Kubernetes, crash safety, probes, hugepages |
| [nmos-ipmx.md](nmos-ipmx.md) | NMOS and IPMX products; Phase 7 | what MTL provides for NMOS and IPMX; the Phase 7 design |
| **Presenters and tooling** | | |
| [presentation/slides.md](presentation/slides.md) | presenters (a talk for maintainers and users) | the design and the plan |
| [sketch/](sketch/README.md) | people who edit a header or an example | the normative headers, the examples, `check.sh` |

## 3. Reading the IDs

| ID | Means | Defined in |
|---|---|---|
| MS1…MS7, Phase 7 | milestones | [implementation-plan.md](implementation-plan.md) §1.2 |
| P0, M0, S0, T1, H1b, E1 (E1a–E1d), C0, C1a, C1w, C1b, A1, C2 (C2a, C2b), B1, B2, A2a, A2c, I1, R1, P1, A2b, B3, X | the tasks of MS1 | [implementation-plan.md](implementation-plan.md) §5.2.1 |
| C-FPS, C-GRANT, C-BRIDGE, C1q1, C1q2 (MS2a), G1 (G1a, G1b; MS3), T-REP (MS4a1), N1–N8, N6a, N6b (MS4a2) | later tasks named by the designs | [implementation-plan.md](implementation-plan.md) §6 |
| RB0–RB2, F2-1 | the st20p re-base tasks (MS2b); the F+2 harness task | [implementation-plan.md](implementation-plan.md) §6.1, §6.6 |
| R1–R8 | the rules every call follows | `mtl.h`, [contract.md](contract.md) §1 |
| D-01…D-244 | design decisions and their reason | [decisions.md](decisions.md) §2 |
| DF-01…DF-29 | the defaults for the ST20 port | [decisions.md](decisions.md) §4 |
| OI-n | open implementation issues, each with its rule and milestone | [decisions.md](decisions.md) §5 |
| G-01…G-166, G-PKT-1…7 | guarantees, each with its test | [guarantees.md](guarantees.md) |
| R-xxx-n, R-PKT-n, K-REQ-n, N-REQ-n, I-REQ-n | requirements | [requirements.md](requirements.md) |
| GO-n, NG-n, P1–P9 | goals, non-goals, personas | [requirements.md](requirements.md) §2 |
| E1–E17 (E5 split into E5a, E5b), R1, R2, MF1–MF10, EK1–EK21, PE1–PE9 | engine changes | [engine.md](engine.md) §6 |
| SF-n, SP-n, DD-n, H-K-n | defects and drift in today's code; pod hazards | [engine.md](engine.md) §7 |
| H1–H10, W0–W2 and the notifier contingency | pinned-core hazards, wake-up mechanisms | [engine.md](engine.md) §1, [wait-protocol.md](wait-protocol.md) §4 |
| S0, S1, S4–S8 | measurements (spikes) | [engine.md](engine.md) §6.1, [implementation-plan.md](implementation-plan.md) §3.3 |
| WH2–WH21, WC1–WC6, QW1–QW22 | the pause-hook tests of the wait protocol | [design/wait-tests.md](design/wait-tests.md) §5 |
| o1–o7, q1–q11, r1–r4, p1–p5; the mutants | the model cases of the wait protocol and the mutants named by the step they break | [design/wait-tests.md](design/wait-tests.md) §1.1 |
| E1–E4, M1, K1–K2, F1–F8, T1–T9, D1–D2, Q1–Q3, RW1–RW2, N0', N1–N9, V1–V2, X1–X4, Y1–Y2, P1–P3, PS1–PS4, U1–U2, A1–A11, J1–J10, C1–C5, DV1–DV3 | the step labels of the wait protocol, each local to its procedure | [wait-protocol.md](wait-protocol.md) |
| ST1–ST9 | the S1 thresholds | [implementation-plan.md](implementation-plan.md) §8.4 |
| SR1–SR3, SR6 | the S1 decision rules | [engine.md](engine.md) §6.1 |
| U-001…U-420, H-01…H-19, CUT-n | legacy capabilities, pre-hide gaps, the cut list | [coverage.md](coverage.md) |
| LB-n | the legacy-behaviour ledger | [migration.md](migration.md) §6.5 |
| NM-*, IX-* | the NMOS and IPMX features MTL provides | [nmos-ipmx.md](nmos-ipmx.md) §3 |
| NX-n | proposals that make the NMOS integration easier | [nmos-ipmx.md](nmos-ipmx.md) §6 |
| G-Nn, GI-n, C-Nn, C-In | NMOS and IPMX gap dispositions and conflicts | [nmos-ipmx.md](nmos-ipmx.md) §21 |
| ex01…ex24 | the examples | [examples.md](examples.md) |

Some letters are reused:

| Letters | Meanings | Tell apart by |
|---|---|---|
| R1, R2 | rules every call follows (R1–R8); engine items; R1 is also a task | contract.md §1; engine.md §6; implementation-plan.md §5.2 |
| H1 | a pinned-core hazard (H1–H10); H1b is a task | engine.md §1; implementation-plan.md §5.2 |
| E1 | a task (E1a–E1d); an engine change (E1–E17); a wait-protocol step (E1–E4) | implementation-plan.md §5.2; engine.md §6; wait-protocol.md |
| S0 | a task; a spike | implementation-plan.md §5.2; engine.md §6.1 |
| P1 | a task; a persona (P1–P9); a wait-protocol step (P1–P3) | implementation-plan.md §5.2; requirements.md §2; wait-protocol.md |
| G1, G-Nn | G1 (G1a, G1b) is an MS3 task and G-Nn an NMOS gap, neither a guarantee G-xx | implementation-plan.md §6; nmos-ipmx.md §21 |
| N1–N8 | MS4a2 tasks; N0' and N1–N9 are steps of INTERRUPT | implementation-plan.md §6; wait-protocol.md |
| A1 | a task; an assumption ("assumption A1"); a wait-protocol step (A1–A11) | implementation-plan.md §5.2; implementation-plan.md §1.1; wait-protocol.md |
| T1 | a task; a timing principle (T1–T7); a wait-protocol step (T1–T9) | implementation-plan.md §5.2; timing.md §1; wait-protocol.md |
| R4–R9 | the review claim numbers of PR #1610 (R4, R6, R7, R9 are used); R4, R6, R7 and R8 are also rules (R1–R8) | pr1610.md §1–§2; contract.md §1 |
| D1 | a PR #1610 pitfall; a wait-protocol step (D1–D2) | pr1610.md; wait-protocol.md |

A step label of the wait protocol (A1, C1, D1, E1, P1, T1, …) is local to its procedure: it is not
a task or a test. The document in the third column of the ID table settles which one a text means.

## 4. The sketch

`sketch/` holds the normative headers and the examples, which compile. Run the check after any
change:

```bash
doc/unified-api/sketch/check.sh                 # every header and example, C99 and C++17, gcc and clang
python3 doc/unified-api/sketch/gen_api_doc.py   # regenerates examples.md and every marked table and enum block
```

[sketch/README.md](sketch/README.md) §4 says what the check covers. A Phase 7 function, and the
types only it uses, is declared under `MTL_LATER`; only the design checks define that macro. Every
other function is exported in its milestone: [sketch/README.md](sketch/README.md) §3 says how,
D-106 says why. The library and its version nodes: [abi.md](abi.md) §2.

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
