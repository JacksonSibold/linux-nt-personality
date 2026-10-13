# 0045: Canonical copy of the NT sources and warning policy (arch B-2, B-3)

Status: Proposed. Needs an owner decision (see the questions at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` B-2 (Medium) and B-3 (Medium).
Date: 2026-10-12

## Context

- B-3: `vm/kbuild.sh:73-79` copies `src/kernel/nt/.` over `arch/x86/nt/`, so the `src/` copy wins for every file. `vm/rebuild_kdir.sh:138-151` skips some files and gives `mm.c` to the `src/` copy. The two copies of `mm.c` differ by about 155 lines, and `dispatch.c:304` differs by one call.
- B-2: `arch/x86/nt/Makefile:19` sets `-w`, which hides every warning. `Makefile:20-35` lists objects whose sources exist only in `src/`.

## Options

1. Keep `src/kernel/nt` as the one canonical copy, make both scripts copy it the same way, and delete the `arch/` copy. Tradeoff: `arch/` becomes an empty overlay, and the review findings must be re-checked against the `src/` copy.
2. Keep `arch/x86/nt` canonical and change the scripts to match. Tradeoff: the `src/` copy is deleted, and its later changes must be ported.
3. For warnings: replace `-w` with `-Wall` (or `-Wall -Werror`), and fix the warnings it shows.

## Recommendation (proposal)

Option 1 for the copies, because `vm/kbuild.sh` already gives it precedence. Option 3 with `-Wall` first, then `-Werror` when the warnings are fixed.

## Patch sketch

- `vm/rebuild_kdir.sh`: use the same copy rule as `vm/kbuild.sh`.
- `arch/x86/nt/Makefile:19`: `ccflags-y += -Wall`.

## Test plan

- Both scripts produce an identical build directory (compare hashes of the copied files).
- Kernel build with `-Wall` shows the warnings for review, not hidden.
- Needs a kernel build to confirm which `mm.c` runs (VM).

## Not verified

Not built or run. The review was static.

## Owner questions

Which copy of the NT personality sources is canonical, and may compiler warnings be turned back on for this directory?
