#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Keep the findings on lines that a change adds or modifies.

usage: changed-lines.py BASE < findings

Each finding is a line that starts with "path:line:" or "path:first-last:", as
compilers and clang-tidy print them. A path is
normalized, and an absolute one is taken relative to the repository root. The
changed lines are those that `git diff -U0 BASE HEAD` adds. Prints the findings
it keeps, with that relative path, and exits 1 if it keeps any.
"""

import os
import re
import subprocess  # nosec B404 # runs git via argv lists, no shell
import sys

HUNK = re.compile(r"@@ -\S+ \+(\d+)(?:,(\d+))? @@")
FINDING = re.compile(r"(?:\./)?(.+?):(\d+)(?:-(\d+))?:")


def git(*args):
    return subprocess.run(
        ["git", "-c", "core.quotePath=false", *args],
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def changed_lines(base):
    lines = {}
    path = None
    for line in git("diff", "-U0", "-M", "--no-color", base, "HEAD").splitlines():
        if line.startswith("+++ "):
            # git ends a path that holds a space with a tab
            path = line[6:].rstrip("\t") if line.startswith("+++ b/") else None
        elif line.startswith("@@") and path:
            start, count = HUNK.match(line).groups()
            start, count = int(start), int(count or 1)
            lines.setdefault(path, set()).update(range(start, start + count))
    return lines


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    lines = changed_lines(sys.argv[1])
    root = git("rev-parse", "--show-toplevel").strip()
    kept = []
    for finding in sys.stdin:
        match = FINDING.match(finding)
        if not match:
            continue
        path = os.path.normpath(match.group(1))
        if os.path.isabs(path):
            path = os.path.relpath(path, root)
        first = int(match.group(2))
        last = int(match.group(3) or first)
        if lines.get(path, set()).intersection(range(first, last + 1)):
            kept.append(path + finding[match.end(1) :].rstrip("\n"))
    for finding in kept:
        print(finding)
    return 1 if kept else 0


if __name__ == "__main__":
    sys.exit(main())
