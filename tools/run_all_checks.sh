#!/usr/bin/env bash
# tools/run_all_checks.sh - run every local check in order and print a
# PASS / FAIL / KNOWN-FAIL / SKIP matrix.
#
# Order:
#   1. unit tests            python -m unittest discover tests/unit
#   2. check_repo            tools/check_repo.py
#   3. check_doc_links       tools/check_doc_links.py
#   4. provenance_scan       tools/provenance_scan.py --check (if the file exists)
#   5. lint                  vm/lint.sh
#   6. syntax                tests/syntax/check.sh
#   7. host                  make -C tests/host test
#   8. kernel-host           make -C tests/kernel-host check
#
# Python: $PYTHON when set and working, otherwise the WinPython path below,
# otherwise python3 or python. A candidate counts only if it runs.
# MSYS2 PATH: /c/msys64/ucrt64/bin and /c/msys64/usr/bin are prepended only when
# they exist (see C:/Users/Jackson/repos/TOOLCHAIN.md). A section whose tool is
# missing prints a SKIP line and is not a failure.
#
# Every section runs under `timeout 600`. kernel-host gets TIMEOUT=600 instead,
# which its Makefile applies per module, so one slow module cannot hide the rest.
#
# Kernel-host modules that are known to fail are listed in
# tests/kernel-host/README.md as a line starting with "known-fail:". They are
# reported as KNOWN-FAIL and do not fail the run.
#
# Usage: tools/run_all_checks.sh        (from anywhere)
# Exit:  0 when no section is FAIL (PASS, KNOWN-FAIL and SKIP are not failures),
#        1 when at least one section is FAIL.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 1

TIMEOUT_SEC=600
WINPY="C:/Users/Jackson/AppData/Local/Programs/WinPython/WPy64-3.13.12.0/python/python.exe"

# MSYS2 toolchain from TOOLCHAIN.md, only if installed.
for d in /c/msys64/usr/bin /c/msys64/ucrt64/bin; do
    [ -d "$d" ] && PATH="$d:$PATH"
done
export PATH

# make's Windows Temp default needs LOCALAPPDATA; derive it from TMPDIR otherwise.
if [ -z "${LOCALAPPDATA:-}" ] && [ -z "${TEMP_DIR:-}" ] && command -v cygpath >/dev/null 2>&1; then
    TEMP_DIR=$(cygpath -m "${TMPDIR:-/tmp}")
    export TEMP_DIR
fi

find_python() {
    local c
    for c in "${PYTHON:-}" "$WINPY" python3 python; do
        [ -n "$c" ] || continue
        if { command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; } && "$c" -c 'import sys' >/dev/null 2>&1; then
            printf '%s' "$c"
            return 0
        fi
    done
    return 1
}
PY=$(find_python) || PY=""

LOGDIR="${TMPDIR:-${TEMP:-/tmp}}/run-all-checks.$$"
mkdir -p "$LOGDIR" || { echo "cannot create log directory $LOGDIR" >&2; exit 1; }
RESULTS=()

record() { RESULTS+=("$1|$2|$3"); }

skip() {
    echo "SKIP: $1: $2"
    record "$1" SKIP "$2"
}

# run_section NAME CMD...  - runs CMD under timeout, logs to $LOGDIR/NAME.log
run_section() {
    local name=$1
    shift
    local log="$LOGDIR/$name.log"
    echo "== $name =="
    timeout "$TIMEOUT_SEC" "$@" >"$log" 2>&1
    local rc=$?
    if [ "$rc" -eq 0 ]; then
        record "$name" PASS ""
    elif [ "$rc" -eq 124 ]; then
        record "$name" FAIL "timed out after ${TIMEOUT_SEC}s"
        tail -n 20 "$log"
    else
        record "$name" FAIL "exit $rc, log $log"
        tail -n 20 "$log"
    fi
}

# Python-based sections need an interpreter.
if [ -n "$PY" ]; then
    run_section unit "$PY" -m unittest discover tests/unit
    run_section check_repo "$PY" tools/check_repo.py
    run_section check_doc_links "$PY" tools/check_doc_links.py
    if [ -f tools/provenance_scan.py ]; then
        run_section provenance "$PY" tools/provenance_scan.py --check
    else
        skip provenance "tools/provenance_scan.py not present"
    fi
else
    for n in unit check_repo check_doc_links provenance; do
        skip "$n" "no working Python (set PYTHON or install WinPython)"
    done
fi

run_section lint bash vm/lint.sh

if command -v gcc >/dev/null 2>&1 || [ -n "${CC:-}" ]; then
    run_section syntax sh tests/syntax/check.sh
else
    skip syntax "gcc not on PATH"
fi

if command -v make >/dev/null 2>&1 && command -v gcc >/dev/null 2>&1; then
    run_section host make -C tests/host test
else
    skip host "make or gcc not on PATH"
fi

# kernel-host: one result row per module, parsed from the "NAME rc=N" lines.
if command -v make >/dev/null 2>&1 && command -v gcc >/dev/null 2>&1; then
    echo "== kernel-host =="
    kh_log="$LOGDIR/kernel-host.log"
    make -C tests/kernel-host check TIMEOUT="$TIMEOUT_SEC" >"$kh_log" 2>&1
    kh_rc=$?
    known=$(sed -n 's/^known-fail:[[:space:]]*//p' tests/kernel-host/README.md | tr '\n' ' ')
    rows=0
    while read -r mod code; do
        rows=$((rows + 1))
        if [ "$code" -eq 0 ]; then
            record "kernel-host:$mod" PASS ""
        elif [[ " $known " == *" $mod "* ]]; then
            record "kernel-host:$mod" KNOWN-FAIL "rc=$code (listed in tests/kernel-host/README.md)"
        elif [ "$code" -eq 124 ]; then
            record "kernel-host:$mod" FAIL "timed out after ${TIMEOUT_SEC}s"
        else
            record "kernel-host:$mod" FAIL "rc=$code, log $kh_log"
        fi
    done < <(sed -n 's/^\([A-Za-z0-9_]*\) rc=\([0-9]*\)\r\{0,1\}$/\1 \2/p' "$kh_log")
    if [ "$rows" -eq 0 ]; then
        record kernel-host FAIL "build or run failed before any module (make rc=$kh_rc), log $kh_log"
        tail -n 20 "$kh_log"
    fi
else
    skip kernel-host "make or gcc not on PATH"
fi

echo
echo "== PASS/FAIL matrix =="
printf '%-26s %-11s %s\n' CHECK STATUS NOTE
fails=0
for r in "${RESULTS[@]}"; do
    IFS='|' read -r name status note <<<"$r"
    printf '%-26s %-11s %s\n' "$name" "$status" "$note"
    [ "$status" = FAIL ] && fails=$((fails + 1))
done
echo
count() { local s=$1 n=0 r; for r in "${RESULTS[@]}"; do [[ "$r" == *"|$s|"* ]] && n=$((n + 1)); done; echo "$n"; }
echo "PASS=$(count PASS) FAIL=$fails KNOWN-FAIL=$(count KNOWN-FAIL) SKIP=$(count SKIP)"
echo "logs: $LOGDIR"

[ "$fails" -eq 0 ]
