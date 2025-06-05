#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -e
VERSIONS_ENV_PATH="$(dirname "$(readlink -qe "${BASH_SOURCE[0]}")")/../../versions.env"

if [ -f "$VERSIONS_ENV_PATH" ]; then
	# shellcheck disable=SC1090
	. "$VERSIONS_ENV_PATH"
else
	echo -e "Error: versions.env file not found at $VERSIONS_ENV_PATH"
	exit 1
fi

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
set -x
cd "${script_folder}"

# check out librist code
rm librist -rf
git clone https://code.videolan.org/rist/librist.git
cd librist
git checkout "${LIBRIST_COMMIT_VER}"

# apply the patches
git am ../*.patch

# build now
meson build
ninja -C build
