#!/usr/bin/env bash
# Copies a checkout (working tree, without .git) into the guest at ~/ntpers-research.
# Replaces the 9p share used by vm/runvm.sh, which QEMU for Windows cannot provide.
# Usage: tools/linux-env/sync-repo.sh [repo-dir]   (default: the repo containing this script)
#
# A Windows checkout with core.autocrlf=true stores text files as LF in git but
# writes them with CRLF. A Linux checkout has LF, and a CRLF shell script fails
# in the guest ("set: Illegal option -"). Files git stores as LF but the working
# tree holds as CRLF are converted back to LF in a staging copy before the copy.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-$(cd "$HERE/../.." && pwd)}"
DEST="${GUEST_DEST:-ntpers-research}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
"$HERE/ssh-guest.sh" "mkdir -p ~/$DEST"
tar -C "$SRC" --exclude=.git -cf - . | tar -C "$STAGE" -xf -
if git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1; then
    # ls-files --eol prints "i/<eol> w/<eol> attr/... <TAB> <path>".
    git -C "$SRC" ls-files --eol -c | awk -F'\t' '$1 ~ /^i\/lf/ && $1 ~ /w\/crlf/ {print $2}' |
    while IFS= read -r path; do
        sed -i 's/\r$//' "$STAGE/$path"
    done
fi
tar -C "$STAGE" -cf - . | "$HERE/ssh-guest.sh" "tar -C ~/$DEST -xf -"
echo "synced $SRC -> guest:~/$DEST"
