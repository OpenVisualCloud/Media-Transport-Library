#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
#
# ST2110 acceptance tests — set up the framework, then run the smoke suite.
#
# Thin wrapper over the repo's own single-source-of-truth scripts:
#   .github/scripts/acceptance_setup.sh   (builds the .local_install tree,
#     hugepages, ICE, NFS /mnt/media, SSH-to-localhost-root, venv, config YAMLs)
# then the canonical pytest invocation (venv python, both config flags).
#
# This MUTATES the host (builds DPDK+MTL into .local_install, mounts NFS,
# rewrites SSH keys, sets CPU governor). Read tasks.md "RULES" before an A/B run:
#   the loader cache is global and last-writer-wins — never run two trees at once.
#
# Usage:
#   sudo -E ./st2110-test/setup-acceptance.sh <PF_BDF> <NFS_SOURCE> [-- <extra pytest args>]
#     PF_BDF      NIC physical function, e.g. 0000:c9:00.0  (a PF, not a VF)
#     NFS_SOURCE  media export, e.g. 10.0.0.5:/mnt/NFS/mtl_assets/media
#
#   sudo ./st2110-test/setup-acceptance.sh --status     # read-only host report
#   sudo -E ./st2110-test/setup-acceptance.sh --run-only <PF_BDF>   # skip setup, just run smoke
#
# Discover a PF BDF:  ./script/nicctl.sh list   (or lspci | grep -i ethernet)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SETUP="$REPO/.github/scripts/acceptance_setup.sh"
ACC="$REPO/tests/acceptance"
PY="$ACC/venv/bin/python3"
TOPO="$ACC/configs/topology_config.yaml"
TEST="$ACC/configs/test_config.yaml"
MARK="${MARK:-smoke}"

[ -x "$SETUP" ] || {
	echo "ERROR: $SETUP missing." >&2
	exit 1
}

run_smoke() {
	[ -x "$PY" ] || {
		echo "ERROR: venv missing ($PY). Run setup first." >&2
		exit 1
	}
	[ -f "$TOPO" ] || {
		echo "ERROR: $TOPO missing. Run setup first." >&2
		exit 1
	}
	[ -f "$TEST" ] || {
		echo "ERROR: $TEST missing. Run setup first." >&2
		exit 1
	}
	echo "== acceptance: pytest -m $MARK =="
	local extra=()
	[ "${#EXTRA[@]}" -gt 0 ] && extra=("${EXTRA[@]}")
	(cd "$ACC" && "$PY" -m pytest \
		--topology_config="$TOPO" --test_config="$TEST" \
		-m "$MARK" -v "${extra[@]}")
}

# --- arg handling ---
EXTRA=()
case "${1:-}" in
--status) exec "$SETUP" status ;;
--run-only)
	shift
	run_smoke
	exit $?
	;;
"" | -h | --help)
	grep '^#' "$0" | sed 's/^# \{0,1\}//'
	exit 0
	;;
esac

PF_BDF="$1"
NFS_SOURCE="${2:-}"
[ -n "$NFS_SOURCE" ] || {
	echo "ERROR: NFS_SOURCE required (media export)." >&2
	exit 2
}
shift 2
while [ $# -gt 0 ]; do
	[ "$1" = "--" ] && {
		shift
		EXTRA=("$@")
		break
	}
	shift
done

echo "== acceptance setup (mutates host) =="
echo "   PF_BDF=$PF_BDF  NFS_SOURCE=$NFS_SOURCE"
"$SETUP" setup --auto --pf-bdf="$PF_BDF" --nfs-source="$NFS_SOURCE"

echo
run_smoke
