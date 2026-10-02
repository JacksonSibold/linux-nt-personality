#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_provenance"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d '\r\n' || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_provenance.json"
LOG="$RESULT_DIR/kernel_provenance.log"
mkdir -p "$RESULT_DIR"
echo "=== Provenance scan in-VM ===" | tee "$LOG"
echo "guest uname: $(cat /proc/sys/kernel/osrelease 2>/dev/null || uname -r)" | tee -a "$LOG"

FORBIDDEN_PATTERN="wine|wineserver|qemu.*tcg|box64|fex-emu|dosemu"
HITS=0
DETAILS=""

echo "Scanning kernel build graph..." | tee -a "$LOG"
if [ -f /proc/kallsyms ]; then
  if grep -qiE "$FORBIDDEN_PATTERN" /proc/kallsyms 2>/dev/null; then
    HITS=$((HITS+1))
    DETAILS="${DETAILS}kallsyms_hit;"
    echo "HIT kallsyms" | tee -a "$LOG"
  fi
fi
if dmesg 2>/dev/null | grep -qiE "$FORBIDDEN_PATTERN"; then
  HITS=$((HITS+1))
  DETAILS="${DETAILS}dmesg_hit;"
  echo "HIT dmesg" | tee -a "$LOG"
fi
if ps 2>/dev/null | grep -v grep | grep -qiE "$FORBIDDEN_PATTERN"; then
  HITS=$((HITS+1))
  DETAILS="${DETAILS}proc_hit;"
fi
for f in /proc/*/comm; do
  if grep -qiE "wineserver|box64|fex" "$f" 2>/dev/null; then
    HITS=$((HITS+1))
    DETAILS="${DETAILS}comm_hit;"
  fi
done

echo "Scanning image payload..." | tee -a "$LOG"
if [ -d /tests_kernel ]; then
  if grep -R -qiE "$FORBIDDEN_PATTERN" /tests_kernel 2>/dev/null; then
    HITS=$((HITS+1))
    DETAILS="${DETAILS}tests_kernel_hit;"
  fi
fi
if [ -d /bin ]; then
  if ls /bin/* 2>/dev/null | xargs -I{} basename {} 2>/dev/null | grep -qiE "wine|qemu|box64|fex" 2>/dev/null; then
    HITS=$((HITS+1))
    DETAILS="${DETAILS}bin_hit;"
  fi
fi
# check PE strings if strings available
if command -v strings >/dev/null 2>&1; then
  for pe in /tests_kernel/*.exe; do
    if [ -f "$pe" ]; then
      if strings "$pe" 2>/dev/null | grep -qiE "wine|qemu|box64|fex"; then
        HITS=$((HITS+1))
        DETAILS="${DETAILS}pe_strings_hit:$(basename $pe);"
      fi
    fi
  done
fi

echo "Scanning build graph on share..." | tee -a "$LOG"
# simple check: look for forbidden includes in share arch if available
if [ -d "$SHARE/arch/x86/nt" ]; then
  COUNT=$(grep -R -i "wine" "$SHARE/arch/x86/nt" 2>/dev/null | wc -l)
  # filter out our own provenance comment lines
  REAL=$(grep -R -i "wine" "$SHARE/arch/x86/nt" 2>/dev/null | grep -v "provenance" | grep -v "FORBIDDEN" | wc -l)
  if [ "$REAL" -gt 0 ]; then
    # if any real include found, count as hit (should be 0)
    if grep -R -i "include.*wine" "$SHARE/arch/x86/nt" 2>/dev/null | grep -q .; then
      HITS=$((HITS+1))
      DETAILS="${DETAILS}build_graph_hit;"
    fi
  fi
fi
if [ -f "$SHARE/vm/initramfs.cpio.gz" ]; then
  echo "initramfs exists" | tee -a "$LOG"
fi

if command -v dpkg >/dev/null 2>&1; then
  if dpkg -l 2>/dev/null | grep -qiE "wine|qemu-user|box64|fex"; then
    HITS=$((HITS+1))
    DETAILS="${DETAILS}dpkg_hit;"
  fi
fi

echo "hits=$HITS details=$DETAILS" | tee -a "$LOG"

STATUS="FAIL"
if [ "$HITS" -eq 0 ]; then STATUS="PASS"; fi

HASH="none"
if command -v sha256sum >/dev/null 2>&1; then
  HASH=$(echo "$HITS$DETAILS" | sha256sum | cut -d' ' -f1)
fi

cat > "$OUT" <<EOF
{
  "test": "provenance",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "hits": $HITS,
  "details": "$DETAILS",
  "hash": "$HASH",
  "forbidden_pattern": "$FORBIDDEN_PATTERN",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi
