#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

REPO_DIR="$(readlink -f "$(dirname -- "${BASH_SOURCE[0]}")/..")"
BUILD_DIR="${BUILD_DIR:-${REPO_DIR}/_build}"
DRIVERS_DIR="${DRIVERS_DIR:-/opt/intel/drivers}"
VERSIONS_ENV_PATH="${REPO_DIR}/versions.env"

if [ -f "$VERSIONS_ENV_PATH" ]; then
	# shellcheck disable=SC1090
	. "$VERSIONS_ENV_PATH"
else
	echo -e "${RED}Error: versions.env file not found at $VERSIONS_ENV_PATH.${NC}"
	exit 1
fi

PM="${PM:-apt-get}"
KERNEL_VERSION="${KERNEL_VERSION:-$(uname -r)}"
TZ="${TZ:-Europe/Warsaw}"
NPROC="${NPROC:-$(nproc)}"

if ! grep "/root/.local/bin" <<<"${PATH}" >/dev/null 2>&1; then
	PATH="/root/.local/bin:/root/bin:/root/usr/bin:${PATH}"
	PKG_CONFIG_PATH="/usr/lib/pkgconfig:/usr/local/lib/pkgconfig:/usr/lib64/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH}"
fi

BOLD="${BOLD:-\e[1;}"
REGULAR="${REGULAR:-\e[0;}"
RED="${RED:-31m}"
GREEN="${GREEN:-32m}"
YELLOW="${YELLOW:-33m}"
BLUE="${BLUE:-34m}"
EndCl='\e[m'

function log_message() {
	local type="${1}"
	shift
	local HEADER="${type}: "
	local FOOTER=""
	# case $(echo $type | tr '[:upper:]' '[:lower:]') in
	if [ -z "${DISABLE_COLOR_PRINT}" ]; then
		FOOTER='\e[0m'
		case "${type}" in
		ERROR) HEADER="${REGULAR}${RED}${type}:  ${BOLD}${RED}" ;;
		WARN | WARNING) HEADER="${REGULAR}${BLUE}${type}: ${BOLD}${YELLOW}" ;;
		SUCC | SUCCESS) HEADER="${REGULAR}${BLUE}${type}: ${BOLD}${GREEN}" ;;
		INFO | *) HEADER="${REGULAR}${BLUE}${type}: ${BOLD}${BLUE}" ;;
		esac
	fi
	echo -e "${HEADER}$*${FOOTER}" >&2
}

function log_info() { log_message "INFO" "$*"; }
function log_success() { log_message "SUCCESS" "$*"; }
function log_warning() { log_message "WARNING" "$*"; }
function log_error() { log_message "ERROR" "$*"; }

function get_user_input_confirm() {
	local confirm
	local confirm_string
	local confirm_default="${1:-0}"
	confirm_string=("(N)o" "(Y)es")

	echo -en "${REGULAR}${BLUE}CHOOSE:${BOLD}${BLUE} (Y)es/(N)o [default: ${confirm_string[$confirm_default]}]: ${EndCl}" >&2
	read -r confirm
	if [[ -z "$confirm" ]]; then
		confirm="$confirm_default"
	else
		{ [[ $confirm == [yY] ]] || [[ $confirm == [yY][eE][sS] ]]; } && confirm="1" || confirm="0"
	fi
	echo "${confirm}"
}

function get_user_input_def_yes() {
	get_user_input_confirm 1
}

function get_user_input_def_no() {
	get_user_input_confirm 0
}

function get_filename() {
	local path="$1"
	echo "${path##*/}"
}

function get_dirname() {
	local path="$1"
	echo "${path%/*}/"
}

function get_extension() {
	local filename
	filename="$(get_filename "${1}")"
	echo "${filename#*.}"
}

function check_extension() {
	local filename="$1"
	local extension="$2"
	if [ "${filename}" == "${filename%"${extension}"}" ]; then
		echo "0"
	else
		echo "1"
	fi
}

function get_basename() {
	local filename
	filename="$(get_filename "${1}")"
	echo "${filename%%.*}"
}

# Extracts namespace and repository part from valid GitHub URL passed as argument.
#  input: valid GitHub repository URL
# output: two element string array, space separated
function get_github_elements() {
	local path_part
	local path_elements
	path_part="${1#*://github.com/}"
	mapfile -t -d'/' path_elements <<<"${path_part}"
	if [[ "${#path_elements[@]}" -lt "2" ]]; then
		log_error "Invalid link passed to get_github_elements method."
		return 1
	fi
	echo "${path_elements[0]} ${path_elements[1]}"
}

function get_github_namespace() {
	cut -d' ' -f1 <<<"$(get_github_elements "$1")"
}

function get_github_repo() {
	cut -d' ' -f2 <<<"$(get_github_elements "$1")"
}

# Adds sufix to base of filename from full path.
# input[1]: string sufix to be added
# input[2]: path string to be modified
#   output: [path]/[file_base][id].[extension]
function get_filepath_add_sufix() {
	local dir_path
	local file_base
	local file_ext
	local file_sufix="${1}"
	local file_path="${2}"
	dir_path="$(get_dirname "${file_path}")"
	file_base="$(get_basename "${file_path}")"
	file_ext="$(get_extension "${file_path}")"
	echo "${dir_path}${file_base}${file_sufix}.${file_ext}"
}

function command_exists {
	command -v "$@" >/dev/null 2>&1
}

function as_root() {
	CMD_TO_EVALUATE="$*"
	CURRENT_USER_ID="$(id -u)"
	EFECTIVE_USER_ID="${EUID:-$CURRENT_USER_ID}"
	AS_ROOT="/bin/bash -c"

	if [ "${EFECTIVE_USER_ID}" -ne 0 ]; then
		if command_exists sudo; then
			AS_ROOT="sudo -E /bin/bash -c"
		elif command_exists su; then
			AS_ROOT="su -c"
		else
			log_error "This command must be run as root [EUID=0] ${CMD_TO_EVALUATE[*]}."
			log_error "- current [EUID=${EFECTIVE_USER_ID}]."
			log_error "- 'sudo' nor 'su' commands were found in PATH."
			log_error "Re-run the script as sudo or install sudo pkg."
			exit 1
		fi
	fi
	$AS_ROOT "${CMD_TO_EVALUATE[*]}"
}

function github_api_call() {
	url=$1
	shift
	GITHUB_API_URL=https://api.github.com
	INPUT_OWNER=$(echo "${url#"${GITHUB_API_URL}/repos/"}" | cut -f1 -d'/')
	INPUT_REPO=$(echo "${url#"${GITHUB_API_URL}/repos/"}" | cut -f2 -d'/')
	API_SUBPATH="${url#"${GITHUB_API_URL}/repos/${INPUT_OWNER}/${INPUT_REPO}/"}"
	if [ -z "${INPUT_GITHUB_TOKEN}" ]; then
		echo >&2 "Set the INPUT_GITHUB_TOKEN env variable first."
		return
	fi

	echo >&2 "GITHUB_API_URL=$GITHUB_API_URL"
	echo >&2 "INPUT_OWNER=$INPUT_OWNER"
	echo >&2 "INPUT_REPO=$INPUT_REPO"
	echo >&2 "API_SUBPATH=$API_SUBPATH"
	echo >&2 "curl --fail-with-body -sSL \"${GITHUB_API_URL}/repos/${INPUT_OWNER}/${INPUT_REPO}/${API_SUBPATH}\" -H \"Authorization: Bearer ${INPUT_GITHUB_TOKEN}\" -H 'Accept: application/vnd.github.v3+json' -H 'Content-Type: application/json' $*"

	if API_RESPONSE=$(curl --fail-with-body -sSL \
		"${GITHUB_API_URL}/repos/${INPUT_OWNER}/${INPUT_REPO}/${API_SUBPATH}" \
		-H "Authorization: Bearer ${INPUT_GITHUB_TOKEN}" \
		-H 'Accept: application/vnd.github.v3+json' \
		-H 'Content-Type: application/json' \
		"$@"); then
		echo "${API_RESPONSE}"
	else
		echo >&2 "GitHub API call failed."
		echo >&2 "${API_RESPONSE}"
		return
	fi
}

function print_logo() {
	if [[ -z "$blue_code" ]]; then
		local blue_code=(26 27 20 19 20 20 21 04 27 26 32 12 33 06 39 38 44 45)
	fi
	local IFS
	local logo_string
	local colorized_logo_string
	IFS=$'\n\t'
	logo_string="$(
		cat <<-EOF
			.-----------------------------------------------------------.
			|        *          .                    ..        .    *   |
			|       .                         .   .  .  .   .           |
			|                                    . .  *:. . .           |
			|                             .  .   . .. .         .       |
			|                    .     . .  . ...    .    .             |
			|  .              .  .  . .    . .  . .                     |
			|                   .    .     . ...   ..   .       .       |
			|            .  .    . *.   . .                             |
			|                   :.  .           .                       |
			|            .   .    .    .                                |
			|        .  .  .    . ^                                     |
			|       .  .. :.    . |             .               .       |
			|.   ... .            |                                     |
			| :.  . .   *.    We are here.              .               |
			|   .               .             *.                        |
			.©-Intel-Corporation--------------------ascii-author-unknown.
			=                                                           =
			=        88                                  88             =
			=        ""                ,d                88             =
			=        88                88                88             =
			=        88  8b,dPPYba,  MM88MMM  ,adPPYba,  88             =
			=        88  88P'   '"8a   88    a8P_____88  88             =
			=        88  88       88   88    8PP"""""""  88             =
			=        88  88       88   88,   "8b,   ,aa  88             =
			=        88  88       88   "Y888  '"Ybbd8"'  88             =
			=                                                           =
			=============================================================
		EOF
	)"

	colorized_logo_string=""
	for ((i = 0; i < ${#logo_string}; i++)); do
		colorized_logo_string+="\e[38;05;${blue_code[$(((i - (i / 64) * 64) / 4))]}m"
		colorized_logo_string+="${logo_string:$i:1}"
	done
	colorized_logo_string+='\e[m\n'

	echo -e "$colorized_logo_string" >&2
}

function print_logo_sequence() {
	set +x
	local wait_between_frames="${1:-0}"
	local wait_cmd=""
	if [ ! "${wait_between_frames}" = "0" ]; then
		wait_cmd="sleep ${wait_between_frames}"
	fi

	blue_code_fixed=(26 27 20 19 20 20 21 04 27 26 32 12 33 06 39 38 44 45)
	size=${#blue_code_fixed[@]}
	for ((move = 0; move < size; move++)); do
		blue_code=()
		for ((i = move; i < size; i++)); do
			blue_code+=("${blue_code_fixed[i]}")
		done
		for ((i = 0; i < move; i++)); do
			blue_code+=("${blue_code_fixed[i]}")
		done
		echo -en "\e[0;0H"
		print_logo
		${wait_cmd}
	done
}

function print_logo_anim() {
	set +x
	local number_of_sequences="${1:-2}"
	local wait_between_frames="${2:-0.025}"
	clear
	for ((pt = 0; pt < number_of_sequences; pt++)); do
		print_logo_sequence "${wait_between_frames}"
	done
}

function catch_error_print_debug() {
	local _last_command_height=""
	local -n _lineno="${1:-LINENO}"
	local -n _bash_lineno="${2:-BASH_LINENO}"
	local _last_command="${3:-${BASH_COMMAND}}"
	local _code="${4:-0}"
	local -a _output_array=()
	_last_command_height="$(wc -l <<<"${_last_command}")"

	_output_array+=(
		'---'
		"lines_history: [${_lineno} ${_bash_lineno[*]}]"
		"function_trace: [${FUNCNAME[*]}]"
		"exit_code: ${_code}"
	)

	if [[ "${#BASH_SOURCE[@]}" -gt '1' ]]; then
		_output_array+=('source_trace:')
		for _item in "${BASH_SOURCE[@]}"; do
			_output_array+=("  - ${_item}")
		done
	else
		_output_array+=("source_trace: [${BASH_SOURCE[*]}]")
	fi

	if [[ "${_last_command_height}" -gt '1' ]]; then
		_output_array+=(
			'last_command: ->'
			"${_last_command}"
		)
	else
		_output_array+=("last_command: ${_last_command}")
	fi

	_output_array+=('---')
	log_error "${_output_array[*]}"
}

# Calling this function executes ERR and SIGINT signals trapping. Triggered trap calls catch_error_print_debug and exit 1
function trap_error_print_debug() {
	log_info "Setting trap for errors handling"
	trap 'catch_error_print_debug "LINENO" "BASH_LINENO" "${BASH_COMMAND}" "${?}"; exit 1' SIGINT ERR
	log_info "Trap set successfuly."
}

# GITHUB_CREDENTIALS="username:password"
# URL construction: https://${GITHUB_CREDENTIALS}@github.com/${name}/archive/${version}.tar.gz
# $1 - name
# $2 - version
# $3 - dest_dir
function git_download_strip_unpack() {
	# Version can be commit sha or tag, examples:
	# version=d2515b90cc0ef651f6d0a6661d5a644490bfc3f3
	# version=refs/tags/v${JPEG_XS_VER}
	name="${1}"
	version="${2}"
	dest_dir="${3}"
	filename="$(get_filename "${version}")"
	[ -n "${GITHUB_CREDENTIALS}" ] && creds="${GITHUB_CREDENTIALS}@" || creds=""

	mkdir -p "${dest_dir}"
	curl -Lf "https://${creds}github.com/${name}/archive/${version}.tar.gz" -o "${dest_dir}/${filename}.tar.gz"
	tar -zx --strip-components=1 -C "${dest_dir}" -f "${dest_dir}/${filename}.tar.gz"
	rm -f "${dest_dir}/${filename}.tar.gz"
}

# Downloads and strip unpack a file from URL ($1) to a target directory ($2)
# $1 - URL to download
# $2 - destination directory to strip unpack the tar.gz
function wget_download_strip_unpack() {
	local filename
	local source_url="${1}"
	local dest_dir="${2}"
	filename="$(get_filename "${source_url}")"
	[ -n "${GITHUB_CREDENTIALS}" ] && creds="${GITHUB_CREDENTIALS}@" || creds=""

	mkdir -p "${dest_dir}"
	curl -Lf "${source_url}" -o "${dest_dir}/${filename}.tar.gz"
	tar -zx --strip-components=1 -C "${dest_dir}" -f "${dest_dir}/${filename}.tar.gz"
	rm -f "${dest_dir}/${filename}.tar.gz"
}

# Example usage:
#	PM="$(setup_package_manager)" && \
#	$PM install python3
function setup_package_manager() {
	TIBER_USE_PM="${PM:-$1}"
	if [[ -x "$(command -v "$TIBER_USE_PM")" ]]; then
		export PM="${TIBER_USE_PM}"
	elif [[ -x "$(command -v yum)" ]]; then
		export PM='yum'
	elif [[ -x "$(command -v dnf)" ]]; then
		export PM='dnf'
	elif [[ -x "$(command -v apt-get)" ]]; then
		export PM='apt-get'
	elif [[ -x "$(command -v apt)" ]]; then
		export PM='apt'
	else
		log_error "No known pkg manager found. Try to re-run with variable, example:"
		log_error "export PM=\"apt\""
		return 1
	fi
	log_info "Setting pkg manager to ${PM}."
	echo "${PM}"
	return 0
}

# Setup build dir and ffmpeg version/directory.
# FFMPEG_VER taken from environment or forced by 1st parameter
# Exports FFMPEG_DIR and FFMPEG_VER
function lib_setup_ffmpeg_dir_and_version() {
	FFMPEG_VER="${1:-$FFMPEG_VER}"
	FFMPEG_7_0_DIR="${FFMPEG_7_0_DIR:-ffmpeg-7-0}"
	FFMPEG_6_1_DIR="${FFMPEG_6_1_DIR:-ffmpeg-6-1}"

	if [[ "${FFMPEG_VER}" == "7.0" ]]; then
		FFMPEG_SUB_DIR="${FFMPEG_7_0_DIR}"
	elif [[ "${FFMPEG_VER}" == "6.1" ]]; then
		FFMPEG_SUB_DIR="${FFMPEG_6_1_DIR}"
	else
		log_error "Unsupported version of FFmpeg == '${FFMPEG_VER}'."
		log_error "Try again, choose one of '7.0', '6.1'."
		exit 2
	fi
	export FFMPEG_VER
	export FFMPEG_SUB_DIR
}

function exec_command() {
	# One of: yes|no|accept-new
	SSH_STRICT_HOST_KEY_CHECKING="accept-new"
	SSH_CMD="ssh -oStrictHostKeyChecking=${SSH_STRICT_HOST_KEY_CHECKING} -t -o"

	local values_returned=""
	local user_at_address=""
	[[ "$#" -eq "2" ]] && user_at_address="${2}"
	[[ "$#" -eq "3" ]] && user_at_address="${3}@${2}"

	if [ "$#" -eq "1" ]; then
		values_returned="$(eval "${1}")"
	elif [[ "$#" -eq "2" ]] || [[ "$#" -eq "3" ]]; then
		values_returned="$($SSH_CMD "RemoteCommand=eval \"${1}\"" "${user_at_address}" 2>/dev/null)"
	else
		log_error "Wrong arguments for exec_command(). Valid number is one of [1 2 3], got $#"
		return 1
	fi

	if [ -z "$values_returned" ]; then
		log_error "Unable to collect results or results are empty."
		return 1
	else
		echo "${values_returned}"
		return 0
	fi
}

function get_hostname() {
	exec_command 'hostname' "$@"
}

function get_intel_nic_device() {
	exec_command "lspci | grep 'Intel Corporation.*\(810\|X722\)'" "$@"
}

function get_default_route_nic() {
	exec_command "ip -json r show default | jq '.[0].dev' -r" "$@"
}

function get_cpu_arch() {
	local arch=""

	if ! arch="$(exec_command 'cat /sys/devices/cpu/caps/pmu_name' "$@")"; then echo "Got: $arch" && return 1; fi

	case $arch in
	icelake)
		log_info "Xeon IceLake CPU (icx)" 1>&2
		echo "icx"
		;;
	sapphire_rapids)
		log_info "Xeon Sapphire Rapids CPU (spr)" 1>&2
		echo "spr"
		;;
	skylake)
		log_info "Xeon SkyLake"
		echo "skl"
		;;
	*)
		log_error "Unsupported architecture: ${arch}. Please edit the script or setup the architecture manually."
		return 1
		;;
	esac
	return 0
}
