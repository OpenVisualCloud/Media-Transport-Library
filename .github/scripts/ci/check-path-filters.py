#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Check .github/path_filters.yml against the workflows and the hash sources.

The rules are in the header of .github/path_filters.yml. Exit 1 on a fault.
"""

import re
import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[3]
FILTERS = ROOT / ".github/path_filters.yml"
WORKFLOWS = ROOT / ".github/workflows"
PR_GATE = "./.github/workflows/pr-gate.yml"
BUILD_FILTER = "build_workflow"
DORNY = "dorny/paths-filter"
OUTPUT_RE = re.compile(r"steps\.filter\.outputs\.([A-Za-z0-9_]+)")
# Glob syntax that to_regex does not translate: braces, classes, extglobs, a
# negation, and a '**' that is not a whole path segment.
UNSUPPORTED = re.compile(r"[{}\[\]()!]|[^/]\*\*|\*\*[^/]")


def flatten(items):
    for item in items:
        if isinstance(item, list):
            yield from flatten(item)
        else:
            yield item


def to_regex(pattern):
    """Translate a picomatch glob (dorny sets dot: true) to a regex.

    Only '*', '?' and a '**' path segment are translated. main rejects a
    pattern that UNSUPPORTED matches before it gets here.
    """
    out, i = [], 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        elif pattern[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


def env_patterns(env_file, faults):
    """Return the patterns that one hash_sources_<name>.env file gives.

    script/hash_sources.sh reads each line as one path and gives it to find
    unquoted, so a line with a space, a glob character or a comment after the
    path does not give the path that it shows.
    """
    patterns = set()
    for number, line in enumerate(read(env_file).split("\n"), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line != line.strip() or re.search(r"[\s#*?\[\]{}\r]", line):
            faults.append(f"{env_file.name}:{number}: {line!r} is not a plain path")
            continue
        patterns.add(line.rstrip("/") + "/**" if line.endswith("/") else line)
    return patterns


def read(path):
    return path.read_bytes().decode("utf-8")


def main():
    faults = []
    filters = {
        name: list(flatten(items))
        for name, items in yaml.safe_load(read(FILTERS)).items()
    }
    for name, items in filters.items():
        for item in items:
            if not isinstance(item, str):
                faults.append(f"{name}: {item!r} is not a path pattern")
        filters[name] = [item for item in items if isinstance(item, str)]
    tracked = subprocess.run(
        ["git", "ls-files"], cwd=ROOT, check=True, capture_output=True, text=True
    ).stdout.splitlines()

    # Each pattern matches a tracked file.
    for name, patterns in filters.items():
        for pattern in patterns:
            if UNSUPPORTED.search(pattern):
                faults.append(f"{name}: '{pattern}' uses unsupported glob syntax")
                continue
            regex = to_regex(pattern)
            if not any(regex.match(path) for path in tracked):
                faults.append(f"{name}: '{pattern}' matches no tracked file")

    # hash_<name> holds the paths of script/hash_sources_<name>.env, one to one.
    hash_names = []
    for env_file in sorted((ROOT / "script").glob("hash_sources_*.env")):
        name = "hash_" + env_file.stem.removeprefix("hash_sources_")
        hash_names.append(name)
        if name not in filters:
            faults.append(f"{env_file.name}: no filter {name}")
            continue
        want, have = env_patterns(env_file, faults), set(filters[name])
        for pattern in sorted(want - have):
            faults.append(f"{name}: '{pattern}' is in {env_file.name}, not here")
        for pattern in sorted(have - want):
            faults.append(f"{name}: '{pattern}' is not in {env_file.name}")

    union = {p for name in hash_names for p in filters.get(name, [])}
    if set(filters.get("hash_sources", [])) != union:
        faults.append("hash_sources: not the union of the hash_<name> filters")

    # Each filter that a workflow reads exists. build_workflow holds each
    # filter that pr-gate.yml gets.
    gated = set()
    workflows = sorted(WORKFLOWS.glob("*.yml")) + sorted(WORKFLOWS.glob("*.yaml"))
    for workflow in workflows:
        text = read(workflow)
        for name in OUTPUT_RE.findall(text):
            if name not in filters:
                faults.append(f"{workflow.name}: reads filter {name}, not defined")
        for job in (yaml.safe_load(text).get("jobs") or {}).values():
            for step in job.get("steps") or []:
                uses = step.get("uses") or ""
                if uses.startswith(DORNY) and step.get("id") != "filter":
                    faults.append(f"{workflow.name}: a {DORNY} step has no id 'filter'")
            if job.get("uses") == PR_GATE:
                name = (job.get("with") or {}).get("filter")
                if name not in filters:
                    faults.append(
                        f"{workflow.name}: pr-gate filter {name}, not defined"
                    )
                else:
                    gated.add(name)

    build = set(filters.get(BUILD_FILTER, []))
    for name in sorted(gated):
        for pattern in sorted(set(filters[name]) - build):
            faults.append(f"{BUILD_FILTER}: no '{pattern}' of {name}")

    for fault in faults:
        print(f"path_filters.yml: {fault}", file=sys.stderr)
    return 1 if faults else 0


if __name__ == "__main__":
    sys.exit(main())
