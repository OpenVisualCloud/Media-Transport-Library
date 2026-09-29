#!/usr/bin/python3

# Copyright 2025 Intel Corporation
# SPDX-License-Identifier: BSD-3-Clause

# Sphinx documentation build configuration file

# General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

from __future__ import annotations

import os
import sys

project = "Media Transport Library"
copyright = "2023-2026, Intel Corporation"
author = "Intel Corporation"

with open(os.path.join(os.path.dirname(__file__), "..", "..", "VERSION")) as f:
    release = f.read().strip()
version = ".".join(release.split(".")[:2])

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "_ext"))

extensions = [
    "myst_parser",
    "sphinx.ext.graphviz",
    "sphinxcontrib.mermaid",
    "sphinx_copybutton",
    "repo_links",
]

coverage_statistics_to_report = coverage_statistics_to_stdout = True

inline_highlight_respect_highlight = False
inline_highlight_literals = False

templates_path = ["_templates"]
exclude_patterns = [
    "_build/*",
    "doc/_build/*",
    "build/*",
    ".venv*",
    "tests/*",
    "patches/*",
    "Thumbs.db",
    ".DS_Store",
    "**/CMakeLists.txt",
    "*CMakeLists.txt",
    "**/requirements.txt",
    # Instructions for the AI coding agents, not user documentation.
    ".claude/*",
    ".github/*",
    "CLAUDE.md",
    "**/CLAUDE.md",
    # Parts that other documents include; not documents of their own.
    "doc/chunks/*",
]

# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = "sphinx_book_theme"
html_static_path = ["../png"]
language = "en_US"

# Options for myst_html_meta output -------------------------------------------------

myst_html_meta = {
    "description lang=en": "Media Transport Library",
    "keywords": "Intel®, Intel, Media Transport Library, MTL, st20, st22, ST 2110, ST2110",
    "property=og:locale": "en_US",
}
myst_enable_extensions = ["strikethrough"]
# Make the GitHub heading anchors (for example "#31-allow-current-user") work.
myst_heading_anchors = 6
myst_fence_as_directive = ["mermaid"]

suppress_warnings = ["myst.strikethrough"]

# Options for "make linkcheck" ------------------------------------------------

# These sites refuse a request that does not come from a browser.
linkcheck_ignore = [
    r"https://dl\.acm\.org/",
    r"https://access\.redhat\.com/",
    r"https://sourceforge\.net/",
]
# GitHub makes these anchors with JavaScript.
linkcheck_anchors_ignore_for_url = [r"https://github\.com/"]

source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

sys.path.insert(0, os.path.abspath(".."))
sys.path.insert(0, os.path.abspath("../../"))
