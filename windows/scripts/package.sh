#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# package.sh - make the Windows installer of MTL (mtl-<version>-win64-setup.exe).
#
# Run it in the MSYS2 UCRT64 shell, from the MTL tree, after the release
# build of doc/build_WIN.md. DPDK must be built with -Dplatform=generic: a
# DPDK built for the CPU of this machine (the default, -march=native) gives
# a libmtl.dll that can stop with an illegal instruction on other CPUs.
#
# Usage: windows/scripts/package.sh [--out <dir>] [--kmods <dpdk-kmods tree>]
#        [--dpdk <DPDK source tree>] [--iscc <ISCC.exe>] [--stage-only]
#
#   --out         Output folder (default: build_installer).
#   --kmods       A dpdk-kmods clone. Default: a clone of DPDK_KMODS_COMMIT
#                 in the output folder.
#   --dpdk        The DPDK source tree, for its licence files (default: dpdk).
#   --iscc        The Inno Setup compiler. Default: iscc on the PATH, or the
#                 Inno Setup 6 folder of Program Files.
#   --stage-only  Make the folder of the files, but not the setup.exe.

set -euo pipefail

DPDK_KMODS_COMMIT="${DPDK_KMODS_COMMIT:-9b182be2ee4bf003c892e1312440e1e5d93eff2c}"
DPDK_KMODS_URL="${DPDK_KMODS_URL:-https://dpdk.org/git/dpdk-kmods}"

die() {
	echo "package.sh: $*" >&2
	exit 1
}

out=build_installer
kmods=""
dpdk_src=dpdk
iscc=""
stage_only=0
while [ $# -gt 0 ]; do
	case "$1" in
	--out) out="$2" && shift ;;
	--kmods) kmods="$2" && shift ;;
	--dpdk) dpdk_src="$2" && shift ;;
	--iscc) iscc="$2" && shift ;;
	--stage-only) stage_only=1 ;;
	-h | --help) sed -n '6,22p' "$0" | sed 's/^# \{0,1\}//' && exit 0 ;;
	*) die "unknown argument: $1 (see --help)" ;;
	esac
	shift
done

[ -n "${MSYSTEM_PREFIX:-}" ] || die "run this in the MSYS2 UCRT64 shell"
if [ ! -f VERSION ] || [ ! -f windows/installer/mtl.iss ]; then die "run this from the MTL tree"; fi
prefix="$MSYSTEM_PREFIX"
version="$(tr -d '\r\n' <VERSION)"
# 26.09.0.DEV -> 26.9.0.0, the numeric version of the setup.exe file.
num_version="$(echo "$version" | awk -F. '{ printf "%d.%d.%d.0", $1, $2, $3 }')"

dpdk_cflags="$(pkg-config --cflags libdpdk)" || die "no libdpdk.pc: build and install DPDK first"
case " $dpdk_cflags " in
*" -march=native "*) die "DPDK is built for this CPU (-march=native): build it with meson setup -Dplatform=generic" ;;
esac
for f in bin/libmtl.dll bin/RxTxApp.exe lib/libmtl.dll.a lib/pkgconfig/mtl.pc include/mtl/mtl_api.h; do
	[ -f "$prefix/$f" ] || die "no $prefix/$f: run ./build.sh release first"
done

stage="$out/stage"
rm -rf "$stage"
mkdir -p "$stage"/{bin,lib/pkgconfig,include,scripts,config,licenses,drivers}

# The programs, and each DLL of MSYS2 that they load.
cp "$prefix/bin/libmtl.dll" "$prefix/bin/RxTxApp.exe" "$stage/bin/"
deps="$({ ldd "$prefix/bin/RxTxApp.exe" "$prefix/bin/libmtl.dll" || true; } | awk '$3 ~ "^'"$prefix"'/bin/" { print $3 }' | sort -u)"
for dll in $deps; do
	[ "$(basename "$dll")" = libmtl.dll ] || cp "$dll" "$stage/bin/"
done
# libmtl.dll imports wpcap.dll, the runtime of npcap. The libpcap of MSYS2
# takes its place: enough to run, not enough to capture packets.
[ -f "$stage/bin/wpcap.dll" ] || cp "$prefix/bin/libpcap.dll" "$stage/bin/wpcap.dll"
[ -f "$stage/bin/libpcap.dll" ] || cp "$prefix/bin/libpcap.dll" "$stage/bin/"

cp "$prefix/lib/libmtl.dll.a" "$stage/lib/"
# shellcheck disable=SC2016 # ${pcfiledir} is for pkg-config, not for the shell.
sed 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$prefix/lib/pkgconfig/mtl.pc" >"$stage/lib/pkgconfig/mtl.pc"
cp -r "$prefix/include/mtl" "$stage/include/"

cp windows/scripts/*.ps1 "$stage/scripts/"
cp windows/installer/loop.json "$stage/config/"

# The driver source: install_drivers.ps1 builds it on the host.
if [ -z "$kmods" ]; then
	kmods="$out/dpdk-kmods"
	if [ "$(git -C "$kmods" rev-parse HEAD 2>/dev/null)" != "$DPDK_KMODS_COMMIT" ]; then
		rm -rf "$kmods"
		git clone -q "$DPDK_KMODS_URL" "$kmods"
		git -C "$kmods" checkout -q "$DPDK_KMODS_COMMIT"
	fi
fi
mkdir -p "$stage/drivers/dpdk-kmods/windows"
for d in netuio virt2phys; do
	[ -f "$kmods/windows/$d/$d.sln" ] || die "no $kmods/windows/$d/$d.sln"
	mkdir -p "$stage/drivers/dpdk-kmods/windows/$d"
	# The source only: no build output of an earlier build.
	find "$kmods/windows/$d" -maxdepth 1 -type f -exec cp {} "$stage/drivers/dpdk-kmods/windows/$d/" \;
done
cp "$kmods/windows/README.rst" "$stage/drivers/dpdk-kmods/windows/"
echo "dpdk-kmods $(git -C "$kmods" rev-parse HEAD 2>/dev/null || echo "$DPDK_KMODS_COMMIT"), BSD-3-Clause (SPDX header of each file)" \
	>"$stage/drivers/dpdk-kmods/VERSION.txt"

# The licences: MTL, DPDK (static in libmtl.dll), and the package of each DLL.
cp LICENSE "$stage/licenses/MTL-LICENSE.txt"
if [ -d "$dpdk_src/license" ]; then
	mkdir -p "$stage/licenses/dpdk"
	cp "$dpdk_src"/license/* "$stage/licenses/dpdk/"
else
	echo "package.sh: warning: no $dpdk_src/license: give --dpdk <DPDK source tree>" >&2
fi
# THIRD-PARTY.txt names the package, the version and the licence of each DLL,
# also when the package has no licence file.
list="$stage/licenses/THIRD-PARTY.txt"
echo "The DLLs of bin, and the MSYS2 package of each one." >"$list"
for dll in "$stage"/bin/*.dll; do
	base="$(basename "$dll")"
	[ "$base" = libmtl.dll ] && continue
	if [ "$base" = wpcap.dll ]; then
		echo "wpcap.dll: a copy of libpcap.dll" >>"$list"
		continue
	fi
	pkg="$(pacman -Qqo "$prefix/bin/$base" 2>/dev/null)" || die "no MSYS2 package owns $prefix/bin/$base"
	lic="$(pacman -Qi "$pkg" | sed -n 's/^Licenses *: //p')"
	echo "$base: $(pacman -Q "$pkg"), licence: $lic" >>"$list"
	name="${pkg#"${MINGW_PACKAGE_PREFIX:-}-"}"
	[ -d "$prefix/share/licenses/$name" ] && cp -r "$prefix/share/licenses/$name" "$stage/licenses/"
done

echo "package.sh: files in $stage"
[ "$stage_only" -eq 1 ] && exit 0

if [ -z "$iscc" ]; then
	iscc="$(command -v iscc || command -v ISCC || true)"
	for p in "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" "/c/Program Files/Inno Setup 6/ISCC.exe"; do
		[ -z "$iscc" ] && [ -f "$p" ] && iscc="$p"
	done
fi
[ -n "$iscc" ] || die "no Inno Setup compiler: install Inno Setup 6, or give --iscc"
# MSYS2 would make a path of each /option.
MSYS2_ARG_CONV_EXCL='*' "$iscc" /Q "/DMtlVersion=$version" "/DMtlNumVersion=$num_version" \
	"/DStageDir=$(cygpath -aw "$stage")" "/O$(cygpath -aw "$out")" "$(cygpath -aw windows/installer/mtl.iss)"
echo "package.sh: $out/mtl-$version-win64-setup.exe"
