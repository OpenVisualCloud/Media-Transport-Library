#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -e

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}

archive_name="xdp-tools.tar.gz"
repo_dir="${script_folder}/xdp-tools"

(return 0 2>/dev/null) && sourced=1 || sourced=0

if [ "$sourced" -eq 0 ]; then
	pushd "${script_folder}" >/dev/null || exit 1

	if [ ! -d "${repo_dir}" ]; then
		echo "Clone XDP source code"
		git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git "${repo_dir}"
	fi

	pushd "${repo_dir}" >/dev/null || exit 1
	./configure
	make
	sudo make install
	pushd lib/libbpf/src >/dev/null || exit 1
	make
	sudo make install
	popd >/dev/null
	popd >/dev/null

	echo "Removing downloaded XDP sources"
	rm -rf "${repo_dir}"
	rm -f "${script_folder}/${archive_name}"

	popd >/dev/null
fi
