#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
OUT="$RESULT_DIR/test_n4.json"
LOG="$RESULT_DIR/test_n4.log"
SERIAL_LOG="/mnt/share/.dodresult/n4_serial.log"
mkdir -p "$RESULT_DIR"
echo "=== N4 thread test ===" | tee "$LOG"
PE="/tests_kernel/n4_thread.exe"
echo "PE=$PE" | tee -a "$LOG"
ls -l "$PE" 2>&1 | tee -a "$LOG" || true
# Clear dmesg before
echo "clear dmesg" | tee -a "$LOG"
dmesg -c > /tmp/dmesg_cleared 2>&1 || true
echo "Running PE..." | tee -a "$LOG"
set +e
timeout 30 "$PE"
RC=$?
set -e
if [ "$RC" -ge 128 ]; then RC_RAW=$RC; RC=$((RC-128)); echo "signal exit raw $RC_RAW mapped $RC" | tee -a "$LOG"; fi
RC_MASKED=$((RC & 0xFF))
echo "PE exit RC=$RC masked $RC_MASKED" | tee -a "$LOG"
# Capture dmesg after
echo "=== DMESG after PE ===" | tee -a "$LOG"
dmesg 2>&1 | tail -n 500 | tee -a "$LOG"
dmesg 2>&1 | grep -E "W1_INSTR|do_syscall_64|do_nt_syscall|NtCreateThreadEx|binfmt_pe" | tail -n 500 | tee -a "$LOG"
# Also capture serial log already? Just copy dmesg to share
dmesg > "$SHARE/.dodresult/n4_dmesg.log" 2>&1 || true
cat "$LOG" > "$SHARE/.dodresult/n4_serial_capture.log" 2>&1 || true
# Determine status - n4_thread exits 0 on success, various codes on failure as per source
STATUS="FAIL"
if [ "$RC_MASKED" -eq 0 ] && [ "$RC" -eq 0 ]; then
  STATUS="PASS"
fi
cat > "$OUT" <<EOF
{
  "test": "n4_thread",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "exit_masked": $RC_MASKED,
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
echo "$RC_MASKED" > "$SHARE/.dodresult/result.exitcode"
# Also write full dmesg to log for post-mortem
if [ "$STATUS" = "PASS" ]; then exit 0; else exit "$RC_MASKED"; fi
