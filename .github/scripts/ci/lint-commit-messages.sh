#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# gitlint every commit in a pull request one message at a time, so the report
# names the commit that failed instead of the range.

set -euo pipefail

base_sha=${BASE_SHA:?usage: BASE_SHA and HEAD_SHA name the pull-request range}
head_sha=${HEAD_SHA:?usage: BASE_SHA and HEAD_SHA name the pull-request range}

message=$(mktemp)
trap 'rm -f "$message"' EXIT

rc=0
for sha in $(git rev-list "${base_sha}..${head_sha}"); do
	git log -1 --format=%B "$sha" >"$message"
	echo "=== $(git log -1 --format='%h %s' "$sha") ==="
	pre-commit run --hook-stage commit-msg \
		--commit-msg-filename "$message" gitlint || rc=1
done
exit "$rc"
