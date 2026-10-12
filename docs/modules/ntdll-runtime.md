# Module: ntdll-runtime (freestanding string, memory and stub library)

Sources: `src/ntdll/ntdll_runtime.c`
Tests: `tests/ntdll-host/test_runtime.c` (allocator overflow and zeroing; built with `-fno-builtin`)
Related: `ntdll-loader.md` and `ntdll-startup.md` (callers), `docs/review/ntdll.md` (findings cited below).

## Purpose

ntdll is linked with `-nostdlib` and cannot import a C library. `ntdll_runtime.c` supplies the libc functions the loader, unwinder and exception code need: memory and string functions, a bump allocator, and stubs for the stdio functions. The whole file is compiled only under `__MINGW32__`. On a Linux host the libc versions are used instead.

## Provided functions

- **Memory:** `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`.
- **Strings:** `strlen`, `wcslen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcasecmp`, `strncasecmp`, `strchr`, `strrchr`, `strdup`.
- **Allocation:** `malloc`, `calloc`, `realloc`, `free`. The allocator is a bump allocator over a static 2 MiB array (`HEAP_SIZE`), with 16-byte rounding.
- **Stubs:** `sscanf`, `snprintf`, `vsnprintf`, `printf`, `fopen`, `fclose`, `fgets`, `fprintf`. These do nothing useful (see RT-3).
- **Support:** `__chkstk`, `_chkstk`, `___chkstk_ms`, `_alloca` (empty), `_errno`, `errno`, `_exit`.

## Behaviour as implemented

- `malloc(0)` is treated as `malloc(1)`. A request above `HEAP_SIZE` returns NULL before rounding, and the bound check is repeated after rounding.
- `calloc` rejects `n * sz` that overflows `size_t`, then allocates and zeroes. This was fixed in `9c733c3` (RT-1).
- `free` does nothing. The heap is never reused.
- `realloc(p, sz)` allocates `sz` bytes and copies `sz` bytes from `p`. The block size is not recorded, so a larger `sz` reads past the old block (RT-2).
- `strncpy` and `strncat` follow the C standard. `strncmp` and `strncasecmp` stop at the first NUL.
- `_exit` halts in a loop on `hlt`, which faults in user mode (RT-6).

## Invariants

- The heap is a single static array. Every allocation comes from `g_heap`, and `g_heap_off` only grows.
- `errno` is a plain global, not per thread.

## Known gaps (see docs/review/ntdll.md)

- **realloc over-read** (RT-2).
- **Stub printf family** writes nothing and returns 0, so output and errors are lost silently (RT-3).
- **errno declaration clashes with MinGW headers** under `-Werror` (RT-4). `tests/ntdll-host/Makefile` relaxes `-Wattributes` for this object only.
- **No reuse of freed memory**, and a 2 MiB limit shared by the whole ntdll (RT-5).
- **_exit uses hlt** (RT-6).
- The allocator is not thread-safe: `g_heap_off` is updated without a lock, so two threads can receive the same block.
