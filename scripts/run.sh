#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -x "$ROOT/.venv/bin/mxl-perf" ]] || { echo "FATAL: run scripts/setup.sh first" >&2; exit 2; }
# shellcheck source=lib/no-proxy.sh
source "$ROOT/scripts/lib/no-proxy.sh"
lab_export_no_proxy   # the runner shells out to kubectl throughout
exec "$ROOT/.venv/bin/mxl-perf" run "$@"
