#!/bin/bash
# freshness.sh - source-to-image freshness gate with identity stamping
# Computes digest over all sources that contribute to kernel/bzImage/initramfs/DLLs,
# records at build time, and refuses to run VM if stale.
# Hardened: missing manifest is FAIL, write only from successful build, stale is FAIL.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KDIR="${KDIR:-$HOME/ntpers/linux-6.18}"
MANIFEST_KDIR="$KDIR/.ntpers_build.manifest"
MANIFEST_PROJECT="$PROJECT_ROOT/.ntpers_build.manifest"
DIGEST_KDIR="$KDIR/.ntpers_build.digest"
DIGEST_PROJECT="$PROJECT_ROOT/.ntpers_build.digest"
BZIMAGE="$KDIR/arch/x86/boot/bzImage"
VMLINUX="$KDIR/vmlinux"
INITRAMFS="$PROJECT_ROOT/vm/initramfs.cpio.gz"
INITRAMFS_DIGEST="$PROJECT_ROOT/vm/initramfs.digest"
INITRAMFS_SHA="$PROJECT_ROOT/vm/initramfs.sha256"
# Identity bundle (extended)
IDENTITY_JSON_KDIR="$KDIR/.ntpers_build.identity.json"
IDENTITY_JSON_PROJECT="$PROJECT_ROOT/.ntpers_build.identity.json"

compute_manifest() {
  local out="$1"
  local tmp
  tmp=$(mktemp)
  # PROJECT src/kernel/nt - exclude generated artifacts (pure functions of tracked inputs)
  for f in "$PROJECT_ROOT"/src/kernel/nt/*; do
    [ -f "$f" ] || continue
    bn=$(basename "$f")
    case "$bn" in
      build_digest.h|kernel_stubs.c|stubs.c|nt_service_list.h|*.bak|*.new|*.rej) continue ;;
    esac
    rel="PROJECT/src/kernel/nt/$bn"
    sha=$(sha256sum "$f" | awk '{print $1}')
    echo "$sha  $rel" >> "$tmp"
  done
  # PROJECT src/kernel/fs
  for f in "$PROJECT_ROOT"/src/kernel/fs/*; do
    [ -f "$f" ] || continue
    bn=$(basename "$f")
    case "$bn" in
      build_digest.h|kernel_stubs.c|stubs.c|*.bak|*.new|*.rej) continue ;;
    esac
    rel="PROJECT/src/kernel/fs/$bn"
    sha=$(sha256sum "$f" | awk '{print $1}')
    echo "$sha  $rel" >> "$tmp"
  done
  # PROJECT src/ntdll/*.h
  for f in "$PROJECT_ROOT"/src/ntdll/*.h; do
    [ -f "$f" ] || continue
    bn=$(basename "$f")
    case "$bn" in
      build_digest.h|*.bak|*.new|*.rej) continue ;;
    esac
    rel="PROJECT/src/ntdll/$bn"
    sha=$(sha256sum "$f" | awk '{print $1}')
    echo "$sha  $rel" >> "$tmp"
  done
  # PROJECT src/ntdll/syscall_table.def is authoritative source for nt_service_list.h (generated)
  if [ -f "$PROJECT_ROOT/src/ntdll/syscall_table.def" ]; then
    sha=$(sha256sum "$PROJECT_ROOT/src/ntdll/syscall_table.def" | awk '{print $1}')
    echo "$sha  PROJECT/src/ntdll/syscall_table.def" >> "$tmp"
  fi
  # PROJECT arch/x86/nt - exclude generated artifacts
  for f in "$PROJECT_ROOT"/arch/x86/nt/*; do
    [ -f "$f" ] || continue
    bn=$(basename "$f")
    case "$bn" in
      build_digest.h|kernel_stubs.c|stubs.c|nt_service_list.h|*.bak|*.new|*.rej) continue ;;
    esac
    rel="PROJECT/arch/x86/nt/$bn"
    sha=$(sha256sum "$f" | awk '{print $1}')
    echo "$sha  $rel" >> "$tmp"
  done
  # PROJECT src/kernel/patches
  if [ -f "$PROJECT_ROOT/src/kernel/patches/thread_info.h" ]; then
    sha=$(sha256sum "$PROJECT_ROOT/src/kernel/patches/thread_info.h" | awk '{print $1}')
    echo "$sha  PROJECT/src/kernel/patches/thread_info.h" >> "$tmp"
  fi
  # KDIR files are derived from PROJECT via rebuild_kdir.sh/kbuild.sh; not included in manifest to avoid drift detection loops.
  # The authoritative sources are PROJECT/*; KDIR is a build artifact.
  sort "$tmp" > "$out"
  rm -f "$tmp"
}

compute_overall() {
  local manifest="$1"
  sha256sum "$manifest" | awk '{print $1}'
}

write_digest() {
  # Hardened: only callable from successful build with token and fresh bzImage
  if [ "${NTPERS_ALLOW_WRITE:-}" != "1" ]; then
    echo "ERROR: freshness write not allowed - must be called from vm/kbuild.sh or vm/rebuild_kdir.sh (set NTPERS_ALLOW_WRITE=1)" >&2
    echo "Direct 'freshness.sh write' is denied to prevent dirty tree from looking clean (audit bypass)." >&2
    return 1
  fi
  # Verify kernel completeness before stamping (prevents stamping incomplete build)
  CHECKER="$PROJECT_ROOT/tests/kernel/check_kernel_complete.sh"
  if [ -x "$CHECKER" ] && [ -f "$VMLINUX" ]; then
    if ! "$CHECKER" "$VMLINUX" >/dev/null 2>&1; then
      echo "ERROR: freshness write refused - kernel completeness check failed ($VMLINUX incomplete)" >&2
      return 1
    fi
  fi
  # Verify bzImage exists and is not stale relative to sources (before stamping)
  if [ ! -f "$BZIMAGE" ]; then
    echo "ERROR: freshness write refused - bzImage not found at $BZIMAGE (build required)" >&2
    return 1
  fi
  local manifest_tmp
  manifest_tmp=$(mktemp)
  compute_manifest "$manifest_tmp"
  local overall
  overall=$(compute_overall "$manifest_tmp")
  # Check that bzImage is newer than kernel sources (otherwise write would make dirty look clean)
  # Only kernel-affecting sources should gate bzImage; vm/* and build Makefile affect initramfs/DLLs not bzImage
  local stale_src=0
  while IFS= read -r line; do
    rel=$(echo "$line" | awk '{print $2}')
    case "$rel" in
      PROJECT/src/kernel/*|PROJECT/src/ntdll/*|PROJECT/arch/x86/nt/*|PROJECT/src/kernel/patches/*|KDIR/*)
        ;;
      *) continue ;;
    esac
    if [[ "$rel" == PROJECT/* ]]; then
      f="$PROJECT_ROOT/${rel#PROJECT/}"
    else
      f="$KDIR/${rel#KDIR/}"
    fi
    if [ -f "$f" ] && [ "$f" -nt "$BZIMAGE" ]; then
      echo "ERROR: freshness write refused - source $rel newer than bzImage (dirty tree, rebuild required before stamping)" >&2
      echo "  $rel mtime $(stat -c %y "$f" 2>/dev/null || stat -f %Sm "$f" 2>/dev/null) > bzImage $(stat -c %y "$BZIMAGE" 2>/dev/null || stat -f %Sm "$BZIMAGE" 2>/dev/null)" >&2
      stale_src=1
    fi
  done < "$manifest_tmp"
  if [ $stale_src -ne 0 ]; then
    rm -f "$manifest_tmp"
    return 1
  fi
  # Note: initramfs staleness warning (direct, not via manifest)
  if [ -f "$INITRAMFS" ]; then
    for f in "$PROJECT_ROOT/vm/init.c" "$PROJECT_ROOT/vm/mkinitramfs.sh"; do
      if [ -f "$f" ] && [ "$f" -nt "$INITRAMFS" ]; then
        echo "WARNING: initramfs stale - ${f#$PROJECT_ROOT/} newer than $INITRAMFS (will be rebuilt by kbuild)" >&2
      fi
    done
  fi
  # All gates passed - write manifest and digest to both KDIR and PROJECT
  mkdir -p "$(dirname "$MANIFEST_KDIR")"
  cp "$manifest_tmp" "$MANIFEST_KDIR"
  echo "$overall" > "$DIGEST_KDIR"
  cp "$manifest_tmp" "$MANIFEST_PROJECT"
  echo "$overall" > "$DIGEST_PROJECT"
  # also record bzImage hash if exists
  if [ -f "$BZIMAGE" ]; then
    sha256sum "$BZIMAGE" | awk '{print $1}' > "$KDIR/.ntpers_bzimage.sha256" 2>/dev/null || true
    cp "$KDIR/.ntpers_bzimage.sha256" "$PROJECT_ROOT/.ntpers_bzimage.sha256" 2>/dev/null || true
  fi
  # also record identity json with overall, bzImage, initramfs, DLLs, git commit
  local git_commit="unknown"
  if command -v git >/dev/null 2>&1 && [ -d "$PROJECT_ROOT/.git" ]; then
    git_commit=$(git -C "$PROJECT_ROOT" rev-parse HEAD 2>/dev/null || echo "unknown")
  fi
  local bz_sha="missing"
  if [ -f "$KDIR/.ntpers_bzimage.sha256" ]; then bz_sha=$(cat "$KDIR/.ntpers_bzimage.sha256"); fi
  local init_sha="missing"
  if [ -f "$INITRAMFS" ]; then init_sha=$(sha256sum "$INITRAMFS" | awk '{print $1}'); fi
  # stamp initramfs sidecars
  if [ -f "$INITRAMFS" ]; then
    echo "$overall" > "$INITRAMFS_DIGEST" 2>/dev/null || true
    sha256sum "$INITRAMFS" | awk '{print $1}' > "$INITRAMFS_SHA" 2>/dev/null || true
  fi
  # stamp DLLs sidecars
  for dll in ntdll.dll kernel32.dll msvcrt.dll advapi32.dll user32.dll kernelbase.dll ucrtbase.dll; do
    if [ -f "$PROJECT_ROOT/build/$dll" ]; then
      sha256sum "$PROJECT_ROOT/build/$dll" | awk '{print $1}' > "$PROJECT_ROOT/build/$dll.sha256" 2>/dev/null || true
      echo "$overall" > "$PROJECT_ROOT/build/$dll.digest" 2>/dev/null || true
    fi
  done
  # write identity json
  cat > "$IDENTITY_JSON_PROJECT" <<EOF
{
  "overall_digest": "$overall",
  "bzimage_sha256": "$bz_sha",
  "initramfs_sha256": "$init_sha",
  "git_commit": "$git_commit",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "manifest_lines": $(wc -l < "$manifest_tmp")
}
EOF
  cp "$IDENTITY_JSON_PROJECT" "$IDENTITY_JSON_KDIR" 2>/dev/null || true
  echo "freshness: recorded digest $overall ($(wc -l < "$manifest_tmp") files) commit $git_commit bzImage $bz_sha"
  rm -f "$manifest_tmp"
}

check_freshness() {
  # Hardened: missing manifest is FAIL (not warning) - closes delete bypass
  if [ ! -f "$MANIFEST_KDIR" ] && [ ! -f "$MANIFEST_PROJECT" ]; then
    echo "ERROR: STALE BUILD DETECTED - no freshness manifest found" >&2
    echo "  Expected $MANIFEST_KDIR or $MANIFEST_PROJECT" >&2
    echo "  This likely means .ntpers_build.* was deleted (bypass) or no build has been done." >&2
    echo "  Run: vm/rebuild_kdir.sh or vm/kbuild.sh" >&2
    return 1
  fi
  if [ ! -f "$MANIFEST_KDIR" ] || [ ! -f "$MANIFEST_PROJECT" ]; then
    echo "ERROR: STALE BUILD DETECTED - manifest present in only one location (drift)" >&2
    echo "  KDIR manifest: $([ -f "$MANIFEST_KDIR" ] && echo present || echo missing)" >&2
    echo "  PROJECT manifest: $([ -f "$MANIFEST_PROJECT" ] && echo present || echo missing)" >&2
    echo "  Run: vm/kbuild.sh" >&2
    return 1
  fi
  # Verify manifests identical (drift between KDIR and PROJECT)
  if ! cmp -s "$MANIFEST_KDIR" "$MANIFEST_PROJECT"; then
    echo "ERROR: STALE BUILD DETECTED - KDIR and PROJECT manifests differ (drift)" >&2
    diff -u "$MANIFEST_KDIR" "$MANIFEST_PROJECT" | head -n 50 >&2 || true
    echo "Run: vm/rebuild_kdir.sh" >&2
    return 1
  fi
  # Verify digests identical
  if [ -f "$DIGEST_KDIR" ] && [ -f "$DIGEST_PROJECT" ]; then
    if ! cmp -s "$DIGEST_KDIR" "$DIGEST_PROJECT"; then
      echo "ERROR: STALE BUILD DETECTED - digest files differ between KDIR and PROJECT" >&2
      echo "  KDIR: $(cat "$DIGEST_KDIR" 2>/dev/null)" >&2
      echo "  PROJECT: $(cat "$DIGEST_PROJECT" 2>/dev/null)" >&2
      return 1
    fi
  fi
  local stored_manifest
  if [ -f "$MANIFEST_KDIR" ]; then
    stored_manifest="$MANIFEST_KDIR"
  else
    stored_manifest="$MANIFEST_PROJECT"
  fi
  local cur_manifest
  cur_manifest=$(mktemp)
  compute_manifest "$cur_manifest"
  if ! cmp -s "$stored_manifest" "$cur_manifest"; then
    echo "ERROR: STALE BUILD DETECTED - kernel image is stale relative to sources" >&2
    echo "The following source files have changed since the last build (vm/kbuild.sh):" >&2
    local diff_tmp
    diff_tmp=$(mktemp)
    diff -u "$stored_manifest" "$cur_manifest" > "$diff_tmp" 2>&1 || true
    grep -E "^[\+\-][0-9a-f]{64}" "$diff_tmp" | head -n 100 >&2 || true
    echo "--- Changed files (hash mismatch) ---" >&2
    awk '{print $2}' "$stored_manifest" | sort > /tmp/fresh_old.$$.txt
    awk '{print $2}' "$cur_manifest" | sort > /tmp/fresh_cur.$$.txt
    while IFS= read -r line; do
      cur_sha=$(echo "$line" | awk '{print $1}')
      cur_rel=$(echo "$line" | awk '{print $2}')
      stored_line=$(grep -F " $cur_rel" "$stored_manifest" || true)
      if [ -z "$stored_line" ]; then
        echo "  NEW: $cur_rel" >&2
      else
        stored_sha=$(echo "$stored_line" | awk '{print $1}')
        if [ "$cur_sha" != "$stored_sha" ]; then
          echo "  MODIFIED: $cur_rel" >&2
        fi
      fi
    done < "$cur_manifest"
    while IFS= read -r line; do
      rel=$(echo "$line" | awk '{print $2}')
      if ! grep -q -F " $rel" "$cur_manifest"; then
        echo "  DELETED: $rel" >&2
      fi
    done < "$stored_manifest"
    rm -f /tmp/fresh_old.$$.txt /tmp/fresh_cur.$$.txt "$diff_tmp"
    if [ -f "$BZIMAGE" ]; then
      echo "--- Files newer than bzImage ($BZIMAGE, $(stat -c %y "$BZIMAGE" 2>/dev/null || stat -f %Sm "$BZIMAGE" 2>/dev/null || echo unknown)) ---" >&2
      local newer_found=0
      while IFS= read -r line; do
        rel=$(echo "$line" | awk '{print $2}')
        if [[ "$rel" == PROJECT/* ]]; then
          f="$PROJECT_ROOT/${rel#PROJECT/}"
        else
          f="$KDIR/${rel#KDIR/}"
        fi
        if [ -f "$f" ] && [ "$f" -nt "$BZIMAGE" ]; then
          echo "  $rel (mtime $(stat -c %y "$f" 2>/dev/null || stat -f %Sm "$f" 2>/dev/null))" >&2
          newer_found=1
        fi
      done < "$cur_manifest"
      if [ $newer_found -eq 0 ]; then
        echo "  (hash differs but mtimes not newer than bzImage - still stale due to digest mismatch)" >&2
      fi
    else
      echo "bzImage not found at $BZIMAGE - build required" >&2
    fi
    echo "Run: vm/kbuild.sh" >&2
    rm -f "$cur_manifest"
    return 1
  fi
  # Even if digest matches, check that no kernel source is newer than bzImage (catches touch without hash change)
  if [ -f "$BZIMAGE" ]; then
    local newer_found=0
    while IFS= read -r line; do
      rel=$(echo "$line" | awk '{print $2}')
      case "$rel" in
        PROJECT/src/kernel/*|PROJECT/src/ntdll/*|PROJECT/arch/x86/nt/*|PROJECT/src/kernel/patches/*|KDIR/*)
          ;;
        *) continue ;;
      esac
      if [[ "$rel" == PROJECT/* ]]; then
        f="$PROJECT_ROOT/${rel#PROJECT/}"
      else
        f="$KDIR/${rel#KDIR/}"
      fi
      if [ -f "$f" ] && [ "$f" -nt "$BZIMAGE" ]; then
        if [ $newer_found -eq 0 ]; then
          echo "ERROR: STALE BUILD DETECTED - source file newer than kernel image" >&2
          echo "The following source files are newer than $BZIMAGE:" >&2
        fi
        echo "  $rel (mtime $(stat -c %y "$f" 2>/dev/null || stat -f %Sm "$f" 2>/dev/null) > bzImage $(stat -c %y "$BZIMAGE" 2>/dev/null || stat -f %Sm "$BZIMAGE" 2>/dev/null))" >&2
        newer_found=1
      fi
    done < "$cur_manifest"
    if [ $newer_found -ne 0 ]; then
      echo "Run: vm/kbuild.sh" >&2
      rm -f "$cur_manifest"
      return 1
    fi
  else
    echo "ERROR: STALE BUILD DETECTED - bzImage missing at $BZIMAGE" >&2
    rm -f "$cur_manifest"
    return 1
  fi
  # Verify bzImage hash matches recorded sha (if present)
  if [ -f "$PROJECT_ROOT/.ntpers_bzimage.sha256" ] && [ -f "$BZIMAGE" ]; then
    local recorded
    recorded=$(cat "$PROJECT_ROOT/.ntpers_bzimage.sha256" 2>/dev/null | tr -d '[:space:]')
    local actual
    actual=$(sha256sum "$BZIMAGE" | awk '{print $1}')
    if [ -n "$recorded" ] && [ "$recorded" != "$actual" ]; then
      echo "ERROR: STALE BUILD DETECTED - bzImage sha256 mismatch (stale bzImage file)" >&2
      echo "  recorded $recorded" >&2
      echo "  actual   $actual" >&2
      echo "  bzImage was replaced or overwritten after build without updating manifest" >&2
      rm -f "$cur_manifest"
      return 1
    fi
  fi
  # Embedded digest check is now optional (sidecar is authoritative)
  # Previous embedding caused self-invalidation via build_digest.h; now disabled as FAIL.
  if false && [ -f "$BZIMAGE" ] && command -v strings >/dev/null 2>&1; then
    local current_overall
    current_overall=$(compute_overall "$cur_manifest")
    local embedded=""
    embedded=$(strings "$BZIMAGE" 2>/dev/null | grep -o "NTPERS_DIGEST:[0-9a-f]*" | head -1 | cut -d: -f2 || true)
    if [ -z "$embedded" ] && [ -f "$VMLINUX" ]; then
      embedded=$(strings "$VMLINUX" 2>/dev/null | grep -o "NTPERS_DIGEST:[0-9a-f]*" | head -1 | cut -d: -f2 || true)
    fi
    if [ -n "$embedded" ] && [ "$embedded" != "$current_overall" ]; then
      echo "WARNING: bzImage embedded digest $embedded != current $current_overall (sidecar check is authoritative)" >&2
    fi
  fi
  # Verify initramfs identity if it exists
  if [ -f "$INITRAMFS" ]; then
    if [ -f "$INITRAMFS_DIGEST" ]; then
      local init_rec
      init_rec=$(cat "$INITRAMFS_DIGEST" 2>/dev/null | tr -d '[:space:]')
      local current_overall2
      current_overall2=$(compute_overall "$cur_manifest")
      if [ -n "$init_rec" ] && [ "$init_rec" != "$current_overall2" ]; then
        echo "ERROR: STALE BUILD DETECTED - initramfs digest mismatch" >&2
        echo "  initramfs recorded $init_rec" >&2
        echo "  current          $current_overall2" >&2
        echo "  initramfs stale relative to sources (run vm/mkinitramfs.sh after build)" >&2
        rm -f "$cur_manifest"
        return 1
      fi
    fi
    if [ -f "$INITRAMFS_SHA" ]; then
      local rec_sha
      rec_sha=$(cat "$INITRAMFS_SHA" 2>/dev/null | tr -d '[:space:]')
      local act_sha
      act_sha=$(sha256sum "$INITRAMFS" | awk '{print $1}')
      if [ -n "$rec_sha" ] && [ "$rec_sha" != "$act_sha" ]; then
        echo "ERROR: STALE BUILD DETECTED - initramfs sha256 mismatch (stale initramfs file)" >&2
        echo "  recorded $rec_sha" >&2
        echo "  actual   $act_sha" >&2
        rm -f "$cur_manifest"
        return 1
      fi
    fi
    # Direct check initramfs freshness vs vm files and DLL sources (not via manifest)
    local init_stale=0
    for f in "$PROJECT_ROOT/vm/init.c" "$PROJECT_ROOT/vm/mkinitramfs.sh" "$PROJECT_ROOT/vm/runvm.sh"; do
      if [ -f "$f" ] && [ "$f" -nt "$INITRAMFS" ]; then
        if [ $init_stale -eq 0 ]; then
          echo "ERROR: STALE BUILD DETECTED - initramfs stale relative to sources" >&2
          echo "  initramfs $INITRAMFS older than:" >&2
        fi
        echo "    ${f#$PROJECT_ROOT/} (mtime $(stat -c %y "$f" 2>/dev/null))" >&2
        init_stale=1
      fi
    done
    for f in "$PROJECT_ROOT"/src/ntdll/* "$PROJECT_ROOT/build/Makefile.ntdll"; do
      [ -f "$f" ] || continue
      if [ "$f" -nt "$INITRAMFS" ]; then
        if [ $init_stale -eq 0 ]; then
          echo "ERROR: STALE BUILD DETECTED - initramfs stale relative to sources" >&2
          echo "  initramfs $INITRAMFS older than:" >&2
        fi
        echo "    ${f#$PROJECT_ROOT/}" >&2
        init_stale=1
      fi
    done
    if [ $init_stale -ne 0 ]; then
      echo "Run: vm/mkinitramfs.sh" >&2
      rm -f "$cur_manifest"
      return 1
    fi
    # Verify DLLs inside initramfs vs sidecar: ensure build DLLs are fresh relative to src
    for dll in ntdll.dll kernel32.dll msvcrt.dll advapi32.dll user32.dll kernelbase.dll ucrtbase.dll; do
      local dll_path="$PROJECT_ROOT/build/$dll"
      if [ -f "$dll_path" ]; then
        local dll_sha_file="$dll_path.sha256"
        if [ -f "$dll_sha_file" ]; then
          local rec
          rec=$(cat "$dll_sha_file" 2>/dev/null | tr -d '[:space:]')
          local act
          act=$(sha256sum "$dll_path" | awk '{print $1}')
          if [ "$rec" != "$act" ]; then
            echo "ERROR: STALE BUILD DETECTED - DLL $dll sha256 mismatch (stale DLL file)" >&2
            echo "  recorded $rec actual $act" >&2
            rm -f "$cur_manifest"
            return 1
          fi
        fi
        local dll_digest_file="$dll_path.digest"
        if [ -f "$dll_digest_file" ]; then
          local recd
          recd=$(cat "$dll_digest_file" 2>/dev/null | tr -d '[:space:]')
          local curo
          curo=$(compute_overall "$cur_manifest")
          if [ -n "$recd" ] && [ "$recd" != "$curo" ]; then
            echo "ERROR: STALE BUILD DETECTED - DLL $dll digest mismatch (built from different sources)" >&2
            echo "  dll recorded $recd current $curo" >&2
            rm -f "$cur_manifest"
            return 1
          fi
        fi
      fi
    done
  else
    echo "WARNING: initramfs not found at $INITRAMFS (vm/mkinitramfs.sh not run?)" >&2
  fi
  rm -f "$cur_manifest"
  return 0
}

# Command dispatch
case "${1:-}" in
  write) write_digest ;;
  check) check_freshness ;;
  compute) compute_manifest "${2:-/tmp/manifest.txt}"; cat "${2:-/tmp/manifest.txt}"; echo "--- overall ---"; sha256sum "${2:-/tmp/manifest.txt}" | awk '{print $1}' ;;
  *)
    if [ "${1:-}" = "" ]; then
      check_freshness
    else
      echo "Usage: $0 {write|check|compute [out]}" >&2
      exit 2
    fi
    ;;
esac
