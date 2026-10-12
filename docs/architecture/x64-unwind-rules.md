# x64 unwind rules (summary)

Source: Microsoft Learn, "x64 exception handling" (`cpp/build/exception-handling-x64`)
and "x64 prolog and epilog" (`cpp/build/prolog-and-epilog`). Fetched 2026-10-12.
This is a paraphrase for the ntdll unwinder. Read the source for the exact text.

## Chained unwind info

- A secondary `UNWIND_INFO` has `UNW_FLAG_CHAININFO` set and no handler flags.
  It ends with a `RUNTIME_FUNCTION` that names the next entry's `UNWIND_INFO`.
  That entry sits after the code array, after padding to an even slot count.
- Chains can be longer than one link. The chain ends at an info with the flag
  clear. That is the primary, and it holds the procedure's entry point.
- The frame register and the fixed-stack allocation must match the primary.
- Chained info is used for noncontiguous code and for saves that happen after the
  prolog (shrink-wrapping). Such saves are `UWOP_SAVE_NONVOL` codes only.
  Pushes and extra stack allocations are not allowed in a chained entry.

## Unwind procedure and code order

- The unwinder walks the codes of the `UNWIND_INFO` that the PC resolved to.
  When that array runs out, it links to the parent and walks the parent's whole
  array. This repeats until the primary.
- So the order is: resolved (secondary) codes first, then each parent in turn,
  ending with the primary. Within one array the codes run in array order, which
  is descending `CodeOffset`.
- The walk stops at the primary, then the return address is popped.

## Unwind codes

- Codes are sorted by descending prolog offset. `UWOP_PUSH_NONVOL` codes come
  first in the prolog and therefore last in the array.
- The `CodeOffset` is the offset just after the instruction that did the operation.
  An operation has run when the PC offset is at or past that value.
- `UWOP_ALLOC_SMALL` allocates `OpInfo * 8 + 8` bytes (8 to 128).
- `UWOP_ALLOC_LARGE`: `OpInfo` 0 gives one slot holding size / 8. `OpInfo` 1 gives
  two slots holding the unscaled 32-bit size, low word first.
- `UWOP_SAVE_NONVOL` stores a register at a scaled-by-8 offset. The `_FAR` form
  is unscaled and takes two slots. `UWOP_SAVE_XMM128` uses a scale of 16.
- `UWOP_SET_FPREG` sets the frame register to `RSP + FrameOffset * 16`.
- `UWOP_PUSH_MACHFRAME` describes a hardware trap frame, with or without an error code.

## Prolog

- A PC is in the prolog when its offset is below `SizeOfProlog`. Only the codes
  that have already run are undone, starting from the first code at or before the
  PC offset.

## Epilog

- An epilog is detected by scanning forward from the PC. It must match exactly
  one of these forms:
  1. `add rsp, constant` or `lea rsp, constant[FPReg]` (the `lea` form needs a
     frame pointer register).
  2. Zero or more 8-byte pops. Popping `rsp` is not allowed.
  3. `ret`, or `jmp` with a memory operand whose ModRM `mod` field is 00.
- No other instruction may appear in an epilog. Nothing can be scheduled in it.
- Without a frame pointer, the stack must be released with `add`, not `lea`.
- The unwinder simulates the rest of the epilog and then repeats the unwind from the
  caller's PC.
