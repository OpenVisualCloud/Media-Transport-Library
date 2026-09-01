#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Where the acceptance-test virtualenv lives, for every script that runs
# pytest or a helper out of it.
#
# The virtualenv is a host prerequisite: a job verifies it and never installs
# it, the way it treats the media share and the apt packages. That contract
# cannot hold inside the workspace. Every hardware job starts with
# actions/checkout, whose default clean is `git clean -ffdx`, and
# tests/acceptance/.venv is gitignored -- so `-x` deletes it before the step
# that verifies it runs, and "provision it on the runner once" is provisioned
# for exactly one job. The fleet legs failed with `Missing acceptance
# virtualenv ... Provision it on the runner once` on hosts where it had been.
#
# So the default lives under the runner user's cache directory, outside
# anything git touches. Resolution order:
#
#   $MTL_CI_VENV              an explicit location, for a host that keeps it
#                             somewhere else
#   tests/acceptance/.venv    an in-workspace virtualenv that already exists;
#   tests/acceptance/venv     a developer's own `python3 -m venv` keeps working,
#                             and a local run does not have to provision a
#                             second copy. Both names are accepted because the
#                             repository provisions both: setup_acceptance.sh
#                             writes `venv`, this script's install case used to
#                             write `.venv`, and `verify` only ever looked for
#                             the latter -- so a host set up with the former had
#                             a working virtualenv the job called missing.
#   $XDG_CACHE_HOME or ~/.cache, then mtl-ci/acceptance-venv
#                             the fleet default, which survives checkout
#
# Usage: source this file, then use ${acceptance_venv} / ${venv_python}.

if [[ -n "${_MTL_ACCEPTANCE_VENV_SH_:-}" ]]; then
	# shellcheck disable=SC2317 # exit is reached when run directly, not sourced
	return 0 2>/dev/null || exit 0
fi
_MTL_ACCEPTANCE_VENV_SH_=1

_mv_repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

if [[ -n "${MTL_CI_VENV:-}" ]]; then
	acceptance_venv="${MTL_CI_VENV}"
elif [[ -x "${_mv_repo_root}/tests/acceptance/.venv/bin/python3" ]]; then
	acceptance_venv="${_mv_repo_root}/tests/acceptance/.venv"
elif [[ -x "${_mv_repo_root}/tests/acceptance/venv/bin/python3" ]]; then
	acceptance_venv="${_mv_repo_root}/tests/acceptance/venv"
else
	acceptance_venv="${XDG_CACHE_HOME:-${HOME}/.cache}/mtl-ci/acceptance-venv"
fi

# shellcheck disable=SC2034 # read by the scripts that source this file
venv_python="${acceptance_venv}/bin/python3"
