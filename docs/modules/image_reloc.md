# Module: image_reloc (base relocation applier for SEC_IMAGE views)

Sources: `src/kernel/nt/image_reloc.h` (header only, `static inline`)
Used by: `src/kernel/nt/mm.c` (`nt_image_relocate`, `mm.c:2068`), userspace build only
Tests: `tests/kernel-host/image_reloc_test.c` (synthesized images, and the MinGW fixtures, which carry no base relocations and so exercise the no-op path)
Related: [mm](mm.md), [image_plan](image_plan.md)

## Purpose

Adds the base delta to every fixup named by an image's base relocation directory. It is pure: no allocation, no kernel headers. The caller supplies the image bytes laid out at their RVAs, the preferred base from the header, the actual base of the view, the directory RVA and size, and a scratch bitmap.

Integer arithmetic is modular, so a view placed below the preferred base (negative delta) works the same way as one placed above it.

## Public API

`ir_apply(uint8_t *image, size_t image_len, uint32_t size_of_image, uint64_t preferred_base, uint64_t actual_base, uint32_t dir_rva, uint32_t dir_size, uint8_t *seen, size_t seen_len, uint32_t *applied)` returns an `ir_status_t`.

- `seen` must hold `ir_seen_bytes(size_of_image)` bytes, one bit per image byte. The call clears it.
- `*applied` receives the number of fixups written. ABSOLUTE entries are not counted.
- `dir_rva == 0` or `dir_size == 0` is `IR_OK` with nothing applied. A zero delta is also `IR_OK`.

Status values (`ir_status_t`): `IR_OK`; `IR_ERR_PARAM` (NULL pointer, image shorter than SizeOfImage, or bitmap too small); `IR_ERR_DIR` (directory outside SizeOfImage or wrapping); `IR_ERR_BLOCK` (block size under 8, not a multiple of 4, past the directory, or a trailing fragment); `IR_ERR_PAGE` (block VirtualAddress at or beyond SizeOfImage); `IR_ERR_TYPE` (fixup type not supported); `IR_ERR_TARGET` (a fixup write not fully inside SizeOfImage); `IR_ERR_OVERLAP` (two fixups write the same byte, or a fixup writes into the directory).

Fixup types: ABSOLUTE (0, no-op), HIGH (1), LOW (2), HIGHLOW (3), DIR64 (10). HIGHADJ (4) takes a second word and is refused, as are the reserved types (6 and 11 to 15).

## Two passes

Pass one (`ir_walk` with `apply == 0`) walks the whole directory. It checks every block and fixup, and marks each target byte in `seen`. The directory's own bytes are marked first, so no fixup may rewrite the directory it is read from. Pass one writes nothing.

Pass two (`ir_walk` with `apply == 1`) writes the fixups. Only if pass one succeeded does pass two run. A failed call therefore leaves the image unchanged, and `*applied` is 0.

## How mm.c uses it

- `nt_map_image_view` (`mm.c:2093`) calls `nt_image_relocate` only when the view is not at the preferred base. The view is still read-write at that point, so fixups can land in any segment; each segment takes its final protection after the relocations.
- A relocation failure returns `STATUS_INVALID_IMAGE_FORMAT`, and the map releases the VAD and the host pages it made.
- The `seen` bitmap is allocated with `kzalloc` for the duration of the call and freed afterwards.

## Known gaps

- A relocation directory the planner accepted can still be malformed. Pass one refuses it, and the map then fails. Nothing in the planner checks the directory first ([image_plan](image_plan.md)).
- HIGHADJ is refused. A real image that uses it would fail to map.
- The MinGW fixtures used by the tests carry no base relocations, so the relocation path is tested mainly with synthesized images.
- The kernel build never applies relocations to an image (its `SEC_IMAGE` path is not built).
