#!/usr/bin/env bash
# One-shot worker bootstrap: stages every host-side installer on a worker and
# runs them there as root.
#
#   install-worker-limits.sh   inotify limits a dense MXL run needs
#   install-pcm-host.sh        Intel PCM Prometheus exporter (UPI, DRAM, L3)
#   install-host-noise.sh      stress-ng and friends for host-scoped neighbors
#   install-rdt-host.sh        resctrl mount + the mxl-rdt-host helper
#   install-platform-probe.sh  read-only DMI access for the DRAM peak figure
#
# Run FROM THE CONTROLLER:
#   scripts/bootstrap-worker.sh [NODE]      # default: LAB_DEFAULT_NODE
#
# The worker sudo password is typed into the worker's own prompt; it is never
# stored here or passed on a command line.
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

NODE="${1:-$LAB_DEFAULT_NODE}"
KEY="${NODE^^}_HOST"; KEY="${KEY//-/_}"
HOST="${!KEY:-}"
[[ -n "$HOST" ]] || { echo "FATAL: no address for $NODE ($KEY) in config/nodes.env" >&2; exit 2; }
TARGET="${LAB_SSH_USER}@$HOST"
# Staging path is relative to the login's home directory on the worker.
STAGE="mxl-worker-install"

FILES=(
  scripts/install-worker-limits.sh
  scripts/install-pcm-host.sh
  scripts/install-host-noise.sh
  scripts/install-rdt-host.sh
  scripts/install-platform-probe.sh
  scripts/mxl-rdt-host.py
)
for file in "${FILES[@]}"; do
  [[ -f "$ROOT/$file" ]] || { echo "FATAL: missing $file" >&2; exit 2; }
done

ssh -o BatchMode=yes "$TARGET" "mkdir -p '$STAGE' && chmod 700 '$STAGE'"
# mxl-rdt-host.py must land beside install-rdt-host.sh; that installer reads it.
scp -q "${FILES[@]/#/$ROOT/}" "$TARGET:$STAGE/"
ssh -o BatchMode=yes "$TARGET" "chmod 755 '$STAGE'/*.sh '$STAGE'/mxl-rdt-host.py && bash -n '$STAGE'/install-*.sh && python3 -m py_compile '$STAGE'/mxl-rdt-host.py"

echo "Staged worker installers on $NODE in ~/$STAGE"
echo "Running them as root on $NODE.${LAB_REMOTE_SUDO:+ Enter the worker sudo password when prompted.}"
echo "The first run builds Intel PCM from source and can take a few minutes."
"${LAB_REMOTE_SSH[@]}" "$TARGET" "
  set -e
  $LAB_REMOTE_SUDO ${LAB_REMOTE_SUDO:+-v}
  # id -un is evaluated on the worker, so the installers get the login that is
  # actually in use there rather than a name hardcoded on the controller.
  me=\$(id -un)
  for script in install-worker-limits.sh install-pcm-host.sh install-host-noise.sh install-rdt-host.sh install-platform-probe.sh; do
    printf '\n== %s ==\n' \"\$script\"
    $LAB_REMOTE_SUDO RDT_USER=\"\$me\" PROBE_USER=\"\$me\" PCM_RESERVED_CPUS='${LAB_RESERVED_CPUS:-0-3}' \
      '$STAGE'/\"\$script\"
  done
"

echo
echo "Worker bootstrap finished. Verifying the RDT helper:"
"$ROOT/scripts/check-rdt-host.sh" "$NODE"
echo
echo "Next: scripts/install-observability.sh (wires Prometheus to the new PCM exporter)"
