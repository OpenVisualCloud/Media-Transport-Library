#!/usr/bin/env bash
# Grants read-only DMI memory access so runs can probe memory transfer rate and
# compute theoretical DRAM peak bandwidth. Run once as root on each worker.
set -Eeuo pipefail

[[ ${EUID} -eq 0 ]] || { echo "FATAL: run as root on Kubernetes worker" >&2; exit 2; }
# The login allowed to read DMI through sudo. scripts/bootstrap-worker.sh passes
# it explicitly; a standalone "sudo ./install-platform-probe.sh" picks up the
# invoking user.
PROBE_USER="${PROBE_USER:-${SUDO_USER:-}}"
[[ -n "$PROBE_USER" ]] || { echo "FATAL: set PROBE_USER to the worker login that runs measurements" >&2; exit 2; }
id "$PROBE_USER" >/dev/null 2>&1 || { echo "FATAL: worker user not found: $PROBE_USER" >&2; exit 2; }
command -v dmidecode >/dev/null || { echo "FATAL: dmidecode is required" >&2; exit 2; }

if [[ "$PROBE_USER" != "root" ]]; then
	command -v sudo >/dev/null || { echo "FATAL: sudo is required for non-root probe user $PROBE_USER" >&2; exit 2; }
	cat >/etc/sudoers.d/mxl-platform-probe <<EOF
$PROBE_USER ALL=(root) NOPASSWD: /usr/sbin/dmidecode -t memory
EOF
	chmod 0440 /etc/sudoers.d/mxl-platform-probe
	visudo -cf /etc/sudoers.d/mxl-platform-probe >/dev/null
fi

if [[ "$PROBE_USER" == "root" ]]; then
	/usr/sbin/dmidecode -t memory | grep -E 'Configured Memory Speed' | head -4
else
	sudo -u "$PROBE_USER" sudo -n /usr/sbin/dmidecode -t memory | grep -E 'Configured Memory Speed' | head -4
fi

echo "Platform probe ready. Runs now record memory transfer rate and theoretical DRAM peak."
