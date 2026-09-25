#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

# Put the MTL Manager service files in place.
#
# meson writes the service files into the build directory, with the real path of
# the MtlManager of that build, so build the manager first. Then:
#   sudo manager/service/install_service.sh
#   manager/service/install_service.sh --user
#
# It does not start the service. The last lines say how.

set -e

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
mode=system
action=install
unit_dir=""

systemd_dir() {
	local name=$1 fallback=$2 value=""
	value=$(pkg-config --variable="$name" systemd 2>/dev/null || true)
	echo "${value:-$fallback}"
}

system_dir=${MTL_SYSTEMD_SYSTEM_DIR:-$(systemd_dir systemdsystemunitdir /lib/systemd/system)}
user_dir=${MTL_SYSTEMD_USER_DIR:-$(systemd_dir systemduserunitdir /lib/systemd/user)}
tmpfiles_dir=${MTL_TMPFILES_DIR:-$(systemd_dir tmpfilesdir /usr/lib/tmpfiles.d)}

usage() {
	cat <<'EOF'
Usage: install_service.sh [--user] [--uninstall] [--units DIR] [--help]

  --user       Work on the service of this user alone, in
               ~/.config/systemd/user. Needs no root.
  --uninstall  Take the service files away again.
  --units DIR  Read the service files from DIR. The default is
               <repo>/build/manager/service, where build.sh writes them.

Environment:
  MTL_SYSTEMD_SYSTEM_DIR  Where the system service file goes.
  MTL_SYSTEMD_USER_DIR    Where the per user service file goes.
  MTL_TMPFILES_DIR        Where the tmpfiles rule goes.
EOF
}

while [ $# -gt 0 ]; do
	case $1 in
	--user)
		mode=user
		;;
	--uninstall)
		action=uninstall
		;;
	--units)
		shift
		unit_dir=$1
		;;
	--help | -h)
		usage
		exit 0
		;;
	*)
		echo "Unknown option $1" >&2
		usage
		exit 1
		;;
	esac
	shift
done

if [ "$mode" = user ]; then
	target_dir=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
	reload=(systemctl --user daemon-reload)
else
	if [ "$(id -u)" -ne 0 ]; then
		echo "The system service needs root. Run this in your own terminal:" >&2
		echo "  sudo $here/install_service.sh $*" >&2
		exit 1
	fi
	target_dir=$system_dir
	reload=(systemctl daemon-reload)
fi

if [ "$action" = uninstall ]; then
	rm -f "$target_dir/mtl-manager.service"
	if [ "$mode" = system ]; then
		rm -f "$user_dir/mtl-manager.service"
		rm -f "$tmpfiles_dir/mtl-manager.conf"
	fi
	"${reload[@]}" || true
	echo "Took the MTL Manager service files away."
	exit 0
fi

if [ -z "$unit_dir" ]; then
	unit_dir=$repo/build/manager/service
fi

for name in mtl-manager.service mtl-manager-user.service; do
	if [ ! -f "$unit_dir/$name" ]; then
		echo "No $name in $unit_dir." >&2
		echo "Build the manager first, or name the directory with --units." >&2
		exit 1
	fi
done

install -d "$target_dir"
if [ "$mode" = user ]; then
	install -m 0644 "$unit_dir/mtl-manager-user.service" "$target_dir/mtl-manager.service"
else
	install -m 0644 "$unit_dir/mtl-manager.service" "$target_dir/mtl-manager.service"
	install -d "$user_dir"
	install -m 0644 "$unit_dir/mtl-manager-user.service" "$user_dir/mtl-manager.service"
	install -d "$tmpfiles_dir"
	install -m 0644 "$here/mtl-manager.conf" "$tmpfiles_dir/mtl-manager.conf"
	systemd-tmpfiles --create "$tmpfiles_dir/mtl-manager.conf" || true
fi

# A reload fails when there is no systemd session, for example in a container.
# The files are in place either way, so say it and go on.
"${reload[@]}" || echo "Could not reload systemd. Reload it yourself later." >&2

echo "Put $target_dir/mtl-manager.service in place."
if [ "$mode" = user ]; then
	echo "Start it with: systemctl --user enable --now mtl-manager"
else
	echo "Start it with: systemctl enable --now mtl-manager"
	echo "A user who needs no XDP program can run a private one instead:"
	echo "  systemctl --user enable --now mtl-manager"
fi
