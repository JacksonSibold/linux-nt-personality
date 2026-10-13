# 0067: _exit spins on hlt and discards the status (ntdll RT-6)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/ntdll.md` RT-6 (Low, needs owner).
Date: 2026-10-12

## Context

`src/ntdll/ntdll_runtime.c:199` defines `_exit` as `while(1) { __asm__ volatile("hlt"); }`. `hlt` is privileged, so in user mode it faults rather than exiting, and the status is discarded. `ExitProcess` (`kernel32.c:172`) calls `NtTerminateProcess` before its `hlt` fallback. Calling `NtTerminateProcess` from `test_runtime` would leave an undefined symbol.

## Options

1. Call `NtTerminateProcess(NtCurrentProcess(), c)` and keep `hlt` only as the fallback, as `ExitProcess` does. Tradeoff: the runtime needs the symbol at link time.
2. Provide a weak `NtTerminateProcess` stub for host tests, and the real one in the NT build. Tradeoff: two definitions to keep in step.

## Recommendation (proposal)

Option 1 in the runtime, with option 2 so the host test links.

## Patch sketch

- `ntdll_runtime.c:199`: call `NtTerminateProcess` with the status, then the fallback.
- Add a weak stub in the host test that records the exit status.

## Test plan

- Host test: `_exit(7)` calls the stub with status 7 and does not return.

## Not verified

Not built or run.

## Owner question

Should `_exit` call `NtTerminateProcess` through a weak symbol that the host test stubs, or through a required symbol?
