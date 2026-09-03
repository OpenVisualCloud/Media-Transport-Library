#!/usr/bin/env bash
set -Eeuo pipefail

[[ ${EUID} -eq 0 ]] || { echo "FATAL: run as root on Kubernetes worker" >&2; exit 2; }
command -v apt-get >/dev/null || { echo "FATAL: apt-get is required" >&2; exit 2; }

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y stress-ng util-linux procps numactl

command -v stress-ng >/dev/null
command -v taskset >/dev/null
command -v setsid >/dev/null
command -v numactl >/dev/null
stress-ng --help 2>&1 | grep -F -- '--cache-no-affinity' >/dev/null || {
  echo "FATAL: installed stress-ng lacks --cache-no-affinity" >&2
  exit 2
}
stress-ng --help 2>&1 | grep -F -- '--stream-l3-size' >/dev/null || {
  echo "FATAL: installed stress-ng lacks --stream-l3-size" >&2
  exit 2
}
stress-ng --version
echo "Host-wide LLC and bandwidth noisy-neighbor prerequisites ready."
