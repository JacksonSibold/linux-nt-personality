#!/usr/bin/env bash
# vm/lint.sh - syntax and lint checks for every tracked shell script.
#
# Step 1 (hard gate): `bash -n` on every tracked *.sh file.
# Step 2 (optional):  shellcheck, only when it is on PATH.  Findings are
#                     filtered by SHELLCHECK_SEVERITY (default: error) so the
#                     gate stays stable; use "warning" or "style" for more.
#
# Usage:
#   bash vm/lint.sh
#   SHELLCHECK_SEVERITY=warning bash vm/lint.sh
#
# Exit: 0 when every check passes, 1 otherwise.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SEVERITY="${SHELLCHECK_SEVERITY:-error}"

if git rev-parse --git-dir >/dev/null 2>&1; then
    mapfile -t FILES < <(git ls-files '*.sh')
else
    mapfile -t FILES < <(find . -name '*.sh' -not -path './.git/*' | sed 's|^\./||' | sort)
fi

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "lint: no shell scripts found under $ROOT" >&2
    exit 1
fi

fail=0

echo "=== lint: bash -n on ${#FILES[@]} scripts ==="
for f in "${FILES[@]}"; do
    if ! out="$(bash -n "$f" 2>&1)"; then
        echo "SYNTAX: $f" >&2
        echo "$out" >&2
        fail=1
    fi
done
if [[ $fail -eq 0 ]]; then
    echo "OK: bash -n passed for all scripts"
fi

if command -v shellcheck >/dev/null 2>&1; then
    echo "=== lint: shellcheck (severity >= $SEVERITY) ==="
    if ! shellcheck --severity="$SEVERITY" "${FILES[@]}"; then
        fail=1
    fi
else
    echo "lint: shellcheck not on PATH, skipping (bash -n checks still ran)"
fi

if [[ $fail -ne 0 ]]; then
    echo "=== lint: FAILED ===" >&2
    exit 1
fi
echo "=== lint: PASS ==="
