#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# checkpatch -- the one entry point for MTL's style and lint checks.
#
# Humans, the git hooks, and CI all run this script, and it runs nothing but the
# hooks declared in .pre-commit-config.yaml. That file is the single source of
# truth for which tool, which version, which arguments and which files; this
# script only decides *which files to feed it* and how to report. It must never
# grow a rule of its own -- see doc/coding_standard.md.
#
#   ./checkpatch.sh                  verify every tracked file
#   ./checkpatch.sh --staged         verify staged files only (what the hook runs)
#   ./checkpatch.sh --files a.c b.md verify specific files
#   ./checkpatch.sh --preview        show what would change, then restore
#   ./checkpatch.sh --install-hooks  activate the git hooks
#
# Verification runs the real fixers, so a failing run leaves their corrections in
# the working tree -- that is the point: `git diff` is the remediation. Use
# --preview when you need the blast radius without touching the tree.
#
# Portability: Linux, macOS and Windows (git-bash / MSYS2). No GNU-only
# constructs -- no mapfile, no nproc, no grep -oP, no readlink -f, no sed -i.

set -eu

PROG=${0##*/}

usage() {
	cat <<'EOF'
Usage: ./checkpatch.sh [MODE]

Modes:
  (none), --all        Verify every tracked file. What CI runs.
  --staged             Verify staged files only. What the git hook runs.
  --files FILE...      Verify the named files.
  --preview            Report what would change without keeping it. Requires a
                       clean tree; restores it afterwards.
  --install-hooks      Install the pre-commit and pre-merge-commit git hooks.
  --bootstrap          Install the pre-commit tool itself (pipx, else pip --user).
  -h, --help           This text.

Exit status: 0 clean, 1 findings, 2 usage or environment problem.

Rules live in .pre-commit-config.yaml and .github/linters/, never in this script.
EOF
}

die() {
	echo "$PROG: $*" >&2
	exit 2
}

# Resolve the pre-commit entry point. Prefer the executable; fall back to the
# module so a `pip install --user` that never made it onto PATH still works, and
# so Windows/MSYS2 layouts (python vs python3) are covered.
PRE_COMMIT=""
find_pre_commit() {
	if command -v pre-commit >/dev/null 2>&1; then
		PRE_COMMIT="pre-commit"
		return 0
	fi
	for py in python3 python py; do
		if command -v "$py" >/dev/null 2>&1 &&
			"$py" -m pre_commit --version >/dev/null 2>&1; then
			PRE_COMMIT="$py -m pre_commit"
			return 0
		fi
	done
	return 1
}

require_pre_commit() {
	find_pre_commit && return 0
	cat >&2 <<'EOF'
checkpatch.sh: pre-commit is not installed.

It is the engine for every check in this repository -- it installs and pins the
linters itself, so you do not need clang-format, shfmt, shellcheck or node on
PATH.

  ./checkpatch.sh --bootstrap        # tries pipx, then pip --user
EOF
	distro_hint >&2
	exit 2
}

# Most current distributions mark their system Python as externally managed
# (PEP 668), which makes `pip install --user` fail outright and needs their own
# package instead. The per-platform commands live in the document, not here: two
# copies of a table nobody diffs is how this repository got three lint systems.
distro_hint() {
	cat <<'EOF'

Or install your platform's package -- dnf, pacman, apt, zypper, brew, pipx all
carry it. Exact commands: doc/coding_standard.md section 6.1.
EOF
}

bootstrap() {
	# pipx first: it is the one route that works unchanged on a PEP 668 system,
	# and it keeps the tool out of the interpreter the distribution owns.
	if command -v pipx >/dev/null 2>&1; then
		echo "Installing pre-commit with pipx"
		if pipx install pre-commit && find_pre_commit; then
			echo "pre-commit is available: $($PRE_COMMIT --version)"
			return 0
		fi
	fi

	for py in python3 python py; do
		command -v "$py" >/dev/null 2>&1 || continue
		echo "Installing pre-commit with $py -m pip install --user pre-commit"
		if ! "$py" -m pip install --user pre-commit; then
			echo "$PROG: pip install failed." >&2
			distro_hint >&2
			exit 2
		fi
		find_pre_commit ||
			die "installed, but not importable -- check that your Python user base is on PATH"
		echo "pre-commit is available: $($PRE_COMMIT --version)"
		return 0
	done

	echo "$PROG: no python interpreter found." >&2
	distro_hint >&2
	exit 2
}

# Report and translate pre-commit's exit status. pre-commit already prints
# file:line diagnostics per tool; re-parsing them into a bespoke format would
# only add a layer that can lie. All this adds is the remediation line.
report() {
	rc=$1
	echo ""
	if [ "$rc" -eq 0 ]; then
		echo "checkpatch: clean."
		return 0
	fi
	# No "run ./format-coding.sh" line here: that script's failure path goes
	# through this function, so it would tell the user to run the script they
	# are already inside.
	cat <<'EOF'
checkpatch: FAILED.

Fixable problems have already been corrected in your working tree -- review with
`git diff` and stage them. Anything still reported above (shellcheck findings,
MD013 line lengths, textlint terminology) has no autofix and needs a human.

  ./checkpatch.sh --staged  re-verify just what you are about to commit
EOF
	return 1
}

run_pre_commit() {
	# Deliberately not `set -e`-fatal: a non-zero exit is a normal result here.
	set +e
	# shellcheck disable=SC2086  # PRE_COMMIT may be "python3 -m pre_commit"
	$PRE_COMMIT run "$@"
	rc=$?
	set -e
	return $rc
}

# --preview keeps the guarantee the old `format-coding.sh --check` provided: see
# the real blast radius of a rule or version change without a mutated tree.
# pre-commit's fixers have no dry-run mode, so the only honest implementation is
# to run them on a verified-clean tree and roll back. Refusing on a dirty tree is
# what makes the rollback safe -- there is nothing of the user's to lose.
preview() {
	if ! git diff --quiet || ! git diff --cached --quiet; then
		die "--preview needs a clean tree (it reverts what the fixers change).
    Commit or stash first, or run ./checkpatch.sh to verify in place."
	fi

	restore() { git checkout -- . 2>/dev/null || true; }
	# A signal handler that just returns would resume after the interrupted
	# command and then report "no file would be modified" -- a confident wrong
	# answer for the one mode whose whole job is measuring the blast radius. HUP
	# and QUIT matter too: the first run builds ~560 MB of tool environments, so
	# a dropped SSH session is a realistic way to lose the rollback.
	trap 'restore; exit 130' INT TERM HUP QUIT
	trap restore EXIT

	set +e
	$PRE_COMMIT run --all-files
	rc=$?
	set -e

	echo ""
	if git diff --quiet; then
		echo "checkpatch --preview: no file would be modified."
	else
		echo "=== files that would be modified ==="
		git --no-pager diff --stat
		echo ""
		echo "=== full diff ==="
		git --no-pager diff
	fi

	restore
	trap - EXIT INT TERM HUP QUIT
	echo ""
	echo "Working tree restored."
	[ "$rc" -eq 0 ] || echo "Unfixable findings were reported above."
	return $rc
}

main() {
	# Resolve the repository from this script's own location, not from the
	# caller's directory: invoked by absolute path from inside some other
	# repository, a CWD-derived root would lint that repository -- and
	# --preview would roll it back.
	script_dir=$(cd "$(dirname "$0")" && pwd)
	root=$(cd "$script_dir" && git rev-parse --show-toplevel 2>/dev/null) ||
		die "not inside a git repository"

	mode="all"
	files=()
	case "${1:-}" in
	"" | --all) ;;
	--staged) mode="staged" ;;
	--preview) mode="preview" ;;
	--install-hooks) mode="install" ;;
	--bootstrap) mode="bootstrap" ;;
	-h | --help)
		usage
		exit 0
		;;
	--files)
		mode="files"
		shift
		[ $# -gt 0 ] || die "--files needs at least one path"
		# Passed through verbatim. pre-commit makes each path absolute *before*
		# it chdirs to the repository root and relative again after, so relative
		# paths from a subdirectory already work; resolving them here as well
		# would break MSYS2, where an MSYS `pwd` is not a path native Python can
		# resolve.
		files=("$@")
		;;
	*) die "unknown option '$1' (try --help)" ;;
	esac

	# Only --files takes operands. Without this, `--staged lib/src/mt_sch.c`
	# silently checks everything staged and ignores the path the user named.
	if [ "$mode" != "files" ] && [ $# -gt 1 ]; then
		die "unexpected argument '$2' (only --files takes paths)"
	fi

	cd "$root"

	if [ "$mode" = "bootstrap" ]; then
		bootstrap
		exit 0
	fi
	require_pre_commit

	rc=0
	case "$mode" in
	install)
		# Hook types come from default_install_hook_types in the config, so
		# pre-commit and pre-merge-commit are both installed.
		$PRE_COMMIT install --install-hooks
		echo ""
		echo "Hooks installed. Bypass in an emergency with --no-verify:"
		echo "  git commit --no-verify / git merge --no-verify"
		echo "CI, not the hook, is the merge authority -- a bypassed commit still fails there."
		;;
	preview) preview ;;
	staged)
		run_pre_commit --show-diff-on-failure || rc=$?
		report "$rc"
		;;
	all)
		run_pre_commit --all-files --show-diff-on-failure || rc=$?
		report "$rc"
		;;
	files)
		run_pre_commit --show-diff-on-failure --files "${files[@]}" || rc=$?
		report "$rc"
		;;
	esac
}

main "$@"
