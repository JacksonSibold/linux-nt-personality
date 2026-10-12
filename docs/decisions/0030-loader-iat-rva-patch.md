# 0030: IAT slot writes use a file offset, not an RVA (LDR-2)

Status: Proposed. Recorded only, not implemented. Needs test harness: the fix is small, but no host test can reach `ldr_resolve_imports` yet (see Test plan).
Finding: `docs/review/ntdll.md` LDR-2 (High).
Date: 2026-10-12

## Context

The loader keeps two address spaces for a mapped module:

- `mod->pe.data` and `mod->file_data` hold the file bytes. `pe_rva_to_offset` (`src/loader/pe_parse.c:730`) returns a file offset.
- `mod->image` holds the image laid out by RVA (`VirtualAddress`), as `ldr_map_and_load` copies sections there.

`ldr_resolve_imports` (`src/ntdll/ldr.c:1292`) mixes the two:

- Line 1344: `pe_rva_to_offset(&mod->pe, desc.FirstThunk, &iat_off)` gives a file offset.
- Line 1346: when `FirstThunk` is 0, `iat_off = ilt_off`, which is also a file offset.
- Line 1350: `iat_slot_off = iat_off + t * 8` is still a file offset.
- Line 1376: `memcpy(mod->image + iat_slot_off, &addr, 8)` writes the resolved address at a file offset inside the RVA-laid image.

The delay-import path (`ldr_resolve_delay_imports`, line 1387) has the same mix. Lines 1446 and 1451 and the write at line 1474 (in the delay loop from line 1387) compute `iat_off` from `ImportAddressTableRVA` through `pe_rva_to_offset` and then write to `mod->image`. The module-handle slot at lines 1438-1440 is already RVA-based (`mod->image + desc.ModuleHandleRVA`) and correct.

Effect: wherever `PointerToRawData` differs from `VirtualAddress` for the `.idata` section, the resolved address is written to the wrong place in the image. The call through the IAT then jumps to an unrelated value. The bug is hidden on images where the two layouts match, and this is why the existing tests do not show it.

The ILT reads (`ilt_off`) are correct: they read from the file, through `pe_read_thunk_data64`.

## Options

1. **Keep the RVA, write at the RVA.** Keep `iat_rva = desc.FirstThunk` (or `ImportAddressTableRVA` for delay imports) and write to `mod->image + iat_rva + t * 8`. Read the ILT through the file offset as now. Tradeoff: two variables per import, which is clear. The change is local to the two loops.
2. **Translate the image offset back.** Keep the file offset and write through an inverse map from file offset to image RVA. Tradeoff: needs a second conversion table, and the inverse is not unique for sections with overlapping raw ranges. Not recommended.
3. **Copy the IAT out and back.** Build the IAT in a file-side buffer, then copy it into the image at the RVA. Tradeoff: an extra buffer and copy, with no gain over option 1.

## Recommendation

Option 1, applied to both loops. Also write the `FirstThunk == 0` fallback as an RVA (`ilt_rva`) rather than the ILT file offset, so that the fallback writes to the same RVA space as the rest.

## Patch sketch

In `ldr_resolve_imports` (`src/ntdll/ldr.c:1341-1376`):

```c
u32 ilt_rva = desc.DUMMYUNIONNAME.OriginalFirstThunk ? desc.DUMMYUNIONNAME.OriginalFirstThunk : desc.FirstThunk;
u32 iat_rva = desc.FirstThunk ? desc.FirstThunk : ilt_rva;
/* ilt_off: file offset of ilt_rva, as now */
...
u32 iat_slot_rva = iat_rva + t * sizeof(IMAGE_THUNK_DATA64);
/* the bounds check from 0017 / pe_bounds.h: iat_slot_rva + 8 <= mod->image_size */
memcpy(mod->image + iat_slot_rva, &addr, sizeof(addr));
```

The delay path changes the same way: `iat_rva = desc.ImportAddressTableRVA`, and the write at line 1474 uses `mod->image + iat_slot_rva`.

The bound check is required, because an RVA now indexes the image directly. Use `pe_range_fits(iat_slot_rva, 8, mod->image_size)` from `src/ntdll/pe_bounds.h`. Return `STATUS_INVALID_IMAGE_FORMAT` when it fails. This also closes a write past the image through a crafted `FirstThunk`.

## Test plan

- Needs test harness (host): the loader does not expose `ldr_resolve_imports`, and the Linux build needs a PE fixture with an import whose `PointerToRawData` differs from `VirtualAddress` in `.idata`. Building that fixture needs `x86_64-w64-mingw32-gcc` (not installed here), or a hand-written PE image built in memory. Either is a separate change.
- Host, once a harness exists: fixture with `.idata` raw offset != RVA. After `ldr_resolve_imports`, `*(u64 *)(mod->image + FirstThunk)` equals the export address, and the bytes at the file-offset location are unchanged.
- Host: a fixture with `FirstThunk` set to `image_size - 4`. The load returns `STATUS_INVALID_IMAGE_FORMAT` and writes nothing.
- Host: the delay-import path with the same fixture shape.
- Needs VM: a MinGW-built test DLL with a relocated `.idata`, imported by a test exe that calls the imported function through its IAT.

## Not verified

- The pe_parse layout claim (`pe_rva_to_offset` returns a file offset) was checked against the function's contract in `src/loader/pe_parse.c:730` and its uses, not by running a fixture.
- Whether any existing Linux test (`ldr_test.c`) depends on the buggy offset was not checked, because the file needs the MinGW compiler to run.
