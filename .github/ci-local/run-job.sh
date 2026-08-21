#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Run a job of .github/workflows/build.yml locally, inside a container that
# simulates the GitHub Actions runner.
#
# The job steps are not re-implemented here. This script only plays the part
# GitHub itself plays -- prepare a runner, restore the caches, hand the job a
# workspace and an environment -- and then executes the job script under
# .github/ci-local/jobs/, which mirrors the workflow steps one for one.
#
#   GitHub                              this script
#   ----------------------------------  ------------------------------------
#   build on runs-on: e835              .github/ci-local/Dockerfile
#   actions/checkout                    rsync of the working tree
#   actions/cache (restore)             immutable snapshots under <cache>/.cache-store
#   job steps                           .github/ci-local/jobs/<job>.sh
#   actions/cache/save                  immutable snapshot, on success only
#
# Two things are deliberately stricter than CI, because CI's laxness is what
# produced the "mtl >= 22.12.0 not found using pkg-config" failure:
#
#   * a restored tree only counts as a hit if it is structurally usable, not
#     merely present;
#   * the cache is only saved when the job succeeded, so a half-built tree can
#     never be promoted to a permanent hit.
#
# A third runner kind, --runner host, runs the job on this machine instead of in
# a container. A container cannot bind a PCI device, reserve hugepages or load a
# driver, so a job that drives a NIC has nothing to prove inside one; on the
# machine that owns the card the same job script is the real thing, with this
# script playing only GitHub's part. In host mode the caches are validated where
# they are and never restored or deleted: the tree in .local_install is what the
# build job's artifact would have restored to, and throwing it away to model a
# cache miss would break a run for local reasons CI does not have.
#
# Usage: run-job.sh [JOB] [OPTIONS]
#
#   JOB                   job to run (default: build)
#
#   -f, --force LIST      rebuild these components even on a cache hit;
#                         comma separated, or "all"
#   -m, --cache-mode MODE accepted for compatibility; strict and github both
#                         model immutable GitHub cache entries and validated
#                         workflow restores.
#   -c, --cache-dir DIR   cache root (default: <repo>/.local_install)
#       --nic NIC         the NIC label of the runner this job lands on, as in
#                         `runs-on: ${{ matrix.nic }}` (e810, e830, e825, e835,
#                         i225, i226). In host mode the label is resolved
#                         against the cards actually present; in a container it
#                         only names the simulation. Implied by the test jobs.
#       --runner KIND     force the runner kind: runner, baremetal or host
#   -n, --network MODE    docker network mode (default: host)
#   -l, --live            bind-mount the repository directly instead of
#                         building from a clean copy. Faster, but then the
#                         container and the host share build/ meson
#                         directories, which breaks when their toolchains
#                         differ.
#   -r, --rebuild-image   rebuild the runner image from scratch
#       --clean           discard the working copy and start from a fresh one
#   -s, --shell           drop into an interactive shell in the prepared
#                         runner instead of running the job
#   -k, --keep            do not remove the container when it exits
#   -h, --help            this help

set -euo pipefail

script_path="$(readlink -f "${BASH_SOURCE[0]}")"
CI_LOCAL_DIR="$(dirname "${script_path}")"
REPO_ROOT="$(cd "${CI_LOCAL_DIR}/../.." && pwd)"

# Components cached by the workflow, in build order.
COMPONENTS=(dpdk mtl jpegxs ffmpeg gstreamer plugins ice)

JOB="build"
CACHE_DIR="${REPO_ROOT}/.local_install"
CACHE_MODE="strict"
WORKDIR="/github/workspace"
NETWORK="host"
FORCE=""
NIC=""
RUNNER_KIND=""
SHELL_MODE=0
KEEP=0
LIVE=0
REBUILD_IMAGE=0
CLEAN=0

show_help() {
	# The comment block at the top of this file, up to the first blank line, is
	# the help text -- so editing one is editing the other.
	sed -n '5,/^$/p' "${script_path}" | sed 's/^# \{0,1\}//'
}

die() {
	echo "error: $*" >&2
	exit 1
}

while [ $# -gt 0 ]; do
	case "$1" in
	-f | --force)
		FORCE="$2"
		shift 2
		;;
	-c | --cache-dir)
		CACHE_DIR="$2"
		shift 2
		;;
	-m | --cache-mode)
		CACHE_MODE="$2"
		shift 2
		;;
	-n | --network)
		NETWORK="$2"
		shift 2
		;;
	--nic)
		NIC="$2"
		shift 2
		;;
	--runner)
		RUNNER_KIND="$2"
		shift 2
		;;
	-l | --live)
		LIVE=1
		shift
		;;
	-r | --rebuild-image)
		REBUILD_IMAGE=1
		shift
		;;
	--clean)
		CLEAN=1
		shift
		;;
	-s | --shell)
		SHELL_MODE=1
		shift
		;;
	-k | --keep)
		KEEP=1
		shift
		;;
	-h | --help)
		show_help
		exit 0
		;;
	-*)
		die "unknown option: $1 (try --help)"
		;;
	*)
		JOB="$1"
		shift
		;;
	esac
done

JOB_SCRIPT="${CI_LOCAL_DIR}/jobs/${JOB}.sh"
[ -f "${JOB_SCRIPT}" ] || die "no such job: ${JOB} (see .github/ci-local/jobs/)"
case "${CACHE_MODE}" in
strict | github) ;;
*) die "unknown cache mode: ${CACHE_MODE} (strict or github)" ;;
esac

TASK_BIN=$(command -v task) || die "task is not installed"

STATE_DIR="${REPO_ROOT}/.ci-local"
LOG_DIR="${STATE_DIR}/logs"
OUT_DIR="${STATE_DIR}/out"
mkdir -p "${CACHE_DIR}" "${LOG_DIR}" "${OUT_DIR}"
CACHE_DIR="$(cd "${CACHE_DIR}" && pwd)"
CACHE_STORE="${CACHE_DIR}/.cache-store"
mkdir -p "${CACHE_STORE}"

# ── the runner: `runs-on:` ──────────────────────────────────────────────────
# Three kinds of runner, because the workflows use two roles and one of them
# cannot be contained. The `build` job produces artifacts on an e835 fleet
# runner; `runs-on: ${{ matrix.nic }}` consumes them and owns a NIC. Locally the
# build role does not need a NIC, so only the consumer role uses the bare-metal
# image and test tooling -- and a consumer job that actually moves packets needs
# the card, so it defaults to running on this machine.
if [ -z "${RUNNER_KIND}" ]; then
	case "${JOB}" in
	smoke-tests | gtest) RUNNER_KIND="host" ;;
	validate-host) RUNNER_KIND="baremetal" ;;
	*) if [ -n "${NIC}" ]; then RUNNER_KIND="baremetal"; else RUNNER_KIND="runner"; fi ;;
	esac
fi
case "${RUNNER_KIND}" in
runner | baremetal | host) ;;
*) die "unknown runner kind: ${RUNNER_KIND} (runner, baremetal or host)" ;;
esac

if [ "${RUNNER_KIND}" = "host" ]; then
	[ "${SHELL_MODE}" -eq 0 ] || die "--shell has no meaning in host mode: this is your shell"
else
	command -v docker >/dev/null 2>&1 || die "docker is not installed"
	docker info >/dev/null 2>&1 || die "cannot talk to the docker daemon"
fi

# The NIC label is a claim about hardware. On the host the job checks it against
# the cards that are present, in its own `pytest-setup -- pci` step, exactly as
# the workflow does -- so leave PCI_DEVICE unset and let that step set it. In a
# container there is no card to check it against, and PCI_DEVICE only names the
# simulation in the diagnostics, so the label's first candidate ID stands in.
PCI_DEVICE=""
if [ -n "${NIC}" ]; then
	nic_ids=$(bash "${REPO_ROOT}/.github/scripts/ci/pytest-setup.sh" nic-ids "${NIC}") ||
		die "unknown nic: ${NIC} (labels: nic_device_ids in .github/scripts/ci/pytest-setup.sh)"
	if [ "${RUNNER_KIND}" != "host" ]; then
		PCI_DEVICE="8086:${nic_ids%% *},8086:${nic_ids%% *}"
	fi
fi

BASE_IMAGE="mtl-ci-local:24.04-$(id -u)"
IMAGE="${BASE_IMAGE}"
[ "${RUNNER_KIND}" = "baremetal" ] && IMAGE="mtl-ci-local:baremetal-$(id -u)"
[ "${RUNNER_KIND}" = "host" ] && IMAGE="none (this host: $(uname -n))"
# The self-hosted runner is a long-lived machine: whatever the build installs
# outside the workspace -- SVT-JPEG-XS into /usr/local, say -- is still there on
# the next run. A container is not, so /usr/local lives in a named volume.
# Docker seeds it from the image, so the tooling baked in at build time
# survives.
USR_LOCAL_VOLUME="mtl-ci-local-usrlocal-${RUNNER_KIND}-$(id -u)"
build_args=(
	--build-arg "RUNNER_UID=$(id -u)"
	--build-arg "RUNNER_GID=$(id -g)"
)
for proxy_var in http_proxy https_proxy no_proxy; do
	[ -n "${!proxy_var:-}" ] && build_args+=(--build-arg "${proxy_var}=${!proxy_var}")
done
[ "${REBUILD_IMAGE}" -eq 1 ] && build_args+=(--no-cache)

build_image() {
	# build_image <tag> <dockerfile> [extra build args...]
	local tag="$1" dockerfile="$2"
	shift 2
	if [ "${REBUILD_IMAGE}" -eq 1 ] || ! docker image inspect "${tag}" >/dev/null 2>&1; then
		echo "building runner image ${tag} ..."
		docker volume rm "${USR_LOCAL_VOLUME}" >/dev/null 2>&1 || true
		docker build "${build_args[@]}" "$@" -t "${tag}" -f "${dockerfile}" "${CI_LOCAL_DIR}" ||
			die "runner image build failed: ${tag}"
	else
		# Cheap: rebuilds only the layers whose inputs changed.
		docker build "${build_args[@]}" "$@" -t "${tag}" -f "${dockerfile}" "${CI_LOCAL_DIR}" \
			>/dev/null || die "runner image build failed: ${tag}"
	fi
}

if [ "${RUNNER_KIND}" != "host" ]; then
	build_image "${BASE_IMAGE}" "${CI_LOCAL_DIR}/Dockerfile"
	if [ "${RUNNER_KIND}" = "baremetal" ]; then
		build_image "${IMAGE}" "${CI_LOCAL_DIR}/Dockerfile.baremetal" \
			--build-arg "BASE_IMAGE=${BASE_IMAGE}"
	fi
	docker volume create "${USR_LOCAL_VOLUME}" >/dev/null
fi

# ── the checkout: `actions/checkout` ────────────────────────────────────────
# CI builds a clean checkout. Do the same: a copy that carries the current
# working tree, including uncommitted changes, but none of the host's meson
# build directories, whose cached compiler and source paths do not survive a
# different distribution inside the container.
if [ "${LIVE}" -eq 1 ] || [ "${RUNNER_KIND}" = "host" ]; then
	# In host mode this checkout *is* the runner's workspace, the way the fleet
	# runner's own checkout is: the tests bind its .local_install and read its
	# configs, so copying it somewhere else would only test the copy.
	SRC_DIR="${REPO_ROOT}"
else
	command -v rsync >/dev/null 2>&1 || die "rsync is required (or pass --live)"
	command -v git >/dev/null 2>&1 || die "git is required (or pass --live)"
	SRC_DIR="${STATE_DIR}/src"
	[ "${CLEAN}" -eq 1 ] && rm -rf "${SRC_DIR}"
	mkdir -p "${SRC_DIR}"
	echo "syncing working tree into ${SRC_DIR#"${REPO_ROOT}"/} ..."
	# Ask git what a checkout holds -- tracked files plus new ones, and none of
	# the host's build directories, which record absolute host paths meson
	# cannot relocate. Copying by list rather than by tree also leaves what the
	# job itself downloads into the workspace (SVT-JPEG-XS, the DPDK tarball,
	# FFmpeg) untouched between runs, which is what makes a second run cheap.
	# Use --clean when a stale copy needs to go.
	file_list="$(mktemp)"
	{
		git -C "${REPO_ROOT}" ls-files -z
		git -C "${REPO_ROOT}" ls-files -z --others --exclude-standard
	} >"${file_list}"
	rsync -a --from0 --files-from="${file_list}" "${REPO_ROOT}/" "${SRC_DIR}/"
	rm -f "${file_list}"
	# Some build steps ask git for the version.
	rsync -a --delete "${REPO_ROOT}/.git/" "${SRC_DIR}/.git/"
fi

# ── the cache keys: the `checksums` job ─────────────────────────────────────
declare -A HASH
hash_out="$(mktemp)"
"${REPO_ROOT}/script/hash_sources.sh" -o "${hash_out}" >/dev/null
while IFS='=' read -r key value; do
	[ -n "${key}" ] && HASH["${key}"]="${value}"
done <"${hash_out}"
rm -f "${hash_out}"

for comp in "${COMPONENTS[@]}"; do
	[ -n "${HASH[${comp}]:-}" ] || die "hash_sources.sh produced no '${comp}' checksum"
done

declare -A CACHE_KEY
key_out="$(mktemp)"
key_env=("GITHUB_OUTPUT=${key_out}")
for comp in "${COMPONENTS[@]}"; do
	key_env+=("HASH_${comp^^}=${HASH[${comp}]}")
done
env "${key_env[@]}" bash "${REPO_ROOT}/.github/scripts/ci/cache-keys.sh"
while IFS='=' read -r key value; do
	[[ "$key" == *_key ]] && CACHE_KEY["${key%_key}"]="$value"
done <"$key_out"
rm -f "$key_out"

# ── the restore: `actions/cache` ────────────────────────────────────────────
# A component is a hit only when the exact immutable entry restores and the
# workflow's structural validation accepts it.
tree_is_usable() {
	LOCAL_INSTALL_ROOT="$CACHE_DIR" bash "${REPO_ROOT}/.github/scripts/ci/validate-cache.sh" "$1" >/dev/null 2>&1
}

is_forced() {
	[ "${FORCE}" = "all" ] && return 0
	[[ ",${FORCE}," == *",$1,"* ]]
}

declare -A MISS
cache_summary=""
for comp in "${COMPONENTS[@]}"; do
	state="MISS"
	if is_forced "${comp}"; then
		state="FORCED"
	elif [ "${RUNNER_KIND}" = "host" ]; then
		# Host mode does not restore anything: the tree already installed here is
		# what the build job's artifact restores to on a fleet runner, and it is
		# what the tests are about to bind. So it is validated where it is, and a
		# component that does not pass counts as the miss it effectively is.
		if tree_is_usable "${comp}"; then state="HIT"; else state="UNUSABLE"; fi
	elif bash "${CI_LOCAL_DIR}/local-cache.sh" restore "$CACHE_STORE" "$comp" \
		"${CACHE_KEY[${comp}]}" "${CACHE_DIR}/${comp}"; then
		if tree_is_usable "${comp}"; then
			state="HIT"
		else
			state="POISONED"
		fi
	fi
	[ "${state}" = "HIT" ] && MISS["${comp}"]=0 || MISS["${comp}"]=1
	cache_summary+="${comp}=${state} "
done

echo "cache: ${cache_summary}"

# On a miss, actions/cache leaves the path empty. Match that: a tree left over
# from an earlier build is not what CI would hand the job, and its contents --
# stale symlinks escaping the prefix, half-installed files -- produce failures
# that exist only on this machine. Not in host mode, where nothing was restored
# and the trees are this machine's only copy: deleting one would turn a stale
# checksum into an unbuildable host, which is not a failure CI can have.
if [ "${RUNNER_KIND}" != "host" ]; then
	for comp in "${COMPONENTS[@]}"; do
		if [ "${MISS[${comp}]}" = "1" ]; then
			rm -rf "${CACHE_DIR:?}/${comp}"
		fi
	done
fi

# ── the job ─────────────────────────────────────────────────────────────────
LOG_FILE="${LOG_DIR}/${JOB}${NIC:+-${NIC}}-$(date -u +%Y%m%dT%H%M%SZ).log"
CONTAINER_NAME="mtl-ci-local-${JOB}${NIC:+-${NIC}}-$$"

# What the job script is told about the runner it was handed. The same set in
# both modes, so a job script never has to know which one it is in -- only
# CI_LOCAL_RUNNER says, for the steps that genuinely cannot be contained.
[ "${RUNNER_KIND}" = "host" ] && WORKDIR="${SRC_DIR}"
job_env=(
	"CI_LOCAL_WORKDIR=${WORKDIR}"
	"CI_LOCAL_OUT=${OUT_DIR}"
	"CI_LOCAL_SHELL=${SHELL_MODE}"
	"CI_LOCAL_NIC=${NIC}"
	"CI_LOCAL_RUNNER=${RUNNER_KIND}"
	"PCI_DEVICE=${PCI_DEVICE}"
)
for comp in "${COMPONENTS[@]}"; do
	upper="${comp^^}"
	job_env+=("CI_LOCAL_MISS_${upper}=${MISS[${comp}]}")
	job_env+=("CI_LOCAL_HASH_${upper}=${HASH[${comp}]}")
done
for proxy_var in http_proxy https_proxy no_proxy HTTP_PROXY HTTPS_PROXY NO_PROXY; do
	[ -n "${!proxy_var:-}" ] && job_env+=("${proxy_var}=${!proxy_var}")
done

if [ "${RUNNER_KIND}" != "host" ]; then
	docker_args=(
		--name "${CONTAINER_NAME}"
		--network "${NETWORK}"
		--workdir "${WORKDIR}"
		--volume "${SRC_DIR}:${WORKDIR}"
		--volume "${CACHE_DIR}:${WORKDIR}/.local_install"
		--volume "${TASK_BIN}:/usr/local/bin/task:ro"
		--volume "/lib/modules:/lib/modules:ro"
		--volume "/usr/src:/usr/src:ro"
		--volume "${USR_LOCAL_VOLUME}:/usr/local"
		--volume "${OUT_DIR}:/github/out"
	)
	# Inside the container the collected output is a mount, not this path.
	for entry in "${job_env[@]}"; do
		case "${entry}" in
		CI_LOCAL_OUT=*) docker_args+=(--env "CI_LOCAL_OUT=/github/out") ;;
		*) docker_args+=(--env "${entry}") ;;
		esac
	done

	[ "${KEEP}" -eq 1 ] || docker_args+=(--rm)
	if [ "${SHELL_MODE}" -eq 1 ]; then
		docker_args+=(--interactive --tty)
	elif [ -t 1 ]; then
		docker_args+=(--tty)
	fi
fi

started="$(date +%s)"
rc=0
if [ "${RUNNER_KIND}" = "host" ]; then
	echo "running ${JOB} on this host (no container) ..."
	set -o pipefail
	env "${job_env[@]}" bash "${JOB_SCRIPT}" 2>&1 | tee "${LOG_FILE}" || rc=$?
	set +o pipefail
elif [ "${SHELL_MODE}" -eq 1 ]; then
	docker run "${docker_args[@]}" "${IMAGE}" \
		bash "${WORKDIR}/.github/ci-local/jobs/${JOB}.sh" || rc=$?
else
	set -o pipefail
	docker run "${docker_args[@]}" "${IMAGE}" \
		bash "${WORKDIR}/.github/ci-local/jobs/${JOB}.sh" 2>&1 |
		tee "${LOG_FILE}" || rc=$?
	set +o pipefail
fi
elapsed=$(($(date +%s) - started))

# ── the save: `actions/cache` post step ─────────────────────────────────────
# Host mode restored nothing, so it has nothing to promote to a cache entry.
if [ "${SHELL_MODE}" -eq 0 ] && [ "${RUNNER_KIND}" != "host" ]; then
	for comp in "${COMPONENTS[@]}"; do
		if [ "${rc}" -eq 0 ] && tree_is_usable "${comp}"; then
			bash "${CI_LOCAL_DIR}/local-cache.sh" save "$CACHE_STORE" "$comp" \
				"${CACHE_KEY[${comp}]}" "${CACHE_DIR}/${comp}"
		fi
	done
fi

echo
echo "=== ci-local ${JOB} summary ==="
echo "result: $([ "${rc}" -eq 0 ] && echo PASS || echo FAIL)"
echo "exit_code: ${rc}"
echo "image: ${IMAGE}"
echo "runner: ${RUNNER_KIND}${NIC:+ (nic ${NIC})}"
echo "workdir: ${WORKDIR}"
echo "source: ${SRC_DIR}"
echo "cache_dir: ${CACHE_DIR}"
echo "cache: ${cache_summary}"
echo "cache_mode: ${CACHE_MODE}"
echo "duration_s: ${elapsed}"
echo "log: ${LOG_FILE}"
echo "diagnostics: ${OUT_DIR}/diagnostics.txt"

exit "${rc}"
