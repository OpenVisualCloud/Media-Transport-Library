#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Bootstrap wrapper for the MTL Validation Setup MCP server.
# Shares the venv with run_server.sh (same dependency: the `mcp` package).
# Called by .vscode/mcp.json — do not invoke mtl_validation_mcp_server.py directly.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"
REQUIREMENTS="$SCRIPT_DIR/requirements.txt"
SERVER="$SCRIPT_DIR/mtl_validation_mcp_server.py"

# Create venv if missing
if [[ ! -x "$VENV_DIR/bin/python3" ]]; then
	python3 -m venv "$VENV_DIR"
fi

# Repair venv if pip is missing (e.g. venv created before ensurepip was available)
if ! "$VENV_DIR/bin/python3" -m pip --version >/dev/null 2>&1; then
	"$VENV_DIR/bin/python3" -m ensurepip --upgrade
fi

# Install deps if mcp module is missing
if ! "$VENV_DIR/bin/python3" -c "import mcp" 2>/dev/null; then
	"$VENV_DIR/bin/python3" -m pip install --quiet -r "$REQUIREMENTS"
fi

exec "$VENV_DIR/bin/python3" "$SERVER"
