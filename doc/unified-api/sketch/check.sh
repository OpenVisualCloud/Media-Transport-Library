#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Doc test for the unified API sketch (doc/unified-api/sketch/README.md).
# Every header and example must compile warning-free as C99 (-Wpadded, -pedantic for the
# headers) and C++17, with gcc and, when present, clang; each example to object code at the
# level its "Needs: MSn" names. Lints: size checks, the include layering, the packet-mode
# naming rule, the milestone of every exported function (comment tag = call-class
# argument), the availability probes, and the examples copied verbatim into examples.md.
# Prints the exported functions per header, per call class and per milestone, and the
# number of frozen names: the one place the documents take these counts from.
# check.sh --tags prints "name<TAB>milestone<TAB>header" per exported function and exits
# (the export check of libmtl reads it).
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
inc="$here/include"
hdr="$inc/mtl/experimental"
cflags=(-std=c99 -Wall -Wextra -Wpadded -Werror -fsyntax-only -I "$inc")
cxxflags=(-std=c++17 -Wall -Wextra -Werror -fsyntax-only -I "$inc")
fail=0
ran=0

# The milestone of every exported function, from its declaration MTL_API_<class>(n) and from
# the tag that ends the comment above it: (MS1) to (MS7), or (Phase 7) and (later) inside
# MTL_LATER, where n is LATER. A declaration takes the tag of the comment that ends closest
# above it, with only a typedef or its continuation between.
tags="$(cd "$hdr" && awk '
    FNR == 1 { depth = 0; later = 0; tag = "" }
    /^#if/ { depth++; if ($0 ~ /defined\(MTL_LATER\)/ && !later) later = depth }
    /^#endif/ { if (later == depth) later = 0; depth-- }
    match($0, /\((MS[1-7]|Phase 7|later)\) \*\/$/) { tag = substr($0, RSTART + 1, RLENGTH - 5); next }
    /^MTL_API_(CP|DP|DPC|WT|AS)\(/ {
      arg = $0; sub(/^MTL_API_[A-Z]+\(/, "", arg); sub(/\).*/, "", arg)
      name = $0; sub(/^MTL_API_[A-Z]+\([^)]*\) /, "", name); sub(/\(.*/, "", name); sub(/.*[ *]/, "", name)
      want = tag ~ /^MS/ ? substr(tag, 3) : (tag == "" ? "?" : "LATER")
      ok = tag != "" && ((later > 0) == (tag == "Phase 7" || tag == "later")) && arg == want
      printf "%s\t%s\t%s\t%s\t%s\n", (ok ? "ok" : "bad"), (tag == "" ? "none" : tag), name, FILENAME, arg
      tag = ""; next
    }
    /^(#define|#if|#endif|struct |enum |static inline|MTL_MUST_CHECK|MTL_SIZE_CHECK|\})/ || /^[ \t]*$/ { tag = "" }
  ' ./*.h)"
if [ "${1:-}" = --tags ]; then
	printf '%s\n' "$tags" | awk -F'\t' '{ sub(/^\.\//, "", $4); print $3 "\t" $5 "\t" $4 }'
	exit 0
fi

# An example compiles to object code at the milestone its "Needs: MSn" names, so a call to a
# function of a later milestone fails there (the error attribute acts at code generation).
eflags=(-std=c99 -Wall -Wextra -Wpadded -Werror -c -o /dev/null -I "$inc")
exxflags=(-std=c++17 -Wall -Wextra -Werror -c -o /dev/null -I "$inc")
needs() { # file: the n of its first "Needs: MSn"
	tr '\n' ' ' <"$1" | grep -oE 'Needs:[ /*]*MS[1-7]' | head -1 | grep -oE '[1-7]$'
}

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
		run "$cc C99 $(basename "$f") at MS$(needs "$f")" "$cc" "${eflags[@]}" -DMTL_TARGET_LEVEL="$(needs "$f")" "$f"
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
		[ -e "$f" ] && run "$cxx C++17 $(basename "$f") at MS$(needs "$f")" "$cxx" "${exxflags[@]}" -DMTL_TARGET_LEVEL="$(needs "$f")" "$f"
	done
done

# Availability (mtl.h, OI-64). Every header together compiles to object code at levels 0 and
# 7, so an inline wrapper never trips the error attribute by being included; a call to an
# MS3 function, directly or through its wrapper, fails at level 2 naming MS3 and compiles at
# level 3, at -O0 and -O2, in C and C++.
for v in 0 7; do
	for cc in "${compilers_c[@]}"; do
		run "$cc C99 all headers -c at level $v" sh -c "echo '$all' | $cc ${eflags[*]} -pedantic -DMTL_TARGET_LEVEL=$v -x c -"
	done
	for cxx in "${compilers_cxx[@]}"; do
		run "$cxx C++17 all headers -c at level $v" sh -c "echo '$all' | $cxx ${exxflags[*]} -pedantic -DMTL_TARGET_LEVEL=$v -x c++ -"
	done
done
probe_direct='#include <mtl/experimental/mtl_events.h>
int f(mtl_session_h s, struct mtl_event* e) { return mtl_read_events(MTL_OBJ_OF_SESSION(s), e, sizeof(*e), 1, 0); }'
probe_wrapper='#include <mtl/experimental/mtl_events.h>
int f(mtl_session_h s, struct mtl_event* e) { return mtl_session_read_events(s, e, 1, 0); }'
for cc in "${compilers_c[@]}" "${compilers_cxx[@]}"; do
	printf '#if defined(__has_attribute)\n#if __has_attribute(error)\nyes\n#endif\n#endif\n' | "$cc" -E -P -x c - | grep -q yes || continue
	case "$cc" in *++) fl=("${exxflags[@]}") lang=c++ ;; *) fl=("${eflags[@]}") lang=c ;; esac
	for p in "$probe_direct" "$probe_wrapper"; do
		for o in -O0 -O2; do
			ran=$((ran + 2))
			if out="$(echo "$p" | "$cc" "${fl[@]}" "$o" -DMTL_TARGET_LEVEL=2 -x "$lang" - 2>&1)" || ! echo "$out" | grep -q 'MS3 function'; then
				echo "FAIL: $cc $o compiled a call to an MS3 function at level 2, or did not name MS3"
				fail=1
			fi
			if ! echo "$p" | "$cc" "${fl[@]}" "$o" -DMTL_TARGET_LEVEL=3 -x "$lang" - >/dev/null 2>&1; then
				echo "FAIL: $cc $o rejected a call to an MS3 function at level 3"
				fail=1
			fi
		done
	done
done
# The in-tree level (MTL_LEVEL + 1) is a valid MTL_TARGET_LEVEL expression.
run "gcc C99 all headers -c at level (MTL_LEVEL+1)" sh -c "echo '$all' | gcc ${eflags[*]} -pedantic '-DMTL_TARGET_LEVEL=(MTL_LEVEL+1)' -x c -"

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
# nothing; every other header includes at least one sibling and nothing else, except that
# mtl_util.h (inline helpers only) may include <string.h> for memcpy.
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
	allowed='^#include "mtl(_[a-z]+)?\.h"$'
	[ "$h" = mtl_util.h ] && allowed="$allowed|^#include <string\.h>\$"
	if grep -E '^#include' "$hdr/$h" | grep -vqE "$allowed"; then
		echo "FAIL: $h includes something other than a sibling mtl header"
		fail=1
	fi
done

# Lint 3: no identifier contains "passthrough" (packet mode is MTL_UNIT_PACKETS, the
# timestamp override MTL_SUBMIT_RTP_TS).
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
    /^#if/ { depth++; if ($0 ~ /defined\(MTL_LATER\)/ && !later) later = depth }
    /^#endif/ { if (later == depth) later = 0; depth-- }
    /^MTL_API_(CP|DP|DPC|WT|AS)\(/ { if ((mode == "later") == (later > 0)) n++ }
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
    /^#if/ { depth++; if ($0 ~ /defined\(MTL_LATER\)/ && !later) later = depth }
    /^#endif/ { if (later == depth) later = 0; depth-- }
    !later && $1 ~ "^MTL_API_" c "\\(" {n++} END {print n + 0}')"
done

# Lint 5: every exported function has its milestone twice, as the tag that ends its comment
# and as the argument of its call-class macro, and they agree ($tags, above).
while IFS=$'\t' read -r ok tag name file arg; do
	if [ "$ok" != ok ]; then
		echo "FAIL: $name ($file) has milestone tag '$tag' and call-class argument '$arg'"
		fail=1
	fi
done <<<"$tags"
printf 'functions per milestone:'
for m in MS1 MS2 MS3 MS4 MS5 MS6 MS7 "Phase 7" later; do
	printf ' %s %d,' "$m" "$(printf '%s\n' "$tags" | awk -F'\t' -v m="$m" '$2 == m {n++} END {print n + 0}')"
done
printf ' (Phase 7 and later: MTL_LATER)\n'

# Frozen names: what the ABI freeze fixes, outside MTL_LATER and without mtl_debug.h (no ABI
# promise). The headers are preprocessed against empty C library headers, so only their
# own declarations remain, without comments and without the MTL_LATER blocks.
fake="$(mktemp -d)"
printf '#define UINTPTR_MAX 0xFFFFFFFFFFFFFFFFu\n' >"$fake/stdint.h"
: >"$fake/stddef.h"
: >"$fake/string.h"
frozen_src="$(for h in "${headers[@]}"; do [ "$h" = mtl_debug.h ] || echo "#include <mtl/experimental/$h>"; done)"
pp="$(echo "$frozen_src" | gcc -E -P -nostdinc -I "$fake" -I "$inc" -x c - | tr '\n' ' ')"
n_macros="$(echo "$frozen_src" | gcc -E -dM -nostdinc -I "$fake" -I "$inc" -x c - |
	awk '{ name = $2; sub(/\(.*/, "", name) } name ~ /^(MTL|mtl)_/ && name !~ /^MTL_EXPERIMENTAL_.*_H$/ && name !~ /_$/ {n++} END {print n + 0}')"
rm -rf "$fake"
n_enums="$(echo "$pp" | grep -oE 'enum [a-z0-9_]* ?\{[^}]*\}' | grep -oE 'MTL_[A-Z0-9_]+ *=' | sort -u | wc -l)"
n_structs="$(echo "$pp" | grep -oE 'struct mtl_[a-z0-9_]+ \{' | sort -u | wc -l)"
n_inline="$(echo "$pp" | grep -oE 'static inline [^(;{]*\(' | sed -E 's/.*[ *]([a-z0-9_]+) *\($/\1/' | sort -u | wc -l)"
n_exported="$(printf '%s\n' "$tags" | awk -F'\t' '$2 ~ /^MS/ {n++} END {print n + 0}')"
printf 'frozen names: %d (exported functions %d, inline functions %d, enum constants %d, macros %d, structs %d)\n' \
	"$((n_exported + n_inline + n_enums + n_macros + n_structs))" "$n_exported" "$n_inline" "$n_enums" "$n_macros" "$n_structs"

if [ "$fail" -ne 0 ]; then
	echo "check.sh: FAILED ($ran compile runs)"
	exit 1
fi
echo "check.sh: OK ($ran compile runs, compilers: ${compilers_c[*]} ${compilers_cxx[*]})"
