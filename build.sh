#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

user=$(whoami)

function usage() {
	echo "Usage: $0 [debug|debugonly|debugoptimized|plain|release] [enable_fuzzing] [unit]"
	exit 0
}

buildtype=release
enable_asan=false
enable_tap=false
enable_usdt=true
enable_fuzzing=false
build_unit=false

: "${MTL_BUILD_ENABLE_ASAN:=false}"
: "${MTL_BUILD_ENABLE_TAP:=false}"
: "${MTL_BUILD_DISABLE_USDT:=false}"
: "${MTL_BUILD_ENABLE_FUZZING:=false}"
: "${MTL_PREFIX_ARGS:=}"

if [ "$MTL_BUILD_ENABLE_ASAN" == "true" ]; then
	enable_asan=true
	buildtype=debug # use debug build as default for asan
	echo "Enable asan check."
fi

if [ "$MTL_BUILD_ENABLE_TAP" == "true" ]; then
	enable_tap=true
	echo "Enable tap"
fi

if [ "$MTL_BUILD_DISABLE_USDT" == "true" ]; then
	enable_usdt=false
	echo "Disable USDT"
fi

if [ "$MTL_BUILD_ENABLE_FUZZING" == "true" ]; then
	enable_fuzzing=true
	echo "Enable fuzzers"
fi

while [ $# -gt 0 ]; do
	case $1 in
	"debug")
		buildtype=debug
		enable_asan=true
		;;
	"debugonly")
		buildtype=debug
		;;
	"debugoptimized")
		buildtype=debugoptimized
		;;
	"plain")
		buildtype=plain
		;;
	"release")
		buildtype=release
		;;
	"enable_fuzzing" | "--enable-fuzzing")
		enable_fuzzing=true
		echo "Enable fuzzers"
		;;
	"unit")
		build_unit=true
		;;
	*)
		usage
		;;
	esac
	shift
done

WORKSPACE=$PWD
LIB_BUILD_DIR=${WORKSPACE}/build
APP_BUILD_DIR=${WORKSPACE}/build/app
TEST_BUILD_DIR=${WORKSPACE}/build/tests
PLUGINS_BUILD_DIR=${WORKSPACE}/build/plugins
LD_PRELOAD_BUILD_DIR=${WORKSPACE}/build/ld_preload
MANAGER_BUILD_DIR=${WORKSPACE}/build/manager
RXTXAPP_BUILD_DIR=${WORKSPACE}/tests/tools/RxTxApp/build
UNIT_BUILD_DIR=${WORKSPACE}/build_unit

# For a build directory that exists, meson setup does not read PKG_CONFIG_PATH
# again, and meson 0.61 also ignores the options. meson configure applies them.
# Each run gives all its options, also the defaults, so a run does not keep the
# prefix or the rpath of an earlier run.
# meson_setup <build dir> <option>...
meson_setup() {
	local dir=$1
	shift
	if [ -f "${dir}/meson-private/coredata.dat" ]; then
		meson configure "${dir}" -Dpkg_config_path="${PKG_CONFIG_PATH:-[]}" "$@"
	else
		meson setup "${dir}" "$@"
	fi
}

# The DPDK of build_dpdk.sh sits beside the prefix. Find it before the unit
# build, so that nobody has to export PKG_CONFIG_PATH for a local install.
if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
	_dpdk_pc=$(find "$(dirname "${MTL_INSTALL_PREFIX}")/dpdk" -name libdpdk.pc -print -quit 2>/dev/null || true)
	if [ -n "${_dpdk_pc}" ]; then
		PKG_CONFIG_PATH="$(dirname "${_dpdk_pc}")${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
		export PKG_CONFIG_PATH
	fi
fi

# build and run the unit gtest suite (tests/unit), then exit
if [ "$build_unit" == "true" ]; then
	meson_setup "${UNIT_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan" -Denable_unit_tests=true
	ninja -C "${UNIT_BUILD_DIR}"

	if [ "$enable_asan" == "true" ]; then
		ASAN_LIBRARY=$(cc -print-file-name=libasan.so)
		LD_PRELOAD="${ASAN_LIBRARY}${LD_PRELOAD:+:${LD_PRELOAD}}" \
			"${UNIT_BUILD_DIR}/tests/unit/UnitTest"
	else
		"${UNIT_BUILD_DIR}/tests/unit/UnitTest"
	fi
	exit 0
fi

if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
	MTL_PREFIX_ARGS="--prefix=$MTL_INSTALL_PREFIX"
elif [ -z "${MTL_PREFIX_ARGS}" ] && [ "${OS:-}" != "Windows_NT" ]; then
	MTL_PREFIX_ARGS="--prefix=/usr/local"
fi

# A prefix install is self-contained: every binary and library records where
# its siblings are relative to itself ($ORIGIN), so the tree runs without
# ldconfig or LD_LIBRARY_PATH and can be moved as one piece. DT_RPATH rather
# than RUNPATH: the lookup then also covers what libmtl itself loads (DPDK).
# rpath_args <dir of the binary> <library dir>... prints the link arguments.
rpath_args() {
	local from=$1 dir args="-Wl,--disable-new-dtags"
	shift
	for dir in "$@"; do
		args+=",-rpath,\$ORIGIN/$(realpath -m --relative-to="$from" "$dir")"
	done
	echo "${args}"
}
# Without a prefix, the link arguments are what meson sets at the first setup:
# CFLAGS (CXXFLAGS for C++) and LDFLAGS. Give them as c_link_args to a C project
# and as cpp_link_args to a C++ project: meson 0.61 configure stops at an option
# of a missing language.
LIB_LINK_ARGS="${CFLAGS:-} ${LDFLAGS:-}"
BIN_LINK_ARGS=${LIB_LINK_ARGS}
CPP_LINK_ARGS="${CXXFLAGS:-} ${LDFLAGS:-}"
if [ -n "${MTL_INSTALL_PREFIX:-}" ] && [ "${OS:-}" != "Windows_NT" ]; then
	mtl_libdir="${MTL_INSTALL_PREFIX}/$(bash -c '. script/common.sh && multiarch_libdir')"
	# pkg-config writes a space in the path as "\ ".
	dpdk_libdir=$(pkg-config --variable=libdir libdpdk)
	dpdk_libdir=${dpdk_libdir//\\ / }
	# libmtl.so and the plugins, in the library directory; the programs, in bin.
	LIB_LINK_ARGS=$(rpath_args "${mtl_libdir}" "${dpdk_libdir}")
	BIN_LINK_ARGS=$(rpath_args "${MTL_INSTALL_PREFIX}/bin" "${mtl_libdir}" "${dpdk_libdir}")
	CPP_LINK_ARGS=${BIN_LINK_ARGS}
fi
# With a prefix, each .pc file names its own location rather than the path of
# this build, so the tree stays valid when moved.
relocate_pkgconfig() {
	local pc relative
	for pc in "$1"/lib/pkgconfig/*.pc "$1"/lib/*/pkgconfig/*.pc "$1"/lib64/pkgconfig/*.pc; do
		[ -f "$pc" ] || continue
		relative=$(realpath -m --relative-to="$(dirname "$pc")" "$1")
		sed -i "s|^prefix=.*|prefix=\${pcfiledir}/${relative}|" "$pc"
	done
}

# meson 0.62 and later do not run ldconfig after an install. Without it, a
# program in /usr/local/bin does not find libmtl.so.
do_install() {
	if [ -n "${MTL_INSTALL_PREFIX:-}" ] || [ "$OS" == "Windows_NT" ]; then
		ninja install
	elif [ "$user" == "root" ]; then
		ninja install
		ldconfig
	else
		# Build as the user, and give back the files that the install writes,
		# so that a later prefix build in the same directory can write them.
		ninja
		sudo ninja install
		for f in meson-logs/install-log.txt .ninja_log; do
			[ ! -e "$f" ] || sudo chown "$user": "$f"
		done
		sudo ldconfig
	fi
}

# build lib
meson_setup "${LIB_BUILD_DIR}" ${MTL_PREFIX_ARGS:+"$MTL_PREFIX_ARGS"} -Dc_link_args="${LIB_LINK_ARGS}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan" -Denable_tap="$enable_tap" -Denable_usdt="$enable_usdt" -Denable_fuzzing="$enable_fuzzing"
pushd "${LIB_BUILD_DIR}"
ninja
do_install
popd
[ -z "${MTL_INSTALL_PREFIX:-}" ] || relocate_pkgconfig "${MTL_INSTALL_PREFIX}"

# When using a custom install prefix, ensure downstream builds find the
# freshly installed mtl.pc rather than a stale system copy.
if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
	_pkgdir=$(find "${MTL_INSTALL_PREFIX}" -name "mtl.pc" -exec dirname {} \; 2>/dev/null | head -1)
	if [ -n "${_pkgdir}" ]; then
		export PKG_CONFIG_PATH="${_pkgdir}:${PKG_CONFIG_PATH:-}"
	fi
fi

# build app
pushd app/
meson_setup "${APP_BUILD_DIR}" ${MTL_PREFIX_ARGS:+"$MTL_PREFIX_ARGS"} -Dc_link_args="${BIN_LINK_ARGS}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${APP_BUILD_DIR}"
ninja
do_install
popd

# build tests
pushd tests/
meson_setup "${TEST_BUILD_DIR}" ${MTL_PREFIX_ARGS:+"$MTL_PREFIX_ARGS"} -Dcpp_link_args="${CPP_LINK_ARGS}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${TEST_BUILD_DIR}"
ninja
do_install
popd

# build plugins
pushd plugins/
meson_setup "${PLUGINS_BUILD_DIR}" ${MTL_PREFIX_ARGS:+"$MTL_PREFIX_ARGS"} -Dc_link_args="${LIB_LINK_ARGS}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${PLUGINS_BUILD_DIR}"
ninja
do_install
popd

# The plugin registry of a prefix install, beside it in conf/: the JPEG XS and
# st22 avcodec plugins where their scripts install them, so that the registry
# is complete in the MTL cache. Not the sample plugin: it claims JPEG XS and
# H.264 too, so a session the real codec cannot serve would pass on it.
# Absolute paths: the tree runs where it was built. Point KAHAWAI_CFG_PATH at it.
if [ -n "${MTL_INSTALL_PREFIX:-}" ] && [ "${OS:-}" != "Windows_NT" ]; then
	(
		. script/common.sh
		local_base=$(dirname "${MTL_INSTALL_PREFIX}")
		registry="${local_base}/conf/kahawai.json"
		register_plugin "${registry}" st22_svt_jpegxs \
			"${local_base}/jpegxs/$(multiarch_libdir)/libst_plugin_st22_svt_jpeg_xs.so"
		register_plugin "${registry}" st22_avcodec \
			"${local_base}/plugins/$(multiarch_libdir)/libst_plugin_st22_avcodec.so"
	)
fi

# build ld_preload
if [ "$OS" != "Windows_NT" ]; then
	pushd ld_preload/
	meson_setup "${LD_PRELOAD_BUILD_DIR}" ${MTL_PREFIX_ARGS:+"$MTL_PREFIX_ARGS"} -Dc_link_args="${LIB_LINK_ARGS}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
	popd
	pushd "${LD_PRELOAD_BUILD_DIR}"
	ninja
	do_install
	popd
fi

# build mtl_manager
if [ "$OS" != "Windows_NT" ]; then
	pushd manager/
	meson_setup "${MANAGER_BUILD_DIR}" ${MTL_PREFIX_ARGS:+"$MTL_PREFIX_ARGS"} -Dcpp_link_args="${CPP_LINK_ARGS}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
	popd
	pushd "${MANAGER_BUILD_DIR}"
	ninja
	do_install
	popd
fi

# build RxTxApp
pushd tests/tools/RxTxApp/
meson_setup "${RXTXAPP_BUILD_DIR}" ${MTL_PREFIX_ARGS:+"$MTL_PREFIX_ARGS"} -Dc_link_args="${BIN_LINK_ARGS}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${RXTXAPP_BUILD_DIR}"
ninja
do_install
popd
