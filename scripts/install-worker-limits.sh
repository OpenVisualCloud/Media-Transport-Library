#!/usr/bin/env bash
# Raise the kernel limits a dense MXL run needs on a worker.
#
# Every MXL flow opens an inotify instance to watch its domain directory, so a
# 20-stream run needs ~40 of them on top of whatever kubelet, containerd,
# systemd and journald already hold. The kernel default
# (fs.inotify.max_user_instances = 128) is shared by every process running as
# root on the host, and it runs out around 10 streams. FFmpeg then fails with:
#
#   inotify_init1 failed: Too many open files
#   Failed to create instance
#   Could not write header (incorrect codec parameters ?): Input/output error
#
# which looks like a codec problem and is not one.
set -Eeuo pipefail

[[ ${EUID} -eq 0 ]] || { echo "FATAL: run as root on Kubernetes worker" >&2; exit 2; }

CONF=/etc/sysctl.d/99-mxl-perf-lab.conf
cat >"$CONF" <<'EOF'
# Installed by mxl-k8s-qos-lab (scripts/install-worker-limits.sh).
# One inotify instance per MXL flow, shared with kubelet/containerd/systemd.
fs.inotify.max_user_instances = 8192
fs.inotify.max_user_watches = 1048576
EOF
chmod 644 "$CONF"

sysctl -p "$CONF"

instances=$(sysctl -n fs.inotify.max_user_instances)
[[ "$instances" -ge 8192 ]] || { echo "FATAL: max_user_instances is $instances after applying $CONF" >&2; exit 2; }
echo "Worker kernel limits ready (persisted in $CONF)."
