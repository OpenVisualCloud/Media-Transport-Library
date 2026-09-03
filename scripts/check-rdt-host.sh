#!/usr/bin/env bash
# Verify that the worker RDT helper and resctrl are ready for an RDT run.
# Read-only; changes nothing.
#
# Run FROM THE CONTROLLER:
#   scripts/check-rdt-host.sh [NODE]        # default: LAB_DEFAULT_NODE
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/config/lab.env"
# shellcheck source=/dev/null
source "$ROOT/config/nodes.env"
: "${LAB_SSH_USER:?set LAB_SSH_USER in config/nodes.env}"
NODE="${1:-$LAB_DEFAULT_NODE}"
KEY="${NODE^^}_HOST"
KEY="${KEY//-/_}"
HOST="${!KEY:-}"
[[ -n "$HOST" ]] || { echo "FATAL: no host address for $NODE ($KEY)" >&2; exit 2; }
TARGET="${LAB_SSH_USER}@$HOST"

ssh -o BatchMode=yes -o ConnectTimeout=10 "$TARGET" '
  set -eu
  command -v /usr/local/sbin/mxl-rdt-host >/dev/null
  if [ "$(id -u)" -eq 0 ]; then admin=""; else admin="sudo -n"; fi
  $admin /usr/local/sbin/mxl-rdt-host capabilities >/tmp/mxl-rdt-check.json
  grep -q '"L3"' /tmp/mxl-rdt-check.json
  grep -q '"L3_MON"' /tmp/mxl-rdt-check.json
  grep -q '"MB"' /tmp/mxl-rdt-check.json
  printf "installed helper version: %s\n" "$(sed -n "s/.*\"helper_version\": *\([0-9]*\).*/\1/p" /tmp/mxl-rdt-check.json | head -1)"
  test -r /sys/fs/resctrl/info/L3/cbm_mask
  test -r /sys/fs/resctrl/info/L3/min_cbm_bits
  test -d /sys/fs/resctrl/info/L3_MON
  if ! grep -qE "^[[:space:]]*L3:" /sys/fs/resctrl/schemata; then
    echo "FATAL: root schemata exposes no L3 allocation resource; CAT profiles cannot run" >&2
    exit 4
  fi
  if ! grep -qE "^[[:space:]]*MB:" /sys/fs/resctrl/schemata; then
    echo "FATAL: root schemata exposes no MB resource; MBA profiles cannot run" >&2
    exit 5
  fi
  printf "L3 ways available: %s, minimum CBM bits: %s, control groups: %s\n" \
    "$(cat /sys/fs/resctrl/info/L3/cbm_mask)" \
    "$(cat /sys/fs/resctrl/info/L3/min_cbm_bits)" \
    "$(cat /sys/fs/resctrl/info/L3/num_closids)"
  if find /sys/fs/resctrl -maxdepth 2 -type d -name "mxl-*" | grep -q .; then
    echo "FATAL: stale mxl-* resctrl groups exist" >&2
    exit 3
  fi
  if systemctl is-active --quiet pcm-sensor-server.service; then
    echo "PCM host exporter: active (RDT compatibility pilot still required)"
  else
    echo "PCM host exporter: inactive"
  fi
  echo "RDT host preflight: ready"
'
