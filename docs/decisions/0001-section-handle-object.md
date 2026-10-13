# 0001: Section handles come from the object table (M-1)

Status: Accepted. Implemented in c8f479e (option 1, src/kernel/nt only). Arch is not changed; see 0021.
Finding: `docs/review/kernel-nt.md` M-1 (High).
Date: 2026-10-12

## Context

`NtCreateSection` returns the kernel address of `struct nt_section` as the handle (`src/kernel/nt/mm.c:1498`, `*SectionHandle = (HANDLE)sec;`). `NtMapViewOfSection` casts the handle straight back (`mm.c:1507`) and reads `sec->attributes`, `sec->size`, `sec->page_protection` and `sec->backing` with no table lookup and no type check. The handle is therefore:

- an address disclosure (a KASLR leak to any caller that creates a section), and
- a forged-pointer primitive: any value passed to `NtMapViewOfSection` is dereferenced as a section and its `backing` is handed to `kshim_section_map` (`src/kernel/nt/kshim.c:117-137`).

Every other kernel object in this tree is reached through `ObCreateHandle` / `ObReferenceObjectByHandle` (`src/kernel/nt/handle.c`, `object.h`). Sections are the exception.

## Options

1. **Object-table section object (recommended).** Register a `struct nt_object_type` for sections with `ObCreateObjectType` / `ObRegisterObjectType` (`object.h:178-182`), allocate the section with `ObCreateObject` (`object.h:184`), and return a handle from `ObCreateHandle`. `NtMapViewOfSection` calls `ObReferenceObjectByHandle(..., &nt_section_type, ...)`. Tradeoff: adds a new object type to the object model, which this batch must not change. It is the only option that removes the disclosure and the forgery at the root.
2. **Side table keyed by handle, like `ntio_handles`.** Store sections in a table with its own lock and return the index. Tradeoff: duplicates handle-table logic, does not integrate with `NtDuplicateObject` or `NtClose`, and is more code to get right than option 1.
3. **Validate the pointer against a live-section list.** Keep `sec` as the handle value and look it up under a lock before use. Tradeoff: fixes the forgery but still returns a kernel address, so the KASLR leak remains. Acceptable only as a stopgap, and must not be reported as a full fix.
4. **Opaque per-process index.** Tradeoff: needs the same table as option 2 and changes the value user space sees.

## Recommendation

Option 1, landed together with 0004 (M-3), since the section object needs a delete callback either way. Option 3 is only a stopgap if the object-model change is deferred.

## Patch sketch (option 1)

- `src/kernel/nt/object.h` (near `:101-105`): declare `extern struct nt_object_type nt_section_type;`.
- `src/kernel/nt/mm.c`, `NtCreateSection` (`:1450-1500`): allocate through `ObCreateObject(&nt_section_type, ...)` instead of the bare `kzalloc` at the top; keep the backing setup (`:1466-1495`); replace `*SectionHandle = (HANDLE)sec;` (`:1498`) with `ObCreateHandle(cur, obj, DesiredAccess, 0, SectionHandle)`. On error, `ObDereferenceObject`.
- `NtMapViewOfSection` (`:1502-1507`): replace the cast with `ObReferenceObjectByHandle(cur, SectionHandle, &nt_section_type, UserMode, 0, &hdr, NULL)`, take the body from `hdr`, and `ObDereferenceObject(hdr)` on every return path. Unknown or wrong-type handles then return `STATUS_INVALID_HANDLE` or `STATUS_OBJECT_TYPE_MISMATCH`.
- `nt_section_type.ops.delete` frees `sec->backing` and `sec` (see 0004).

## Test plan

- `src/kernel/nt/mm_test.c`: create a section, map it, unmap it. Pass forged handle values (a small integer, a kernel-looking value) to `NtMapViewOfSection` and expect `STATUS_INVALID_HANDLE`.
- `src/kernel/nt/object_test.c`: `NtDuplicateObject` on a section handle keeps the section alive after the original handle closes.
- A file or event handle passed to `NtMapViewOfSection` returns `STATUS_OBJECT_TYPE_MISMATCH`.
- `tests/kernel-host` is owned by another workstream and is not used.

## Outcome (c8f479e)

- `NtCreateSection` creates a Section object (`nt_section_type`, `src/kernel/nt/mm.c`) and returns a handle-table index from `ObInsertObject`. The granted access is `DesiredAccess`.
- `NtMapViewOfSection` resolves the handle with `ObReferenceObjectByHandle` against `nt_section_type`. The required mask is `SECTION_MAP_READ`, plus `SECTION_MAP_WRITE` for `PAGE_READWRITE` and `SECTION_MAP_EXECUTE` for the `PAGE_EXECUTE*` protections. A missing right gives `STATUS_ACCESS_DENIED`. The lookup reference is dropped after mapping.
- Each VAD that holds a section holds one object reference. Splits take a reference per copy and every VAD free drops one (`nt_vad_free`). The section is freed when the last handle and the last view are gone.
- Tests: `src/kernel/nt/mm_test.c` (handle is an object handle, forged integers and the body address are rejected, wrong type, closed handle, access masks, lifetime with the allocation tracker, split view).

Still open:

- `NtQuerySection`, `NtExtendSection` and `NtOpenSection` are in the service list but are not implemented in this tree. Named sections are not supported; `ObjectAttributes` is ignored.
- `arch/x86/nt/mm.c` has its own section code and was not changed (0021).
- Section lifetime (0004, kernel-nt M-3) is also covered by the lifetime tests in c8f479e, but that record was not changed here.
