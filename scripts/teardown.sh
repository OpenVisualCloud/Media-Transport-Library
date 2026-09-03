#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/no-proxy.sh
source "$ROOT/scripts/lib/no-proxy.sh"
lab_export_no_proxy   # deleting the namespace goes through the API server too
exec "$ROOT/.venv/bin/mxl-perf" teardown "$@"
