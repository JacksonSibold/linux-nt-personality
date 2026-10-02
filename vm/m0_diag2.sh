#!/bin/sh
set -eu
SHARE=/mnt/share
RESULT_DIR=/.dodresult
mkdir -p 
echo === M0 diag ===
PE=/tests_kernel/minimal.exe
if [ ! -x  ]; then PE=/tests_kernel/minimal0.exe; fi
echo Running 
set +e

RC=True
set -e
echo PE exit RC=
dmesg | tail -n 200
echo  > /.dodresult/result.exitcode
cp /tmp/m0.log /.dodresult/m0_diag.log 2>/dev/null || true
if [  -eq 42 ]; then exit 0; else exit ; fi
