#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -e

download_mirror=843928
script_name=$(basename "$0")
script_path=$(readlink -qe "$0")
script_folder=${script_path/$script_name/}
cd "${script_folder}"

if [ -n "$1" ]; then
	e810_driver_ver=$1
else
	e810_driver_ver=1.16.3
fi

echo "Building e810 driver version: $e810_driver_ver form mirror $download_mirror"

wget "https://downloadmirror.intel.com/${download_mirror}/ice-${e810_driver_ver}.tar.gz"
tar xvzf ice-1.16.3.tar.gz
cd ice-1.16.3

git init
git add .
git commit -m "init version ${e810_driver_ver}"
git am ../../patches/ice_drv/${e810_driver_ver}/*.patch

cd src
make
sudo make install
sudo rmmod ice
sudo modprobe ice
cd -
