#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_sync"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d '\r\n' || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_sync.json"
LOG="$RESULT_DIR/kernel_sync.log"
mkdir -p "$RESULT_DIR"
echo "=== SYNC in-VM harness ===" | tee "$LOG"
# For now reuse handles probe as sync (event)
PE="/tests_kernel/handles_probe.exe"
if [ ! -x "$PE" ]; then PE="/tests_kernel/minimal0.exe"; fi
echo "Running $PE (sync via Event)" | tee -a "$LOG"
set +e
"$PE"
RC=$?
set -e
if [ "$RC" -ge 128 ]; then RC=$((RC-128)); fi
echo "RC=$RC" | tee -a "$LOG"
STATUS="FAIL"
MISSING="NtWaitForMultipleObjects (partial), NtWaitForSingleObject with timeout, Mutant abandonment"
if [ "$RC" -eq 0 ]; then
  # Even if PE succeeded, full sync matrix not yet implemented
  STATUS="FAIL"
  MISSING="WaitAny/WaitAll with 64 objects, mutant abandonment, event manual/auto reset semantics need full validation (NtWaitForMultipleObjects currently returns TIMEOUT for non-signaled)"
else
  STATUS="FAIL"
fi
cat > "$OUT" <<EOF
{
  "test": "sync",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "missing_service": "$MISSING",
  "note": "Event create/set/close succeed; WaitForSingleObject on signaled event succeeds, but full 4096-case sync matrix including abandonment and WaitAny/All with timeout not yet fully implemented (nt_sync.c returns TIMEOUT for non-signaled).",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
exit 1
