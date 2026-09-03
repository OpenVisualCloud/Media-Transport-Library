#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$ROOT/.venv/bin/mxl-perf" summarize "${1:-$ROOT/results}" --min-fps "${MIN_FPS:-59.5}"
