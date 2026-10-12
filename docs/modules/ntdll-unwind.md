# Module: ntdll-unwind (x64 table-driven unwinding)

Sources: `src/ntdll/unwind.c`, `src/ntdll/unwind.h`
Uses types from `src/loader/pe_format.h` (`IMAGE_RUNTIME_FUNCTION_ENTRY`, `UNWIND_INFO`, `UNWIND_CODE`).
Related: `ntdll-except.md` (the dispatcher that calls this module), `ntdll-startup.md` (`RtlAddFunctionTable`), `docs/review/ntdll.md` (findings cited below).

## Purpose

Finds the `RUNTIME_FUNCTION` for a program counter and undoes one frame using the `UNWIND_INFO` codes, so that the exception dispatcher can walk the stack and find handlers. It implements the x64 unwind operation codes, mid-prologue handling, chained unwind info, and leaf functions. The header comment cites the Microsoft x64 exception-handling documentation.

## Public API (unwind.h)

- Module registry: `unwind_register_module(imageBase, sizeOfImage, funcTable, funcCount, image)`, `unwind_clear_modules`, `unwind_module_count`.
- Lookup: `RtlLookupFunctionEntry(pc, &imageBase, history)`, `RtlLookupFunctionEntryEx`.
- Unwind: `RtlVirtualUnwind(handlerType, imageBase, pc, entry, ctx, &handlerData, &establisher, ctxPointers)`, which returns the language handler or NULL.
- Context and raise: `RtlCaptureContext`, `RtlRaiseException` (calls `_NtRaiseException` in `except.c`), `RtlUnwindEx`.
- Test helper: `unwind_one_frame(ctx, imageBase, entry, image, &establisher)`.

Constants: `UWOP_*` codes 0 to 10, and `UNW_FLAG_EHANDLER` (0x1), `UNW_FLAG_UHANDLER` (0x2), `UNW_FLAG_CHAININFO` (0x4).

## Data structures

- `unwind_module_t` (`unwind.c`): `imageBase`, `sizeOfImage`, `funcTable`, `funcCount`, `image`. A module is an entry in a fixed array of 64 (`MAX_MODULES`). The table is searched linearly.
- `chain_elem_t` and `MAX_CHAIN` (8): the chain of unwind infos for one function.
- `DISPATCHER_CONTEXT` and `KNONVOLATILE_CONTEXT_POINTERS` (`unwind.h`): mirror the Windows layouts.

## Behaviour as implemented

- **Lookup.** `lookup_in_module` is a binary search over `BeginAddress`, with `EndAddress` masked by `~3`. An RVA is `pc - imageBase`. A module with `sizeOfImage == 0` is bounded only by its table (UNW-1, fixed in `a09f0a3`).
- **Registry.** Registrations go into a fixed array. A registration returns -1 when the array is full. `unwind_clear_modules` removes every entry, and nothing removes one entry (see MAIN-7).
- **Decoding.** `UNWIND_INFO` is read at `image + UnwindInfoAddress`. Codes are walked in array order (descending `CodeOffset`). For a PC inside the prolog, the walk starts at the first code whose `CodeOffset` is not beyond the PC, and that code and the ones after it are undone. `ALLOC_LARGE` reads one or two slots of size, `SAVE_*` reads the offset from the next slot scaled by 8 or 16, and `SET_FPREG` restores RSP from the frame register.
- **Frame register.** When any info in the chain has a frame register, the save offsets are computed from that register minus `FrameOffset * 16`. Otherwise they are computed from the running RSP.
- **Handler.** `RtlVirtualUnwind` returns the handler RVA after the codes when `UNW_FLAG_EHANDLER` or `UNW_FLAG_UHANDLER` is set, and sets `HandlerData` to the bytes after it. It ignores `HandlerType` (UNW-3).
- **Leaf frames.** `unwind_one_frame` with a NULL entry pops the return address at RSP. This needs a known module or a non-NULL image pointer.
- **Captured context.** `RtlCaptureContext` writes the integer registers, RIP, RSP, EFLAGS, segment selectors, MXCSR and an FXSAVE image into a `CONTEXT`, using inline assembly on x86-64 GCC.

## Invariants

- Registered tables must be sorted by `BeginAddress`. The lookup does not check this.
- Chained entries are assumed to share the module and follow the primary's codes. The order in which chained codes are applied is not verified against the specification.

## Known gaps and defects (see docs/review/ntdll.md)

- **RtlUnwindEx does not unwind to the target.** It walks frames, but it discards the unwind handlers it finds and does not transfer control to `TargetIp` (UNW-2).
- **No handler-type filter** in `RtlVirtualUnwind` (UNW-3).
- **EndAddress is masked with ~3** (UNW-4). The x64 table has no flag bits, so the mask shortens each function by up to three bytes. Confirm against the specification before changing it.
- **Reads are not bounds-checked** against the module, so a corrupt table reads outside the image (UNW-5).
- `RtlDeleteFunctionTable` lives in `ntdll_main.c` and does not unregister (MAIN-7).
