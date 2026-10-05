#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Run clang-tidy on the C and C++ files that changed since BASE.

usage: clang-tidy-changed.py CLANG_TIDY BASE

Each changed .c or .cpp file runs with the compile command of the first build,
in path order, that compiles it (build/ before build_unit/) and the checks of
.clang-tidy; a changed header runs through a file that includes it. Warnings
and errors, in the file and in the headers it includes, go to stdout as
"path:line:col: ...", with an absolute path, for changed-lines.py. A changed
file that no build compiles is listed on stderr. Exits 1 when clang-tidy can't
process a file, so the file can't pass unchecked.
"""

import json
import re
import shlex
import subprocess  # nosec B404 # runs git and clang-tidy via argv lists, no shell
import sys
import tempfile
from pathlib import Path

DIAGNOSTIC = re.compile(r"(.+?):\d+:\d+: (?:warning|error): ")
# gcc options that the builds add when gcc takes them and that clang rejects
GCC_ONLY = {"-flarge-source-files"}


def git(*args):
    return subprocess.run(
        ["git", "-c", "core.quotePath=false", *args],
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def including_file(header, builds):
    """A compiled file that includes a header with the name of HEADER."""
    pattern = re.compile(
        r'#\s*include\s*[<"]([^>"]*/)?' + re.escape(header.name) + r'[>"]'
    )
    for source in sorted(builds):
        if source.is_file() and pattern.search(source.read_text(errors="replace")):
            return source
    return None


def run(clang_tidy, entry):
    """clang-tidy on ENTRY of a compile database, without the GCC_ONLY options."""
    entry = dict(entry)
    if "arguments" in entry:
        entry["arguments"] = [a for a in entry["arguments"] if a not in GCC_ONLY]
    else:
        entry["command"] = shlex.join(
            a for a in shlex.split(entry["command"]) if a not in GCC_ONLY
        )
    with tempfile.TemporaryDirectory() as database:
        (Path(database) / "compile_commands.json").write_text(json.dumps([entry]))
        return subprocess.run(
            [
                clang_tidy,
                "--quiet",
                "--header-filter=.*",
                "--extra-arg=-Wno-error",
                "-p",
                database,
                str(Path(entry["directory"]) / entry["file"]),
            ],
            capture_output=True,
            text=True,
        )


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    clang_tidy, base = sys.argv[1:]
    root = Path(git("rev-parse", "--show-toplevel").strip()).resolve()
    changed = git(
        "diff", "-M", "--name-only", "--diff-filter=AMR", base, "HEAD", "--"
    ).splitlines()
    builds = {}
    for database in sorted(root.rglob("compile_commands.json")):
        for entry in json.loads(database.read_text()):
            builds.setdefault(
                (Path(entry["directory"]) / entry["file"]).resolve(), entry
            )
    sources = set()
    for name in changed:
        path = (root / name).resolve()
        if path.suffix in (".c", ".cpp"):
            source = path if path in builds else None
        elif path.suffix in (".h", ".hpp"):
            source = including_file(path, builds)
        else:
            continue
        if source is None:
            print(f"{name}: no build compiles it, not checked", file=sys.stderr)
        else:
            sources.add(source)
    status = 0
    for source in sorted(sources):
        entry = builds[source]
        result = run(clang_tidy, entry)
        # clang-tidy prints a path as the compile command has it, which can
        # be relative to the build directory
        for line in result.stdout.splitlines():
            match = DIAGNOSTIC.match(line)
            if match:
                print(
                    f"{Path(entry['directory']) / match.group(1)}{line[match.end(1) :]}"
                )
        if result.returncode != 0:
            # including errors without a location, such as a rejected option
            errors = "\n".join(
                line for line in result.stdout.splitlines() if "error:" in line
            )
            print(
                f"{source}: clang-tidy failed with {result.returncode}:\n{errors}\n{result.stderr}",
                file=sys.stderr,
            )
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
