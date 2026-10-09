#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

# Does the card this job runs against need the Kahawai ICE driver?
#
# The ICE artifact, its validation and its activation all exist to align one
# kernel module -- the ice driver of the E8xx family -- with the MTL version
# under test. A runner whose card is served by a different driver has nothing to
# align: an i225/i226 is an igc device, and demanding an ice.ko built for this
# kernel there fails a job for a reason with no bearing on what it tests.
#
# The answer follows from the NIC label, which is what decides the card, so it is
# the same answer on every runner carrying that label. An empty label -- an
# unlabeled runner, such as the performance rig -- is assumed to need it, which
# is the behaviour every job had before this existed.
#
# Prints true or false, and appends required=<answer> to $GITHUB_OUTPUT when the
# runner provides one. The exit status is success either way: "no" is an answer,
# not a failure.

nic=${1:-${NIC:-}}

case "${nic}" in
i225 | i226) required=false ;;
*) required=true ;;
esac

echo "${required}"
if [[ -n ${GITHUB_OUTPUT:-} ]]; then
	printf 'required=%s\n' "${required}" >>"${GITHUB_OUTPUT}"
fi
