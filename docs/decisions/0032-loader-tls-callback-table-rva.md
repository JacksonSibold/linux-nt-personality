# 0032: TLS callback table read at its preferred-base address (LDR-4)

Status: Implemented in bef4d79 (LDR-4). The translation and bounded entry read are `ldr_tls_cb_table_rva` and `ldr_tls_cb_entry` in `src/ntdll/ldr_pure.h`, used by `ldr_run_tls_callbacks`. Host test: `tests/ntdll-host/test_ldr_pure.c`. Deviation from the sketch: a table outside the image skips the callbacks and does not fail the load; failing the load is part of LDR-3 (0031). The relocated-base case still needs VM.
Finding: `docs/review/ntdll.md` LDR-4 (High).
Date: 2026-10-12

## Context

`ldr_map_and_load` reads the TLS directory from the file, before relocation:

- `src/ntdll/ldr.c:1001-1004`: `pe_tls_directory(&mod->pe, &tls)` fills `IMAGE_TLS_DIRECTORY64 tls`, which is copied into `mod->tls` with `has_tls = 1`.
- `pe_tls_directory` reads from the file, so `tls.AddressOfCallBacks` is a VA based on the image's preferred base (`pe.image_base`, which `ldr.c:958` stores as `entry.OriginalBase`).

`ldr_run_tls_callbacks` (`src/ntdll/ldr.c:1485`) then uses that VA directly:

- Line 1491: `ULONG_PTR cb_table_va = (ULONG_PTR)mod->tls.AddressOfCallBacks;`
- Line 1499: `ULONG_PTR *table = (ULONG_PTR *)cb_table_va;` and the loop at line 1500 reads `table[i]` until zero.
- The comment at lines 1494-1497 says the stored value "is already the actual VA". That holds only for an image loaded at its preferred base.

Effect: for any image not mapped at its preferred base, the read goes to an unrelated address in the host process. A wrong or unmapped address faults in the loader. A mapped one yields garbage callbacks, which then run. Both paths are reached from `ldr_process_attach` (line 1529).

The loop also has no bound. It stops only at a zero entry, so a table without a terminator reads past the image.

## Options

1. **Convert to an image RVA and read from `mod->image`.** `rva = AddressOfCallBacks - OriginalBase`. Check that the VA lies at or above `OriginalBase` and that `rva + 8 <= image_size` before reading. Read entries from `mod->image + rva`, and stop at zero or at the end of the image. Tradeoff: one subtraction and one range check, with no change to the parser.
2. **Read the TLS directory from the mapped image after relocation.** Call `pe_tls_directory` on a view of `mod->image`, or copy the directory from `mod->image + tls_dir_rva`. Tradeoff: the directory then comes from the relocated image, so `AddressOfCallBacks` is already the actual VA. This is correct too, but it changes where `mod->tls` is filled, which means moving code out of `ldr_map_and_load`.
3. **Store the RVA at parse time.** Keep only the RVA in `mod->tls` and convert once. Tradeoff: changes the struct and every reader of `mod->tls`; not needed for this fix.

## Recommendation

Option 1. It is local to `ldr_run_tls_callbacks`, needs no change to the mapper, and adds the bound check that the current loop lacks. Option 2 is the cleaner long-term shape and can follow when the TLS directory is reworked for LDR-1.

## Patch sketch

In `ldr_run_tls_callbacks` (`src/ntdll/ldr.c:1485-1507`), replace the body after the `has_tls` check with:

```c
ULONG_PTR cb_table_va = (ULONG_PTR)mod->tls.AddressOfCallBacks;
if (cb_table_va == 0) return;
if (cb_table_va < (ULONG_PTR)mod->entry.OriginalBase) return;   /* or fail the load */
ULONG_PTR rva64 = cb_table_va - (ULONG_PTR)mod->entry.OriginalBase;
if (rva64 > mod->image_size || mod->image_size - rva64 < sizeof(ULONG_PTR)) return;
u32 rva = (u32)rva64;
for (size_t i = 0; ; ++i) {
    if (!pe_range_fits(rva, (i + 1) * sizeof(ULONG_PTR), mod->image_size)) break;
    ULONG_PTR cb_va;
    memcpy(&cb_va, mod->image + rva + i * sizeof(ULONG_PTR), sizeof(cb_va));
    if (cb_va == 0) break;
    PIMAGE_TLS_CALLBACK cb = (PIMAGE_TLS_CALLBACK)cb_va;
    ...observer hook as now...
}
```

Use `pe_range_fits` from `src/ntdll/pe_bounds.h` (the same helper as LDR-5 and LDR-6). Reject a `AddressOfCallBacks` below `OriginalBase` as an invalid image: a TLS callback table that is not in the image is malformed, so `ldr_process_attach` should fail. For this sketch, the function returns early, and the attach-failure path from 0031 covers the fail-the-load option.

The callback pointers themselves are already relocated (they live in the image, and their DIR64 entries are covered by the relocation pass), so the values read from `mod->image` are final VAs.

## Test plan

- Host (needs the fixture harness from 0030): a module loaded at a base other than its preferred base, with a two-entry callback table. The observer hook sees the two relocated callback addresses, in order.
- Host: `AddressOfCallBacks` below `OriginalBase` is not dereferenced; no hook call is made.
- Host: a table with no terminator, ending at `image_size`. The loop stops at the image end; no read past it (run under the host sanitizer if available).
- Host: `AddressOfCallBacks` at `image_size - 4`. No read; no hook call.
- Needs VM: a MinGW TLS-callback DLL loaded at a non-preferred base (forced by a conflicting mapping). The callback runs and sets a flag the test exe reads.

## Not verified

- `pe_tls_directory` (`src/loader/pe_parse.c:1284`) returns the raw file fields through `pe_read_tls_directory64` and does no base subtraction. This was read from the return path, not run against a fixture.
- The interaction with the TLS index slot and `_tls_index` (not used by the loader) was not checked.
