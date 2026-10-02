#!/bin/sh
set -eu
SHARE=/mnt/share
RESULT_DIR=$SHARE/.dodresult
mkdir -p $RESULT_DIR
echo "=== M0 diag ==="
PE=/tests_kernel/minimal.exe
if [ ! -x $PE ]; then PE=/tests_kernel/minimal0.exe; fi
echo "Running $PE"
set +e
$PE
RC=$?
set -e
echo "PE exit RC=$RC"
dmesg | tail -n 300
echo $RC > $SHARE/.dodresult/result.exitcode
dmesg | tail -n 300 > $SHARE/.dodresult/m0_diag_dmesg.log 2>/dev/null || true
if [ $RC -eq 42 ]; then exit 0; else exit $RC; fi
