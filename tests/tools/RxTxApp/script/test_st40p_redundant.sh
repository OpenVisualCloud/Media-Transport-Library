#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Test gapped ST 2022-7 redundancy for st40p pipeline sessions.
#
# Creates 4 VFs from the given PF, then runs RxTxApp with:
#   TX: VF0 + VF1  (two-path redundant transmit)
#   RX: VF2 + VF3  (two-path redundant receive)
#
# Usage:
#   sudo ./test_st40p_redundant.sh [PF_BDF] [TEST_TIME_SEC]
#
# Defaults:
#   PF_BDF=0000:31:00.0  TEST_TIME_SEC=30

set -euo pipefail

###############################################################################
# Tunables
###############################################################################
PF_BDF="${1:-0000:31:00.0}"
TEST_TIME="${2:-30}"
NUM_VFS=4
NICCTL="$(cd "$(dirname "$0")/../../../../script" && pwd)/nicctl.sh"
RXTXAPP="$(cd "$(dirname "$0")/.." && pwd)/build/RxTxApp"
TMPDIR_BASE=$(mktemp -d /tmp/st40p_test_XXXXXX)
LOG_LEVEL="info"

TX_IP_P="192.168.40.10"
TX_IP_R="192.168.40.11"
RX_IP_P="192.168.40.12"
RX_IP_R="192.168.40.13"
MCAST_P="239.40.1.1"
MCAST_R="239.40.1.2"

cleanup() {
    echo ">>> Cleaning up..."
    # Kill any lingering RxTxApp
    kill "$PID_TX" 2>/dev/null && wait "$PID_TX" 2>/dev/null || true
    kill "$PID_RX" 2>/dev/null && wait "$PID_RX" 2>/dev/null || true
    # Disable VFs
    echo 0 > "/sys/bus/pci/devices/${PF_BDF}/sriov_numvfs" 2>/dev/null || true
    rm -rf "$TMPDIR_BASE"
    echo ">>> Cleanup done"
}
trap cleanup EXIT

###############################################################################
# Pre-flight
###############################################################################
if [[ $EUID -ne 0 ]]; then
    echo "ERROR: must run as root (sudo)" >&2
    exit 1
fi

if [[ ! -x "$RXTXAPP" ]]; then
    echo "ERROR: RxTxApp not found at $RXTXAPP" >&2
    echo "       Build it first: cd tests/tools/RxTxApp && meson setup build && ninja -C build" >&2
    exit 1
fi

if [[ ! -f "$NICCTL" ]]; then
    echo "ERROR: nicctl.sh not found at $NICCTL" >&2
    exit 1
fi

if [[ ! -d "/sys/bus/pci/devices/${PF_BDF}" ]]; then
    echo "ERROR: PF $PF_BDF does not exist" >&2
    exit 1
fi

###############################################################################
# Step 1: Create VFs
###############################################################################
echo ">>> Disabling any existing VFs on $PF_BDF"
echo 0 > "/sys/bus/pci/devices/${PF_BDF}/sriov_numvfs" 2>/dev/null || true
sleep 1

echo ">>> Creating $NUM_VFS VFs on $PF_BDF"
bash "$NICCTL" create_vf "$PF_BDF" "$NUM_VFS"
sleep 2

# Discover VF BDFs
VF_BDFS=()
for i in $(seq 0 $((NUM_VFS - 1))); do
    vfpath="/sys/bus/pci/devices/${PF_BDF}/virtfn${i}"
    if [[ ! -L "$vfpath" ]]; then
        echo "ERROR: VF${i} symlink not found at $vfpath" >&2
        exit 1
    fi
    vf_bdf=$(readlink "$vfpath" | awk -F/ '{print $NF}')
    VF_BDFS+=("$vf_bdf")
done

echo ">>> VFs created:"
echo "    TX_P: ${VF_BDFS[0]}"
echo "    TX_R: ${VF_BDFS[1]}"
echo "    RX_P: ${VF_BDFS[2]}"
echo "    RX_R: ${VF_BDFS[3]}"

###############################################################################
# Step 2: Generate JSON configs
###############################################################################
TX_JSON="$TMPDIR_BASE/tx_st40p_redundant.json"
RX_JSON="$TMPDIR_BASE/rx_st40p_redundant.json"

cat > "$TX_JSON" <<EOF
{
    "interfaces": [
        {
            "name": "${VF_BDFS[0]}",
            "ip": "${TX_IP_P}"
        },
        {
            "name": "${VF_BDFS[1]}",
            "ip": "${TX_IP_R}"
        }
    ],
    "tx_sessions": [
        {
            "dip": [
                "${MCAST_P}",
                "${MCAST_R}"
            ],
            "interface": [0, 1],
            "st40p": [
                {
                    "replicas": 1,
                    "start_port": 10100,
                    "payload_type": 113,
                    "ancillary_fps": "p59"
                }
            ]
        }
    ]
}
EOF

cat > "$RX_JSON" <<EOF
{
    "interfaces": [
        {
            "name": "${VF_BDFS[2]}",
            "ip": "${RX_IP_P}"
        },
        {
            "name": "${VF_BDFS[3]}",
            "ip": "${RX_IP_R}"
        }
    ],
    "rx_sessions": [
        {
            "ip": [
                "${MCAST_P}",
                "${MCAST_R}"
            ],
            "interface": [0, 1],
            "st40p": [
                {
                    "replicas": 1,
                    "start_port": 10100,
                    "payload_type": 113,
                    "ancillary_fps": "p59"
                }
            ]
        }
    ]
}
EOF

echo ">>> TX config: $TX_JSON"
cat "$TX_JSON"
echo ""
echo ">>> RX config: $RX_JSON"
cat "$RX_JSON"
echo ""

###############################################################################
# Step 3: Run TX + RX in parallel
###############################################################################
TX_TIME=$TEST_TIME
RX_TIME=$((TEST_TIME - 5))  # RX exits a bit earlier so TX sees clean shutdown

echo ">>> Starting TX (${TX_TIME}s) and RX (${RX_TIME}s)..."
echo ""

PID_TX=""
PID_RX=""

$RXTXAPP --config_file "$TX_JSON" --test_time "$TX_TIME" --log_level "$LOG_LEVEL" &
PID_TX=$!

# Small delay so TX registers its sessions before RX joins
sleep 2

$RXTXAPP --config_file "$RX_JSON" --test_time "$RX_TIME" --log_level "$LOG_LEVEL" &
PID_RX=$!

echo ">>> TX PID=$PID_TX, RX PID=$PID_RX"

###############################################################################
# Step 4: Wait and collect results
###############################################################################
RX_RC=0
TX_RC=0

wait "$PID_RX" || RX_RC=$?
echo ">>> RX exited with code $RX_RC"

wait "$PID_TX" || TX_RC=$?
echo ">>> TX exited with code $TX_RC"

echo ""
if [[ $RX_RC -eq 0 && $TX_RC -eq 0 ]]; then
    echo "****** st40p gapped redundancy test PASSED ******"
else
    echo "****** st40p gapped redundancy test FAILED (TX=$TX_RC, RX=$RX_RC) ******"
    exit 1
fi
