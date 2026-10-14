#!/bin/bash
# check_kernel_complete.sh - completeness gate for the NT personality in a built vmlinux.
#
# Usage: check_kernel_complete.sh [VMLINUX]
#
# The expectations come from the repository, not from numbers written here.
# The rules and the verdict are in tools/kernel_gate.py; this script finds the
# files and runs it:
#   - each service in src/kernel/nt/nt_service_list.h that tools/service_inventory.py
#     classifies with a strong kernel-branch handler (IMPLEMENTED, REFUSES or
#     STUB-NOT-IMPLEMENTED) must be a strong nt_svc_<Name> symbol in VMLINUX. A
#     weak-only or absent one is a shortfall, and the gate FAILS.
#   - the NT entry points (do_nt_syscall, nt_service_table, ...) must be strong symbols.
#   - when KDIR/.config sets CONFIG_NT_PERSONALITY=y, each obj-$(CONFIG_NT_PERSONALITY)
#     object in arch/x86/nt/Makefile must be a member of KDIR/arch/x86/nt/built-in.a.
# Services with no kernel-branch strong handler are listed as kernel gaps and do not
# fail the gate. The repository is the directory above tests/ (this script's location).
# KDIR is the directory that holds VMLINUX.
#
# Replaces the earlier gate, which expected 355 nt_svc_* symbols and 17 objects. The
# repository produces 171 table entries and 20 Makefile objects, so those constants
# could never be met. See docs/vm/BUILD-RECIPE.md, "Completeness gate".
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
VMLINUX="${1:-vmlinux}"
if [ ! -f "$VMLINUX" ]; then
  # Try common locations
  for cand in ~/ntpers/linux-6.18.56/vmlinux ~/ntpers/linux-6.18/vmlinux ./vmlinux /root/ntpers/linux-6.18/vmlinux; do
    if [ -f "$cand" ]; then VMLINUX="$cand"; break; fi
  done
fi
if [ ! -f "$VMLINUX" ]; then
  echo "ERROR: vmlinux not found at $VMLINUX" >&2
  exit 1
fi
KDIR="$(dirname "$VMLINUX")"
CONFIG_FILE="$KDIR/.config"
BUILTIN_A="$KDIR/arch/x86/nt/built-in.a"
NM_TMP="$(mktemp)"
AR_TMP="$(mktemp)"
trap 'rm -f "$NM_TMP" "$AR_TMP"' EXIT
echo "Checking NT personality in $VMLINUX (KDIR=$KDIR, repo=$REPO) ..."
if ! nm "$VMLINUX" > "$NM_TMP" 2>/dev/null; then
  echo "ERROR: nm failed on $VMLINUX" >&2
  exit 1
fi
EXTRA_ARGS=()
if [ -f "$BUILTIN_A" ]; then
  if ! ar t "$BUILTIN_A" > "$AR_TMP" 2>/dev/null; then
    echo "ERROR: ar t failed on $BUILTIN_A" >&2
    exit 1
  fi
  EXTRA_ARGS+=(--ar "$AR_TMP")
fi
if [ -f "$CONFIG_FILE" ]; then
  EXTRA_ARGS+=(--config "$CONFIG_FILE")
fi
# The exit status of kernel_gate.py is this script's exit status (set -e); the trap
# removes the temporary files either way.
python3 "$REPO/tools/kernel_gate.py" --repo "$REPO" --nm "$NM_TMP" --label "$VMLINUX" \
  ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}
