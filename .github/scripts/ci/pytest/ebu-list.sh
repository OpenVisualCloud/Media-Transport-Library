#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

runner_env=${MTL_CI_RUNNER_ENV:-/etc/mtl-ci/runner.env}
if [[ -r ${runner_env} ]]; then
	set -a
	# shellcheck source=/dev/null
	source "${runner_env}"
	set +a
fi
compose_dir=${EBU_LIST_DIR:-${HOME}/mtl/devtools/ebu-list}
ebu_ip=${EBU_IP:-}
ebu_user=${EBU_USER:-}

hint() {
	echo "Provision it on the host once with: task ci:ebu-list -- up" >&2
	echo "The stack is expected in ${compose_dir} (override with EBU_LIST_DIR)." >&2
	echo "Set EBU_IP/EBU_USER/EBU_PASSWORD in ${runner_env}." >&2
}

# --noproxy: the host's http_proxy cannot route to the lab analyser.
token() {
	curl --silent --show-error --noproxy '*' --max-time 15 --request POST \
		--header 'Content-Type: application/json' \
		--data "{\"username\": \"${ebu_user}\", \"password\": \"${EBU_PASSWORD:-}\"}" \
		"http://${ebu_ip}/auth/login" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p' || true
}

compose() {
	[[ -f ${compose_dir}/docker-compose.yml ]] || {
		echo "No EBU LIST compose stack in ${compose_dir}." >&2
		echo "Clone it with: gh repo clone intel-sandbox/Media-Transport-Library-Devtools $(dirname "${compose_dir}")" >&2
		return 1
	}
	(cd "${compose_dir}" && docker compose "$@")
}

case "${1:-}" in
status)
	echo "stack dir: ${compose_dir}, EBU_IP: ${ebu_ip:-<unset>}, EBU_USER: ${ebu_user:-<unset>}, runner env: ${runner_env}"
	compose ps || true
	[[ -z ${ebu_ip} ]] || echo "auth: $([[ -n $(token) ]] && echo ok || echo "no token") from http://${ebu_ip}/auth/login"
	;;
up)
	compose up --detach
	echo "EBU LIST starting; check it in a few seconds with: task ci:ebu-list -- verify"
	;;
down)
	compose down
	;;
verify)
	failed=0
	if ! command -v netsniff-ng >/dev/null; then
		echo "Missing netsniff-ng; install it on the host once with: sudo apt-get install -y netsniff-ng" >&2
		failed=1
	elif ! sudo -n true 2>/dev/null; then
		echo "sudo netsniff-ng needs passwordless sudo; grant it on the host once with: sudo usermod -aG sudo $(id -un)" >&2
		failed=1
	fi
	if [[ -z ${ebu_ip} ]]; then
		# Without EBU_IP the suite still runs, just without a verdict, unless the host requires one.
		echo "EBU_IP is unset in ${runner_env}; tests will run without a compliance verdict." >&2
		hint
		[[ -z ${GITHUB_STEP_SUMMARY:-} ]] ||
			echo "No ST 2110 compliance verdict: EBU_IP is unset on $(hostname)." >>"${GITHUB_STEP_SUMMARY}"
		if [[ ${MTL_CI_REQUIRE_COMPLIANCE:-0} == 1 ]]; then
			echo "MTL_CI_REQUIRE_COMPLIANCE=1 on this host, so this is a failure." >&2
			exit 1
		fi
		exit "${failed}"
	fi
	if [[ -z ${ebu_user} || -z ${EBU_PASSWORD:-} ]]; then
		echo "EBU_IP is set but EBU_USER/EBU_PASSWORD are not; the analyser will reject every upload." >&2
		hint
		exit 1
	fi
	if [[ -z $(token) ]]; then
		echo "EBU LIST at http://${ebu_ip} returned no token for ${ebu_user}: the stack is down or the account is missing." >&2
		hint
		exit 1
	fi
	echo "EBU LIST at http://${ebu_ip} authenticated ${ebu_user}."
	exit "${failed}"
	;;
*)
	echo "Usage: $0 {status|up|down|verify}" >&2
	exit 2
	;;
esac
