#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
# Enforces the rules in the header of .github/path_filters.yml.

import re
import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[3]
GLOB = {"**/": "(?:.*/)?", "**": ".*", "*": "[^/]*", "?": "[^/]"}


def flatten(items):
    for item in items:
        yield from flatten(item) if isinstance(item, list) else [item]


def to_regex(pattern):
    parts = re.split(r"(\*\*/|\*\*|\*|\?)", pattern)
    return re.compile("".join(GLOB.get(p, re.escape(p)) for p in parts) + r"\Z")


faults = []
filters = {}
for name, items in yaml.safe_load(
    (ROOT / ".github/path_filters.yml").read_bytes()
).items():
    filters[name] = []
    for item in flatten(items):
        if isinstance(item, str):
            filters[name].append(item)
        else:
            faults.append(f"{name}: {item!r} is not a path pattern")
tracked = subprocess.run(
    ["git", "ls-files"], cwd=ROOT, check=True, capture_output=True, text=True
).stdout.splitlines()

for name, patterns in filters.items():
    for pattern in patterns:
        if pattern.startswith("script/") and re.search(r"[*?]", pattern):
            faults.append(f"{name}: '{pattern}' is a glob in script/, name each file")
        if re.search(r"[{}\[\]()!]|[^/]\*\*|\*\*[^/]", pattern):
            faults.append(f"{name}: '{pattern}' uses unsupported glob syntax")
            continue
        if not any(to_regex(pattern).match(path) for path in tracked):
            faults.append(f"{name}: '{pattern}' matches no tracked file")

scripts = {path for path in tracked if re.fullmatch(r"script/[^/]+\.sh", path)}
scripts.add(".github/scripts/setup_environment.sh")
for path in sorted(scripts - set(filters.get("scripts", []))):
    faults.append(f"scripts: no '{path}'")

union = set()
for env_file in sorted((ROOT / "script").glob("hash_sources_*.env")):
    name = "hash_" + env_file.stem.removeprefix("hash_sources_")
    if name not in filters:
        faults.append(f"{env_file.name}: no filter {name}")
        continue
    want, have = set(), set(filters[name])
    union |= have
    lines = env_file.read_bytes().decode().split("\n")
    for number, line in enumerate(lines, 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line != line.strip() or re.search(r"[\s#*?\[\]{}\r]", line):
            faults.append(f"{env_file.name}:{number}: {line!r} is not a plain path")
            continue
        want.add(line.rstrip("/") + "/**" if line.endswith("/") else line)
    for pattern in sorted(want - have):
        faults.append(f"{name}: '{pattern}' is in {env_file.name}, not here")
    for pattern in sorted(have - want):
        faults.append(f"{name}: '{pattern}' is not in {env_file.name}")
if set(filters.get("hash_sources", [])) != union:
    faults.append("hash_sources: not the union of the hash_<name> filters")

gated = set()
workflows = ROOT / ".github/workflows"
for workflow in sorted(workflows.glob("*.yml")) + sorted(workflows.glob("*.yaml")):
    text = workflow.read_bytes().decode()
    for name in re.findall(r"steps\.filter\.outputs\.([A-Za-z0-9_]+)", text):
        if name not in filters:
            faults.append(f"{workflow.name}: reads filter {name}, not defined")
    for job in (yaml.safe_load(text).get("jobs") or {}).values():
        for step in job.get("steps") or []:
            uses = step.get("uses") or ""
            if uses.startswith("dorny/paths-filter") and step.get("id") != "filter":
                faults.append(
                    f"{workflow.name}: a dorny/paths-filter step has no id 'filter'"
                )
        if job.get("uses") == "./.github/workflows/pr-gate.yml":
            name = (job.get("with") or {}).get("filter")
            if name in filters:
                gated.add(name)
            else:
                faults.append(f"{workflow.name}: pr-gate filter {name}, not defined")

for name in sorted(gated):
    for pattern in sorted(set(filters[name]) - set(filters.get("build_workflow", []))):
        faults.append(f"build_workflow: no '{pattern}' of {name}")

for fault in faults:
    print(f"path_filters.yml: {fault}", file=sys.stderr)
sys.exit(1 if faults else 0)
