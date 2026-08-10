#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""
MCP Server for running this repository's GitHub Actions workflows locally.

The workflows that matter here run on self-hosted, NIC-owning runners, which
makes the usual debugging loop -- push a commit, wait for a queue, download a
40 MB log -- expensive enough that failures get guessed at rather than
diagnosed. .github/ci-local/ runs those jobs in a container that simulates
the runner; this server exposes it as tools so an agent can drive the loop
itself instead of shelling out and pasting logs into its own context.

Every tool caps what it returns: full output goes to a file under
build/logs/, and the tool result carries a verdict, a path and, on failure,
the tail. Reproductions and builds take minutes, so timeouts are generous.

The server is read-only with respect to GitHub. It never pushes, re-runs a
remote workflow, comments, or merges -- everything happens on this machine.

Usage:
    pip install -r requirements.txt
    python mtl_ci_mcp_server.py
"""

from __future__ import annotations

import shlex
import textwrap
from pathlib import Path

from mcp.server.fastmcp import FastMCP
from mtl_setup_common import (
    REPO_ROOT,
    _run_rc,
    _summarize_output,
)

CI_LOCAL = REPO_ROOT / ".github" / "ci-local"
RUN_JOB = CI_LOCAL / "run-job.sh"
STATE_DIR = REPO_ROOT / ".ci-local"
LOG_DIR = STATE_DIR / "logs"
OUT_DIR = STATE_DIR / "out"

VALID_NICS = ("e810", "e830", "e835", "e825")

mcp = FastMCP(
    "mtl-ci-local",
    instructions=textwrap.dedent(
        """\
        MTL Local CI MCP Server — runs the repository's GitHub Actions jobs
        on this machine, in a container that simulates the runner.

        Common workflows:
        • Before pushing:        ci_test_pr()
        • One job:               ci_run_job("build") / ci_run_job("validate-host", nic="e810")
        • Why did CI fail?:      ci_cache_status() → ci_run_job(...) → ci_last_log()
        • Known cache bug:       ci_reproduce_cache_poisoning()
        • Host prerequisites:    ci_check_ebpf()
        • Shared entry points:   ci_list_tasks() → ci_run_task("ebpf:check")

        Never talks to GitHub: no pushes, no re-runs, no comments. To prepare
        a real host for hardware tests use the `mtl-system-setup` server; to
        prepare pytest use `mtl-validation-setup`.
        """
    ),
)


def _summary_block(out: str) -> str:
    """Extract the `=== ci-local ... summary ===` block run-job.sh prints."""
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if line.startswith("=== ci-local"):
            return "\n".join(lines[i:])
    return ""


def _latest_log(pattern: str = "*.log") -> Path | None:
    if not LOG_DIR.is_dir():
        return None
    logs = sorted(LOG_DIR.glob(pattern), key=lambda p: p.stat().st_mtime)
    return logs[-1] if logs else None


@mcp.tool()
def ci_list_jobs() -> str:
    """List the workflow jobs that can be run locally.

    Each name maps to a script under .github/ci-local/jobs/ that mirrors the
    steps of the corresponding job, one section per workflow step.
    """
    jobs_dir = CI_LOCAL / "jobs"
    if not jobs_dir.is_dir():
        return f"No jobs directory at {jobs_dir}"
    lines = ["Jobs runnable via ci_run_job():", ""]
    for path in sorted(jobs_dir.glob("*.sh")):
        # The third comment line of each job script says what it mirrors.
        desc = ""
        for line in path.read_text().splitlines()[:12]:
            if line.startswith("# The ") or line.startswith("# The `"):
                desc = line.lstrip("# ").rstrip()
                break
        lines.append(f"- **{path.stem}** — {desc}")
    lines += [
        "",
        "NIC matrix for the bare-metal jobs: " + ", ".join(VALID_NICS),
    ]
    return "\n".join(lines)


@mcp.tool()
def ci_run_job(
    job: str = "build",
    nic: str = "",
    force: str = "",
    cache_mode: str = "strict",
    clean: bool = False,
    timeout_s: int = 3600,
) -> str:
    """Run one workflow job locally in the simulated runner.

    Args:
        job: job name, e.g. "build" or "validate-host" (see ci_list_jobs).
        nic: matrix NIC to simulate for bare-metal jobs (e810/e830/e835/e825).
             Selects the bare-metal runner image and sets PCI_DEVICE.
        force: comma-separated components to rebuild despite a cache hit,
             or "all". Components: dpdk, mtl, ffmpeg, gstreamer, plugins.
        cache_mode: "strict" (a cache hit must be structurally usable, and
             only a passing job may save) or "github" (what the workflow does
             today -- the key alone decides). Use "github" to reproduce the
             poisoned-cache failure.
        clean: discard the working copy and take a fresh one first.
        timeout_s: give a cold build room; it compiles DPDK, MTL and FFmpeg.

    Returns a verdict, the cache state per component, and the log path.
    """
    if not RUN_JOB.is_file():
        return f"ERROR: {RUN_JOB} not found."
    if nic and nic not in VALID_NICS:
        return f"ERROR: unknown nic '{nic}'. Valid: {', '.join(VALID_NICS)}"
    if cache_mode not in ("strict", "github"):
        return f"ERROR: cache_mode must be 'strict' or 'github', got '{cache_mode}'"

    cmd = [str(RUN_JOB), job, "--cache-mode", cache_mode]
    if nic:
        cmd += ["--nic", nic]
    if force:
        cmd += ["--force", force]
    if clean:
        cmd += ["--clean"]

    rc, out = _run_rc(cmd, timeout=timeout_s)
    summary = _summary_block(out)
    body = _summarize_output(f"ci_local_{job}{'_' + nic if nic else ''}", out, rc=rc)
    return f"{body}\n\n```\n{summary}\n```" if summary else body


@mcp.tool()
def ci_test_pr(
    nics: str = "e810,e830,e835",
    skip_build: bool = False,
    with_lint: bool = False,
    timeout_s: int = 5400,
) -> str:
    """Run everything a pull request triggers, in the order GitHub runs it.

    build.yml, then the pr-gate change filter, then .github/actions/validate-host
    for each NIC in the matrix. The hardware tests themselves (pytest -m smoke,
    gtest.sh) cannot run in a container and are reported as such.

    Args:
        nics: comma-separated matrix NICs, or "none" to stop after the build.
        skip_build: assume .local_install is already populated.
        with_lint: also run super-linter, as linter.yml does. Pulls a large
             image on first use.
    """
    script = CI_LOCAL / "test-pr-locally.sh"
    if not script.is_file():
        return f"ERROR: {script} not found."

    cmd = [str(script), "--nic", nics]
    if skip_build:
        cmd += ["--skip-build"]
    if with_lint:
        cmd += ["--with-lint"]

    rc, out = _run_rc(cmd, timeout=timeout_s)
    # The script's own summary table is the useful part; keep all of it.
    lines = out.splitlines()
    table = ""
    for i, line in enumerate(lines):
        if line.startswith("═══ test-pr-locally summary"):
            table = "\n".join(lines[i:])
            break
    return f"{body}\n\n```\n{table}\n```" if table else body
    body = _summarize_output("ci_test_pr", out, rc=rc)


@mcp.tool()
def ci_cache_status() -> str:
    """Show, per component, whether the local cache would hit, and why.

    Compares the current source checksums (the keys the `checksums` job
    computes) against the saved stamps, and checks that each restored tree
    actually contains the artifact its consumers resolve. A key that matches
    while the tree is unusable is the failure mode this whole harness exists
    for.
    """
    hash_script = REPO_ROOT / "script" / "hash_sources.sh"
    if not hash_script.is_file():
        return f"ERROR: {hash_script} not found."

    import tempfile

    with tempfile.NamedTemporaryFile(suffix=".env") as tmp:
        rc, out = _run_rc([str(hash_script), "-o", tmp.name], timeout=300)
        if rc != 0:
            return f"ERROR: hash_sources.sh failed (exit {rc})\n```\n{out[-2000:]}\n```"
        keys = dict(
            line.split("=", 1)
            for line in Path(tmp.name).read_text().splitlines()
            if "=" in line
        )

    cache_dir = REPO_ROOT / ".local_install"
    stamp_dir = cache_dir / ".stamps"
    artifacts = {
        "dpdk": "libdpdk.pc",
        "mtl": "mtl.pc",
        "ffmpeg": "libavcodec.pc",
        "gstreamer": "*.so",
        "plugins": "*.so",
    }

    rows = ["| component | key | stamp | tree | verdict |", "|---|---|---|---|---|"]
    for comp, artifact in artifacts.items():
        key = keys.get(comp, "")
        stamp_file = stamp_dir / comp
        stamp = stamp_file.read_text().strip() if stamp_file.is_file() else ""
        tree = cache_dir / comp
        usable = tree.is_dir() and any(tree.rglob(artifact))
        if not key:
            verdict = "no key"
        elif stamp != key:
            verdict = "MISS (key changed)"
        elif not usable:
            verdict = "**STALE** (key matches, tree unusable)"
        else:
            verdict = "HIT"
        rows.append(
            f"| {comp} | `{key[:12]}` | `{stamp[:12] or '-'}` | "
            f"{'ok' if usable else 'missing ' + artifact} | {verdict} |"
        )

    return "\n".join(
        ["### Local cache state", "", *rows, "", f"Cache root: `{cache_dir}`"]
    )


@mcp.tool()
def ci_check_ebpf(mode: str = "all", strict: bool = False, require_xdp: bool = False) -> str:
    """Check this host's eBPF/XDP prerequisites, as the CI jobs now do.

    Args:
        mode: "build" (what compiling DPDK and the eBPF objects needs),
              "runtime" (what running AF_XDP on this kernel needs), or "all".
        strict: fail on a missing required prerequisite instead of reporting.
        require_xdp: also demand xdp-tools/libbpf at the exact versions pinned
              in versions.env. This is what base_build asserts after building
              them, and what a host provisioned for AF_XDP should satisfy.

    Same script and flags as `.github/actions/check-ebpf` and `task ebpf:check`,
    so its verdict is the one CI will reach.
    """
    script = REPO_ROOT / "script" / "build_ebpf_xdp.sh"
    if not script.is_file():
        return f"ERROR: {script} not found."
    if mode not in ("build", "runtime", "all"):
        return f"ERROR: mode must be build, runtime or all; got '{mode}'"
    cmd = ["bash", str(script), "--check", "--mode", mode]
    if strict:
        cmd += ["--strict"]
    if require_xdp:
        cmd += ["--require-xdp"]
    rc, out = _run_rc(cmd, timeout=120)
    return f"**Result: {'OK' if rc == 0 else f'FAILED (exit {rc})'}**\n```\n{out}\n```"


@mcp.tool()
def ci_list_tasks() -> str:
    """List the Taskfile entry points CI and developers share.

    These are the commands the workflows themselves call, so running one
    locally executes the same code path a runner would.
    """
    rc, out = _run_rc("task --list", timeout=60)
    if rc != 0:
        return (
            "ERROR: `task` is unavailable or the Taskfile is invalid.\n"
            "Install it from https://taskfile.dev/installation/.\n"
            f"```\n{out}\n```"
        )
    return f"```\n{out}\n```"


@mcp.tool()
def ci_run_task(task: str, args: str = "", timeout_s: int = 1800) -> str:
    """Run a Taskfile task, e.g. "ebpf:check" or "ci:validate-host".

    Args:
        task: task name as reported by ci_list_tasks().
        args: extra arguments, e.g. "MODE=build REQUIRE_XDP=1" or "NIC=e830".
        timeout_s: give builds and matrix runs room; checks finish in seconds.

    Prefer this over shelling out: it is the same invocation the composite
    actions use, so a pass here means the same command passes in CI.
    """
    if not task or task.startswith("-"):
        return "ERROR: pass a task name, e.g. 'ebpf:check'."
    cmd = shlex.join(["task", task] + shlex.split(args))
    rc, out = _run_rc(cmd, timeout=timeout_s)
    verdict = "OK" if rc == 0 else f"FAILED (exit {rc})"
    return f"**task {task}: {verdict}**\n```\n{_summarize_output(out)}\n```"


@mcp.tool()
def ci_last_log(job: str = "", lines: int = 60, grep: str = "") -> str:
    """Return the tail of the most recent local job log.

    Args:
        job: restrict to a job, e.g. "build" or "validate-host". Empty = latest.
        lines: how many trailing lines to return (capped at 400).
        grep: if given, return matching lines instead of the tail. Use it to
              find the first real error in a long build log, e.g. "error|not found".
    """
    lines = max(1, min(lines, 400))
    log = _latest_log(f"{job}*.log" if job else "*.log")
    if log is None:
        return f"No logs under {LOG_DIR}. Run a job first."

    content = log.read_text(errors="replace")
    total = len(content.splitlines())
    if grep:
        rc, out = _run_rc(["grep", "-inE", grep, str(log)], timeout=60)
        matched = out.splitlines()[-lines:]
        body = "\n".join(matched) if matched else "(no matches)"
        return f"`{log}` ({total} lines), matching `{grep}`:\n```\n{body}\n```"
    tail = "\n".join(content.splitlines()[-lines:])
    return f"`{log}` ({total} lines), last {lines}:\n```\n{tail}\n```"


@mcp.tool()
def ci_diagnostics() -> str:
    """Return the environment snapshot the last local job captured.

    The build job writes what was on PKG_CONFIG_PATH, which .pc files existed
    and how .local_install was laid out at the moment it finished; validate-host
    writes the environment it hands the tests. These are the facts a CI log
    almost never contains and that most failures turn on.
    """
    files = [
        OUT_DIR / "diagnostics.txt",
        OUT_DIR / "validate-host-diagnostics.txt",
    ]
    parts = []
    for path in files:
        if path.is_file():
            text = path.read_text(errors="replace")
            if len(text) > 8000:
                text = text[:8000] + "\n... (truncated)"
            parts.append(f"### `{path.name}`\n```\n{text}\n```")
    if not parts:
        return f"No diagnostics under {OUT_DIR}. Run a job first."
    return "\n\n".join(parts)


if __name__ == "__main__":
    mcp.run()
