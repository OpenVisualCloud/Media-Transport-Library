#!/usr/bin/env bash
# MCP profiling framework.
#
# Installs cpu-debug-mcp on the worker: an MCP server exposing ~70 read-only
# Linux CPU/perf tools (per-core load, affinity, starvation analysis, IRQ and
# softirq distribution, cgroup quotas, NUMA placement, Intel PCM counters,
# turbostat, eBPF latency tracing). Use it to ask *why* a run missed 60 FPS -
# the campaign reports tell you that it did, not which core was stolen by what.
#
# It is a diagnosis tool, not part of a measurement: the tools are read-only,
# but tracing tools do perturb the system, so profile a repro run, never a run
# whose numbers you intend to publish.
#
# Run FROM THE CONTROLLER:
#   scripts/install-mcp-profiler.sh [NODE]     # default: LAB_DEFAULT_NODE
#
# Source selection: MCP_LOCAL_DIR (a local checkout, copied with rsync) wins,
# otherwise MCP_REPO is cloned on the worker.
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

MCP_LOCAL_DIR="${MCP_LOCAL_DIR:-$(cd "$ROOT/.." && pwd)/cpu-debug-MCP}"
MCP_REPO="${MCP_REPO:-https://github.com/moleksy/cpu-debug-MCP.git}"
MCP_PORT="${MCP_PORT:-3001}"
REMOTE_DIR="cpu-debug-mcp"

echo "== 1/4 sources on $NODE =="
if [[ -d "$MCP_LOCAL_DIR/src" ]]; then
  echo "copying $MCP_LOCAL_DIR (build artefacts and dependencies are rebuilt on the worker)"
  rsync -az --delete \
    --exclude node_modules --exclude dist --exclude .git --exclude native/target \
    --exclude '*.pdf' \
    "$MCP_LOCAL_DIR/" "$TARGET:$REMOTE_DIR/"
else
  echo "no local checkout at $MCP_LOCAL_DIR; cloning $MCP_REPO on the worker"
  ssh -o BatchMode=yes "$TARGET" "
    set -e
    [[ -d '$REMOTE_DIR/.git' ]] || git clone '$MCP_REPO' '$REMOTE_DIR'
    cd '$REMOTE_DIR' && git pull --ff-only
  "
fi

echo "== 2/4 Node.js >= 18 and profiling tools =="
[[ -z "$LAB_REMOTE_SUDO" ]] || echo "Enter the worker sudo password when prompted."
"${LAB_REMOTE_SSH[@]}" "$TARGET" "
set -Eeuo pipefail
need_node=1
if command -v node >/dev/null; then
  major=\$(node -v | sed 's/^v\([0-9]*\).*/\1/')
  [[ \"\$major\" -ge 18 ]] && need_node=0
fi
if [[ \$need_node -eq 1 ]]; then
  curl -fsSL https://deb.nodesource.com/setup_22.x | $LAB_REMOTE_SUDO ${LAB_REMOTE_SUDO:+-E} bash -
  $LAB_REMOTE_SUDO apt-get install -y -qq nodejs
fi
node -v
# Optional but high value: turbostat (frequency/C-state residency), sysstat,
# ethtool, and the BCC tools behind the latency-tracing MCP tools.
$LAB_REMOTE_SUDO apt-get install -y -qq sysstat ethtool 'linux-tools-common' \"linux-tools-\$(uname -r)\" || \
  echo 'WARN: turbostat package unavailable for this kernel; frequency tools degrade gracefully'
$LAB_REMOTE_SUDO apt-get install -y -qq bpfcc-tools bpftrace || \
  echo 'WARN: BCC/bpftrace unavailable; the 16 tracing tools degrade gracefully'
"

echo "== 3/4 building the MCP server on $NODE =="
ssh -o BatchMode=yes "$TARGET" "
set -Eeuo pipefail
cd '$REMOTE_DIR'
npm install --no-audit --no-fund
npm run build
node dist/index.js --help >/dev/null 2>&1 || true
ls -la dist/index.js
"

echo "== 4/4 TCP listener on 127.0.0.1:$MCP_PORT =="
# Bound to loopback on the worker: reach it through an SSH tunnel, so the tool
# surface is never exposed on the lab network.
# The unit runs as the login that owns the build, resolved on the worker itself.
"${LAB_REMOTE_SSH[@]}" "$TARGET" "
set -Eeuo pipefail
$LAB_REMOTE_SUDO tee /etc/systemd/system/cpu-debug-mcp.service >/dev/null <<UNIT
[Unit]
Description=cpu-debug MCP profiling server (loopback only)
After=network-online.target

[Service]
Type=simple
User=\$(id -un)
WorkingDirectory=\$HOME/$REMOTE_DIR
ExecStart=/usr/bin/node dist/index.js --tcp --port $MCP_PORT
Restart=on-failure
RestartSec=5
CPUAffinity=${LAB_RESERVED_CPUS:-0-3}

[Install]
WantedBy=multi-user.target
UNIT
$LAB_REMOTE_SUDO systemctl daemon-reload
$LAB_REMOTE_SUDO systemctl enable --now cpu-debug-mcp.service
sleep 2
systemctl is-active cpu-debug-mcp.service
"

cat <<EOF

MCP profiling framework ready on $NODE.

Open a tunnel from the machine running your MCP client:
  ssh -N -L $MCP_PORT:127.0.0.1:$MCP_PORT ${LAB_SSH_USER}@$HOST

Then point the client at it (.vscode/mcp.json or your agent config):
  {
    "servers": {
      "cpu-debug": { "type": "http", "url": "http://127.0.0.1:$MCP_PORT/mcp" }
    }
  }

Call the 'capabilities' tool first - it reports which optional subsystems
(PCM on port 9738, EMON, BCC, turbostat) are actually available on this worker.
See docs/13-mcp-profiling.md for the triage recipes.
EOF
