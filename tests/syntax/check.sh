#!/bin/sh
# check.sh - syntax-check the kernel-facing C sources against the userspace shims.
#
# For every src/kernel/nt/*.c, src/loader/*.c and src/kernel/fs/*.c file, runs
#   gcc -fsyntax-only -Wall -Wextra -std=c11 -DUSERSPACE_KSHIM \
#       -include tests/syntax/shim/kernel_compat.h -Itests/syntax/shim FILE
# and reports PASS/FAIL per file. Nothing is linked or executed.
#
# Usage: tests/syntax/check.sh            (from anywhere)
#        CC=/path/to/gcc tests/syntax/check.sh
#
# Exit status: 0 if every file passes, otherwise the number of failing files.
# Known failures are described in tests/syntax/README.md.

set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-gcc}
SHIM_DIR=tests/syntax/shim
FORCE_INC=tests/syntax/shim/kernel_compat.h

cd "$ROOT" || exit 1

pass=0
fail=0
warn_total=0
tmp=$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/check.$$")
trap 'rm -f "$tmp"' EXIT

for f in src/kernel/nt/*.c src/loader/*.c src/kernel/fs/*.c; do
    [ -f "$f" ] || continue
    if "$CC" -fsyntax-only -Wall -Wextra -std=c11 -DUSERSPACE_KSHIM \
            -include "$FORCE_INC" -I"$SHIM_DIR" "$f" >"$tmp" 2>&1; then
        status=PASS
        pass=$((pass + 1))
    else
        status=FAIL
        fail=$((fail + 1))
    fi
    warns=$(grep -c 'warning:' "$tmp" || true)
    warn_total=$((warn_total + warns))
    printf '%-4s  %-40s  warnings=%s\n' "$status" "$f" "$warns"
    if [ "$status" = FAIL ]; then
        grep -E 'error|fatal' "$tmp" | head -3 | sed 's/^/        /'
    fi
done

echo "---"
echo "passed=$pass failed=$fail warnings=$warn_total"
exit "$fail"
