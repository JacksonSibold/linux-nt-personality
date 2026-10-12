#!/usr/bin/env bash
# Build the PE test corpus with mingw-w64 and dump structural facts
# that the native PE loader must handle.
set -euo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="/tmp/pecorpus"
mkdir -p "$OUT"

cp "$PROJ/tests/conformance/pe_smoke.c" "$OUT/pe_smoke.c"
sed -i 's/\r$//' "$OUT/pe_smoke.c"

cd "$OUT"

echo "=== BUILD: dynamic CRT ==="
x86_64-w64-mingw32-gcc -O2 -o pe_smoke.exe pe_smoke.c

echo "=== BUILD: static, no CRT startup ==="
x86_64-w64-mingw32-gcc -O2 -static -o pe_smoke_static.exe pe_smoke.c

echo "=== BUILD: export-forwarder DLL ==="
cp "$PROJ/tests/conformance/pe_fwd.c" "$OUT/pe_fwd.c"
cp "$PROJ/tests/conformance/pe_fwd.def" "$OUT/pe_fwd.def"
sed -i 's/\r$//' "$OUT/pe_fwd.c" "$OUT/pe_fwd.def"
x86_64-w64-mingw32-gcc -O2 -shared -o pe_fwd.dll pe_fwd.c pe_fwd.def

ls -la ./*.exe ./*.dll 2>/dev/null || true

echo
echo "=== file(1) ==="
file ./*.exe

echo
echo "=== objdump -f (entry point / arch) ==="
objdump -f pe_smoke.exe

echo
echo "=== sections ==="
objdump -h pe_smoke.exe

echo
echo "=== imported DLLs (what the loader must resolve) ==="
objdump -p pe_smoke.exe | grep -i "DLL Name" || true

echo
echo "=== static build imports ==="
objdump -p pe_smoke_static.exe | grep -i "DLL Name" || true

echo
echo "=== DOS/PE magic bytes ==="
xxd -l 64 pe_smoke.exe
