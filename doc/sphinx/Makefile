
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation
# Media Transport Library

# Minimal makefile for Sphinx documentation

SPHINXOPTS    ?= --conf-dir .
SPHINXBUILD   ?= sphinx-build
SOURCEDIR     = ../../
BUILDDIR      = ../_build

help:
	@$(SPHINXBUILD) -M help "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)

.PHONY: help Makefile

# Catch-all target: route all unknown targets to Sphinx using the new
# "make mode" option.  $(O) is meant as a shortcut for $(SPHINXOPTS).
%: Makefile
	@$(SPHINXBUILD) -M $@ "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)
