#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_sync"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d "\r\n" || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_sync_wait64.json"
LOG="$RESULT_DIR/kernel_sync_wait64.log"
mkdir -p "$RESULT_DIR"
echo "=== SYNC_WAIT64 in-VM ===" | tee "$LOG"
PE="/tests_kernel/sync_wait64.exe"
if [ ! -x "$PE" ]; then echo "missing $PE" | tee -a "$LOG"; cat > "$OUT" <<EOF
{"test":"sync_wait64","run_id": "$RUN_ID",
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
STATUS="FAIL"; MISSING=""; NOTE=""; VOLUME="0"
case "$RC" in
  0) STATUS="PASS"; MISSING=""; NOTE="64-object WaitAny indices correct (1,63,0), WaitAll 2/64 PASS, invalid param/handle PASS, 10k shuffle PASS, abandon sections done"; VOLUME="10000" ;;
  90) STATUS="FAIL"; MISSING="NtCreateEvent"; NOTE="PE detected STATUS_NOT_IMPLEMENTED on NtCreateEvent - wait engine not reachable" ;;
  80) STATUS="FAIL"; MISSING="NtCreateThreadEx"; NOTE="PE detected STATUS_NOT_IMPLEMENTED on NtCreateThreadEx - wait abandonment part blocked, but core 64-limit indices required for DOD-SYNC-A007/B007 not fully mappable until threads" ;;
  81) STATUS="FAIL"; MISSING="NtCreateThreadEx"; NOTE="PE detected STATUS_NOT_IMPLEMENTED on NtCreateThreadEx for abandon" ;;
  *) STATUS="FAIL"; NOTE="Case $RC failed: divergence from Windows golden wait_multiple indices or volume (probe_sync.json wait_any_index 1 wait_all etc)" ;;
esac
# Threshold for DOD-SYNC-A007/B007 is 1M shuffles; we do 10k, so even on PASS we are below threshold -> honest report not mappable
if [ "$STATUS" = "PASS" ] && [ "$VOLUME" -lt 1000000 ]; then
  NOTE="$NOTE | VOLUME 10k < threshold 1M for DOD-SYNC-A007/B007, criterion not mappable yet (need 1M)"
fi
cat > "$OUT" <<EOF
{
  "test": "sync_wait64",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "missing_service": "$MISSING",
  "volume": "$VOLUME",
  "threshold": 1000000,
  "note": "$NOTE",
  "golden": "probe_sync.json wait_multiple wait_any_index 1 wait_all_2signaled 0 wait_all_timeout 258 abandon 128",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi