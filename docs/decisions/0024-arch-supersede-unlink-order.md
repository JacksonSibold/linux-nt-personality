# 0024: FILE_SUPERSEDE deletes the file before the open can succeed (arch IO-5)

Status: Proposed. Blocked/needs owner (see the note at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` IO-5 (High).
Date: 2026-10-12

## Context

In `arch/x86/nt/nt_io.c`, the kernel branch of `ntio_apply_disposition` calls `kshim_vfs_unlink(path)` for `FILE_SUPERSEDE` (lines `1106-1108`). That function is called at `1303`, before the share check (`1489`) and before the open succeeds. If the share check or the open then fails, the original file is already gone.

## Options

1. Move the unlink (or the replace) to after the share check and the open succeed. Tradeoff: the replace becomes the last step; a crash between the open and the unlink can leave both names. Small change.
2. Rename the existing file aside to a temporary name, unlink it only on success, and rename it back on failure. Tradeoff: two more operations, and the rename-back can fail. Use this if a later step can still fail after option 1's point.
3. Keep as is. Not acceptable.

## Recommendation

Option 1, provided nothing after the new point can fail. Otherwise option 2.

## Patch sketch

- Remove the unlink from `ntio_apply_disposition`.
- Call it from the open path after the share check and after the file object exists.

## Test plan

- Kernel test: create a file with known content, then open it with `FILE_SUPERSEDE` and a sharing mode that conflicts. The open fails and the file still exists with its original content.
- Kernel test: a `FILE_SUPERSEDE` open that succeeds replaces the content.
- Host: not applicable.

## Not verified

Not built or run. The order of the share check and open within `nt_io.c` was taken from the review's line references.

## Blocked/needs owner

Not implemented. The patch sketch is incomplete. Two points in the record do not match the code:

- The share check (`nt_io.c:1489`) runs after the open (`:1407`), not before it.
- The `FILE_SUPERSEDE` path sets `O_CREAT | O_TRUNC` (`ntio_apply_disposition`, `:1101-1111`). The open therefore truncates the existing file before the share check. Moving the unlink alone still loses the contents when the share check fails.

Needs an owner decision between option 2 (rename aside, unlink on success, rename back on failure) and dropping `O_TRUNC` for the superseded case with a truncate after success. Both change the open sequence.
