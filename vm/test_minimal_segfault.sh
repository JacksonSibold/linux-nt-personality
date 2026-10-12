#!/bin/sh
set -x
echo "=== minimal segfault debug ==="
ls -l /tests_kernel/minimal.exe
timeout 5 /tests_kernel/minimal.exe; echo "minimal exit:$?"
echo "--- dmesg tail ---"
dmesg | tail -n 100
echo "--- done ---"
echo "0" > /mnt/share/.dodresult/result.exitcode
