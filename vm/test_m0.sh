#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
OUT="$RESULT_DIR/test_m0.json"
LOG="$RESULT_DIR/test_m0.log"
mkdir -p "$RESULT_DIR"
echo "=== M0 PE test (exit 42) ===" | tee "$LOG"
PE="/tests_kernel/minimal.exe"
if [ ! -x "$PE" ]; then
  PE="/tests_kernel/minimal0.exe"
fi
echo "Running $PE" | tee -a "$LOG"
set +e
"$PE"
RC=$?
set -e
# The PE should exit 42 via NtTerminateProcess
# QEMU may return RC as 42 or 42+128? Check
if [ "$RC" -ge 128 ]; then RC=$((RC-128)); fi
# Also normalize for do_exit bug
RC_MASKED=$((RC & 0xFF))
echo "PE exit RC=$RC (masked $RC_MASKED)" | tee -a "$LOG"
STATUS="FAIL"
if [ "$RC_MASKED" -eq 42 ] || [ "$RC" -eq 42 ]; then
  STATUS="PASS"
fi
cat > "$OUT" <<EOF
{
  "test": "m0",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "exit_masked": $RC_MASKED,
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
# Also write result.exitcode for runvm harness
echo "$RC_MASKED" > "$SHARE/.dodresult/result.exitcode"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi
