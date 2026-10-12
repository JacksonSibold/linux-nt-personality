#!/bin/bash
# golden_latest.sh - idempotent creation of tests/differential/golden/latest symlink
# Creates symlink latest -> 26200.9168 in WSL if missing, does nothing if present.
# Must not copy 27 MB and must not touch golden contents.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GOLDEN_ROOT="$PROJECT_ROOT/tests/differential/golden"
PINNED="26200.9168"
LATEST="$GOLDEN_ROOT/latest"

if [ -e "$LATEST" ] || [ -L "$LATEST" ]; then
  TARGET="$(readlink "$LATEST" 2>/dev/null || echo "dir")"
  echo "latest already present: $LATEST -> $TARGET (no action)"
  exit 0
fi

if [ ! -d "$GOLDEN_ROOT/$PINNED" ]; then
  echo "FAIL: pinned golden dir $GOLDEN_ROOT/$PINNED missing - cannot create symlink" >&2
  exit 1
fi

# Create symlink (relative target keeps it repo-portable)
ln -s "$PINNED" "$LATEST"
echo "created symlink $LATEST -> $PINNED"
