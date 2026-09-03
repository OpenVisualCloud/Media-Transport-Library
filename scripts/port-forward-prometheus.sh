#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/no-proxy.sh
source "$ROOT/scripts/lib/no-proxy.sh"
lab_export_no_proxy   # or the port-forward goes to the API server via a proxy
exec kubectl -n monitoring port-forward svc/monitoring-kube-prometheus-prometheus 19090:9090
