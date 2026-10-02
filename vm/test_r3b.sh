#!/bin/sh
set -x
echo "=== R3 import test b ==="
cp /mnt/share/build/probes/r3_minimal.exe /tmp/r3_test.exe
chmod +x /tmp/r3_test.exe
ls -l /tmp/r3_test.exe
echo "--- running R3 directly ---"
/tmp/r3_test.exe
RC=$?
echo "R3 direct exit:$RC"
echo "--- dmesg ---"
dmesg | tail -n 20
echo "$RC" > /mnt/share/.dodresult/result.exitcode
if [ "$RC" -eq 42 ]; then echo "PASS"; exit 0; else echo "FAIL expected 42 got $RC"; exit 1; fi
