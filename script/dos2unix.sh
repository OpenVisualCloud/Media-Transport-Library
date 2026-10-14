#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

show_help() {
	cat <<EOF
Usage: ${script_name} [OPTIONS]

Convert the line endings of text files from DOS (CRLF) to Unix (LF) with
dos2unix. The script operates on the current directory and its
subdirectories. It converts only the files that git tracks and that git
sees as text. It does not touch the .git directory, binary files, empty
files, or files that git does not track. Run it from a directory in a git
work tree.

REQUIRED PACKAGES (Debian/Ubuntu):
	dos2unix git

OPTIONS:
	-h		Show this help message

EXAMPLES:
	${script_name}			# Convert the text files below the current directory
	cd lib && ../script/${script_name}	# Convert only the text files in lib/
EOF
}

# Writes the path of each tracked text file below the current directory,
# with a NUL after each path. "git ls-files --eol" gives the line ending
# that git sees in the work tree file: lf, crlf, mixed, -text (binary),
# none (empty), or nothing (file not in the work tree).
list_tracked_text_files() {
	local entry info eol

	git ls-files -z --eol | while IFS= read -r -d '' entry; do
		info="${entry%%$'\t'*}"
		eol="${info#* w/}"
		eol="${eol%% *}"
		case "$eol" in
		lf | crlf | mixed) printf './%s\0' "${entry#*$'\t'}" ;;
		esac
	done
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

	require_commands dos2unix:dos2unix git:git || exit 1
	if [ "$(git rev-parse --is-inside-work-tree 2>/dev/null)" != "true" ]; then
		log_error "The current directory is not in a git work tree: ${PWD}"
		exit 1
	fi

	log_info "dos2unix check"
	list_tracked_text_files | xargs -0 -r dos2unix
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
