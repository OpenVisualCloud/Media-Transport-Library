# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""The framerate-token contract between the media tables and RxTxApp.

RxTxApp's JSON parser accepts eleven ``pXX`` tokens and rejects everything else,
exiting before ``mtl_init()``. The media tables store NTSC rates as fractions, so
a token built by interpolation -- ``f"p{info['fps']}"`` -- is ``'p5994/100'``:
rejected, no packet sent, and the failure surfaces ~90 s later as an empty packet
capture that names neither the config nor the framerate.
``parse_fps_to_pformat()`` is the only correct way to build the token for a table
that stores a rate.

The module-level check below is syntactic: it recognises the ``f"p{...}"`` shape,
which is the one the suite uses, not every way a token could be assembled. A
green tier says that shape is absent, not that no other could appear.

This tier runs on every PR to main with stock python3 and nothing installed, so
every check here reads either a dependency-free module (``media_files``) or
source text.
"""

import ast
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ACCEPTANCE = ROOT / "tests/acceptance"
PARSE_JSON_C = ROOT / "tests/tools/RxTxApp/src/parse_json.c"
sys.path.insert(0, str(ACCEPTANCE))

from mtl_engine import media_files  # noqa: E402

# Every session type whose fps field the config builders write, and the C
# function that parses it. All three accept the same eleven tokens.
PARSERS = ("parse_st20p_fps", "parse_st22p_fps", "parse_st40p_fps")

# Known debt, named rather than silently skipped: these two modules still build
# the token by interpolation, and their p29 and p59 rows are fractional (their
# p50 rows are not, so some of their cases do run). The single-host performance
# suite is being disabled and removed separately; this tuple goes with it.
UNCONVERTED = (
    "tests/single/performance/test_1tx_1nic_1port.py",
    "tests/single/performance/test_1tx_1rx_2nics_2ports.py",
)


def tables_with_a_fraction():
    """``{table name: [fps, ...]}`` for tables holding a non-integer rate.

    Every rate in such a table, not only its fractional ones: a converted call
    site hands the converter whichever row its parametrization picks.
    """
    tables = {}
    for name, value in vars(media_files).items():
        if name.startswith("_") or not isinstance(value, dict):
            continue
        rates = sorted(
            {
                str(entry["fps"])
                for entry in value.values()
                if isinstance(entry, dict) and "fps" in entry
            }
        )
        if any("/" in rate for rate in rates):
            tables[name] = rates
    return tables


def accepted_tokens():
    """The pXX tokens the C parsers accept, read out of parse_json.c."""
    source = PARSE_JSON_C.read_text()
    per_parser = {}
    for parser in PARSERS:
        body = re.search(rf"static int {parser}\(.*?\n\}}", source, re.S)
        assert body, f"{parser} not found in {PARSE_JSON_C}"
        per_parser[parser] = set(
            re.findall(r'strcmp\(fps, "(\w+)"\) == 0', body.group(0))
        )
    return per_parser


def interpolated_fps_tokens(module):
    """Line numbers in *module* where an fps token is built as ``f"p{...}"``."""
    tree = ast.parse(module.read_text())
    return [
        node.lineno
        for node in ast.walk(tree)
        if isinstance(node, ast.JoinedStr)
        and node.values
        and isinstance(node.values[0], ast.Constant)
        and node.values[0].value == "p"
        and any(isinstance(v, ast.FormattedValue) for v in node.values[1:])
    ]


class FpsTokenContractTests(unittest.TestCase):
    def setUp(self):
        self.fractional = tables_with_a_fraction()
        # Guards both cases below: they are vacuous if no fraction remains.
        self.assertTrue(
            self.fractional,
            "no fractional fps left in any media table -- these tests are moot",
        )

    def test_no_test_module_interpolates_a_fractional_tables_fps(self):
        """A module reading such a table must convert, never interpolate.

        Which entry a run picks is a parametrization detail, so a module that
        indexes only integer rows today is still one edit away from the empty
        capture. The rule is per module, not per row.
        """
        candidates = [
            module
            for module in sorted(ACCEPTANCE.glob("tests/**/test_*.py"))
            if any(
                re.search(rf"\b{table}\b", module.read_text())
                for table in self.fractional
            )
        ]
        self.assertTrue(candidates, "found no module reading a fractional table")
        readers = [
            module
            for module in candidates
            if not str(module.relative_to(ACCEPTANCE)).startswith(UNCONVERTED)
        ]
        self.assertEqual(
            [
                str(module.relative_to(ACCEPTANCE))
                for module in candidates
                if module not in readers
                and "parse_fps_to_pformat" in module.read_text()
            ],
            [],
            "an exemption covers a module that already converts, so widening it "
            "has stopped the rule being checked where it was applied",
        )
        offenders = {
            str(module.relative_to(ACCEPTANCE)): lines
            for module in readers
            if (lines := interpolated_fps_tokens(module))
        }
        self.assertEqual(
            offenders,
            {},
            "these build an fps token by interpolation, which yields "
            "'p5994/100' for an NTSC asset; call parse_fps_to_pformat() instead",
        )

    def test_the_converter_yields_a_token_the_c_parser_accepts(self):
        """What every converted call site now depends on."""
        for parser, tokens in accepted_tokens().items():
            for table, rates in self.fractional.items():
                for rate in rates:
                    with self.subTest(parser=parser, table=table, fps=rate):
                        self.assertIn(media_files.parse_fps_to_pformat(rate), tokens)


if __name__ == "__main__":
    unittest.main()
