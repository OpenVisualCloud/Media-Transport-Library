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
- **Agents**: "MTL Developer" (code changes), "MTL TDD" (tests-first workflow), "MTL Reviewer" (adversarial code review), "MTL System Admin" (host setup via MCP), "MTL Validation Setup" (pytest framework prep).
- **Skills**: `/mtl-build` (build + format + verify workflow).
- **Knowledge Base**: `.github/copilot-docs/mtl-knowledge-base.md` — architecture, session API lifecycle, pacing, data-plane internals. Consult before non-trivial library changes.
