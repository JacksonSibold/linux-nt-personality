#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_sync"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d "\r\n" || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_sync_mutant.json"
LOG="$RESULT_DIR/kernel_sync_mutant.log"
mkdir -p "$RESULT_DIR"
echo "=== SYNC_MUTANT in-VM ===" | tee "$LOG"
PE="/tests_kernel/sync_mutant.exe"
if [ ! -x "$PE" ]; then echo "missing $PE" | tee -a "$LOG"; cat > "$OUT" <<EOF
{"test":"sync_mutant","run_id": "$RUN_ID",
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
  0) STATUS="PASS"; NOTE="recursive acquire 3 PASS, 3 releases PASS, extra release STATUS_MUTANT_NOT_OWNED 0xC0000046, abandon STATUS_ABANDONED 0x80 PASS" ;;
  90) STATUS="FAIL"; MISSING="NtCreateMutant"; NOTE="PE detected STATUS_NOT_IMPLEMENTED 0xC0000002 on NtCreateMutant (real NTSTATUS), mutant objects not implemented in kernel (nt_services.c only has stubs)" ;;
  80) STATUS="FAIL"; MISSING="NtCreateThreadEx"; NOTE="PE detected STATUS_NOT_IMPLEMENTED on NtCreateThreadEx - mutant non-owner and abandonment require threads, blocked" ;;
  81) STATUS="FAIL"; MISSING="NtCreateThreadEx"; NOTE="PE detected STATUS_NOT_IMPLEMENTED on NtCreateThreadEx - abandonment test blocked" ;;
  *) STATUS="FAIL"; NOTE="Case $RC failed: divergence from Windows golden mutant recursive/non-owner/abandon (probe_sync.json)" ;;
esac
cat > "$OUT" <<EOF
{
  "test": "sync_mutant",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "missing_service": "$MISSING",
  "note": "$NOTE",
  "golden": "probe_sync.json mutant recursive_acquire, non_owner 0xC0000046, abandon 0x80",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi