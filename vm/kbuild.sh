#!/bin/bash
# kbuild.sh - ONLY sanctioned way to build the NT personality kernel
# Restores .config if missing, builds, and refuses success unless completeness check passes.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KDIR="${KDIR:-$HOME/ntpers/linux-6.18}"
CONFIG_SRC="$SCRIPT_DIR/kernel-config-ntpers"
CONFIG_DST="$KDIR/.config"
CHECKER="$PROJECT_ROOT/tests/kernel/check_kernel_complete.sh"

echo "=== kbuild: NT personality kernel build ==="
echo "KDIR=$KDIR"
echo "PROJECT_ROOT=$PROJECT_ROOT"

# Restore .config if missing
if [ ! -f "$CONFIG_DST" ]; then
  echo "WARNING: $CONFIG_DST missing - restoring from $CONFIG_SRC"
  if [ ! -f "$CONFIG_SRC" ]; then
    echo "ERROR: No .config and no $CONFIG_SRC to restore" >&2
    exit 1
  fi
  cp "$CONFIG_SRC" "$CONFIG_DST"
  echo "Restored .config"
else
  echo ".config present"
fi

# Verify required options are present
echo "Checking required CONFIG options..."
required=(
  "CONFIG_NT_PERSONALITY=y"
  "CONFIG_BINFMT_PE=y"
  "CONFIG_VIRTIO=y"
  "CONFIG_VIRTIO_BLK=y"
  "CONFIG_VIRTIO_PCI=y"
  "CONFIG_NET_9P=y"
  "CONFIG_NET_9P_VIRTIO=y"
  "CONFIG_9P_FS=y"
  "CONFIG_DEVTMPFS=y"
  "CONFIG_DEVTMPFS_MOUNT=y"
  "CONFIG_SERIAL_8250_CONSOLE=y"
  "CONFIG_BLK_DEV_INITRD=y"
  "CONFIG_SECCOMP=y"
  "CONFIG_AUDIT=y"
  "CONFIG_DEBUG_FS=y"
  "CONFIG_DYNAMIC_DEBUG=y"
)
missing=0
for opt in "${required[@]}"; do
  if ! grep -q "^$opt" "$CONFIG_DST"; then
    echo "MISSING: $opt" >&2
    missing=1
  else
    echo "OK: $opt"
  fi
done
if [ $missing -ne 0 ]; then
  echo "ERROR: .config missing required options" >&2
  exit 1
fi

# --- Hardening: restore NT sources if missing (so `make clean` never loses them) ---
# Canonical NT sources live in $PROJECT_ROOT/src/kernel/nt, arch/x86/nt, src/ntdll.
# KDIR is a plain 6.18 tarball extraction; we copy/symlink NT sources in.
# This makes `make clean` (which deletes *.o, built-in.a) unable to delete .c files,
# and makes `make mrproper` recoverable via this restore.
restore_nt_sources() {
  local restored=0
  if [ ! -f "$KDIR/arch/x86/nt/dispatch.c" ] || [ ! -f "$KDIR/arch/x86/nt/proc_thread.c" ] || [ ! -f "$KDIR/fs/binfmt_pe.c" ]; then
    echo "WARNING: NT sources missing in KDIR - restoring from project..."
    mkdir -p "$KDIR/arch/x86/nt" "$KDIR/arch/ntdll" "$KDIR/fs"
    # arch/x86/nt: copy from project arch/x86/nt (tracked) and src/kernel/nt (canonical)
    if [ -d "$PROJECT_ROOT/arch/x86/nt" ]; then
      cp -a "$PROJECT_ROOT/arch/x86/nt/." "$KDIR/arch/x86/nt/" 2>/dev/null || true
      restored=1
    fi
    if [ -d "$PROJECT_ROOT/src/kernel/nt" ]; then
      cp -a "$PROJECT_ROOT/src/kernel/nt/." "$KDIR/arch/x86/nt/" 2>/dev/null || true
      restored=1
    fi
    # Ensure stubs/kernel_stubs exist (not tracked, but required for 17 objects)
    if [ ! -f "$KDIR/arch/x86/nt/stubs.c" ]; then
      : > "$KDIR/arch/x86/nt/stubs.c"
      echo "Created stubs.c"
    fi
    if [ ! -f "$KDIR/arch/x86/nt/kernel_stubs.c" ]; then
      cat > "$KDIR/arch/x86/nt/kernel_stubs.c" <<'EOS'
#include <linux/kernel.h>
void nt_banner_init(void){pr_info("NT personality: ntpers v1.0 banner\n");}
EOS
      echo "Created kernel_stubs.c"
    fi
    # fs/binfmt_pe.c
    if [ -f "$PROJECT_ROOT/src/kernel/fs/binfmt_pe.c" ]; then
      cp "$PROJECT_ROOT/src/kernel/fs/binfmt_pe.c" "$KDIR/fs/binfmt_pe.c"
      restored=1
    fi
    # arch/ntdll headers for sync.h ../../ntdll
    mkdir -p "$KDIR/arch/ntdll"
    if [ -d "$PROJECT_ROOT/src/ntdll" ]; then
      cp -a "$PROJECT_ROOT/src/ntdll/"*.h "$KDIR/arch/ntdll/" 2>/dev/null || true
    fi
    # Kconfig, Kbuild, Makefile patches
    if [ ! -f "$KDIR/arch/x86/Kconfig.nt" ]; then
      python3 "$PROJECT_ROOT/create_kconfig_nt.py" 2>/dev/null || true
    fi
    if ! grep -q "arch/x86/Kconfig.nt" "$KDIR/arch/x86/Kconfig" 2>/dev/null; then
      python3 "$PROJECT_ROOT/create_kconfig_nt.py" 2>/dev/null || true
    fi
    if ! grep -q "obj-.*nt/" "$KDIR/arch/x86/Kbuild" 2>/dev/null; then
      echo 'obj-$(CONFIG_NT_PERSONALITY) += nt/' >> "$KDIR/arch/x86/Kbuild"
      echo "Patched arch/x86/Kbuild for nt/"
    fi
    if ! grep -q "binfmt_pe" "$KDIR/fs/Makefile" 2>/dev/null; then
      python3 "$PROJECT_ROOT/scratch/add_fs_makefile.py" 2>/dev/null || true
    fi
    if ! grep -q "BINFMT_PE" "$KDIR/fs/Kconfig.binfmt" 2>/dev/null; then
      python3 "$PROJECT_ROOT/scratch/add_fs_kconfig2.py" 2>/dev/null || true
    fi
    # thread_info, syscall, fork, sched
    if ! grep -q "TIF_NTABI" "$KDIR/arch/x86/include/asm/thread_info.h" 2>/dev/null; then
      cp "$PROJECT_ROOT/src/kernel/patches/thread_info.h" "$KDIR/arch/x86/include/asm/thread_info.h" 2>/dev/null || cp ~/ntpers/linux-6.18.old/arch/x86/include/asm/thread_info.h "$KDIR/arch/x86/include/asm/thread_info.h" 2>/dev/null || true
      python3 "$PROJECT_ROOT/add_suspend.py" 2>/dev/null || true
    fi
    # Ensure syscall_64 has NT dispatch (check for do_nt_syscall)
    if ! grep -q "do_nt_syscall" "$KDIR/arch/x86/entry/syscall_64.c" 2>/dev/null; then
      cp ~/ntpers/linux-6.18.old/arch/x86/entry/syscall_64.c "$KDIR/arch/x86/entry/syscall_64.c" 2>/dev/null || true
      python3 "$PROJECT_ROOT/fix_syscall.py" 2>/dev/null || true
      python3 "$PROJECT_ROOT/fix_syscall_orig.py" 2>/dev/null || true
    fi
    echo "NT sources restore done (restored=$restored)"
  fi
}
restore_nt_sources

# Ensure Kconfig.nt and arch/x86/nt are sane (preflight already checks, but double-check Makefile)
echo "Checking arch/x86/nt/Makefile..."
if [ ! -f "$KDIR/arch/x86/nt/Makefile" ]; then
  echo "ERROR: arch/x86/nt/Makefile missing (even after restore)" >&2
  exit 1
fi
expected_objects=(
  "dispatch.o"
  "proc_thread.o"
  "mm.o"
  "handle.o"
  "kshim.o"
  "iocp.o"
  "object.o"
  "sync.o"
  "security.o"
  "job.o"
  "port.o"
  "nt_obj.o"
  "nt_io.o"
  "ntstatus_kernel.o"
  "nt_services.o"
  "debugfs_tests.o"
  "stubs.o"
  "weak.o"
  "kernel_stubs.o"
)
for obj in "${expected_objects[@]}"; do
  if ! grep -q "$obj" "$KDIR/arch/x86/nt/Makefile"; then
    echo "ERROR: Makefile missing $obj (truncated?)" >&2
    exit 1
  fi
done
echo "Makefile OK (18 objects)"

# Build - with hardening against `make clean` corruption and fixdep race
echo "Preparing host tools (fixdep, objtool) single-threaded..."
make -C "$KDIR" prepare -j1 2>&1 | tail -n 20 || true
echo "Building kernel (make -j$(nproc))..."
cd "$KDIR"
# Use nproc, but handle stale built-in.a / fixdep race that `make clean` leaves
set +e
make -j$(nproc) 2>&1 | tee /tmp/kbuild.log
BUILD_RC=${PIPESTATUS[0]}
set -e
if [ $BUILD_RC -ne 0 ]; then
  if grep -q "ar:.*No such file" /tmp/kbuild.log || grep -q "No rule to make target.*built-in.a" /tmp/kbuild.log || grep -q "fixdep:.*No such file" /tmp/kbuild.log || grep -q "No rule to make target.*realmode.elf" /tmp/kbuild.log; then
    echo "WARNING: Detected make clean corruption (stale built-in.a / fixdep race) - recovering..."
    # Remove stale archives and object files that `make clean` leaves behind
    find "$KDIR" -name "built-in.a" -delete 2>/dev/null || true
    find "$KDIR" -name "*.o" -delete 2>/dev/null || true
    find "$KDIR" -name ".*.cmd" -delete 2>/dev/null || true
    find "$KDIR" -name ".tmp_*" -delete 2>/dev/null || true
    # Full mrproper recovery: re-extract is heavy, but mrproper + olddefconfig is sufficient
    # Save .config, mrproper, restore, and rebuild from scratch
    cp "$CONFIG_DST" /tmp/kconfig.bak 2>/dev/null || cp "$CONFIG_SRC" /tmp/kconfig.bak
    make -C "$KDIR" mrproper 2>&1 | tail -n 20 || true
    # Restore NT sources that mrproper would have deleted if they were in KDIR (they live outside, so restore)
    restore_nt_sources
    cp /tmp/kconfig.bak "$CONFIG_DST"
    make -C "$KDIR" olddefconfig 2>&1 | tail -n 20 || true
    make -C "$KDIR" prepare -j1 2>&1 | tail -n 20 || true
    echo "Retrying build after recovery..."
    make -j$(nproc) 2>&1 | tee /tmp/kbuild.log
    BUILD_RC=${PIPESTATUS[0]}
    if [ $BUILD_RC -ne 0 ]; then
      echo "ERROR: Build still failing after recovery" >&2
      cat /tmp/kbuild.log >&2
      exit 1
    fi
  else
    echo "ERROR: Build failed (not clean corruption)" >&2
    cat /tmp/kbuild.log >&2
    exit 1
  fi
fi
echo "Build completed, running completeness check..."

# Run completeness checker (must pass)
if [ ! -x "$CHECKER" ]; then
  chmod +x "$CHECKER" || true
fi
if ! "$CHECKER" "$KDIR/vmlinux"; then
  echo "ERROR: Completeness check FAILED - kernel incomplete" >&2
  echo "This usually means arch/x86/nt/Makefile was truncated or symbols missing" >&2
  exit 1
fi

echo "=== kbuild: PASS - kernel complete and verified ==="
# M0 regression gate: ensure minimal PE still exits 42. This catches the
# "LdrInitializeThunk for no-import PE" regression that broke M0 twice.
# The gate boots the freshly built kernel and runs vm/test_m0.sh which
# executes tests/kernel/bin/minimal.exe via binfmt_pe.
echo "=== kbuild: running M0 regression gate (check_m0.sh) ==="
if ! "$PROJECT_ROOT/vm/check_m0.sh"; then
  echo "ERROR: M0 gate FAILED - minimal.exe did not exit 42 (regression)" >&2
  echo "This build breaks the load-bearing milestone. See vm/check_m0.sh and serial log." >&2
  exit 1
fi
# --- Identity stamping: record digest + artifact hashes (only on success) ---
# Deterministic ordering: build all artifacts first (kernel + initramfs), then stamp once at end.
# Previously stamped before initramfs rebuild and again after, causing embedded vs on-disk mismatch;
# now stamp once after every artifact is final so embedded and sidecar digests agree.
echo "=== kbuild: stamping artifact identity ==="
if [ -x "$PROJECT_ROOT/vm/mkinitramfs.sh" ]; then
  echo " rebuilding initramfs with stamping..."
  "$PROJECT_ROOT/vm/mkinitramfs.sh" 2>&1 | tail -n 20 || echo " mkinitramfs warning after kbuild" >&2
fi
if [ -x "$PROJECT_ROOT/vm/freshness.sh" ]; then
  NTPERS_ALLOW_WRITE=1 "$PROJECT_ROOT/vm/freshness.sh" write 2>&1 | tail -n 20 || { echo "WARNING: freshness stamp failed (check build completeness)" >&2; }
fi
echo "=== kbuild: M0 gate PASS ==="
