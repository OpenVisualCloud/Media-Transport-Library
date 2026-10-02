#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Doc test for the unified API sketch (doc/unified-api/sketch/README.md): every header and
# example must compile warning-free as C99 (-Wpadded included) and C++17, with gcc and, when
# present, clang. Also lints size checks and *_init coverage, and prints the function count.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
inc="$here/include"
hdr="$inc/mtl/experimental"
cflags=(-std=c99 -Wall -Wextra -Wpadded -Werror -fsyntax-only -I "$inc")
cxxflags=(-std=c++17 -Wall -Wextra -Werror -fsyntax-only -I "$inc")
fail=0
ran=0

run() { # label, command...
	local label="$1"
	shift
	ran=$((ran + 1))
	if ! out="$("$@" 2>&1)"; then
		echo "FAIL: $label"
		echo "$out" | head -40
		fail=1
	fi
}

compilers_c=(gcc)
compilers_cxx=(g++)
command -v clang >/dev/null 2>&1 && compilers_c+=(clang)
command -v clang++ >/dev/null 2>&1 && compilers_cxx+=(clang++)

headers=(mtl_unified.h mtl_simple.h mtl_debug.h)
variants=("" "-DMTL_UNIFIED_NO_INLINE" "-DMTL_UNIFIED_LATER")

for cc in "${compilers_c[@]}"; do
	for h in "${headers[@]}"; do
		for v in "${variants[@]}"; do
			# shellcheck disable=SC2086
			run "$cc C99 $h $v" sh -c "echo '#include <mtl/experimental/$h>' | $cc ${cflags[*]} -pedantic $v -x c -"
		done
	done
	for f in "$here"/examples/*.c; do
		run "$cc C99 $(basename "$f")" "$cc" "${cflags[@]}" "$f"
	done
done

for cxx in "${compilers_cxx[@]}"; do
	for h in "${headers[@]}"; do
		for v in "${variants[@]}"; do
			run "$cxx C++17 $h $v" sh -c "echo '#include <mtl/experimental/$h>' | $cxx ${cxxflags[*]} -Wpadded $v -x c++ -"
		done
	done
	run "$cxx C++17 examples_cpp.cpp" "$cxx" "${cxxflags[@]}" "$here/examples/examples_cpp.cpp"
done

# Lint 1: every public struct and union has a size check.
for h in "${headers[@]}"; do
	for kind in struct union; do
		macro=MTL_SIZE_CHECK
		[ "$kind" = union ] && macro=MTL_USIZE_CHECK
		while read -r name; do
			if ! grep -qh "$macro($name," "$hdr"/*.h; then
				echo "FAIL: $kind $name has no $macro"
				fail=1
			fi
		done < <(grep -oE "^(typedef )?$kind mtl_[a-z0-9_]+ \{" "$hdr/$h" | sed -E "s/^(typedef )?$kind //; s/ \{//")
	done
done

# Lint 2: every struct that starts with struct_size has an exported *_init().
for h in "${headers[@]}"; do
	while read -r name; do
		base="${name#mtl_}"
		if ! grep -qE "MTL_API_[A-Z]+ void mtl_${base}_init\(" "$hdr"/*.h; then
			echo "FAIL: struct $name has struct_size but no mtl_${base}_init()"
			fail=1
		fi
	done < <(awk '/^struct mtl_[a-z0-9_]+ \{/ {n=$2; getline; if ($0 ~ /uint32_t struct_size;/) print n}' "$hdr/$h")
done

# Lint 3: 10-api-sketch.md shows every example verbatim (the header and files win).
doc="$here/../10-api-sketch.md"
if [ -f "$doc" ] && command -v python3 >/dev/null 2>&1; then
	for f in "$here"/examples/*; do
		if ! python3 -c 'import sys; sys.exit(open(sys.argv[1]).read().rstrip("\n") not in open(sys.argv[2]).read())' "$f" "$doc"; then
			echo "FAIL: $(basename "$f") is not copied verbatim into 10-api-sketch.md"
			fail=1
		fi
	done
fi

# Function count: declarations start a line with a call-class macro. The MTL_UNIFIED_LATER
# block is counted separately; inline helpers count once (their exported twins).
count() { # file, mode(api|later)
	awk -v mode="$2" '
    /^#if defined\(MTL_UNIFIED_LATER\)/ {later = 1}
    later && /^#endif/ {later = 0}
    /^MTL_API_(CP|DP|DPC|WT|AS) / { if ((mode == "later") == (later == 1)) n++ }
    END {print n + 0}' "$1"
}
total=0
for h in "${headers[@]}"; do
	n="$(count "$hdr/$h" api)"
	total=$((total + n))
	printf '%-14s %4d exported functions\n' "$h" "$n"
done
printf '%-14s %4d exported functions (0.1 surface)\n' "total" "$total"
printf '%-14s %4d reserved for later (MTL_UNIFIED_LATER)\n' "" "$(count "$hdr/mtl_unified.h" later)"
for c in CP DP DPC WT AS; do
	printf '  MTL_API_%-4s %4d\n' "$c" "$(cat "$hdr"/*.h | awk -v c="$c" '
    /^#if defined\(MTL_UNIFIED_LATER\)/ {later = 1}
    later && /^#endif/ {later = 0}
    !later && $1 == "MTL_API_" c {n++} END {print n + 0}')"
done

if [ "$fail" -ne 0 ]; then
	echo "check.sh: FAILED ($ran compile runs)"
	exit 1
fi
echo "check.sh: OK ($ran compile runs, compilers: ${compilers_c[*]} ${compilers_cxx[*]})"
