#!/usr/bin/env bash
# Diagnose where the M0 PE dies. Called by the master during integration.
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ" || exit 1

LOG=$(ls -t .dodresult/serial-*.log 2>/dev/null | head -1)
echo "=== serial log: $LOG ==="
if [ -n "$LOG" ]; then
    grep -inE 'binfmt_pe|segfault|Ldr|nt console|general protection|RIP|traps|minimal' "$LOG" | tail -40
fi

echo
echo "=== M0 test PE entry bytes (should be a raw NtTerminateProcess stub) ==="
if [ -f tests/kernel/bin/minimal.exe ]; then
    ls -la tests/kernel/bin/minimal.exe
    x86_64-w64-mingw32-objdump -p tests/kernel/bin/minimal.exe 2>/dev/null | grep -E 'ImageBase|AddressOfEntryPoint|SizeOfImage|DLL Name' | head
fi

echo
echo "=== does minimal.exe import anything? (zero imports expected) ==="
x86_64-w64-mingw32-objdump -p tests/kernel/bin/minimal.exe 2>/dev/null | grep -c 'DLL Name' || echo 0
