#!/usr/bin/env bash
# One-time guest provisioning over the serial console. Run after: start-vm.ps1 -Shell (boots to root bash).
# Creates user ntpers (locked password, SSH key only, passwordless sudo), SSH host keys, a DHCP
# systemd-networkd unit (cloud-init would normally write this; it never runs on this image), and
# enables systemd-resolved. Then syncs. Shut the guest down afterwards with stop-vm.ps1 (or monitor 'quit').
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LINUX_ENV="${LINUX_ENV:-C:/Users/Jackson/repos/linux-env}"
PUB="$(cat "$LINUX_ENV/keys/id_ed25519.pub")"
CMDS="$LINUX_ENV/run/provision-cmds.txt"
mkdir -p "$LINUX_ENV/run"

cat > "$CMDS" <<EOF
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev
useradd -m -s /bin/bash -G sudo ntpers
install -d -m 700 -o ntpers -g ntpers /home/ntpers/.ssh
echo '$PUB' > /home/ntpers/.ssh/authorized_keys
chown ntpers:ntpers /home/ntpers/.ssh/authorized_keys; chmod 600 /home/ntpers/.ssh/authorized_keys
echo 'ntpers ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/ntpers; chmod 440 /etc/sudoers.d/ntpers
visudo -c
ssh-keygen -A
printf '[Match]\nName=en* eth*\n\n[Network]\nDHCP=yes\n' > /etc/systemd/network/10-dhcp.network
ln -sf /lib/systemd/system/systemd-resolved.service /etc/systemd/system/multi-user.target.wants/systemd-resolved.service
ssh-keygen -lf /home/ntpers/.ssh/authorized_keys
sync
EOF

# Verify the key line landed intact: the serial console has garbled typed text before, so check the fingerprint after.
powershell -NoProfile -ExecutionPolicy Bypass -File "$HERE/serial-cmd.ps1" -InFile "$CMDS" -WaitMs 2000 -ReadSeconds 3 | tr -d '\r'
echo "expect the last fingerprint to match keys/id_ed25519.pub"
ssh-keygen -lf "$LINUX_ENV/keys/id_ed25519.pub"
