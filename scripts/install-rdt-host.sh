#!/usr/bin/env bash
set -Eeuo pipefail

[[ ${EUID} -eq 0 ]] || { echo "FATAL: run as root on Kubernetes worker" >&2; exit 2; }
command -v apt-get >/dev/null || { echo "FATAL: apt-get is required" >&2; exit 2; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HELPER_SOURCE="$SCRIPT_DIR/mxl-rdt-host.py"
# The login allowed to use the helper through sudo. scripts/bootstrap-worker.sh
# passes it explicitly; a standalone "sudo ./install-rdt-host.sh" picks up the
# invoking user.
RDT_USER="${RDT_USER:-${SUDO_USER:-}}"
[[ -n "$RDT_USER" ]] || { echo "FATAL: set RDT_USER to the worker login that drives RDT" >&2; exit 2; }
[[ -f "$HELPER_SOURCE" ]] || { echo "FATAL: helper missing beside installer: $HELPER_SOURCE" >&2; exit 2; }
id "$RDT_USER" >/dev/null 2>&1 || { echo "FATAL: worker user not found: $RDT_USER" >&2; exit 2; }

aexport=noninteractive
export DEBIAN_FRONTEND="$aexport"
apt-get update
apt-get install -y intel-cmt-cat util-linux

install -o root -g root -m 0755 "$HELPER_SOURCE" /usr/local/sbin/mxl-rdt-host
if [[ "$RDT_USER" != "root" ]]; then
  command -v sudo >/dev/null || { echo "FATAL: sudo is required for non-root RDT user $RDT_USER" >&2; exit 2; }
  cat >/etc/sudoers.d/mxl-rdt-host <<EOF
$RDT_USER ALL=(root) NOPASSWD: /usr/local/sbin/mxl-rdt-host capabilities, /usr/local/sbin/mxl-rdt-host start -- *, /usr/local/sbin/mxl-rdt-host stop
EOF
  chmod 0440 /etc/sudoers.d/mxl-rdt-host
  visudo -cf /etc/sudoers.d/mxl-rdt-host >/dev/null
fi

mkdir -p /sys/fs/resctrl
if ! findmnt -t resctrl /sys/fs/resctrl >/dev/null; then
  mount -t resctrl resctrl /sys/fs/resctrl
fi
if ! grep -Eq '^[^#]+[[:space:]]+/sys/fs/resctrl[[:space:]]+resctrl[[:space:]]' /etc/fstab; then
  printf '%s\n' 'resctrl /sys/fs/resctrl resctrl defaults 0 0' >>/etc/fstab
fi

if [[ "$RDT_USER" == "root" ]]; then
  /usr/local/sbin/mxl-rdt-host capabilities >/tmp/mxl-rdt-capabilities.json
else
  sudo -u "$RDT_USER" sudo -n /usr/local/sbin/mxl-rdt-host capabilities >/tmp/mxl-rdt-capabilities.json
fi
if ! pqos-os -D >/tmp/mxl-pqos-capabilities.txt 2>/tmp/mxl-pqos-capabilities.err; then
  printf '%s\n' "WARNING: pqos-os capability probe unavailable; active PCM/PQoS process may own API lock." >&2
  printf '%s\n' "WARNING: resctrl helper validation passed; run monitor-only compatibility pilot before RDT control." >&2
fi

echo "RDT host helper ready. Monitoring/control remain disabled until run opts in."
echo "Capabilities: /tmp/mxl-rdt-capabilities.json"
echo "Optional PQoS probe: /tmp/mxl-pqos-capabilities.txt and /tmp/mxl-pqos-capabilities.err"
