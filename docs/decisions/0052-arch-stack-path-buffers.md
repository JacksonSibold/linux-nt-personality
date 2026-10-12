# 0052: Large stack buffers in the file path (arch IO-16)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` IO-16 (Low).
Date: 2026-10-12

## Context

`arch/x86/nt/nt_io.c:1234` (`char path[PATH_MAX]` in `ntio_open_file`), `:975` (`char rel[PATH_MAX]` in `ntio_resolve_path`), and `:2263` and `:2279` (`newpath` and `rel` in `ntio_set_rename`) are each 4 KiB on the kernel stack. Some are nested.

## Options

1. Allocate the path buffers with `kmalloc` and free them on every return path. Tradeoff: every return path changes, and allocation can fail.
2. Keep the stack buffers and record the stack budget. Tradeoff: nested calls can overflow the kernel stack.

## Recommendation (proposal)

Option 1, after the owner confirms the stack budget. Option 2 is acceptable only if the budget allows the nested depth.

## Patch sketch

- Replace each `char x[PATH_MAX]` with a `kmalloc` buffer and a single cleanup label.

## Test plan

- Kernel build with `-Wframe-larger-than` (VM) before and after.
- Rename and open paths still succeed; allocation failure returns `STATUS_INSUFFICIENT_RESOURCES`.

## Not verified

Not built or run.

## Owner question

Is the kernel stack budget large enough for these 4 KiB buffers, or should they move to `kmalloc`?
