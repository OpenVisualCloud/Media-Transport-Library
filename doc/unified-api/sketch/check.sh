#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Doc test for the unified API sketch (doc/unified-api/sketch/README.md), revision 4.
# Every header and example must compile warning-free as C99 (-Wpadded, -pedantic for the
# headers) and C++17, with gcc and, when present, clang. Lints: size checks, the include
# layering, the naming rule of S8, and the examples copied verbatim into examples.md.
# Prints the function count per header and per call class.
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

mapfile -t headers < <(cd "$hdr" && find . -maxdepth 1 -name '*.h' -printf '%f\n' | sort)
variants=("" "-DMTL_LATER" "-D__bindgen")

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

# Every header together, in both orders and both languages: tag clashes between headers.
all="$(for h in "${headers[@]}"; do echo "#include <mtl/experimental/$h>"; done)"
run "gcc C99 all headers" sh -c "echo '$all' | gcc ${cflags[*]} -pedantic -x c -"
run "gcc C99 all headers reversed" sh -c "echo '$all' | tac | gcc ${cflags[*]} -pedantic -x c -"
run "g++ C++17 all headers" sh -c "echo '$all' | g++ ${cxxflags[*]} -pedantic -x c++ -"

# The legacy headers and the new ones in one file, as a gradual port needs: no tag or name
# clash. Runs when a configured build tree (build/mtl_build_config.h) is present.
legacy="$here/../../../include"
if [ -f "$here/../../../build/mtl_build_config.h" ]; then
	both="$(printf '#include <%s>\n' mtl_api.h st_pipeline_api.h st20_api.h st30_api.h st40_api.h st41_api.h st30_pipeline_api.h st40_pipeline_api.h)
$all"
	run "gcc legacy and new headers together" sh -c "echo '$both' | gcc -std=gnu11 -fsyntax-only -I '$inc' -I '$legacy' -I '$here/../../../build' -x c -"
fi

# MTL_SAME on two handle types must not compile cleanly (-Werror in C, an error in C++).
probe='#include <mtl/experimental/mtl.h>
int f(mtl_session_h s, mtl_instance_h m) { return MTL_SAME(s, m); }'
if echo "$probe" | gcc "${cflags[@]}" -x c - >/dev/null 2>&1; then
	echo "FAIL: MTL_SAME accepted two handle types (C)"
	fail=1
fi
if echo "$probe" | g++ "${cxxflags[@]}" -x c++ - >/dev/null 2>&1; then
	echo "FAIL: MTL_SAME accepted two handle types (C++)"
	fail=1
fi

for cxx in "${compilers_cxx[@]}"; do
	for h in "${headers[@]}"; do
		for v in "" "-DMTL_LATER"; do
			run "$cxx C++17 $h $v" sh -c "echo '#include <mtl/experimental/$h>' | $cxx ${cxxflags[*]} -Wpadded -pedantic $v -x c++ -"
		done
	done
	for f in "$here"/examples/*.cpp; do
		[ -e "$f" ] && run "$cxx C++17 $(basename "$f")" "$cxx" "${cxxflags[@]}" "$f"
	done
done

# Lint 1: every public struct has a size check.
for h in "${headers[@]}"; do
	while read -r name; do
		if ! grep -qh "MTL_SIZE_CHECK($name," "$hdr"/*.h; then
			echo "FAIL: struct $name ($h) has no MTL_SIZE_CHECK"
			fail=1
		fi
	done < <(grep -oE "^(typedef )?struct mtl_[a-z0-9_]+ \{" "$hdr/$h" | sed -E "s/^(typedef )?struct //; s/ \{//")
done

# Lint 2: layering. mtl.h includes only <stddef.h> and <stdint.h>; mtl_reasons.h includes
# nothing; every other header includes at least one sibling and nothing else.
if grep -E '^#include' "$hdr/mtl.h" | grep -vqE '<stddef.h>|<stdint.h>'; then
	echo "FAIL: mtl.h includes more than <stddef.h> and <stdint.h>"
	fail=1
fi
if grep -qE '^#include' "$hdr/mtl_reasons.h"; then
	echo "FAIL: mtl_reasons.h includes a header"
	fail=1
fi
for h in "${headers[@]}"; do
	case "$h" in mtl.h | mtl_reasons.h) continue ;; esac
	if ! grep -qE '^#include "mtl(_[a-z]+)?\.h"$' "$hdr/$h"; then
		echo "FAIL: $h includes no sibling mtl header"
		fail=1
	fi
	if grep -E '^#include' "$hdr/$h" | grep -vqE '^#include "mtl(_[a-z]+)?\.h"$'; then
		echo "FAIL: $h includes something other than a sibling mtl header"
		fail=1
	fi
done

# Lint 3: no identifier contains "passthrough" (packet mode is MTL_UNIT_PACKETS, the
# timestamp override MTL_SUBMIT_RTP_TS; S8 §4.1).
if cat "$hdr"/*.h "$here"/examples/* 2>/dev/null | grep -qiE '(_passthrough|passthrough_)'; then
	echo "FAIL: an identifier contains 'passthrough'"
	fail=1
fi

# Lint 4: examples.md shows every example verbatim (the header and files win).
doc="$here/../examples.md"
if [ -f "$doc" ] && command -v python3 >/dev/null 2>&1; then
	for f in "$here"/examples/*; do
		if ! python3 -c 'import sys; sys.exit(open(sys.argv[1]).read().rstrip("\n") not in open(sys.argv[2]).read())' "$f" "$doc"; then
			echo "FAIL: $(basename "$f") is not copied verbatim into examples.md"
			fail=1
		fi
	done
fi

# Function count: declarations start a line with a call-class macro; the MTL_LATER blocks
# are counted separately.
count() { # file, mode(api|later)
	awk -v mode="$2" '
    /^#if defined\(MTL_LATER\)/ {later = 1}
    later && /^#endif/ {later = 0}
    /^MTL_API_(CP|DP|DPC|WT|AS) / { if ((mode == "later") == (later == 1)) n++ }
    END {print n + 0}' "$1"
}
total=0
later=0
for h in "${headers[@]}"; do
	n="$(count "$hdr/$h" api)"
	total=$((total + n))
	later=$((later + $(count "$hdr/$h" later)))
	printf '%-16s %4d functions %5d lines\n' "$h" "$n" "$(wc -l <"$hdr/$h")"
done
printf '%-16s %4d functions (0.2 surface), %d reserved for later (MTL_LATER)\n' "total" "$total" "$later"
for c in CP DP DPC WT AS; do
	printf '  MTL_API_%-4s %4d\n' "$c" "$(cat "$hdr"/*.h | awk -v c="$c" '
    /^#if defined\(MTL_LATER\)/ {later = 1}
    later && /^#endif/ {later = 0}
    !later && $1 == "MTL_API_" c {n++} END {print n + 0}')"
done

if [ "$fail" -ne 0 ]; then
	echo "check.sh: FAILED ($ran compile runs)"
	exit 1
fi
echo "check.sh: OK ($ran compile runs, compilers: ${compilers_c[*]} ${compilers_cxx[*]})"
