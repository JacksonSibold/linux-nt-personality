# 0049: Reference leak when renaming relative to a root directory (arch IO-8)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` IO-8 (Medium).
Date: 2026-10-12

## Context

`arch/x86/nt/nt_io.c:2276` calls `ntio_handle_lookup(ri->RootDirectory, 0)`, which takes a reference through `ntio_file_get` (`nt_io.c:420`). The rename branch (`2276-2287`) never calls `ntio_file_put(root)`. `ntio_handle_lookup` also returns IOCP objects, so a plain put is not enough for every type.

## Options

1. Call `ntio_file_put` on every return path. Tradeoff: wrong for a root that is an IOCP object.
2. Release by object type, so an IOCP object is released with its own put. Tradeoff: more code, belongs with the handle-access work in records 0022 and 0023.

## Recommendation (proposal)

Option 2, done as part of the IO-3 and IO-4 change. Option 1 is a stop-gap for file roots only.

## Patch sketch

- `nt_io.c:2276-2287`: release the root on each return path, by type.

## Test plan

- Rename relative to a root directory: the reference count returns to its starting value on success and on each failure.

## Not verified

Not built or run.

## Owner question

Should the root reference be released by object type, together with the handle-access change in records 0022 and 0023?
