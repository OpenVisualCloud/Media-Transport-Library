#!/usr/bin/python3

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation
# Media Transport Library

# Sphinx documentation build configuration file

# General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

from __future__ import annotations

import os
import sys

project = "Media Transport Library"
copyright = "2023-2025, Intel Corporation"
author = "Intel Corporation"
version = release = "24.09"

extensions = [
    "myst_parser",
    "sphinx.ext.graphviz",
    "sphinxcontrib.mermaid",
    "sphinx_copybutton",
]

coverage_statistics_to_report = coverage_statistics_to_stdout = True

inline_highlight_respect_highlight = False
inline_highlight_literals = False

templates_path = ["_templates"]
exclude_patterns = [
    "_build/*",
    "tests/*",
    "patches/*",
    "Thumbs.db",
    ".DS_Store",
    "**/CMakeLists.txt",
    "*CMakeLists.txt",
    "**/requirements.txt",
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
myst_fence_as_directive = ["mermaid"]

suppress_warnings = ["myst.xref_missing", "myst.strikethrough"]

source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

sys.path.insert(0, os.path.abspath(".."))
sys.path.insert(0, os.path.abspath("../../"))
