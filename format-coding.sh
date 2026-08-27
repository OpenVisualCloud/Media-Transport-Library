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
#   ./format-coding.sh                  apply every fix
#   ./format-coding.sh --all            the same thing, said explicitly
#   ./format-coding.sh --staged         fix staged files only
#   ./format-coding.sh --files a.c b.md fix specific files
#   ./format-coding.sh --check          show what would change without changing it
#   ./format-coding.sh --preview        the same thing, spelled checkpatch's way
#
# Tool versions used to be checked against whatever was installed locally, which
# meant a wrong version could silently reformat hundreds of unrelated files.
# pre-commit removes the whole failure mode: it installs the pinned version of
# every tool itself, so clang-format, shfmt, shellcheck and node no longer need
# to be on PATH at all.

set -eu

CHECKPATCH="$(cd "$(dirname "$0")" && pwd)/checkpatch.sh"

# The write path forwards only the modes named here, so a new checkpatch.sh mode
# cannot leak into it; --check and --preview exec the verify-only preview instead.
# The operand grammar stays over there, so there is no second copy of it to drift.
case "${1:-}" in
--check | --preview)
	# Non-mutating preview, including the full diff of what would change.
	shift
	exec "$CHECKPATCH" --preview "$@"
	;;
-h | --help)
	cat <<'EOF'
Usage: ./format-coding.sh [MODE]

Modes:
  (none), --all        Apply every autofix to every tracked file.
  --staged             Apply the autofixes to staged files only.
  --files FILE...      Apply the autofixes to the named files.
  --check, --preview   Report what would change without keeping it. Requires a
                       clean tree; restores it afterwards.
  -h, --help           This text.

Exit status: 0 clean, 1 findings, 2 usage or environment problem (from
./checkpatch.sh, or locally for an unknown mode), 130 if an interrupted
--preview rolls back.

Verification, hook installation and --bootstrap live in ./checkpatch.sh.
Rules live in .pre-commit-config.yaml and .github/linters/, never in this script.
EOF
	exit 0
	;;
"" | --all | --staged | --files) ;;
*)
	echo "format-coding.sh: no write mode for '$1' -- try --help" >&2
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
"$CHECKPATCH" "$@" || rc=$?
exit "$rc"
