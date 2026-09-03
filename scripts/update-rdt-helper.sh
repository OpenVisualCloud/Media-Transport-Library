#!/usr/bin/env bash
# Reinstalls the worker RDT helper after it changes in this checkout.
#
# Run FROM THE CONTROLLER:
#   scripts/update-rdt-helper.sh [NODE]     # default: LAB_DEFAULT_NODE
#
# The worker sudo password is typed into the worker's own prompt.
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
KEY="${NODE^^}_HOST"
KEY="${KEY//-/_}"
HOST="${!KEY:-}"
[[ -n "$HOST" ]] || { echo "FATAL: no host address for $NODE ($KEY)" >&2; exit 2; }
TARGET="${LAB_SSH_USER}@$HOST"
# Staging path is relative to the login's home directory on the worker.
STAGE="mxl-rdt-install"

ssh -o BatchMode=yes "$TARGET" "mkdir -p '$STAGE' && chmod 700 '$STAGE'"
scp -q "$ROOT/scripts/mxl-rdt-host.py" "$TARGET:$STAGE/"
ssh -o BatchMode=yes "$TARGET" "python3 -m py_compile '$STAGE'/mxl-rdt-host.py"

if ssh -o BatchMode=yes "$TARGET" "$LAB_REMOTE_SUDO ${LAB_REMOTE_SUDO:+-n} install -o root -g root -m 0755 '$STAGE'/mxl-rdt-host.py /usr/local/sbin/mxl-rdt-host" 2>/dev/null; then
  echo "Helper updated without prompting."
else
  echo "Installing helper on $NODE. Enter the worker sudo password when prompted."
  "${LAB_REMOTE_SSH[@]}" "$TARGET" "$LAB_REMOTE_SUDO install -o root -g root -m 0755 '$STAGE'/mxl-rdt-host.py /usr/local/sbin/mxl-rdt-host"
fi

"$ROOT/scripts/check-rdt-host.sh" "$NODE"
