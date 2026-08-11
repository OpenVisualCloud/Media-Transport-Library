#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
bash "${root_dir}/.github/scripts/setup_environment.sh"

if [[ -n ${REMOTE_HOST:-} ]]; then
	branch=${REMOTE_BRANCH:-}
	branch=${branch#refs/heads/}
	env_file=${REMOTE_ENV_FILE:-~/.mtl_build_env}
	env_file=${env_file/#\~/$HOME}
	{
		[[ -f $env_file ]] && cat "$env_file" || echo "::warning::No env file at $env_file; using defaults"
		cat "${root_dir}/.github/scripts/remote_build.sh"
	} | ssh -o StrictHostKeyChecking=no "$REMOTE_HOST" bash -s -- "${REMOTE_MTL_PATH:-}" "$branch"
fi
