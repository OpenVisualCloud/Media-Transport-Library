#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Turn clang-tidy findings into SARIF 2.1.0 for GitHub code scanning.

usage: clang-tidy-sarif.py < findings > clang-tidy.sarif

Each finding is a "path:line:col: warning|error: message [check]" line with a
path relative to the repository root, as changed-lines.py keeps them; other
lines are skipped. No finding gives a run without results, which code scanning
reads as a clean analysis.
"""

import json
import re
import sys

FINDING = re.compile(r"(.+?):(\d+):(\d+): (warning|error): (.*?)(?: \[([^]]+)\])?$")


def main():
    results = []
    for line in sys.stdin:
        match = FINDING.match(line.rstrip("\n"))
        if not match:
            continue
        path, row, column, level, message, checks = match.groups()
        region = {"startLine": int(row), "startColumn": int(column)}
        results.append(
            {
                # a finding that several enabled checks share lists them all
                "ruleId": checks.split(",")[0] if checks else "clang-tidy",
                "level": level,
                "message": {"text": message},
                "locations": [
                    {
                        "physicalLocation": {
                            "artifactLocation": {"uri": path},
                            "region": region,
                        }
                    }
                ],
            }
        )
    driver = {
        "name": "clang-tidy",
        "informationUri": "https://clang.llvm.org/extra/clang-tidy/",
        "rules": [{"id": rule} for rule in sorted({r["ruleId"] for r in results})],
    }
    sarif = {
        "$schema": "https://json.schemastore.org/sarif-2.1.0.json",
        "version": "2.1.0",
        "runs": [{"tool": {"driver": driver}, "results": results}],
    }
    json.dump(sarif, sys.stdout, indent=2)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
