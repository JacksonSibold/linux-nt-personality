#!/bin/sh
# static_analyze.sh - informational static analysis of the kernel-facing C sources.
#
# Scope: src/loader, src/kernel/nt, src/ntdll and arch/x86/nt (or the FILE
# arguments given on the command line). Each translation unit is checked
# against the host-shim profiles in order; the first profile under which
# -fsyntax-only succeeds is used. Units that no profile accepts are reported
# as UNBUILDABLE and skipped.
#
# For each buildable unit it runs
#   gcc -fsyntax-only -Wall -Wextra -Wshadow -Wconversion -Wformat=2 -Wnull-dereference PROFILE FILE
#   timeout 120 gcc -fanalyzer -c -o /dev/null PROFILE FILE
# and normalises every warning and error to "file:line: category: message".
#
# Nothing is linked or executed.
#
# Usage: tools/static_analyze.sh [FILE...]
# Env:   CC       compiler (default gcc)
#        OUT      output directory (default build/static-analysis)
#        TIMEOUT  per-unit analyzer limit in seconds (default 120)
#
# Output (in OUT):
#   report.txt    normalised diagnostics, sorted, one per line
#   summary.txt   counts per file and category (tab separated: file, category, count)
#   units.txt     per-unit status (tab separated: file, profile or UNBUILDABLE, analyzer rc)
#
# Exit status is 0: the analysis is informational and does not gate CI.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}

# Without a writable temp directory the MSYS2 gcc fails with "Cannot create
# temporary file in C:\WINDOWS\" (TOOLCHAIN.md). Fall back to /tmp, which
# maps to the Windows temp directory under MSYS2.
if [ -z "${TMP:-}" ] || [ -z "${TEMP:-}" ]; then
    tmpdir=$(cygpath -m "${TMPDIR:-/tmp}" 2>/dev/null || echo "${TMPDIR:-/tmp}")
    export TMP="${TMP:-$tmpdir}" TEMP="${TEMP:-$tmpdir}" TMPDIR="${TMPDIR:-/tmp}"
fi
OUT=${OUT:-build/static-analysis}
TIMEOUT=${TIMEOUT:-120}

cd "$ROOT" || exit 0

# Host-shim profiles. Word splitting of these variables is intentional.
# KERNEL_SYNTAX is the tests/syntax/check.sh profile; KERNEL_HOST is the
# tests/kernel-host/Makefile profile (without its generated prelude output);
# NTDLL_HOST is the tests/ntdll-host/Makefile profile.
PROFILE_KERNEL_SYNTAX="-std=c11 -DUSERSPACE_KSHIM -include tests/syntax/shim/kernel_compat.h -Itests/syntax/shim -iquote src/kernel/nt -iquote arch/x86/nt"
PROFILE_KERNEL_HOST="-std=gnu11 -DUSERSPACE_KSHIM -include tests/kernel-host/shim/khost_prelude.h -Itests/kernel-host/shim -iquote src/kernel/nt -iquote arch/x86/nt"
PROFILE_NTDLL_HOST="-std=c11 -Isrc/ntdll -Isrc/loader"

WARN_FLAGS="-Wall -Wextra -Wshadow -Wconversion -Wformat=2 -Wnull-dereference"

if [ "$#" -gt 0 ]; then
    FILES="$*"
else
    FILES=$(ls src/loader/*.c src/kernel/nt/*.c src/ntdll/*.c arch/x86/nt/*.c 2>/dev/null)
fi

mkdir -p "$OUT" || exit 0
RAW="$OUT/raw.txt"
REPORT="$OUT/report.txt"
SUMMARY="$OUT/summary.txt"
UNITS="$OUT/units.txt"
: > "$RAW"
: > "$UNITS"

# Print the label of the first profile that accepts FILE with -fsyntax-only;
# fail otherwise. Labels: kernel-syntax, kernel-host, ntdll-host.
choose_profile() {
    case "$1" in
        src/ntdll/*) labels="ntdll-host" ;;
        *) labels="kernel-syntax kernel-host" ;;
    esac
    for label in $labels; do
        case "$label" in
            kernel-syntax) prof=$PROFILE_KERNEL_SYNTAX ;;
            kernel-host) prof=$PROFILE_KERNEL_HOST ;;
            ntdll-host) prof=$PROFILE_NTDLL_HOST ;;
        esac
        if "$CC" -fsyntax-only $WARN_FLAGS $prof "$1" >/dev/null 2>&1; then
            printf '%s\n' "$label"
            return 0
        fi
    done
    return 1
}

profile_flags() {
    case "$1" in
        kernel-syntax) printf '%s' "$PROFILE_KERNEL_SYNTAX" ;;
        kernel-host) printf '%s' "$PROFILE_KERNEL_HOST" ;;
        ntdll-host) printf '%s' "$PROFILE_NTDLL_HOST" ;;
    esac
}

# Normalise gcc diagnostics on stdin to "file:line: category: message".
# The category is the -W option name without the -W prefix, or "error".
normalise() {
    awk '
    {
        if (match($0, /^[^ ]+:[0-9]+:[0-9]+: (warning|error|fatal error): /)) {
            head = substr($0, 1, RLENGTH)
            msg = substr($0, RLENGTH + 1)
            split(head, p, ":")
            cat = "error"
            if (match(msg, /\[-W[^]]+\]$/)) {
                cat = substr(msg, RSTART + 3, RLENGTH - 4)
                msg = substr(msg, 1, RSTART - 2)
            }
            print p[1] ":" p[2] ": " cat ": " msg
        }
    }'
}

for f in $FILES; do
    [ -f "$f" ] || continue
    if ! label=$(choose_profile "$f"); then
        printf '%s\tUNBUILDABLE\t-\n' "$f" >> "$UNITS"
        continue
    fi
    prof=$(profile_flags "$label")

    # Syntax and warning pass.
    "$CC" -fsyntax-only $WARN_FLAGS $prof "$f" >"$OUT/syntax.tmp" 2>&1
    cat "$OUT/syntax.tmp" >> "$RAW"

    # Analyzer pass. rc 124 means the per-unit timeout expired.
    timeout "$TIMEOUT" "$CC" -fanalyzer -c -o /dev/null $prof "$f" >"$OUT/analyzer.tmp" 2>&1
    rc=$?
    cat "$OUT/analyzer.tmp" >> "$RAW"
    printf '%s\t%s\t%s\n' "$f" "$label" "$rc" >> "$UNITS"
done

normalise < "$RAW" | sort -u > "$REPORT"

# Counts per file and category.
awk -F': ' '{ split($1, a, ":"); n[a[1] "\t" $2]++ }
    END { for (k in n) print k "\t" n[k] }' "$REPORT" | sort > "$SUMMARY"

rm -f "$OUT/syntax.tmp" "$OUT/analyzer.tmp"

echo "static_analyze: units=$(wc -l < "$UNITS" | tr -d ' ') diagnostics=$(wc -l < "$REPORT" | tr -d ' ')"
exit 0
