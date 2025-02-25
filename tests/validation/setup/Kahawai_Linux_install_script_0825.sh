#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022-2025 Intel Corporation

set -e -o pipefail

default_path=/home/media/ws
work_path=${default_path}/workspace
json_config_path="${work_path}/sample_config"
tx_json_file="./config/test_tx_1port_1v.json"
rx_json_file="./config/test_rx_1port_1v.json"
rxtx_json_file="./tests/script/1080p59_1v.json"
expect_ice_version='1.9.11'
expect_ddp_version='1.3.30.0'
expect_dpdk_version='22.07'

if [ -d ${work_path} ]; then
	rm -rf ${work_path}
fi

mkdir -p ${work_path}
mkdir -p ${json_config_path}

if [ ! -e libraries.media.st2110.kahawai.tgz ]; then
	echo "No libraries.media.st2110.kahawai.tgz related package! please check it"
	exit 1
fi

tar xvf libraries.media.st2110.kahawai.tgz -C ${work_path} >/dev/null 2>&1
function get_json_file() {
	cd ${work_path}/libraries.media.st2110.kahawai || exit
	if [ -e ${tx_json_file} ]; then
		cp ${tx_json_file} ${json_config_path}/tx_sample.json
	else
		echo "No ${tx_json_file} in build path"
		#exit 1
	fi
	if [ -e ${rx_json_file} ]; then
		cp ${rx_json_file} ${json_config_path}/rx_sample.json
	else
		echo "No ${rx_json_file} in build path"
		#exit 1
	fi
	if [ -e ${rxtx_json_file} ]; then
		cp ${rxtx_json_file} ${json_config_path}/rxtx_sample.json
	else
		echo "No ${rxtx_json_file} in build path"
		#exit 1
	fi
}

function get_ice_package() {
	cd ${work_path} || exit
	wget https://ubit-artifactory-sh.intel.com/artifactory/NPG_VCD-local/Kahawai/ICE/Release_27_6.zip >/dev/null 2>&1
	#wget https://downloadmirror.intel.com/739627/Release_27_6.zip  > /dev/null 2>&1
	unzip -d tmp Release_27_6.zip >/dev/null 2>&1
	cp -rp ./tmp/PROCGB/Linux/ice-*.tar.gz ./
	rm -rf tmp
	found=false
	for file in ice-*.tar.gz; do
		if [ -e "$file" ]; then
			found=true
			break
		fi
	done
	if [ "$found" = false ]; then
		echo "No ice*.tar.gz related package! please check it"
		exit 1
	fi
	tar xvf ice*.tar.gz
	rm -rf ice*.tar.gz
}

##Install third pard software
function precondition() {
	echo "Y" | sudo apt-get install libsdl2-ttf-dev git gcc meson python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libsdl2-dev openssl libssl-dev
	sudo pip3 install pyelftools
}

##Check the VT-D and VT-X option in BIOS
function check_iommu_status() {
	count=$(find /sys/kernel/iommu_groups -mindepth 1 -maxdepth 1 -type d | wc -l)
	echo "$count"
	if [ "${count}" -eq 0 ]; then
		echo "VT-D and VT-X are not enabled in BOIS"
		exit 1
	else
		echo "VT-D and VT-X have been enabled in BOIS"
	fi
}

##Patch DPDK with Kahawai patches
##Buid and install DPDK driver
##Build kahawai sample
function build_install_dpdk() {
	rm -rf /usr/local/share/dpdk

	##Git clond DPDK driver
	cd ${work_path} || exit
	git clone https://github.com/DPDK/dpdk.git
	cd dpdk || exit
	git checkout v${expect_dpdk_version}
	git switch -c v${expect_dpdk_version}

	##Check DPDK version
	dpdk_version=$(git branch | grep "\*" | grep "v${expect_dpdk_version}")
	if [ -z "${dpdk_version}" ]; then
		echo "DPDK version is fialed, current is $(git branch | grep "\*")"
		exit 1
	else
		echo "DPDK git clone passed, current is $(git branch | grep "\*")"
	fi

	##Patch Kahawai patches
	mapfile -t patch_list < <(find ${work_path}/libraries.media.st2110.kahawai*/patches/dpdk/${expect_dpdk_version} -name '*.patch' -type f | sort -n -k 10 -t /)
	for patch in "${patch_list[@]}"; do
		echo "git am $patch"
		git am "$patch"
	done

	##Buid and install DPDK driver
	meson build
	ninja -C build
	cd build || exit
	sudo ninja install
	pkg-config --cflags libdpdk
	pkg-config --libs libdpdk
	pkg-config --modversion libdpdk

	##Check DPDK
	if [ ! -d /usr/local/share/dpdk ]; then
		echo "Install DPDK failed"
		exit 1
	else
		echo "Install DPDK passed"
	fi

	##Build Kahawai Sample
	cd ${work_path}/libraries.media.st2110.kahawai || exit
	./build.sh

	##Check Kahawai Sample
	if [ ! -e build/app/RxTxApp ]; then
		echo "Build kahawai sample failed"
		exit 1
	else
		echo "Build kahawai sample passed"
	fi
	if [ ! -e build/tests/KahawaiTest ]; then
		echo "Build kahawai test sample failed"
		exit 1
	else
		echo "Build kahawai test sample passed"
	fi
}

##Install ICE driver
function install_ice() {
	##Install ICE
	cd ${work_path}/ice*/src || exit
	make
	sudo make install
	rmmod ice
	modprobe ice

	##check ICE version
	ice_tab=0
	ice_version=$(find ${work_path} -maxdepth 1 -name 'ice*' -type d | cut -d "-" -f 2)
	mapfile -t ice_actual_version < <(dmesg | grep ice | grep E8 | awk -F "version " '{print $2}' | sort -u)
	for ice_loop in "${ice_actual_version[@]}"; do
		if [ "${ice_version}" == "${ice_loop}" ]; then
			ice_tab=1
		fi
	done

	if [ ${ice_tab} -eq 0 ]; then

		echo "ICE install failed, the actual version is ${ice_actual_version[*]}, but the expect is ${ice_version}"
		exit 1
	else
		echo "ICE inatall passed, the actual version is ${ice_actual_version[*]}, the expect is ${ice_version}"
	fi

	##Install DDP
	cd ${work_path}/ice*/ddp || exit
	ice_pkg=$(ls ice*.pkg)

	if [ -e /usr/lib/firmware/updates/intel/ice/ddp/"${ice_pkg}" ]; then
		rm -rf /usr/lib/firmware/updates/intel/ice/ddp/"${ice_pkg}"
	fi

	cp "${ice_pkg}" /usr/lib/firmware/updates/intel/ice/ddp

	cd /usr/lib/firmware/updates/intel/ice/ddp || exit
	if [ -e ice.pkg ]; then
		rm -rf ice.pkg
	fi
	chmod 755 "${ice_pkg}"
	ln -s "${ice_pkg}" ice.pkg
	rmmod ice
	modprobe ice

	##Check DDP version
	ddp_tab=0
	ddp_version=$(echo "$ice_pkg" | awk -F "ice-" '{print $2}' | awk -F ".pkg" '{print $1}')
	mapfile -t ddp_actual_version < <(dmesg | grep "DDP" | grep "successfully" | awk -F "version " '{print $2}' | sort -u)
	for ddp_loop in "${ddp_actual_version[@]}"; do
		if [ "${ddp_version}" == "${ddp_loop}" ]; then
			ddp_tab=1
		fi
	done

	if [ ${ddp_tab} -eq 0 ]; then
		echo "DDP install failed, actual is ${ddp_actual_version[*]}, expect is ${ddp_version}"
	else
		echo "DDP install passed, actual is ${ddp_actual_version[*]}, expect is ${ddp_version}"
	fi

}

function check_ice_version() {
	actual_ice_version=$(dmesg | grep "E8" | grep ${expect_ice_version})
	actual_ddp_version=$(dmesg | grep "DDP" | grep ${expect_ddp_version})
	if [[ -z ${actual_ice_version} ]] || [[ -z ${actual_ddp_version} ]]; then
		echo "The ice verson is not correct, reintall it"
		echo "${actual_ice_version}"
		echo "${actual_ddp_version}"
		get_ice_package
		install_ice
	else
		echo "The ice verson is correct"
		echo "${actual_ice_version}"
		echo "${actual_ddp_version}"
	fi
}

##Bind network card to DPDK driver
function bind_nic_pf() {
	cd ${work_path}/libraries.media.st2110.kahawai* || exit
	mapfile -t port_list < <(${work_path}/dpdk/usertools/dpdk-devbind.py -s | grep E8 | awk '{print $1}')
	for port in "${port_list[@]}"; do
		echo "$port"
		##./script/nicctl.sh disable_vf $port
		./script/nicctl.sh bind_pmd "$port"
	done
}

check_iommu_status
#get_json_file
precondition
check_ice_version
build_install_dpdk
bind_nic_pf
echo "Set up environment successfully"
