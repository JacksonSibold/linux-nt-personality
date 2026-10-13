# 0038: Path mapping is a string prefix; symlinks escape the root; default root is the host (arch IO-11)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` IO-11 (Medium).
Date: 2026-10-12

## Context

`arch/x86/nt/nt_io.c:74` sets `g_kshim_root` to `/`, so every NT path maps into the host filesystem. `kshim_ntpath_to_linux` (`nt_io.c:172-323`) rejects `..` but then builds a plain path string under the root. `kshim_filp_open` follows symlinks and does not use `RESOLVE_BENEATH` or `O_NOFOLLOW`, so a symlink inside the root can point outside it.

## Options

1. Default `g_kshim_root` to a dedicated directory and open with `openat2` using `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`. Tradeoff: changes the default namespace and needs `openat2` in the kernel.
2. Keep `/` as the root and add `O_NOFOLLOW` on the final component only. Tradeoff: intermediate symlinks still escape.
3. Document the personality as trusted-only and keep the current behaviour. Tradeoff: no confinement.

## Recommendation (proposal)

Option 1. The default root is the main exposure, and only `openat2` closes the symlink path.

## Patch sketch

- `nt_io.c:74`: default the root to a dedicated directory, settable at init.
- `kshim_filp_open`: open relative to the root with `openat2` and the two resolve flags.
- Keep the `..` rejection; it becomes a second layer.

## Test plan

- A symlink inside the root pointing to a host path: open returns an access or not-found status.
- A `..` component and an absolute symlink: both denied.
- A normal path under the root: unchanged.
- Needs a kernel build for `openat2` (VM).

## Not verified

Not built or run. Whether `openat2` is available on every target kernel is not checked.

## Owner question

Should NT paths be confined to a dedicated root by default, with `openat2` as a kernel dependency?
