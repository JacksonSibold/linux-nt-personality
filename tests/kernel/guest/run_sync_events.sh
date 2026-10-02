#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_sync"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d "\r\n" || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_sync_events.json"
LOG="$RESULT_DIR/kernel_sync_events.log"
mkdir -p "$RESULT_DIR"
echo "=== SYNC_EVENTS in-VM ===" | tee "$LOG"
PE="/tests_kernel/sync_events.exe"
if [ ! -x "$PE" ]; then echo "missing $PE" | tee -a "$LOG"; cat > "$OUT" <<EOF
{"test":"sync_events","run_id": "$RUN_ID",
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
# Map exit codes from sync_events.c: 0 PASS, 90 missing NtPulseEvent (0xC0000002), 80 blocked by thread, 91 missing NtCreateEvent etc, otherwise case failure
STATUS="FAIL"
MISSING=""
NOTE=""
VOLUME="0"
case "$RC" in
  0) STATUS="PASS"; MISSING=""; NOTE="auto 258->0->258 manual 258->0->0->258 pulse no-waiter 0,prev0,timeout258 PASS; thread waiter blocked not required for basic"; VOLUME="3" ;;
  90) STATUS="FAIL"; MISSING="NtPulseEvent"; NOTE="PE detected STATUS_NOT_IMPLEMENTED 0xC0000002 on NtPulseEvent syscall (real NTSTATUS), pulse semantics not implemented in kernel" ;;
  91) STATUS="FAIL"; MISSING="NtCreateEvent"; NOTE="PE detected STATUS_NOT_IMPLEMENTED on NtCreateEvent" ;;
  80) STATUS="FAIL"; MISSING="NtCreateThreadEx"; NOTE="PE detected STATUS_NOT_IMPLEMENTED 0xC0000002 on NtCreateThreadEx - pulse with waiters blocked (needs threads)" ;;
  *) STATUS="FAIL"; MISSING=""; NOTE="Case $RC failed: divergence from Windows golden (probe_sync.json) - see sync_events.c exit mapping" ;;
esac
# Honest volume: this test covers 3 core event cases at volume 1 each; not yet 10M stress
cat > "$OUT" <<EOF
{
  "test": "sync_events",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "missing_service": "$MISSING",
  "volume": "$VOLUME",
  "note": "$NOTE",
  "golden": "probe_sync.json events auto/manual/pulse_auto/pulse_manual",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi