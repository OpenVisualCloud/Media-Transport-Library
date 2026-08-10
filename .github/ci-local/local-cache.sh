#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

operation=${1:?usage: local-cache.sh restore|save STORE COMPONENT KEY PATH}
store=${2:?}
component=${3:?}
key=${4:?}
path=${5:?}
key_hash=$(printf '%s' "$key" | sha256sum | cut -d' ' -f1)
entry="${store}/${component}/${key_hash}"

case "$operation" in
restore)
	[ -f "${entry}/key" ] && [ "$(cat "${entry}/key")" = "$key" ] || exit 1
	rm -rf "$path"
	mkdir -p "$path"
	cp -a "${entry}/tree/." "$path/"
	;;
save)
	[ -d "$path" ] || exit 1
	if [ -e "$entry" ]; then
		echo "cache entry already exists and remains immutable: ${component}"
		exit 0
	fi
	stage="${entry}.tmp.$$"
	trap 'rm -rf "$stage"' EXIT
	mkdir -p "${stage}/tree"
	cp -a "${path}/." "${stage}/tree/"
	printf '%s\n' "$key" >"${stage}/key"
	mkdir -p "$(dirname "$entry")"
	mv "$stage" "$entry"
	trap - EXIT
	;;
*)
	echo "unknown local cache operation: ${operation}" >&2
	exit 2
	;;
esac