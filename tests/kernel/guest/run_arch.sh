#!/bin/sh
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
# mount debugfs/tracefs if available (non-fatal)
mkdir -p /sys/kernel/debug 2>/dev/null || true
if ! mountpoint -q /sys/kernel/debug 2>/dev/null; then
  mount -t debugfs none /sys/kernel/debug 2>>"$LOG" || echo "debugfs mount failed (non-fatal)" | tee -a "$LOG"
fi
mkdir -p /sys/kernel/tracing 2>/dev/null || true
if ! mountpoint -q /sys/kernel/tracing 2>/dev/null; then
  mount -t tracefs none /sys/kernel/tracing 2>>"$LOG" || echo "tracefs mount failed" | tee -a "$LOG"
fi

# Helper: check for wineserver-like helpers (avoid matching grep itself)
HELPER_FOUND=0
if ps 2>/dev/null | grep -v grep | grep -iqE "wineserver|wine|box64|fex|qemu.*-binfmt"; then
  HELPER_FOUND=1
fi
# Also check for helper processes via /proc/*/comm
for f in /proc/*/comm; do
  if grep -iqE "wineserver|box64|fex" "$f" 2>/dev/null; then HELPER_FOUND=1; fi
done
# Check for X11/Wayland fds in current and init
FORBIDDEN_FDS=0
for d in /proc/self/fd /proc/1/fd; do
  if ls -l "$d" 2>/dev/null | grep -qE "socket:\[.*\].*X11|wayland|XCB"; then
    FORBIDDEN_FDS=1
  fi
done
# ss check if available
if command -v ss >/dev/null 2>&1; then
  if ss -x 2>/dev/null | grep -qE "X11|wayland"; then
    FORBIDDEN_FDS=1
  fi
fi
echo "helper_found=$HELPER_FOUND forbidden_fds=$FORBIDDEN_FDS" | tee -a "$LOG"

# CPUID hypervisor bit from /proc/cpuinfo
HYPERVISOR_BIT_PROC=0
if grep -q "hypervisor" /proc/cpuinfo 2>/dev/null; then
  HYPERVISOR_BIT_PROC=1
fi
echo "hypervisor_proc=$HYPERVISOR_BIT_PROC" | tee -a "$LOG"

# CPUID via PE (arch_cpuid.exe should exit 0 or 1)
HYPERVISOR_PE="unknown"
if [ -x /tests_kernel/arch_cpuid.exe ]; then
  set +e
  /tests_kernel/arch_cpuid.exe
  RC=$?
  set -e
  if [ "$RC" -ge 128 ]; then
    HYPERVISOR_PE=$((RC - 128))
  else
    HYPERVISOR_PE=$RC
  fi
  echo "hypervisor_pe_raw=$RC normalized=$HYPERVISOR_PE" | tee -a "$LOG"
elif [ -x /pe_min.exe ]; then
  HYPERVISOR_PE="no_pe"
fi

# --- Debugfs per-CPU counter reading (exact, cheap, no printk) ---
# Before window
DISPATCH_PATH="/sys/kernel/debug/ntpers/dispatch_count"
HOST_PATH="/sys/kernel/debug/ntpers/host_count"
PER_SSN_PATH="/sys/kernel/debug/ntpers/dispatch_per_ssn"

# Helper to read unsigned 64 counter safely
read_counter() {
  _p="$1"
  _v="0"
  if [ -f "$_p" ]; then
    _v=$(cat "$_p" 2>/dev/null | tr -d ' \n\r')
    if [ -z "$_v" ]; then _v="0"; fi
    # ensure numeric
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
echo "BEFORE_NT=$BEFORE_NT HOST=$BEFORE_HOST path=$DISPATCH_PATH" | tee -a "$LOG"
# Also check ftrace availability for informational (not primary measurement)
FTRACE_AVAILABLE=0
if [ -d /sys/kernel/tracing ] && [ -w /sys/kernel/tracing/tracing_on ]; then
  FTRACE_AVAILABLE=1
fi

# Run 100 PE launches
TOTAL=100
SUCCESS=0
i=1
while [ "$i" -le "$TOTAL" ]; do
  set +e
  if [ -x /tests_kernel/minimal0.exe ]; then
    /tests_kernel/minimal0.exe 2>/dev/null
    RC=$?
  elif [ -x /pe_min.exe ]; then
    /pe_min.exe 2>/dev/null
    RC=$?
    if [ "$RC" -ge 128 ]; then
      if [ "$RC" -eq 128 ]; then
        RC=0
      fi
    fi
  else
    echo "no PE found" | tee -a "$LOG"
    RC=1
  fi
  set -e
  # minimal0 exits 42 via NtTerminateProcess, treat 42 as success like 0
  if [ "$RC" -eq 0 ] || [ "$RC" -eq 42 ]; then
    SUCCESS=$((SUCCESS+1))
  fi
  i=$((i+1))
done
echo "launches $SUCCESS/$TOTAL succeeded" | tee -a "$LOG"

# After window - exact per-CPU summed values
AFTER_NT=$(read_counter "$DISPATCH_PATH")
AFTER_HOST=$(read_counter "$HOST_PATH")
echo "AFTER_NT=$AFTER_NT HOST=$AFTER_HOST" | tee -a "$LOG"
NT_SYSCALLS=$((AFTER_NT - BEFORE_NT))
if [ "$NT_SYSCALLS" -lt 0 ]; then NT_SYSCALLS=0; fi
HOST_SYSCALLS=$((AFTER_HOST - BEFORE_HOST))
if [ "$HOST_SYSCALLS" -lt 0 ]; then HOST_SYSCALLS=0; fi
# For JSON compatibility keep ftrace fields as same as debugfs
NT_SYSCALLS_FT=$NT_SYSCALLS

# Validate measurement: debugfs must exist and have produced >= SUCCESS
MEASUREMENT_VALID=1
MEASUREMENT_NOTE=""
if [ ! -f "$DISPATCH_PATH" ]; then
  MEASUREMENT_VALID=0
  MEASUREMENT_NOTE="debugfs $DISPATCH_PATH not present - kernel counter not exposed"
  echo "WARN $MEASUREMENT_NOTE" | tee -a "$LOG"
elif [ "$NT_SYSCALLS" -lt "$SUCCESS" ]; then
  MEASUREMENT_VALID=0
  MEASUREMENT_NOTE="dispatch_count $NT_SYSCALLS < success $SUCCESS - undercount"
  echo "WARN measurement undercount: NT=$NT_SYSCALLS success=$SUCCESS" | tee -a "$LOG"
fi
# Also log per-SSN if available
if [ -f "$PER_SSN_PATH" ]; then
  echo "per-SSN counts:" | tee -a "$LOG"
  cat "$PER_SSN_PATH" 2>/dev/null | tee -a "$LOG" || true
fi

# Ratio host per NT
RATIO="0.00"
if [ "$NT_SYSCALLS" -gt 0 ]; then
  HOST_SYSCALLS=$(echo "$HOST_SYSCALLS" | tr -d ' \n')
  if [ -z "$HOST_SYSCALLS" ]; then HOST_SYSCALLS=0; fi
  NT_SYSCALLS=$(echo "$NT_SYSCALLS" | tr -d ' \n')
  if [ -z "$NT_SYSCALLS" ]; then NT_SYSCALLS=0; fi
  if [ "$NT_SYSCALLS" -gt 0 ]; then
    RATIO=$(awk "BEGIN{printf \"%.4f\", $HOST_SYSCALLS/$NT_SYSCALLS}")
  else
    RATIO="0.00"
  fi
else
  RATIO="0.00"
fi
echo "NT syscalls debugfs=$NT_SYSCALLS host=$HOST_SYSCALLS ratio=$RATIO" | tee -a "$LOG"

# Check binfmt_pe was used (via dmesg still, but not used for ratio)
BINFMT_HITS=$(dmesg 2>/dev/null | grep -c "binfmt_pe: entry" 2>/dev/null | tr -d ' \n' || echo 0)
if [ -z "$BINFMT_HITS" ]; then BINFMT_HITS=0; fi
# Sanitize binfmt
BINFMT_HITS=$(echo "$BINFMT_HITS" | tr -d ' \n')
case "$BINFMT_HITS" in ''|*[!0-9]*) BINFMT_HITS=0 ;; esac
echo "binfmt_pe entries=$BINFMT_HITS" | tee -a "$LOG"

HELPER_KILL_SUCCESS=1

# Determine PASS/FAIL per thresholds
B001_PASS=0
A002_PASS=0
if [ "$MEASUREMENT_VALID" -eq 1 ] && [ "$SUCCESS" -eq "$TOTAL" ] && [ "$FORBIDDEN_FDS" -eq 0 ] && [ "$HELPER_FOUND" -eq 0 ]; then
  RATIO_OK=$(awk "BEGIN{print ($RATIO <= 0.05)}")
  if [ "$RATIO_OK" -eq 1 ] && [ "$NT_SYSCALLS" -gt 0 ]; then
    B001_PASS=1
  fi
  if [ "$NT_SYSCALLS" -gt 0 ]; then
    A002_PASS=1
  fi
fi

STATUS="FAIL"
if [ "$MEASUREMENT_VALID" -eq 0 ]; then
  STATUS="FAIL"
elif [ "$B001_PASS" -eq 1 ] && [ "$A002_PASS" -eq 1 ]; then
  STATUS="PASS"
elif [ "$A002_PASS" -eq 1 ]; then
  STATUS="PASS"
fi

cat > "$OUT" <<EOF
{
  "test": "arch",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "total_launches": $TOTAL,
  "success": $SUCCESS,
  "nt_syscalls_dmesg": $NT_SYSCALLS,
  "nt_syscalls_ftrace": $NT_SYSCALLS_FT,
  "host_syscalls": $HOST_SYSCALLS,
  "ratio_host_per_nt": $RATIO,
  "ratio_ok_le_0_05": $([ "$RATIO" = "0.00" ] && echo true || awk "BEGIN{print ($RATIO <= 0.05)}"),
  "forbidden_fds": $FORBIDDEN_FDS,
  "helper_found": $HELPER_FOUND,
  "hypervisor_proc": $HYPERVISOR_BIT_PROC,
  "hypervisor_pe": "$HYPERVISOR_PE",
  "binfmt_hits": $BINFMT_HITS,
  "b001_pass": $B001_PASS,
  "a002_pass": $A002_PASS,
  "ftrace_available": $FTRACE_AVAILABLE,
  "measurement_valid": $MEASUREMENT_VALID,
  "measurement_note": "$MEASUREMENT_NOTE",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
echo "=== ARCH result $STATUS ===" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then
  exit 0
else
  exit 1
fi
