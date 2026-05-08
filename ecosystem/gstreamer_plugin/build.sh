#!/bin/bash

set -euo pipefail

BUILD_DIR="builddir"
DEBUG=false

SCRIPT_NAME=$(basename "${BASH_SOURCE[0]}")
SCRIPT_PATH=$(readlink -qe "${BASH_SOURCE[0]}")
SCRIPT_FOLDER=${SCRIPT_PATH/$SCRIPT_NAME/}
cd "${SCRIPT_FOLDER}" || exit 1

# Parse command-line arguments
for arg in "$@"; do
	case "$arg" in
	--debug)
		DEBUG=true
		shift
		;;
	*)
		shift
		;;
	esac
done

# Build prefix args when MTL_INSTALL_PREFIX is set (local install)
MTL_PREFIX_ARGS=""
if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
	MTL_PREFIX_ARGS="--prefix=$MTL_INSTALL_PREFIX --libdir=."
fi

if [ -d "$BUILD_DIR" ]; then
	echo "Removing existing build directory..."
	rm -rf "$BUILD_DIR" || {
		echo "Failed to remove existing build directory"
		exit 1
	}
fi

if [ "$DEBUG" = true ]; then
	meson setup --buildtype=debug ${MTL_PREFIX_ARGS:+$MTL_PREFIX_ARGS} "$BUILD_DIR"
else
	meson setup ${MTL_PREFIX_ARGS:+$MTL_PREFIX_ARGS} "$BUILD_DIR"
fi

meson compile -C "$BUILD_DIR"

# Install plugins
if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
	meson install -C "$BUILD_DIR"
