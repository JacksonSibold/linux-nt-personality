#!/bin/sh
set -x
cp /mnt/share/build/probes/r3_minimal.exe.patched /tmp/r3_test.exe
chmod +x /tmp/r3_test.exe
timeout 5 /tmp/r3_test.exe > /tmp/r3_out.txt 2>&1
RC=$?
cat /tmp/r3_out.txt
echo "RC=$RC"
dmesg | tail -n 20
echo $RC > /mnt/share/.dodresult/result.exitcode
if [ $RC -eq 42 ]; then exit 0; else exit 1; fi
