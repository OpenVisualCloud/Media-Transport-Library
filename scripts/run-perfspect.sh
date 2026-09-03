#!/usr/bin/env bash
# PerfSpect baseline report.
#
# Intel PerfSpect captures the platform as configured: BIOS version and settings,
# CPU model and frequencies, prefetchers, C-states, uncore, DIMM population and
# speed, kernel and software versions, PMU status. Capture it ONCE per machine
# before any measurement - it is the evidence that a customer's hardware matches
# the BKC, and the first thing to compare when their FPS numbers differ.
#
# Run FROM THE CONTROLLER:
#   scripts/run-perfspect.sh [NODE]          # default: LAB_DEFAULT_NODE
#
# Installs PerfSpect on the worker if absent, runs a full configuration report,
# and copies the HTML report into results/perfspect/<node>/.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/config/lab.env"
# shellcheck source=/dev/null
source "$ROOT/config/nodes.env"
: "${LAB_SSH_USER:?set LAB_SSH_USER in config/nodes.env}"
# shellcheck source=lib/remote-admin.sh
source "$ROOT/scripts/lib/remote-admin.sh"
lab_remote_admin_init "$LAB_SSH_USER"
# One definition of where PerfSpect lives, shared with scripts/configure-power.sh.
# shellcheck source=lib/perfspect.sh
source "$ROOT/scripts/lib/perfspect.sh"

NODE="${1:-$LAB_DEFAULT_NODE}"
KEY="${NODE^^}_HOST"; KEY="${KEY//-/_}"
HOST="${!KEY:-}"
[[ -n "$HOST" ]] || { echo "FATAL: no address for $NODE ($KEY) in config/nodes.env" >&2; exit 2; }
TARGET="${LAB_SSH_USER}@$HOST"
REMOTE_DIR="$LAB_PERFSPECT_DIR"
LOCAL_DIR="$ROOT/${LAB_RESULTS_DIR:-results}/perfspect/$NODE"

echo "== 1/3 ensuring PerfSpect is installed on $NODE =="
lab_perfspect_install "$TARGET"

echo "== 2/3 collecting the report on $NODE =="
# PerfSpect reads MSRs and DMI, so parts of the report need root. Enter the
# worker sudo password when prompted; it is never stored here.
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
"${LAB_REMOTE_SSH[@]}" "$TARGET" "
  set -e
  cd ~/$REMOTE_DIR
  $LAB_REMOTE_SUDO ./perfspect report --all --format html,json,xlsx --noupdate --output ~/$REMOTE_DIR/baseline-$STAMP
  $LAB_REMOTE_SUDO chown -R \$(id -u):\$(id -g) ~/$REMOTE_DIR/baseline-$STAMP
"

echo "== 3/3 copying the report back =="
mkdir -p "$LOCAL_DIR"
scp -q -r "$TARGET:$REMOTE_DIR/baseline-$STAMP" "$LOCAL_DIR/"
ln -sfn "baseline-$STAMP" "$LOCAL_DIR/latest"

echo
echo "PerfSpect baseline for $NODE: $LOCAL_DIR/baseline-$STAMP"
find "$LOCAL_DIR/baseline-$STAMP" -maxdepth 1 -name '*.html' -printf '  open %p\n'
echo "Compare a customer platform against docs/14-reference-bkc.md before trusting FPS deltas."
echo "If the P-state driver, governor, EPB, EPP or ELC is not what the BKC expects:"
echo "  scripts/configure-power.sh $NODE     # sets them, and keeps them across a reboot"
