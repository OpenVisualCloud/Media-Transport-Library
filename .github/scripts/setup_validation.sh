#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# One-shot host preparation for tests/validation/ pytest.
#
# Idempotent. Each stage is independent: probe -> install if missing -> verify.
# Stage failures print stage name, last command, last 20 lines of stderr, and a
# pointer to the failure-table row in
# .github/instructions/mtl-validation-tests.instructions.md.
#
# == Stages (default = ON unless noted) ===========================================
#   STAGE_PREFLIGHT=1   sanity checks (NIC, disk, nproc); never installs anything
#   STAGE_APT=1         apt build dependencies (delegates to setup_environment.sh)
#   STAGE_DPDK=1        DPDK build+install if pkg-config libdpdk missing
#   STAGE_ICE=1         ICE driver build+install if /lib/modules/*/updates/drivers/net/ethernet/intel/ice/ice.ko missing
#   STAGE_MTL=1         libmtl + RxTxApp + MtlManager (./build.sh)
#   STAGE_HUGEPAGES=1   allocate 2 MiB hugepages if free < 1 GiB (4 GiB total)
#   STAGE_NFS=1         MANDATORY when /mnt/media empty -> needs NFS_SOURCE
#   STAGE_SSH=1         passwordless ssh-to-root from invoking user
#   STAGE_VENV=1        tests/validation/venv + pip install requirements
#   STAGE_CONFIGS=1     tests/validation/configs/{topology,test}_config.yaml
#   STAGE_FFMPEG_PLUGIN=0   opt-in: ecosystem/ffmpeg_plugin/build.sh
#   STAGE_GST_PLUGIN=0      opt-in: ecosystem/gstreamer_plugin/build.sh
#
# == Inputs (env vars) ============================================================
#   NFS_SOURCE       host:/export, e.g. 10.123.232.121:/mnt/NFS/mtl_assets/media
#                    (the lab default is a SUGGESTION, never assumed; agent must
#                     ASK the user every run)
#   NFS_PERSIST=0    when 1, append /etc/fstab entry so reboot survives
#   NFS_MOUNT_OPTS=ro,vers=3,nolock,soft,timeo=50,retrans=2
#                    default mount options (read-only, NFSv3 to avoid lockd)
#   PCI_DEVICE_BDF   target NIC PF BDF, e.g. 0000:c9:00.0
#                    auto-picked from first 8086:1592 if unset
#   SSH_KEY          private key path; auto-picks first ~/.ssh/id_{ed25519,rsa,ecdsa}
#   TEST_TIME=30     test_config.yaml::test_time
#   VERBOSE=0        when 1, stream wrapped-command stdout/stderr live; default
#                    captures it and only prints the tail on failure
#   CHECK_ONLY=0     when 1, every stage runs probes only and prints
#                    pass | would install | missing; never modifies the host
#
# == Expected wall time ===========================================================
#   Cold install (no DPDK/ICE/MTL/venv on host) : ~7-10 min total
#       APT 30-60s | DPDK 2-4 min | ICE 30-90s | MTL 1-3 min | VENV 20-40s | rest <5s
#   Warm re-run (everything probed satisfied)   : <5s
#   NFS mount alone                             : <2s on LAN
#   Agents: stream output, do NOT time out at 60s.
# =================================================================================

set -uo pipefail # NOTE: no -e; we manage errors per stage

# -------------------- defaults --------------------
: "${STAGE_PREFLIGHT:=1}"
: "${STAGE_APT:=1}"
: "${STAGE_DPDK:=1}"
: "${STAGE_ICE:=1}"
: "${STAGE_MTL:=1}"
: "${STAGE_HUGEPAGES:=1}"
: "${STAGE_NFS:=1}"
: "${STAGE_SSH:=1}"
: "${STAGE_VENV:=1}"
: "${STAGE_CONFIGS:=1}"
: "${STAGE_FFMPEG_PLUGIN:=0}"
: "${STAGE_GST_PLUGIN:=0}"

: "${TEST_TIME:=30}"
: "${NFS_SOURCE:=}"
: "${NFS_PERSIST:=0}"
: "${NFS_MOUNT_OPTS:=ro,vers=3,nolock,soft,timeo=50,retrans=2}"
: "${PCI_DEVICE_BDF:=}"
: "${SSH_KEY:=}"
: "${VERBOSE:=0}"
: "${CHECK_ONLY:=0}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root" || exit 1

# -------------------- run-log tee --------------------
RUN_LOG="/tmp/setup_validation-$(date -u +%Y%m%dT%H%M%SZ).log"
exec > >(tee -a "$RUN_LOG") 2>&1

# -------------------- pretty output --------------------
RED=$'\033[1;31m'
YEL=$'\033[1;33m'
CYN=$'\033[1;36m'
GRN=$'\033[1;32m'
CLR=$'\033[0m'
log() { printf '%s[setup_validation]%s %s\n' "$CYN" "$CLR" "$*" >&2; }
warn() { printf '%s[setup_validation] WARN:%s %s\n' "$YEL" "$CLR" "$*" >&2; }
ok() { printf '%s[setup_validation] OK:%s %s\n' "$GRN" "$CLR" "$*" >&2; }
err() { printf '%s[setup_validation] FAIL:%s %s\n' "$RED" "$CLR" "$*" >&2; }

invoking_user="${SUDO_USER:-$USER}"
invoking_home=$(getent passwd "$invoking_user" | cut -d: -f6)

declare -A STAGE_DURATION # stage -> seconds
declare -A STAGE_RESULT   # stage -> ok|skip|fail
declare -a STAGE_ORDER

# -------------------- error context --------------------
# Capture the line + command that triggered ERR inside a stage so the trap can
# print useful diagnostics. Disabled inside our own helpers via trap_pause.
LAST_LINE=0
LAST_CMD=""
trap_arm() { trap 'LAST_LINE=$LINENO; LAST_CMD=$BASH_COMMAND' DEBUG; }
trap_pause() { trap - DEBUG; }
trap_arm

# -------------------- check-only helper --------------------
# Inside a stage_X function, call `check_only_or_install || return $?` after the
# probe block has decided that installation is needed. In CHECK_ONLY=1 mode it
# prints "would install" and short-circuits with rc=2 (treated as soft-fail by
# run_stage so subsequent stages still run).
check_only_or_install() {
	if [[ "$CHECK_ONLY" == "1" ]]; then
		warn "$1: would install (CHECK_ONLY=1) — skipping"
		return 2
	fi
	return 0
}

run_stage() {
	# run_stage <name> <hint_key> <function-or-command...>
	local name="$1"
	shift
	local hint_key="$1"
	shift
	STAGE_ORDER+=("$name")

	local out_file
	out_file=$(mktemp -t "stage-${name}.XXXXXX.out")
	local t0=$SECONDS
	log "── $name : start"
	local rc=0
	if [[ "$VERBOSE" == "1" ]]; then
		"$@" > >(tee -a "$out_file") 2>&1 || rc=$?
	else
		"$@" >"$out_file" 2>&1 || rc=$?
	fi
	local dt=$((SECONDS - t0))
	STAGE_DURATION[$name]=$dt

	trap_pause
	if [[ $rc -eq 0 ]]; then
		STAGE_RESULT[$name]=ok
		ok "$name : ${dt}s"
		rm -f "$out_file"
	elif [[ $rc -eq 2 && "$CHECK_ONLY" == "1" ]]; then
		STAGE_RESULT[$name]="would-install"
		warn "$name : would install (${dt}s)"
		rm -f "$out_file"
	else
		STAGE_RESULT[$name]=fail
		err "$name : EXIT $rc after ${dt}s"
		err "  hint: see failure table key '$hint_key' in"
		err "        .github/instructions/mtl-validation-tests.instructions.md"
		err "  last command : $LAST_CMD  (line $LAST_LINE)"
		err "  last 30 lines of stage output (full: $out_file ; run log: $RUN_LOG):"
		tail -n 30 "$out_file" | sed 's/^/    | /' >&2
		print_summary
		exit "$rc"
	fi
	trap_arm
}

skip_stage() {
	local name="$1" why="$2"
	STAGE_ORDER+=("$name")
	STAGE_DURATION[$name]=0
	STAGE_RESULT[$name]=skip
	log "── $name : skipped — $why"
}

print_summary() {
	trap_pause
	log ""
	log "════════════════════════════════════════════════════════════════════"
	log " stage             result          time"
	log " ─────             ──────          ────"
	local s
	for s in "${STAGE_ORDER[@]}"; do
		printf '%s[setup_validation]%s  %-16s  %-14s  %ss\n' \
			"$CYN" "$CLR" "$s" "${STAGE_RESULT[$s]:-?}" "${STAGE_DURATION[$s]:-?}" >&2
	done
	log ""
	log " RxTxApp        : $([[ -x tests/tools/RxTxApp/build/RxTxApp ]] && echo OK || echo MISSING)"
	log " MtlManager     : $([[ -x build/manager/MtlManager ]] && echo OK || echo MISSING)"
	log " libmtl.so      : $(ldconfig -p 2>/dev/null | grep -q libmtl.so && echo OK || echo MISSING)"
	log " libdpdk        : $(pkg-config --modversion libdpdk 2>/dev/null || echo MISSING)"
	log " ice driver     : $(modinfo ice 2>/dev/null | awk '/^version:/ {print $2; exit}' || echo MISSING) @ $(modinfo -n ice 2>/dev/null || echo '<none>')"
	log " hugepages free : $(awk '/HugePages_Free/ {print $2*2 " MiB"}' /proc/meminfo)"
	if mountpoint -q /mnt/media; then
		log " /mnt/media     : $(findmnt -no SOURCE /mnt/media) ($(df -h /mnt/media | awk 'NR==2{print $5" used of "$2}'))"
		log " media files    : $(find /mnt/media -mindepth 1 -maxdepth 1 2>/dev/null | wc -l) entries"
	else
		log " /mnt/media     : NOT MOUNTED"
	fi
	log " venv           : $([[ -x tests/validation/venv/bin/python3 ]] && echo OK || echo MISSING)"
	log " configs        : $([[ -f tests/validation/configs/topology_config.yaml && -f tests/validation/configs/test_config.yaml ]] && echo OK || echo MISSING)"
	log " run log        : $RUN_LOG"
	log "════════════════════════════════════════════════════════════════════"
	trap_arm
}

# ============================================================================
# STAGE FUNCTIONS
# ============================================================================

stage_preflight() {
	local nic_count free_g cpus
	nic_count=$(lspci -d 8086:1592 2>/dev/null | wc -l)
	if [[ "$nic_count" -eq 0 ]]; then
		warn "preflight: no Intel E810 NIC (8086:1592) detected; pytest will fail"
	else
		log "preflight: NIC E810 PF count = $nic_count"
	fi
	free_g=$(df -BG --output=avail / | tail -1 | tr -dc 0-9)
	if [[ "$free_g" -lt 10 ]]; then
		warn "preflight: only ${free_g}G free on /; recommend >= 10G"
	fi
	cpus=$(nproc)
	if [[ "$cpus" -lt 4 ]]; then
		warn "preflight: only ${cpus} CPUs; tests need >= 4"
	fi
	if ! sudo -n true 2>/dev/null; then
		err "preflight: sudo requires a password — re-run after 'sudo -v' or configure NOPASSWD"
		return 1
	fi
	# Pending kernel upgrade — out-of-tree ice would build for the wrong kernel
	if [[ -e /var/run/reboot-required ]]; then
		warn "preflight: /var/run/reboot-required present — reboot first if kernel/header packages were updated"
	fi
	local running latest
	running=$(uname -r)
	latest=$(dpkg -l 'linux-image-[0-9]*' 2>/dev/null | awk '/^ii/ {print $2}' |
		sed 's/^linux-image-//' | sort -V | tail -1)
	if [[ -n "$latest" && "$latest" != "$running" ]]; then
		warn "preflight: running kernel $running but $latest is installed — reboot recommended before ice rebuild"
	fi
	# Selected PF status (informational)
	local bdf="$PCI_DEVICE_BDF"
	[[ -z "$bdf" && "$nic_count" -gt 0 ]] && bdf="0000:$(lspci -d 8086:1592 | head -1 | awk '{print $1}')"
	if [[ -n "$bdf" ]]; then
		local drv numa
		drv=$(basename "$(readlink -f "/sys/bus/pci/devices/$bdf/driver" 2>/dev/null)" 2>/dev/null)
		numa=$(<"/sys/bus/pci/devices/$bdf/numa_node" 2>/dev/null)
		log "preflight: PF $bdf driver=${drv:-<none>} numa_node=${numa:-<none>}"
		if [[ "$numa" == "-1" ]]; then
			warn "preflight: PF numa_node=-1 (BIOS may need NUMA enabled)"
		fi
	fi
	return 0
}

stage_apt() {
	if dpkg -l meson libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libssl-dev \
		systemtap-sdt-dev clang netsniff-ng unzip ethtool 2>/dev/null |
		awk '/^ii/{n++} END{exit !(n>=11)}'; then
		log "apt: required packages already installed"
		return 0
	fi
	check_only_or_install "apt" || return $?
	log "apt: invoking setup_environment.sh (apt deps only)"
	SETUP_ENVIRONMENT=1 \
		SETUP_BUILD_AND_INSTALL_DPDK=0 \
		SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 \
		SETUP_BUILD_AND_INSTALL_EBPF_XDP=0 \
		SETUP_BUILD_AND_INSTALL_GPU_DIRECT=0 \
		MTL_BUILD_AND_INSTALL=0 \
		bash .github/scripts/setup_environment.sh
}

dpdk_ldcache_ready() {
	local cache lib
	cache=$(ldconfig -p 2>/dev/null)
	for lib in librte_eal.so librte_ethdev.so librte_pcapng.so; do
		[[ "$cache" == *"$lib"* ]] || return 1
	done
}

stage_dpdk() {
	if pkg-config --exists libdpdk 2>/dev/null; then
		log "dpdk: pkg-config libdpdk = $(pkg-config --modversion libdpdk) — present"
		if ! dpdk_ldcache_ready; then
			check_only_or_install "dpdk ldconfig" || return $?
			warn "dpdk: pkg-config present but DPDK libs are missing from ld cache — running ldconfig"
			sudo ldconfig
		fi
		dpdk_ldcache_ready || {
			err "dpdk: DPDK shared libs still missing from ldconfig cache"
			return 1
		}
		return 0
	fi
	check_only_or_install "dpdk" || return $?
	log "dpdk: building & installing (script/build_dpdk.sh via setup_environment.sh)"
	SETUP_ENVIRONMENT=0 \
		SETUP_BUILD_AND_INSTALL_DPDK=1 \
		SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 \
		SETUP_BUILD_AND_INSTALL_EBPF_XDP=0 \
		SETUP_BUILD_AND_INSTALL_GPU_DIRECT=0 \
		MTL_BUILD_AND_INSTALL=0 \
		bash .github/scripts/setup_environment.sh
	sudo ldconfig
	pkg-config --exists libdpdk || {
		err "dpdk: pkg-config still missing libdpdk after install"
		return 1
	}
	dpdk_ldcache_ready || {
		err "dpdk: DPDK shared libs missing from ldconfig cache after install"
		return 1
	}
}

stage_ice() {
	# MTL needs the patched OUT-OF-TREE Intel ice driver (versions.env::ICE_VER).
	# The stock kernel ice does not support the iavf TM virtchnl messages MTL's
	# rate-limit pacing path uses; auto-pacing then segfaults inside the DPDK
	# iavf PMD. Detect by version AND module path; reload if wrong one is live.
	local want="${ICE_VER:-}"
	if [[ -z "$want" && -f versions.env ]]; then
		# shellcheck disable=SC1091
		want=$(. ./versions.env && echo "$ICE_VER")
	fi
	local ko
	ko="/lib/modules/$(uname -r)/updates/drivers/net/ethernet/intel/ice/ice.ko"
	local installed_ver=""
	if [[ -f "$ko" ]]; then
		installed_ver=$(modinfo "$ko" 2>/dev/null | awk '/^version:/ {print $2; exit}')
	fi
	local live_path live_ver
	live_path=$(modinfo -n ice 2>/dev/null)
	live_ver=$(modinfo ice 2>/dev/null | awk '/^version:/ {print $2; exit}')
	log "ice: want=${want:-?}  installed=${installed_ver:-<none>}@${ko}  live=${live_ver:-<none>}@${live_path:-<none>}"

	# If the patched module is already installed AND already live, nothing to do.
	if [[ -n "$installed_ver" && -n "$want" && "$installed_ver" == *"$want"* &&
		"$live_path" == "$ko" && "$live_ver" == "$installed_ver" ]]; then
		log "ice: out-of-tree driver $live_ver already installed and loaded"
		return 0
	fi

	# If the patched module is installed but the live one is the stock/wrong one,
	# reload it without rebuilding.
	if [[ -f "$ko" && "$live_path" != "$ko" ]]; then
		log "ice: patched driver present but stock module is loaded — reloading"
		check_only_or_install "ice (reload)" || return $?
		ice_reload "$ko" || return 1
		return 0
	fi

	# Otherwise build & install via setup_environment.sh, then reload.
	if [[ "$live_path" == */kernel/* ]]; then
		warn "ice: stock kernel ice ($live_ver) is loaded; MTL needs out-of-tree ${want:-MTL-patched} driver"
		warn "ice: with stock ice, RL pacing will SEGFAULT inside iavf_tm_node_add (DPDK iavf PMD)"
	fi
	check_only_or_install "ice (build)" || return $?
	log "ice: building & installing patched out-of-tree driver via setup_environment.sh"
	SETUP_ENVIRONMENT=0 \
		SETUP_BUILD_AND_INSTALL_DPDK=0 \
		SETUP_BUILD_AND_INSTALL_ICE_DRIVER=1 \
		SETUP_BUILD_AND_INSTALL_EBPF_XDP=0 \
		SETUP_BUILD_AND_INSTALL_GPU_DIRECT=0 \
		MTL_BUILD_AND_INSTALL=0 \
		bash .github/scripts/setup_environment.sh

	[[ -f "$ko" ]] || {
		err "ice: install finished but $ko is missing"
		return 1
	}
	ice_reload "$ko" || return 1
}

# Reload ice from the given .ko path. Refuses if PFs have active VFs in use
# (the operator must stop pytest / unbind VFs first).
ice_reload() {
	local ko="$1"
	# Detect VFs currently bound to vfio-pci or held open
	local busy=0 vfio_dev
	for vfio_dev in /dev/vfio/[0-9]*; do
		[[ -e "$vfio_dev" ]] || continue
		if sudo fuser "$vfio_dev" >/dev/null 2>&1; then
			busy=1
			break
		fi
	done
	if ((busy)); then
		err "ice: VFIO devices are in use (RxTxApp/MtlManager running?). Stop them first."
		err "     try: sudo pkill -9 RxTxApp MtlManager"
		return 1
	fi
	log "ice: depmod -a && rmmod irdma ice && modprobe ice"
	sudo depmod -a
	sudo rmmod irdma 2>/dev/null || true
	if ! sudo rmmod ice 2>/dev/null; then
		err "ice: rmmod ice failed (module busy). Free VFs/PFs and retry."
		return 1
	fi
	sudo modprobe ice || {
		err "ice: modprobe ice failed; check dmesg"
		return 1
	}
	local now_ver now_path
	now_ver=$(modinfo ice 2>/dev/null | awk '/^version:/ {print $2; exit}')
	now_path=$(modinfo -n ice 2>/dev/null)
	if [[ "$now_path" != "$ko" ]]; then
		err "ice: reload picked $now_path, not $ko"
		return 1
	fi
	log "ice: reloaded — live=$now_ver@$now_path"
}

stage_mtl() {
	if [[ -x build/manager/MtlManager && -x tests/tools/RxTxApp/build/RxTxApp ]] &&
		ldconfig -p 2>/dev/null | grep -q libmtl.so; then
		log "mtl: build/ populated and libmtl.so installed — skipping rebuild"
		return 0
	fi
	check_only_or_install "mtl" || return $?
	log "mtl: ./build.sh"
	./build.sh
	[[ -x tests/tools/RxTxApp/build/RxTxApp ]] || {
		log "mtl: RxTxApp not produced by build.sh — building separately"
		(cd tests/tools/RxTxApp && meson setup build && meson compile -C build)
	}
	[[ -x build/manager/MtlManager ]] || {
		err "mtl: build/manager/MtlManager missing"
		return 1
	}
	[[ -x tests/tools/RxTxApp/build/RxTxApp ]] || {
		err "mtl: RxTxApp missing"
		return 1
	}
}

stage_hugepages() {
	local free_mb
	free_mb=$(awk '/HugePages_Free/ {print $2*2}' /proc/meminfo)
	if ((free_mb < 1024)); then
		check_only_or_install "hugepages" || return $?
		log "hugepages: only ${free_mb} MiB free — allocating 2048 × 2 MiB"
		echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages >/dev/null
		free_mb=$(awk '/HugePages_Free/ {print $2*2}' /proc/meminfo)
	fi
	log "hugepages: ${free_mb} MiB free"
}

# Verify NFS export contains at least one canonical media file the framework references.
nfs_assert_has_media() {
	local f=ParkJoy_1920x1080_10bit_50Hz_P422_yuv422p10be_To_yuv422YCBCR10be.yuv
	if [[ ! -f "/mnt/media/$f" ]]; then
		warn "nfs: /mnt/media is mounted but expected file '$f' is absent."
		warn "     Pytests will SKIP with 'Media file not present'."
		warn "     Confirm that NFS_SOURCE points to the mtl_assets/media tree."
	else
		log "nfs: verified canonical media present (/mnt/media/$f)"
	fi
}

stage_nfs() {
	sudo mkdir -p /mnt/media
	if mountpoint -q /mnt/media; then
		log "nfs: /mnt/media already mounted from $(findmnt -no SOURCE /mnt/media)"
		nfs_assert_has_media
		return 0
	fi
	if [[ -n "$(ls -A /mnt/media 2>/dev/null)" ]]; then
		log "nfs: /mnt/media is non-empty (local files) — leaving alone"
		nfs_assert_has_media
		return 0
	fi
	if [[ -z "$NFS_SOURCE" ]]; then
		cat >&2 <<'EOF'

[setup_validation] FAIL: STAGE_NFS=1 but NFS_SOURCE is empty.

  /mnt/media must contain the SMPTE reference YUV / WAV / PCM media used by
  almost every test under tests/validation/tests/single/. Without it nearly
  all tests SKIP with "Media file not present".

  Re-run with NFS_SOURCE=<host>:<export>, e.g.

      NFS_SOURCE=10.123.232.121:/mnt/NFS/mtl_assets/media \
        bash .github/scripts/setup_validation.sh

  (The address above is only a known lab default — confirm with the human
  operator. Every host has a different storage server. Never assume.)

  To skip NFS knowingly (most tests will SKIP) set STAGE_NFS=0.

EOF
		return 1
	fi
	check_only_or_install "nfs" || return $?
	if ! command -v mount.nfs >/dev/null; then
		log "nfs: installing nfs-common (required for mount.nfs)"
		sudo apt-get install -y nfs-common
	fi
	log "nfs: mounting $NFS_SOURCE -> /mnt/media (opts: $NFS_MOUNT_OPTS)"
	sudo mount -t nfs -o "$NFS_MOUNT_OPTS" "$NFS_SOURCE" /mnt/media || {
		err "nfs: mount failed; check connectivity (ping ${NFS_SOURCE%%:*}) and export"
		return 1
	}
	if [[ "$NFS_PERSIST" == "1" ]]; then
		if ! grep -qF "$NFS_SOURCE /mnt/media" /etc/fstab; then
			log "nfs: appending /etc/fstab entry"
			echo "$NFS_SOURCE /mnt/media nfs $NFS_MOUNT_OPTS 0 0" | sudo tee -a /etc/fstab >/dev/null
		fi
	fi
	nfs_assert_has_media
}

stage_ssh() {
	if [[ -z "$SSH_KEY" ]]; then
		for cand in id_ed25519 id_rsa id_ecdsa; do
			if [[ -r "$invoking_home/.ssh/$cand" ]]; then
				SSH_KEY="$invoking_home/.ssh/$cand"
				break
			fi
		done
	fi
	# Fast path: existing key already authorized for root@127.0.0.1
	if [[ -n "$SSH_KEY" ]] && sudo -u "$invoking_user" ssh -i "$SSH_KEY" \
		-o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=3 \
		root@127.0.0.1 whoami 2>/dev/null | grep -qx root; then
		log "ssh: $invoking_user → root@127.0.0.1 already works (key=$SSH_KEY)"
		export SSH_KEY
		return 0
	fi
	check_only_or_install "ssh" || return $?
	if [[ -z "$SSH_KEY" ]]; then
		log "ssh: no key found — generating ed25519 for $invoking_user"
		sudo -u "$invoking_user" ssh-keygen -t ed25519 -N '' -f "$invoking_home/.ssh/id_ed25519"
		SSH_KEY="$invoking_home/.ssh/id_ed25519"
	fi
	local pub="${SSH_KEY}.pub"
	[[ -r "$pub" ]] || {
		err "ssh: ${pub} unreadable"
		return 1
	}
	local pubkey
	pubkey=$(<"$pub")
	if sudo grep -qF "$pubkey" /root/.ssh/authorized_keys 2>/dev/null; then
		log "ssh: pubkey already in /root/.ssh/authorized_keys"
	else
		log "ssh: appending ${pub} to /root/.ssh/authorized_keys"
		sudo mkdir -p /root/.ssh && sudo chmod 700 /root/.ssh
		printf '%s\n' "$pubkey" | sudo tee -a /root/.ssh/authorized_keys >/dev/null
		sudo chmod 600 /root/.ssh/authorized_keys
	fi
	if ! sudo -u "$invoking_user" ssh-keygen -F 127.0.0.1 >/dev/null 2>&1; then
		ssh-keyscan -H 127.0.0.1 2>/dev/null |
			sudo -u "$invoking_user" tee -a "$invoking_home/.ssh/known_hosts" >/dev/null
	fi
	if sudo -u "$invoking_user" ssh -i "$SSH_KEY" -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
		root@127.0.0.1 whoami 2>/dev/null | grep -qx root; then
		log "ssh: $invoking_user → root@127.0.0.1 verified"
	else
		err "ssh: passwordless ssh to root@127.0.0.1 still failing"
		return 1
	fi
	# Export so configs stage can use it
	export SSH_KEY
}

stage_venv() {
	local venv=tests/validation/venv
	if [[ -x "$venv/bin/python3" ]] &&
		"$venv/bin/python3" -c 'import pytest, pytest_mfd_config' 2>/dev/null; then
		log "venv: $venv present and pytest_mfd_config importable"
		return 0
	fi
	check_only_or_install "venv" || return $?
	if [[ ! -x "$venv/bin/python3" ]]; then
		log "venv: creating at $venv"
		python3 -m venv "$venv"
	fi
	log "venv: pip install requirements (quiet)"
	"$venv/bin/pip" install -q --upgrade pip
	"$venv/bin/pip" install -q -r tests/validation/requirements.txt
	if [[ -f tests/validation/common/integrity/requirements.txt ]]; then
		"$venv/bin/pip" install -q -r tests/validation/common/integrity/requirements.txt ||
			warn "venv: integrity extras failed (non-fatal)"
	fi
	"$venv/bin/python3" -c 'import pytest, pytest_mfd_config' ||
		{
			err "venv: pytest_mfd_config not importable"
			return 1
		}
}

stage_configs() {
	if [[ -f tests/validation/configs/topology_config.yaml &&
		-f tests/validation/configs/test_config.yaml ]]; then
		local detected_vd cur_vd
		detected_vd=$(lspci -d 8086:1592 2>/dev/null | head -1 | awk '{print "0000:"$1}')
		if [[ -n "$detected_vd" ]]; then
			cur_vd=$(grep -m1 'pci_device:' tests/validation/configs/topology_config.yaml | tr -d "' " | cut -d: -f2-)
			log "configs: kept (already present, NIC=$cur_vd)"
		else
			log "configs: kept (already present)"
		fi
		return 0
	fi
	check_only_or_install "configs" || return $?
	[[ -n "$PCI_DEVICE_BDF" ]] || PCI_DEVICE_BDF=$(lspci -d 8086:1592 2>/dev/null | head -1 | awk '{print "0000:"$1}')
	[[ -n "$PCI_DEVICE_BDF" ]] || {
		err "configs: no 8086:1592 PF found and PCI_DEVICE_BDF unset"
		return 1
	}
	[[ -n "$SSH_KEY" ]] || {
		err "configs: SSH_KEY not set (run STAGE_SSH first)"
		return 1
	}
	log "configs: gen_config.py PCI=$PCI_DEVICE_BDF KEY=$SSH_KEY TEST_TIME=$TEST_TIME"
	(cd tests/validation/configs &&
		"../venv/bin/python3" gen_config.py \
			--session_id 0 --mtl_path "$repo_root" \
			--pci_device "$PCI_DEVICE_BDF" --ip_address 127.0.0.1 \
			--username root --key_path "$SSH_KEY" --no_capture \
			--media_path /mnt/media --test_time "$TEST_TIME")
	local vendor_device
	vendor_device=$(lspci -s "${PCI_DEVICE_BDF#0000:}" -n 2>/dev/null | awk '{print $3}')
	if [[ -n "$vendor_device" ]]; then
		log "configs: patching pci_device → '$vendor_device' (framework wants vendor:device, not BDF)"
		sed -i "s|pci_device:.*|pci_device: '$vendor_device'|" tests/validation/configs/topology_config.yaml
	fi
}

stage_ffmpeg_plugin() {
	if compgen -G "ecosystem/ffmpeg_plugin/FFmpeg-release-*/ffmpeg" >/dev/null; then
		log "ffmpeg-plugin: already built"
		return 0
	fi
	check_only_or_install "ffmpeg-plugin" || return $?
	log "ffmpeg-plugin: ecosystem/ffmpeg_plugin/build.sh"
	(cd ecosystem/ffmpeg_plugin && ./build.sh)
}

stage_gst_plugin() {
	if compgen -G "ecosystem/gstreamer_plugin/builddir/libgstmtl_*.so" >/dev/null; then
		log "gst-plugin: already built"
		return 0
	fi
	check_only_or_install "gst-plugin" || return $?
	log "gst-plugin: ecosystem/gstreamer_plugin/build.sh"
	(cd ecosystem/gstreamer_plugin && ./build.sh)
}

# ============================================================================
# BANNER
# ============================================================================
log "════════════════════════════════════════════════════════════════════"
log " MTL validation host preparation"
log "════════════════════════════════════════════════════════════════════"
log " stages enabled :"
for v in PREFLIGHT NFS APT DPDK ICE MTL HUGEPAGES SSH VENV CONFIGS; do
	stage_var="STAGE_$v"
	if [[ "${!stage_var}" == "1" ]]; then
		log "   ✓ $v"
	else
		log "   · $v (STAGE_$v=0)"
	fi
done
for v in FFMPEG_PLUGIN GST_PLUGIN; do
	stage_var="STAGE_$v"
	if [[ "${!stage_var}" == "1" ]]; then
		log "   ✓ $v (opt-in)"
	else
		log "   · $v (opt-in via STAGE_$v=1)"
	fi
done
log " inputs         : NFS_SOURCE='${NFS_SOURCE:-<unset>}' PCI=${PCI_DEVICE_BDF:-<auto>} TEST_TIME=$TEST_TIME"
log " mode           : $([[ "$CHECK_ONLY" == "1" ]] && echo 'CHECK_ONLY=1 (probe only, no install)' || echo install)"
log " run log        : $RUN_LOG"
log " expected time  : cold ~7-10 min ; warm <5s ; CHECK_ONLY <2s — agents must NOT time out"
log "════════════════════════════════════════════════════════════════════"

# Warn about NFS upfront, before slow stages run.
if [[ "$STAGE_NFS" == "0" ]]; then
	warn "STAGE_NFS=0 — most pytest cases under tests/single/ will SKIP."
	warn "Without /mnt/media populated, st20p/st22p/st30p/st40p/st41/ffmpeg/gstreamer/"
	warn "kernel_socket/ptp/rss_mode/virtio_user tests cannot run."
fi

# ============================================================================
# RUN STAGES
#   Order: cheap fast-fails first (preflight, NFS) so a missing NFS_SOURCE
#   surfaces in <2s instead of after 7 min of DPDK/ICE/MTL builds.
# ============================================================================
if [[ "$STAGE_PREFLIGHT" == "1" ]]; then run_stage preflight preflight stage_preflight; else skip_stage preflight "STAGE_PREFLIGHT=0"; fi
if [[ "$STAGE_NFS" == "1" ]]; then run_stage nfs "Media file not present" stage_nfs; else skip_stage nfs "STAGE_NFS=0"; fi
if [[ "$STAGE_APT" == "1" ]]; then run_stage apt "apt deps" stage_apt; else skip_stage apt "STAGE_APT=0"; fi
if [[ "$STAGE_DPDK" == "1" ]]; then run_stage dpdk "DPDK build" stage_dpdk; else skip_stage dpdk "STAGE_DPDK=0"; fi
if [[ "$STAGE_ICE" == "1" ]]; then run_stage ice "ICE driver" stage_ice; else skip_stage ice "STAGE_ICE=0"; fi
if [[ "$STAGE_MTL" == "1" ]]; then run_stage mtl "MTL build" stage_mtl; else skip_stage mtl "STAGE_MTL=0"; fi
if [[ "$STAGE_HUGEPAGES" == "1" ]]; then run_stage hugepages hugepages stage_hugepages; else skip_stage hugepages "STAGE_HUGEPAGES=0"; fi
if [[ "$STAGE_SSH" == "1" ]]; then run_stage ssh "ssh to root" stage_ssh; else skip_stage ssh "STAGE_SSH=0"; fi
if [[ "$STAGE_VENV" == "1" ]]; then run_stage venv "venv" stage_venv; else skip_stage venv "STAGE_VENV=0"; fi
if [[ "$STAGE_CONFIGS" == "1" ]]; then run_stage configs "configs" stage_configs; else skip_stage configs "STAGE_CONFIGS=0"; fi
if [[ "$STAGE_FFMPEG_PLUGIN" == "1" ]]; then run_stage ffmpeg_plugin "ffmpeg plugin" stage_ffmpeg_plugin; else skip_stage ffmpeg_plugin "STAGE_FFMPEG_PLUGIN=0"; fi
if [[ "$STAGE_GST_PLUGIN" == "1" ]]; then run_stage gst_plugin "gst plugin" stage_gst_plugin; else skip_stage gst_plugin "STAGE_GST_PLUGIN=0"; fi

print_summary

log ""
log "Next: cd tests/validation && sudo -E ./venv/bin/python3 -m pytest \\"
log "        --topology_config=configs/topology_config.yaml \\"
log "        --test_config=configs/test_config.yaml \\"
log "        \"tests/single/st20p/fps/test_fps.py::test_fps[|fps = p50|-ParkJoy_1080p]\" \\"
log "        --tb=short -v"
