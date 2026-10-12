# 0012: Error returns after the point of no return in PE exec (K-4)

Status: Proposed. Kernel source (`src/kernel/fs`). Recorded only in this batch, not implemented.
Finding: `docs/review/kernel-fs.md` K-4 (High).
Date: 2026-10-12

## Context

`load_pe_binary` calls `begin_new_exec()` at `src/kernel/fs/binfmt_pe.c:906`. After that call the old mm is gone. These later failures still `goto out` and return an error: `setup_arg_pages` (915), `pe_pick_base` (924), `pe_map_sections` (927), `pe_apply_relocs` (930), `pe_setup_stack` (933), `pe_build_process_parameters` (936), `pe_init_peb` (939), `pe_init_teb` (942), and the TEB range check (1028-1029). The exec core cannot roll back a committed exec.

## Options

1. Move every fallible step before `begin_new_exec`. Tradeoff: map sections, relocations and stack need a staging address space. Large change.
2. After commit, on failure call `force_sigsegv(SIGSEGV)` and return 0, as `load_elf_binary` does for post-commit errors. Tradeoff: the process dies with SIGSEGV instead of getting an errno. Small change.
3. Leave as is. Not acceptable: it returns an error with a torn address space and a stale `bprm`.

## Recommendation

Option 2 now, through one helper (for example `pe_post_commit_fail`) used by every site above. Option 1 for the cheap checks that do not depend on the new mm: the TEB range check (1028-1029) and the allocations, which can move above line 906. Other kernel findings that add failure points after commit (K-10, K-13) depend on this one.

## Patch sketch

- Add `pe_post_commit_fail(const char *what, int err)`: `pr_err` (with `%p`, not `%px`; see K-16) and `force_sigsegv(SIGSEGV)`, return 0.
- Replace each `goto out` after line 906 with a call to it.
- Move the TEB range check and any pure-arithmetic checks above line 906.

## Test plan

- Kernel test with a fault-injection hook (for example a module parameter that fails `pe_map_sections` once): the exec returns 0, the task receives SIGSEGV, and the old mm is not reused.
- Host: not applicable, the code is kernel-only.

## Not verified

Not built or run. Kernel behavior after `load_binary` returns an error post-commit is from the general exec contract, and `fs/exec.c` is not in this tree.
