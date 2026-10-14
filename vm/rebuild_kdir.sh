#!/bin/bash
# vm/rebuild_kdir.sh - From-scratch reproducible kernel rebuild
# Extracts vanilla 6.18 tarball, applies every tracked source/patch deterministically,
# regenerates generated files, configures and builds. This is the ONLY way to
# recover from KDIR corruption that survives git checkout (kbuild.sh only restores missing files).
# Usage: ./vm/rebuild_kdir.sh [--no-build]  (default builds)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KDIR="${KDIR:-$HOME/ntpers/linux-6.18}"
export KDIR
TARBALL="${TARBALL:-$HOME/ntpers/linux-6.18.tar.xz}"
export TARBALL
export PROJECT_ROOT
CONFIG_SRC="$SCRIPT_DIR/kernel-config-ntpers"
NO_BUILD=0
if [[ "${1:-}" == "--no-build" ]]; then NO_BUILD=1; fi

echo "=== rebuild_kdir: from-scratch reproducible build ==="
echo "PROJECT_ROOT=$PROJECT_ROOT"
echo "KDIR=$KDIR"
echo "TARBALL=$TARBALL"

if [[ ! -f "$TARBALL" ]]; then
  echo "ERROR: tarball not found: $TARBALL" >&2
  exit 1
fi

# 1. Remove existing KDIR entirely (proof of reproducibility)
# For DrvFs robustness, we avoid rm -rf on the live KDIR which often leaves "Directory not empty".
# Instead, we extract vanilla to a temp and rsync it over KDIR with --delete, which handles stale files.
# First, ensure KDIR parent exists and is writable
if [[ -d "$KDIR" ]]; then
  echo "Removing existing KDIR $KDIR ..."
  # WSL DrvFs rm -rf is unreliable (leaves "Directory not empty"); use loop with python + shopt
  for _rep in 1 2 3; do
    python3 - << 'PY' 2>/dev/null || true
import pathlib, shutil, os
k = os.environ.get("KDIR", str(pathlib.Path.home() / "ntpers/linux-6.18"))
p = pathlib.Path(k)
if p.exists():
    shutil.rmtree(p, ignore_errors=True)
PY
    rm -rf "$KDIR" 2>/dev/null || true
    shopt -s dotglob 2>/dev/null || true
    rm -rf "$KDIR"/* 2>/dev/null || true
    # Remove hidden dotfiles (excluding . and ..)
    for _dot in "$KDIR"/.*; do
      [[ -e "$_dot" ]] || continue
      case "$(basename "$_dot")" in "."|"..") continue ;; esac
      rm -rf "$_dot" 2>/dev/null || true
    done
    rmdir "$KDIR" 2>/dev/null || true
    shopt -u dotglob 2>/dev/null || true
    find "$KDIR" -type f -delete 2>/dev/null || true
    find "$KDIR" -depth -type d -exec rmdir {} \; 2>/dev/null || true
    rm -rf "$KDIR" 2>/dev/null || true
    [[ ! -d "$KDIR" ]] && break
    sleep 0.3
  done
  if [[ -d "$KDIR" ]]; then
    echo "ERROR: failed to remove KDIR $KDIR" >&2
    ls -la "$KDIR" 2>&1 | head -n 20 >&2 || true
    exit 1
  fi
  sync 2>/dev/null || true
fi

# 2. Extract vanilla - use temp dir + rsync to handle DrvFs gracefully
echo "Extracting $TARBALL -> $KDIR ..."
mkdir -p "$(dirname "$KDIR")"
TMP_EXTRACT=$(mktemp -d)
# Extract to temp to avoid tar-over-existing-dir races
if ! tar -xf "$TARBALL" -C "$TMP_EXTRACT"; then
  echo "ERROR: tar extraction failed" >&2
  rm -rf "$TMP_EXTRACT" 2>/dev/null || true
  exit 1
fi
if [[ ! -d "$TMP_EXTRACT/linux-6.18" ]]; then
  echo "ERROR: tar extraction did not produce linux-6.18 in $TMP_EXTRACT" >&2
  ls -la "$TMP_EXTRACT" 2>&1 | head -n 20 >&2 || true
  rm -rf "$TMP_EXTRACT" 2>/dev/null || true
  exit 1
fi
# Use cp -a to copy vanilla to KDIR (more reliable than rsync on DrvFs for large trees)
# KDIR should be gone after step 1, so cp -a will create it
if [[ -d "$KDIR" ]]; then
  echo "ERROR: KDIR still exists before extraction, cannot copy" >&2
  ls -la "$KDIR" 2>&1 | head -n 20 >&2 || true
  # Try to remove again
  python3 - << 'PY' 2>/dev/null || true
import pathlib, shutil, os
k = os.environ.get("KDIR", str(pathlib.Path.home() / "ntpers/linux-6.18"))
p = pathlib.Path(k)
if p.exists():
    shutil.rmtree(p, ignore_errors=True)
PY
  rm -rf "$KDIR" 2>/dev/null || true
  if [[ -d "$KDIR" ]]; then
    echo "ERROR: KDIR still exists after second removal" >&2
    ls -la "$KDIR" 2>&1 | head -n 20 >&2 || true
    rm -rf "$TMP_EXTRACT" 2>/dev/null || true
    exit 1
  fi
fi
# Copy vanilla tree to KDIR
ls -la "$(dirname "$KDIR")" 2>&1 | head -n 10 >&2 || true
# Use cp without piping to head to avoid SIGPIPE killing cp
if ! cp -a "$TMP_EXTRACT/linux-6.18" "$KDIR"; then
  echo "ERROR: cp -a failed" >&2
  ls -la "$(dirname "$KDIR")" 2>&1 | head -n 20 >&2 || true
  rm -rf "$TMP_EXTRACT" 2>/dev/null || true
  exit 1
fi
ls -la "$KDIR" 2>&1 | head -n 20 >&2 || true
# Verify KDIR is now populated
if [[ ! -f "$KDIR/Makefile" ]]; then
  echo "ERROR: KDIR missing Makefile after cp -a" >&2
  ls -la "$KDIR" 2>&1 | head -n 20 >&2 || true
  rm -rf "$TMP_EXTRACT" 2>/dev/null || true
  exit 1
fi
rm -rf "$TMP_EXTRACT" 2>/dev/null || true
echo "Extraction complete: $(ls -d "$KDIR")"

# 3. Apply tracked NT sources unconditionally (overwrite, not only when missing)
echo "Applying tracked NT sources..."

# Ensure dirs exist
mkdir -p "$KDIR/arch/x86/nt" "$KDIR/arch/ntdll" "$KDIR/fs"

# arch/x86/nt: src/kernel/nt/ is the one canonical NT tree (docs/analysis/tree-divergence.md,
# section 5). Copy it, then the arch-only files (Makefile, kernel_stubs.c). A file in both
# trees is refused, as tools/ntpers_kpatch.py does.
if [[ -d "$PROJECT_ROOT/arch/x86/nt" ]]; then
  for f in "$PROJECT_ROOT"/arch/x86/nt/*; do
    [[ -f "$f" ]] || continue
    bn=$(basename "$f")
    if [[ -f "$PROJECT_ROOT/src/kernel/nt/$bn" ]]; then
      echo "ERROR: arch/x86/nt/$bn duplicates src/kernel/nt/$bn; delete the arch copy" >&2
      exit 1
    fi
  done
fi
if [[ -d "$PROJECT_ROOT/src/kernel/nt" ]]; then
  echo "  copying src/kernel/nt/ -> KDIR/arch/x86/nt/ (canonical)"
  while IFS= read -r -d '' f; do
    case "$(basename "$f")" in
      *.bak|*.new|*.rej) continue ;;
    esac
    cp -a "$f" "$KDIR/arch/x86/nt/" 2>/dev/null || true
  done < <(find "$PROJECT_ROOT/src/kernel/nt" -maxdepth 1 -type f -print0)
fi
if [[ -f "$PROJECT_ROOT/arch/x86/nt/Makefile" ]]; then
  cp -a "$PROJECT_ROOT/arch/x86/nt/Makefile" "$KDIR/arch/x86/nt/Makefile"
fi

# Generated but required: stubs.c, kernel_stubs.c, nt_service_list.h is already tracked via arch/x86/nt/
echo "  ensuring generated stubs..."
: > "$KDIR/arch/x86/nt/stubs.c"
cat > "$KDIR/arch/x86/nt/kernel_stubs.c" <<'EOS'
#include <linux/kernel.h>
void nt_banner_init(void){pr_info("NT personality: ntpers v1.0 banner\n");}
EOS

# fs/binfmt_pe.c
if [[ -f "$PROJECT_ROOT/src/kernel/fs/binfmt_pe.c" ]]; then
  echo "  copying src/kernel/fs/binfmt_pe.c -> KDIR/fs/binfmt_pe.c"
  cp "$PROJECT_ROOT/src/kernel/fs/binfmt_pe.c" "$KDIR/fs/binfmt_pe.c"
fi

# arch/ntdll headers
if [[ -d "$PROJECT_ROOT/src/ntdll" ]]; then
  echo "  copying src/ntdll/*.h -> KDIR/arch/ntdll/"
  cp -a "$PROJECT_ROOT/src/ntdll/"*.h "$KDIR/arch/ntdll/" 2>/dev/null || true
fi

# 4. Patch Kconfig/Kbuild/Makefile deterministically
echo "Patching Kconfig/Kbuild..."

# arch/x86/Kconfig.nt (deterministic generation)
cat > "$KDIR/arch/x86/Kconfig.nt" <<'EOS'
# SPDX-License-Identifier: GPL-2.0
config NT_PERSONALITY
	bool "NT personality (Windows NT syscall ABI) and PE loader"
	depends on X86_64
	default y
	help
	  Enable the NT personality and PE loader.
EOS
echo "  created arch/x86/Kconfig.nt"

# arch/x86/Kconfig: ensure source line exists exactly once
if ! grep -q 'arch/x86/Kconfig.nt' "$KDIR/arch/x86/Kconfig"; then
  # Insert after HYPERVISOR_GUEST endif
  python3 - << 'PY'
import pathlib
import os
p = pathlib.Path(os.environ.get('KDIR', str(pathlib.Path.home() / 'ntpers/linux-6.18'))) / 'arch/x86/Kconfig'
t = p.read_text()
if 'arch/x86/Kconfig.nt' not in t:
    t = t.replace('endif # HYPERVISOR_GUEST', 'endif # HYPERVISOR_GUEST\n\nsource "arch/x86/Kconfig.nt"')
    p.write_text(t)
    print("patched Kconfig")
PY
fi

# arch/x86/Kbuild: ensure nt/ obj line
if ! grep -q 'obj-.*nt/' "$KDIR/arch/x86/Kbuild"; then
  echo 'obj-$(CONFIG_NT_PERSONALITY) += nt/' >> "$KDIR/arch/x86/Kbuild"
  echo "  patched arch/x86/Kbuild"
fi

# fs/Makefile: ensure binfmt_pe
if ! grep -q 'binfmt_pe' "$KDIR/fs/Makefile"; then
  if [[ -f "$PROJECT_ROOT/scratch/add_fs_makefile.py" ]]; then
    python3 "$PROJECT_ROOT/scratch/add_fs_makefile.py" 2>/dev/null || true
  else
    python3 - << 'PY'
import pathlib
import os
p = pathlib.Path(os.environ.get('KDIR', str(pathlib.Path.home() / 'ntpers/linux-6.18'))) / 'fs/Makefile'
t = p.read_text()
if 'binfmt_pe' not in t:
    t = t.replace('obj-$(CONFIG_BINFMT_FLAT)\t+= binfmt_flat.o', 'obj-$(CONFIG_BINFMT_FLAT)\t+= binfmt_flat.o\nobj-$(CONFIG_BINFMT_PE)\t+= binfmt_pe.o')
    p.write_text(t)
PY
  fi
  echo "  patched fs/Makefile"
fi

# fs/Kconfig.binfmt: ensure BINFMT_PE
if ! grep -q 'BINFMT_PE' "$KDIR/fs/Kconfig.binfmt"; then
  if [[ -f "$PROJECT_ROOT/scratch/add_fs_kconfig2.py" ]]; then
    python3 "$PROJECT_ROOT/scratch/add_fs_kconfig2.py" 2>/dev/null || true
  else
    python3 - << 'PY'
import pathlib
import os
p = pathlib.Path(os.environ.get('KDIR', str(pathlib.Path.home() / 'ntpers/linux-6.18'))) / 'fs/Kconfig.binfmt'
t = p.read_text()
if 'BINFMT_PE' not in t:
    t = t.replace('endmenu', 'config BINFMT_PE\n\ttristate "Kernel support for PE binaries (NT personality)"\n\tdepends on X86_64\n\tdefault y\n\thelp\n\t  Say Y here to enable the kernel to execute PE binaries.\n\nendmenu')
    p.write_text(t)
PY
  fi
  echo "  patched fs/Kconfig.binfmt"
fi

# 5. Patch thread_info.h deterministically
echo "Patching thread_info.h..."
python3 - << 'PY'
import pathlib
import os
p = pathlib.Path(os.environ.get('KDIR', str(pathlib.Path.home() / 'ntpers/linux-6.18'))) / 'arch/x86/include/asm/thread_info.h'
t = p.read_text()
if 'TIF_NTABI' not in t:
    t = t.replace('#define TIF_BLOCKSTEP', '#define TIF_NTSUSPEND\t30\t/* NT thread suspend */\n#define TIF_NTABI\t\t29\t/* task uses the NT system-call ABI */\n#define TIF_BLOCKSTEP')
    t = t.replace('#define _TIF_BLOCKSTEP', '#define _TIF_NTSUSPEND\tBIT(TIF_NTSUSPEND)\n#define _TIF_NTABI\t\tBIT(TIF_NTABI)\n#define _TIF_BLOCKSTEP')
    p.write_text(t)
    print("  patched thread_info.h for TIF_NTABI/NTSUSPEND")
else:
    # Ensure both present and in correct order
    if 'TIF_NTSUSPEND' not in t:
        t = t.replace('#define TIF_NTABI', '#define TIF_NTSUSPEND\t30\t/* NT thread suspend */\n#define TIF_NTABI')
        t = t.replace('#define _TIF_NTABI', '#define _TIF_NTSUSPEND\tBIT(TIF_NTSUSPEND)\n#define _TIF_NTABI')
        p.write_text(t)
        print("  added missing TIF_NTSUSPEND")
    else:
        print("  thread_info.h already patched")
PY

# 5b. Patch sched.h for nt_thread (per-task NT thread pointer)
echo "Patching sched.h..."
python3 - << 'PY'
import pathlib
import os
p = pathlib.Path(os.environ.get('KDIR', str(pathlib.Path.home() / 'ntpers/linux-6.18'))) / 'include/linux/sched.h'
t = p.read_text()
if '*nt_thread;' not in t:
    # Insert before randomized_struct_fields_end
    t = t.replace('\t/* CPU-specific state of this task: */\n\tstruct thread_struct\t\tthread;', '\t/* CPU-specific state of this task: */\n\tstruct thread_struct\t\tthread;\n\n#ifdef CONFIG_NT_PERSONALITY\n\tvoid\t\t\t\t*nt_thread;\n#endif')
    p.write_text(t)
    print("  patched sched.h for nt_thread")
else:
    print("  sched.h already patched")
PY

# 5c. Patch fork.c to init nt_thread
echo "Patching fork.c..."
python3 - << 'PY'
import pathlib
import os
p = pathlib.Path(os.environ.get('KDIR', str(pathlib.Path.home() / 'ntpers/linux-6.18'))) / 'kernel/fork.c'
t = p.read_text()
if 'tsk->nt_thread' not in t:
    t = t.replace('\terr = alloc_thread_stack_node(tsk, node);', '#ifdef CONFIG_NT_PERSONALITY\n\ttsk->nt_thread = NULL;\n#endif\n\n\terr = alloc_thread_stack_node(tsk, node);')
    p.write_text(t)
    print("  patched fork.c")
else:
    print("  fork.c already patched")
PY

# 6. Patch syscall_64.c deterministically
echo "Patching syscall_64.c..."
python3 - << 'PY'
import pathlib
import os
p = pathlib.Path(os.environ.get('KDIR', str(pathlib.Path.home() / 'ntpers/linux-6.18'))) / 'arch/x86/entry/syscall_64.c'
t = p.read_text()
if 'do_nt_syscall' not in t:
    # Add includes
    if '#include <linux/entry-common.h>' in t and 'do_nt_syscall' not in t:
        t = t.replace('#include <linux/entry-common.h>', '#include <linux/entry-common.h>\n#include <linux/nospec.h>\n#include <asm/syscall.h>\n#ifdef CONFIG_NT_PERSONALITY\nvoid do_nt_syscall(struct pt_regs *regs, unsigned int ssn);\nbool nt_can_sysret(struct pt_regs *regs);\n#endif')
    # Add dispatch in do_syscall_64
    if 'test_thread_flag(TIF_NTABI)' not in t:
        t = t.replace('__visible noinstr bool do_syscall_64(struct pt_regs *regs, int nr)\n{\n\tadd_random_kstack_offset();', '__visible noinstr bool do_syscall_64(struct pt_regs *regs, int nr)\n{\n\tadd_random_kstack_offset();\n\n#ifdef CONFIG_NT_PERSONALITY\n\tif (unlikely(test_thread_flag(TIF_NTABI))) {\n\t\tdo_nt_syscall(regs, (unsigned int)nr);\n\t\tsyscall_exit_to_user_mode(regs);\n\t\treturn nt_can_sysret(regs);\n\t}\n#endif')
    p.write_text(t)
    print("  patched syscall_64.c")
else:
    print("  syscall_64.c already patched")
PY

# 7. Ensure .config from vm/kernel-config-ntpers
echo "Installing .config..."
cp "$CONFIG_SRC" "$KDIR/.config"
echo "  installed .config from $CONFIG_SRC"

if [[ $NO_BUILD -eq 1 ]]; then
  echo "=== rebuild_kdir: --no-build requested, stopping before build ==="
  exit 0
fi

# 8. Build - MUST run olddefconfig and prepare BEFORE any compilation that needs autoconf.h
echo "Building kernel (prepare)..."
# Clean stale host tools that can cause "Text file busy" on DrvFs
rm -f "$KDIR/scripts/basic/fixdep" 2>/dev/null || true
# olddefconfig generates .config dependencies, prepare generates include/generated/autoconf.h
make -C "$KDIR" olddefconfig
# Handle fixdep race that can cause prepare to fail with "fixdep: not found" (seen in verifier)
# Use single-threaded prepare and retry on failure, similar to kbuild.sh recovery
set +e
make -C "$KDIR" prepare -j1 2>&1 | tee /tmp/rebuild_prepare.log
PREP_RC=${PIPESTATUS[0]}
set -e
if [[ $PREP_RC -ne 0 ]]; then
  echo "WARNING: make prepare failed with $PREP_RC, retrying after cleaning host tools..." >&2
  cat /tmp/rebuild_prepare.log >&2
  find "$KDIR" -name "built-in.a" -delete 2>/dev/null || true
  rm -f "$KDIR/scripts/basic/fixdep" 2>/dev/null || true
  make -C "$KDIR" prepare -j1 2>&1 | tee /tmp/rebuild_prepare.log
  PREP_RC=${PIPESTATUS[0]}
  if [[ $PREP_RC -ne 0 ]]; then
    echo "ERROR: make prepare still failing after retry" >&2
    cat /tmp/rebuild_prepare.log >&2
    exit $PREP_RC
  fi
fi
# Critical gate: autoconf.h must exist before full build, otherwise NT sources will fail with missing header
if [[ ! -f "$KDIR/include/generated/autoconf.h" ]]; then
  echo "ERROR: include/generated/autoconf.h missing after make prepare - build cannot proceed" >&2
  echo "This indicates olddefconfig/prepare did not complete successfully" >&2
  exit 1
fi
echo "prepare complete - autoconf.h present"
echo "Building kernel (make -j$(nproc))..."
set +e
make -C "$KDIR" -j$(nproc) 2>&1 | tee /tmp/rebuild_kdir_build.log
BUILD_RC=${PIPESTATUS[0]}
set -e
if [[ $BUILD_RC -ne 0 ]]; then
  echo "WARNING: make -j$(nproc) failed with $BUILD_RC, checking for known races..." >&2
  cat /tmp/rebuild_kdir_build.log | tail -n 30 >&2 || true
  # Known parallel races: __is_kernel_text, srso_alias_untrain_ret, fixdep, built-in.a
  if grep -q "ar:.*No such file" /tmp/rebuild_kdir_build.log || grep -q "No rule to make target.*built-in.a" /tmp/rebuild_kdir_build.log || grep -q "fixdep:.*No such file" /tmp/rebuild_kdir_build.log || grep -q "No rule to make target.*realmode.elf" /tmp/rebuild_kdir_build.log || grep -q "__is_kernel_text" /tmp/rebuild_kdir_build.log || grep -q "srso_alias" /tmp/rebuild_kdir_build.log || grep -q "implicit declaration" /tmp/rebuild_kdir_build.log || grep -q "Text file busy" /tmp/rebuild_kdir_build.log; then
    echo "Detected parallel build race, retrying with -j1..." >&2
  else
    echo "Unknown build failure, retrying with -j1 as fallback..." >&2
  fi
  find "$KDIR" -name "built-in.a" -delete 2>/dev/null || true
  rm -f "$KDIR/scripts/basic/fixdep" 2>/dev/null || true
  make -C "$KDIR" prepare -j1 2>&1 | tail -n 20 || true
  set +e
  make -C "$KDIR" -j1 2>&1 | tee /tmp/rebuild_kdir_build.log
  BUILD_RC=${PIPESTATUS[0]}
  set -e
  if [[ $BUILD_RC -ne 0 ]]; then
    echo "ERROR: kernel build failed even with -j1, exit $BUILD_RC" >&2
    cat /tmp/rebuild_kdir_build.log >&2
    exit $BUILD_RC
  else
    echo "Build succeeded with -j1 after parallel failure" >&2
  fi
fi
echo "Build complete."

# 9. Verify completeness
CHECKER="$PROJECT_ROOT/tests/kernel/check_kernel_complete.sh"
if [[ -x "$CHECKER" ]]; then
  echo "Running completeness check..."
  "$CHECKER" "$KDIR/vmlinux"
fi

# 10. Rebuild initramfs first, then stamp once after all artifacts final (deterministic)
# Previously stamped before initramfs and again after, causing embedded vs on-disk mismatch;
# now single final stamp ensures bzImage + initramfs are both final before digest recorded.
if [[ -x "$PROJECT_ROOT/vm/mkinitramfs.sh" ]]; then
  echo "Rebuilding initramfs with identity..."
  "$PROJECT_ROOT/vm/mkinitramfs.sh" 2>&1 | tail -n 20 || true
fi
if [[ -x "$PROJECT_ROOT/vm/freshness.sh" ]]; then
  echo "Updating freshness manifest (hardened)..."
  set +e
  NTPERS_ALLOW_WRITE=1 "$PROJECT_ROOT/vm/freshness.sh" write 2>&1 | tee /tmp/freshness_write.log
  FRESH_RC=${PIPESTATUS[0]}
  set -e
  if [[ $FRESH_RC -ne 0 ]]; then
    echo "ERROR: freshness write failed with $FRESH_RC" >&2
    cat /tmp/freshness_write.log >&2
    exit $FRESH_RC
  fi
  # Verify both manifests exist and are identical - closes drift bypass
  if [[ ! -f "$KDIR/.ntpers_build.manifest" ]]; then
    echo "ERROR: KDIR manifest missing after freshness write ($KDIR/.ntpers_build.manifest)" >&2
    exit 1
  fi
  if [[ ! -f "$PROJECT_ROOT/.ntpers_build.manifest" ]]; then
    echo "ERROR: PROJECT manifest missing after freshness write ($PROJECT_ROOT/.ntpers_build.manifest)" >&2
    exit 1
  fi
  if ! cmp -s "$KDIR/.ntpers_build.manifest" "$PROJECT_ROOT/.ntpers_build.manifest"; then
    echo "ERROR: manifest drift between KDIR and PROJECT after rebuild" >&2
    diff -u "$KDIR/.ntpers_build.manifest" "$PROJECT_ROOT/.ntpers_build.manifest" | head -n 50 >&2 || true
    exit 1
  fi
  echo "Manifests verified - KDIR and PROJECT identical"
fi

echo "=== rebuild_kdir: PASS - kernel reproducible and built ==="
