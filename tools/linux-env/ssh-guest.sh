#!/usr/bin/env bash
# SSH helper for the guest (QEMU user-net forward 127.0.0.1:2222 -> guest:22).
# Usage: tools/linux-env/ssh-guest.sh [command...]
set -euo pipefail
LINUX_ENV="${LINUX_ENV:-C:/Users/Jackson/repos/linux-env}"
PORT="${GUEST_SSH_PORT:-2222}"
exec ssh -i "$LINUX_ENV/keys/id_ed25519" -p "$PORT" \
    -o IdentitiesOnly=yes \
    -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile="$LINUX_ENV/keys/known_hosts" \
    -o ServerAliveInterval=30 \
    ntpers@127.0.0.1 "$@"
