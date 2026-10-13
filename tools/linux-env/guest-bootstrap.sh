#!/bin/sh
# Installs build dependencies inside the Debian 12 guest. Run as user ntpers (sudo is passwordless).
# Usage (from host): tools/linux-env/ssh-guest.sh 'sh -s' < tools/linux-env/guest-bootstrap.sh
set -eu
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential gcc make bc flex bison libssl-dev libelf-dev \
    busybox-static cpio gzip xz-utils git python3 \
    rsync ca-certificates curl pkg-config dwarves
echo "--- versions"
gcc --version | head -1
make --version | head -1
busybox | head -1
python3 --version
git --version
