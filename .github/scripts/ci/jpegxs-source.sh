#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

operation=${1:?usage: jpegxs-source.sh path ROOT REVISION | validate PATH REVISION}
path=${2:?}
revision=${3:?}

case "$operation" in
path) printf '%s/SVT-JPEG-XS-%s\n' "$path" "$revision" ;;
validate)
	[ -f "${path}/CMakeLists.txt" ]
	[ -f "${path}/.mtl-revision" ]
	[ "$(cat "${path}/.mtl-revision")" = "$revision" ]
	;;
*) exit 2 ;;
esac