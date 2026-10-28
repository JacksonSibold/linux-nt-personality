# Parallel work: who owns which area

Two Claude sessions commit to `main` at the same time. To keep merges trivial, each area below has one owner at a time. A
session that needs a change in another session's area makes the smallest edit it can and says so in the commit message.
Always `git pull --rebase origin main` before a push, and push small commits.

Status date: 2026-10-28.

| Area | Files | Owner |
|---|---|---|
| C runtime and Universal CRT | `src/crt/`, `src/ntdll/ucrtbase*`, `tests/crt-host/`, `build/gen_ucrtbase.py` | session A (CRT, kernel32) |
| kernel32 | `src/ntdll/kernel32*`, `src/ntdll/k32_*_pure.h` | session A |
| ntdll Rtl library | `src/ntdll/rtl_*.h`, `src/ntdll/ntdll_rtl*.c`, `tests/ntdll-host/test_rtl_*.c`, `src/ntdll/ntdll_exports.txt` (Rtl block) | session B (reuse from Wine and ReactOS) |
| Driver host: ntoskrnl and hal exports | `src/kernel/nt/driver/`, `docs/status/DRIVERHOST-EXPORTS.md` | session B |
| Reuse record | `docs/policy/THIRD-PARTY.md` | session B (session A appends rows if it copies anything) |

Shared hot files, edited by both: `README.md` counts, `tests/ntdll-host/Makefile`, `src/ntdll/Makefile`,
`tools/run_all_checks.sh`, `docs/status/WIN32-USERLAND.md`. Edit these in a separate, small commit and rebase right away.
