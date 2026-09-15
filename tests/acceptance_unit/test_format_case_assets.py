# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Every case st20p/test_format.py actually runs must be readable by FFmpeg.

That test's RxTxApp leg is skipped, so FFmpeg is the only consumer of its assets,
and FFmpeg sources them with ``-f rawvideo -pix_fmt <file_format>``. A file_format
that ffmpeg_pix_fmt() cannot map -- RFC 4175, which is a transport packing rather
than an AVPixelFormat -- aborts the case while the command is still being built.

A row that declares its own skip is a recorded coverage gap, not a case that runs,
so it is exempt. A row in any other shape is a gate failure rather than a silent
pass: that is what keeps a partial rewrite of the table from passing vacuously.

This tier runs with stock python3 and nothing installed, so the table is read as
source text rather than imported.
"""

import ast
import sys
import unittest
from pathlib import Path

ACCEPTANCE = Path(__file__).resolve().parents[1] / "acceptance"
FORMAT_TEST_PY = ACCEPTANCE / "tests/single/st20p/test_format.py"
sys.path.insert(0, str(ACCEPTANCE))

from mtl_engine import media_files  # noqa: E402
from mtl_engine.config.mappings import ffmpeg_pix_fmt  # noqa: E402


def _case_table():
    """Every row bound into test_format.py's FORMAT_CASES, across all bindings.

    Reading only the first binding would let a later ``FORMAT_CASES += [...]`` add
    rows this gate never sees, so both assignment forms are collected.
    """
    rows = []
    for node in ast.walk(ast.parse(FORMAT_TEST_PY.read_text())):
        if isinstance(node, ast.Assign):
            targets = node.targets
        elif isinstance(node, ast.AugAssign):
            targets = [node.target]
        else:
            continue
        if not any(isinstance(t, ast.Name) and t.id == "FORMAT_CASES" for t in targets):
            continue
        if not isinstance(node.value, ast.List):
            raise AssertionError(
                f"this gate reads FORMAT_CASES as source text, so every binding of it "
                f"must be a list literal; got {type(node.value).__name__} in "
                f"{FORMAT_TEST_PY.name}"
            )
        rows += node.value.elts
    if not rows:
        raise AssertionError(f"no FORMAT_CASES list literal in {FORMAT_TEST_PY.name}")
    return rows


def _declares_skip(element):
    """True if a row skips itself unconditionally, so it never reaches FFmpeg.

    Matched on the mark's own name: ``skipif`` is a condition evaluated at run time,
    and a row carrying one still runs whenever that condition is false.
    """
    if not isinstance(element, ast.Call):
        return False
    for kw in element.keywords:
        if kw.arg != "marks":
            continue
        marks = kw.value.elts if isinstance(kw.value, ast.List) else [kw.value]
        for mark in marks:
            func = mark.func if isinstance(mark, ast.Call) else mark
            if isinstance(func, ast.Attribute) and func.attr == "skip":
                return True
    return False


def _media_asset(element):
    """The media table entry a case row passes to the media_file fixture."""
    found = [
        getattr(media_files, sub.value.id)[sub.slice.value]
        for sub in ast.walk(element)
        if isinstance(sub, ast.Subscript)
        and isinstance(sub.value, ast.Name)
        and isinstance(sub.slice, ast.Constant)
        and isinstance(getattr(media_files, sub.value.id, None), dict)
    ]
    return found[0] if len(found) == 1 else None


class FormatCaseTests(unittest.TestCase):
    def test_running_format_cases_are_ffmpeg_ingestible(self):
        checked = 0
        for element in _case_table():
            where = ast.unparse(element)
            if _declares_skip(element):
                continue
            asset = _media_asset(element)
            self.assertIsNotNone(
                asset,
                f"cannot tell which media asset this row uses, so it cannot be "
                f'checked; a row must name exactly one <table>["<key>"] entry '
                f"from media_files.py: {where}",
            )
            with self.subTest(case=where):
                # Raises ValueError for a format FFmpeg has no mapping for.
                ffmpeg_pix_fmt(asset["file_format"])
            checked += 1
        self.assertTrue(
            checked,
            f"no running case left in {FORMAT_TEST_PY.name}; an emptied or "
            "entirely skipped table would otherwise satisfy this test vacuously",
        )


if __name__ == "__main__":
    unittest.main()
