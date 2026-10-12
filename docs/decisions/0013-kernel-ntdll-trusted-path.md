# 0013: ntdll is loaded from a world-writable path (K-5)

Status: Proposed. Kernel source (`src/kernel/fs`). Recorded only in this batch, not implemented.
Finding: `docs/review/kernel-fs.md` K-5 (High).
Date: 2026-10-12

## Context

`pe_map_ntdll` (`src/kernel/fs/binfmt_pe.c:622-631`) tries `/ntdll.dll`, `/windows/system32/ntdll.dll` and `/tmp/ntdll.dll`. It opens with `filp_open(O_RDONLY)` and uses the file with no owner, mode or file-type check. If the first two are missing, any local user can create `/tmp/ntdll.dll`. Its `LdrInitializeThunk` then runs as the entry point of every PE process (`851-855`, `952-953`). A FIFO at that path blocks `filp_open` inside exec, because `O_NONBLOCK` is not set.

## Options

1. Remove the `/tmp` candidate. Tradeoff: exec fails when the system copy is absent, which is the intended result. Small change.
2. Also require a root-owned regular file with no group or world write, open with `O_NOFOLLOW`, and check `S_ISREG` on the inode before reading. Tradeoff: a few more lines; closes the FIFO hang.
3. Embed ntdll in the kernel image. Tradeoff: build change; removes the file lookup.

## Recommendation

Options 1 and 2 together now. Option 3 is a later design choice.

## Patch sketch

- Delete the `/tmp/ntdll.dll` entry.
- After `filp_open`, check `file_inode(f)`: `S_ISREG`, `uid == 0`, `(mode & (S_IWGRP | S_IWOTH)) == 0`. Fail with `-EACCES` otherwise.
- Use `O_RDONLY | O_NOFOLLOW | O_NONBLOCK`.

## Test plan

- Kernel test: with no system ntdll, create `/tmp/ntdll.dll` as a non-root user and exec a PE. Expect failure, not a load.
- Kernel test: a FIFO at the candidate path does not block exec.
- Host: not applicable.

## Not verified

Not built or run. The candidate paths are as recorded in the review; the current tree was not re-read for this record.
