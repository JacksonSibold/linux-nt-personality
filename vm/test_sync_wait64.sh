#!/bin/sh
set -eu
SHARE="/mnt/share"
RESULT_DIR="$SHARE/.dodresult"
mkdir -p "$RESULT_DIR"
PE="/tests_kernel/sync_wait64.exe"
echo "PE=$PE"
ls -l "$PE"
dmesg -c > /tmp/dmesg_cleared 2>&1 || true
set +e
timeout 30 "$PE"
RC=$?
set -e
RC_MASKED=$((RC & 0xFF))
echo "exit $RC masked $RC_MASKED"
dmesg | tail -n 200
echo "$RC_MASKED" > "$SHARE/.dodresult/result.exitcode"
if [ "$RC_MASKED" -eq 0 ]; then exit 0; else exit "$RC_MASKED"; fi
