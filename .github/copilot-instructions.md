# Media Transport Library (MTL) - Copilot Instructions

## Overview

MTL implements SMPTE ST 2110 for professional media transport over IP. DPDK-based with HW pacing (Intel E810/E830). Supports ST2110-20 (video), ST2110-22 (compressed video), ST2110-30 (audio), ST2110-40 (ancillary), ST2110-41 (fast metadata).

## Production-Quality Bar

This is a **client-visible, production repository**. Every change goes through rigorous review.

- **Performance and maintainability above all** — design the simplest architecture that is fast and easy to understand. Never patch around a problem; find the right solution.
- **Design before code** — understand the problem, study existing patterns, plan the approach, then implement. Do not start writing code until you can explain why this design is the right one.
- **Minimal diffs only** — do not add speculative code, convenience wrappers, or refactors beyond what was explicitly requested.
- **No dead code** — never commit commented-out code, unused helpers, or placeholder stubs.
- **No gratuitous changes** — do not touch formatting, comments, or variable names in lines you are not modifying for functional reasons.
- **Each patch must be self-contained** — buildable, testable, and small enough to review in one sitting.

## Always-On Coding Conventions

- **C99 only** in library core (`lib/`). C++ allowed only in tests (gtest).
- **Naming prefixes**: `mt_` (core internals), `mtl_` (public core API), `st_`/`st20_`/`st22_`/`st30_`/`st40_`/`st41_` (media APIs), `st20p_`/`st22p_`/`st30p_` (pipeline APIs).
- **Error returns**: 0 = success, negative = error. Free resources in reverse allocation order on failure.
- **Never block in tasklets** — no malloc, no mutex, no sleep, no INFO-level logging in data-plane paths.
- **Formatting**: `clang-format-14` enforced by CI. Always run `./format-coding.sh` before committing.
- **Build verification**: Always run `./build.sh` after changes to verify compilation.
- **Logging**: Use `dbg()`/`info()`/`warn()`/`err()`. Never use `printf`.
- **Comments**: Short and descriptive. Do not comment obvious code.

## Available Tooling

- **MCP Server** (`mtl-system-setup`): 35+ tools for host setup — hugepages, VFs, ICE driver, MtlManager, running gtests. Use these instead of raw shell commands for system administration.
- **Agents**: "MTL Planner" (routes multi-subsystem work), "MTL Developer (TDD)" (writes code + tests in one context window, enforces the six-gate TDD loop), "MTL Reviewer" (adversarial code review — enforced exit gate), "MTL System Admin" (host setup + KahawaiTest via MCP — enforced exit gate for data-plane changes), "MTL Validation Setup" (pytest framework prep), "Explore" (read-only Q&A).
- **Skills**: `/mtl-build` (build + format + verify workflow); `/mtl-write-test` (author a new unit/integration/pytest test — tier picker + golden templates); `/mtl-commit` (stage + commit as atomic, well-formed commits — user-triggered, never automatic).
- **Knowledge Base**: `.github/copilot-docs/mtl-knowledge-base.md` — architecture, session API lifecycle, pacing, data-plane internals. Consult before non-trivial library changes.

## Agent Routing Matrix

Use this table to pick the right subagent. Resolve ambiguity in order:
**(1) file-path match → (2) binary/tool match → (3) read-only? use Explore → (4) multi-agent? use Planner.**

| Task | Agent | Why |
|---|---|---|
| Multi-step work crossing 2+ subsystems (code + host + manager + plugins…) | **MTL Planner** | Decomposes and routes; no execution |
| Edit any of `lib/`, `include/`, `app/`, `plugins/`, `ecosystem/`, `tests/unit/`, `tests/integration_tests/` | **MTL Developer (TDD)** | Owns code + tests + the six-gate TDD loop in one context window |
| Build (`./build.sh`, `ninja -C build`, `./format-coding.sh`) | **MTL Developer (TDD)** | Build is Gate 4 of its loop |
| Run `./build.sh unit` (unit gtest, no NIC) | **MTL Developer (TDD)** | Test runs are Gate 2 (fail) and Gate 4 (pass) |
| Run `KahawaiTest` (integration gtest, real VFs) | **MTL System Admin** | Only one with `run_gtest` MCP tool; enforced Gate 6 for data-plane changes |
| Host setup (hugepages, VFs, drivers, MtlManager) | **MTL System Admin** | MCP-only, no shell |
| Adversarial review of a saved diff | **MTL Reviewer** | Read-only; enforced Gate 5; refuses if diff empty |
| Prepare pytest under `tests/validation/` (apt, NFS, configs) | **MTL Validation Setup** | MCP-only, idempotent (`setup_validation_full`) |
| Run pytest under `tests/validation/` | *main agent* per `.github/instructions/mtl-validation-tests.instructions.md` | Validation Setup refuses by design |
| Read-only Q&A, code archaeology, "where is X defined?" | **Explore** | Cheap, parallelizable |

**Capability boundaries** — every agent declares CAN/CANNOT in its body's "Capability contract" section. If a tool the agent needs is unavailable (e.g. shell `execute` disabled), the agent refuses at Gate 0 rather than producing degraded work. When refused, either enable the missing tool or pick a different agent from the matrix.

## Default workflow for any code change — the six-gate TDD loop

Every behavior-changing edit walks these gates in order. They live inside **MTL Developer (TDD)**; the Planner uses them as the default plan shape for multi-subsystem work.

0. **Tools present** — `execute` (shell) + `build/` exist, else refuse.
1. **Knowledge** — written "context I established" block: subsystem, files, KB section, invariants. Delegate to **Explore** if the agent cannot fill it.
2. **Failing test** — a gtest at the right tier ([`/mtl-write-test`](skills/mtl-write-test/SKILL.md)) exists and **fails** before any production-code edit. Pure-refactor / docs / build-system changes may skip with a stated exemption.
3. **Implement** — minimal diff to pass the Gate 2 test.
4. **Green test + clean build** — same test passes; `./format-coding.sh` + `./build.sh` clean.
5. **Review** — **MTL Reviewer** verdict, no unaddressed BLOCKERs. **Mandatory, no exemption.**
6. **Integration** — **MTL System Admin** runs the matching `KahawaiTest` filter on real VFs. Mandatory for data-plane / session-lifecycle / pacing / DMA / RSS / kernel-socket / AF_XDP / virtio-user changes; may be skipped with a stated exemption for pure control-plane code.

The agent walks Gates 0–4 inside its own body — it must report evidence for each
before firing Gate 5. Gates 5 and 6 are **handoffs to sibling agents**; the Develop
agent's reply terminates before they respond. The user (or orchestrator) owns the
decision to commit once Reviewer returns. If Reviewer raises BLOCKERs the user
re-invokes the Develop agent with them, and Gates 2–4 run again for the fix. This is
why Gates 5 and 6 are the only truly enforceable checks — they involve independent
agents producing independent evidence; Gates 0–4 rely on the Develop agent following
its own checklist.
