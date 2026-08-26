#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
#
# ST2110 RxTxApp loopback — single process, TX on VF0 -> RX on VF1.
#
# Takes a stock loop_json scenario, retargets its two interfaces onto your VFs,
# points the media at a generated synthetic file (no NFS media needed), and runs
# RxTxApp for a fixed time. RX prints per-frame latency ("measure_latency").
#
# Needs two DPDK-bound VFs of the same PF, hugepages, and MtlManager running.
#
# Usage:
#   sudo ./st2110-test/run-rxtxapp-loopback.sh [MODE] [P_PORT] [R_PORT]
#     MODE   st20p (default) | video | st30p     -- ST2110-20 pipeline / raw / -30 audio
#   env: TEST_TIME (default 15s), MEDIA (reuse a real .yuv/.pcm instead of synthetic)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JSON_DIR="$REPO/tests/tools/RxTxApp/script/loop_json"
AUDIO_DIR="$REPO/tests/tools/RxTxApp/script/audio_json"

MODE="${1:-st20p}"
P_PORT="${2:-${P_PORT:-0000:af:01.0}}"
R_PORT="${3:-${R_PORT:-0000:af:01.1}}"
TEST_TIME="${TEST_TIME:-15}"

case "$MODE" in
st20p)
	SRC="$JSON_DIR/st20p_1v_1080p59.json"
	URLKEY=st20p_url
	FRAMES=3
	FRAMEBYTES=$((1920 * 1080 * 5 / 2))
	;;
video)
	SRC="$JSON_DIR/1080p59_1v.json"
	URLKEY=video_url
	FRAMES=3
	FRAMEBYTES=$((1920 * 1080 * 5 / 2))
	;;
st30p)
	SRC="$AUDIO_DIR/st30p_loop.json"
	URLKEY=audio_url
	FRAMES=1
	FRAMEBYTES=$((4 * 1024 * 1024))
	;;
*)
	echo "ERROR: MODE must be st20p | video | st30p" >&2
	exit 2
	;;
esac

# Locate RxTxApp: prefer the system install, fall back to the build tree.
BIN="$(command -v RxTxApp || true)"
[ -n "$BIN" ] || BIN="$REPO/tests/tools/RxTxApp/build/RxTxApp"
[ -x "$BIN" ] || {
	echo "ERROR: RxTxApp not found (build it with ./build.sh)." >&2
	exit 1
}
[ -f "$SRC" ] || {
	echo "ERROR: scenario $SRC not found." >&2
	exit 1
}
[ "$(id -u)" -eq 0 ] || echo "WARN: not root — VF access will fail." >&2

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
CFG="$WORK/config.json"

# Media: reuse a real file if given, else synthesize zeros of the right size.
if [ -n "${MEDIA:-}" ]; then
	MEDIA_FILE="$MEDIA"
	[ -f "$MEDIA_FILE" ] || {
		echo "ERROR: MEDIA=$MEDIA_FILE not found." >&2
		exit 1
	}
else
	MEDIA_FILE="$WORK/media.bin"
	echo "Generating $((FRAMES * FRAMEBYTES)) bytes of synthetic media -> $MEDIA_FILE"
	head -c "$((FRAMES * FRAMEBYTES))" /dev/zero >"$MEDIA_FILE"
fi

# Retarget the two interfaces onto the requested VFs and rewrite the media URL.
python3 - "$SRC" "$CFG" "$P_PORT" "$R_PORT" "$URLKEY" "$MEDIA_FILE" <<'PY'
import json, sys
src, dst, p, r, urlkey, media = sys.argv[1:7]
d = json.load(open(src))
d["interfaces"][0]["name"] = p
d["interfaces"][1]["name"] = r
for grp in ("tx_sessions", "rx_sessions"):
    for s in d.get(grp, []):
        for media_arr in s.values():
            if isinstance(media_arr, list):
                for m in media_arr:
                    if isinstance(m, dict) and urlkey in m:
                        m[urlkey] = media
json.dump(d, open(dst, "w"), indent=4)
PY

echo "== ST2110 RxTxApp loopback ($MODE) =="
echo "   P_PORT=$P_PORT (tx)  R_PORT=$R_PORT (rx)  test_time=${TEST_TIME}s  scenario=$(basename "$SRC")"
CMD=("$BIN" --config_file "$CFG" --test_time "$TEST_TIME" --log_level notice)
echo "+ ${CMD[*]}"
echo

LOG="$WORK/run.log"
"${CMD[@]}" 2>&1 | tee "$LOG"
rc="${PIPESTATUS[0]}"

echo
echo "-- version proof --"
grep -i "dpdk version:" "$LOG" || echo "WARN: no 'dpdk version:' line"
echo "-- rx summary --"
grep -iE "OK|fps|latency|error|wrong" "$LOG" | tail -n 15 || true
echo "-- exit code: $rc --"
exit "$rc"
