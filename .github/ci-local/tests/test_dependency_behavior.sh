#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
temporary_dir=$(mktemp -d)
trap 'rm -rf "$temporary_dir"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

value() {
	sed -n "s/^$1=//p" "$2"
}

test_cache_schema_migration() {
	first="${temporary_dir}/keys-v1"
	second="${temporary_dir}/keys-v2"
	key_env=(
		HASH_DPDK=dpdk HASH_MTL=mtl HASH_JPEGXS=jpegxs HASH_FFMPEG=ffmpeg
		HASH_GSTREAMER=gstreamer HASH_PLUGINS=plugins HASH_ICE=ice
		ICE_KERNEL_RELEASE=fixture-kernel ICE_ARCH=x86_64 ICE_ABI_SHA256=abi
		ICE_COMPILER_SHA256=ice-compiler JPEGXS_COMPILER_SHA256=jpeg-compiler
		CC=/missing/consumer-compiler
	)
	env "${key_env[@]}" CI_CACHE_SCHEMA=1 GITHUB_OUTPUT="$first" \
		bash "${root_dir}/.github/scripts/ci/cache-keys.sh"
	env "${key_env[@]}" CI_CACHE_SCHEMA=2 GITHUB_OUTPUT="$second" \
		bash "${root_dir}/.github/scripts/ci/cache-keys.sh"
	for component in dpdk mtl jpegxs ffmpeg gstreamer plugins ice; do
		[ "$(value "${component}_key" "$first")" != "$(value "${component}_key" "$second")" ] ||
			fail "${component} cache key ignored the schema"
	done
	grep -q 'ice_key=.*-abi-ice-compiler$' "$first" || fail "ICE cache key omitted ABI or compiler identity"
	grep -q 'jpegxs_key=.*-jpeg-compiler-jpegxs$' "$first" || fail "JPEG XS cache key omitted compiler identity"

	third="${temporary_dir}/keys-compiler"
	env "${key_env[@]}" ICE_COMPILER_SHA256=changed-ice JPEGXS_COMPILER_SHA256=changed-jpeg \
		CI_CACHE_SCHEMA=1 GITHUB_OUTPUT="$third" bash "${root_dir}/.github/scripts/ci/cache-keys.sh"
	[ "$(value ice_key "$first")" != "$(value ice_key "$third")" ] || fail "ICE cache key ignored compiler identity"
	[ "$(value jpegxs_key "$first")" != "$(value jpegxs_key "$third")" ] || fail "JPEG XS cache key ignored compiler identity"
}

test_shared_fleet_ice_key() {
	common_env=(
		HASH_DPDK=dpdk HASH_MTL=mtl HASH_JPEGXS=jpegxs HASH_FFMPEG=ffmpeg
		HASH_GSTREAMER=gstreamer HASH_PLUGINS=plugins HASH_ICE=ice
		ICE_KERNEL_RELEASE=fleet-kernel ICE_ARCH=x86_64 ICE_ABI_SHA256=fleet-abi
		ICE_COMPILER_SHA256=fleet-compiler JPEGXS_COMPILER_SHA256=fleet-compiler
		CC=/missing/consumer-compiler
	)
	expected=""
	for nic in e810 e830 e835; do
		output="${temporary_dir}/keys-${nic}"
		env "${common_env[@]}" CI_NIC="$nic" GITHUB_OUTPUT="$output" \
			bash "${root_dir}/.github/scripts/ci/cache-keys.sh"
		key=$(value ice_key "$output")
		if [ -z "$expected" ]; then
			expected=$key
		else
			[ "$key" = "$expected" ] || fail "ICE cache key varies by NIC model"
		fi
	done
}

test_kernel_abi_fingerprint() {
	kernel_build="${temporary_dir}/kernel-build"
	mkdir -p "$kernel_build/include/generated" "$kernel_build/include/config" \
		"$kernel_build/arch/x86/include/generated"
	printf 'CONFIG_MODVERSIONS=y\n' >"$kernel_build/.config"
	printf 'symbols\n' >"$kernel_build/Module.symvers"
	printf '#define LINUX_COMPILER "gcc-13"\n' >"$kernel_build/include/generated/compile.h"
	printf '#define CONFIG_TEST 1\n' >"$kernel_build/include/generated/autoconf.h"
	first="${temporary_dir}/abi-first"
	second="${temporary_dir}/abi-second"
	ICE_KERNEL_BUILD_DIR="$kernel_build" ICE_KERNEL_RELEASE=fixture ICE_ARCH=x86_64 \
		GITHUB_OUTPUT="$first" bash "${root_dir}/.github/scripts/ci/ice-abi.sh"
	printf '#define CONFIG_TEST 2\n' >"$kernel_build/include/generated/autoconf.h"
	ICE_KERNEL_BUILD_DIR="$kernel_build" ICE_KERNEL_RELEASE=fixture ICE_ARCH=x86_64 \
		GITHUB_OUTPUT="$second" bash "${root_dir}/.github/scripts/ci/ice-abi.sh"
	[ "$(value abi_sha256 "$first")" != "$(value abi_sha256 "$second")" ] ||
		fail "kernel ABI fingerprint ignored generated headers"
}

test_yaml_policy_checker() {
	policy_root="${temporary_dir}/policy/.github"
	mkdir -p "$policy_root/workflows/nested" "$policy_root/actions/example" "$policy_root/legacy"
	printf 'jobs: {}\n' >"$policy_root/workflows/clean.yml"
	printf 'steps:\n  - run: |\n      legacy\n' >"$policy_root/legacy/ignored.yml"
	CI_YAML_POLICY_ROOT="$policy_root" bash "${root_dir}/.github/scripts/ci/check-yaml-policy.sh" >/dev/null
	printf 'jobs:\n  nested:\n    steps:\n      - script: |\n          violation\n' >"$policy_root/workflows/nested/check.yaml"
	if CI_YAML_POLICY_ROOT="$policy_root" bash "${root_dir}/.github/scripts/ci/check-yaml-policy.sh" >/dev/null 2>&1; then
		fail "YAML policy checker accepted a nested .yaml inline program"
	fi
	printf 'jobs:\n  mutable:\n    steps:\n      - uses: actions/checkout@v4\n' >"$policy_root/workflows/nested/check.yaml"
	if CI_YAML_POLICY_ROOT="$policy_root" bash "${root_dir}/.github/scripts/ci/check-yaml-policy.sh" >/dev/null 2>&1; then
		fail "YAML policy checker accepted a mutable external action"
	fi
	printf 'jobs:\n  mutable:\n    steps:\n      - "uses" : "actions/checkout@v4"\n' >"$policy_root/workflows/nested/check.yaml"
	if CI_YAML_POLICY_ROOT="$policy_root" bash "${root_dir}/.github/scripts/ci/check-yaml-policy.sh" >/dev/null 2>&1; then
		fail "YAML policy checker accepted a quoted mutable action"
	fi
	printf 'jobs:\n  pinned:\n    steps:\n      - "uses" : "actions/checkout@11d5960a326750d5838078e36cf38b85af677262"\n' >"$policy_root/workflows/nested/check.yaml"
	CI_YAML_POLICY_ROOT="$policy_root" bash "${root_dir}/.github/scripts/ci/check-yaml-policy.sh" >/dev/null
}

test_immutable_cache_poison_migration() {
	store="${temporary_dir}/cache-store"
	source="${temporary_dir}/cache-source"
	restored="${temporary_dir}/cache-restored"
	mkdir -p "$source"
	printf 'poison\n' >"$source/payload"
	bash "${root_dir}/.github/ci-local/local-cache.sh" save "$store" jpegxs key-v1 "$source"
	printf 'valid\n' >"$source/payload"
	bash "${root_dir}/.github/ci-local/local-cache.sh" save "$store" jpegxs key-v1 "$source"
	bash "${root_dir}/.github/ci-local/local-cache.sh" restore "$store" jpegxs key-v1 "$restored"
	[ "$(cat "$restored/payload")" = poison ] || fail "immutable cache entry was overwritten"
	bash "${root_dir}/.github/ci-local/local-cache.sh" save "$store" jpegxs key-v2 "$source"
	bash "${root_dir}/.github/ci-local/local-cache.sh" restore "$store" jpegxs key-v2 "$restored"
	[ "$(cat "$restored/payload")" = valid ] || fail "schema migration did not move to a clean cache key"
}

test_invalid_exact_cache_hit_fails() {
	install_root="${temporary_dir}/invalid-cache"
	mkdir -p "$install_root/dpdk"
	github_env="${temporary_dir}/invalid-cache.env"
	github_output="${temporary_dir}/invalid-cache.output"
	if LOCAL_INSTALL_ROOT="$install_root" CACHE_HIT_DPDK=true \
		GITHUB_ENV="$github_env" GITHUB_OUTPUT="$github_output" \
		bash "${root_dir}/.github/scripts/ci/evaluate-caches.sh" \
		>"${temporary_dir}/invalid-cache.log" 2>&1; then
		fail "invalid exact cache hit was treated as rebuildable"
	fi
	grep -qi 'cache schema' "${temporary_dir}/invalid-cache.log" ||
		fail "invalid cache failure did not explain schema rotation"
}

test_pytest_report_combiner() {
	report_dir="${root_dir}/python-reports"
	output="${temporary_dir}/report.output"
	python="${temporary_dir}/report-python"
	rm -rf "$report_dir"
	mkdir -p "$report_dir"
	cat >"$report_dir/nightly-test-report-e810-st20p.html" <<'EOF'
<html><body><span class="passed">1 passed</span></body></html>
EOF
	cat >"$python" <<'EOF'
#!/usr/bin/env bash
while [ "$#" -gt 0 ]; do
	if [ "$1" = --output-html ]; then
		printf '<html>combined</html>\n' >"$2"
		exit 0
	fi
	shift
done
exit 1
EOF
	chmod +x "$python"
	REPORT_PYTHON="$python" GITHUB_OUTPUT="$output" \
		bash "${root_dir}/.github/scripts/ci/reports.sh" combine-pytest >/dev/null
	report_path=$(value report_path "$output")
	[ -f "$report_path" ] || fail "pytest report combiner did not produce its output path"
	rm -rf "$report_dir"
}

test_hash_waterfall() {
	fixture="${temporary_dir}/hash-repo"
	mkdir -p "$fixture/script" "$fixture/.github/scripts/ci" "$fixture/ecosystem/ffmpeg_plugin"
	cp -a "${root_dir}/script/." "$fixture/script/"
	cp "${root_dir}/versions.env" "$fixture/"
	cp "${root_dir}/.github/scripts/setup_environment.sh" "$fixture/.github/scripts/"
	cp -a "${root_dir}/.github/scripts/ci/." "$fixture/.github/scripts/ci/"
	cp -a "${root_dir}/ecosystem/ffmpeg_plugin/." "$fixture/ecosystem/ffmpeg_plugin/"

	before="${temporary_dir}/hash-before"
	after="${temporary_dir}/hash-after"
	(cd "$fixture" && bash script/hash_sources.sh -o "$before" >/dev/null)
	echo '# hash invalidation probe' >>"$fixture/.github/scripts/ci/validate-jpegxs.sh"
	(cd "$fixture" && bash script/hash_sources.sh -o "$after" >/dev/null)

	[ "$(value jpegxs "$before")" != "$(value jpegxs "$after")" ] || fail "JPEG hash ignored bridge/build logic"
	[ "$(value ffmpeg "$before")" != "$(value ffmpeg "$after")" ] || fail "FFmpeg hash ignored JPEG hash"
	[ "$(value ice "$before")" = "$(value ice "$after")" ] || fail "JPEG change unexpectedly altered ICE hash"
}

create_jpeg_fixture() {
	bundle=$1
	# shellcheck disable=SC1091
	. "${root_dir}/versions.env"
	mkdir -p "$bundle/lib/pkgconfig" "$bundle/lib/plugins"
	printf 'runtime\n' >"$bundle/lib/libSvtJpegxs.so.0"
	ln -s libSvtJpegxs.so.0 "$bundle/lib/libSvtJpegxs.so"
	printf 'plugin\n' >"$bundle/lib/plugins/libst_plugin_st22_svt_jpeg_xs.so"
	cat >"$bundle/lib/pkgconfig/SvtJpegxs.pc" <<'EOF'
prefix=${pcfiledir}/../..
libdir=${prefix}/lib
Name: SvtJpegxs
Description: fixture
Version: 0.10.0
Libs: -L${libdir} -lSvtJpegxs
EOF
	compiler_sha256=$(bash "${root_dir}/.github/scripts/ci/compiler-identity.sh" producer)
	source_hash=$(bash "${root_dir}/script/hash_sources.sh" | sed -n 's/^jpegxs=//p')
	printf 'schema=1\nsvt_jpeg_xs_revision=%s\narchitecture=%s\ncompiler_sha256=%s\nsource_hash=%s\n' \
		"$SVT_JPEG_XS_VER" "$(uname -m)" "$compiler_sha256" "$source_hash" >"$bundle/bundle.env"
	(cd "$bundle" && find . -type l -printf '%p=%l\n' | sort >symlinks.manifest)
	manifest="${bundle}.manifest"
	(cd "$bundle" && find . -type f ! -name manifest.sha256 -print0 | sort -z | xargs -0 sha256sum >"$manifest")
	mv "$manifest" "$bundle/manifest.sha256"
}

test_jpeg_validation() {
	bundle="${temporary_dir}/jpegxs"
	create_jpeg_fixture "$bundle"
	JPEGXS_ROOT="$bundle" CC=/missing/consumer-compiler \
		bash "${root_dir}/.github/scripts/ci/validate-jpegxs.sh" >/dev/null
	sed -i 's/^svt_jpeg_xs_revision=.*/svt_jpeg_xs_revision=stale/' "$bundle/bundle.env"
	stale_manifest="${bundle}.stale-manifest"
	(cd "$bundle" && find . -type f ! -name manifest.sha256 -print0 | sort -z | xargs -0 sha256sum >"$stale_manifest")
	mv "$stale_manifest" "$bundle/manifest.sha256"
	if JPEGXS_ROOT="$bundle" bash "${root_dir}/.github/scripts/ci/validate-jpegxs.sh" >/dev/null 2>&1; then
		fail "JPEG validator accepted a stale bundle revision"
	fi
	rm -rf "$bundle"
	create_jpeg_fixture "$bundle"
	if JPEGXS_ROOT="$bundle" JPEGXS_EXPECTED_COMPILER_SHA256=changed \
		bash "${root_dir}/.github/scripts/ci/validate-jpegxs.sh" >/dev/null 2>&1; then
		fail "JPEG validator accepted a different compiler identity"
	fi
	printf 'corrupt\n' >>"$bundle/lib/libSvtJpegxs.so.0"
	if JPEGXS_ROOT="$bundle" bash "${root_dir}/.github/scripts/ci/validate-jpegxs.sh" >/dev/null 2>&1; then
		fail "JPEG validator accepted a malformed manifest"
	fi

	rm -rf "$bundle"
	create_jpeg_fixture "$bundle"
	ln -s /usr/lib "$bundle/external"
	if LOCAL_INSTALL_ROOT="$temporary_dir" bash "${root_dir}/.github/scripts/ci/validate-cache.sh" jpegxs >/dev/null 2>&1; then
		fail "cache validator accepted an external symlink"
	fi
}

test_jpeg_source_revision() {
	source_root="${temporary_dir}/jpeg-source"
	old_path=$(bash "${root_dir}/.github/scripts/ci/jpegxs-source.sh" path "$source_root" old)
	new_path=$(bash "${root_dir}/.github/scripts/ci/jpegxs-source.sh" path "$source_root" new)
	[ "$old_path" != "$new_path" ] || fail "JPEG source path ignored the revision"
	mkdir -p "$old_path" "$new_path"
	: >"$old_path/CMakeLists.txt"
	: >"$new_path/CMakeLists.txt"
	printf 'old\n' >"$old_path/.mtl-revision"
	printf 'old\n' >"$new_path/.mtl-revision"
	if bash "${root_dir}/.github/scripts/ci/jpegxs-source.sh" validate "$new_path" new >/dev/null 2>&1; then
		fail "JPEG source validator accepted a stale revision stamp"
	fi
	printf 'new\n' >"$new_path/.mtl-revision"
	bash "${root_dir}/.github/scripts/ci/jpegxs-source.sh" validate "$new_path" new >/dev/null
}

create_ice_fixture() {
	bundle_root=$1
	kernel=$2
	arch=$3
	dir="${bundle_root}/${kernel}/${arch}"
	# shellcheck disable=SC1091
	. "${root_dir}/versions.env"
	mkdir -p "$dir"
	printf 'module fixture\n' >"$dir/ice.ko"
	printf 'iavf module fixture\n' >"$dir/iavf.ko"
	hash=$(sha256sum "$dir/ice.ko" | cut -d' ' -f1)
	iavf_hash=$(sha256sum "$dir/iavf.ko" | cut -d' ' -f1)
	producer_compiler_sha256=$(bash "${root_dir}/.github/scripts/ci/compiler-identity.sh" producer)
	cat >"$dir/metadata.env" <<EOF
schema=3
source_hash=source
ice_version=${ICE_VER}
ice_dmid=${ICE_DMID}
iavf_version=${IAVF_VER}
iavf_dmid=${IAVF_DMID}
kernel_release=${kernel}
architecture=${arch}
compiler_sha256=${producer_compiler_sha256}
kernel_compiler_sha256=kernel-compiler
kernel_abi_sha256=abi
vermagic=${kernel} SMP mod_unload
module_sha256=${hash}
signer=
sig_id=
iavf_vermagic=${kernel} SMP mod_unload
iavf_module_sha256=${iavf_hash}
iavf_signer=
iavf_sig_id=
EOF
}

create_modinfo_fixture() {
	command=$1
	cat >"$command" <<'EOF'
#!/usr/bin/env bash
case "$2" in
vermagic) echo "${FAKE_VERMAGIC}" ;;
signer) echo "${FAKE_SIGNER:-}" ;;
sig_id) echo "${FAKE_SIG_ID:-}" ;;
*)
	if [ "$1" = -n ] && [ "$2" = ice ]; then
		echo "${FAKE_MODULE_PATH}"
	else
		exit 2
	fi
	;;
esac
EOF
	chmod +x "$command"
}

create_nm_fixture() {
	command=$1
	cat >"$command" <<'EOF'
#!/usr/bin/env bash
printf '0000000000000000 t ice_vc_cfg_q_bw\n'
EOF
	chmod +x "$command"
}

test_ice_validation() {
	kernel='fixture-kernel'
	arch=x86_64
	bundle="${temporary_dir}/ice"
	modinfo="${temporary_dir}/modinfo"
	nm_command="${temporary_dir}/nm"
	create_ice_fixture "$bundle" "$kernel" "$arch"
	create_modinfo_fixture "$modinfo"
	create_nm_fixture "$nm_command"
	export FAKE_VERMAGIC="${kernel} SMP mod_unload"

	ice_env=(ICE_BUNDLE_ROOT="$bundle" ICE_KERNEL_RELEASE="$kernel" ICE_ARCH="$arch" ICE_MODINFO="$modinfo" ICE_NM="$nm_command" ICE_EXPECTED_SOURCE_HASH=source ICE_EXPECTED_ABI_SHA256=abi)
	env "${ice_env[@]}" CC=/missing/host-compiler bash "${root_dir}/.github/scripts/ci/validate-ice.sh" >/dev/null
	if env "${ice_env[@]}" ICE_EXPECTED_COMPILER_SHA256=changed \
		bash "${root_dir}/.github/scripts/ci/validate-ice.sh" >/dev/null 2>&1; then
		fail "ICE validator accepted a different compiler identity"
	fi
	printf '#!/usr/bin/env bash\nexit 0\n' >"$nm_command"
	if env "${ice_env[@]}" bash "${root_dir}/.github/scripts/ci/validate-ice.sh" >/dev/null 2>&1; then
		fail "ICE validator accepted a module without Kahawai QoS capability"
	fi
	create_nm_fixture "$nm_command"
	if env "${ice_env[@]}" ICE_EXPECTED_ABI_SHA256=changed bash "${root_dir}/.github/scripts/ci/validate-ice.sh" >/dev/null 2>&1; then
		fail "ICE validator accepted a kernel header/config ABI mismatch"
	fi
	if env "${ice_env[@]}" ICE_KERNEL_RELEASE=other bash "${root_dir}/.github/scripts/ci/validate-ice.sh" >/dev/null 2>&1; then
		fail "ICE validator accepted a fleet ABI mismatch"
	fi
	if env "${ice_env[@]}" ICE_SECURE_BOOT_STATE='SecureBoot enabled' bash "${root_dir}/.github/scripts/ci/validate-ice.sh" >/dev/null 2>&1; then
		fail "ICE validator accepted an unsigned module under Secure Boot"
	fi
	# The suite binds VFs, so a cache entry that carries ice.ko alone is unusable:
	# the host would load the packaged ice against the distribution iavf.
	mv "${bundle}/${kernel}/${arch}/iavf.ko" "${temporary_dir}/iavf.ko.held"
	if env "${ice_env[@]}" bash "${root_dir}/.github/scripts/ci/validate-ice.sh" >/dev/null 2>&1; then
		fail "ICE validator accepted a bundle without the matching IAVF module"
	fi
	mv "${temporary_dir}/iavf.ko.held" "${bundle}/${kernel}/${arch}/iavf.ko"
	mokutil="${temporary_dir}/mokutil"
	printf '#!/usr/bin/env bash\nexit 1\n' >"$mokutil"
	chmod +x "$mokutil"
	if env "${ice_env[@]}" ICE_MOKUTIL="$mokutil" ICE_REQUIRE_SECURE_BOOT_PROBE=1 \
		bash "${root_dir}/.github/scripts/ci/validate-ice.sh" >/dev/null 2>&1; then
		fail "ICE validator treated a failed Secure Boot probe as disabled"
	fi
}

# A host whose python3 has no ensurepip is what mtl-runner-12 was on the day the
# i225 leg first reached a runner: the leg died twelve seconds in, on the one step
# that cannot be worked around from inside a test. These cover the way out of it.
test_acceptance_venv_builders() {
	local stub_dir="${temporary_dir}/venv-stubs" venv="${temporary_dir}/venv-under-test"
	local real_python3 out
	real_python3=$(command -v python3)
	mkdir -p "$stub_dir"

	# A python3 that has everything except ensurepip, which is exactly what
	# `python3` without `python3-venv` is on Debian and Ubuntu.
	cat >"${stub_dir}/python3" <<-STUB
		#!/usr/bin/env bash
		if [ "\$*" = "-c import ensurepip" ]; then exit 1; fi
		exec "${real_python3}" "\$@"
	STUB
	chmod +x "${stub_dir}/python3"

	# With no ensurepip and no alternative, the job names the package and stops:
	# this is host state, and a test job does not install it.
	out=$(MTL_CI_VENV="$venv" PATH="${stub_dir}:/usr/bin:/bin" \
		bash "${root_dir}/.github/scripts/ci/pytest-setup.sh" ensure 2>&1) && true
	grep -q 'sudo apt-get install -y python3-venv' <<<"$out" ||
		fail "a host with no way to build a virtualenv was not told which package to install"
	[ ! -e "$venv" ] || fail "a virtualenv that could not be built left a directory behind"

	# A virtualenv that reports what it was asked to build, so the assertion is
	# about which builder the script chose and not about pip reaching the network.
	# It is put on PATH only now: with it there from the start, the case above
	# would have had an alternative and this test would have proved nothing.
	cat >"${stub_dir}/virtualenv" <<-'STUB'
		#!/usr/bin/env bash
		echo "virtualenv-stub $*" >"${STUB_LOG}"
		exit 3
	STUB
	chmod +x "${stub_dir}/virtualenv"

	# With virtualenv on PATH it is used instead, because it carries its own pip.
	out=$(STUB_LOG="${temporary_dir}/builder.log" MTL_CI_VENV="$venv" \
		PATH="${stub_dir}:/usr/bin:/bin" \
		bash "${root_dir}/.github/scripts/ci/pytest-setup.sh" ensure 2>&1) && true
	grep -q "Building it with: virtualenv --python python3" <<<"$out" ||
		fail "a host with virtualenv but no ensurepip did not fall back to it"
	grep -q "virtualenv-stub --python python3 ${venv}" "${temporary_dir}/builder.log" ||
		fail "the virtualenv fallback was not given the virtualenv path to build"
}

# The suite logs into the DUT over SSH even when the DUT is the runner itself, and
# paramiko reports a key it cannot use as the *password* being refused. So the
# preflight has to name which of the two it is, before a test runs.
test_self_login_preflight() {
	local script="${root_dir}/.github/scripts/ci/pytest-setup.sh" out
	local stub_dir="${temporary_dir}/ssh-stubs" key="${temporary_dir}/fake-key"

	out=$(MTL_CI_RUNNER_ENV=/dev/null RUNNER_SSH_KEY="${temporary_dir}/absent-key" \
		bash "$script" connection 2>&1) && fail "a host with no SSH key was called ready"
	grep -q "ssh-keygen -t ed25519" <<<"$out" ||
		fail "a host with no SSH key was not told how to create one"

	# A key that exists but is not authorised: the exit status is ssh's, and what
	# ssh said about it has to reach the log -- that line is the whole diagnosis.
	mkdir -p "$stub_dir"
	printf 'not really a key\n' >"$key"
	cat >"${stub_dir}/ssh" <<-'STUB'
		#!/usr/bin/env bash
		echo "test@127.0.0.1: Permission denied (publickey)." >&2
		exit 255
	STUB
	chmod +x "${stub_dir}/ssh"
	out=$(MTL_CI_RUNNER_ENV=/dev/null RUNNER_SSH_KEY="$key" \
		PATH="${stub_dir}:${PATH}" bash "$script" connection 2>&1) &&
		fail "a key the host rejects was called ready"
	grep -q 'Permission denied (publickey)' <<<"$out" ||
		fail "what ssh said about the rejected key was swallowed"
	# The advice has to be a command that runs on the host that prints it. These
	# hosts offer publickey only, so `ssh-copy-id` cannot log in to copy anything
	# -- the append does the same job and needs no login.
	grep -q "cat ${key}.pub >> .*/authorized_keys" <<<"$out" ||
		fail "a rejected key was not paired with the command that authorises it"
	if grep -q 'ssh-copy-id' <<<"$out"; then
		fail "the fix printed for a publickey-only host needs a login it cannot make"
	fi

	# And a host where it works says so, rather than saying nothing.
	printf '#!/usr/bin/env bash\nexit 0\n' >"${stub_dir}/ssh"
	out=$(MTL_CI_RUNNER_ENV=/dev/null RUNNER_SSH_KEY="$key" \
		PATH="${stub_dir}:${PATH}" bash "$script" connection 2>&1) ||
		fail "a host whose login works was reported as broken"
	grep -q 'works, so the framework can reach this host' <<<"$out" ||
		fail "a working login was not confirmed in the log"
}

# The compliance analyser is not deployed anywhere on the fleet yet. Absence of a
# verdict is a documented degraded mode; a check for it that fails the leg turns
# every capture leg red for a thing the leg does not need to transmit a frame.
test_compliance_check_is_not_a_gate() {
	local script="${root_dir}/.github/scripts/ci/ebu-list.sh" out
	local stub_dir="${temporary_dir}/capture-stubs" summary="${temporary_dir}/step-summary"
	mkdir -p "$stub_dir" "${temporary_dir}/no-tools"

	# `verify` judges two independent things: whether the host can capture a pcap
	# at all, and whether an analyser exists to judge it. Stub the capture half so
	# what follows is about the analyser half only. Without these the exit-status
	# assertion below reads as "absence of EBU_IP failed the leg" on any host
	# without netsniff-ng -- which is every hosted runner this suite itself runs
	# on, and is how this test passed on a NIC host and failed in CI.
	printf '#!/usr/bin/env bash\nexit 0\n' >"${stub_dir}/netsniff-ng"
	printf '#!/usr/bin/env bash\nexit 0\n' >"${stub_dir}/sudo"
	chmod +x "${stub_dir}/netsniff-ng" "${stub_dir}/sudo"
	local -a capture_host=(
		MTL_CI_RUNNER_ENV=/dev/null
		GITHUB_STEP_SUMMARY="$summary"
		PATH="${stub_dir}:${PATH}"
	)

	out=$(env "${capture_host[@]}" bash "$script" verify 2>&1) ||
		fail "an unset EBU_IP failed the leg instead of announcing a missing verdict"
	grep -q 'without a compliance verdict' <<<"$out" ||
		fail "an unset EBU_IP was not reported at all"
	grep -q 'No ST 2110 compliance verdict' "$summary" ||
		fail "the degraded mode was not recorded in the job summary"

	if env "${capture_host[@]}" MTL_CI_REQUIRE_COMPLIANCE=1 \
		bash "$script" verify >/dev/null 2>&1; then
		fail "a host that requires a compliance verdict accepted not having one"
	fi

	# Set but incomplete is misconfiguration rather than absence, and stays fatal:
	# the analyser would reject every upload, several minutes into a test.
	if env "${capture_host[@]}" EBU_IP=192.0.2.1 bash "$script" verify >/dev/null 2>&1; then
		fail "an EBU_IP with no credentials was accepted"
	fi

	# The capture half is a gate, whatever the analyser situation: a host that
	# cannot record a pcap produces nothing to judge, so it fails naming the
	# package. GITHUB_STEP_SUMMARY is dropped here because the empty PATH leaves
	# no `hostname` for the summary line to call, and the interpreter is named
	# absolutely because that PATH has no `bash` for env to resolve either.
	out=$(env -u GITHUB_STEP_SUMMARY MTL_CI_RUNNER_ENV=/dev/null \
		PATH="${temporary_dir}/no-tools" "$(command -v bash)" "$script" verify 2>&1) && true
	grep -q 'sudo apt-get install -y netsniff-ng' <<<"$out" ||
		fail "a host that cannot capture a pcap was not told which package to install"
}

test_cache_schema_migration
test_acceptance_venv_builders
test_self_login_preflight
test_compliance_check_is_not_a_gate
test_shared_fleet_ice_key
test_kernel_abi_fingerprint
test_yaml_policy_checker
test_immutable_cache_poison_migration
test_invalid_exact_cache_hit_fails
test_pytest_report_combiner
test_hash_waterfall
test_jpeg_validation
test_jpeg_source_revision
test_ice_validation
echo "dependency behavior tests: PASS"
