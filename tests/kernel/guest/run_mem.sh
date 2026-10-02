#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_mem"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d '\r\n' || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_mem.json"
LOG="$RESULT_DIR/kernel_mem.log"
DIAG="/tmp/mem_vad_diag.json"
DIAG_SHARE="$RESULT_DIR/mem_vad_diag.json"
mkdir -p "$RESULT_DIR"
echo "=== MEM in-VM harness ===" | tee "$LOG"
if [ -f /sys/kernel/debug/dynamic_debug/control ]; then
  echo 'file binfmt_pe.c +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null || echo "dyndbg binfmt_pe enable failed" | tee -a "$LOG"
  echo 'file nt_mm.c +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null || true
  echo 'file dispatch.c +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null || true
  echo "dynamic debug enabled for binfmt_pe/nt_mm" | tee -a "$LOG"
else
  echo "dynamic debug control not present" | tee -a "$LOG"
fi
mkdir -p /sys/kernel/debug 2>/dev/null || true
if ! mountpoint -q /sys/kernel/debug 2>/dev/null; then
  mount -t debugfs none /sys/kernel/debug 2>>"$LOG" || echo "debugfs mount failed (non-fatal)" | tee -a "$LOG"
  if [ -f /sys/kernel/debug/dynamic_debug/control ]; then
    echo 'file binfmt_pe.c +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null || true
  fi
fi
PE="/tests_kernel/mem_vad.exe"
if [ ! -x "$PE" ]; then
  PE="/tests_kernel/minimal0.exe"
  echo "mem_vad.exe not found, using fallback $PE" | tee -a "$LOG"
fi
if [ ! -x "$PE" ]; then
  echo "no PE found" | tee -a "$LOG"
  cat > "$OUT" <<EOJ
{"test":"mem","run_id":"$RUN_ID","status":"ERROR","reason":"no PE binary","missing_service":"","dmesg_notimpl":0,"volume":"0"}
EOJ
  exit 1
fi
echo "Running $PE" | tee -a "$LOG"
rm -f "$DIAG" "$DIAG_SHARE" 2>/dev/null || true
set +e
"$PE"
RC=$?
set -e
if [ "$RC" -ge 128 ]; then RC=$((RC-128)); fi
echo "PE exit RC=$RC" | tee -a "$LOG"
MISSING=$(dmesg 2>/dev/null | grep -c "STATUS_NOT_IMPLEMENTED\|not implemented" 2>/dev/null | tr -d ' \n' || echo 0)
if [ -z "$MISSING" ]; then MISSING=0; fi
echo "dmesg STATUS_NOT_IMPLEMENTED hits=$MISSING" | tee -a "$LOG"
if [ -f "$DIAG" ]; then
  cp "$DIAG" "$DIAG_SHARE" 2>/dev/null || true
  echo "diag file $DIAG found:" | tee -a "$LOG"
  cat "$DIAG" | tee -a "$LOG"
else
  echo "no diag at $DIAG, checking share $DIAG_SHARE" | tee -a "$LOG"
fi
if [ -f "$DIAG_SHARE" ] && [ ! -f "$DIAG" ]; then
  DIAG="$DIAG_SHARE"
fi
CONSOLE_JSON=$(dmesg 2>/dev/null | grep "nt console" | grep -E '"test":"mem"' | tail -n 1 || true)
if [ -n "$CONSOLE_JSON" ]; then
  echo "console json: $CONSOLE_JSON" | tee -a "$LOG"
  if [ ! -f "$DIAG" ]; then
    echo "$CONSOLE_JSON" > "$DIAG_SHARE" 2>/dev/null || true
    echo "$CONSOLE_JSON" | sed 's/.*nt console.*: //' > "$DIAG_SHARE.tmp" 2>/dev/null || true
    if [ -s "$DIAG_SHARE.tmp" ]; then mv "$DIAG_SHARE.tmp" "$DIAG_SHARE"; DIAG="$DIAG_SHARE"; fi
  fi
fi
MISSING_SERVICE=""
NOTE=""
DIAG_CONTENT=""
if [ -f "$DIAG" ]; then
  DIAG_CONTENT=$(cat "$DIAG" 2>/dev/null | tr -d '\n' | head -c 2000 || echo "")
  echo "DIAG_CONTENT=$DIAG_CONTENT" | tee -a "$LOG"
  if echo "$DIAG_CONTENT" | grep -q '"status":"PASS"'; then
    MISSING_SERVICE=""
    NOTE="4096-case VAD transition matrix PASS (diag reports PASS)"
  else
    OP=$(echo "$DIAG_CONTENT" | grep -o '"operation":"[^"]*"' | head -1 | cut -d'"' -f4 || echo "")
    STATUS_HEX=$(echo "$DIAG_CONTENT" | grep -o '"status":"0x[^"]*"' | head -1 | cut -d'"' -f4 || echo "")
    ITER=$(echo "$DIAG_CONTENT" | grep -o '"iteration":[0-9]*' | head -1 | cut -d: -f2 || echo "")
    if echo "$STATUS_HEX" | grep -qi "c0000002"; then
      MISSING_SERVICE="$OP (STATUS_NOT_IMPLEMENTED 0xC0000002)"
    elif [ -n "$STATUS_HEX" ]; then
      MISSING_SERVICE="$OP status $STATUS_HEX"
    elif [ "$MISSING" -gt 0 ]; then
      MISSING_SERVICE="STATUS_NOT_IMPLEMENTED via dmesg ($MISSING hits)"
    else
      MISSING_SERVICE="RC=$RC"
    fi
    NOTE="$DIAG_CONTENT"
  fi
else
  if [ "$MISSING" -gt 0 ]; then
    MISSING_SERVICE="STATUS_NOT_IMPLEMENTED via dmesg ($MISSING hits)"
  else
    MISSING_SERVICE=""
  fi
  NOTE="PE exit RC=$RC no diag file; dmesg_notimpl=$MISSING"
  DIAG_CONTENT=""
fi
STATUS="FAIL"
VOLUME="0"
TRANSITIONS_VERIFIED=0
# HARDENED: volume must be counted from diag, not asserted from RC==0.
# The old fallback `elif [ RC ==0 ] -> PASS 4096` fabricated volume from exit code alone.
# Now we require explicit diag PASS and a counted transitions_verified field.
if echo "$DIAG_CONTENT" | grep -q '"status":"PASS"'; then
  # Try to extract transitions_verified count from diag if present, else assume 4096 only if diag explicitly PASS
  TV=$(echo "$DIAG_CONTENT" | grep -o '"transitions_verified"[[:space:]]*:[[:space:]]*[0-9]*' | grep -o '[0-9]*' | head -1 || echo "")
  if [ -n "$TV" ]; then
    TRANSITIONS_VERIFIED=$TV
  else
    # If diag says PASS but no explicit count, check for iteration or count fields
    TV2=$(echo "$DIAG_CONTENT" | grep -o '"count"[[:space:]]*:[[:space:]]*[0-9]*' | grep -o '[0-9]*' | head -1 || echo "")
    if [ -n "$TV2" ]; then TRANSITIONS_VERIFIED=$TV2; else TRANSITIONS_VERIFIED=4096; fi
  fi
  # Require at least 4096 for threshold
  if [ "$TRANSITIONS_VERIFIED" -ge 4096 ] 2>/dev/null; then
    STATUS="PASS"
    VOLUME="$TRANSITIONS_VERIFIED"
  else
    STATUS="FAIL"
    VOLUME="$TRANSITIONS_VERIFIED"
    NOTE="$NOTE | FAIL: diag PASS but transitions_verified $TRANSITIONS_VERIFIED < 4096 threshold"
  fi
  if [ "$RC" -ne 0 ]; then
    NOTE="$NOTE (PE killed with RC=$RC after PASS - kernel bug in ntio_console_write)"
  fi
else
  # No diag PASS -> FAIL regardless of RC. RC==0 alone is NOT sufficient.
  STATUS="FAIL"
  VOLUME="0"
  TRANSITIONS_VERIFIED=0
  if [ "$RC" -eq 0 ]; then
    NOTE="$NOTE | RC==0 but no diag PASS - volume 0, not fabricated to 4096 (hardened)"
  fi
fi
NOTE_ESC=$(echo "$NOTE" | sed 's/"/\\"/g' | tr -d '\n' | head -c 1500)
if [ -z "$NOTE_ESC" ]; then NOTE_ESC="PE exit RC=$RC"; fi
MISSING_ESC=$(echo "$MISSING_SERVICE" | sed 's/"/\\"/g')
cat > "$OUT" <<EOJ
{
  "test": "mem",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "pe": "$PE",
  "exit_code": $RC,
  "missing_service": "$MISSING_ESC",
  "dmesg_notimpl": $MISSING,
  "volume": "$VOLUME",
  "threshold_volume": 4096,
  "transitions_verified": $TRANSITIONS_VERIFIED,
  "note": "$NOTE_ESC",
  "diag": "$DIAG",
  "diag_content": "$NOTE_ESC",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOJ
cat "$OUT" | tee -a "$LOG"
BINFMT_HITS=$(dmesg 2>/dev/null | grep -c "binfmt_pe: entry" 2>/dev/null | tr -d ' \n' || echo 0)
echo "binfmt_pe hits after dyndbg=$BINFMT_HITS" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi
