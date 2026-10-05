#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

# swig reads the MTL headers from this directory.
MTL_INCLUDE_DIR="/usr/local/include"

show_help() {
	cat <<EOF
Usage: ${script_name} [OPTIONS]

Build the SWIG Python binding of MTL in python/swig, and install it into the
system Python. swig reads the MTL headers from ${MTL_INCLUDE_DIR}/mtl.
Install MTL before you run this script.

REQUIRED PACKAGES (Debian/Ubuntu):
	gcc swig python3 python3-dev python3-setuptools
	MTL (build and install it with build.sh)

OPTIONS:
	-h		Show this help message

EXAMPLES:
	${script_name}		# Build and install the pymtl module
EOF
}

check_build_packages() {
	local missing=0

	require_commands cc:gcc swig:swig python3:python3 || return 1
	if ! python3 -c 'import setuptools' 2>/dev/null; then
		log_error "Required package is missing: python3-setuptools"
		missing=1
	fi
	if ! python3 -c 'import os, sys, sysconfig
sys.exit(not os.path.isfile(os.path.join(sysconfig.get_paths()["include"], "Python.h")))'; then
		log_error "Required package is missing: python3-dev"
		missing=1
	fi
	if [ ! -f "${MTL_INCLUDE_DIR}/mtl/mtl_api.h" ]; then
		log_error "Required package is missing: MTL (build and install it with build.sh)"
		missing=1
	fi
	return "$missing"
}

main() {
	local opt

	while getopts ":h" opt; do
		case $opt in
		h)
			show_help
			exit 0
			;;
		*)
			log_error "Unknown option: -${OPTARG}"
			show_help
			exit 1
			;;
		esac
	done
	shift $((OPTIND - 1))
	[ "$#" -eq 0 ] || {
		log_error "Unexpected argument: $1"
		show_help
		exit 1
	}

	check_build_packages || exit 1

	cd "${REPO_DIR}/python/swig"
	log_info "Generate the SWIG wrapper"
	swig -python -I"${MTL_INCLUDE_DIR}" -o pymtl_wrap.c pymtl.i
	log_info "Build the Python extension"
	python3 setup.py build_ext --inplace
	log_info "Install the Python extension"
	as_root python3 setup.py install
	log_success "Python binding installed"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
