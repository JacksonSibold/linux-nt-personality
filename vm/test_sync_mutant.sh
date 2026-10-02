#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
OUT="$RESULT_DIR/test_sync_mutant.json"
LOG="$RESULT_DIR/test_sync_mutant.log"
mkdir -p "$RESULT_DIR"
echo "=== sync_mutant test ===" | tee "$LOG"
PE="/tests_kernel/sync_mutant.exe"
echo "PE=$PE" | tee -a "$LOG"
ls -l "$PE" 2>&1 | tee -a "$LOG" || true
dmesg -c > /tmp/dmesg_cleared 2>&1 || true
echo "Running PE..." | tee -a "$LOG"
set +e
timeout 30 "$PE"
RC=$?
set -e
if [ "$RC" -ge 128 ]; then RC_RAW=$RC; RC=$((RC-128)); echo "signal exit raw $RC_RAW mapped $RC" | tee -a "$LOG"; fi
RC_MASKED=$((RC & 0xFF))
echo "PE exit RC=$RC masked $RC_MASKED" | tee -a "$LOG"
echo "=== DMESG after PE ===" | tee -a "$LOG"
dmesg 2>&1 | tail -n 500 | tee -a "$LOG"
dmesg > "$SHARE/.dodresult/sync_mutant_dmesg.log" 2>&1 || true
cat "$LOG" > "$SHARE/.dodresult/sync_mutant_serial.log" 2>&1 || true
STATUS="FAIL"
if [ "$RC_MASKED" -eq 0 ] && [ "$RC" -eq 0 ]; then
  STATUS="PASS"
fi
# Also check for RC 9 extra-release case vs 80/81 etc
cat > "$OUT" <<EOF
{
  "test": "sync_mutant",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "exit_masked": $RC_MASKED,
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
echo "$RC_MASKED" > "$SHARE/.dodresult/result.exitcode"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit "$RC_MASKED"; fi
