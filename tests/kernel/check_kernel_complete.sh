#!/bin/bash
# check_kernel_complete.sh - host-side guard against truncated NT Makefile
# Runs nm vmlinux and fails if expected NT symbols are missing.
# This turns silent truncation (STATUS_NOT_IMPLEMENTED) into immediate error.
# Fixed: uses actual emitted symbol names (nt_svc_Nt*), avoids SIGPIPE via temp file,
#        resolves .config and built-in.a relative to vmlinux location, checks counts.
set -euo pipefail
VMLINUX="${1:-vmlinux}"
if [ ! -f "$VMLINUX" ]; then
  # Try common locations
  for cand in ~/ntpers/linux-6.18/vmlinux ./vmlinux /root/ntpers/linux-6.18/vmlinux; do
    if [ -f "$cand" ]; then VMLINUX="$cand"; break; fi
  done
fi
if [ ! -f "$VMLINUX" ]; then
  echo "ERROR: vmlinux not found at $VMLINUX" >&2
  exit 1
fi
# Derive kernel directory from vmlinux location for .config and built-in.a
KDIR="$(dirname "$VMLINUX")"
# Fallback: if KDIR does not contain .config, try ~/ntpers/linux-6.18 and cwd
if [ ! -f "$KDIR/.config" ]; then
  for d in ~/ntpers/linux-6.18 /root/ntpers/linux-6.18 .; do
    if [ -f "$d/.config" ]; then KDIR="$d"; break; fi
  done
fi
echo "Checking NT symbols in $VMLINUX (KDIR=$KDIR) ..."
# Expected symbols as actually emitted. Nt* services are dispatched via nt_svc_Nt* (weak aliases)
# not bare Nt* (except NtAllocateVirtualMemory which has both). We check the nt_svc_ name.
expected=(
  "do_nt_syscall"
  "nt_service_table"
  "nt_enter_personality"
  "nt_exit_personality"
  "ObCreateHandle"
  "ObReferenceObjectByHandle"
  "nt_create_event"
  "se_access_check"
  "nt_svc_NtAllocateVirtualMemory"
  "nt_svc_NtCreateFile"
  "nt_svc_NtCreateIoCompletion"
  "nt_svc_NtCreateMutant"
  "nt_svc_NtQueryInformationFile"
  "nt_banner_init"
)
missing=()
# Use temp file to avoid pipefail SIGPIPE (grep -q exits early causing nm SIGPIPE=141)
NM_TMP=$(mktemp /tmp/check_nm.XXXXXX)
trap 'rm -f "$NM_TMP" /tmp/check_ar.txt' EXIT
nm "$VMLINUX" 2>/dev/null > "$NM_TMP" || true
for sym in "${expected[@]}"; do
  if ! grep -F -q "$sym" "$NM_TMP"; then
      echo "MISSING: $sym" >&2
      missing+=("$sym")
  else
    echo "OK: $sym"
  fi
done
# Additional count check: expect ~355 nt_svc_* symbols
NT_SVC_COUNT=$(grep -c "nt_svc_" "$NM_TMP" || true)
# Some nm versions emit W or T, we count occurrence of nt_svc_ string
if [ -z "$NT_SVC_COUNT" ]; then NT_SVC_COUNT=0; fi
echo "nt_svc_* count: $NT_SVC_COUNT (expected >=350)"
if [ "$NT_SVC_COUNT" -lt 350 ]; then
  echo "MISSING: nt_svc_* count $NT_SVC_COUNT < 350 (expected 355)" >&2
  missing+=("nt_svc_count:$NT_SVC_COUNT")
fi
# Also check that built-in.a contains expected objects if CONFIG_NT_PERSONALITY=y
CONFIG_FILE="$KDIR/.config"
BUILTIN_A="$KDIR/arch/x86/nt/built-in.a"
if grep -q "CONFIG_NT_PERSONALITY=y" "$CONFIG_FILE" 2>/dev/null; then
  echo "CONFIG_NT_PERSONALITY=y - checking $BUILTIN_A objects..."
  if [ -f "$BUILTIN_A" ]; then
    ar t "$BUILTIN_A" 2>/dev/null > /tmp/check_ar.txt || true
    OBJ_COUNT=$(wc -l < /tmp/check_ar.txt 2>/dev/null | tr -d ' ' || echo 0)
    echo "built-in.a object count: $OBJ_COUNT (expected 17)"
    if [ "$OBJ_COUNT" -lt 17 ]; then
      echo "MISSING OBJECTS: built-in.a has $OBJ_COUNT objects, expected 17" >&2
      missing+=("built-in.a:count:$OBJ_COUNT")
    fi
    for obj in dispatch.o proc_thread.o mm.o handle.o kshim.o iocp.o object.o sync.o security.o nt_obj.o nt_io.o ntstatus_kernel.o nt_services.o debugfs_tests.o stubs.o weak.o kernel_stubs.o; do
      if ! grep -F -q "$obj" /tmp/check_ar.txt; then
        echo "MISSING OBJECT in built-in.a: $obj" >&2
        missing+=("built-in.a:$obj")
      else
        echo "OK object: $obj"
      fi
    done
  else
    echo "WARNING: $BUILTIN_A not found, skipping object check (kernel not built here)" >&2
  fi
else
  echo "CONFIG_NT_PERSONALITY not enabled or .config not found at $CONFIG_FILE - skipping built-in.a check"
fi
if [ ${#missing[@]} -ne 0 ]; then
  echo "ERROR: NT kernel incomplete - missing ${#missing[@]} symbols/objects: ${missing[*]}" >&2
  echo "This usually means arch/x86/nt/Makefile was truncated." >&2
  echo "Serial log banner would also be missing: check dmesg for NT personality banner" >&2
  exit 1
fi
echo "PASS: All NT symbols present - kernel complete"
exit 0
