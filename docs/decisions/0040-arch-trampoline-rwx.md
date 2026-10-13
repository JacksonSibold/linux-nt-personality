# 0040: Synthetic trampoline page is RWX and never unmapped (arch P-5)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` P-5 (Medium).
Date: 2026-10-12

## Context

`arch/x86/nt/proc_thread.c:1076` maps the trampoline with `PROT_READ|PROT_WRITE|PROT_EXEC`. The comment at `:1058-1066` acknowledges the W^X violation. `nt_free_teb_stack_kernel` (`:1049-1058`) frees the TEB and stack but not the trampoline, which is never recorded, so each thread creation leaks one RWX user page.

## Options

1. Map `PROT_READ|PROT_EXEC` after writing the stub, record the address in `nt_ethread`, and `vm_munmap` it on thread exit. Tradeoff: a per-thread record and a layout change in `nt_ethread`.
2. Keep RWX and document it. Tradeoff: the leak and the W^X violation remain.

## Recommendation (proposal)

Option 1. It fixes both the leak and the protection, and the record is needed anyway.

## Patch sketch

- `proc_thread.c:1076`: write the stub while RW, then `mprotect` to RX (the protection call is the owner's choice).
- Add a trampoline field to `nt_ethread`; free it in `nt_free_teb_stack_kernel`.

## Test plan

- Create and exit 1000 threads; the count of RWX pages in `/proc/self/maps` is zero and the mapping count does not grow.
- The trampoline still runs (thread start works). Needs a kernel build to check (VM).

## Not verified

Not built or run.

## Owner question

Is a per-thread trampoline record in `nt_ethread` (a layout change) acceptable, with the page made read and execute only?
