#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

runner_env=/etc/mtl-ci/runner.env
if [[ -r ${runner_env} ]]; then
	set -a
	# shellcheck source=/dev/null
	source "${runner_env}"
	set +a
fi
ebu_ip=${EBU_IP:-}
ebu_user=${EBU_USER:-}

# --noproxy: the host's http_proxy cannot route to the lab analyser. jq reads the
# account from the environment and the body goes on stdin, because another user
# can read the arguments of a process.
token() {
	jq -n '{username: env.EBU_USER, password: env.EBU_PASSWORD}' |
		curl --silent --show-error --noproxy '*' --max-time 15 --request POST \
			--header 'Content-Type: application/json' --data @- \
			"http://${ebu_ip}/auth/login" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p' || true
}

case "${1:-}" in
verify)
	failed=0
	if ! command -v netsniff-ng >/dev/null; then
		echo "netsniff-ng is not installed" >&2
		failed=1
	elif ! sudo -n true 2>/dev/null; then
		echo "sudo netsniff-ng needs passwordless sudo" >&2
		failed=1
	fi
	if [[ -z ${ebu_ip} ]]; then
		# Without EBU_IP the suite still runs, just without a verdict, unless the host requires one.
		echo "EBU_IP is unset in ${runner_env}; tests will run without a compliance verdict." >&2
		[[ -z ${GITHUB_STEP_SUMMARY:-} ]] ||
			echo "No ST 2110 compliance verdict: EBU_IP is unset on $(hostname)." >>"${GITHUB_STEP_SUMMARY}"
		[[ ${MTL_CI_REQUIRE_COMPLIANCE:-0} != 1 ]] || exit 1
		exit "${failed}"
	fi
	if [[ -z ${ebu_user} || -z ${EBU_PASSWORD:-} ]]; then
		echo "EBU_IP is set but EBU_USER/EBU_PASSWORD are not in ${runner_env}" >&2
		exit 1
	fi
	if [[ -z $(token) ]]; then
		echo "EBU LIST at http://${ebu_ip} returned no token for ${ebu_user}" >&2
		exit 1
	fi
	echo "EBU LIST at http://${ebu_ip} authenticated ${ebu_user}."
	exit "${failed}"
	;;
*)
	echo "Usage: $0 {verify}" >&2
	exit 2
	;;
esac
