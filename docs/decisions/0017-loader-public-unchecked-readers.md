# 0017: Public unchecked PE readers (L-9)

Status: Proposed. Changes call sites in `src/ntdll`. Recorded only in this batch, not implemented.
Finding: `docs/review/loader.md` L-9 (Low).
Date: 2026-10-12

## Context

`pe_file_u16` and `pe_file_u32` (`src/loader/pe_parse.c:119-127`, declared in `pe_parse.h:132-133`) do no bounds checking. Internal callers check first. The API still invites misuse.

Making them `static`, as the review suggests, is not possible. Callers outside `src/loader` use them:

- `src/ntdll/ldr.c:847` (`pe_file_u16`) and `src/ntdll/ldr.c:952` (`pe_file_u32`)
- `build/test_ntdll_pe.c:220`, `227`, `228` (`pe_file_u32`, `pe_file_u16`)

## Options

1. Make them `static`. Not viable: breaks the callers above.
2. Rename to `pe_file_u16_unchecked` and `pe_file_u32_unchecked`, and add checked wrappers that return `pe_status_t` through `pe_check_read`. Tradeoff: the ntdll call sites change and each needs its own bounds check.
3. Keep the names and document in `pe_parse.h` that the caller must check bounds. Tradeoff: no code change; the risk stays.

## Recommendation

Option 2, as its own change that also updates `src/ntdll/ldr.c`. It is not done in this batch because it changes files outside the loader scope.

## Patch sketch

- `pe_parse.h`: add `pe_status_t pe_file_u32_checked(const pe_context_t *ctx, u32 off, u32 *out);` and the u16 form. Implement with `pe_check_read`.
- Rename the existing two to `_unchecked` and update `ldr.c:847`, `ldr.c:952` and the two test lines to use the checked form, or to call `_unchecked` after a check.

## Test plan

- Host: checked u32 at `size - 4` succeeds, at `size - 3` returns `PE_ERR_BOUNDS`. Same for u16 at `size - 1`.
- Host: `build/test_ntdll_pe.c` still builds after the rename.
- Grep: no call to the unchecked names outside the reviewed sites.

## Not verified

Call sites were located by grep. Each `ldr.c` call site's preceding check was not reviewed here.
