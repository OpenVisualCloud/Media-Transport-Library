#!/usr/bin/env bash
# Create a DMF CRM MxlMediaFunctionParameters resource manifest.
#
# Prerequisites: completed PerfSpect baseline and profiling run.
# See docs/15-dmf-crm-manifest.md for full documentation.
#
# Usage:
#   scripts/create-dmf-manifest.sh [OPTIONS]
#
# Quick example:
#   scripts/create-dmf-manifest.sh \
#       --perfspect results/perfspect/k8s-w2/latest \
#       --profile   results/pinned-20240115T120000Z \
#       --service   encoder \
#       --output    manifest/mxl-encoder-pinned.yaml
#
# All options are forwarded to python/mxlperf/dmf_manifest.py.
# Run  scripts/create-dmf-manifest.sh --help  for the full option list.
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Ensure the mxlperf package is on PYTHONPATH.
export PYTHONPATH="${ROOT}/python${PYTHONPATH:+:$PYTHONPATH}"

exec python3 -m mxlperf.dmf_manifest "$@"
