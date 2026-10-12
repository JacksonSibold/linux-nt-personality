#!/bin/sh
set -x
cp /mnt/share/tests/differential/build/probe_path.exe /tmp/probe_path.exe
chmod +x /tmp/probe_path.exe
timeout 5 /tmp/probe_path.exe > /tmp/probe_out.txt 2>&1 &
PID=$!
sleep 2
dmesg | tail -n 50
wait $PID
RC=$?
cat /tmp/probe_out.txt
echo "RC=$RC"
dmesg | tail -n 50
echo $RC > /mnt/share/.dodresult/result.exitcode
exit $RC
