#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_sync"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d "\r\n" || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_sync_atomic.json"
LOG="$RESULT_DIR/kernel_sync_atomic.log"
mkdir -p "$RESULT_DIR"
echo "=== SYNC_ATOMIC in-VM WaitAll atomicity ===" | tee "$LOG"
PE="/tests_kernel/sync_atomic.exe"
if [ ! -x "$PE" ]; then echo "missing $PE" | tee -a "$LOG"; cat > "$OUT" <<EOF
{"test":"sync_atomic","run_id": "$RUN_ID",
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
  0) STATUS="PASS"; NOTE="WaitAll atomicity contention PASS: overlapping sets different orders, no deadlock, no partial acquisition, sorted-address acquisition-with-undo works per kernel debugfs wait_all:PASS" ;;
  90) STATUS="FAIL"; MISSING="NtCreateEvent"; NOTE="PE detected STATUS_NOT_IMPLEMENTED on NtCreateEvent" ;;
  80) STATUS="FAIL"; MISSING="NtCreateThreadEx"; NOTE="PE detected STATUS_NOT_IMPLEMENTED 0xC0000002 on NtCreateThreadEx - atomicity contention requires threads, blocked (valuable finding per task: coordinate with NtCreateThreadEx state)" ;;
  *) STATUS="FAIL"; NOTE="Case $RC failed: deadlock or partial acquisition detected (atomicity violation) or index mismatch" ;;
esac
cat > "$OUT" <<EOF
{
  "test": "sync_atomic",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "missing_service": "$MISSING",
  "note": "$NOTE",
  "golden": "WaitAll atomicity: no deadlock, no partial, sorted-address undo (prepare_to_wait/finish_wait)",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi