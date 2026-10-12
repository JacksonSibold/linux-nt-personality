#!/bin/bash
# preflight.sh - verify tree is sane BEFORE any agent starts work
# Checks: .config present with required options, Makefile has all objects,
# no duplicate strong symbols across arch/x86/nt/*.c, and tree builds (optional quick check)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KDIR="${KDIR:-$HOME/ntpers/linux-6.18}"

echo "=== preflight: NT personality tree sanity check ==="
echo "KDIR=$KDIR"
echo "PROJECT_ROOT=$PROJECT_ROOT"

FAIL=0

# 1. .config present with required options
echo "--- Checking .config ---"
if [ ! -f "$KDIR/.config" ]; then
  echo "FAIL: $KDIR/.config missing (make clean removed it?)" >&2
  FAIL=1
else
  echo "OK: .config present"
  required=(
    "CONFIG_NT_PERSONALITY=y"
    "CONFIG_BINFMT_PE=y"
    "CONFIG_VIRTIO=y"
    "CONFIG_VIRTIO_BLK=y"
    "CONFIG_VIRTIO_PCI=y"
    "CONFIG_NET_9P=y"
    "CONFIG_NET_9P_VIRTIO=y"
    "CONFIG_9P_FS=y"
    "CONFIG_DEVTMPFS=y"
    "CONFIG_DEVTMPFS_MOUNT=y"
    "CONFIG_SERIAL_8250_CONSOLE=y"
    "CONFIG_BLK_DEV_INITRD=y"
    "CONFIG_SECCOMP=y"
    "CONFIG_AUDIT=y"
    "CONFIG_DEBUG_FS=y"
    "CONFIG_DYNAMIC_DEBUG=y"
  )
  for opt in "${required[@]}"; do
    if ! grep -q "^$opt" "$KDIR/.config"; then
      echo "FAIL: $opt missing in .config" >&2
      FAIL=1
    else
      echo "OK: $opt"
    fi
  done
fi

# 2. arch/x86/nt/Makefile has all objects
echo "--- Checking arch/x86/nt/Makefile ---"
MF="$KDIR/arch/x86/nt/Makefile"
if [ ! -f "$MF" ]; then
  echo "FAIL: $MF missing" >&2
  FAIL=1
else
  expected=(
    "dispatch.o"
    "proc_thread.o"
    "mm.o"
    "handle.o"
    "kshim.o"
    "iocp.o"
    "object.o"
    "sync.o"
    "security.o"
    "nt_obj.o"
    "nt_io.o"
    "ntstatus_kernel.o"
    "nt_services.o"
    "debugfs_tests.o"
    "stubs.o"
    "weak.o"
    "kernel_stubs.o"
  )
  for obj in "${expected[@]}"; do
    if ! grep -q "$obj" "$MF"; then
      echo "FAIL: Makefile missing $obj (truncated?)" >&2
      FAIL=1
    else
      echo "OK: $obj in Makefile"
    fi
  done
  count=$(grep -c "obj-" "$MF" || true)
  echo "Makefile obj lines: $count (expected 17)"
  if [ "$count" -lt 17 ]; then
    echo "FAIL: Makefile has only $count objects, expected 17" >&2
    FAIL=1
  fi
fi

# 3. No duplicate strong symbols across arch/x86/nt/*.c (using nm on objects)
echo "--- Checking for duplicate strong symbols (nm) ---"
# Build NT objects if needed to have .o files
# Try to build just the nt directory quickly
if [ ! -f "$KDIR/arch/x86/nt/built-in.a" ]; then
  echo "NOTE: built-in.a not found, attempting to build arch/x86/nt objects..."
  # This will compile the NT objects if needed
  (cd "$KDIR" && make arch/x86/nt/built-in.a -j"$(nproc)" 2>&1 | tail -n 20) || echo "build attempt done"
fi

# Use nm on built objects to find duplicates
# We check for g_kshim_root as an example, but generally find any strong symbol appearing in >1 object
if compgen -G "$KDIR/arch/x86/nt/*.o" > /dev/null; then
  echo "Scanning objects for duplicate strong symbols..."
  TMP=$(mktemp)
  TMP2=$(mktemp)
  trap 'rm -f "$TMP" "$TMP2"' EXIT
  # List all global symbols (T, D, B) from each object
  for o in "$KDIR"/arch/x86/nt/*.o; do
    nm -g "$o" 2>/dev/null | awk '{print $3}' | sort >> "$TMP" || true
  done
  sort "$TMP" | uniq -c | sort -nr > "$TMP2"
  # Find duplicates (count >1) that are not weak (nm would show V/W for weak)
  # For strong duplicates, check nm -g without weak filter
  # More precise: list symbols with type T/D/B/R (strong) and find duplicates
  TMP3=$(mktemp)
  for o in "$KDIR"/arch/x86/nt/*.o; do
    nm -g --defined-only "$o" 2>/dev/null | grep -E " [TDBR] " | awk '{print $3}' >> "$TMP3" || true
  done
  sort "$TMP3" | uniq -d > "$TMP"
  if [ -s "$TMP" ]; then
    echo "FAIL: Duplicate strong symbols found:" >&2
    cat "$TMP" >&2
    # Show which objects contain the duplicate
    for sym in $(cat "$TMP"); do
      echo "Symbol $sym in:" >&2
      for o in "$KDIR"/arch/x86/nt/*.o; do
        if nm -g --defined-only "$o" 2>/dev/null | grep -q " $sym$"; then
          echo "  $o" >&2
        fi
      done
    done
    FAIL=1
  else
    echo "OK: No duplicate strong symbols"
  fi
  # Specific check for g_kshim_root
  if grep -q "g_kshim_root" "$TMP" 2>/dev/null; then
    echo "FAIL: g_kshim_root duplicate (would cause /tmp vs / bug)" >&2
    FAIL=1
  fi
  rm -f "$TMP" "$TMP2" "$TMP3"
else
  echo "WARNING: No .o files found to check for duplicates (build not done)" >&2
  # Don't fail yet, but warn
fi

# 4. Tree builds (quick check: try to build vmlinux or at least check for previous successful build)
echo "--- Checking tree builds ---"
if [ ! -f "$KDIR/vmlinux" ]; then
  echo "WARNING: vmlinux not found, tree has not been built yet" >&2
  echo "Attempting quick build check (arch/x86/nt/built-in.a)..."
  if ! (cd "$KDIR" && make arch/x86/nt/built-in.a -j"$(nproc)" 2>&1 | tail -n 20); then
    echo "FAIL: arch/x86/nt failed to build" >&2
    FAIL=1
  else
    echo "OK: arch/x86/nt builds"
  fi
else
  # Check vmlinux is recent (within last day) and has NT symbols
  # Use temp file to avoid SIGPIPE (grep -q exits early causing nm SIGPIPE=141 with pipefail)
  TMPNM=$(mktemp)
  nm "$KDIR/vmlinux" 2>/dev/null > "$TMPNM" || true
  if ! grep -q "do_nt_syscall" "$TMPNM"; then
    echo "FAIL: vmlinux exists but missing NT symbols (not built with NT personality?)" >&2
    FAIL=1
  else
    echo "OK: vmlinux exists and has NT symbols"
  fi
  rm -f "$TMPNM"
fi

if [ $FAIL -ne 0 ]; then
  echo "=== preflight: FAILED ===" >&2
  exit 1
else
  echo "=== preflight: PASS ==="
  exit 0
fi
