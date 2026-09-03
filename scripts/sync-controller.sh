#!/usr/bin/env bash
# Copy this checkout to the controller, where runs are executed from.
#
# Results, the virtualenv and rendered manifests stay local to each side: the
# controller produces results, your workstation only ships code and config.
#
#   scripts/sync-controller.sh [--dry-run]
#
# The destination is MXL_REMOTE_ROOT, by default a directory named
# mxl-k8s-qos-lab in the home directory of LAB_SSH_USER on the controller.
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/config/lab.env"
# shellcheck source=/dev/null
source "$ROOT/config/nodes.env"
: "${LAB_SSH_USER:?set LAB_SSH_USER in config/nodes.env}"

KEY="${LAB_CONTROLLER^^}_HOST"; KEY="${KEY//-/_}"
HOST="${!KEY:-}"
[[ -n "$HOST" ]] || { echo "FATAL: no address for $LAB_CONTROLLER ($KEY) in config/nodes.env" >&2; exit 2; }
TARGET="$LAB_SSH_USER@$HOST"

# Relative to the login's home directory unless MXL_REMOTE_ROOT is absolute.
REMOTE_ROOT="${MXL_REMOTE_ROOT:-mxl-k8s-qos-lab}"
MODE=()
if [[ ${1:-} == "--dry-run" ]]; then
  MODE=(--dry-run)
elif [[ $# -gt 0 ]]; then
  echo "Usage: $0 [--dry-run]" >&2
  exit 2
fi

rsync -azcvi "${MODE[@]}" \
  --exclude '.venv/' \
  --exclude '.rendered/' \
  --exclude 'results/' \
  --exclude '__pycache__/' \
  --exclude '*.pyc' \
  --exclude '.pytest_cache/' \
  --exclude 'python/*.egg-info/' \
  "$ROOT/" "$TARGET:$REMOTE_ROOT/"

echo
echo "Synced to $LAB_CONTROLLER:$REMOTE_ROOT"
echo "On the controller: cd $REMOTE_ROOT && scripts/setup.sh && scripts/preflight.sh"
