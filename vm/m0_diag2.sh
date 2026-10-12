#!/bin/sh
# DEPRECATED: broken copy of vm/diag_m0.sh with its variables stripped (mkdir -p with no
# argument, empty [ -x ] test, RC=True, exit with no status). Not repaired: the intended
# output paths and exit policy cannot be recovered unambiguously. Use vm/diag_m0.sh instead.
# See docs/vm/CLEANUP.md (section 1). Do not run.
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
