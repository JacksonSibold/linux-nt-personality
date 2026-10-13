# Module: image_plan (SEC_IMAGE layout planner)

Sources: `src/kernel/nt/image_plan.h` (header only, `static inline`)
Depends on: `src/loader/pe_parse.h` and `pe_parse.c` (validation of the PE headers and section table)
Used by: `src/kernel/nt/mm.c` (userspace build only, `nt_create_image_section` at `mm.c:1939`)
Tests: `tests/kernel-host/image_plan_test.c` (synthesized images and the MinGW fixtures built by `tests/host`)
Related: [mm](mm.md) (the section that keeps the plan), [image_reloc](image_reloc.md) (applies relocations to the mapped copy)

## Purpose

Turns a whole PE file into the mapping plan that `NtCreateSection(SEC_IMAGE)` records and `NtMapViewOfSection` applies. The planner is pure: no kernel headers, no allocation, no I/O. The caller reads the file into memory and passes the bytes in.

The plan is one header region followed by one segment per section, in RVA order. Each segment has a page protection and a zero-filled tail. A section with no VirtualSize and no raw data maps nothing and is left out.

The kernel build does not include this header. Its `NtCreateSection(SEC_IMAGE)` returns `STATUS_NOT_IMPLEMENTED`.

## Public API

- `ip_plan_build(const u8 *data, size_t file_size, ip_plan_t *out)` returns an `ip_status_t`. On `IP_OK`, `*out` is fully written and `out->num_segments` is set.
- `ip_prot_from_characteristics(u32)` maps section characteristics to an `ip_prot_t`. A writable section is copy-on-write (`IP_PROT_WRITECOPY`, or `IP_PROT_EXECUTE_WRITECOPY` with X). Otherwise X gives `EXECUTE` or `EXECUTE_READ`, R alone gives `READONLY`, and no flag gives `NOACCESS`.
- `ip_status_from_pe(pe_status_t)` maps a `pe_validate` failure: section errors to `IP_ERR_LAYOUT`, bounds errors to `IP_ERR_RAW_RANGE`, anything else to `IP_ERR_FORMAT`.
- `ip_peek_magic(const u8 *d, size_t n, u16 *magic)` reads the optional-header magic before any other check.

Status values (`ip_status_t`): `IP_OK`, `IP_ERR_PARAM`, `IP_ERR_FORMAT`, `IP_ERR_PE32` (32-bit optional header), `IP_ERR_ALIGN` (ImageBase zero or not 64 KiB aligned, or a raw pointer not FileAlignment-aligned), `IP_ERR_UNSUPPORTED_ALIGN` (SectionAlignment below a page), `IP_ERR_LAYOUT`, `IP_ERR_RAW_RANGE` and `IP_ERR_BASE` (image outside user space).

`mm.c` maps these to NT statuses in `ip_status_to_nt` (`mm.c:1906`): `IP_ERR_UNSUPPORTED_ALIGN` to `STATUS_NOT_IMPLEMENTED`, `IP_ERR_PARAM` to `STATUS_INVALID_PARAMETER`, and the rest to `STATUS_INVALID_IMAGE_FORMAT`. A 32-bit image is therefore `STATUS_INVALID_IMAGE_FORMAT` here, because its own NT status is not defined in this tree.

## Data structures

- `ip_segment_t` - `rva`, `size` (a multiple of SectionAlignment), `file_offset`, `file_size` (bytes copied from the file, at most `size`), `zero_size` (the zero-filled tail), `prot`, and `is_header`.
- `ip_plan_t` - `preferred_base`, `size_of_image`, `size_of_headers`, `section_alignment`, `file_alignment`, `entry_rva`, `relocatable` (a base-relocation directory exists and the image is not `RELOCS_STRIPPED`), `relocs_stripped`, `reloc_rva`, `reloc_size`, `num_segments`, and `seg[IP_MAX_SECTIONS + 1]`.
- Limits: `IP_MAX_SECTIONS` is 96. `IP_USER_LIMIT` is `0x00007FFFFFF0000`, the exclusive top of the image range. `IP_ALLOC_GRANULARITY` is 64 KiB and `IP_PAGE_SIZE` is 4 KiB.

## Rules, in the order the build applies them

1. `data` and `out` are non-NULL and the file is not empty (`IP_ERR_PARAM`).
2. The file starts with `MZ` (otherwise `IP_ERR_FORMAT`). A 32-bit optional-header magic returns `IP_ERR_PE32` before any other check.
3. `pe_context_init` and `pe_validate` run on the bytes. Their failures map through `ip_status_from_pe`.
4. SectionAlignment is at least a page. ImageBase is non-zero and 64 KiB aligned. The image fits below `IP_USER_LIMIT`.
5. Segment 0 is the header region: SizeOfHeaders rounded up to a page, read-only, with the file supplying SizeOfHeaders bytes.
6. Each section is checked. Its RVA is SectionAlignment-aligned, the mapped size (VirtualSize rounded up to SectionAlignment, or SizeOfRawData when VirtualSize is 0) fits inside SizeOfImage, and a non-zero raw size needs a FileAlignment-aligned pointer and raw data inside the file. The file supplies `min(raw, VirtualSize)` bytes.
7. Sections are sorted by RVA (insertion sort over segments 1 to n-1).
8. Every pair of segments, the header included, must not overlap once rounded to pages.

## Known gaps

- The planner does not check that the relocation directory is well formed. That check is done when relocations are applied ([image_reloc](image_reloc.md)).
- Only 64-bit images are planned. A 32-bit image is refused.
- The kernel build does not build this header, so the planner has no kernel-branch test.
- The plan is kept in the section object and freed by `nt_section_delete`. The section keeps a file descriptor for the bytes that are copied at map time.
