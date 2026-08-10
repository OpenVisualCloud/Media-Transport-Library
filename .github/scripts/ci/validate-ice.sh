#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
# shellcheck disable=SC1091
. "${root_dir}/versions.env"
kernel_release=${ICE_KERNEL_RELEASE:-$(uname -r)}
architecture=${ICE_ARCH:-$(uname -m)}
bundle_root=${ICE_BUNDLE_ROOT:-"${root_dir}/.local_install/ice"}
artifact_dir="${bundle_root}/${kernel_release}/${architecture}"
module="${artifact_dir}/ice.ko"
metadata="${artifact_dir}/metadata.env"
nm_command=${ICE_NM:-nm}
modinfo_cmd=${ICE_MODINFO:-modinfo}

metadata_value() {
	sed -n "s/^$1=//p" "$metadata"
}

test -s "$module" || {
	echo "ICE module is missing for ${kernel_release}/${architecture}" >&2
	exit 1
}
test -s "$metadata" || {
	echo "ICE metadata is missing for ${kernel_release}/${architecture}" >&2
	exit 1
}

for field in schema source_hash ice_version ice_dmid kernel_release architecture \
	compiler_sha256 kernel_compiler_sha256 kernel_abi_sha256 vermagic module_sha256 signer sig_id; do
	grep -q "^${field}=" "$metadata" || {
		echo "ICE metadata field is missing: ${field}" >&2
		exit 1
	}
done

"$nm_command" "$module" | grep '[[:space:]]ice_vc_cfg_q_bw$' >/dev/null || {
	echo "ICE artifact is missing the Kahawai QoS capability" >&2
	exit 1
}
expected_compiler_sha256=${ICE_EXPECTED_COMPILER_SHA256:-$(${CC:-cc} --version | sed -n '1p' | sha256sum | cut -d' ' -f1)}
[ "$(metadata_value compiler_sha256)" = "$expected_compiler_sha256" ] || {
	echo "ICE compiler identity does not match this cache key" >&2
	exit 1
}

test "$(metadata_value schema)" = 2
test "$(metadata_value ice_version)" = "$ICE_VER"
test "$(metadata_value ice_dmid)" = "$ICE_DMID"
if [ -n "${ICE_EXPECTED_SOURCE_HASH:-}" ]; then
	expected_source_hash=$ICE_EXPECTED_SOURCE_HASH
else
	hash_output=$(mktemp)
	trap 'rm -f "$hash_output"' EXIT
	bash "${root_dir}/script/hash_sources.sh" -o "$hash_output" >/dev/null
	expected_source_hash=$(sed -n 's/^ice=//p' "$hash_output")
fi
test "$(metadata_value source_hash)" = "$expected_source_hash" || {
	echo "ICE source hash does not match this checkout" >&2
	exit 1
}
test "$(metadata_value kernel_release)" = "$kernel_release" || {
	echo "ICE kernel mismatch: artifact=$(metadata_value kernel_release) host=${kernel_release}" >&2
	exit 1
}
test "$(metadata_value architecture)" = "$architecture" || {
	echo "ICE architecture mismatch: artifact=$(metadata_value architecture) host=${architecture}" >&2
	exit 1
}
if [ -n "${ICE_EXPECTED_ABI_SHA256:-}" ]; then
	expected_abi_sha256=$ICE_EXPECTED_ABI_SHA256
else
	abi_output=$(mktemp)
	trap 'rm -f "${hash_output:-}" "$abi_output"' EXIT
	GITHUB_OUTPUT="$abi_output" bash "${root_dir}/.github/scripts/ci/ice-abi.sh"
	expected_abi_sha256=$(sed -n 's/^abi_sha256=//p' "$abi_output")
fi
test "$(metadata_value kernel_abi_sha256)" = "$expected_abi_sha256" || {
	echo "ICE kernel header/config ABI fingerprint mismatch" >&2
	exit 1
}
echo "$(metadata_value module_sha256)  ${module}" | sha256sum --check --status || {
	echo "ICE module SHA-256 mismatch" >&2
	exit 1
}

actual_vermagic=$($modinfo_cmd -F vermagic "$module")
test "$actual_vermagic" = "$(metadata_value vermagic)" || {
	echo "ICE vermagic metadata mismatch" >&2
	exit 1
}
case "$actual_vermagic" in
"${kernel_release} "* | "${kernel_release}") ;;
*)
	echo "ICE vermagic is incompatible with ${kernel_release}: ${actual_vermagic}" >&2
	exit 1
	;;
esac

actual_signer=$($modinfo_cmd -F signer "$module")
actual_sig_id=$($modinfo_cmd -F sig_id "$module")
test "$actual_signer" = "$(metadata_value signer)" || {
	echo "ICE signer metadata mismatch" >&2
	exit 1
}
test "$actual_sig_id" = "$(metadata_value sig_id)" || {
	echo "ICE signature metadata mismatch" >&2
	exit 1
}

secure_boot_output=${ICE_SECURE_BOOT_STATE:-unknown}
mokutil_cmd=${ICE_MOKUTIL:-mokutil}
if [ "$secure_boot_output" = unknown ] && command -v "$mokutil_cmd" >/dev/null 2>&1; then
	if ! secure_boot_output=$($mokutil_cmd --sb-state 2>/dev/null); then
		secure_boot_output=unknown
	fi
fi
secure_boot=unknown
if [ "${secure_boot_output,,}" = enabled ] || grep -qi '^SecureBoot enabled$' <<<"$secure_boot_output"; then
	secure_boot=enabled
elif [ "${secure_boot_output,,}" = disabled ] || grep -qi '^SecureBoot disabled$' <<<"$secure_boot_output"; then
	secure_boot=disabled
fi
if [ "${ICE_REQUIRE_SECURE_BOOT_PROBE:-0}" = 1 ] && [ "$secure_boot" = unknown ]; then
	echo "Secure Boot state could not be determined" >&2
	exit 1
fi
if [ "$secure_boot" = enabled ] && [ -z "$actual_signer" ]; then
	echo "Secure Boot is enabled but the ICE module is unsigned" >&2
	exit 1
fi

echo "ICE artifact: valid (${kernel_release}/${architecture})"