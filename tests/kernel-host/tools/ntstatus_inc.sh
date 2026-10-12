#!/bin/sh
# ntstatus_inc.sh INPUT OUTPUT - emit the NTSTATUS->DOS entry lines that
# src/kernel/nt/gen_ntstatus_table.py writes to ntstatus_table.inc.
# Fallback for hosts without python3: same table body, same entry lines.
set -e
awk '/NtStatusToDosErrorTable\[\]/ { f = 1; next } f && /^};/ { exit } f' "$1" |
  sed -n -E 's/^[[:space:]]*(\{.*\},?)[[:space:]]*$/    \1/p' > "$2"
