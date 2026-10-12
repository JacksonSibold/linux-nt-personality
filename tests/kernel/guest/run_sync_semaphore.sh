#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_sync"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d "\r\n" || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_sync_semaphore.json"
LOG="$RESULT_DIR/kernel_sync_semaphore.log"
mkdir -p "$RESULT_DIR"
echo "=== SYNC_SEMAPHORE in-VM ===" | tee "$LOG"
PE="/tests_kernel/sync_semaphore.exe"
if [ ! -x "$PE" ]; then echo "missing $PE" | tee -a "$LOG"; cat > "$OUT" <<EOF
{"test":"sync_semaphore","run_id": "$RUN_ID",
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
  0) STATUS="PASS"; NOTE="initial1_max3 release prev 1,2 over-release STATUS_SEMAPHORE_LIMIT_EXCEEDED 0xC0000047 prev -1, prev_count_test 0/5 PASS per golden" ;;
  90) STATUS="FAIL"; MISSING="NtCreateSemaphore"; NOTE="PE detected STATUS_NOT_IMPLEMENTED 0xC0000002 on NtCreateSemaphore, semaphore objects not implemented (only NtCreateEvent wired)" ;;
  *) STATUS="FAIL"; NOTE="Case $RC failed: divergence from Windows golden semaphore 0xC0000047 etc" ;;
esac
cat > "$OUT" <<EOF
{
  "test": "sync_semaphore",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "missing_service": "$MISSING",
  "note": "$NOTE",
  "golden": "probe_sync.json semaphore initial1_max3, nt_over_release 0xC0000047, prev_count_test",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi