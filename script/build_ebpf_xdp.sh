#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -e
VERSIONS_ENV_PATH="$(dirname "$(readlink -qe "${BASH_SOURCE[0]}")")/../versions.env"

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

archive_name="archive.zip"
repo_dir="${script_folder}/xdp-tools"

(return 0 2>/dev/null) && sourced=1 || sourced=0

if [ "$sourced" -eq 0 ]; then
	pushd "${script_folder}" >/dev/null || exit 1

	if [ -d "${repo_dir}" ]; then
		echo "XDP \"$(realpath "$repo_dir")\" source directory already exists, please remove it first"
		exit 1
	fi

	echo "Clone XDP source code"
	wget -O "${archive_name}" "$XDP_REPO_URL"
	mkdir -p "${repo_dir}"
	unzip "${archive_name}" -d "${repo_dir}"
	mv "${repo_dir}"/xdp-tools-*/* "${repo_dir}"

	rm "${archive_name}"
	wget -O "${archive_name}" "$EBPF_REPO_URL"
	unzip "${archive_name}" -d "${repo_dir}"/lib/libbpf
	mv "${repo_dir}"/lib/libbpf/libbpf*/* "${repo_dir}"/lib/libbpf

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
