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

The production-inspection tools are read-only with respect to GitHub. They
query pushed PR checks but never push, re-run a workflow, comment, or merge.

Usage:
    pip install -r requirements.txt
    python mtl_ci_mcp_server.py
"""

from __future__ import annotations

import json
import re
import shlex
import textwrap
from pathlib import Path

from mcp.server.fastmcp import FastMCP
from mtl_setup_common import REPO_ROOT, _run_rc, _summarize_output

CI_LOCAL = REPO_ROOT / ".github" / "ci-local"
RUN_JOB = CI_LOCAL / "run-job.sh"
STATE_DIR = REPO_ROOT / ".ci-local"
LOG_DIR = STATE_DIR / "logs"
OUT_DIR = STATE_DIR / "out"

VALID_NICS = ("e810", "e830", "e835", "e825")
GITHUB_REPO_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
FAILED_CONCLUSIONS = {
    "action_required",
    "cancelled",
    "failure",
    "startup_failure",
    "timed_out",
}
ERROR_RE = re.compile(
    r"(?:##\[error\]|\b(?:error|fatal|failed|failure)\b|not found|no such file)",
    re.IGNORECASE,
)

mcp = FastMCP(
    "mtl-ci-local",
    instructions=textwrap.dedent(
        """\
        MTL CI MCP Server — runs jobs locally and inspects pushed PR checks.

        Common workflows:
        • After pushing:         ci_pr_checks(1676) → ci_pr_failures(1676)
        • Before pushing:        ci_test_pr()
        • One job:               ci_run_job("build") / ci_run_job("validate-host", nic="e810")
        • Why did CI fail?:      ci_cache_status() → ci_run_job(...) → ci_last_log()
        • Host prerequisites:    ci_check_ebpf()
        • Shared entry points:   ci_list_tasks() → ci_run_task("ebpf:check")

        GitHub access is read-only: no pushes, re-runs, comments, or merges.
        To prepare a real host for hardware tests use the `mtl-system-setup`
        server; to prepare pytest use `mtl-validation-setup`.
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


def _github_repo(repo: str) -> tuple[str | None, str | None]:
    if repo:
        candidate = repo
    else:
        rc, out = _run_rc(["git", "remote", "get-url", "origin"], timeout=30)
        if rc != 0:
            return None, "ERROR: cannot determine GitHub repository from origin."
        candidate = out.strip().removesuffix(".git")
        candidate = re.sub(r"^(?:https://github\.com/|git@github\.com:)", "", candidate)
    if not GITHUB_REPO_RE.fullmatch(candidate):
        return (
            None,
            f"ERROR: invalid GitHub repository '{candidate}'. Expected owner/repo.",
        )
    return candidate, None


def _gh_json(args: list[str], timeout: int = 120) -> tuple[object | None, str | None]:
    rc, out = _run_rc(["gh", *args], timeout=timeout)
    if rc != 0:
        command = " ".join(args[:2])
        return None, f"ERROR: gh {command} failed (exit {rc})."
    try:
        return json.loads(out), None
    except json.JSONDecodeError as exc:
        return None, f"ERROR: gh returned invalid JSON: {exc}"


def _log_excerpt(log: str, limit: int = 8) -> tuple[list[str], int]:
    matches: list[str] = []
    seen: set[str] = set()
    context = 0
    for raw_line in log.splitlines():
        line = raw_line.split("\t", 3)[-1].strip()
        line = re.sub(r"^\d{4}-\d\d-\d\dT\S+Z\s*", "", line)
        if not line:
            continue
        is_error = bool(ERROR_RE.search(line))
        if "Command output for " in line:
            context = 8
        elif context:
            context -= 1
        if (not is_error and not context) or line == "------" or line in seen:
            continue
        seen.add(line)
        matches.append(line[:500])
    return matches[:limit], max(0, len(matches) - limit)


@mcp.tool()
def ci_pr_checks(pr: int, repo: str = "", max_checks: int = 40) -> str:
    """Return a compact production check summary for a pushed pull request.

    Args:
        pr: pull request number.
        repo: optional owner/repo; defaults to the origin remote.
        max_checks: check rows retained, capped at 100.
    """
    if pr < 1:
        return "ERROR: pr must be a positive integer."
    repository, error = _github_repo(repo)
    if error:
        return error
    data, error = _gh_json(
        [
            "pr",
            "checks",
            str(pr),
            "--repo",
            repository,
            "--json",
            "name,state,link,bucket,workflow",
        ]
    )
    if error:
        return error
    if not isinstance(data, list):
        return "ERROR: gh returned an unexpected check list."

    order = {"fail": 0, "pending": 1, "cancel": 2, "skipping": 3, "pass": 4}
    checks = sorted(
        data,
        key=lambda check: (
            order.get(check.get("bucket", ""), 5),
            check.get("name", ""),
        ),
    )
    max_checks = max(1, min(max_checks, 100))
    counts: dict[str, int] = {}
    for check in checks:
        bucket = check.get("bucket", "unknown")
        counts[bucket] = counts.get(bucket, 0) + 1
    rows = ["| result | check | workflow |", "|---|---|---|"]
    for check in checks[:max_checks]:
        bucket = check.get("bucket", "unknown")
        rows.append(
            f"| {bucket} | [{check.get('name', '')}]({check.get('link', '')}) | "
            f"{check.get('workflow', '')} |"
        )
    summary = ", ".join(f"{count} {bucket}" for bucket, count in sorted(counts.items()))
    omitted = len(checks) - max_checks
    if omitted > 0:
        rows.append(f"\n{omitted} additional checks omitted.")
    return f"### PR {pr} production checks\n\n{summary}\n\n" + "\n".join(rows)


@mcp.tool()
def ci_pr_failures(
    pr: int, repo: str = "", log_lines: int = 8, max_failures: int = 10
) -> str:
    """Return bounded diagnostics for failed production PR checks.

    Uses check-run annotations when available. Otherwise it reports failed
    steps and extracts only high-signal lines from the failed workflow log.

    Args:
        pr: pull request number.
        repo: optional owner/repo; defaults to the origin remote.
        log_lines: error lines retained per failed check, capped at 20.
        max_failures: failed checks retained, capped at 20.
    """
    if pr < 1:
        return "ERROR: pr must be a positive integer."
    repository, error = _github_repo(repo)
    if error:
        return error
    view, error = _gh_json(
        ["pr", "view", str(pr), "--repo", repository, "--json", "headRefOid,url"]
    )
    if error:
        return error
    if not isinstance(view, dict):
        return "ERROR: gh returned unexpected pull request metadata."
    checks, error = _gh_json(
        [
            "api",
            "-H",
            "Accept: application/vnd.github+json",
            f"repos/{repository}/commits/{view['headRefOid']}/check-runs?per_page=100",
        ]
    )
    if error:
        return error
    if not isinstance(checks, dict):
        return "ERROR: gh returned an unexpected check-run response."
    failed = [
        check
        for check in checks.get("check_runs", [])
        if check.get("conclusion") in FAILED_CONCLUSIONS
    ]
    if not failed:
        return f"PR {pr} has no failed production checks.\n\n{view['url']}"

    log_lines = max(1, min(log_lines, 20))
    max_failures = max(1, min(max_failures, 20))
    sections = [f"### PR {pr} production failures", ""]
    fetched_runs: set[str] = set()
    for check in failed[:max_failures]:
        sections.append(f"#### [{check['name']}]({check['details_url']})")
        annotations, annotation_error = _gh_json(
            [
                "api",
                "-H",
                "Accept: application/vnd.github+json",
                f"repos/{repository}/check-runs/{check['id']}/annotations?per_page=100",
            ]
        )
        failures = (
            [item for item in annotations if item.get("annotation_level") == "failure"]
            if not annotation_error and isinstance(annotations, list)
            else []
        )
        if failures:
            for annotation in failures[:log_lines]:
                location = annotation.get("path", "")
                if annotation.get("start_line"):
                    location += f":{annotation['start_line']}"
                message = annotation.get("message", "").strip()[:500]
                sections.append(f"- `{location}`: {message}")
            omitted_annotations = len(failures) - log_lines
            if omitted_annotations > 0:
                sections.append(
                    f"- {omitted_annotations} additional annotations omitted."
                )
            sections.append("")
            continue

        run_match = re.search(r"/actions/runs/(\d+)", check.get("details_url", ""))
        if not run_match or run_match.group(1) in fetched_runs:
            sections.extend(["- No check annotations available.", ""])
            continue
        run_id = run_match.group(1)
        fetched_runs.add(run_id)
        run, run_error = _gh_json(
            ["run", "view", run_id, "--repo", repository, "--json", "jobs"]
        )
        if not run_error and isinstance(run, dict):
            failed_steps = [
                step["name"]
                for job in run.get("jobs", [])
                for step in job.get("steps", [])
                if step.get("conclusion") == "failure"
            ]
            if failed_steps:
                sections.append("- Failed step: " + ", ".join(failed_steps[:5]))
        rc, log = _run_rc(
            ["gh", "run", "view", run_id, "--repo", repository, "--log-failed"],
            timeout=180,
        )
        excerpt, omitted_lines = _log_excerpt(log, log_lines) if rc == 0 else ([], 0)
        sections.extend(
            [f"- {line}" for line in excerpt]
            or ["- No concise error lines found; open the linked check."]
        )
        if omitted_lines:
            sections.append(f"- {omitted_lines} additional error lines omitted.")
        sections.append("")
    omitted_failures = len(failed) - max_failures
    if omitted_failures > 0:
        sections.append(f"{omitted_failures} additional failed checks omitted.")
    return "\n".join(sections).rstrip()


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
    body = _summarize_output("ci_test_pr", out, rc=rc)
    return f"{body}\n\n```\n{table}\n```" if table else body


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
def ci_check_ebpf(
    mode: str = "all", strict: bool = False, require_xdp: bool = False
) -> str:
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
    summary = _summarize_output(f"ci_task_{task.replace(':', '_')}", out, rc=rc)
    return f"**task {task}: {verdict}**\n{summary}"


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
