#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -e

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
cd "${script_folder}"

(return 0 2>/dev/null) && sourced=1 || sourced=0

if [ "$sourced" -eq 0 ]; then
	cd "${script_folder}"

	if [ ! -d "xdp-tools" ]; then
		echo "Clone XDP source code"
		git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git
	fi

	cd xdp-tools
	./configure
	make
	sudo make install
	cd lib/libbpf/src
	make
	sudo make install
fi
