#!/bin/sh
set -eu
PE="/tests_kernel/sync_atomic.exe"
ls -l "$PE"
dmesg -c > /tmp/d 2>&1 || true
set +e
timeout 30 "$PE"
RC=$?
set -e
RC_MASKED=$((RC & 0xFF))
echo "sync_atomic exit $RC masked $RC_MASKED"
dmesg | tail -n 200
echo "$RC_MASKED" > /mnt/share/.dodresult/result.exitcode
if [ "$RC_MASKED" -eq 0 ]; then exit 0; else exit "$RC_MASKED"; fi
