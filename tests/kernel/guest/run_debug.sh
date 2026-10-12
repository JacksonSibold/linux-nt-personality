#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
OUT="$RESULT_DIR/debug_mem.json"
LOG="$RESULT_DIR/debug_mem.log"
mkdir -p "$RESULT_DIR"
echo "=== DEBUG MEM ===" | tee "$LOG"
PE="/tests_kernel/debug_mem.exe"
if [ ! -x "$PE" ]; then echo "no PE"; exit 1; fi
echo "Running $PE" | tee -a "$LOG"
set +e
"$PE"
RC=$?
set -e
if [ "$RC" -ge 128 ]; then RC=$((RC-128)); fi
echo "RC=$RC" | tee -a "$LOG"
dmesg | tail -n 50 | tee -a "$LOG"
if [ "$RC" -eq 0 ]; then echo PASS; else echo FAIL; fi
echo "$RC" > "$SHARE/.dodresult/result.exitcode"
exit $RC
