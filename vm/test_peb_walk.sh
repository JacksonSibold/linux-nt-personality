#!/bin/sh
set -x
echo "=== PEB Ldr walk test ==="
# Copy peb_walk exe from share
cp /mnt/share/scratch/peb_walk.exe /tmp/peb_walk.exe
chmod +x /tmp/peb_walk.exe
ls -l /tmp/peb_walk.exe
ls -l /tmp/ntdll.dll /tmp/kernel32.dll
echo "--- running peb_walk (with imports) ---"
timeout 5 /tmp/peb_walk.exe; echo "PEB walk exit:$?"
# Also test zero-import minimal still passes
echo "--- running minimal0 (zero-import) ---"
timeout 5 /tests_kernel/minimal0.exe; echo "minimal0 exit:$?"
# Also try to run peb_walk via direct path for bonus: check serial for Ldr init
dmesg | tail -n 100
echo "--- done ---"
# Save output
timeout 5 /tmp/peb_walk.exe > /tmp/peb_out.txt 2>&1; echo "second run exit:$?"
cat /tmp/peb_out.txt > /mnt/share/.dodresult/peb_walk_output.txt
cat /tmp/peb_out.txt
# Determine success: need count >=2
if grep -q "PASS" /tmp/peb_out.txt; then echo "PEB PASS"; echo 42 > /mnt/share/.dodresult/result.exitcode; exit 0; else echo "PEB FAIL"; cat /tmp/peb_out.txt; echo 1 > /mnt/share/.dodresult/result.exitcode; exit 1; fi
