#!/bin/sh
set -eu
export PATH="/usr/bin:/bin:/usr/local/bin:$PATH"
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu:/usr/lib:/lib:${LD_LIBRARY_PATH:-}"
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
RUN_ID_FILE="$SHARE/.dodresult/run_id_pmu"
RUN_ID=$(cat "$RUN_ID_FILE" 2>/dev/null | tr -d '\r\n' || echo "")
if [ -z "$RUN_ID" ]; then RUN_ID="missing"; fi
OUT="$RESULT_DIR/kernel_pmu.json"
LOG="$RESULT_DIR/kernel_pmu.log"
mkdir -p "$RESULT_DIR"
echo "=== PMU fingerprint in-VM ===" | tee "$LOG"
echo "PATH=$PATH" | tee -a "$LOG"
echo "perf which: $(which perf 2>&1 || echo notfound) $(ls -l /usr/bin/perf 2>&1 | head)" | tee -a "$LOG"
PE="/tests_kernel/pmu_probe.exe"
ELF="/tests_kernel/pmu_elf"
# fallback paths
if [ ! -x "$PE" ]; then PE="/tests_kernel/pmu_probe.exe"; fi
if [ ! -x "$ELF" ]; then
  if [ -x "/tests_kernel/pmu_elf" ]; then ELF="/tests_kernel/pmu_elf"; else ELF=""; fi
fi
echo "PE=$PE ELF=$ELF" | tee -a "$LOG"

# Function to run perf stat if available
PERF_BIN="/usr/bin/perf"
if [ ! -x "$PERF_BIN" ]; then PERF_BIN="$(command -v perf 2>/dev/null || echo "")"; fi
run_perf() {
  BIN="$1"
  LABEL="$2"
  OUTP="$RESULT_DIR/perf_${LABEL}.log"
  if [ -n "$PERF_BIN" ] && [ -x "$PERF_BIN" ]; then
    echo "perf available at $PERF_BIN, measuring $BIN" >> "$LOG" 2>&1
    echo "perf available at $PERF_BIN, measuring $BIN" >&2
    set +e
    "$PERF_BIN" stat -e instructions,cycles,branches,branch-misses -x, -o "$OUTP" -- "$BIN" 2>>"$LOG"
    RC=$?
    set -e
    echo "perf $LABEL RC=$RC" >> "$LOG" 2>&1
    echo "perf $LABEL RC=$RC" >&2
    cat "$OUTP" 2>/dev/null >> "$LOG" 2>&1 || true
    cat "$OUTP" 2>/dev/null >&2 || true
    INSTR=$(grep -i "instructions" "$OUTP" 2>/dev/null | head -n1 | cut -d',' -f1 | tr -d ' ' || echo 0)
    CYCLES=$(grep -i "cycles" "$OUTP" 2>/dev/null | grep -v "branch" | head -n1 | cut -d',' -f1 | tr -d ' ' || echo 0)
    # handle <not counted> case
    if echo "$INSTR" | grep -q "not"; then INSTR=0; fi
    if echo "$CYCLES" | grep -q "not"; then CYCLES=0; fi
    if [ -z "$INSTR" ]; then INSTR=0; fi
    if [ -z "$CYCLES" ]; then CYCLES=0; fi
    # strip < > and spaces
    INSTR=$(echo "$INSTR" | tr -d '<>' | tr -d ' ')
    CYCLES=$(echo "$CYCLES" | tr -d '<>' | tr -d ' ')
    if [ -z "$INSTR" ]; then INSTR=0; fi
    if [ -z "$CYCLES" ]; then CYCLES=0; fi
    echo "$INSTR $CYCLES"
  else
    echo "perf not found, fallback" >> "$LOG" 2>&1
    echo "perf not found, fallback" >&2
    set +e
    "$BIN" 2>/dev/null
    RC=$?
    set -e
    echo "0 0"
  fi
}

# If ELF missing, try to build or use shell equivalent
if [ ! -x "$ELF" ]; then
  echo "ELF twin missing, trying to locate or build minimal ELF loop" | tee -a "$LOG"
  # create simple shell loop as fallback for measurement? but we want real ELF
  # search share for pmu_elf
  if [ -f "$SHARE/tests/kernel/bin/pmu_elf" ]; then
    cp "$SHARE/tests/kernel/bin/pmu_elf" /tmp/pmu_elf 2>/dev/null && chmod +x /tmp/pmu_elf && ELF="/tmp/pmu_elf"
  fi
fi

PE_RESULT=$(run_perf "$PE" "pe")
ELF_RESULT=$(run_perf "$ELF" "elf")

PE_INSTR=$(echo "$PE_RESULT" | awk '{print $1}')
PE_CYCLES=$(echo "$PE_RESULT" | awk '{print $2}')
ELF_INSTR=$(echo "$ELF_RESULT" | awk '{print $1}')
ELF_CYCLES=$(echo "$ELF_RESULT" | awk '{print $2}')

echo "PE instr=$PE_INSTR cycles=$PE_CYCLES" | tee -a "$LOG"
echo "ELF instr=$ELF_INSTR cycles=$ELF_CYCLES" | tee -a "$LOG"

# compute ratio if perf numbers available, else mark as no-perf
RATIO="null"
IPC_PE="null"
IPC_ELF="null"
BAND_OK=0
PERF_AVAILABLE=0
if [ -n "$PERF_BIN" ] && [ -x "$PERF_BIN" ]; then
  PERF_AVAILABLE=1
fi
if [ "$PERF_AVAILABLE" -eq 1 ] && [ "$PE_INSTR" != "0" ] && [ "$ELF_INSTR" != "0" ] && [ -n "$PE_INSTR" ] && [ -n "$ELF_INSTR" ]; then
  # numeric check
  RATIO=$(awk "BEGIN{printf \"%.4f\", $PE_INSTR/$ELF_INSTR}" 2>/dev/null || echo "null")
  # IPC = instr / cycles
  if [ "$PE_CYCLES" != "0" ] && [ "$ELF_CYCLES" != "0" ]; then
    IPC_PE=$(awk "BEGIN{printf \"%.4f\", $PE_INSTR/$PE_CYCLES}" 2>/dev/null || echo "null")
    IPC_ELF=$(awk "BEGIN{printf \"%.4f\", $ELF_INSTR/$ELF_CYCLES}" 2>/dev/null || echo "null")
    IPC_RATIO=$(awk "BEGIN{printf \"%.4f\", $IPC_PE/$IPC_ELF}" 2>/dev/null || echo "null")
  else
    IPC_RATIO="null"
  fi
  # check band [0.8,1.25]
  BAND_OK=$(awk "BEGIN{print ($RATIO >=0.8 && $RATIO <=1.25)}" 2>/dev/null || echo 0)
  if [ -z "$BAND_OK" ]; then BAND_OK=0; fi
  echo "ratio=$RATIO IPC_PE=$IPC_PE IPC_ELF=$IPC_ELF IPC_ratio=$IPC_RATIO band_ok=$BAND_OK" | tee -a "$LOG"
else
  echo "perf numbers unavailable, band check skipped" | tee -a "$LOG"
  BAND_OK=0
  RATIO="perf_unavailable"
fi

# Also CPUID hypervisor check
HYP_PROC=0
if grep -q "hypervisor" /proc/cpuinfo 2>/dev/null; then HYP_PROC=1; fi
HYP_PE="unknown"
if [ -x /tests_kernel/arch_cpuid.exe ]; then
  set +e
  /tests_kernel/arch_cpuid.exe
  RC=$?
  set -e
  if [ "$RC" -ge 128 ]; then RC=$((RC-128)); fi; HYP_PE=$RC
fi
echo "hypervisor_proc=$HYP_PROC hypervisor_pe=$HYP_PE" | tee -a "$LOG"

# Determine PASS per B002 PMU band: requires ratio in band on same silicon (we are on same silicon VM)
STATUS="FAIL"
if [ "$BAND_OK" -eq 1 ]; then STATUS="PASS"; else STATUS="FAIL"; fi
# If perf unavailable, we cannot PASS, but we still record evidence
if [ "$PERF_AVAILABLE" -eq 0 ]; then STATUS="FAIL"; fi

cat > "$OUT" <<EOF
{
  "test": "pmu",
  "run_id": "$RUN_ID",
  "status": "$STATUS",
  "perf_available": $PERF_AVAILABLE,
  "pe_instructions": "$PE_INSTR",
  "pe_cycles": "$PE_CYCLES",
  "elf_instructions": "$ELF_INSTR",
  "elf_cycles": "$ELF_CYCLES",
  "ratio_pe_over_elf": "$RATIO",
  "ipc_pe": "$IPC_PE",
  "ipc_elf": "$IPC_ELF",
  "band_ok_0_8_1_25": $BAND_OK,
  "hypervisor_proc": $HYP_PROC,
  "hypervisor_pe": "$HYP_PE",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
cat "$OUT" | tee -a "$LOG"
if [ "$STATUS" = "PASS" ]; then exit 0; else exit 1; fi
