#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Doc test for the unified API sketch (doc/unified-api/sketch/README.md).
# Every header and example must compile warning-free as C99 (-Wpadded, -pedantic for the
# headers) and C++17, with gcc and, when present, clang; each example to object code at the
# level its "Needs: MSn" names. Lints: size checks, the include layering, the packet-mode
# naming rule, the milestone of every exported function (comment tag = call-class
# argument), the availability probes, the examples copied verbatim into examples.md, the
# examples' calls of every exported function (G-114, nm -u), the example file names and
# numbers (once each, no gap) and each example's own number on its first line, the reasons
# table, one home per load-bearing number (numbers.txt), a
# migration.md §6.5 row for every legacy mode of the core, every section reference
# qualified by a document and every link and #anchor of the documents (check_refs.py), and
# that each still names the heading it named at the base revision (HEAD, or $CHECK_REFS_BASE)
# unless its line was edited since; one "no value" (no validity flag, INT64_MIN only in
# MTL_TIME_NONE, no sentinel of an example's own) and no retired name (retired.txt) outside
# decisions.md; no min_tx_delay_ns that adds the pick-up lead itself; and a "+ δ" after every
# live-sink "TAI(ct)".
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
# The legacy headers, for an example that includes <mtl_api.h> (ex24, from the legacy API)
# and for the mixed-header check below: the repository's include/ and the build tree's
# mtl_build_config.h, or without a build tree a stub with the values that header defines.
legacy="$here/../../../include"
cfg="$here/../../../build"
if [ ! -f "$cfg/mtl_build_config.h" ]; then
	cfg="$(mktemp -d)"
	printf '#pragma once\n#define MTL_VERSION_MAJOR 0\n#define MTL_VERSION_MINOR 0\n#define MTL_VERSION_LAST 0\n#define MTL_VERSION_EXTRA "DEV"\n#define MTL_COMPILER "check.sh"\n' >"$cfg/mtl_build_config.h"
fi
lflags=(-isystem "$legacy" -isystem "$cfg")
uses_legacy() { grep -q '^#include <mtl_api.h>' "$1"; }
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
		extra=()
		uses_legacy "$f" && extra=("${lflags[@]}")
		run "$cc C99 $(basename "$f") at MS$(needs "$f")" "$cc" "${eflags[@]}" "${extra[@]}" -DMTL_TARGET_LEVEL="$(needs "$f")" "$f"
	done
done

# Every header together, in both orders and both languages: tag clashes between headers.
all="$(for h in "${headers[@]}"; do echo "#include <mtl/experimental/$h>"; done)"
run "gcc C99 all headers" sh -c "echo '$all' | gcc ${cflags[*]} -pedantic -x c -"
run "gcc C99 all headers reversed" sh -c "echo '$all' | tac | gcc ${cflags[*]} -pedantic -x c -"
run "g++ C++17 all headers" sh -c "echo '$all' | g++ ${cxxflags[*]} -pedantic -x c++ -"

# The legacy headers and the new ones in one file, as a gradual port needs: no tag or name
# clash.
both="$(printf '#include <%s>\n' mtl_api.h st_pipeline_api.h st20_api.h st30_api.h st40_api.h st41_api.h st30_pipeline_api.h st40_pipeline_api.h)
$all"
run "gcc legacy and new headers together" sh -c "echo '$both' | gcc -std=gnu11 -fsyntax-only -I '$inc' -I '$legacy' -I '$cfg' -x c -"

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

# Lint 4: examples.md shows every example verbatim (the header and files win), and
# concepts.md its copies of ex01 (§3) and ex02 (§4).
doc="$here/../examples.md"
if [ -f "$doc" ] && command -v python3 >/dev/null 2>&1; then
	for f in "$here"/examples/*; do
		if ! python3 -c 'import sys; sys.exit(open(sys.argv[1]).read().rstrip("\n") not in open(sys.argv[2]).read())' "$f" "$doc"; then
			echo "FAIL: $(basename "$f") is not copied verbatim into examples.md"
			fail=1
		fi
	done
fi

doc2="$here/../concepts.md"
if [ -f "$doc2" ] && command -v python3 >/dev/null 2>&1; then
	for f in "$here"/examples/ex01_tx_video.c "$here"/examples/ex02_rx_video.c; do
		if ! python3 -c 'import sys; sys.exit(open(sys.argv[1]).read().rstrip("\n") not in open(sys.argv[2]).read())' "$f" "$doc2"; then
			echo "FAIL: $(basename "$f") is not copied verbatim into concepts.md"
			fail=1
		fi
	done
fi

# Lint 9: the examples call every exported function outside MTL_LATER and mtl_debug.h (the
# G-114 method on the examples): each example is compiled at its Needs level and the
# undefined symbols of its object (nm -u) are matched against --tags. A call through an
# inline wrapper counts, since a used wrapper is emitted and references the function. An
# example that does not compile, a missing nm and an empty object list fail the lint.
if ! command -v nm >/dev/null 2>&1; then
	echo "FAIL: nm not found: lint 9 (G-114) cannot run"
	fail=1
else
	objdir="$(mktemp -d)"
	nobj=0
	for f in "$here"/examples/*.c "$here"/examples/*.cpp; do
		[ -e "$f" ] || continue
		extra=()
		uses_legacy "$f" && extra=("${lflags[@]}")
		o="$objdir/$(basename "$f").o"
		case "$f" in
		*.cpp) cmd=(g++ -std=c++17 -c -o "$o" -I "$inc" -DMTL_TARGET_LEVEL="$(needs "$f")" "$f") ;;
		*) cmd=(gcc -std=c99 -c -o "$o" -I "$inc" "${extra[@]}" -DMTL_TARGET_LEVEL="$(needs "$f")" "$f") ;;
		esac
		if ! out="$("${cmd[@]}" 2>&1)"; then
			echo "FAIL: lint 9 could not compile $(basename "$f")"
			echo "$out" | head -20
			fail=1
			continue
		fi
		nobj=$((nobj + 1))
	done
	if [ "$nobj" -eq 0 ]; then
		echo "FAIL: lint 9 compiled no example"
		fail=1
	fi
	called="$(for o in "$objdir"/*.o; do [ -e "$o" ] && nm -u "$o" | awk '{print $NF}'; done | sort -u)"
	rm -rf "$objdir"
	while IFS=$'\t' read -r ok tag name file arg; do
		case "$tag" in "Phase 7" | later) continue ;; esac
		[ "$file" = ./mtl_debug.h ] && continue
		if ! printf '%s\n' "$called" | grep -qx "$name"; then
			echo "FAIL: $name ($file) is called by no example (G-114)"
			fail=1
		fi
	done <<<"$tags"
fi

# Lint 10: the examples directory holds ex_common.h, examples_cpp.cpp and exNN_<name>.c
# files only, numbered 01 to N once each with no gap; each example says its own number
# first ("/* exNN — ", the C++ file "// <name> — ").
nums=()
for f in "$here"/examples/*; do
	b="$(basename "$f")"
	case "$b" in
	ex_common.h) continue ;;
	*.cpp) want="// $b — " ;;
	ex[0-9][0-9]_[a-z0-9_]*.c)
		want="/* ${b%%_*} — "
		nums+=("${b:2:2}")
		;;
	*)
		echo "FAIL: examples/$b is not ex_common.h, a .cpp file or exNN_<name>.c"
		fail=1
		continue
		;;
	esac
	first="$(head -n 1 "$f")"
	if [ "${first#"$want"}" = "$first" ]; then
		echo "FAIL: $b does not start with '$want'"
		fail=1
	fi
done
dups="$(printf '%s\n' "${nums[@]}" | sort | uniq -d)"
if [ -n "$dups" ]; then
	echo "FAIL: example numbers used twice: $(printf '%s\n' "$dups" | tr '\n' ' ')"
	fail=1
fi
i=1
for n in $(printf '%s\n' "${nums[@]}" | sort -u); do
	if [ "$((10#$n))" -ne "$i" ]; then
		echo "FAIL: example numbers have a gap: ex$(printf '%02d' "$i") is missing"
		fail=1
		break
	fi
	i=$((i + 1))
done

# Lint 6 (until H1b's tables commit replaces it with gen_api_doc.py --check): the reasons of
# mtl_reasons.h, in ascending order, are exactly the rows of contract.md §8.3's marked table.
hdr_reasons="$(grep -oE 'MTL_REASON_[A-Z0-9_]+ = [0-9]+' "$hdr/mtl_reasons.h" | sed -E 's/MTL_REASON_([A-Z0-9_]+) = ([0-9]+)/\2 \1/')"
if ! printf '%s\n' "$hdr_reasons" | awk 'NR > 1 && $1 + 0 <= prev { print "FAIL: mtl_reasons.h: " $2 " out of ascending order"; bad = 1 } { prev = $1 + 0 } END { exit bad }'; then
	fail=1
fi
doc_reasons="$(awk '/<!-- BEGIN TABLE reasons/ { on = 1; next } /<!-- END TABLE reasons/ { on = 0 } on && /^\| [0-9]+ \| `[A-Z0-9_]+` \|/ { gsub(/`/, ""); print $2, $4 }' "$here/../contract.md")"
if [ "$(printf '%s\n' "$hdr_reasons" | sort)" != "$(printf '%s\n' "$doc_reasons" | sort)" ]; then
	echo "FAIL: mtl_reasons.h and contract.md §8.3 (the marked table) list different reasons:"
	diff <(printf '%s\n' "$hdr_reasons" | sort) <(printf '%s\n' "$doc_reasons" | sort) | head -20
	fail=1
fi

docs="$(cd "$here/.." && pwd)"
# Lint 7: one home per load-bearing number (README §1, numbers.txt).
while IFS=$'\t' read -r name home re; do
	case "$name" in '#'* | '') continue ;; esac
	hits="$(grep -rniE --include='*.md' -- "$re" "$docs" | sed "s|^$docs/||" | grep -v "^$home:")"
	if [ -n "$hits" ]; then
		echo "FAIL: '$name' is stated outside its home $home (say it in words and link the home):"
		echo "$hits" | cut -c1-160 | head -20
		fail=1
	fi
done <"$here/numbers.txt"

# Lint 8: every legacy mode of the core (ST_CORE_LEGACY_*) has its row in migration.md §6.5.
core="$here/../../../lib/src/st2110/core"
if [ -d "$core" ]; then
	for m in $(grep -rhoE 'ST_CORE_LEGACY_[A-Z0-9_]+' "$core" | sort -u); do
		if ! grep -q "\`$m\`" "$docs/migration.md"; then
			echo "FAIL: $m (lib/src/st2110/core/) has no row in migration.md §6.5"
			fail=1
		fi
	done
fi

# Lint 11: every section reference qualified by a document ("contract.md §5.5", "README §1",
# "[timing.md](timing.md) §6.1", lists and ranges after it) and every link and #anchor resolves
# to a file and a heading (check_refs.py; bare "§n" references are not checked). In a git work
# tree it also fails when a reference whose line was not edited since the base revision now
# names a heading of another title than at the base: a section renumbered or retitled under
# it. The base is HEAD, so uncommitted edits are checked against the last commit; for a
# branch set CHECK_REFS_BASE to its merge base (CHECK_REFS_BASE="$(git merge-base main HEAD)").
if command -v python3 >/dev/null 2>&1; then
	base="${CHECK_REFS_BASE:-}"
	if [ -z "$base" ] && git -C "$docs" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
		base=HEAD
	fi
	python3 -I "$here/check_refs.py" ${base:+--base "$base"} "$docs" || fail=1
else
	echo "FAIL: python3 not found: lint 11 cannot run"
	fail=1
fi

# Lint 12: one "no value" (D-215). No header defines a validity flag; INT64_MIN appears only in
# MTL_TIME_NONE's definition and its check; no example builds its own sentinel (a ternary that
# falls back to -1, 0 or INT64_MIN, or a time or index set to -1); no retired name
# (retired.txt) appears outside decisions.md, in the documents, the template, the sketch and
# the models.
if grep -nE '^#define MTL_[A-Z0-9_]+_VALID ' "$hdr"/*.h; then
	echo "FAIL: a validity flag is defined (D-215)"
	fail=1
fi
if grep -n 'INT64_MIN' "$hdr"/*.h | grep -vE ':#define MTL_TIME_NONE |mtl_sz_time_none'; then
	echo "FAIL: INT64_MIN outside MTL_TIME_NONE (D-215)"
	fail=1
fi
if grep -rnE '\?[^:;]*(media_index|_ns|index)[^:;]*: *\(?(-1|INT64_MIN|0)\)? *;|[a-z_]*(index|_ns|first|next) *= *-1 *;' "$here/examples"; then
	echo "FAIL: an example builds its own sentinel (D-215)"
	fail=1
fi
while IFS=$'\t' read -r name row; do
	case "$name" in '#'* | '') continue ;; esac
	hits="$(grep -rnF --include='*.md' --include='*.in' --include='*.h' --include='*.c' --include='*.cpp' --include='*.py' --include='*.sh' --include='*.txt' -- "$name" "$docs" | sed "s|^$docs/||" | grep -vE '^(decisions\.md|sketch/retired\.txt):')"
	if [ -n "$hits" ]; then
		echo "FAIL: '$name' was retired by $row:"
		echo "$hits" | cut -c1-160 | head -10
		fail=1
	fi
done <"$here/retired.txt"

# Lint 13: no text sets min_tx_delay_ns to a value that includes the pick-up lead (D-218: MTL
# adds the lead itself), and no example or header reads pickup_lead_ns to add it. Paragraphs
# are joined before matching.
if grep -rn 'pickup_lead_ns' "$here/examples" "$hdr"; then
	echo "FAIL: an example or header reads pickup_lead_ns; MTL adds the lead itself (D-218)"
	fail=1
fi
# Lint 14: every live-sink "TAI(ct)" in the documents, code blocks included, is followed by
# "+ δ" (D-221), also when the line wraps between them or a line holds two of them.
if command -v python3 >/dev/null 2>&1; then
	python3 -I - "$docs" <<'EOF' || fail=1
import os, re, sys
docs = sys.argv[1]
bad = 0
lead = r"(?:the |its |MTL's )?(?:pick-up )?lead\b"
pat = re.compile(
    r"(?:one|a) (?:frame|unit|field)(?: period)? (?:\+|plus) " + lead
    + r"|(?<![+] )(?<![+] `)min_tx_delay_ns`?\s+(?:=|to|is|the)\s[^.;]{0,80}?(?:\+|plus)\s+" + lead
    + r"|(?:\+|plus|,)\s*(?:the )?pick-up lead and a margin"
    + r"|a margin that covers the lead", re.I)
ct = re.compile(r"TAI\(ct\)(?!\s*\+\s*δ)")
for root, _, files in os.walk(docs):
    for f in files:
        if not f.endswith((".md", ".in")) or f == "decisions.md":
            continue
        p = os.path.join(root, f)
        raw = open(p, encoding="utf-8").read()
        text = re.sub(r"```.*?```", "", raw, flags=re.S)
        for para in re.split(r"\n\s*\n", text):
            flat = " ".join(para.split())
            for m in pat.finditer(flat):
                print(f"FAIL: {os.path.relpath(p, docs)}: lint 13, the lead added to the delay: '{m.group(0)[:80]}'")
                bad = 1
        if f.endswith(".md"):
            for para in re.split(r"\n\s*\n", raw):
                flat = " ".join(para.split())
                for m in ct.finditer(flat):
                    print(f"FAIL: {os.path.relpath(p, docs)}: lint 14, a live-sink media time without + δ: '{flat[m.start():m.start() + 60]}'")
                    bad = 1
sys.exit(bad)
EOF
else
	echo "FAIL: python3 not found: lints 13 and 14 cannot run"
	fail=1
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
frozen_src="$(for h in "${headers[@]}"; do case "$h" in mtl_debug.h | mtl_legacy.h) ;; *) echo "#include <mtl/experimental/$h>" ;; esac; done)"
pp="$(echo "$frozen_src" | gcc -E -P -nostdinc -I "$fake" -I "$inc" -x c - | tr '\n' ' ')"
n_macros="$(echo "$frozen_src" | gcc -E -dM -nostdinc -I "$fake" -I "$inc" -x c - |
	awk '{ name = $2; sub(/\(.*/, "", name) } name ~ /^(MTL|mtl)_/ && name !~ /^MTL_EXPERIMENTAL_.*_H$/ && name !~ /_$/ {n++} END {print n + 0}')"
rm -rf "$fake"
n_enums="$(echo "$pp" | grep -oE 'enum [a-z0-9_]* ?\{[^}]*\}' | grep -oE 'MTL_[A-Z0-9_]+ *=' | sort -u | wc -l)"
n_structs="$(echo "$pp" | grep -oE 'struct mtl_[a-z0-9_]+ \{' | sort -u | wc -l)"
n_inline="$(echo "$pp" | grep -oE 'static inline [^(;{]*\(' | sed -E 's/.*[ *]([a-z0-9_]+) *\($/\1/' | sort -u | wc -l)"
n_exported="$(printf '%s\n' "$tags" | awk -F'\t' '$2 ~ /^MS/ && $4 !~ /mtl_(debug|legacy)\.h$/ {n++} END {print n + 0}')"
printf 'frozen names: %d (exported functions %d, inline functions %d, enum constants %d, macros %d, structs %d)\n' \
	"$((n_exported + n_inline + n_enums + n_macros + n_structs))" "$n_exported" "$n_inline" "$n_enums" "$n_macros" "$n_structs"

if [ "$fail" -ne 0 ]; then
	echo "check.sh: FAILED ($ran compile runs)"
	exit 1
fi
echo "check.sh: OK ($ran compile runs, compilers: ${compilers_c[*]} ${compilers_cxx[*]})"
