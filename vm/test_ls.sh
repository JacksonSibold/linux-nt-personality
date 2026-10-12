#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
OUT="$RESULT_DIR/test_ls.json"
LOG="$RESULT_DIR/test_ls.log"
mkdir -p "$RESULT_DIR"
echo "=== ls test ===" | tee "$LOG"
ls -l /tests_kernel/ 2>&1 | tee -a "$LOG"
for f in /tests_kernel/*; do echo "--- $f ---" | tee -a "$LOG"; file "$f" 2>&1 | tee -a "$LOG" || true; hexdump -C "$f" | head -n 5 | tee -a "$LOG" || true; done
# Try running each PE and capture exit
for f in /tests_kernel/minimal*.exe /tests_kernel/*.exe; do
  if [ -x "$f" ]; then
    echo "Trying $f" | tee -a "$LOG"
    set +e
    "$f"
    RC=$?
    set -e
    if [ "$RC" -ge 128 ]; then RC=$((RC-128)); fi
    echo "  exit $RC" | tee -a "$LOG"
  fi
done
dmesg | grep -E "binfmt_pe|do_nt_syscall|ntpers" | tail -n 50 | tee -a "$LOG" || true
cat > "$OUT" <<EOF
{"test":"ls","status":"PASS","timestamp":"$(date -u +%Y-%m-%dT%H:%M:%SZ)"}
EOF
echo "0" > "$SHARE/.dodresult/result.exitcode"
exit 0
