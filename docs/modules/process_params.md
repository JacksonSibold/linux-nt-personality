# Module: process_params (process parameter block builder and validator)

Sources: `src/kernel/nt/process_params.h` (header only, `static inline`)
Tests: `tests/kernel-host/process_params_test.c`
Not called by any service. Process-creation milestone M1 (host part), see [process-creation](../architecture/process-creation.md).
Related: [proc_create_args](proc_create_args.md) (the NtCreateUserProcess argument copy-in)

## Purpose

Defines a relocatable block that carries the four strings a new process needs from `RTL_USER_PROCESS_PARAMETERS`: ImagePathName, CommandLine, CurrentDirectory and Environment. Every string is stored as UTF-16 bytes at an offset from the start of the block, never as a pointer. The block can therefore be copied into another address space and used there unchanged. The planned consumer is `binfmt_pe`, which would read the block from an inherited memfd (milestone M1, not built).

The builder and validator are pure. They take no lock, allocate nothing and touch only the buffers passed in. The caller copies user memory into a kernel buffer first.

## Format (version 1)

All fields are little-endian ULONGs, offsets from the block start:

| Offset | Field |
|---|---|
| 0x00 | `Magic`, `PP_MAGIC` ("PPR1") |
| 0x04 | `Version`, `PP_VERSION` (1) |
| 0x08 | `TotalSize`, the length of the whole block |
| 0x0C | `Flags`, must be 0 |
| 0x10, 0x14 | ImagePath offset, length in bytes |
| 0x18, 0x1C | CommandLine offset, length in bytes |
| 0x20, 0x24 | CurrentDir offset, length in bytes |
| 0x28, 0x2C | Environment offset, length in bytes |
| 0x30 | `Reserved[4]`, must be 0 |
| 0x40 | string data, packed in the order above, each string 2-byte aligned |

Limits: `PP_MAX_CHARS` is 0x7FFF (the `UNICODE_STRING` limit, 0xFFFE bytes). `PP_MAX_BLOB_SIZE` is 0x50000 (320 KiB).

## Builder: PpBuildParamsBlob

Input is `struct pp_params_in`: `image` (required, 1 to `PP_MAX_CHARS` characters, no NUL), `command_line` (optional, no NUL), `current_dir` (optional, no NUL) and `environment` (optional; when set it must end in two NULs).

Normalization:

- An absent environment is stored as the canonical empty block, two NUL characters.
- A non-empty current directory without a trailing backslash gets one. A current directory that would exceed `PP_MAX_CHARS` after the backslash is added returns `STATUS_NAME_TOO_LONG`.

`*out_len` always receives the size the block needs, so a caller can pass `out = NULL` and `out_cap = 0` to query it. Errors: `STATUS_INVALID_PARAMETER` for a malformed input, `STATUS_NAME_TOO_LONG` for a string or block over its limit, and `STATUS_BUFFER_TOO_SMALL` when `out_cap` is smaller than the block.

## Validator: PpValidateParamsBlob

Accepts only what the builder could have produced in shape, and fills a `struct pp_params_view` that points into the block. The checks, in order:

1. Length between `PP_HEADER_SIZE` and `PP_MAX_BLOB_SIZE`, and even. A wrong length is `STATUS_INFO_LENGTH_MISMATCH`.
2. The block is 2-byte aligned.
3. Magic and version match; `TotalSize` equals the length (`STATUS_INFO_LENGTH_MISMATCH` otherwise); `Flags` and the reserved words are zero.
4. Each range has an even offset and length, the length is within its maximum, the range starts at or after the header and ends inside the block, and no two non-empty ranges overlap.
5. The image is at least one character and has no NUL. The command line, if present, has no NUL. A current directory, if present, has no NUL and ends in a backslash.
6. The environment is at least two characters and ends in two NULs.

Any failure is `STATUS_INVALID_PARAMETER`, except the length and total-size checks above.

## Known gaps

- Nothing calls the builder or the validator outside the tests. `binfmt_pe` does not read the block (M1 is open).
- `ProcessParameters` in `NtCreateUserProcess` is not read. The stub in `proc_thread.c` ignores it, because no consumer exists yet ([proc_create_args](proc_create_args.md)).
- The format is not checked against Windows' `RTL_USER_PROCESS_PARAMETERS` layout; it is this tree's own block. The header says so.
