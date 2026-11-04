#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Fetches the scanners that check-hardening.sh runs from HARDENING_TOOLS_DIR,
# as tools.env pins them, see doc/build.md §4.5.

set -euo pipefail

scan_dir=$(dirname "${BASH_SOURCE[0]}")
# shellcheck disable=SC1091
. "${scan_dir}/tools.env"
tools=$HARDENING_TOOLS_DIR

perl -MGetopt::Long -MPod::Usage -MIPC::Open3 -MTerm::ANSIColor -MIO::Select -e 1 2>/dev/null || {
	echo "::error::hardening-check needs perl with Getopt::Long, Pod::Usage, IPC::Open3, Term::ANSIColor and IO::Select" >&2
	exit 1
}
command -v objdump >/dev/null || {
	echo "::error::hardening-check needs objdump" >&2
	exit 1
}

# A copy restored from the cache is checked again.
fetch() { # <url> <file> <sha256|sha512> <hash>
	"$3sum" --status -c <<<"$4  $2" 2>/dev/null && return
	curl -fsSL --connect-timeout 10 --max-time 60 --retry 3 --retry-max-time 120 -o "$2" "$1"
	"$3sum" --status -c <<<"$4  $2" || {
		rm -f "$2"
		echo "::error::$1 does not match tools.env" >&2
		exit 1
	}
}
mkdir -p "${tools}"
fetch "https://github.com/slimm609/checksec/releases/download/${CHECKSEC_VERSION}/checksec_${CHECKSEC_VERSION}_linux_amd64.tar.gz" \
	"${tools}/checksec.tar.gz" sha512 "${CHECKSEC_SHA512}"
fetch "https://salsa.debian.org/debian/devscripts/-/raw/${HARDENING_CHECK_COMMIT}/scripts/hardening-check.pl" \
	"${tools}/hardening-check.pl" sha256 "${HARDENING_CHECK_SHA256}"
tar --no-same-owner -xzf "${tools}/checksec.tar.gz" -C "${tools}" checksec
