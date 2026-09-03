#!/usr/bin/env bash
# Install Intel PCM's Prometheus exporter (pcm-sensor-server) as a host service
# on a Kubernetes worker. This is what supplies the whole-worker counters the
# report needs and that no in-cluster exporter can see: UPI cross-socket
# traffic, DRAM read/write bandwidth, and L3 hit ratio.
#
# Run as root ON THE WORKER. The binary is taken from, in order:
#   1. $PCM_BINARY, if set
#   2. pcm-sensor-server staged beside this script (scp'd by bootstrap-worker.sh)
#   3. a source build of intel/pcm at $PCM_REF (needs git, cmake, a compiler)
set -Eeuo pipefail

[[ ${EUID} -eq 0 ]] || { echo "FATAL: run as root on Kubernetes worker" >&2; exit 2; }
command -v systemctl >/dev/null || { echo "FATAL: systemd is required" >&2; exit 2; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESERVED_CPUS="${PCM_RESERVED_CPUS:-0-3}"
PCM_PORT="${PCM_PORT:-9738}"
PCM_REF="${PCM_REF:-2026-07-08-public}"
TARGET_BINARY="/usr/local/sbin/pcm-sensor-server"

SOURCE_BINARY="${PCM_BINARY:-}"
if [[ -z "$SOURCE_BINARY" && -x "$SCRIPT_DIR/pcm-sensor-server" ]]; then
  SOURCE_BINARY="$SCRIPT_DIR/pcm-sensor-server"
fi
if [[ -z "$SOURCE_BINARY" ]]; then
  echo "== no pcm-sensor-server staged; building intel/pcm $PCM_REF from source =="
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq git cmake build-essential
  BUILD_DIR="${PCM_BUILD_DIR:-/opt/pcm-src}"
  if [[ ! -d "$BUILD_DIR/.git" ]]; then
    rm -rf "$BUILD_DIR"
    git clone --recursive --depth 1 --branch "$PCM_REF" https://github.com/intel/pcm "$BUILD_DIR"
  fi
  cmake -S "$BUILD_DIR" -B "$BUILD_DIR/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD_DIR/build" --target pcm-sensor-server -j "$(nproc)"
  SOURCE_BINARY="$(find "$BUILD_DIR/build" -name pcm-sensor-server -type f -perm -u+x | head -1)"
fi
[[ -x "$SOURCE_BINARY" ]] || { echo "FATAL: no usable pcm-sensor-server binary" >&2; exit 2; }

# PCM reads model-specific registers, so the msr module must be loaded.
[[ -e /dev/cpu/0/msr ]] || modprobe msr
[[ -e /dev/cpu/0/msr ]] || { echo "FATAL: /dev/cpu/0/msr unavailable" >&2; exit 2; }

systemctl disable --now pcm-sensor-server.service 2>/dev/null || true
systemctl reset-failed pcm-sensor-server.service 2>/dev/null || true
install -o root -g root -m 0755 "$SOURCE_BINARY" "$TARGET_BINARY"

# CPUAffinity keeps the exporter on the kubelet-reserved CPUs so it never steals
# time from a pinned encoder core and distorts the numbers it is measuring.
# `yes |` answers PCM's "another instance may be running" prompt.
cat >/etc/systemd/system/pcm-sensor-server.service <<EOF
[Unit]
Description=Intel PCM host-wide Prometheus exporter
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
Restart=on-failure
RestartSec=10
# pcm-sensor-server binds without SO_REUSEADDR, so a restart fails with
# "Exception Server Constructor: Cannot bind to port" until every socket on the
# port is gone - including the TIME_WAIT entries left by Prometheus scrapes,
# which linger about a minute. Hence 'ss -tan' (all states), not just listeners.
ExecStartPre=/bin/sh -c 'for _ in \$(seq 1 120); do ss -tan 2>/dev/null | awk "{print \\\$4}" | grep -q ":$PCM_PORT\$" || exit 0; sleep 1; done; echo "port $PCM_PORT still held after 120s" >&2; exit 1'
ExecStart=/bin/sh -c 'exec /usr/bin/yes | $TARGET_BINARY -p $PCM_PORT'
CPUAffinity=$RESERVED_CPUS
LimitNOFILE=1000000
TimeoutStopSec=20

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now pcm-sensor-server.service
# Generous: the port can be blocked by TIME_WAIT for a minute (see the unit
# above) and PCM then needs another minute to publish counters from 4 CPUs.
echo "Waiting for PCM UPI metrics on the worker-local endpoint..."
for _ in {1..240}; do
  if curl --noproxy '*' --connect-timeout 2 --max-time 10 -fsS \
      "http://127.0.0.1:$PCM_PORT/metrics" 2>/dev/null \
      | grep -F 'Incoming_Data_Traffic_On_Link_' >/dev/null; then
    echo "PCM host exporter ready on port $PCM_PORT (UPI, DRAM and L3 counters visible)."
    exit 0
  fi
  sleep 1
done
echo "FATAL: exporter did not publish UPI counters; see journal below" >&2
journalctl -u pcm-sensor-server.service --no-pager -n 100 >&2
exit 1
