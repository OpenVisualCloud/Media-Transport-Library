#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# One-shot pytest-specific preparation for tests/acceptance/.
#
# Broad host setup (apt, DPDK, ICE, MTL build, hugepages, CPU governor,
# optional plugins) is handled by MCP tool setup_acceptance_tests_base.
#
# This script intentionally keeps only pytest-custom responsibilities.
# Idempotent. Each stage is independent: probe -> install if missing -> verify.
# Stage failures print stage name, last command, last 20 lines of stderr, and a
# pointer to the failure-table row in
# .github/instructions/mtl-acceptance-tests.instructions.md.
#
# == Stages (default = ON unless noted) ===========================================
#   STAGE_PREFLIGHT=1   sanity checks and broad-prereq verification (no installs)
#   STAGE_NFS=1         MANDATORY when /mnt/media empty -> needs NFS_SOURCE
#   STAGE_SSH=1         passwordless ssh-to-root from invoking user
#   STAGE_VENV=1        tests/acceptance/venv + pip install requirements
#   STAGE_CONFIGS=1     tests/acceptance/configs/{topology,test}_config.yaml
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
#   EBU_IP           EBU LIST server IP for PCAP compliance analysis.
#                    OPTIONAL — only set this if the human explicitly asked
#                    for compliance/EBU checking (agent must ASK, never
#                    assume or guess an EBU server). Requires EBU_USER and
#                    EBU_PASSWORD too; all three are required together.
#   EBU_USER         EBU LIST server username (paired with EBU_IP).
#   EBU_PASSWORD     EBU LIST server password (paired with EBU_IP). Never
#                    placed on a command line — read from the environment
#                    only, so it doesn't leak into `ps` output.
#   CAPTURE_PCI_DEVICE  dedicated NIC PF BDF (different physical PF/card than
#                    PCI_DEVICE_BDF, e.g. 0000:15:00.1) used for netsniff-ng
#                    packet capture. Passed to gen_config.py as its own
#                    --capture_pci_device, kept separate from PCI_DEVICE_BDF
#                    so PCI_DEVICE_BDF may itself list 1+ DUT PF candidates
#                    (comma-separated, e.g. two PFs on a second card) without
#                    disturbing which NIC is used for capture — needed for
#                    PF-mode DUT tests that require a PF candidate not
#                    sharing an IOMMU group with the capture NIC. Compliance
#                    checking needs this in addition to EBU_IP — without a
#                    capture PF, no wire capture is possible and
#                    "compliance" stays false even with EBU creds set.
#   VERBOSE=0        when 1, stream wrapped-command stdout/stderr live; default
#                    captures it and only prints the tail on failure
#   CHECK_ONLY=0     when 1, every stage runs probes only and prints
#                    pass | would install | missing; never modifies the host
#
# == Expected wall time ===========================================================
#   Cold run (venv + configs + NFS + SSH)       : ~1-3 min total
#   Warm re-run (everything probed satisfied)    : <5s
#   NFS mount alone                             : <2s on LAN
#   Agents: stream output, do NOT time out at 60s.
# =================================================================================

set -uo pipefail # NOTE: no -e; we manage errors per stage

# -------------------- defaults --------------------
: "${STAGE_PREFLIGHT:=1}"
: "${STAGE_NFS:=1}"
: "${STAGE_SSH:=1}"
: "${STAGE_VENV:=1}"
: "${STAGE_CONFIGS:=1}"

: "${TEST_TIME:=30}"
: "${NFS_SOURCE:=}"
: "${NFS_PERSIST:=0}"
: "${NFS_MOUNT_OPTS:=ro,vers=3,nolock,soft,timeo=50,retrans=2}"
: "${PCI_DEVICE_BDF:=}"
: "${CAPTURE_PCI_DEVICE:=}"
: "${EBU_IP:=}"
: "${EBU_USER:=}"
: "${EBU_PASSWORD:=}"
: "${SSH_KEY:=}"
: "${VERBOSE:=0}"
: "${CHECK_ONLY:=0}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root" || exit 1

# -------------------- run-log tee --------------------
RUN_LOG="/tmp/setup_acceptance-$(date -u +%Y%m%dT%H%M%SZ).log"
exec > >(tee -a "$RUN_LOG") 2>&1
# fd 3 is that same stream, saved for the few messages that must reach the
# operator even from inside a stage: run_stage captures a stage's stdout and
# stderr into a temp file and, when the stage succeeds with VERBOSE=0 (the
# default), deletes it — so an ordinary warn from a *successful* stage reaches
# neither the console nor the run log.
exec 3>&2

# -------------------- pretty output --------------------
RED=$'\033[1;31m'
YEL=$'\033[1;33m'
CYN=$'\033[1;36m'
GRN=$'\033[1;32m'
CLR=$'\033[0m'
log() { printf '%s[setup_acceptance]%s %s\n' "$CYN" "$CLR" "$*" >&2; }
warn() { printf '%s[setup_acceptance] WARN:%s %s\n' "$YEL" "$CLR" "$*" >&2; }
# Same as warn, but on fd 3 so it survives run_stage's output capture. For
# things the operator has to act on or know a file changed under them.
notice() { printf '%s[setup_acceptance] NOTE:%s %s\n' "$YEL" "$CLR" "$*" >&3; }
ok() { printf '%s[setup_acceptance] OK:%s %s\n' "$GRN" "$CLR" "$*" >&2; }
err() { printf '%s[setup_acceptance] FAIL:%s %s\n' "$RED" "$CLR" "$*" >&2; }

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
		err "        .github/instructions/mtl-acceptance-tests.instructions.md"
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
		printf '%s[setup_acceptance]%s  %-16s  %-14s  %ss\n' \
			"$CYN" "$CLR" "$s" "${STAGE_RESULT[$s]:-?}" "${STAGE_DURATION[$s]:-?}" >&2
	done
	log ""
	# Pytest (this framework) hard-requires the .local_install prefix tree —
	# see tests/acceptance/mtl_engine/const.py PREFIX=".local_install". This is
	# SEPARATE from the system-wide build/ + /usr/local tree gtest/KahawaiTest
	# uses; built via MCP tool setup_acceptance_tests_base/setup_acceptance_tests_full.
	log " .local_install/mtl/bin/RxTxApp    : $([[ -x .local_install/mtl/bin/RxTxApp ]] && echo OK || echo MISSING)"
	log " .local_install/mtl/bin/MtlManager : $([[ -x .local_install/mtl/bin/MtlManager ]] && echo OK || echo MISSING)"
	log " .local_install/mtl/lib*/libmtl.so : $([[ -f .local_install/mtl/lib64/libmtl.so || -f .local_install/mtl/lib/x86_64-linux-gnu/libmtl.so ]] && echo OK || echo MISSING)"
	log " .local_install/ffmpeg/bin/ffmpeg  : $([[ -x .local_install/ffmpeg/bin/ffmpeg ]] && echo OK || echo 'MISSING (only needed for application=ffmpeg tests)')"
	log " libdpdk (system, for gtest)       : $(pkg-config --modversion libdpdk 2>/dev/null || echo MISSING)"
	log " ice driver     : $(modinfo ice 2>/dev/null | awk '/^version:/ {print $2; exit}' || echo MISSING) @ $(modinfo -n ice 2>/dev/null || echo '<none>')"
	log " hugepages free : $(awk '/HugePages_Free/ {print $2*2 " MiB"}' /proc/meminfo)"
	if mountpoint -q /mnt/media; then
		log " /mnt/media     : $(findmnt -no SOURCE /mnt/media) ($(df -h /mnt/media | awk 'NR==2{print $5" used of "$2}'))"
		log " media files    : $(find /mnt/media -mindepth 1 -maxdepth 1 2>/dev/null | wc -l) entries"
	else
		log " /mnt/media     : NOT MOUNTED"
	fi
	log " venv           : $([[ -x tests/acceptance/venv/bin/python3 ]] && echo OK || echo MISSING)"
	log " configs        : $([[ -f tests/acceptance/configs/topology_config.yaml && -f tests/acceptance/configs/test_config.yaml ]] && echo OK || echo MISSING)"
	if [[ -f tests/acceptance/configs/test_config.yaml ]]; then
		log " compliance     : $(grep -m1 '^compliance:' tests/acceptance/configs/test_config.yaml | awk '{print $2}')"
	fi
	log " run log        : $RUN_LOG"
	log "════════════════════════════════════════════════════════════════════"
	trap_arm
}

# ============================================================================
# STAGE FUNCTIONS
# ============================================================================

stage_preflight() {
	local nic_count free_g cpus
	nic_count=$(lspci -nn 2>/dev/null | grep -cEi '8086:(1592|12d2|579d|1249)')
	if [[ "$nic_count" -eq 0 ]]; then
		warn "preflight: no Intel E810/E830/E825/E835 NIC detected; pytest will fail"
	else
		log "preflight: NIC E810/E830/E825/E835 PF count = $nic_count"
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
	if [[ -z "$bdf" && "$nic_count" -gt 0 ]]; then
		bdf="0000:$(lspci -nn | grep -Ei '8086:(1592|12d2|579d|1249)' | head -1 | awk '{print $1}')"
	fi
	if [[ -n "$bdf" ]]; then
		local drv numa
		drv=$(basename "$(readlink -f "/sys/bus/pci/devices/$bdf/driver" 2>/dev/null)" 2>/dev/null)
		numa=$(<"/sys/bus/pci/devices/$bdf/numa_node" 2>/dev/null)
		log "preflight: PF $bdf driver=${drv:-<none>} numa_node=${numa:-<none>}"
		if [[ "$numa" == "-1" ]]; then
			warn "preflight: PF numa_node=-1 (BIOS may need NUMA enabled)"
		fi
	fi

	# Broad setup must be done via MCP tool setup_acceptance_tests_base.
	local missing=0 free_mb governor ice_path mtl_found=0
	if ! pkg-config --exists libdpdk 2>/dev/null; then
		warn "preflight: libdpdk missing"
		missing=1
	fi
	if ldconfig -p 2>/dev/null | grep -Eq 'libmtl\.so(\s|$)'; then
		mtl_found=1
	elif [[ -f /usr/local/lib/x86_64-linux-gnu/libmtl.so || -f /usr/local/lib64/libmtl.so || -f /usr/local/lib/libmtl.so ]]; then
		mtl_found=1
	fi
	if ((!mtl_found)); then
		warn "preflight: libmtl.so missing in ld cache"
		missing=1
	fi
	# NOTE: pytest needs .local_install/mtl/bin/{MtlManager,RxTxApp}, built by
	# MCP tool setup_acceptance_tests_base/setup_acceptance_tests_full — a SEPARATE tree
	# from build/manager + tests/tools/RxTxApp/build used by gtest/KahawaiTest.
	if [[ ! -x .local_install/mtl/bin/MtlManager || ! -x .local_install/mtl/bin/RxTxApp ]]; then
		warn "preflight: .local_install/mtl/bin/{MtlManager,RxTxApp} missing (pytest needs this, not build/manager or tests/tools/RxTxApp/build)"
		missing=1
	fi
	ice_path=$(modinfo -n ice 2>/dev/null || true)
	if [[ "$ice_path" != *"/updates/"* ]]; then
		warn "preflight: out-of-tree ice driver not loaded (path=$ice_path)"
		missing=1
	fi
	free_mb=$(awk '/HugePages_Free/ {print $2*2}' /proc/meminfo)
	if ((free_mb < 1024)); then
		warn "preflight: hugepages free is ${free_mb} MiB (<1024 MiB)"
		missing=1
	fi
	governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)
	if [[ "$governor" != "performance" ]]; then
		warn "preflight: CPU governor is '$governor' (expected performance)"
		missing=1
	fi

	if ((missing)); then
		err "preflight: broad host prerequisites are missing"
		err "run MCP tool setup_acceptance_tests_base first, then rerun this script"
		return 1
	fi

	log "preflight: broad prerequisites look ready (managed by MCP)"
	return 0
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

[setup_acceptance] FAIL: STAGE_NFS=1 but NFS_SOURCE is empty.

  /mnt/media must contain the SMPTE reference YUV / WAV / PCM media used by
  almost every test under tests/acceptance/tests/single/. Without it nearly
  all tests SKIP with "Media file not present".

  Re-run with NFS_SOURCE=<host>:<export>, e.g.

      NFS_SOURCE=10.123.232.121:/mnt/NFS/mtl_assets/media \
        bash .github/scripts/setup_acceptance.sh

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
	local venv=tests/acceptance/venv
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
	"$venv/bin/pip" install -q -r tests/acceptance/requirements.txt
	if [[ -f tests/acceptance/common/integrity/requirements.txt ]]; then
		"$venv/bin/pip" install -q -r tests/acceptance/common/integrity/requirements.txt ||
			warn "venv: integrity extras failed (non-fatal)"
	fi
	"$venv/bin/python3" -c 'import pytest, pytest_mfd_config' ||
		{
			err "venv: pytest_mfd_config not importable"
			return 1
		}
}

# Read one scalar out of a top-level block of the existing test_config.yaml,
# e.g. _yaml_block_field ebu_server ebu_ip. An empty block name reads a
# top-level scalar instead: _yaml_block_field '' test_time. Prints nothing if
# the block or the key is absent. awk rather than a yaml parser because the
# blocks it reads are flat maps of scalars and that does not justify a second
# python startup. Takes the whole remainder of the line rather than $2, so a
# value containing spaces (an EBU password) survives; \047 is a single quote,
# spelled in octal to keep the awk program single-quotable in shell.
#
# Two things the value has to survive, because hand-written configs have both:
# quotes, and a trailing `# ...` comment. An unquoted scalar ends at the first
# ` #', so the comment is stripped; a quoted one ends at its closing quote, so
# anything after it — comment included — is dropped with the quotes.
_yaml_block_field() {
	awk -v block="$1" -v key="$2:" '
		BEGIN { top = (block == ""); if (!top) block = block ":" }
		!top && $1 == block { inside = 1; next }
		!top && inside && /^[^[:space:]]/ { inside = 0 }
		(top ? /^[^[:space:]]/ : inside) && $1 == key {
			sub(/^[[:space:]]*[^:]*:[[:space:]]*/, "")
			sub(/[[:space:]]*\r?$/, "")
			q = substr($0, 1, 1)
			if (q == "\"" || q == "\047") {
				rest = substr($0, 2)
				close_at = index(rest, q)
				if (close_at > 0) {
					$0 = substr(rest, 1, close_at - 1)
				}
			} else {
				sub(/[[:space:]]+#.*$/, "")
				sub(/[[:space:]]+$/, "")
			}
			print
			exit
		}
	' tests/acceptance/configs/test_config.yaml 2>/dev/null
}

# The two repairs below edit one key of an existing test_config.yaml in place.
# Regenerating for either would be wrong: gen_test_config() emits only the keys
# it knows about, so it cannot round-trip the ones only an operator can set --
# top-level interface_type (read at ~80 call sites across 40 files via
# test_config.get("interface_type", "VF")), capture_cfg.sniff_interface /
# sniff_interface_index / phc_sync / capture_time / silent, ebu_server.proxy --
# and unlike the regenerating triggers, these two fire on hosts where nothing
# changed and nothing was asked.

# Raise ramdisk.media.size_gib, leaving every other byte of the file alone.
# Anchored so 'size_gib:' must follow the leading whitespace directly, which is
# what makes it unable to match 'tmpfs_size_gib:' (a sibling under ramdisk:, at
# indent 2 — not at column 0) at any indent. It patches the *first* indented
# 'size_gib:' in the file, which is ramdisk.media's only while media: precedes
# any other block carrying that key, as every generated config has it. Fails
# without touching the file if no such line is there.
_raise_media_size_gib() {
	local want="$1" cfg=tests/acceptance/configs/test_config.yaml
	# Create the temp file with the config's own mode before the redirect
	# truncates it: this file stores ebu_server.password in plaintext, and an
	# operator who chmod'd it must not silently get 0644 back.
	cp --attributes-only --preserve=mode,ownership "$cfg" "$cfg.new" 2>/dev/null || :
	awk -v want="$want" '
		!patched && /^[[:space:]]+size_gib:/ {
			sub(/:.*$/, ": " want)
			patched = 1
		}
		{ print }
		END { exit patched ? 0 : 1 }
	' "$cfg" >"$cfg.new" || {
		rm -f "$cfg.new"
		return 1
	}
	mv "$cfg.new" "$cfg"
}

# Append the compliance opt-out gen_config.py's 'enable: false' branch exists to
# write. A config predating that branch names no sniff NIC anywhere, so false is
# the only value it could truthfully take; an operator who wants compliance
# passes EBU_IP plus --capture-pci-device, which regenerates instead. Appending
# a top-level key at column 0 closes whatever block preceded it.
_append_capture_disabled() {
	local cfg=tests/acceptance/configs/test_config.yaml
	[[ -z "$(tail -c1 "$cfg")" ]] || printf '\n' >>"$cfg"
	printf 'capture_cfg:\n  enable: false\n' >>"$cfg"
}

# True when the existing config's capture_cfg.sniff_pci_device already names the
# capture NIC the operator just passed. CAPTURE_PCI_DEVICE is a BDF while the
# config stores vendor:device (what the framework's PCIDevice parser wants), so
# resolve it the same way gen_config.py does before comparing. A BDF lspci
# cannot resolve counts as "not named": regenerating from the explicit flag is
# what the operator asked for, and only a regeneration can add the capture PF to
# topology_config.yaml's network_interfaces.
_config_names_capture_device() {
	local want="$1" stored
	stored=$(_yaml_block_field capture_cfg sniff_pci_device)
	[[ -n "$stored" ]] || return 1
	if [[ "$want" == *.* ]]; then
		want=$(lspci -s "${want#0000:}" -n 2>/dev/null | awk '{print $3}')
	fi
	[[ -n "$want" && "$stored" == "$want" ]]
}

stage_configs() {
	local detected_bdf detected_vendor_device cur_vd need_regen=0 repaired=0 would_repair=0
	local cfg=tests/acceptance/configs/test_config.yaml
	detected_bdf=$(lspci -nn | grep -Ei '8086:(1592|12d2|579d|1249)' | head -1 | awk '{print "0000:"$1}')
	if [[ -n "$detected_bdf" ]]; then
		detected_vendor_device=$(lspci -s "${detected_bdf#0000:}" -n 2>/dev/null | awk '{print $3}')
	fi

	if [[ -f tests/acceptance/configs/topology_config.yaml &&
		-f tests/acceptance/configs/test_config.yaml ]]; then
		cur_vd=$(grep -m1 'pci_device:' tests/acceptance/configs/topology_config.yaml | tr -d "' " | cut -d: -f2-)
		local cur_compliance cur_has_capture cur_media_gib want_media_gib
		local cur_test_time sized_test_time
		cur_compliance=$(grep -m1 '^compliance:' tests/acceptance/configs/test_config.yaml | awk '{print $2}')
		cur_has_capture=$(grep -qm1 '^capture_cfg:' tests/acceptance/configs/test_config.yaml && echo 1 || echo 0)
		# Whether a regeneration could actually turn compliance on. gen_config.py
		# sets compliance = has_ebu AND has_sniff, and the regeneration below
		# forces --no_capture whenever CAPTURE_PCI_DEVICE is unset, so EBU
		# credentials on their own never flip it — either the operator passed a
		# capture PF, or one is stored in the config for the carry-forward below
		# to pick up. Deciding this before the trigger is what keeps the trigger
		# convergent: without it, a host with EBU credentials and no capture PF —
		# a combination this script's header documents as supported — regenerates
		# on every warm re-run, and each regeneration drops the keys
		# gen_test_config() cannot express.
		local can_enable_compliance=0
		if [[ -n "$CAPTURE_PCI_DEVICE" ]] ||
			[[ -n "$(_yaml_block_field capture_cfg sniff_pci_device)" ]]; then
			can_enable_compliance=1
		fi
		if [[ -n "$EBU_IP" && "$cur_compliance" != "true" ]] && ((!can_enable_compliance)); then
			notice "configs: EBU_IP is set but no capture PF is known, so compliance stays disabled — pass --capture-pci-device to enable it"
		fi
		# 'size_gib:' under ramdisk.media, anchored so it cannot match
		# 'tmpfs_size_gib:'. want_media_gib is what gen_config.py would derive
		# for the window below on this host; empty if the probe cannot run, in
		# which case the size comparison below is skipped rather than guessed.
		cur_media_gib=$(grep -m1 -E '^[[:space:]]+size_gib:' tests/acceptance/configs/test_config.yaml | awk '{print $2}')
		# The window to size against is the LARGER of TEST_TIME and the
		# test_time already in the file. TEST_TIME defaults to 30 whether or not
		# --test-time was passed, while the file's value is what pytest will
		# actually run for -- so sizing against TEST_TIME alone would "raise"
		# the mount to 78 GiB on a config that runs for 60s and needs 148.
		# test_time itself is left as the file has it, and said so below: this
		# repair path exists precisely to touch one key.
		cur_test_time=$(_yaml_block_field '' test_time)
		sized_test_time="$TEST_TIME"
		if [[ "$cur_test_time" =~ ^[0-9]+$ ]] && ((cur_test_time > sized_test_time)); then
			sized_test_time="$cur_test_time"
		fi
		# The sizer's stderr is where it says the derived size had to be clamped
		# to half of RAM ("may hit ENOSPC"), so it goes to the console rather
		# than /dev/null — but only once a size came back, since before
		# stage_venv the same stderr is just an ImportError.
		local sizer_err
		sizer_err=$(mktemp)
		want_media_gib=$(cd tests/acceptance/configs 2>/dev/null &&
			"../venv/bin/python3" -c "import gen_config, sys; print(gen_config._media_ramdisk_gib(int(sys.argv[1])))" \
				"$sized_test_time" 2>"$sizer_err")
		if [[ -n "$want_media_gib" && -s "$sizer_err" ]]; then
			notice "configs: $(tr '\n' ' ' <"$sizer_err")"
		fi
		rm -f "$sizer_err"
		if [[ -z "$want_media_gib" && -n "$cur_media_gib" ]]; then
			notice "configs: could not run the ramdisk sizer (venv or gen_config.py unavailable) — ramdisk.media.size_gib left at $cur_media_gib, unchecked against a ${sized_test_time}s run"
		fi
		if [[ -n "$detected_vendor_device" && "$cur_vd" != "$detected_vendor_device" ]]; then
			warn "configs: stale pci_device '$cur_vd' != detected '$detected_vendor_device' — regenerating"
			need_regen=1
		elif [[ -n "$EBU_IP" && "$cur_compliance" != "true" ]] && ((can_enable_compliance)); then
			# Ordered ahead of the in-place repairs below: an operator who
			# passes EBU_IP is asking for a compliance-enabled config, which
			# only a regeneration can produce. Guarded so it fires only when the
			# regeneration can deliver it; when it cannot, this falls through to
			# the in-place repairs instead of rewriting the file to no effect.
			warn "configs: EBU_IP provided but compliance not yet enabled in existing config — regenerating"
			need_regen=1
		elif [[ -n "$CAPTURE_PCI_DEVICE" ]] && ! _config_names_capture_device "$CAPTURE_PCI_DEVICE"; then
			# Same reason: --capture-pci-device is only honoured by a
			# regeneration (it also has to reach topology_config.yaml's
			# network_interfaces), so without this trigger an explicitly passed
			# capture NIC is a silent no-op on an existing config — and the
			# repair path below would write 'capture_cfg.enable: false' in the
			# same run, the exact opposite of what was asked.
			warn "configs: --capture-pci-device=$CAPTURE_PCI_DEVICE is not the sniff device in the existing config — regenerating"
			need_regen=1
		else
			if [[ "$cur_has_capture" != "1" ]]; then
				# An ABSENT capture_cfg is read by the pcap_capture fixture as
				# "this host does compliance", so every test taking that
				# fixture fails with "ebu_server is not configured" instead of
				# running its data-path oracles. Configs predating
				# gen_config.py's explicit 'enable: false' have no capture_cfg
				# at all, and no env var announces that — so this cannot be
				# conditional on one.
				if [[ "$CHECK_ONLY" == "1" ]]; then
					notice "configs: no capture_cfg — pcap tests would hard-FAIL on 'ebu_server is not configured' (CHECK_ONLY=1, not repaired)"
					would_repair=1
				elif _append_capture_disabled; then
					notice "configs: no capture_cfg — appended 'capture_cfg: {enable: false}' so pcap tests skip the verdict instead of hard-FAILing"
					repaired=1
				else
					err "configs: no capture_cfg and it could not be appended to $cfg — add a 'capture_cfg:' block with 'enable: false' by hand"
					return 1
				fi
			fi
			# Only on this path: the regenerating branches above do apply
			# TEST_TIME, so there would be nothing half-applied to report.
			if [[ "$cur_test_time" =~ ^[0-9]+$ && "$cur_test_time" != "$TEST_TIME" ]]; then
				notice "configs: test_time in $cfg is ${cur_test_time}s, not the requested ${TEST_TIME}s — left as-is (delete the configs to regenerate at ${TEST_TIME}s); the ramdisk is sized for the longer of the two, ${sized_test_time}s"
			fi
			if [[ -z "$want_media_gib" ]]; then
				: # sizer unavailable; already reported above
			elif [[ ! "$cur_media_gib" =~ ^[0-9]+$ ]]; then
				# Either no anchored 'size_gib:' line (a flow-style
				# `media: {…, size_gib: N}` block, or no ramdisk block at all)
				# or a non-numeric value. Both leave nothing this repair can
				# compare or patch, and neither may reach the numeric test
				# below: under `set -u`-style strictness an unset/garbage
				# operand aborts the stage instead of reporting anything.
				notice "configs: could not read a numeric ramdisk.media.size_gib from $cfg (found '${cur_media_gib:-<no anchored size_gib: line>}') — left alone, unchecked against the ${want_media_gib} GiB a ${sized_test_time}s run needs; set it by hand or delete the configs to regenerate"
			elif ((cur_media_gib < want_media_gib)); then
				# Too small a media ramdisk is not a truncated artifact:
				# filesink reports ENOSPC as a pipeline error and the
				# byte-throughput oracles read the shortfall as an MTL delivery
				# failure, so a healthy run fails for want of disk. Only grows
				# it — a hand-raised size is kept.
				if [[ "$CHECK_ONLY" == "1" ]]; then
					notice "configs: ramdisk.media.size_gib=$cur_media_gib is below the ${want_media_gib} GiB a ${sized_test_time}s run needs (CHECK_ONLY=1, not repaired)"
					would_repair=1
				elif _raise_media_size_gib "$want_media_gib"; then
					notice "configs: raised ramdisk.media.size_gib $cur_media_gib -> $want_media_gib for a ${sized_test_time}s run"
					repaired=1
				else
					err "configs: ramdisk.media.size_gib=$cur_media_gib is below the ${want_media_gib} GiB a ${sized_test_time}s run needs and no 'size_gib:' line could be patched — raise it in $cfg by hand"
					return 1
				fi
			fi
			if ((repaired)); then
				log "configs: repaired in place — no other key touched"
			elif [[ -n "$detected_vendor_device" ]]; then
				log "configs: kept (already present, NIC=$cur_vd)"
			else
				log "configs: kept (already present)"
			fi
		fi
		if ((!need_regen)); then
			if ((would_repair)); then
				# CHECK_ONLY found repairs it deliberately did not apply, so the
				# config is still broken for pytest. Report that the way every
				# other stage reports it — rc=2, which run_stage renders as
				# "would-install" — instead of printing the findings above and
				# then a healthy summary row, which is what
				# mtl-acceptance-tests.instructions.md tells the operator names
				# the broken stage.
				return 2
			fi
			return 0
		fi
		# Regeneration writes the file from gen_test_config()'s fixed key set,
		# so anything only an operator can set is lost. Name those keys and keep
		# a copy instead of dropping them silently, and carry forward the two
		# things the env may not hold: EBU credentials and the sniff NIC.
		# Without the latter the regen forces --no_capture and compliance goes
		# false, silently downgrading a compliance-capable host.
		if [[ "$CHECK_ONLY" != "1" ]]; then
			# The copy is unconditional. The list below names the keys worth
			# calling out, but it cannot be exhaustive — anything gen_test_config()
			# does not emit is dropped — and a key missing from the list is
			# exactly the case where the operator has no warning and would need
			# the .bak most. One cp of a 1 KB file is the cheaper side of that.
			#
			# It also must not overwrite an earlier backup. A second regeneration
			# copies the ALREADY-regenerated file, so a fixed destination would
			# replace the only surviving copy of the keys the first regeneration
			# dropped — and silently, because by then those keys are gone from
			# the live file too, so the notice below is not taken. Numbering
			# rather than timestamping keeps that collision-free by
			# construction: two regenerations in the same second cannot land on
			# one name.
			local backup="$cfg.bak" backup_n=0
			while [[ -e "$backup" ]]; do
				backup="$cfg.bak.$((++backup_n))"
			done
			cp -f "$cfg" "$backup"
			local -a lost=()
			grep -q '^interface_type:' "$cfg" && lost+=(interface_type)
			grep -q '^[[:space:]]*sniff_interface:' "$cfg" && lost+=(capture_cfg.sniff_interface)
			grep -q '^[[:space:]]*sniff_interface_index:' "$cfg" && lost+=(capture_cfg.sniff_interface_index)
			grep -q '^[[:space:]]*phc_sync:' "$cfg" && lost+=(capture_cfg.phc_sync)
			grep -q '^[[:space:]]*capture_time:' "$cfg" && lost+=(capture_cfg.capture_time)
			grep -q '^[[:space:]]*frames_number:' "$cfg" && lost+=(capture_cfg.frames_number)
			grep -q '^[[:space:]]*packets_number:' "$cfg" && lost+=(capture_cfg.packets_number)
			grep -q '^[[:space:]]*silent:' "$cfg" && lost+=(capture_cfg.silent)
			[[ "$(_yaml_block_field ebu_server proxy)" =~ ^(false|)$ ]] || lost+=(ebu_server.proxy)
			if ((${#lost[@]})); then
				notice "configs: regeneration cannot express ${lost[*]} — previous file kept as $backup; re-apply those keys by hand"
			else
				log "configs: previous $cfg kept as $backup before regenerating"
			fi
		fi
		# Each of the three fields is carried forward on its own. Gating all three
		# on EBU_IP being empty lets a partial invocation destroy the other two:
		# EBU_PASSWORD is env-only — never a flag, so it cannot leak into ps — so
		# `--ebu-ip=X --ebu-user=Y` with EBU_PASSWORD unset leaves it empty,
		# gen_config.py's has_ebu = all([ip, user, password]) goes false, and the
		# whole ebu_server block is dropped from a config that had all three. The
		# operator asked to enable compliance and would instead lose the stored
		# password.
		local -a carried=()
		if [[ -z "$EBU_IP" ]]; then
			EBU_IP=$(_yaml_block_field ebu_server ebu_ip)
			[[ -z "$EBU_IP" ]] || carried+=("ebu_ip=$EBU_IP")
		fi
		if [[ -z "$EBU_USER" ]]; then
			EBU_USER=$(_yaml_block_field ebu_server user)
			[[ -z "$EBU_USER" ]] || carried+=("user=$EBU_USER")
		fi
		if [[ -z "$EBU_PASSWORD" ]]; then
			EBU_PASSWORD=$(_yaml_block_field ebu_server password)
			# Named, never echoed.
			[[ -z "$EBU_PASSWORD" ]] || carried+=(password)
		fi
		if ((${#carried[@]})); then
			log "configs: carrying forward ebu_server ${carried[*]} from the existing config"
		fi
		# gen_config.py passes an already-resolved vendor:device through
		# _bdf_to_vendor_device unchanged, so the stored value is a valid
		# --capture_pci_device. Only carried forward while that device is still
		# in the machine: the trigger above fires precisely when NICs moved, and
		# naming a departed sniff device turns every pcap test into a hard
		# "sniff_pci_device not found" instead of a green run with the
		# "Compliance check SUPPRESSED" warning.
		if [[ -z "$CAPTURE_PCI_DEVICE" ]]; then
			local stored_sniff
			stored_sniff=$(_yaml_block_field capture_cfg sniff_pci_device)
			if [[ -z "$stored_sniff" ]]; then
				:
			elif lspci -d "$stored_sniff" -n 2>/dev/null | grep -q .; then
				CAPTURE_PCI_DEVICE="$stored_sniff"
				log "configs: carrying forward capture_cfg sniff_pci_device=$CAPTURE_PCI_DEVICE"
			else
				notice "configs: stored sniff_pci_device=$stored_sniff is no longer present — compliance will be disabled; pass --capture-pci-device to re-enable it"
			fi
		fi
	fi
	check_only_or_install "configs" || return $?
	[[ -n "$PCI_DEVICE_BDF" ]] || PCI_DEVICE_BDF="$detected_bdf"
	[[ -n "$PCI_DEVICE_BDF" ]] || {
		err "configs: no E810/E830/E825/E835 PF found and PCI_DEVICE_BDF unset"
		return 1
	}
	[[ -n "$SSH_KEY" ]] || {
		err "configs: SSH_KEY not set (run STAGE_SSH first)"
		return 1
	}

	# Compliance checking needs a dedicated PF for netsniff-ng capture in
	# addition to EBU creds — pass it as its own --capture_pci_device so
	# gen_config.py's has_sniff (bool(capture_pci_device)) can go true.
	# PCI_DEVICE_BDF may itself be a comma-separated list of 1+ DUT PF
	# candidates; it is passed through untouched, separate from capture.
	# --no_capture is forced when CAPTURE_PCI_DEVICE is unset so
	# gen_config.py's legacy "2nd comma-separated --pci_device entry is the
	# sniff device" fallback never misfires against a multi-PF DUT list.
	local pci_device_arg="$PCI_DEVICE_BDF"
	local capture_args=(--no_capture)
	if [[ -n "$CAPTURE_PCI_DEVICE" ]]; then
		capture_args=(--capture_pci_device "$CAPTURE_PCI_DEVICE")
	fi

	local ebu_args=()
	if [[ -n "$EBU_IP" ]]; then
		ebu_args=(--ebu_ip "$EBU_IP" --ebu_user "$EBU_USER" --ebu_password "$EBU_PASSWORD")
	fi

	log "configs: gen_config.py PCI=$pci_device_arg CAPTURE_PCI=${CAPTURE_PCI_DEVICE:-<none>} KEY=$SSH_KEY TEST_TIME=$TEST_TIME$([[ -n "$EBU_IP" ]] && echo " EBU_IP=$EBU_IP")"
	# gen_config.py resolves each BDF to 'vendor:device' (what the framework's
	# PCIDevice parser wants, not a bus address) and assigns interface_index
	# scoped per vendor:device group itself, so no post-hoc patching is
	# needed here.
	# gen_config.py's stderr carries the one warning it can emit — the media
	# ramdisk had to be clamped to half of RAM, so heavy cases "may hit ENOSPC"
	# — and on this path run_stage would swallow it: it captures the stage's
	# output to a temp file and deletes it when the stage succeeds. So it is
	# re-emitted through notice (fd 3), the same way the size probe above does.
	local gen_err gen_rc=0
	gen_err=$(mktemp)
	(cd tests/acceptance/configs &&
		"../venv/bin/python3" gen_config.py \
			--session_id 0 --mtl_path "$repo_root" \
			--pci_device "$pci_device_arg" --ip_address 127.0.0.1 \
			--username root --key_path "$SSH_KEY" "${capture_args[@]}" \
			--media_path /mnt/media --test_time "$TEST_TIME" "${ebu_args[@]}") \
		2>"$gen_err" || gen_rc=$?
	if [[ -s "$gen_err" ]]; then
		if ((gen_rc)); then
			cat "$gen_err" >&2
		else
			notice "configs: $(tr '\n' ' ' <"$gen_err")"
		fi
	fi
	rm -f "$gen_err"
	return "$gen_rc"
}

# ============================================================================
# BANNER
# ============================================================================
log "════════════════════════════════════════════════════════════════════"
log " MTL acceptance_tests host preparation"
log "════════════════════════════════════════════════════════════════════"
log " stages enabled :"
for v in PREFLIGHT NFS SSH VENV CONFIGS; do
	stage_var="STAGE_$v"
	if [[ "${!stage_var}" == "1" ]]; then
		log "   ✓ $v"
	else
		log "   · $v (STAGE_$v=0)"
	fi
done
log " inputs         : NFS_SOURCE='${NFS_SOURCE:-<unset>}' PCI=${PCI_DEVICE_BDF:-<auto>} TEST_TIME=$TEST_TIME"
log " compliance     : EBU_IP=${EBU_IP:-<unset>} CAPTURE_PCI_DEVICE=${CAPTURE_PCI_DEVICE:-<unset>}"
log " mode           : $([[ "$CHECK_ONLY" == "1" ]] && echo 'CHECK_ONLY=1 (probe only, no install)' || echo install)"
log " run log        : $RUN_LOG"
log " expected time  : cold ~1-3 min ; warm <5s ; CHECK_ONLY <2s — agents must NOT time out"
log " note           : broad host setup is done by MCP tool setup_acceptance_tests_base"
log "════════════════════════════════════════════════════════════════════"

# Warn about NFS upfront, before slow stages run.
if [[ "$STAGE_NFS" == "0" ]]; then
	warn "STAGE_NFS=0 — most pytest cases under tests/single/ will SKIP."
	warn "Without /mnt/media populated, st20p/st22p/st30p/st40p/st41/ffmpeg/gstreamer/"
	warn "kernel_socket/ptp/rss_mode/virtio_user tests cannot run."
fi

# ============================================================================
# RUN STAGES
#   Order: cheap fast-fails first (preflight, NFS).
# ============================================================================
if [[ "$STAGE_PREFLIGHT" == "1" ]]; then run_stage preflight preflight stage_preflight; else skip_stage preflight "STAGE_PREFLIGHT=0"; fi
if [[ "$STAGE_NFS" == "1" ]]; then run_stage nfs "Media file not present" stage_nfs; else skip_stage nfs "STAGE_NFS=0"; fi
if [[ "$STAGE_SSH" == "1" ]]; then run_stage ssh "ssh to root" stage_ssh; else skip_stage ssh "STAGE_SSH=0"; fi
if [[ "$STAGE_VENV" == "1" ]]; then run_stage venv "venv" stage_venv; else skip_stage venv "STAGE_VENV=0"; fi
if [[ "$STAGE_CONFIGS" == "1" ]]; then run_stage configs "configs" stage_configs; else skip_stage configs "STAGE_CONFIGS=0"; fi

print_summary

log ""
log "Next: cd tests/acceptance && sudo -E ./venv/bin/python3 -m pytest \\"
log "        --topology_config=configs/topology_config.yaml \\"
log "        --test_config=configs/test_config.yaml \\"
log "        \"tests/single/st20p/test_fps.py::test_st20p_fps[|fps = p60|-Penguin_1080p-|application = rxtxapp|]\" \\"
log "        --tb=short -v"
