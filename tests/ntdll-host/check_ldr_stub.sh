#!/usr/bin/env bash
# check_ldr_stub.sh - fail when LdrLoadDll in a built ntdll.dll is still the
# MinGW stub (decision 0029, review LDR-1). The stub is a short body that
# returns STATUS_NOT_SUPPORTED (0xc00000bb). The real loader is much longer.
#
#   tests/ntdll-host/check_ldr_stub.sh [ntdll.dll]
#
# Build-only: reads the disassembly with objdump and never runs the DLL.
# Exit codes: 0 real loader, 1 stub still linked, 2 usage or missing tool/file.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DLL="${1:-$ROOT/src/ntdll/out/ntdll.dll}"

command -v objdump >/dev/null 2>&1 || { echo "check_ldr_stub: objdump not found" >&2; exit 2; }
[ -f "$DLL" ] || { echo "check_ldr_stub: no DLL at $DLL (run make -C src/ntdll first)" >&2; exit 2; }

# Body of LdrLoadDll: from its label to the next blank line.
body="$(objdump -d "$DLL" | tr -d '\r' \
  | awk '/^[0-9a-f]+ <LdrLoadDll>:$/ { on = 1; next } on && /^$/ { exit } on { print }')"

if [ -z "$body" ]; then
  echo "check_ldr_stub: LdrLoadDll not found in $DLL" >&2
  exit 2
fi

# Count real instructions only: the linker pads functions with nop forms.
insns="$(printf '%s\n' "$body" | grep ':' | grep -vc -E 'nop|xchg +%ax,%ax|int3')"
if [ "$insns" -le 3 ] && printf '%s\n' "$body" | grep -q 'c00000bb'; then
  echo "check_ldr_stub: FAIL LdrLoadDll is the STATUS_NOT_SUPPORTED stub ($insns instructions)"
  exit 1
fi
echo "check_ldr_stub: OK LdrLoadDll is not the stub ($insns instructions)"
exit 0
