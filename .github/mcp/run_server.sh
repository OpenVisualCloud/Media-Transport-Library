#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Bootstrap wrapper for the MTL MCP server.
# Auto-creates the Python venv and installs dependencies if missing.
# Called by .vscode/mcp.json — do not invoke mtl_mcp_server.py directly.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"
REQUIREMENTS="$SCRIPT_DIR/requirements.txt"
SERVER="$SCRIPT_DIR/mtl_mcp_server.py"

# Create venv if missing
if [[ ! -x "$VENV_DIR/bin/python3" ]]; then
	python3 -m venv "$VENV_DIR"
fi

# Install deps if mcp module is missing
if ! "$VENV_DIR/bin/python3" -c "import mcp" 2>/dev/null; then
	"$VENV_DIR/bin/pip" install --quiet -r "$REQUIREMENTS"
fi

exec "$VENV_DIR/bin/python3" "$SERVER"
