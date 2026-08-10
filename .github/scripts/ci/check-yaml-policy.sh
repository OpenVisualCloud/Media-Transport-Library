#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
policy_root=${CI_YAML_POLICY_ROOT:-"${root_dir}/.github"}
violations=$(mktemp)
trap 'rm -f "$violations"' EXIT

grep -RInE --include='*.yml' --include='*.yaml' \
	'^[[:space:]]*(-[[:space:]]*)?(run|script):[[:space:]]*[|>][-+]?[[:space:]]*$' \
	"${policy_root}/workflows" "${policy_root}/actions" >"$violations" || true

if [ -s "$violations" ]; then
	echo "Active workflow/composite YAML contains inline programs:" >&2
	sed "s|${policy_root}/|.github/|" "$violations" >&2
	echo ".github/legacy is excluded because it is not an active Actions surface." >&2
	exit 1
fi

grep -RInE --include='*.yml' --include='*.yaml' '^[[:space:]]*(-[[:space:]]*)?uses:' \
	"${policy_root}/workflows" "${policy_root}/actions" |
	grep -vE 'uses:[[:space:]]*(\./.*|docker://.*|[^[:space:]@]+@[0-9a-f]{40}([[:space:]]*#.*)?)$' \
	>"$violations" || true
if [ -s "$violations" ]; then
	echo "Active workflow/composite YAML contains mutable external actions:" >&2
	sed "s|${policy_root}/|.github/|" "$violations" >&2
	exit 1
fi

echo "active YAML inline-program policy: PASS"