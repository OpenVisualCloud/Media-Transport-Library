# Copyright 2026 Intel Corporation
# SPDX-License-Identifier: BSD-3-Clause

"""Make the list of the fuzz targets from the build file and the harnesses.

The ``fuzz-targets`` directive reads the ``fuzz_targets`` list of
``tests/fuzz/meson.build``. For each target, it reads the first ``/** */``
comment of the harness source and the production ``.c`` file that the harness
includes. It writes a summary table and one section for each target. A new
harness thus gets its section with no change to the documentation.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from docutils import nodes
from docutils.statemachine import StringList
from sphinx.util.docutils import SphinxDirective
from sphinx.util.nodes import nested_parse_with_titles

# This file is tests/doc/_ext/fuzz_targets.py.
ROOT_DIR = Path(__file__).resolve().parents[3]
FUZZ_DIR = ROOT_DIR / "tests" / "fuzz"
BUILD_FILE = FUZZ_DIR / "meson.build"
# The include directories of tests/fuzz/meson.build.
INCLUDE_DIRS = (ROOT_DIR / "lib" / "src", ROOT_DIR / "include")

# One ['name', 'dir/file.c'] entry of the fuzz_targets list.
TARGET_RE = re.compile(r"\[\s*'(\w+)'\s*,\s*'([^']+\.c)'\s*\]")
COMMENT_RE = re.compile(r"/\*\*(.*?)\*/", re.S)
INCLUDE_RE = re.compile(r'^#include "([^"]+\.c)"', re.M)
# A C name with an underscore, or a function call, for example "struct st_hdr".
IDENT_RE = re.compile(r"\b((?:struct )?[A-Za-z]\w*_\w*(?:\(\))?)")


class FuzzTargetError(Exception):
    """A fault in tests/fuzz that stops the list."""


@dataclass
class FuzzTarget:
    name: str
    harness: Path
    # The paragraphs of the file comment, as reStructuredText.
    paragraphs: list[str]
    # The production .c files that the harness includes.
    code: list[Path]


def target_sources() -> list[tuple[str, Path]]:
    """Return the name and the harness source of each fuzz_targets entry."""
    if not BUILD_FILE.is_file():
        raise FuzzTargetError(f"{BUILD_FILE} does not exist")
    entries = TARGET_RE.findall(BUILD_FILE.read_text(encoding="utf-8"))
    if not entries:
        raise FuzzTargetError(f"{BUILD_FILE} has no fuzz_targets entry")
    return [(name, FUZZ_DIR / source) for name, source in entries]


def read_target(name: str, harness: Path) -> FuzzTarget:
    if not harness.is_file():
        raise FuzzTargetError(f"{name} has no source {harness}")
    source = harness.read_text(encoding="utf-8")
    paragraphs = [_literal(p) for p in _paragraphs(source)]
    if not paragraphs:
        raise FuzzTargetError(f"{harness} has no /** */ file comment")
    code = [_find(include, harness.parent) for include in INCLUDE_RE.findall(source)]
    return FuzzTarget(name, harness, paragraphs, [path for path in code if path])


def _paragraphs(source: str) -> list[str]:
    """Return the paragraphs of the first /** */ comment, without @file."""
    match = COMMENT_RE.search(source)
    if not match:
        return []
    lines = []
    for line in match.group(1).splitlines():
        line = re.sub(r"^\s*\*? ?", "", line).rstrip()
        if not line.startswith("@file"):
            lines.append(line)
    text = "\n".join(lines).strip()
    return [" ".join(p.split()) for p in re.split(r"\n\s*\n", text) if p.strip()]


def _literal(text: str) -> str:
    return IDENT_RE.sub(r"``\1``", text)


def _find(include: str, harness_dir: Path) -> Path | None:
    """Return the file that the compiler takes for #include "include"."""
    for base in (harness_dir, *INCLUDE_DIRS):
        path = base / include
        if path.is_file():
            return path.resolve()
    return None


def _link(path: Path, url: str) -> str:
    relative = path.relative_to(ROOT_DIR).as_posix()
    return f"`{relative} <{url}/{relative}>`__"


def _table(targets: list[FuzzTarget]) -> list[str]:
    lines = [
        ".. list-table::",
        "   :header-rows: 1",
        "   :widths: 30 70",
        "",
        "   * - Target",
        "     - Input",
    ]
    for target in targets:
        lines += [f"   * - :ref:`fuzz-{target.name}`", f"     - {target.paragraphs[0]}"]
    return lines + [""]


def _section(target: FuzzTarget, url: str) -> list[str]:
    title = f"``{target.name}``"
    lines = [f".. _fuzz-{target.name}:", "", title, "-" * len(title), ""]
    for paragraph in target.paragraphs:
        lines += [paragraph, ""]
    lines.append(f":Harness: {_link(target.harness, url)}")
    lines += [f":Code under test: {_link(path, url)}" for path in target.code]
    return lines + [
        "",
        ".. code-block:: sh",
        "",
        f"   mkdir -p corpus/{target.name}",
        f"   ./build_fuzz/tests/fuzz/{target.name} -max_total_time=60 corpus/{target.name}",
        "",
    ]


class FuzzTargets(SphinxDirective):
    """Write the table and one section for each target of tests/fuzz."""

    has_content = False

    def run(self):
        # Each file is a dependency before it is read, so that the next build
        # reads the page again after a fix.
        self.env.note_dependency(str(BUILD_FILE))
        try:
            sources = target_sources()
            for _, harness in sources:
                self.env.note_dependency(str(harness))
            targets = [read_target(name, harness) for name, harness in sources]
        except FuzzTargetError as error:
            raise self.error(f"fuzz-targets: {error}") from error

        url = self.config.fuzz_targets_url
        lines = _table(targets)
        for target in targets:
            lines += _section(target, url)
        content = StringList()
        for line in lines:
            content.append(line, str(BUILD_FILE))
        node = nodes.section()
        node.document = self.state.document
        nested_parse_with_titles(self.state, content, node)
        return node.children


def setup(app):
    app.add_config_value(
        "fuzz_targets_url",
        "https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main",
        "env",
    )
    app.add_directive("fuzz-targets", FuzzTargets)
    return {"parallel_read_safe": True, "parallel_write_safe": True}
