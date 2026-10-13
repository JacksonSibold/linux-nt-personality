#!/usr/bin/env bash
# vm/lint.sh - syntax and lint checks for every tracked shell script.
#
# Step 1 (hard gate): `bash -n` on every tracked *.sh file.
# Step 2 (optional):  shellcheck, only when it is on PATH.  Findings are
#                     filtered by SHELLCHECK_SEVERITY (default: error) so the
#                     gate stays stable; use "warning" or "style" for more.
# Step 3 (hard gate when python3 is on PATH): tools/ntpers_kpatch.py --selftest.
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
    # Scripts that are known to be broken and deprecated (docs/vm/CLEANUP.md, section 1).
    # The linter cannot parse them, so they are exempt from it only; bash -n above still
    # checks them. Remove an entry when the file is deleted.
    SHELLCHECK_EXEMPT=("vm/m0_diag2.sh")
    SC_FILES=()
    for f in "${FILES[@]}"; do
        skip=0
        for x in "${SHELLCHECK_EXEMPT[@]}"; do
            [[ "$f" == "$x" ]] && skip=1
        done
        [[ $skip -eq 0 ]] && SC_FILES+=("$f")
    done
    echo "=== lint: shellcheck (severity >= $SEVERITY) on ${#SC_FILES[@]} scripts ==="
    if ! shellcheck --severity="$SEVERITY" "${SC_FILES[@]}"; then
        fail=1
    fi
else
    echo "lint: shellcheck not on PATH, skipping (bash -n checks still ran)"
fi

# Step 3 (hard gate when python3 is present): the kernel-tree edit tool's own
# primitive tests. The full unit suite covers the rest.
if command -v python3 >/dev/null 2>&1 && python3 -c 'import sys' >/dev/null 2>&1; then
    echo "=== lint: tools/ntpers_kpatch.py --selftest ==="
    if ! python3 "$ROOT/tools/ntpers_kpatch.py" --selftest; then
        fail=1
    fi
else
    echo "lint: no working python3 on PATH, skipping ntpers_kpatch selftest"
fi

if [[ $fail -ne 0 ]]; then
    echo "=== lint: FAILED ===" >&2
    exit 1
fi
echo "=== lint: PASS ==="
