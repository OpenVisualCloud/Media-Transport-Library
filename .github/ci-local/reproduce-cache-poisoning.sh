#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Reproduce, locally and in a couple of minutes, the CI failure
#
#   ::notice::DPDK=HIT  MTL=HIT  FFmpeg=MISS ...
#   ERROR: mtl >= 22.12.0 not found using pkg-config
#
# on a branch that changed nothing feeding the `mtl` cache key.
#
# The cause is not in the sources. `actions/cache` saves in a post step that
# runs whether the job passed or not, so a run that died part-way through
# installing MTL stores its half-written tree under stash-mtl-<checksum>.
# Every later run with the same sources restores that tree, sees
# cache-hit == true, skips the MTL build, and then fails in the first consumer
# that resolves mtl.pc -- the FFmpeg plugin's configure. The key never changes,
# so the entry cannot age out on its own.
#
# This script plays that out in four steps:
#
#   1. build everything, so the cache is warm and valid
#   2. poison it: strip the MTL tree the way an aborted install would, and
#      leave its cache key in place
#   3. run the job the way the workflow behaves today -- expect the CI failure
#   4. run it with the proposed rule, that a hit must be usable -- expect a pass
#
# Usage: reproduce-cache-poisoning.sh

set -uo pipefail

script_path="$(readlink -f "${BASH_SOURCE[0]}")"
CI_LOCAL_DIR="$(dirname "${script_path}")"
REPO_ROOT="$(cd "${CI_LOCAL_DIR}/../.." && pwd)"
RUN_JOB="${CI_LOCAL_DIR}/run-job.sh"
CACHE_DIR="${REPO_ROOT}/.local_install"
LOG_DIR="${REPO_ROOT}/.ci-local/logs"

# The CI error, verbatim. ecosystem/ffmpeg_plugin/*/0001-*.patch adds the
# require_pkg_config call that emits it.
CI_ERROR="mtl >= 22.12.0 not found using pkg-config"

step() {
	echo
	echo "═══ $* ═══"
}

step "1/4  warm the cache"
"${RUN_JOB}" build >/dev/null 2>&1
rc=$?
if [ "${rc}" -ne 0 ]; then
	echo "FAIL: could not get a clean build to start from (exit ${rc})"
	echo "      see ${LOG_DIR}"
	exit 1
fi
echo "cache is warm and valid"

step "2/4  poison the MTL entry, as a failed run would"
# An install that dies part-way leaves some of the tree behind. Keep bin/,
# drop everything else -- lib/, and with it pkgconfig/mtl.pc.
find "${CACHE_DIR}/mtl" -mindepth 1 -maxdepth 1 ! -name bin -exec rm -rf {} + 2>/dev/null
echo "MTL tree now holds: $(find "${CACHE_DIR}/mtl" -mindepth 1 -maxdepth 1 -printf '%f ')"
echo "MTL cache key still: $(cut -c1-16 "${CACHE_DIR}/.stamps/mtl")..."
echo "(the sources did not change, so the key is still considered valid)"

step "3/4  run as the workflow behaves today  (--cache-mode github)"
# --force ffmpeg stands in for a pull request that touched the FFmpeg plugin:
# its cache key changes, MTL's does not. MTL is restored from the poisoned
# entry, and the FFmpeg build is the first thing to ask for mtl.pc.
"${RUN_JOB}" build --cache-mode github --force ffmpeg >"${LOG_DIR}/poisoned.log" 2>&1
poisoned_rc=$?
grep -E "MTL=HIT|${CI_ERROR}" "${LOG_DIR}/poisoned.log" | tail -3

step "4/4  run with the proposed rule  (default --cache-mode strict)"
"${RUN_JOB}" build --force ffmpeg >"${LOG_DIR}/fixed.log" 2>&1
fixed_rc=$?
grep -E "^cache: " "${LOG_DIR}/fixed.log" | tail -1

echo
echo "=== verdict ==="
if [ "${poisoned_rc}" -ne 0 ] && grep -q "${CI_ERROR}" "${LOG_DIR}/poisoned.log"; then
	echo "reproduced:  github cache semantics -> exit ${poisoned_rc}, '${CI_ERROR}'"
else
	echo "NOT reproduced: expected a failure carrying '${CI_ERROR}'"
	echo "                got exit ${poisoned_rc}, see ${LOG_DIR}/poisoned.log"
fi
if [ "${fixed_rc}" -eq 0 ]; then
	echo "fixed:       strict cache semantics -> MTL reported STALE, rebuilt, exit 0"
else
	echo "NOT fixed:   strict semantics still failed, exit ${fixed_rc}"
	echo "             see ${LOG_DIR}/fixed.log"
fi
echo
echo "logs: ${LOG_DIR}/poisoned.log  ${LOG_DIR}/fixed.log"

[ "${poisoned_rc}" -ne 0 ] && [ "${fixed_rc}" -eq 0 ]
