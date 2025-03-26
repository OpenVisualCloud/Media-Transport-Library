#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

# Based on clang-format
# For ubuntu, pls "apt-get install clang-format"
# When updating clang format version, remember to update also GHA clang lister version:
#  - uses: DoozyX/clang-format-lint-action@v0.18.2
#     with:
#       clangFormatVersion: '14'
#       source: '.'
#       extensions: 'hpp,h,cpp,c,cc'
# in
# .github/workflows/afxdp_build.yml
# .github/workflows/centos_build.yml
# .github/workflows/tools_build.yml
# .github/workflows/ubuntu_build.yml

set -e



CLANG_FORMAT_TOOL="clang-format-14" # for super-linter v6 action

if ! command -v ${CLANG_FORMAT_TOOL} &> /dev/null; then
	echo "${CLANG_FORMAT_TOOL} not available"

	if ! command -v docker &> /dev/null; then
		echo "docker not available"
		exit 1
	fi

	cd "$(dirname $0)"

	# shellcheck disable=SC2154
	docker build -t clang-format-mtl:latest -f docker/clang-format.dockerfile --build-arg HTTP_PROXY="$http_proxy" --build-arg HTTPS_PROXY="$https_proxy" ../
	docker run --rm -v "$(dirname $0):/Media-Transport-Library" clang-format-mtl:latest "/Media-Transport-Library/$(basename $0)"

	cd -

else
	echo "clang-format check"
	find . -path ./build -prune -o -regex '.*\.\(cpp\|hpp\|cc\|c\|h\)' ! -name 'pymtl_wrap.c' \
		! -name 'vmlinux.h' -exec ${CLANG_FORMAT_TOOL} --verbose -i {} +

	black python/
	isort python/
fi


