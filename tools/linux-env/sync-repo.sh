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
# GUEST_DEST is required. It names a private directory under the guest home. The old default
# (ntpers-research) is the shared working copy that other agents use, and a sync into it
# overwrites their files. Refuse it unless GUEST_DEST_FORCE=1 is set on purpose.
DEST="${GUEST_DEST:-}"
if [ -z "$DEST" ]; then
    echo "sync-repo.sh: GUEST_DEST is required (a private directory name under ~, e.g. GUEST_DEST=pathreg)" >&2
    exit 2
fi
case "$DEST" in
    */*|*..*|/*|.*|"") echo "sync-repo.sh: GUEST_DEST must be a plain directory name under ~, got '$DEST'" >&2; exit 2 ;;
esac
if [ "$DEST" = "ntpers-research" ] && [ "${GUEST_DEST_FORCE:-0}" != "1" ]; then
    echo "sync-repo.sh: refusing GUEST_DEST=ntpers-research (the shared guest copy). Use a private name," >&2
    echo "  or set GUEST_DEST_FORCE=1 if you really mean to overwrite it." >&2
    exit 2
fi
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
