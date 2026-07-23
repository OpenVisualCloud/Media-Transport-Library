#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Top-level entry point to take a host to "ready to run
# tests/validation/tests/single/ pytest" — with NO agent/MCP/AI required.
#
# Usage:
#   validation_setup.sh status                 # discovery only, no changes
#   validation_setup.sh setup                   # interactive: discover, ask, apply
#   validation_setup.sh setup --auto [flags]    # non-interactive (CI / MCP use)
#   validation_setup.sh setup --base-only ...   # only the broad host-setup phase
#   validation_setup.sh setup --pytest-only ... # only the pytest-specific phase
#
# This script is the single source of truth for the workflow the
# "mtl-validation-setup" MCP server used to perform conversationally.
# It composes two existing, independently runnable pieces:
#   1. validation_setup_base.sh   — apt/ICE/DPDK/MTL(+plugins)/hugepages/governor
#   2. setup_validation.sh        — NFS media/localhost root SSH/venv/configs
# Discovery (read-only status probing) lives in
# lib/mtl_validation_discover.sh and is shared by both phases and by this
# script, so there is exactly one implementation of "is the ICE driver OK?",
# "which PFs exist?", etc. The MCP server shells out to this same script
# instead of re-implementing any of this logic in Python.
#
# See --help for the full flag list.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root" || exit 1
# shellcheck source=lib/mtl_validation_discover.sh disable=SC1091
source "$repo_root/.github/scripts/lib/mtl_validation_discover.sh"

CYN=$'\033[1;36m'
GRN=$'\033[1;32m'
YEL=$'\033[1;33m'
RED=$'\033[1;31m'
CLR=$'\033[0m'
log() { printf '%s[validation_setup]%s %s\n' "$CYN" "$CLR" "$*" >&2; }
warn() { printf '%s[validation_setup] WARN:%s %s\n' "$YEL" "$CLR" "$*" >&2; }
err() { printf '%s[validation_setup] FAIL:%s %s\n' "$RED" "$CLR" "$*" >&2; }
ok() { printf '%s[validation_setup] OK:%s %s\n' "$GRN" "$CLR" "$*" >&2; }

usage() {
	cat <<'EOF'
Usage:
  validation_setup.sh status                    Discovery-only report, no changes.
  validation_setup.sh setup [flags]              Interactive by default (tty); add
                                                  --auto for non-interactive use.

Flags for 'setup' (all optional; interactive mode prompts for anything unset):
  --auto                     Non-interactive: use flags/env only, never prompt.
                              Fails fast if a required value (e.g. NFS source,
                              when /mnt/media is empty) is missing.
  --yes                      Skip the final confirmation prompt (interactive mode).
  --base-only                Only run the broad host-setup phase.
  --pytest-only              Only run the pytest-specific phase (NFS/SSH/venv/configs).
  --pf-bdf=BDF               DUT NIC PF BDF, e.g. 0000:c9:00.0. May be a comma
                              separated list of DUT PF candidates.
  --nfs-source=HOST:/EXPORT  Media NFS export, e.g. 10.0.0.5:/mnt/NFS/mtl_assets/media
  --nfs-persist              Add an /etc/fstab entry so the NFS mount survives reboot.
  --nr-hugepages=N           Number of 2MB hugepages (default 2048).
  --build-mode=MODE          release|debug|debugonly (default release).
  --ffmpeg / --no-ffmpeg     Build the FFmpeg plugin into .local_install (default: on).
  --gstreamer / --no-gstreamer  Build the GStreamer plugin (default: off).
  --test-time=N              test_config.yaml::test_time in seconds (default 30).
  --ebu-ip=IP                EBU LIST server IP, for PCAP compliance checking.
  --ebu-user=USER            EBU LIST server username (paired with --ebu-ip).
                              Password: set EBU_PASSWORD in the environment,
                              never as a CLI flag (would leak into `ps`).
  --capture-pci-device=BDF   A second NIC PF (different from --pf-bdf) used for
                              netsniff-ng packet capture; required for compliance.
  --check-only               Probe-only for the pytest-specific stages; makes no
                              changes (passed through as CHECK_ONLY to
                              setup_validation.sh).
  -h, --help                 Show this help.

Examples:
  # Human, fully interactive, asked for everything:
  .github/scripts/validation_setup.sh setup

  # CI / MCP server, fully scripted:
  .github/scripts/validation_setup.sh setup --auto \
      --nfs-source=10.0.0.5:/mnt/NFS/mtl_assets/media --pf-bdf=0000:c9:00.0
EOF
}

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
AUTO=0
ASSUME_YES=0
BASE_ONLY=0
PYTEST_ONLY=0
PF_BDF=""
NFS_SOURCE=""
NFS_PERSIST=0
NR_HUGEPAGES=2048
BUILD_MODE=release
INCLUDE_FFMPEG=1
INCLUDE_GSTREAMER=0
TEST_TIME=30
EBU_IP=""
EBU_USER=""
CAPTURE_PCI_DEVICE=""
CHECK_ONLY=0

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
mode="${1:-}"
[[ $# -gt 0 ]] && shift
case "$mode" in
status | setup) ;;
-h | --help | "")
	usage
	exit 0
	;;
*)
	err "unknown mode '$mode' (expected 'status' or 'setup')"
	usage
	exit 1
	;;
esac

while [[ $# -gt 0 ]]; do
	case "$1" in
	--auto) AUTO=1 ;;
	--yes) ASSUME_YES=1 ;;
	--base-only) BASE_ONLY=1 ;;
	--pytest-only) PYTEST_ONLY=1 ;;
	--pf-bdf=*) PF_BDF="${1#*=}" ;;
	--nfs-source=*) NFS_SOURCE="${1#*=}" ;;
	--nfs-persist) NFS_PERSIST=1 ;;
	--nr-hugepages=*) NR_HUGEPAGES="${1#*=}" ;;
	--build-mode=*) BUILD_MODE="${1#*=}" ;;
	--ffmpeg) INCLUDE_FFMPEG=1 ;;
	--no-ffmpeg) INCLUDE_FFMPEG=0 ;;
	--gstreamer) INCLUDE_GSTREAMER=1 ;;
	--no-gstreamer) INCLUDE_GSTREAMER=0 ;;
	--test-time=*) TEST_TIME="${1#*=}" ;;
	--ebu-ip=*) EBU_IP="${1#*=}" ;;
	--ebu-user=*) EBU_USER="${1#*=}" ;;
	--capture-pci-device=*) CAPTURE_PCI_DEVICE="${1#*=}" ;;
	--check-only) CHECK_ONLY=1 ;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		err "unknown flag '$1'"
		usage
		exit 1
		;;
	esac
	shift
done

if [[ "$BASE_ONLY" == "1" && "$PYTEST_ONLY" == "1" ]]; then
	err "--base-only and --pytest-only are mutually exclusive"
	exit 1
fi

EBU_PASSWORD="${EBU_PASSWORD:-}"

# ---------------------------------------------------------------------------
# status mode
# ---------------------------------------------------------------------------
if [[ "$mode" == "status" ]]; then
	vd_full_report
	exit 0
fi

# ---------------------------------------------------------------------------
# setup mode
# ---------------------------------------------------------------------------
interactive=0
if [[ "$AUTO" == "0" && -t 0 ]]; then
	interactive=1
fi

log "════════════════════════════════════════════════════════════════════"
log " MTL validation host setup — current status"
log "════════════════════════════════════════════════════════════════════"
vd_full_report >&2
log "════════════════════════════════════════════════════════════════════"

# ask <var-name> <prompt> <default>  — interactive only; no-op under --auto.
ask() {
	local __var="$1" prompt="$2" default="$3" reply
	if [[ "$interactive" == "0" ]]; then
		return 0
	fi
	if [[ -n "$default" ]]; then
		read -rp "$prompt [$default]: " reply
	else
		read -rp "$prompt: " reply
	fi
	reply="${reply:-$default}"
	printf -v "$__var" '%s' "$reply"
}

ask_yn() {
	# ask_yn <var-name> <prompt> <default: y|n>
	local __var="$1" prompt="$2" default="$3" reply
	if [[ "$interactive" == "0" ]]; then
		return 0
	fi
	read -rp "$prompt [$([[ "$default" == y ]] && echo Y/n || echo y/N)]: " reply
	reply="${reply:-$default}"
	[[ "$reply" =~ ^[Yy] ]] && printf -v "$__var" '1' || printf -v "$__var" '0'
}

if [[ "$PYTEST_ONLY" != "1" ]]; then
	echo >&2
	log "-- Broad host setup options --"
	ask BUILD_MODE "Build mode (release/debug/debugonly)" "$BUILD_MODE"
	ask NR_HUGEPAGES "Number of 2MB hugepages" "$NR_HUGEPAGES"
	ask_yn INCLUDE_FFMPEG "Build FFmpeg plugin" "$([[ "$INCLUDE_FFMPEG" == 1 ]] && echo y || echo n)"
	ask_yn INCLUDE_GSTREAMER "Build GStreamer plugin" "$([[ "$INCLUDE_GSTREAMER" == 1 ]] && echo y || echo n)"
fi

if [[ "$BASE_ONLY" != "1" ]]; then
	echo >&2
	log "-- Pytest-specific setup options --"

	if [[ -z "$PF_BDF" ]]; then
		mapfile -t pf_rows < <(vd_list_pfs)
		if [[ ${#pf_rows[@]} -eq 0 ]]; then
			warn "no Intel E810/E830/E825/E835 PF auto-detected"
			ask PF_BDF "DUT NIC PF BDF (e.g. 0000:c9:00.0)" ""
		elif [[ ${#pf_rows[@]} -eq 1 ]]; then
			PF_BDF="${pf_rows[0]%%|*}"
			log "auto-selected PF: $PF_BDF (only candidate)"
			ask PF_BDF "DUT NIC PF BDF" "$PF_BDF"
		else
			if [[ "$interactive" == "1" ]]; then
				echo "Multiple PF candidates found:" >&2
				menu_idx=1
				for row in "${pf_rows[@]}"; do
					IFS='|' read -r bdf iface driver numa _totalvfs _numvfs <<<"$row"
					printf '  %d) %s  iface=%s driver=%s numa=%s\n' "$menu_idx" "$bdf" "$iface" "$driver" "$numa" >&2
					menu_idx=$((menu_idx + 1))
				done
				read -rp "Select PF number (or type a BDF): " reply
				if [[ "$reply" =~ ^[0-9]+$ && "$reply" -ge 1 && "$reply" -le ${#pf_rows[@]} ]]; then
					PF_BDF="${pf_rows[$((reply - 1))]%%|*}"
				else
					PF_BDF="$reply"
				fi
			else
				err "multiple PF candidates found and --pf-bdf not given in --auto mode:"
				vd_report_pfs >&2
				exit 1
			fi
		fi
	fi

	if vd_nfs_media_ok; then
		log "NFS media already present — not prompting for NFS_SOURCE"
	elif [[ -z "$NFS_SOURCE" ]]; then
		if [[ "$interactive" == "1" ]]; then
			while [[ -z "$NFS_SOURCE" ]]; do
				read -rp "NFS media source (host:/export), REQUIRED: " NFS_SOURCE
			done
		else
			err "/mnt/media has no media and --nfs-source not given in --auto mode"
			exit 1
		fi
	fi

	ask TEST_TIME "test_config.yaml test_time (seconds)" "$TEST_TIME"

	if [[ -z "$EBU_IP" && "$interactive" == "1" ]]; then
		want_ebu=0
		ask_yn want_ebu "Enable EBU LIST PCAP compliance checking" n
		if [[ "$want_ebu" == "1" ]]; then
			ask EBU_IP "EBU LIST server IP" ""
			ask EBU_USER "EBU LIST server username" ""
			read -rsp "EBU LIST server password: " EBU_PASSWORD
			echo >&2
			ask CAPTURE_PCI_DEVICE "Capture NIC PF BDF (2nd PF, different from $PF_BDF)" ""
		fi
	fi
fi

# ---------------------------------------------------------------------------
# Confirm
# ---------------------------------------------------------------------------
echo >&2
log "-- Plan --"
[[ "$PYTEST_ONLY" != "1" ]] && log "  base:   build-mode=$BUILD_MODE nr_hugepages=$NR_HUGEPAGES ffmpeg=$INCLUDE_FFMPEG gstreamer=$INCLUDE_GSTREAMER"
[[ "$BASE_ONLY" != "1" ]] && log "  pytest: pf_bdf=${PF_BDF:-<auto>} nfs_source=${NFS_SOURCE:-<already mounted>} test_time=$TEST_TIME ebu=$([[ -n "$EBU_IP" ]] && echo yes || echo no)"

if [[ "$interactive" == "1" && "$ASSUME_YES" != "1" ]]; then
	read -rp "Proceed? [Y/n]: " confirm
	if [[ -n "$confirm" && ! "$confirm" =~ ^[Yy] ]]; then
		log "aborted by user"
		exit 1
	fi
fi

# ---------------------------------------------------------------------------
# Execute
# ---------------------------------------------------------------------------
rc=0
if [[ "$PYTEST_ONLY" != "1" ]]; then
	NR_HUGEPAGES="$NR_HUGEPAGES" \
		BUILD_MODE="$BUILD_MODE" \
		INCLUDE_FFMPEG_PLUGIN="$INCLUDE_FFMPEG" \
		INCLUDE_GSTREAMER_PLUGIN="$INCLUDE_GSTREAMER" \
		bash "$repo_root/.github/scripts/validation_setup_base.sh" || rc=$?
	if [[ "$rc" != "0" ]]; then
		err "broad host setup failed (exit $rc)"
		exit "$rc"
	fi
fi

if [[ "$BASE_ONLY" != "1" ]]; then
	env_args=(
		TEST_TIME="$TEST_TIME"
		NFS_PERSIST="$NFS_PERSIST"
		CHECK_ONLY="$CHECK_ONLY"
	)
	[[ -n "$NFS_SOURCE" ]] && env_args+=(NFS_SOURCE="$NFS_SOURCE")
	[[ -n "$PF_BDF" ]] && env_args+=(PCI_DEVICE_BDF="$PF_BDF")
	[[ -n "$EBU_IP" ]] && env_args+=(EBU_IP="$EBU_IP" EBU_USER="$EBU_USER" EBU_PASSWORD="$EBU_PASSWORD")
	[[ -n "$CAPTURE_PCI_DEVICE" ]] && env_args+=(CAPTURE_PCI_DEVICE="$CAPTURE_PCI_DEVICE")
	env "${env_args[@]}" bash "$repo_root/.github/scripts/setup_validation.sh" || rc=$?
	if [[ "$rc" != "0" ]]; then
		err "pytest-specific setup failed (exit $rc)"
		exit "$rc"
	fi
fi

echo >&2
ok "Validation host setup complete"
log "Run e.g.:"
log "  cd tests/validation && sudo -E ./venv/bin/python3 -m pytest \\"
log "    --topology_config=configs/topology_config.yaml \\"
log "    --test_config=configs/test_config.yaml \\"
log "    tests/single/st20p/test_input_formats.py --tb=short -v"
