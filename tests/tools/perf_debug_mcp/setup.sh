#!/usr/bin/env bash
#═══════════════════════════════════════════════════════════════════════════════
# setup.sh — One-shot setup for perf_debug_mcp
#
# Target OS:  Ubuntu 22.04+ (also works on 24.04)
# Privileges: Requires root (or sudo) for package installation
#
# This script installs all system-level dependencies, toolchains, and
# optional subsystems needed by the MCP server.
#
# Usage:
#   chmod +x setup.sh
#   sudo ./setup.sh              # full install (all components)
#   sudo ./setup.sh --minimal    # skip EMON, PCM, Rust, eBPF — just core
#   sudo ./setup.sh --skip-pcm   # skip Intel PCM sensor server
#   sudo ./setup.sh --skip-emon  # skip Intel EMON/SEP
#   sudo ./setup.sh --skip-rust  # skip Rust toolchain / native eBPF helper
#
# After running this script:
#   npm install && npm run build
#   npm test       # 224 tests
#═══════════════════════════════════════════════════════════════════════════════
set -euo pipefail

# ─── Color helpers ───────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

info() { echo -e "${BLUE}[INFO]${NC}  $*"; }
ok() { echo -e "${GREEN}[OK]${NC}    $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err() { echo -e "${RED}[ERROR]${NC} $*"; }
section() { echo -e "\n${CYAN}═══ $* ═══${NC}"; }

# ─── Parse flags ─────────────────────────────────────────────────────────────
SKIP_PCM=false
SKIP_EMON=false
SKIP_RUST=false
NODE_MAJOR=22 # LTS version to install

for arg in "$@"; do
	case "$arg" in
	--minimal)
		SKIP_PCM=true
		SKIP_EMON=true
		SKIP_RUST=true
		;;
	--skip-pcm) SKIP_PCM=true ;;
	--skip-emon) SKIP_EMON=true ;;
	--skip-rust) SKIP_RUST=true ;;
	--help | -h)
		echo "Usage: sudo $0 [--minimal] [--skip-pcm] [--skip-emon] [--skip-rust]"
		exit 0
		;;
	*)
		err "Unknown flag: $arg"
		exit 1
		;;
	esac
done

# ─── Preflight checks ───────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
	err "This script must be run as root (or with sudo)."
	exit 1
fi

if ! grep -qiE 'ubuntu|debian' /etc/os-release 2>/dev/null; then
	warn "This script is designed for Ubuntu/Debian. Proceeding anyway..."
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NPROC=$(nproc)

section "System Update"
apt-get update -qq
ok "Package lists updated"

#═══════════════════════════════════════════════════════════════════════════════
# 1. Core system packages
#═══════════════════════════════════════════════════════════════════════════════
section "1/8 — Core System Packages"

CORE_PKGS=(
	# Build essentials
	build-essential
	git
	curl
	wget
	ca-certificates
	gnupg
	lsb-release
	# Shell utilities used by MCP tools via sshExecSafe / bash -c
	coreutils # cat, head, tail, basename, ls, test, etc.
	grep
	sed
	gawk
	findutils # find, xargs
	procps    # ps, pgrep, top
	iproute2  # ip, ss
	# /proc and /sys readers
	sysstat # mpstat, iostat, sar
	# Network utilities
	ethtool        # mtl_nic_pf_stats: ethtool -S/-i
	net-tools      # ifconfig (legacy, sometimes expected)
	socat          # serial-to-TCP bridge utility
	openssh-client # ssh for remote MTL tools
	# Python 3 (standard library only — no pip packages needed)
	python3
	python3-minimal
	# Serial/USB
	usbutils # lsusb
	# Hugepages (for DPDK/MTL tools to read /sys/kernel/mm/hugepages)
	libhugetlbfs-bin # hugeadm (optional but helpful)
	# Kernel headers (needed for eBPF CO-RE and EMON SEP drivers)
	"linux-headers-$(uname -r)"
	# Turbostat (MSR-based CPU frequency, C-state, SMI, power monitoring)
	linux-tools-common
	"linux-tools-$(uname -r)"
	# BCC/eBPF tracing tools (runqlat, offcputime, hardirqs, cpudist, etc.)
	bpfcc-tools       # ~80 BCC tools: runqlat-bpfcc, offcputime-bpfcc, hardirqs-bpfcc, etc.
	bpftrace          # high-level eBPF tracing language
	systemtap-sdt-dev # USDT probe headers (sys/sdt.h) — needed at MTL build time for USDT probes
	# RDMA diagnostics (rdma stat, rdma res show)
	rdma-core # rdma CLI tool for RoCE/IB counter inspection
	# Misc
	jq # handy for JSON processing
)

info "Installing ${#CORE_PKGS[@]} core packages..."
apt-get install -y -qq "${CORE_PKGS[@]}" 2>/dev/null || {
	# Some packages may not exist on all Ubuntu versions — install what we can
	warn "Some packages failed; retrying individually..."
	for pkg in "${CORE_PKGS[@]}"; do
		apt-get install -y -qq "$pkg" 2>/dev/null || warn "Could not install: $pkg (skipping)"
	done
}
ok "Core packages installed"

#═══════════════════════════════════════════════════════════════════════════════
# 2. Node.js (v22 LTS via NodeSource)
#═══════════════════════════════════════════════════════════════════════════════
section "2/8 — Node.js ${NODE_MAJOR}.x"

if command -v node &>/dev/null; then
	CURRENT_NODE=$(node -v | sed 's/^v//' | cut -d. -f1)
	if [[ "$CURRENT_NODE" -ge 18 ]]; then
		ok "Node.js $(node -v) already installed (meets >=18 requirement)"
	else
		warn "Node.js $(node -v) is too old — installing v${NODE_MAJOR}"
		INSTALL_NODE=true
	fi
else
	INSTALL_NODE=true
fi

if [[ "${INSTALL_NODE:-false}" == "true" ]]; then
	info "Installing Node.js ${NODE_MAJOR}.x from NodeSource..."

	# NodeSource gpg key + repo
	mkdir -p /etc/apt/keyrings
	curl -fsSL "https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key" |
		gpg --dearmor -o /etc/apt/keyrings/nodesource.gpg 2>/dev/null || true

	echo "deb [signed-by=/etc/apt/keyrings/nodesource.gpg] https://deb.nodesource.com/node_${NODE_MAJOR}.x nodistro main" \
		>/etc/apt/sources.list.d/nodesource.list

	apt-get update -qq
	apt-get install -y -qq nodejs
	ok "Node.js $(node -v) installed"
fi

# Verify npm
if ! command -v npm &>/dev/null; then
	err "npm not found after Node.js install"
	exit 1
fi
ok "npm $(npm -v) available"

#═══════════════════════════════════════════════════════════════════════════════
# 3. Intel PCM (pcm-sensor-server)
#═══════════════════════════════════════════════════════════════════════════════
section "3/8 — Intel PCM (Performance Counter Monitor)"

if [[ "$SKIP_PCM" == "true" ]]; then
	warn "Skipping Intel PCM (--skip-pcm or --minimal)"
else
	if command -v pcm-sensor-server &>/dev/null; then
		ok "pcm-sensor-server already installed at $(which pcm-sensor-server)"
	else
		info "Building Intel PCM from source..."

		PCM_BUILD_DIR=$(mktemp -d)
		trap 'rm -rf $PCM_BUILD_DIR' EXIT

		apt-get install -y -qq cmake
		git clone --depth 1 https://github.com/intel/pcm.git "$PCM_BUILD_DIR/pcm"
		mkdir -p "$PCM_BUILD_DIR/pcm/build"
		pushd "$PCM_BUILD_DIR/pcm/build" >/dev/null

		cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
		make -j"$NPROC" 2>&1 | tail -5

		# Install binaries
		cp -f bin/pcm-sensor-server /usr/local/bin/
		cp -f bin/pcm-iio /usr/local/bin/ 2>/dev/null || true
		cp -f bin/pcm /usr/local/bin/ 2>/dev/null || true
		cp -f bin/pcm-memory /usr/local/bin/ 2>/dev/null || true
		cp -f bin/pcm-pcie /usr/local/bin/ 2>/dev/null || true

		popd >/dev/null
		trap - EXIT
		rm -rf "$PCM_BUILD_DIR"

		ok "Intel PCM installed to /usr/local/bin/"
	fi

	# Create systemd service for pcm-sensor-server
	if [[ ! -f /etc/systemd/system/pcm-sensor-server.service ]]; then
		info "Creating systemd service for pcm-sensor-server..."

		cat >/etc/systemd/system/pcm-sensor-server.service <<'UNIT'
[Unit]
Description=Intel PCM Sensor Server (port 9738)
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/pcm-sensor-server -p 9738 -r --
Restart=on-failure
RestartSec=5
# PCM needs raw HW counter access
AmbientCapabilities=CAP_SYS_RAWIO CAP_DAC_READ_SEARCH
NoNewPrivileges=false

[Install]
WantedBy=multi-user.target
UNIT

		systemctl daemon-reload
		systemctl enable pcm-sensor-server.service
		ok "pcm-sensor-server.service created and enabled"
		info "Start with: systemctl start pcm-sensor-server"
	else
		ok "pcm-sensor-server.service already exists"
	fi

	# Verify pcm-iio is available (used by emon_pcie_topology fallback)
	if command -v pcm-iio &>/dev/null; then
		ok "pcm-iio available at $(which pcm-iio)"
	else
		warn "pcm-iio not found — emon_pcie_topology will use heuristic IIO stack mapping"
	fi
fi

#═══════════════════════════════════════════════════════════════════════════════
# 4. Intel EMON / SEP (optional — requires separate Intel download)
#═══════════════════════════════════════════════════════════════════════════════
section "4/8 — Intel EMON / SEP"

if [[ "$SKIP_EMON" == "true" ]]; then
	warn "Skipping Intel EMON setup (--skip-emon or --minimal)"
else
	EMON_FOUND=false
	for emon_path in \
		/opt/intel/sep/bin64/emon \
		/opt/intel/sep_private/bin64/emon \
		/usr/local/bin/emon \
		/usr/bin/emon; do
		if [[ -x "$emon_path" ]]; then
			EMON_FOUND=true
			ok "EMON found at $emon_path"
			break
		fi
	done

	if [[ "$EMON_FOUND" == "false" ]]; then
		warn "Intel EMON / SEP not found."
		warn ""
		warn "EMON requires a manual download from Intel:"
		warn "  1. Download Intel VTune or SEP from https://www.intel.com/content/www/us/en/developer/tools/oneapi/vtune-profiler-download.html"
		warn "  2. Install to /opt/intel/sep/"
		warn "  3. Load kernel drivers:  /opt/intel/sep/sepdk/src/insmod-sep   (or build with ./build-driver)"
		warn "  4. Set EMON_PATH env var if installed elsewhere"
		warn ""
		warn "EMON tools (emon_capabilities, emon_collect, emon_triage, emon_pcie_topology)"
		warn "will report EMON as unavailable until it is installed."
	else
		# Check if SEP kernel drivers are loaded
		if lsmod | grep -qE 'sep|socperf'; then
			ok "SEP/socperf kernel modules loaded"
		else
			warn "SEP kernel modules not loaded."
			warn "Load with:  /opt/intel/sep/sepdk/src/insmod-sep"
			warn "Or build them first:  cd /opt/intel/sep/sepdk/src && ./build-driver && ./insmod-sep"
		fi
	fi
fi

#═══════════════════════════════════════════════════════════════════════════════
# 5. Rust toolchain (for native eBPF helper — optional)
#═══════════════════════════════════════════════════════════════════════════════
section "5/8 — Rust Toolchain (native eBPF helper)"

if [[ "$SKIP_RUST" == "true" ]]; then
	warn "Skipping Rust toolchain (--skip-rust or --minimal)"
	warn "The server will run in pure fallback mode (no eBPF advanced mode)."
else
	if command -v cargo &>/dev/null; then
		ok "Rust toolchain already installed: $(cargo --version)"
	else
		info "Installing Rust toolchain via rustup..."
		# Install as the real user if running under sudo, otherwise as root
		REAL_USER="${SUDO_USER:-root}"
		REAL_HOME=$(eval echo "~$REAL_USER")

		if [[ "$REAL_USER" != "root" ]]; then
			su - "$REAL_USER" -c 'curl --proto "=https" --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable' || {
				warn "rustup install as $REAL_USER failed — trying as root"
				curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
			}
			# Source the env for this session
			export PATH="$REAL_HOME/.cargo/bin:$PATH"
		else
			curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
			export PATH="/root/.cargo/bin:$PATH"
		fi
		ok "Rust $(cargo --version 2>/dev/null || echo '(just installed)') installed"
	fi

	# eBPF build dependencies (for libbpf-rs / libbpf-sys)
	info "Installing eBPF build dependencies..."
	apt-get install -y -qq \
		clang \
		llvm \
		libelf-dev \
		zlib1g-dev \
		libbpf-dev \
		2>/dev/null || warn "Some eBPF build deps failed (may be OK if not using --features ebpf)"

	ok "eBPF build dependencies installed"
fi

#═══════════════════════════════════════════════════════════════════════════════
# 6. Hugepages configuration (for DPDK/MTL)
#═══════════════════════════════════════════════════════════════════════════════
section "6/8 — Hugepages Configuration"

# Check current hugepage state
HP_2M_TOTAL=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)
HP_1G_TOTAL=$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages 2>/dev/null || echo 0)

info "Current hugepages: 2MB pages=$HP_2M_TOTAL, 1GB pages=$HP_1G_TOTAL"

if [[ "$HP_2M_TOTAL" -eq 0 && "$HP_1G_TOTAL" -eq 0 ]]; then
	warn "No hugepages configured. DPDK/MTL requires hugepages."
	warn ""
	warn "To allocate 2MB hugepages (recommended for DPDK):"
	warn "  echo 2048 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
	warn ""
	warn "To make persistent, add to /etc/sysctl.conf:"
	warn "  vm.nr_hugepages = 2048"
	warn ""
	warn "For 1GB hugepages (better TLB coverage), add kernel boot param:"
	warn "  hugepagesz=1G hugepages=8 default_hugepagesz=1G"
	warn ""
	warn "Then mount hugetlbfs:"
	warn "  mkdir -p /dev/hugepages"
	warn "  mount -t hugetlbfs nodev /dev/hugepages"
else
	ok "Hugepages already configured"
fi

# Ensure hugetlbfs mount exists
if ! mountpoint -q /dev/hugepages 2>/dev/null; then
	warn "/dev/hugepages is not mounted"
	warn "Mount with:  mount -t hugetlbfs nodev /dev/hugepages"
	warn "Or add to /etc/fstab:  nodev /dev/hugepages hugetlbfs defaults 0 0"
else
	ok "/dev/hugepages mounted"
fi

#═══════════════════════════════════════════════════════════════════════════════
# 7. Serial relay (socat)
#═══════════════════════════════════════════════════════════════════════════════
section "7/8 — Serial Relay (socat)"

if command -v socat &>/dev/null; then
	ok "socat installed at $(which socat)"
else
	# Already in CORE_PKGS but just in case
	apt-get install -y -qq socat
	ok "socat installed"
fi

# Create a systemd service template for socat serial bridge
if [[ ! -f /etc/systemd/system/socat-serial-bridge@.service ]]; then
	info "Creating socat serial bridge systemd template..."
	cat >/etc/systemd/system/socat-serial-bridge@.service <<'UNIT'
[Unit]
Description=socat serial bridge (TCP %i → /dev/ttyUSB0)
After=network.target
ConditionPathExists=/dev/ttyUSB0

[Service]
Type=simple
# %i is the TCP listen port (e.g., 3333)
ExecStart=/usr/bin/socat TCP-LISTEN:%i,reuseaddr,fork FILE:/dev/ttyUSB0,b9600,cs8,raw,echo=0
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT

	systemctl daemon-reload
	ok "socat-serial-bridge@.service template created"
	info "Enable with: systemctl enable --now socat-serial-bridge@3333"
	info "This bridges TCP port 3333 ↔ /dev/ttyUSB0 (9600 baud)"
else
	ok "socat-serial-bridge template already exists"
fi

#═══════════════════════════════════════════════════════════════════════════════
# 8. Build MCP servers
#═══════════════════════════════════════════════════════════════════════════════
section "8/8 — Build MCP Servers"

if [[ -f "$SCRIPT_DIR/package.json" ]]; then
	info "Building perf-debug-mcp..."
	pushd "$SCRIPT_DIR" >/dev/null

	# npm install + build (as non-root user if possible)
	REAL_USER="${SUDO_USER:-root}"
	if [[ "$REAL_USER" != "root" ]]; then
		su - "$REAL_USER" -c "cd '$SCRIPT_DIR' && npm install --no-fund --no-audit 2>&1 | tail -3"
		su - "$REAL_USER" -c "cd '$SCRIPT_DIR' && npm run build 2>&1 | tail -3"
	else
		npm install --no-fund --no-audit 2>&1 | tail -3
		npm run build 2>&1 | tail -3
	fi
	ok "perf-debug-mcp built"

	# Build native Rust helper (optional)
	if [[ "$SKIP_RUST" == "false" && -f "$SCRIPT_DIR/native/Cargo.toml" ]]; then
		if command -v cargo &>/dev/null; then
			info "Building Rust native helper..."
			if [[ "$REAL_USER" != "root" ]]; then
				su - "$REAL_USER" -c "cd '$SCRIPT_DIR/native' && cargo build --release 2>&1 | tail -5" ||
					warn "Rust native build failed (non-critical — fallback mode will be used)"
			else
				(cd native && cargo build --release 2>&1 | tail -5) ||
					warn "Rust native build failed (non-critical — fallback mode will be used)"
			fi
			if [[ -f "$SCRIPT_DIR/native/target/release/cpu-debug-ebpf" ]]; then
				ok "Native helper built: native/target/release/cpu-debug-ebpf"
			fi
		fi
	fi

	popd >/dev/null
else
	warn "package.json not found in $SCRIPT_DIR — skipping build"
	warn "Run manually: cd <project_dir> && npm install && npm run build"
fi

#═══════════════════════════════════════════════════════════════════════════════
# Summary
#═══════════════════════════════════════════════════════════════════════════════
section "Setup Complete"

echo ""
echo -e "${GREEN}══════════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}                   Installation Summary${NC}"
echo -e "${GREEN}══════════════════════════════════════════════════════════════${NC}"
echo ""

row() { printf "  %-18s %s\n" "$1" "$2"; }

row "Node.js:" "$(node -v 2>/dev/null || echo 'NOT INSTALLED')"
row "npm:" "$(npm -v 2>/dev/null || echo 'NOT INSTALLED')"
row "Python:" "$(python3 --version 2>/dev/null || echo 'NOT FOUND')"

if [[ "$SKIP_PCM" == "true" ]]; then
	row "PCM:" "SKIPPED"
elif command -v pcm-sensor-server &>/dev/null; then
	row "PCM:" "INSTALLED ($(which pcm-sensor-server))"
else
	row "PCM:" "NOT FOUND"
fi

if [[ "$SKIP_EMON" == "true" ]]; then
	row "EMON/SEP:" "SKIPPED"
else
	EMON_LOC="NOT FOUND"
	for p in /opt/intel/sep/bin64/emon /opt/intel/sep_private/bin64/emon /usr/local/bin/emon; do
		[[ -x "$p" ]] && EMON_LOC="INSTALLED ($p)" && break
	done
	row "EMON:" "$EMON_LOC"
fi

if [[ "$SKIP_RUST" == "true" ]]; then
	row "Rust:" "SKIPPED"
else
	row "Rust:" "$(cargo --version 2>/dev/null || echo 'NOT FOUND')"
fi

row "socat:" "$(command -v socat &>/dev/null && echo 'INSTALLED' || echo 'NOT FOUND')"
row "ethtool:" "$(command -v ethtool &>/dev/null && echo 'INSTALLED' || echo 'NOT FOUND')"
row "turbostat:" "$(command -v turbostat &>/dev/null && echo "INSTALLED ($(turbostat --version 2>&1 | head -1))" || echo 'NOT FOUND')"
row "bcc-tools:" "$(command -v runqlat-bpfcc &>/dev/null && echo 'INSTALLED' || echo 'NOT FOUND')"
row "bpftrace:" "$(command -v bpftrace &>/dev/null && echo "INSTALLED ($(bpftrace --version 2>/dev/null | head -1))" || echo 'NOT FOUND')"
row "USDT (sdt.h):" "$(dpkg -s systemtap-sdt-dev &>/dev/null && echo 'INSTALLED' || echo 'NOT FOUND')"
row "rdma-core:" "$(command -v rdma &>/dev/null && echo 'INSTALLED' || echo 'NOT FOUND')"
row "Hugepages 2MB:" "${HP_2M_TOTAL} pages"
row "Hugepages 1GB:" "${HP_1G_TOTAL} pages"

echo ""
echo -e "${GREEN}══════════════════════════════════════════════════════════════${NC}"

echo ""
echo -e "${CYAN}Next steps:${NC}"
echo ""
echo "  1. Start PCM sensor server (if installed):"
echo "       sudo systemctl start pcm-sensor-server"
echo ""
echo "  2. Load EMON drivers (if installed):"
echo "       sudo /opt/intel/sep/sepdk/src/insmod-sep"
echo ""
echo "  3. Configure hugepages for DPDK (if not already done):"
echo "       echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
echo ""
echo "  4. Run the server:"
echo "       node dist/index.js                                # stdio mode"
echo "       node dist/index.js --tcp --port 3001              # TCP mode"
echo ""
echo "  5. Run tests:"
echo "       npm test                       # 224 tests"
echo ""
echo -e "${GREEN}Setup complete!${NC}"
