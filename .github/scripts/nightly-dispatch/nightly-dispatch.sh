#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

# Start the nightly workflows now, through the workflow_dispatch API. The
# workflows have no schedule of their own; doc/ci_runner_setup.md says why, and
# how mtl-nightly-dispatch.timer runs this.
#
# Both are posted from one place on purpose: Nightly Combined Report pairs the
# latest completed run of each, so the two have to start in the same window.
#
# No --retry: a POST that timed out may still have created its run, and a
# second one would put the whole suite on the runners twice. A missed start is
# recovered by hand.

repo=OpenVisualCloud/Media-Transport-Library
token=$(<"${CREDENTIALS_DIRECTORY:?run this from mtl-nightly-dispatch.service}/token")

# One failed dispatch does not stop the other: a nightly that does start is
# still worth its results.
status=0
for workflow in nightly-gtest.yml nightly-pytest.yml; do
	# The header goes in on stdin so that the token never reaches an argv,
	# which any local user can read in /proc.
	if printf 'Authorization: Bearer %s\n' "${token}" |
		curl -sS --fail-with-body --max-time 30 -X POST -H @- \
			-H 'Accept: application/vnd.github+json' \
			-H 'X-GitHub-Api-Version: 2022-11-28' \
			-d '{"ref":"main"}' \
			"https://api.github.com/repos/${repo}/actions/workflows/${workflow}/dispatches"; then
		echo
		echo "${workflow}: dispatched on main"
	else
		echo
		echo "${workflow}: dispatch failed" >&2
		status=1
	fi
done
exit "${status}"
