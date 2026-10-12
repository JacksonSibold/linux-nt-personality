#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
OUT="$RESULT_DIR/test_m0.json"
LOG="$RESULT_DIR/test_m0.log"
mkdir -p "$RESULT_DIR"
echo "=== M0 PE test minimal0 (exit 42) ===" | tee "$LOG"
PE="/tests_kernel/minimal0.exe"
echo "Running $PE" | tee -a "$LOG"
set +e
"$PE"
RC=$?
set -e
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
echo "$RC_MASKED" > "$SHARE/.dodresult/result.exitcode"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi
