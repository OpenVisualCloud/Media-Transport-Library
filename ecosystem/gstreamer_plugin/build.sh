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

if [ -d "$BUILD_DIR" ]; then
	echo "Removing existing build directory..."
	rm -rf "$BUILD_DIR" || {
		echo "Failed to remove existing build directory"
		exit 1
	}
fi

if [ "$DEBUG" = true ]; then
	meson setup --buildtype=debug "$BUILD_DIR"
else
	meson setup "$BUILD_DIR"
fi

meson compile -C "$BUILD_DIR"
