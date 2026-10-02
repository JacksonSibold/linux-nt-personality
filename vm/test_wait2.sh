#!/bin/sh
set -x
cp /mnt/share/build/probes/r3_minimal.exe /tmp/r3_test.exe
chmod +x /tmp/r3_test.exe
cp /mnt/share/build/wait_test /tmp/wait_test
chmod +x /tmp/wait_test
/tmp/wait_test /tmp/r3_test.exe
RC=$?
echo "wait_test exit:$RC"
echo "$RC" > /mnt/share/.dodresult/result.exitcode
# Also test direct
/tmp/r3_test.exe
echo "direct exit:$?"
# Check with wait_test for minimal
cp /mnt/share/tests/kernel/bin/minimal.exe /tmp/min.exe
chmod +x /tmp/min.exe
/tmp/wait_test /tmp/min.exe
echo "wait_test min exit:$?"
dmesg | tail -n 50
echo "done"
echo "0" > /mnt/share/.dodresult/result.exitcode
