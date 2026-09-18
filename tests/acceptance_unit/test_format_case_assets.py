# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""What st20p/test_format.py runs must be readable by FFmpeg, and must run at all.

FFmpeg sources that test's assets with ``-f rawvideo -pix_fmt <file_format>``. A
file_format that ffmpeg_pix_fmt() cannot map -- RFC 4175, which is a transport
packing rather than an AVPixelFormat -- aborts the case while the command is still
being built.

The remaining gates cover the module's leg table. Application.set_params() raises for
a kwarg outside UNIVERSAL_PARAMS, so a kwarg only one adapter accepts belongs on that
adapter's row rather than in the call every leg makes; and a leg that cannot run says
so on its row, where these gates can see it, rather than from the test body.

Every running case row is held to FFmpeg's requirement even though only two of the
three legs are FFmpeg, because one table feeds them all and so has to satisfy its
strictest consumer. A row that declares its own skip is a recorded coverage gap, not a
case that runs, so it is exempt. A row in any other shape is a gate failure rather than
a silent pass: that is what keeps a partial rewrite of a table from passing vacuously.

This tier runs with stock python3 and nothing installed, so both modules are read as
source text rather than imported.
"""

import ast
import sys
import unittest
from pathlib import Path

ACCEPTANCE = Path(__file__).resolve().parents[1] / "acceptance"
FORMAT_TEST_PY = ACCEPTANCE / "tests/single/st20p/test_format.py"
FFMPEG_PY = ACCEPTANCE / "mtl_engine/ffmpeg.py"
# Parsed once: every gate below reads the same two modules.
FORMAT_TREE = ast.parse(FORMAT_TEST_PY.read_text())
FFMPEG_TREE = ast.parse(FFMPEG_PY.read_text())
sys.path.insert(0, str(ACCEPTANCE))

from mtl_engine import media_files  # noqa: E402
from mtl_engine.config.mappings import ffmpeg_pix_fmt  # noqa: E402
from mtl_engine.config.universal_params import UNIVERSAL_PARAMS  # noqa: E402

# The two mark sets differ on purpose. A skipif row may still run, so it has to satisfy
# the ingestibility check; by the same token it cannot be counted on to run, so it
# cannot stand in for the leg this module requires.
NEVER_RUNS = {"skip"}
MAY_NOT_RUN = {"skip", "skipif", "xfail"}


def _parametrized(name):
    """True if a parametrize decorator takes the table itself, not a copy of it."""
    return any(
        isinstance(arg, ast.Name) and arg.id == name
        for node in ast.walk(FORMAT_TREE)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr == "parametrize"
        for arg in node.args
    )


def _table(name):
    """Every row the module parametrizes over, across all bindings of one table.

    Reading only the first binding would let a later ``<name> += [...]`` add rows
    this gate never sees, so both assignment forms are collected. A table the
    decorator no longer takes by name is a gate failure too: a slice or a literal
    list there leaves these gates reading rows nothing collects.
    """
    rows = []
    for node in ast.walk(FORMAT_TREE):
        if isinstance(node, ast.Assign):
            targets = node.targets
        elif isinstance(node, ast.AugAssign):
            targets = [node.target]
        else:
            continue
        if not any(isinstance(t, ast.Name) and t.id == name for t in targets):
            continue
        if not isinstance(node.value, ast.List):
            raise AssertionError(
                f"this gate reads {name} as source text, so every binding of it "
                f"must be a list literal; got {type(node.value).__name__} in "
                f"{FORMAT_TEST_PY.name}"
            )
        rows += node.value.elts
    if not rows:
        raise AssertionError(f"no {name} list literal in {FORMAT_TEST_PY.name}")
    if not _parametrized(name):
        raise AssertionError(
            f"no parametrize decorator in {FORMAT_TEST_PY.name} takes {name} itself, "
            f"so these gates would read rows that no case is generated from"
        )
    return rows


def _marks(element):
    """The pytest mark names a row carries, e.g. ``{"skip"}`` or ``{"skipif"}``.

    ``marks`` takes a single decorator or any collection of them, so all three
    sequence literals are unwrapped: reading only a list would leave a row marked
    ``marks=(pytest.mark.skip(...),)`` looking unmarked to both gates below.
    """
    if not isinstance(element, ast.Call):
        return set()
    names = set()
    for kw in element.keywords:
        if kw.arg != "marks":
            continue
        sequences = (ast.List, ast.Tuple, ast.Set)
        marks = kw.value.elts if isinstance(kw.value, sequences) else [kw.value]
        for mark in marks:
            func = mark.func if isinstance(mark, ast.Call) else mark
            if isinstance(func, ast.Attribute):
                names.add(func.attr)
    return names


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


def _adapter_kwargs(application):
    """The kwarg names one adapter accepts: the universal set plus its own keys.

    Only FFmpeg's private keys are modelled, because it is the only adapter this
    module's legs pass one to. An adapter that grows a private kwarg of its own has
    to be added here before a leg row may carry it.
    """
    if application != "ffmpeg":
        return set(UNIVERSAL_PARAMS)
    classes = [
        node
        for node in ast.walk(FFMPEG_TREE)
        if isinstance(node, ast.ClassDef) and node.name == "FFmpeg"
    ]
    for node in ast.walk(classes[0]) if classes else []:
        if isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id == "_FFMPEG_ONLY_KEYS"
            for t in node.targets
        ):
            if not isinstance(node.value, ast.Tuple):
                raise AssertionError(
                    f"this gate reads _FFMPEG_ONLY_KEYS as source text, so it must "
                    f"stay a tuple literal; got {type(node.value).__name__} in "
                    f"{FFMPEG_PY.name}"
                )
            return set(UNIVERSAL_PARAMS) | {e.value for e in node.value.elts}
    raise AssertionError(f"no FFmpeg._FFMPEG_ONLY_KEYS tuple in {FFMPEG_PY.name}")


def _leg(element):
    """The application a leg row names and the kwargs it splats for that adapter."""
    args = element.args if isinstance(element, ast.Call) else []
    if len(args) < 2 or not isinstance(args[0], ast.Constant):
        raise AssertionError(
            f"this gate reads the leg table as source text, so every row must be "
            f"pytest.param(<application>, <kwargs>, ...); got {ast.unparse(element)}"
        )
    kwargs = args[1]
    if not isinstance(kwargs, ast.Dict) or not all(
        isinstance(key, ast.Constant) for key in kwargs.keys
    ):
        raise AssertionError(
            f"a leg row's kwargs must be a dict literal with named keys, so this gate "
            f"can see which leg gets what; got {ast.unparse(element)}"
        )
    return args[0].value, {key.value for key in kwargs.keys}


def _create_command_kwargs():
    """Kwarg names test_format.py names literally in a create_command() call.

    A ``**mapping`` argument is deliberately not followed: that is the form the test
    uses to pass an adapter-private kwarg to one leg only.
    """
    calls = [
        node
        for node in ast.walk(FORMAT_TREE)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr == "create_command"
    ]
    if not calls:
        raise AssertionError(f"no create_command() call in {FORMAT_TEST_PY.name}")
    return {kw.arg for call in calls for kw in call.keywords if kw.arg}


class FormatCaseTests(unittest.TestCase):
    def test_running_format_cases_are_ffmpeg_ingestible(self):
        checked = 0
        for element in _table("FORMAT_CASES"):
            where = ast.unparse(element)
            if _marks(element) & NEVER_RUNS:
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

    def test_each_leg_gets_only_kwargs_its_adapter_accepts(self):
        for element in _table("APPLICATION_LEGS"):
            application, kwargs = _leg(element)
            rejected = sorted(kwargs - _adapter_kwargs(application))
            with self.subTest(leg=ast.unparse(element)):
                self.assertFalse(
                    rejected,
                    f"the {application} leg is handed {rejected}, which neither "
                    f"UNIVERSAL_PARAMS nor that adapter's modelled keys carry, so "
                    f"every {application} case would raise. Either the kwarg is "
                    f"misspelled, or it belongs to another adapter.",
                )

    def test_an_rxtxapp_leg_runs(self):
        running = set()
        for element in _table("APPLICATION_LEGS"):
            application, _ = _leg(element)
            if not _marks(element) & MAY_NOT_RUN:
                running.add(application)
        self.assertIn(
            "rxtxapp",
            running,
            f"{FORMAT_TEST_PY.name} has no RxTxApp leg that runs; that is the "
            "coverage hole this gate exists to keep closed",
        )

    def test_adapter_private_kwargs_stay_out_of_the_shared_call(self):
        shared = sorted(_create_command_kwargs() - set(UNIVERSAL_PARAMS))
        self.assertFalse(
            shared,
            f"{FORMAT_TEST_PY.name} names {shared} in the create_command() call, "
            "where every leg gets them. A kwarg outside UNIVERSAL_PARAMS is either "
            "misspelled, or belongs on the row of the leg whose adapter accepts it.",
        )

    def test_no_leg_is_skipped_from_the_test_body(self):
        """A leg's gap is declared on its row, where these gates can see it.

        Only a skip on the application alone is a gate failure. A condition mixing it
        with another parameter axis has no row to live on -- one cell of a
        cross-product is not a row -- and neither has a run-time skip for a missing
        encoder or a one-interface host.
        """

        def _skips(node):
            for sub in ast.walk(node):
                if not isinstance(sub, ast.Call):
                    continue
                func = sub.func
                if isinstance(func, ast.Attribute) and func.attr == "skip":
                    return True
                if isinstance(func, ast.Name) and func.id == "skip":
                    return True
            return False

        def _constants(node):
            """A constant, or a sequence literal of them -- `in ("a", "b")` counts."""
            if isinstance(node, (ast.List, ast.Set, ast.Tuple)):
                return all(isinstance(e, ast.Constant) for e in node.elts)
            return isinstance(node, ast.Constant)

        def _on_application_alone(test):
            return (
                isinstance(test, ast.Compare)
                and isinstance(test.left, ast.Name)
                and test.left.id == "application"
                and all(_constants(c) for c in test.comparators)
            )

        hidden = [
            ast.unparse(node.test)
            for node in ast.walk(FORMAT_TREE)
            if isinstance(node, ast.If)
            and _on_application_alone(node.test)
            and _skips(node)
        ]
        self.assertFalse(
            hidden,
            f"{FORMAT_TEST_PY.name} skips a whole leg at run time on {hidden}. "
            "Declare the gap on that leg's row instead, or stop generating the case.",
        )


if __name__ == "__main__":
    unittest.main()
