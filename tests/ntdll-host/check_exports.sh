#!/usr/bin/env bash
# check_exports.sh - compare the export names of a built ntdll.dll with its .def.
#
#   tests/ntdll-host/check_exports.sh [ntdll.dll] [ntdll.def]
#
# Build-only: reads the PE export table with objdump and never loads or runs
# the DLL (Windows Application Control blocks fresh binaries, MAIN-1).
# Not run by CI. Invoked by `make -C src/ntdll check`.
#
# Exit codes: 0 names match, 1 mismatch, 2 usage or missing tool/file.
# The comparison is exact: no allow-list. Known findings in the .def itself
# (for example the x86 decorated name _DllMainCRTStartup@12, DEF-1) are
# reported in docs/modules/ntdll-syscalls.md, not hidden here.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DLL="${1:-$ROOT/src/ntdll/out/ntdll.dll}"
DEF="${2:-$ROOT/src/ntdll/out/ntdll.def}"

command -v objdump >/dev/null 2>&1 || { echo "check_exports: objdump not found" >&2; exit 2; }
[ -f "$DLL" ] || { echo "check_exports: no DLL at $DLL (run make -C src/ntdll first)" >&2; exit 2; }
[ -f "$DEF" ] || { echo "check_exports: no .def at $DEF" >&2; exit 2; }

tmp="$(mktemp -d)" || exit 2
trap 'rm -rf "$tmp"' EXIT

# DLL side: name-table lines look like "[  2] +base[  3]  0002 DllMain".
# Address-table lines end in "Export RVA" or "Forward To", so they do not match.
objdump -p "$DLL" \
  | tr -d '\r' \
  | awk '/^[[:space:]]*\[[[:space:]]*[0-9]+\][[:space:]]+\+base\[[[:space:]]*[0-9]+\][[:space:]]+[0-9a-fA-F]{4}[[:space:]]+[A-Za-z_@][A-Za-z_@0-9.]*[[:space:]]*$/ { print $NF }' \
  | sort -u > "$tmp/dll.txt"

# .def side: "LIBRARY", "EXPORTS", blank and ';' comment lines are skipped.
# For "Exported=Internal" the exported name is the left side.
tr -d '\r' < "$DEF" \
  | sed -e 's/;.*$//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
  | awk -F= 'NF && !/^(LIBRARY|EXPORTS)([[:space:]]|$)/ { print $1 }' \
  | sort -u > "$tmp/def.txt"

if [ ! -s "$tmp/dll.txt" ]; then
  echo "check_exports: no export names read from $DLL" >&2
  exit 2
fi

comm -23 "$tmp/def.txt" "$tmp/dll.txt" > "$tmp/def_only.txt"
comm -13 "$tmp/def.txt" "$tmp/dll.txt" > "$tmp/dll_only.txt"

fail=0
while IFS= read -r name; do
  [ -n "$name" ] || continue
  echo "check_exports: FAIL in .def but not exported: $name"
  fail=1
done < "$tmp/def_only.txt"

while IFS= read -r name; do
  [ -n "$name" ] || continue
  echo "check_exports: FAIL exported but not in .def: $name"
  fail=1
done < "$tmp/dll_only.txt"

dll_n=$(wc -l < "$tmp/dll.txt" | tr -d ' ')
def_n=$(wc -l < "$tmp/def.txt" | tr -d ' ')
if [ "$fail" -eq 0 ]; then
  echo "check_exports: OK ($dll_n exported names match $def_n .def names)"
  exit 0
fi
echo "check_exports: FAILED ($dll_n exported, $def_n in .def)"
exit 1
