# Kernel patches (deprecated)

Both patches in this directory are **deprecated**. Do not apply them with
`git apply`, and do not add new patches here for the kernel tree.

| File | Status |
|---|---|
| `0001-nt-personality-dispatch.patch` | Does not apply: the `syscall_64.c` hunk header says 10 old and 22 new lines, the body has 11 and 25 (BUILD-RECIPE G2). Its "Verified against linux-6.18" claim is not supported by anything in the repository (G20). |
| `0002-nt-seccomp-audit-arch.patch` | Does not apply: three bare `@@` headers (BUILD-RECIPE G3). Its `enter.c` hooks are not wired, and `enter.c` is still an open decision (G3). |

## What replaces them

`tools/ntpers_kpatch.py` makes every kernel-tree edit these patches described,
plus the wiring that used to live only in `vm/rebuild_kdir.sh`. It checks each
anchor against the tree it edits and refuses to write anything when an anchor
is missing:

```
python3 tools/ntpers_kpatch.py --tree "$KDIR" --check   # report only
python3 tools/ntpers_kpatch.py --tree "$KDIR"           # apply
```

`vm/guest-bootstrap.sh` runs the script in place of `git apply`.

The script differs from the patch text in these places, on purpose:

- The NT branch in `do_syscall_64()` calls `enter_from_user_mode()` before
  `do_nt_syscall()`. The patch omits it, so the exit would run without the
  matching entry.
- `TIF_NTABI` and `TIF_NTSUSPEND` are checked against the tree's existing bits
  and are not assumed to be free (G4).
- The `enter.c` hooks from patch 0002 are a `todo` edit. The run stops on that
  todo until BUILD-RECIPE G3 is decided.

## Why the files are kept

Regenerating a correct unified diff needs the exact 6.18 source, which is not
in the repository and was not available when this was written. A hand-written
diff would repeat the problem that made the patches fail. The files are kept as
a record of what the change was meant to be. Delete them once the script has
built and booted a kernel (BUILD-RECIPE G22).
