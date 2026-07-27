#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Broad host setup for tests/validation/ pytest — builds the SEPARATE
# `.local_install` tree (see tests/validation/mtl_engine/const.py PREFIX)
# that RxTxApp/MtlManager/ffmpeg/gstreamer under pytest resolve against.
# This tree is independent from the system-wide build/ + /usr/local install
# used by gtest/KahawaiTest (that one is handled by the mtl-system-setup
# scripts/MCP server instead).
#
# This is the standalone-script equivalent of the (now legacy) MCP tool
# `setup_validation_base`. It is called by:
#   - validation_setup.sh (interactive/auto top-level entry point)
#   - the mtl-validation-setup MCP server (mtl_validation_mcp_server.py),
#     so the MCP tool and a human running this by hand share one
#     implementation instead of two.
#
# Stages: apt dependencies -> ICE driver check/rebuild -> DPDK+MTL(+plugins)
# build into .local_install -> hugepages -> CPU governor.
#
# == Inputs (env vars) ==============================================
#   NR_HUGEPAGES=2048         number of 2MB hugepages (4GB default)
#   BUILD_MODE=release        release|debug|debugonly
#   INCLUDE_FFMPEG_PLUGIN=0   1 to also build the FFmpeg plugin
#   INCLUDE_GSTREAMER_PLUGIN=0  1 to also build the GStreamer plugin
#   LOCAL_PREFIX=<repo>/.local_install/mtl   install prefix override
#   SKIP_APT=0                1 to skip apt dependency install (already done)
#   SKIP_ICE=0                1 to skip ICE driver check/rebuild
#
# Idempotent — safe to re-run; each stage is a thin wrapper around
# setup_environment.sh, which itself only does work that's missing.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root" || exit 1
# shellcheck source=lib/mtl_validation_discover.sh disable=SC1091
source "$repo_root/.github/scripts/lib/mtl_validation_discover.sh"

: "${NR_HUGEPAGES:=2048}"
: "${BUILD_MODE:=release}"
: "${INCLUDE_FFMPEG_PLUGIN:=0}"
: "${INCLUDE_GSTREAMER_PLUGIN:=0}"
: "${LOCAL_PREFIX:=$VD_LOCAL_PREFIX_DEFAULT}"
: "${SKIP_APT:=0}"
: "${SKIP_ICE:=0}"

CYN=$'\033[1;36m'
GRN=$'\033[1;32m'
RED=$'\033[1;31m'
CLR=$'\033[0m'
log() { printf '%s[validation_setup_base]%s %s\n' "$CYN" "$CLR" "$*" >&2; }
ok() { printf '%s[validation_setup_base] OK:%s %s\n' "$GRN" "$CLR" "$*" >&2; }
err() { printf '%s[validation_setup_base] FAIL:%s %s\n' "$RED" "$CLR" "$*" >&2; }

case "$BUILD_MODE" in
release | debug | debugonly) ;;
*)
	err "BUILD_MODE must be one of release/debug/debugonly, got '$BUILD_MODE'"
	exit 1
	;;
esac

log "════════════════════════════════════════════════════════════════════"
log " Broad validation host setup"
log "   local prefix          : $LOCAL_PREFIX"
log "   build mode            : $BUILD_MODE"
log "   nr_hugepages           : $NR_HUGEPAGES"
log "   ffmpeg plugin          : $INCLUDE_FFMPEG_PLUGIN"
log "   gstreamer plugin       : $INCLUDE_GSTREAMER_PLUGIN"
log "════════════════════════════════════════════════════════════════════"

# -------------------- Step 1: apt dependencies --------------------
if [[ "$SKIP_APT" == "1" ]]; then
	log "Step 1/5: apt dependencies — skipped (SKIP_APT=1)"
else
	log "Step 1/5: apt dependencies"
	mh_install_dependencies
fi

# -------------------- Step 2: ICE driver --------------------
if [[ "$SKIP_ICE" == "1" ]]; then
	log "Step 2/5: ICE driver — skipped (SKIP_ICE=1)"
elif vd_ice_driver_ok; then
	ok "Step 2/5: ICE driver already OK ($(vd_ice_live_version))"
else
	log "Step 2/5: ICE driver needs rebuild (required=$(vd_ice_required_version) live=$(vd_ice_live_version))"
	mh_ice_driver_rebuild
	if vd_ice_driver_ok; then
		ok "ICE driver rebuilt OK"
	else
		err "ICE driver still not OK after rebuild"
	fi
	log "NOTE: reloading ice destroys any existing VFs — recreate them if gtest/KahawaiTest needs VFs too"
fi

# -------------------- Step 3: DPDK + MTL (+plugins) build --------------------
log "Step 3/5: build DPDK + MTL into $LOCAL_PREFIX"
mtl_build_flag=1
mtl_build_debug_flag=0
if [[ "$BUILD_MODE" != "release" ]]; then
	mtl_build_flag=0
	mtl_build_debug_flag=1
fi
MTL_INSTALL_PREFIX="$LOCAL_PREFIX" \
	SETUP_ENVIRONMENT=0 \
	SETUP_BUILD_AND_INSTALL_DPDK=1 \
	SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 \
	MTL_BUILD_AND_INSTALL="$mtl_build_flag" \
	MTL_BUILD_AND_INSTALL_DEBUG="$mtl_build_debug_flag" \
	ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN="$INCLUDE_FFMPEG_PLUGIN" \
	ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN="$INCLUDE_GSTREAMER_PLUGIN" \
	bash .github/scripts/setup_environment.sh
sudo ldconfig 2>/dev/null || true

if vd_local_install_ready "$LOCAL_PREFIX"; then
	ok "Step 3/5: MtlManager + RxTxApp present under $LOCAL_PREFIX/bin"
else
	err "Step 3/5: $LOCAL_PREFIX/bin/{MtlManager,RxTxApp} missing after build"
	exit 1
fi

# -------------------- Step 4: hugepages --------------------
log "Step 4/5: hugepages"
mh_hugepages_set "$NR_HUGEPAGES"
vd_report_hugepages

# -------------------- Step 5: CPU governor --------------------
log "Step 5/5: CPU governor -> performance"
mh_cpu_governor_set_performance
if vd_cpu_governor_is_performance; then
	ok "Step 5/5: all CPUs at performance governor"
else
	err "Step 5/5: some CPUs not at performance governor"
fi

log "════════════════════════════════════════════════════════════════════"
ok "Broad validation host setup complete"
log "Next: run .github/scripts/setup_validation.sh (NFS/SSH/venv/configs),"
log "      or 'validation_setup.sh setup' to do both interactively."
log "════════════════════════════════════════════════════════════════════"
