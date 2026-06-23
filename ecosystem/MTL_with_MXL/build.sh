#!/usr/bin/env bash
# Build all MTL-MXL demo pipelines (poc, poc_14, poc_8k).
#
# Usage:
#   ./build.sh -m /path/to/mxl/build/Linux-GCC-Release
#
# Prerequisites:
#   - MTL installed system-wide (pkg-config mtl)
#   - MXL SDK built (pass build prefix via -m)
#   - SVT-JPEG-XS installed (for poc_8k)
#   - libjpeg-turbo, libcurl, pthreads
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MXL_ROOT=""
BUILD_TYPE="RelWithDebInfo"
JOBS="$(nproc)"

usage() {
	cat <<EOF
Usage: $0 -m <MXL_BUILD_PREFIX> [-t <BUILD_TYPE>] [-j <JOBS>]

Options:
  -m <path>   MXL SDK build prefix (e.g. mxl-sdk/build/Linux-GCC-Release)
  -t <type>   CMake build type (default: RelWithDebInfo)
  -j <N>      Parallel jobs (default: nproc)
  -h          Show this help
EOF
	exit 1
}

while getopts "m:t:j:h" opt; do
	case "$opt" in
	m) MXL_ROOT="$OPTARG" ;;
	t) BUILD_TYPE="$OPTARG" ;;
	j) JOBS="$OPTARG" ;;
	h | *) usage ;;
	esac
done

if [[ -z "$MXL_ROOT" ]]; then
	echo "Error: MXL_ROOT is required. Use -m <path>." >&2
	usage
fi

MXL_ROOT="$(cd "$MXL_ROOT" && pwd)"

build_pipeline() {
	local name="$1"
	local src_dir="$SCRIPT_DIR/$name"
	local build_dir="$src_dir/build"

	echo "══════════════════════════════════════════════"
	echo " Building $name"
	echo "══════════════════════════════════════════════"

	mkdir -p "$build_dir"
	cmake -S "$src_dir" -B "$build_dir" \
		-DMXL_ROOT="$MXL_ROOT" \
		-DCMAKE_BUILD_TYPE="$BUILD_TYPE"
	cmake --build "$build_dir" -j "$JOBS"

	echo " $name: OK"
	echo ""
}

build_pipeline poc
build_pipeline poc_14
build_pipeline poc_8k

echo "All pipelines built successfully."
echo "Binaries:"
echo "  poc/build/    — synthetic_st20_tx, mtl_to_mxl_sender, mxl_sink_receiver"
echo "  poc_14/build/ — synthetic_st20_tx_14, mtl_to_mxl_sender_14, mxl_sink_receiver_14"
echo "  poc_8k/build/ — poc_8k_compositor, poc_8k_compositor_tx, poc_8k_sender, poc_8k_receiver"
