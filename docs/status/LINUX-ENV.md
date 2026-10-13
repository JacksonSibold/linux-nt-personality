# Linux guest on Windows 11 (QEMU, no elevation)

Status: **working**. A Debian 12 (bookworm) x86_64 guest boots under QEMU for Windows, is reachable over SSH,
and has the build dependencies for the NT-personality kernel. The NT kernel (Linux 6.18.56) is **built** in the guest
and **boots to the M0 check** (exit 42) under a second host QEMU run with `-kernel`. See
`docs/status/KERNEL-BOOT.md` for the result and the gaps that remain. Host tooling lives outside the repo in `C:\Users\Jackson\repos\linux-env`; only scripts and
docs are committed under `tools/linux-env/` and `docs/status/`. No keys or images are in the repo.

## Reaching the guest

```
C:/Users/Jackson/repos/ntpers-linuxenv/tools/linux-env/ssh-guest.sh
# or directly:
ssh -i C:/Users/Jackson/repos/linux-env/keys/id_ed25519 -p 2222 -o IdentitiesOnly=yes \
    -o UserKnownHostsFile=C:/Users/Jackson/repos/linux-env/keys/known_hosts ntpers@127.0.0.1
```

User `ntpers` (uid 1000, groups `sudo`). Password login is disabled (locked password); key only.
Passwordless sudo. Guest project copy: `~/ntpers-research` (synced with `tools/linux-env/sync-repo.sh`).

Fingerprint of the client key authorized in the guest (`ssh-keygen -lf keys/id_ed25519.pub`):
`SHA256:xQ1XzOYlKBUe4LrSQjz+nfxcG/xywqGfYZSYtCZH1qQ`.

## Survey (read-only, before any install)

| Item | Result |
|---|---|
| `wsl -l -v` | "The Windows Subsystem for Linux is not installed." Not installed; not attempted (rule: no `wsl --install`). |
| `qemu-system-x86_64` on PATH | not found |
| `winget --version` | v1.29.380 |
| docker | not installed |
| VirtualBox / VMware | not installed |
| `Win32_ComputerSystem.HypervisorPresent` | True |
| `Win32_Processor.VirtualizationFirmwareEnabled` | False (as reported by WMI; not changed) |
| CPU | Intel Xeon E5-2660 v2 (no nested acceleration verified) |
| `qemu -accel help` (after install) | `tcg`, `whpx` compiled in |

Windows Hypervisor Platform was not checked further (no feature changes allowed). WHPX was tried once with the
cloud-image GRUB boot and produced no console output in 7 minutes, so it is **not verified**. Everything below
runs on **TCG** (pure emulation).

## Download log

All downloads were checked against a published checksum. Each lives in its own directory under
`C:\Users\Jackson\repos\linux-env\downloads\`. `tools/linux-env/fetch-artifacts.sh` repeats these steps.

| File | Source URL | Size (bytes) | SHA256 | Verified against | Use |
|---|---|---|---|---|---|
| `QEMU_11.1.0_Machine_X64_nullsoft_en-US.exe` (= `qemu-w64-setup-20260811.exe`) | https://qemu.weilnetz.de/w64/2026/qemu-w64-setup-20260811.exe (winget `SoftwareFreedomConservancy.QEMU` 11.1.0) | 206,615,928 | `f98a8aeb5f7faea9765b6dee28316c266cd179d80354a2fed8e50176f9a2e59f` | winget manifest `InstallerSha256` | **Not executed** (see Blockers). Unpacked with 7-Zip into `linux-env\qemu\` |
| `7zr.exe` | https://www.7-zip.org/a/7zr.exe | 602,624 | `256feca8e274e5da655e2a284fabafd9f554365eb164862089dacd4e8276d282` | none published on the page; recorded | Extracts the 7-Zip extra package |
| `7z2604-extra.7z` | https://www.7-zip.org/a/7z2604-extra.7z | 1,764,616 | `dc4b11d3399db18b063630137145f5585d8f7ac847bf3639bd1185d7d1f7cee0` | none published on the page; recorded | Provides `7za.exe` |
| `7z2604-x64.msi` | https://www.7-zip.org/a/7z2604-x64.msi (winget `7zip.7zip` 26.04) | 2,009,088 | `0b01334a418654293513449f61d6bfc99e5196ca371e3c3dc961eda57cd535c6` | none published on the page; recorded | **Not installed**. Administrative extraction with `7za` only; gives full `7z.exe`/`7z.dll` (NSIS reader) |
| `debian-12-genericcloud-amd64.qcow2` | https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-genericcloud-amd64.qcow2 | 349,896,704 | `9ebb87ba8e3e0ab593e39cb89fed15dda05e5997b575285a7fae4ac4bc42ad20` | SHA512 `a09170d17e13af43da61774666f520e3bbec0f8bd96b4db37c15f3426fb327cc...` from `SHA512SUMS` (OK) | Guest disk base (overlay `disk\ntpers-guest.qcow2`, 25G) |
| `linux-image-6.1.0-53-cloud-amd64-unsigned_6.1.187-1_amd64.deb` | https://deb.debian.org/debian-security/pool/updates/main/l/linux/linux-image-6.1.0-53-cloud-amd64-unsigned_6.1.187-1_amd64.deb | 26,565,380 | `9001721c6ba33401f95bbeeb0e41ec91ce2cdb5aaceac6133cb9d093734bba0e` | `bookworm-security` `Packages.xz` SHA256 (OK) | Source of `vmlinuz-6.1.0-53-cloud-amd64` (kernel) and the virtio `.ko` modules |
| `busybox-static_1.35.0-4+deb12u1+b1_amd64.deb` | https://deb.debian.org/debian/pool/main/b/busybox/busybox-static_1.35.0-4+deb12u1+b1_amd64.deb | 928,188 | `3d3fdbe91d4660c873e14b092c213fe81c1da6362daa236eb25d0171eb108744` | bookworm `Packages.xz` SHA256 (OK) | Static busybox for the initramfs |

Kernel source for the NT build (downloaded inside the guest to `~/ntpers/`, 2026-10-13):

| File | Source URL | Size (bytes) | SHA256 | Verified against | Use |
|---|---|---|---|---|---|
| `sha256sums.asc` | https://cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc | 253,076 | (signed file, not hashed here) | PGP signature: **not verified**, see below | Source of the tarball hash |
| `linux-6.18.56.tar.xz` | https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.56.tar.xz | 154,807,564 | `4633bbda2ab95eaf2a7af4041236451d6759c72e6946265531af4b3aacefbf89` | `sha256sums.asc` line for `linux-6.18.56.tar.xz` (match) | Newest 6.18.y in the checksum file; extracted to `~/ntpers/linux-6.18.56` (905 MB) |
| `signatures.html` | https://www.kernel.org/category/signatures.html | 19,036 | (not hashed) | none (page with fingerprints) | Source of the fingerprints below |
| `k.asc` (key B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1) | https://keys.openpgp.org/vks/v1/by-fingerprint/B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1 | 854 | (not hashed) | none | Key that signed `sha256sums.asc`; see gap |

Signature status. Keys for Linus Torvalds (`ABAF 11C6 5A29 70B1 30AB E3C4 79BE 3E43 0041 1886`) and Greg
Kroah-Hartman (`647F 2865 4894 E3BD 4571 99BE 38DB BDC8 6092 693E`) were fetched over the kernel.org WKD and
their fingerprints match the page. `sha256sums.asc` is signed by key `B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1`
(RSA 4096, 2013, no user ID on keys.openpgp.org). That key has no certification from either verified key in
the copy fetched, so `gpg --verify` reports "No public key" and the signature is **not verified**. The
tarball hash was checked against the file's line only. Open: confirm B8868C80 belongs to a kernel.org release
signer (out of band) and re-run `gpg --verify sha256sums.asc`.

Local artifacts (generated, not downloaded): `linux-env\keys\id_ed25519{,.pub}` (ed25519, no passphrase,
generated with `ssh-keygen`), `linux-env\disk\ntpers-guest.qcow2` (overlay), `linux-env\disk\work.qcow2`
(30G, created but not used), `linux-env\seed.iso` (cloud-init seed, not effective, see below),
`linux-env\initrd\initrd.cpio` (built from the repo's `init` plus the downloads above).

## Blockers and what was done instead

1. **QEMU winget installer needs administrator.** The official installer embeds
   `requestedExecutionLevel level="requireAdministrator"`, and the winget manifest says `Scope: machine`.
   Running it would trigger a UAC prompt, so it was **not run**. This is not an Application Control block
   (no rc=126). Workaround: `7z.exe` (full 7-Zip, from the MSI payload) listed and unpacked the NSIS payload
   into `linux-env\qemu\` (3,389 files). `qemu-system-x86_64.exe --version` runs: 11.1.0.
   The standalone `7za.exe` cannot read NSIS, so the full build was needed.
2. **cloud-init never ran on the guest image.** The GRUB boot of the image produced a login prompt, but no
   cloud-init units appear in the boot log, `ssh.service` failed four times (no host keys), and the `ntpers`
   user was never created. The seed ISO (volume `CIDATA`, written with Windows `tar --format iso9660`) had no
   effect. Its GRUB config (timeout 0) gave no way to edit the kernel command line.
   Workaround: boot the image's own kernel with a small initramfs (`tools/linux-env/initrd/`), which loads
   the virtio modules and switch_roots into the image root. The root shell (`ntpers.shell=bash`) then runs
   `tools/linux-env/provision.sh` over the serial console to create the user, key, sudoers entry, host keys,
   a DHCP networkd unit, and enable systemd-resolved.
3. **Serial console input is unreliable for long lines.** Typed characters were occasionally doubled or
   dropped (`chmood`, a stray `C` in the key). Every key was verified afterwards with `ssh-keygen -lf`
   and `visudo -c`. `provision.sh` ends with the same fingerprint check.
4. **9p share is not available.** `vm/runvm.sh` uses `-virtfs local` and KVM; neither exists in QEMU for Windows.
   The replacement is `tools/linux-env/sync-repo.sh` (tar over SSH into `~/ntpers-research`). The VM scripts
   (`vm/runvm.sh`, `vm/kbuild.sh`) are still Linux-host scripts and were not run.
5. **Repository check finding (pre-existing, not changed here).** Running the project's
   `tools/check_repo.py` in the guest reports `FAIL [crlf-mix] tests/kernel-host/Makefile: 232 CRLF lines and
   5 bare-LF lines`. This is the committed blob at `HEAD`, not a checkout artifact.

## Scripts (`tools/linux-env/`)

| Script | Runs on | Purpose |
|---|---|---|
| `fetch-artifacts.sh` | Git Bash | Downloads, verifies, and unpacks the artifacts above (no installers executed) |
| `initrd/build-initrd.sh` (+ `initrd/init`, `initrd/fixmode.js`) | Git Bash + node | Builds `initrd.cpio`; `fixmode.js` sets the exec bit that Windows `tar` drops |
| `start-vm.ps1 [-Shell]` | PowerShell | Starts QEMU (TCG), creates the overlay, forwards SSH to 2222, serial TCP 5555, monitor TCP 4444 |
| `serial-cmd.ps1 -InFile f` | PowerShell | Sends commands to the serial console and prints the output |
| `provision.sh` | Git Bash | One-time guest setup over serial (run after `start-vm.ps1 -Shell`) |
| `stop-vm.ps1` | PowerShell | `system_powerdown` via the monitor, waits, then kills QEMU |
| `ssh-guest.sh [cmd]` | Git Bash | SSH helper for the guest |
| `sync-repo.sh [dir]` | Git Bash | Copies a checkout (no `.git`) into `~/ntpers-research` |
| `guest-bootstrap.sh` | guest | Installs build dependencies (`ssh-guest.sh 'sh -s' < guest-bootstrap.sh`) |

Start the guest (normal boot), then wait about a minute for SSH:

```
powershell -ExecutionPolicy Bypass -File tools\linux-env\start-vm.ps1
tools\linux-env\ssh-guest.sh 'uname -a'
```

Stop it: `powershell -ExecutionPolicy Bypass -File tools\linux-env\stop-vm.ps1`.

Re-provision from scratch (only if the overlay is deleted): `start-vm.ps1 -Shell`, then `provision.sh`,
then `stop-vm.ps1`.

## Timings (measured)

| Step | Time |
|---|---|
| QEMU, 7-Zip, Debian image, kernel, busybox downloads | each completed within a single tool call; SHA checks pass |
| Boot to systemd targets (TCG, 4 vCPU, 4 GB) | about 17 s to `basic.target`; SSH answers within about 1 minute of start |
| Guest dependency install (`guest-bootstrap.sh`, apt) | **9 min 18 s** |
| `sync-repo.sh` (601 files, 8.7 MB) | 12.7 s |
| `gcc` smoke test (C hello) | immediate |

Expected performance for the kernel build is **not measured**. Under TCG the guest runs roughly an order of
magnitude slower than native KVM, so a full kernel build should be expected to take hours, not minutes.
Measure it before relying on it.

## Dependencies installed in the guest

Debian 12 with `build-essential` (gcc 12.2.0-14, GNU Make 4.3), `bc`, `flex`, `bison`, `libssl-dev`,
`libelf-dev`, `busybox-static` (1.35.0), `cpio`, `git` (2.39.5), `python3` (3.11.2), `rsync`, `dwarves`,
`pkg-config`, `xz-utils`, `curl`, `ca-certificates`. The `qemu-system-x86` package was **not** installed, as
specified. Kernel source is not present yet.

## Not done

- Done since this file was written: the kernel.org tarball is downloaded and logged above, extracted to
  `~/ntpers/linux-6.18.56`, patched by `tools/ntpers_kpatch.py`, configured and built (bzImage), and the
  M0 gate ran on a 9p-free path (`tools/linux-env/m0-initramfs.sh`, `tools/linux-env/boot-m0-host.ps1`).
  Remaining: the repo's completeness gate fails and the `vm/` 9p scripts are unused. See
  `docs/status/KERNEL-BOOT.md`.
- `runvm.sh` and the 9p share do not work on QEMU for Windows. Options: an SSH-driven boot test, or a guest
  build/run inside this VM with the results copied back over `scp`.
- WHPX acceleration is not verified; `-accel tcg` is what runs.
- `docs/status/LINUX-ENV.md` itself was written by hand from the commands that were run. The helper scripts
  under `tools/linux-env/` were built from those commands. The full `fetch-artifacts.sh` run was not repeated
  end to end.
- Git: the branch `haiku/linuxenv` in `C:\Users\Jackson\repos\ntpers-linuxenv` has the scripts and this doc
  committed. Nothing was pushed.
