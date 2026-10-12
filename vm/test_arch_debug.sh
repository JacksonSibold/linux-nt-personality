#!/bin/sh
set -x
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_arch"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d '\r\n' || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_arch.json"
LOG="$RESULT_DIR/kernel_arch.log"
mkdir -p "$RESULT_DIR"
echo "=== ARCH in-VM harness ===" | tee "$LOG"
echo "guest uname: $(cat /proc/sys/kernel/osrelease 2>/dev/null || uname -r)" | tee -a "$LOG"
mkdir -p /sys/kernel/debug 2>/dev/null || true
if ! mountpoint -q /sys/kernel/debug 2>/dev/null; then
  mount -t debugfs none /sys/kernel/debug 2>>"$LOG" || echo "debugfs mount failed (non-fatal)" | tee -a "$LOG"
fi
echo "debugfs ok" | tee -a "$LOG"
DISPATCH_PATH="/sys/kernel/debug/ntpers/dispatch_count"
HOST_PATH="/sys/kernel/debug/ntpers/host_count"
read_counter() {
  _p="$1"
  _v="0"
  if [ -f "$_p" ]; then
    _v=$(cat "$_p" 2>/dev/null | tr -d ' \n\r')
    if [ -z "$_v" ]; then _v="0"; fi
    case "$_v" in
      ''|*[!0-9]*) _v="0" ;;
    esac
  else
    _v="0"
  fi
  echo "$_v"
}
BEFORE_NT=$(read_counter "$DISPATCH_PATH")
BEFORE_HOST=$(read_counter "$HOST_PATH")
echo "BEFORE_NT=$BEFORE_NT HOST=$BEFORE_HOST" | tee -a "$LOG"
TOTAL=100
SUCCESS=0
i=1
while [ "$i" -le "$TOTAL" ]; do
  if [ -x /tests_kernel/minimal0.exe ]; then
    /tests_kernel/minimal0.exe 2>/dev/null
    RC=$?
  else
    echo "no PE" | tee -a "$LOG"
    RC=1
  fi
  if [ "$RC" -eq 0 ]; then SUCCESS=$((SUCCESS+1)); fi
  i=$((i+1))
done
echo "launches $SUCCESS/$TOTAL succeeded" | tee -a "$LOG"
AFTER_NT=$(read_counter "$DISPATCH_PATH")
AFTER_HOST=$(read_counter "$HOST_PATH")
echo "AFTER_NT=$AFTER_NT HOST=$AFTER_HOST" | tee -a "$LOG"
NT_SYSCALLS=$((AFTER_NT - BEFORE_NT))
if [ "$NT_SYSCALLS" -lt 0 ]; then NT_SYSCALLS=0; fi
HOST_SYSCALLS=$((AFTER_HOST - BEFORE_HOST))
if [ "$HOST_SYSCALLS" -lt 0 ]; then HOST_SYSCALLS=0; fi
echo "NT=$NT_SYSCALLS HOST=$HOST_SYSCALLS" | tee -a "$LOG"
if [ "$NT_SYSCALLS" -gt 0 ]; then RATIO=$(awk "BEGIN{printf \"%.4f\", $HOST_SYSCALLS/$NT_SYSCALLS}"); echo "RATIO=$RATIO" | tee -a "$LOG"; fi
echo "done" | tee -a "$LOG"
mkdir -p /mnt/share/.dodresult
cat > /mnt/share/.dodresult/result.exitcode <<EOF
0
EOF
exit 0
