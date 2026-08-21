#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

# The EBU LIST analyser that turns a captured pcap into an ST 2110 verdict.
#
# The acceptance framework does not judge compliance itself: `pcap_capture`
# records with netsniff-ng, posts the pcap to an EBU LIST instance and reads the
# verdict back out of the report (tests/acceptance/mtl_engine/pcap_compliance.py).
# So a host that captures perfectly still produces no verdict without a reachable
# analyser, and the failure surfaces late, inside a test, as "PCAP upload to EBU
# LIST failed". This checks for it up front, and holds the one description of how
# the instance is deployed so the next operator does not have to rediscover it.
#
# EBU LIST is a docker compose stack, deployed from the ebu-list directory of the
# internal Media-Transport-Library-Devtools repository. It is kept outside the MTL
# checkout so it never appears in `git status`:
#
#   gh repo clone intel-sandbox/Media-Transport-Library-Devtools ~/mtl/devtools
#   cd ~/mtl/devtools/ebu-list && cp .env.template .env   # then edit .env
#
# Usage: ebu-list.sh {status|up|down|verify}

# The endpoint and credentials are lab facts, so they come from the same host
# file the acceptance configuration is generated from rather than from a second
# copy here. See .github/ci-local/runner.env.example.
runner_env=${MTL_CI_RUNNER_ENV:-/etc/mtl-ci/runner.env}
if [[ -r ${runner_env} ]]; then
	set -a
	# shellcheck source=/dev/null
	source "${runner_env}"
	set +a
fi

# Where the compose stack was cloned. A default is worth having because every
# host in the fleet is provisioned the same way, but EBU_LIST_DIR in runner.env
# wins for a host that put it elsewhere.
compose_dir=${EBU_LIST_DIR:-${HOME}/mtl/devtools/ebu-list}

ebu_ip=${EBU_IP:-}
ebu_user=${EBU_USER:-}
ebu_password=${EBU_PASSWORD:-}

provision_hint() {
	echo "Provision it on the host once with: task ci:ebu-list -- up" >&2
	echo "The stack is expected in ${compose_dir} (override with EBU_LIST_DIR)." >&2
	echo "Set EBU_IP/EBU_USER/EBU_PASSWORD in ${runner_env}." >&2
}

# EBU LIST is reached over plain HTTP on the lab network and the analyser is a
# lab service, so curl is told to ignore the proxy: this host exports http_proxy
# for internet access, and without --noproxy an upload to a lab address is sent
# to a proxy that cannot route to it. The Python client does the same by setting
# session.trust_env = False.
list_curl() {
	curl --silent --show-error --noproxy '*' --max-time 15 "$@"
}

# The token endpoint doubles as the reachability check: it proves the web server
# answers, that the API is the version the framework speaks (the token arrives as
# content.token, which compliance_client.py reads), and that the configured
# account exists. A 200 on / would prove only that nginx is up.
login_token() {
	list_curl --request POST \
		--header 'Content-Type: application/json' \
		--data "{\"username\": \"${ebu_user}\", \"password\": \"${ebu_password}\"}" \
		"http://${ebu_ip}/auth/login" |
		sed -n 's/.*"token":"\([^"]*\)".*/\1/p'
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
	printf '%-14s %s\n' 'stack dir' "${compose_dir}"
	printf '%-14s %s\n' 'EBU_IP' "${ebu_ip:-<unset>}"
	printf '%-14s %s\n' 'EBU_USER' "${ebu_user:-<unset>}"
	printf '%-14s %s\n' 'runner env' "$([[ -r ${runner_env} ]] && echo "${runner_env}" || echo "${runner_env} (absent)")"
	if [[ -f ${compose_dir}/docker-compose.yml ]]; then
		compose ps
	else
		echo "No compose stack in ${compose_dir}"
	fi
	if [[ -n ${ebu_ip} ]]; then
		printf '%-14s %s\n' 'auth' \
			"$([[ -n $(login_token || true) ]] && echo "ok (http://${ebu_ip})" || echo "no token from http://${ebu_ip}/auth/login")"
	fi
	;;
up)
	# Provisioning path, run by hand on the host. No CI job calls this: bringing a
	# multi-container service up is a change to a host that other jobs share.
	compose up --detach
	echo "EBU LIST starting; the analyser needs a few seconds before it answers."
	echo "Check it with: task ci:ebu-list -- verify"
	;;
down)
	compose down
	;;
verify)
	# CI-facing check. Everything it can find wrong is a host problem, so it
	# fails with the command that fixes it rather than trying to fix it.
	failed=0

	# The framework shells out to `sudo netsniff-ng` for the capture itself
	# (create_pcap_file/netsniff.py), because opening a PF_PACKET socket and
	# raising the socket memory limits both need privilege. Over SSH there is no
	# terminal to prompt on, so the sudo rule has to be passwordless.
	if ! command -v netsniff-ng >/dev/null 2>&1; then
		echo "Missing netsniff-ng, so no pcap can be captured to judge." >&2
		echo "Install it on the host once with: sudo apt-get install -y netsniff-ng" >&2
		failed=1
	elif ! sudo -n true 2>/dev/null; then
		echo "Passwordless sudo is not available, so sudo netsniff-ng cannot capture." >&2
		echo "Grant it on the host once with: sudo usermod -aG sudo $(id -un)" >&2
		failed=1
	fi

	if [[ -z ${ebu_ip} ]]; then
		# Absence is not misconfiguration. pytest-setup.sh omits the ebu_server
		# block when EBU_IP is unset and the suite then runs without a compliance
		# verdict -- every transport case still transmits, receives and compares.
		# So this announces the degraded mode instead of failing the leg: a check
		# added to stop a late failure inside a test must not become an earlier
		# failure of its own, which is what it was on a fleet where no host has
		# the analyser deployed yet.
		#
		# A host that has provisioned it sets MTL_CI_REQUIRE_COMPLIANCE=1 in
		# runner.env, and then losing it is a failure rather than a quiet
		# downgrade.
		echo "EBU_IP is unset in ${runner_env}; tests will run without a compliance verdict." >&2
		provision_hint
		if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
			echo "No ST 2110 compliance verdict: EBU_IP is unset on $(hostname)." \
				>>"${GITHUB_STEP_SUMMARY}"
		fi
		if [[ ${MTL_CI_REQUIRE_COMPLIANCE:-0} == 1 ]]; then
			echo "MTL_CI_REQUIRE_COMPLIANCE=1 on this host, so this is a failure." >&2
			exit 1
		fi
		exit "${failed}"
	fi

	if [[ -z ${ebu_user} || -z ${ebu_password} ]]; then
		echo "EBU_IP is set but EBU_USER/EBU_PASSWORD are not; the analyser will reject every upload." >&2
		provision_hint
		exit 1
	fi

	if [[ -z $(login_token || true) ]]; then
		echo "EBU LIST at http://${ebu_ip} did not return a token for user ${ebu_user}." >&2
		echo "Either the stack is down or the account does not exist." >&2
		provision_hint
		exit 1
	fi
	echo "EBU LIST at http://${ebu_ip} authenticated ${ebu_user} and speaks the expected API."

	[[ ${failed} -eq 0 ]] || exit 1
	;;
*)
	echo "Usage: $0 {status|up|down|verify}" >&2
	exit 2
	;;
esac
