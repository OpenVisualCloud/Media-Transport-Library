#!/usr/bin/env bash
# build.sh — Build the perf-debug-mcp MCP server + native eBPF helper + BPF programs
#
# This script:
#   1. Builds the TypeScript MCP server (tsc)
#   2. Builds the Rust native helper with --features ebpf
#   3. Generates vmlinux.h from kernel BTF (needed for CO-RE BPF programs)
#   4. Compiles BPF C programs (.bpf.c → .bpf.o) with clang
#   5. Places .bpf.o files alongside the binary for runtime discovery
#
# Prerequisites (installed by setup.sh):
#   - Node.js ≥18, npm
#   - Rust toolchain (cargo)
#   - clang, libbpf-dev, libelf-dev, zlib1g-dev
#   - bpftool (from linux-tools-common, for vmlinux.h generation)
#   - /sys/kernel/btf/vmlinux (kernel BTF — present on Ubuntu ≥20.04 w/ CONFIG_DEBUG_INFO_BTF)
#
# Flags:
#   --skip-ts       Skip TypeScript build
#   --skip-native   Skip Rust + BPF build entirely
#   --skip-ebpf     Build Rust without eBPF feature (default features only)
#   --force-vmlinux Regenerate vmlinux.h even if it already exists
set -euo pipefail
cd "$(dirname "$0")"

SKIP_TS=false
SKIP_NATIVE=false
SKIP_EBPF=false
FORCE_VMLINUX=false

for arg in "$@"; do
	case "$arg" in
	--skip-ts) SKIP_TS=true ;;
	--skip-native) SKIP_NATIVE=true ;;
	--skip-ebpf) SKIP_EBPF=true ;;
	--force-vmlinux) FORCE_VMLINUX=true ;;
	--help | -h)
		echo "Usage: $0 [--skip-ts] [--skip-native] [--skip-ebpf] [--force-vmlinux]"
		exit 0
		;;
	*)
		echo "Unknown flag: $arg"
		exit 1
		;;
	esac
done

# ─── 1. TypeScript build ────────────────────────────────────────────────────
if [[ "$SKIP_TS" == "false" ]]; then
	echo "=== Building TypeScript MCP server ==="
	npm run build
	echo "    TypeScript build complete."
else
	echo "=== Skipping TypeScript build (--skip-ts) ==="
fi

# ─── 2–5. Rust + eBPF build ─────────────────────────────────────────────────
if [[ "$SKIP_NATIVE" == "true" ]]; then
	echo "=== Skipping native build (--skip-native) ==="
	echo "    The server will run in pure fallback mode."
	echo "=== Build complete ==="
	exit 0
fi

if ! command -v cargo &>/dev/null; then
	echo "WARN: Rust toolchain not found — native helper not built."
	echo "      The server will run in pure fallback mode."
	echo "      Install Rust: curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
	echo "=== Build complete (TypeScript only) ==="
	exit 0
fi

NATIVE_DIR="native"
BPF_PROGRAMS_DIR="$NATIVE_DIR/src/ebpf/programs"
RELEASE_DIR="$NATIVE_DIR/target/release"

# ─── 2. Build Rust binary ───────────────────────────────────────────────────
echo "=== Building Rust native helper ==="

CARGO_FEATURES=""
if [[ "$SKIP_EBPF" == "false" ]]; then
	CARGO_FEATURES="--features ebpf"

	# Pin libbpf-sys for Rust < 1.82 compatibility (libbpf-sys ≥1.6 requires 1.82+)
	RUSTC_MINOR=$(rustc --version | grep -oP '1\.(\d+)' | head -1 | cut -d. -f2)
	if [[ "$RUSTC_MINOR" -lt 82 ]]; then
		echo "    Rust $(rustc --version | grep -oP '[\d.]+' | head -1) detected (< 1.82)"
		echo "    Pinning libbpf-sys to 1.5.0 for compatibility..."
		# Only pin if not already at 1.5.0
		if ! grep -q 'version = "1.5.0' "$NATIVE_DIR/Cargo.lock" 2>/dev/null ||
			! grep -A1 'name = "libbpf-sys"' "$NATIVE_DIR/Cargo.lock" | grep -q '1.5.0'; then
			(cd "$NATIVE_DIR" && cargo update libbpf-sys --precise '1.5.0+v1.5.0' 2>&1)
		else
			echo "    libbpf-sys already pinned to 1.5.0."
		fi
	fi
fi

pushd "$NATIVE_DIR" >/dev/null
# shellcheck disable=SC2086
cargo build --release $CARGO_FEATURES 2>&1
popd >/dev/null

if [[ ! -f "$RELEASE_DIR/cpu-debug-ebpf" ]]; then
	echo "ERROR: Rust build did not produce binary at $RELEASE_DIR/cpu-debug-ebpf"
	exit 1
fi
echo "    Binary: $RELEASE_DIR/cpu-debug-ebpf"

# If eBPF is skipped, we're done with the native build
if [[ "$SKIP_EBPF" == "true" ]]; then
	echo "    Built without eBPF feature (--skip-ebpf). No BPF programs to compile."
	echo "=== Build complete ==="
	exit 0
fi

# ─── 3. Generate vmlinux.h from kernel BTF ──────────────────────────────────
echo "=== Generating vmlinux.h from kernel BTF ==="

VMLINUX_H="$BPF_PROGRAMS_DIR/vmlinux.h"

if [[ -f "$VMLINUX_H" && "$FORCE_VMLINUX" == "false" ]]; then
	echo "    vmlinux.h already exists ($(wc -l <"$VMLINUX_H") lines). Use --force-vmlinux to regenerate."
else
	if [[ ! -f /sys/kernel/btf/vmlinux ]]; then
		echo "WARN: /sys/kernel/btf/vmlinux not found — kernel was built without CONFIG_DEBUG_INFO_BTF."
		echo "      eBPF CO-RE programs cannot be compiled. Skipping BPF build."
		echo "      The native helper will still work for non-eBPF subcommands."
		echo "=== Build complete (partial — no BPF objects) ==="
		exit 0
	fi

	if ! command -v bpftool &>/dev/null; then
		echo "WARN: bpftool not found — install linux-tools-common to generate vmlinux.h."
		echo "      Skipping BPF program compilation."
		echo "=== Build complete (partial — no BPF objects) ==="
		exit 0
	fi

	bpftool btf dump file /sys/kernel/btf/vmlinux format c >"$VMLINUX_H"
	echo "    Generated: $VMLINUX_H ($(wc -l <"$VMLINUX_H") lines)"
fi

# ─── 4. Compile BPF C programs ──────────────────────────────────────────────
echo "=== Compiling BPF C programs ==="

if ! command -v clang &>/dev/null; then
	echo "WARN: clang not found — cannot compile BPF programs."
	echo "      Install with: apt-get install -y clang libbpf-dev"
	echo "      The native helper will run but eBPF sched_snapshot will fail."
	echo "=== Build complete (partial — no BPF objects) ==="
	exit 0
fi

BPF_CFLAGS="-O2 -g -target bpf -D__TARGET_ARCH_x86 -I/usr/include/bpf -I$BPF_PROGRAMS_DIR"
BPF_BUILT=0

for bpf_src in "$BPF_PROGRAMS_DIR"/*.bpf.c; do
	[[ -f "$bpf_src" ]] || continue
	bpf_obj="${bpf_src%.c}.o"
	bpf_name=$(basename "$bpf_src")

	# Rebuild if source is newer than object (or object doesn't exist)
	if [[ "$bpf_obj" -nt "$bpf_src" ]] 2>/dev/null; then
		echo "    $bpf_name — up to date"
	else
		echo "    Compiling $bpf_name ..."
		# shellcheck disable=SC2086
		clang $BPF_CFLAGS -c "$bpf_src" -o "$bpf_obj"
		echo "    → $(basename "$bpf_obj") ($(stat -c%s "$bpf_obj") bytes)"
	fi
	BPF_BUILT=$((BPF_BUILT + 1))
done

if [[ "$BPF_BUILT" -eq 0 ]]; then
	echo "    No .bpf.c files found in $BPF_PROGRAMS_DIR"
fi

# ─── 5. Place BPF objects alongside binary ───────────────────────────────────
echo "=== Placing BPF objects for runtime discovery ==="

for bpf_obj in "$BPF_PROGRAMS_DIR"/*.bpf.o; do
	[[ -f "$bpf_obj" ]] || continue
	cp -f "$bpf_obj" "$RELEASE_DIR/"
	echo "    Copied $(basename "$bpf_obj") → $RELEASE_DIR/"
done

echo "=== Build complete ==="
echo "Start with:  node dist/index.js        (stdio / VS Code)"
echo "         or: node dist/index.js --tcp   (HTTP on port 3001)"
