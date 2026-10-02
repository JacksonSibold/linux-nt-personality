#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_sync"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d "\r\n" || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_sync_timing.json"
LOG="$RESULT_DIR/kernel_sync_timing.log"
mkdir -p "$RESULT_DIR"
echo "=== SYNC_TIMING in-VM ===" | tee "$LOG"
PE="/tests_kernel/sync_timing.exe"
if [ ! -x "$PE" ]; then echo "missing $PE" | tee -a "$LOG"; cat > "$OUT" <<EOF
{"test":"sync_timing","run_id": "$RUN_ID",
  "status":"ERROR","reason":"PE not found","missing_service":"build missing","timestamp":"$(date -u +%Y-%m-%dT%H:%M:%SZ)"}
EOF
exit 1; fi
echo "Running $PE" | tee -a "$LOG"
set +e
"$PE"
RC=$?
set -e
if [ "$RC" -ge 128 ]; then RC=$((RC-128)); fi
echo "RC=$RC" | tee -a "$LOG"
STATUS="FAIL"; MISSING=""; NOTE=""
case "$RC" in
  0) STATUS="PASS"; NOTE="timeout accuracy 100ms within 80-150ms band and 10ms band per golden 109.6ms PASS" ;;
  90) STATUS="FAIL"; MISSING="NtCreateEvent"; NOTE="PE detected STATUS_NOT_IMPLEMENTED on NtCreateEvent" ;;
  80) STATUS="FAIL"; MISSING="NtQueryPerformanceCounter"; NOTE="PE detected STATUS_NOT_IMPLEMENTED on NtQueryPerformanceCounter - cannot measure elapsed, timeout result PASS but accuracy not verifiable (threshold not met)" ;;
  81) STATUS="FAIL"; MISSING="NtQueryPerformanceCounter"; NOTE="PE detected STATUS_NOT_IMPLEMENTED on NtQueryPerformanceCounter" ;;
  *) STATUS="FAIL"; NOTE="Case $RC failed: timeout not 258 or elapsed outside band (probe_sync.json elapsed 109ms)" ;;
esac
cat > "$OUT" <<EOF
{
  "test": "sync_timing",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "missing_service": "$MISSING",
  "note": "$NOTE",
  "golden": "probe_sync.json timeout wait_100ms_result 258 elapsed 109.624ms",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi