# GitHub Actions issue — CI tooling plan

## The problem

Our `build` job fails because it cannot find the MTL library. We will fix that,
and build tooling (an MCP server for CI actions + scripts in `.github/scripts`)
so the AI can inspect PR results directly after local tests. The scripts must be
independent and simple enough that a human can run them by hand — the MCP server
only wraps them.

### Failing run

- Job: <https://github.com/OpenVisualCloud/Media-Transport-Library/actions/runs/30359264440/job/90275514969?pr=1661>
- PR: <https://github.com/OpenVisualCloud/Media-Transport-Library/pull/1661>

### Confirmed failure

Run `30359264440` (`fix_double_free`, PR #1661) — `build` job, step
*Setup environment and build*:

```text
ERROR: mtl >= 22.12.0 not found using pkg-config
```

The cache reported `MTL=HIT`, so nothing rebuilt it, and `PKG_CONFIG_PATH`
(`.local_install/mtl/lib/x86_64-linux-gnu/pkgconfig`) did not resolve. That is
the "where is the MTL library" problem.

## `gh` CLI status

Installed (v2.96.0, official apt repo) and authenticated. Token scopes:
`repo`, `workflow`, `read:org`, `gist` — enough to read Actions runs, jobs,
logs and artifacts.

---

## Design: what to build and why

The common mistake is wrapping `gh` in MCP and calling it done. `gh` is already
a fine CLI; the AI's problem is not *access*, it is **signal-to-noise and
turnaround time**. Raw Actions logs are tens of megabytes — a single
`gh run view --log-failed` on the run above returned megabytes of FFmpeg
`inflating:` lines to surface one error line. Any tool that hands a model a raw
log burns the context window before it diagnoses anything.

**Design principle: the MCP layer's job is evidence reduction and local
reproduction, not API proxying.**

### 1. Keep the existing layering

Plain scripts in `.github/scripts/` that a human can run standalone;
`mtl_ci_mcp_server.py` is a thin wrapper, mirroring `mtl_mcp_server.py` +
`mtl_setup_common.py`. Logic must never live only inside MCP — it is lost the
moment the server is not running.

### 2. Small, composable, output-capped primitives

- `ci_run_list(branch|pr)` — recent runs + conclusions (tiny output).
- `ci_failure_report(run_id)` — the key tool: walk failed jobs → failed steps →
  extract `##[error]` / `##[warning]` annotations plus N lines of context around
  each, dedupe, **hard-cap output**. Returns a structured summary, never a log.
- `ci_log_grep(run_id, job, regex, context)` — targeted follow-up when the
  report is not enough.
- `ci_artifacts(run_id)` — pull diagnostic bundles (see §4).

Every tool returns bounded text and writes the full blob to a temp path the
model can grep on demand.

### 3. Local reproduction is the highest-leverage piece

A push-to-test loop is ~10 minutes; local repro is seconds.
`ci_reproduce(run_id, job)` materializes the job's `env:` block and inputs into
a `.env` and runs **the same** `.github/scripts/setup_environment.sh` locally.
The workflows already call shared scripts, so this is mostly plumbing — and it
turns the AI from "guess, push, wait" into an actual debug loop.

### 4. Make CI emit diagnostics instead of forcing log archaeology

The current failure is undiagnosable from the log alone. Add a failure trap to
the build scripts that dumps a bundle and uploads it as an artifact:

- `PKG_CONFIG_PATH`
- `pkg-config --list-all | grep -i mtl`
- `find .local_install -name '*.pc'`
- cache hit/miss per component
- `$GITHUB_WORKSPACE`, tool versions, disk free

Plus `::error::` annotations so `gh run view --json` yields structured errors
rather than needing regex. This single change is worth more than the entire MCP
server.

### 5. Remote / self-hosted specifics

`runs-on: dpdk` is our own box, so state persists after a failure. Resist
building SSH-into-runner tooling — it needs secrets, it is racy, and it breaks
when the runner is reimaged. The always-run diagnostics step in §4 gets ~95% of
the value with zero credentials.

### 6. Root-cause class worth fixing properly

Caches are keyed on **source checksum**, but the installed artifacts embed
**absolute paths** (`.pc` files hardcode `prefix=`). Restoring that cache under
a different workspace path makes pkg-config silently find nothing — a whole
class of "works on rerun, fails on PR" flakes. Worth a `ci_cache_audit` tool
and/or making the `.pc` files relocatable.

### 7. Guardrails

Read-only by default. No `run rerun`, `pr merge`, `pr comment`, or
`workflow dispatch` in the tool surface without explicit confirmation — an AI
that can retrigger CI will retrigger CI forever instead of fixing the bug. Cap
every output. Never echo token or secret values.

---

## Build order

1. `ci_failure_report`
2. CI diagnostic bundle (§4)
3. `ci_reproduce`
4. The rest (`ci_log_grep`, `ci_artifacts`, `ci_cache_audit`)

The first two would have diagnosed this exact failure automatically.
