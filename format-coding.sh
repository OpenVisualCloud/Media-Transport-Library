#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Apply every formatter and linter fix in the repository.
#
# This is the write-mode face of ./checkpatch.sh: both run the exact same hooks
# from .pre-commit-config.yaml, which is the single source of truth for which
# tool, which version and which arguments. Neither script contains a rule.
#
# The name is kept because a dozen documents, skills and agent prompts invoke it.
#
#   ./format-coding.sh           apply every fix
#   ./format-coding.sh --check   show what would change without changing it
#
# Tool versions used to be checked against whatever was installed locally, which
# meant a wrong version could silently reformat hundreds of unrelated files.
# pre-commit removes the whole failure mode: it installs the pinned version of
# every tool itself, so clang-format, shfmt, shellcheck and node no longer need
# to be on PATH at all.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
CHECKPATCH="$SCRIPT_DIR/checkpatch.sh"

# Before dispatching, not after: --check used to exec from the caller's directory,
# so running it by absolute path from another repository previewed and rolled back
# *that* repository.
cd "$SCRIPT_DIR"

case "${1:-}" in
--check)
	# Non-mutating preview, including the full diff of what would change.
	exec "$CHECKPATCH" --preview
	;;
-h | --help)
	cat <<'EOF'
Usage: ./format-coding.sh [--check]

  (none)    Apply every autofix, then report anything left for a human.
  --check   Show what would change and restore the tree. Needs a clean tree.

Verification, staged-only checks and hook installation live in ./checkpatch.sh.
Rules live in .pre-commit-config.yaml and .github/linters/.
EOF
	exit 0
	;;
"") ;;
*)
	echo "format-coding.sh: unknown option '$1' (try --help)" >&2
	exit 2
	;;
esac

# One pass, not two. The fixers write their corrections as they run and
# pre-commit already prints "files were modified by this hook" per hook, so a
# second whole-tree pass over ~1300 C/C++ files bought only a cosmetic
# "fixed it for you" vs "needs a human" distinction -- and that distinction was
# wrong anyway, since it read `git diff`, which is dominated by the user's own
# edits rather than the formatters'.
rc=0
"$CHECKPATCH" --all || rc=$?

echo ""
if [ "$rc" -eq 0 ]; then
	echo "Formatting applied where needed. Review with:  git diff --stat"
	exit 0
fi

echo "Findings above have no autofix -- fix them by hand, then re-run this script."
exit 1
