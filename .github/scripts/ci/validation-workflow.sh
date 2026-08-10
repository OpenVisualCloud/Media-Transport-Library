#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
operation=${1:?usage: validation-workflow.sh OPERATION}

bind_kernel() {
	sudo rmmod irdma 2>/dev/null || true
	sudo "${root_dir}/script/nicctl.sh" bind_kernel "${TEST_PF_PORT_P}" || true
	sudo "${root_dir}/script/nicctl.sh" bind_kernel "${TEST_PF_PORT_R}" || true
}

case "$operation" in
print-environment) env | grep TEST_ || true ;;
install-dependencies)
	sudo apt update
	sudo apt-get remove -y pipenv || true
	sudo apt-get install -y git gcc meson tar zip pkg-config python3 python3-pyelftools \
		python3-virtualenv python3-pip libnuma-dev libjson-c-dev libpcap-dev \
		libgtest-dev libsdl2-dev libsdl2-ttf-dev libssl-dev systemtap-sdt-dev \
		libbpf-dev libelf1
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
install-pipenv)
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
*) echo "unknown validation workflow operation: ${operation}" >&2; exit 2 ;;
esac