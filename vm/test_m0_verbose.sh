#!/bin/sh
set -x
echo "=== M0 verbose ==="
ls -l /tests_kernel/minimal* /tests_kernel/minimal.exe 2>&1 | cat
echo "--- running minimal.exe ---"
timeout 5 /tests_kernel/minimal.exe; echo "exit:$?"
echo "--- dmesg ---"
dmesg | tail -n 200
echo "--- done ---"
# Also check PEB via helper if available
timeout 5 /tmp/peb_walk.exe 2>&1 | cat || true
dmesg | grep -E "Ldr|PEB|binfmt" | tail -n 50
echo "--- second run minimal0 ---"
timeout 5 /tests_kernel/minimal0.exe; echo "exit2:$?"
dmesg | grep -E "Ldr|PEB" | tail -n 50
# Save result
if [ "$(timeout 5 /tests_kernel/minimal.exe; echo $?)" = "42" ]; then echo "M0 PASS"; echo 42 > /mnt/share/.dodresult/result.exitcode; exit 0; else echo "FAIL"; exit 1; fi
