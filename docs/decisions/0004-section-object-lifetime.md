# 0004: Section object lifetime (M-3)

Status: Proposed. Depends on 0001. Not implemented.
Finding: `docs/review/kernel-nt.md` M-3 (Medium).
Date: 2026-10-12

## Context

`NtCreateSection` (`src/kernel/nt/mm.c:1450-1500`) allocates `sec` with `kzalloc` and its backing store with `kshim_section_create` / `kshim_section_create_file` (`kshim.h:215-216`). `kfree(sec)` runs only on the error paths in that function. No close, destroy or delete routine exists for sections. `vad->section` (set in `NtMapViewOfSection`, `mm.c:1527` area) keeps a pointer to the section after the view is unmapped.

Each successful `NtCreateSection` leaks `sec` and `sec->size` bytes of backing for the life of the system. This is reachable by unprivileged callers, so it is a memory exhaustion path.

There is no `kshim_section_destroy` in `kshim.h`, so the backing release also needs a shim function, or the existing fd and mem fields must be released directly.

## Options

1. **Object type with a delete callback (recommended).** This is 0001's option 1. `nt_section_type.ops.delete` releases the backing and frees `sec`. Lifetime comes from the object reference count: a reference per handle (from `ObCreateHandle`) and per live VAD (`NtMapViewOfSection` takes one and `NtUnmapViewOfSection` drops it). Tradeoff: needs 0001 and the VAD reference is a new call on the unmap path.
2. **Free on last unmap when no handle is open.** Tradeoff: needs a handle count on the section, which is the same state as option 1 but hand-rolled, and it is easy to get wrong with `NtDuplicateObject`.
3. **Free at process exit.** Tradeoff: sections outlive the process that created them if handles were duplicated, so this is unsafe.

## Recommendation

Option 1, implemented together with 0001.

## Patch sketch

- `src/kernel/nt/kshim.h` / `kshim.c`: add `void kshim_section_destroy(struct kshim_section_backing *b);`. In the userspace shim, close `b->fd` and unmap `b->mem` if set. In the kernel build, drop the file reference and free the anonymous pages. Both sides need the same contract.
- `src/kernel/nt/mm.c`: add `static void nt_section_delete(struct nt_object_header *obj)` which calls `kshim_section_destroy(&sec->backing)` and `kfree(sec)`, and register it as `nt_section_type.ops.delete` (0001).
- `NtUnmapViewOfSection` (`mm.c:1552` area): after `nt_list_remove` and `kfree(vad)` on the view-removal path, call `ObDereferenceObject` on the section for each VAD whose `section` is set.
- `NtMapViewOfSection`: `ObReferenceObject` the section when `vad->section = sec` (`:1527`), so the VAD holds a reference.
- Remove the `kfree(sec)` on the error paths in `NtCreateSection` once the object owns the memory; error paths call `ObDereferenceObject` instead.

## Test plan

- `src/kernel/nt/mm_test.c`: create 100,000 sections in a loop, closing each handle, and check the backing count (instrument `kshim_section_destroy`) matches the create count.
- Map, unmap, close: the section is freed on the last of these, and not before (check with a counter in `nt_section_delete`).
- Duplicate the handle, close the original, unmap, and confirm the section is still usable through the duplicate.
- `tests/kernel-host` is not used.

## Open questions

- The file-backed path (`kshim_section_create_file`) takes a path-like `FileHandle` cast (M-7 in the review). Its destroy path depends on whether M-7 is fixed first.
