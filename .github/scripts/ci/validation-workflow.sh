#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
operation=${1:?usage: validation-workflow.sh OPERATION}

build_packages=(
	git gcc meson tar zip pkg-config python3 python3-pyelftools
	python3-virtualenv python3-pip libnuma-dev libjson-c-dev libpcap-dev
	libgtest-dev libsdl2-dev libsdl2-ttf-dev libssl-dev systemtap-sdt-dev
	libbpf-dev libelf1
)

bind_kernel() {
	sudo rmmod irdma 2>/dev/null || true
	sudo "${root_dir}/script/nicctl.sh" bind_kernel "${TEST_PF_PORT_P}" || true
	sudo "${root_dir}/script/nicctl.sh" bind_kernel "${TEST_PF_PORT_R}" || true
}

case "$operation" in
print-environment) env | grep TEST_ || true ;;
read-dpdk-version)
	# shellcheck source=/dev/null
	. "${root_dir}/versions.env"
	echo "DPDK_VERSION=${DPDK_VER:?}" >>"${GITHUB_ENV:?}"
	;;
verify-dependencies)
	# A CI job never installs onto the runner: apt during a job mutates a shared
	# host, races with other jobs on it and hides drift in the host image. The
	# job only states what it needs, and a missing package is a host fault.
	missing=()
	for pkg in "${build_packages[@]}"; do
		dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q 'ok installed' ||
			missing+=("$pkg")
	done
	if [[ ${#missing[@]} -gt 0 ]]; then
		echo "Missing build packages on this runner: ${missing[*]}" >&2
		echo "Provision the host once with: task ci:validation -- install-dependencies" >&2
		exit 1
	fi
	echo "All ${#build_packages[@]} build packages present"
	;;
install-dependencies)
	# Provisioning path, run by hand on the host. No CI job calls this.
	sudo apt update
	sudo apt-get remove -y pipenv || true
	sudo apt-get install -y "${build_packages[@]}"
	;;
patch-dpdk) patch -d "${root_dir}/dpdk" -p1 -i <(cat "${root_dir}/patches/dpdk/${DPDK_VERSION}"/*.patch) ;;
build-dpdk)
	meson setup "${root_dir}/dpdk/build" "${root_dir}/dpdk"
	ninja -C "${root_dir}/dpdk/build"
	sudo ninja -C "${root_dir}/dpdk/build" install
	;;
build-mtl)
	"${root_dir}/build.sh"
	sudo ldconfig
	;;
verify-pipenv)
	# Same rule for Python: the pipenv environment belongs to the host image.
	cd "${root_dir}/tests/acceptance"
	if ! venv_path=$(python3 -m pipenv --venv 2>/dev/null); then
		echo "No pipenv environment for ${PWD} on this runner." >&2
		echo "Provision the host once with: task ci:validation -- install-pipenv" >&2
		exit 1
	fi
	activate_path="${venv_path}/bin/activate"
	echo "VIRTUAL_ENV=${activate_path}" >>"${GITHUB_ENV:?}"
	echo "VIRTUAL_ENV=${activate_path}" >>"${GITHUB_OUTPUT:?}"
	;;
install-pipenv)
	# Provisioning path, run by hand on the host. No CI job calls this.
	cd "${root_dir}/tests/acceptance"
	python3 -m pip install pipenv
	python3 -m pipenv install -r requirements.txt
	activate_path="$(python3 -m pipenv --venv)/bin/activate"
	echo "VIRTUAL_ENV=${activate_path}" >>"${GITHUB_ENV:?}"
	echo "VIRTUAL_ENV=${activate_path}" >>"${GITHUB_OUTPUT:?}"
	;;
select-ports)
	case "${PORT_P_NAME:?}" in TEST_*_PORT_*) ;; *) exit 2 ;; esac
	case "${PORT_R_NAME:?}" in TEST_*_PORT_*) ;; *) exit 2 ;; esac
	test_port_p=${!PORT_P_NAME:?}
	test_port_r=${!PORT_R_NAME:?}
	printf 'TEST_PORT_P=%s\nTEST_PORT_R=%s\n' "$test_port_p" "$test_port_r" | tee -a "${GITHUB_ENV:?}"
	;;
bind-kernel) bind_kernel ;;
bind-interface)
	sudo rmmod irdma 2>/dev/null || true
	sudo "${root_dir}/script/nicctl.sh" "${VALIDATION_IFACE_BINDING:?}" "${TEST_PF_PORT_P}" || true
	sudo "${root_dir}/script/nicctl.sh" "${VALIDATION_IFACE_BINDING:?}" "${TEST_PF_PORT_R}" || true
	;;
run-tests)
	cd "${root_dir}/tests/acceptance"
	bash "${root_dir}/.github/scripts/run_validation_tests.sh"
	;;
prerelease) echo "== TO BE IMPLEMENTED ${VALIDATION_PRERELEASE:?} ==" ;;
archive-logs)
	cd "${root_dir}/tests/acceptance"
	sudo tar -czf validation-execution-logs.tar.gz ./logs
	sudo rm -rf ./logs
	;;
restore-owner) sudo chown -R "${USER}" "$root_dir" ;;
summary)
	{
		echo "## Runner ${RUNNER_NAME:?}"
		echo "Below are variables defined on the ${RUNNER_NAME} self-hosted runner"
		echo "| Variable | Value |"
		echo "| --- | --- |"
		for name in TEST_PF_PORT_P TEST_PF_PORT_R TEST_PORT_P TEST_PORT_R \
			TEST_DMA_PORT_P TEST_DMA_PORT_R TEST_VF_PORT_P_0 TEST_VF_PORT_P_1 \
			TEST_VF_PORT_P_2 TEST_VF_PORT_P_3 TEST_VF_PORT_R_0 TEST_VF_PORT_R_1 \
			TEST_VF_PORT_R_2 TEST_VF_PORT_R_3; do
			echo "| ${name} | ${!name:-} |"
		done
	} >>"${GITHUB_STEP_SUMMARY:?}"
	;;
*)
	echo "unknown validation workflow operation: ${operation}" >&2
	exit 2
	;;
esac
