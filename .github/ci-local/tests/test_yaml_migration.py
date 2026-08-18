#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[3]
ACTIVE_DIRS = (ROOT / ".github/workflows", ROOT / ".github/actions")


def active_yaml():
    for directory in ACTIVE_DIRS:
        yield from directory.rglob("*.yml")
        yield from directory.rglob("*.yaml")


def walk(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk(child)


def main():
    taskfile = yaml.safe_load((ROOT / "Taskfile.yml").read_text(encoding="utf-8"))
    tasks = set(taskfile["tasks"])
    errors = []

    required_tasks = {
        "ci:hash-dependencies",
        "ci:build-dependencies",
        "ci:validate-dependencies",
        "ci:build-jpegxs",
        "ci:validate-jpegxs",
        "ci:build-ice",
        "ci:validate-ice",
        "ci:activate-ice",
        "ci:validate-host",
    }
    for task in sorted(required_tasks - tasks):
        errors.append(f"Taskfile.yml: required task {task} is missing")

    build_workflow = yaml.safe_load(
        (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
    )
    if build_workflow["jobs"]["build"]["runs-on"] != "e835":
        errors.append(
            ".github/workflows/build.yml: ICE producer must run on the validation fleet"
        )

    pr_gate_workflow = yaml.load(
        (ROOT / ".github/workflows/pr-gate.yml").read_text(encoding="utf-8"),
        Loader=yaml.BaseLoader,
    )
    wait_job = pr_gate_workflow["jobs"]["wait-for-build"]
    wait_step = next(
        step
        for step in wait_job["steps"]
        if step.get("uses") == "./.github/actions/wait-for-workflow"
    )
    wait_action = yaml.load(
        (ROOT / ".github/actions/wait-for-workflow/action.yml").read_text(
            encoding="utf-8"
        ),
        Loader=yaml.BaseLoader,
    )
    wait_seconds = int(
        wait_step.get("with", {}).get(
            "timeout", wait_action["inputs"]["timeout"]["default"]
        )
    )
    wait_job_seconds = int(wait_job["timeout-minutes"]) * 60
    if wait_seconds != 600:
        errors.append(
            ".github/workflows/pr-gate.yml: Build execution wait must be 10 minutes"
        )
    if wait_job_seconds != 1200:
        errors.append(
            ".github/workflows/pr-gate.yml: wait-for-build job must have a 20-minute outer cap"
        )

    for path in active_yaml():
        source = path.read_text(encoding="utf-8")
        document = yaml.safe_load(source)
        for match in re.finditer(r"run:\s+task\s+(ci:[\w-]+)", source):
            if match.group(1) not in tasks:
                errors.append(
                    f"{path.relative_to(ROOT)}: unknown task {match.group(1)}"
                )
        for node in walk(document):
            script = node.get("script")
            if isinstance(script, str) and "require(" in script and "github" in script:
                if (
                    "return require(" not in script
                    or "core" not in script
                    or "context" not in script
                ):
                    errors.append(
                        f"{path.relative_to(ROOT)}: invalid github-script module loader"
                    )

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("YAML task/module mappings: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
