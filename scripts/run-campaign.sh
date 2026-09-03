#!/usr/bin/env bash
# Run every row of a campaign file, then rebuild the cross-run summary.
#
# A campaign file is a plain text list of run.sh arguments, one run per line.
# Blank lines and lines starting with # are ignored. Example:
#
#   pinned --streams 20 --noisy-neighbor host-a --rdt-control mba-20
#
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLAN="${1:-}"
[[ -n "$PLAN" ]] || { echo "usage: $0 campaigns/<plan>.env" >&2; exit 2; }
[[ -f "$PLAN" ]] || { echo "FATAL: campaign plan not found: $PLAN" >&2; exit 2; }

row=0
failed=0
while read -r line <&3; do
  line="${line%%#*}"
  # shellcheck disable=SC2086
  set -- $line
  [[ $# -eq 0 ]] && continue
  row=$((row + 1))
  printf '\n=== campaign row %d: %s ===\n' "$row" "$*"
  # The runner itself uses ssh and kubectl; give them their own stdin so they
  # cannot swallow the remaining plan rows.
  if ! "$ROOT/scripts/run.sh" "$@" </dev/null; then
    failed=$((failed + 1))
    echo "WARNING: campaign row $row failed: $*" >&2
  fi
done 3< "$PLAN"

[[ "$row" -gt 0 ]] || { echo "FATAL: campaign contains no runnable rows: $PLAN" >&2; exit 2; }
printf '\n=== campaign complete: %d rows, %d failed ===\n' "$row" "$failed"
"$ROOT/scripts/summarize.sh"
[[ "$failed" -eq 0 ]]
