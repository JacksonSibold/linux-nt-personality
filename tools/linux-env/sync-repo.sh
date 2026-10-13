#!/usr/bin/env bash
# Copies a checkout (working tree, without .git) into the guest at ~/ntpers-research.
# Replaces the 9p share used by vm/runvm.sh, which QEMU for Windows cannot provide.
# Usage: tools/linux-env/sync-repo.sh [repo-dir]   (default: the repo containing this script)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-$(cd "$HERE/../.." && pwd)}"
DEST="${GUEST_DEST:-ntpers-research}"
"$HERE/ssh-guest.sh" "mkdir -p ~/$DEST"
tar -C "$SRC" --exclude=.git -cf - . | "$HERE/ssh-guest.sh" "tar -C ~/$DEST -xf -"
echo "synced $SRC -> guest:~/$DEST"
