#!/bin/sh
cat /sys/kernel/debug/ntpers/dispatch_per_ssn 2>&1 | head -n 50
echo "--- dispatch count ---"
cat /sys/kernel/debug/ntpers/dispatch_count 2>&1 | head
