#!/bin/sh
set -x
echo "=== test minimal0 expect 0 ==="
timeout 5 /tests_kernel/minimal0.exe
RC0=$?
echo "minimal0 RC=$RC0"
dmesg | tail -n 100
echo "=== test minimal expect 42 ==="
timeout 5 /tests_kernel/minimal.exe
RC1=$?
echo "minimal RC=$RC1"
dmesg | tail -n 100
echo "=== dmesg grep ==="
dmesg | grep -i binfmt | tail -n 50
dmesg | grep -i ntpers | tail -n 50
dmesg | grep -i segfault | tail -n 50
if [ $RC0 -eq 0 ]; then echo "minimal0 PASS"; else echo "minimal0 FAIL got $RC0"; fi
if [ $RC1 -eq 42 ]; then echo "minimal PASS"; else echo "minimal FAIL got $RC1"; fi
