# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""st20p/test_format.py runs FFmpeg only, so its assets must be readable.

FFmpeg sources the file with ``-f rawvideo -pix_fmt <file_format>``, so
file_format has to name a real AVPixelFormat. RFC 4175 is a transport packing,
not an AVPixelFormat, and ffmpeg_pix_fmt() raises for it -- which is what the
nightly hit, thirty times per NIC, before the case table was pointed at a table
FFmpeg can ingest.

This tier runs on every PR to main with stock python3 and nothing installed, so
the case table is read as source text rather than imported.
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


class FormatCaseTests(unittest.TestCase):
    def test_format_cases_are_ffmpeg_ingestible(self):
        # Read the assets as `<table>["<key>"]` subscripts instead of evaluating
        # the case table: that keeps working when a row gains a pytest.param(...)
        # wrapper or a marker.
        assets = [
            (ast.unparse(sub), getattr(media_files, sub.value.id)[sub.slice.value])
            for sub in ast.walk(ast.parse(FORMAT_TEST_PY.read_text()))
            if isinstance(sub, ast.Subscript)
            and isinstance(sub.value, ast.Name)
            and isinstance(sub.slice, ast.Constant)
            and isinstance(getattr(media_files, sub.value.id, None), dict)
        ]
        self.assertTrue(
            assets,
            f"found no media assets in {FORMAT_TEST_PY.name}; an emptied case "
            "table would otherwise satisfy this test vacuously",
        )
        for where, media_file in assets:
            with self.subTest(asset=where):
                # Raises ValueError for an unmappable format such as RFC 4175.
                ffmpeg_pix_fmt(media_file["file_format"])


if __name__ == "__main__":
    unittest.main()
