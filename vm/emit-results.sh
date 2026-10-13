#!/bin/sh
# vm/emit-results.sh - guest side of the initrd-serial transport.
#
# Usage (inside the guest, run by /init after the test script):
#   /emit-results.sh [SHARE_DIR]          (default SHARE_DIR is /mnt/share)
#
# Prints every regular file under SHARE_DIR/.dodresult to stdout, which is the
# serial console, as frames that tools/vm_serial_collect.py verifies on the host:
#
#   @@NTPERS-EMIT-BEGIN@@
#   @@NTPERS-FILE .dodresult/<name> <size> <sha256>@@
#   <base64 body, 76 columns>
#   @@END@@
#   ...
#   @@NTPERS-EMIT-END@@
#
# Paths are relative to SHARE_DIR. A file whose name contains whitespace cannot be
# framed (the header is space separated); it is reported on the console and skipped.
# Only busybox applets are used: base64, sha256sum, wc, find, sort, cut, tr, sync.
set -u

SHARE="${1:-/mnt/share}"
RESULTS=".dodresult"

echo "@@NTPERS-EMIT-BEGIN@@"
if ! cd "$SHARE" 2>/dev/null; then
    echo "emit: cannot enter $SHARE"
    echo "@@NTPERS-EMIT-END@@"
    exit 1
fi

if [ -d "$RESULTS" ]; then
    find "$RESULTS" -type f | sort | while IFS= read -r f; do
        case "$f" in
            *[[:space:]]*)
                echo "emit: skipped (whitespace in name): $f"
                continue ;;
        esac
        size=$(wc -c < "$f" | tr -d ' ')
        sha=$(sha256sum "$f" | cut -d ' ' -f 1)
        echo "@@NTPERS-FILE $f $size $sha@@"
        base64 "$f"
        echo "@@END@@"
    done
fi

sync
echo "@@NTPERS-EMIT-END@@"
exit 0
