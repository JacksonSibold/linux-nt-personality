# 0021: Section handle is a kernel pointer (arch M-2)

Status: Proposed. Arch source (`arch/x86/nt`). Not implemented in arch. The same fix is in `src/kernel/nt` (c8f479e, record 0001), but it was not copied here: `arch/x86/nt/mm.c` is not structurally the same as `src/kernel/nt/mm.c`. Its `NtMapViewOfSection` carries the offset and size checks from `d921888` that src lacks, and it keeps VAD coalescing that src removed (`docs/analysis/tree-divergence.md`, mm.c, 10 hunks). Porting would mean choosing between the two trees' mm behaviour first (tree-divergence section 5, step 2 and 3), so it is left open.
Finding: `docs/review/arch-x86-nt.md` M-2 (High).
Date: 2026-10-12

## Context

`arch/x86/nt/mm.c:1540` writes `*SectionHandle = (HANDLE)sec;`, the address of a `kzalloc` object. That discloses a kernel address (defeats KASLR) and makes the handle forgeable. `NtMapViewOfSection` (`mm.c:1549`) casts any integer back to `struct nt_section *` and dereferences it, checking only for NULL. The `nt_svc_*` wrappers are outside this directory and were not reviewed. This is the same pattern as `src/kernel/nt/mm.c` (0001).

## Options

1. Allocate the handle through the object manager and resolve it in `NtMapViewOfSection` with a type-checked lookup (`ObReferenceObjectByHandle`). Tradeoff: needs the object-table API. Matches 0001.
2. Per-process index table; the handle is a tagged index. Tradeoff: new table and slot reuse rules (see 0003 for the slot-reuse decision in `src/kernel/nt`).
3. Keep the pointer and hide it with a cookie. Tradeoff: still forgeable. Not acceptable.

## Recommendation

Option 1, implemented the same way as 0001 so both trees follow one rule.

## Patch sketch

- `NtCreateSection`: create the handle through the object table; return only the handle.
- `NtMapViewOfSection`: look the handle up with the type check; fail with `STATUS_INVALID_HANDLE` otherwise.

## Test plan

- Kernel test: `NtMapViewOfSection` with the address of a kernel object as the handle returns `STATUS_INVALID_HANDLE`.
- Kernel test: a handle returned by `NtCreateSection` is not a kernel address (outside the kernel address range of the test host).
- Host: not applicable.

## Not verified

Not built or run. The `nt_svc_*` wrappers that forward these calls were not read.
