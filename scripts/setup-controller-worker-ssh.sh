#!/usr/bin/env bash
# Give the controller passwordless SSH to every worker.
#
# The host-scoped noisy neighbor and the RDT helper are driven from the
# controller over SSH, so the controller needs its own key on each worker. This
# generates that key if it is missing, installs it, then proves the hop works.
#
# Run FROM ANY HOST that already has SSH key access to all nodes:
#   scripts/setup-controller-worker-ssh.sh
#
# Nodes come from LAB_CONTROLLER and LAB_WORKERS in config/lab.env; addresses
# and the login come from config/nodes.env.
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/config/lab.env"
# shellcheck source=/dev/null
source "$ROOT/config/nodes.env"
: "${LAB_SSH_USER:?set LAB_SSH_USER in config/nodes.env}"

target_for() {
  local key="${1^^}_HOST"; key="${key//-/_}"
  local addr="${!key:-}"
  [[ -n "$addr" ]] || { echo "FATAL: no address for $1 ($key) in config/nodes.env" >&2; exit 2; }
  printf '%s@%s' "$LAB_SSH_USER" "$addr"
}

CONTROLLER="$(target_for "$LAB_CONTROLLER")"
IFS=, read -r -a WORKER_NODES <<<"$LAB_WORKERS"

ssh -o BatchMode=yes "$CONTROLLER" '
  umask 077
  mkdir -p ~/.ssh
  if [[ ! -f ~/.ssh/id_ed25519 ]]; then
    ssh-keygen -q -t ed25519 -N "" -C "mxl-host-noise-controller" -f ~/.ssh/id_ed25519
  fi
  chmod 700 ~/.ssh
  chmod 600 ~/.ssh/id_ed25519
  chmod 644 ~/.ssh/id_ed25519.pub
'

controller_key="$(ssh -o BatchMode=yes "$CONTROLLER" 'cat ~/.ssh/id_ed25519.pub')"

# The controller needs its own key too: install-k8s-cluster.sh reaches every node
# the same way, and when it runs on the controller that includes the controller.
for node in "$LAB_CONTROLLER" "${WORKER_NODES[@]}"; do
  worker="$(target_for "$node")"
  printf '%s\n' "$controller_key" | ssh -o BatchMode=yes "$worker" '
    umask 077
    mkdir -p ~/.ssh
    touch ~/.ssh/authorized_keys
    IFS= read -r key
    grep -qxF "$key" ~/.ssh/authorized_keys || printf "%s\n" "$key" >>~/.ssh/authorized_keys
    chmod 700 ~/.ssh
    chmod 600 ~/.ssh/authorized_keys
  '
  ssh -o BatchMode=yes "$CONTROLLER" \
    "ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 '$worker' 'echo controller-to-$node-ssh=ready'"
done
