# Module: ntdll-except (exception dispatch, vectored handlers, signal mapping)

Sources: `src/ntdll/except.c`, `src/ntdll/except.h`
Related: `ntdll-unwind.md` (frame walking used by dispatch), `ntdll-startup.md` (`KiUserExceptionDispatcher`), `ntdll-win32.md` (`SetUnhandledExceptionFilter` shim in `kernel32.c`), `docs/review/ntdll.md` (findings cited below).
Tests: `src/ntdll/except_test.c` (Linux build; not run in this review), `tests/ntdll-host/test_except.c` (vectored handler return codes).

## Purpose

Routes exceptions to handlers. There are three parts:

1. **Vectored handlers.** A process-wide list of handlers that run before any frame handler (`AddVectoredExceptionHandler`), and a list of continue handlers that run after a frame handler resumes execution (`AddVectoredContinueHandler`).
2. **Frame dispatch.** `RtlDispatchException` walks the stack with `RtlLookupFunctionEntry` and `RtlVirtualUnwind`, calling each frame's language handler until one resumes execution or the walk ends.
3. **Signal conversion** (Linux build only). `signal_to_exception` turns SIGSEGV, SIGFPE and similar signals into `EXCEPTION_RECORD` values and a `CONTEXT`, and `exception_to_signal` maps back.

## Public API (except.h)

- `AddVectoredExceptionHandler(first, handler)`, `RemoveVectoredExceptionHandler(handle)`, `AddVectoredContinueHandler`, `RemoveVectoredContinueHandler`.
- `SetUnhandledExceptionFilter(filter)`, `UnhandledExceptionFilter(info)`.
- `RtlDispatchException(rec, ctx)` returning `EXCEPTION_DISPOSITION`, and `RtlDispatchExceptionWrapper(rec, ctx)`.
- `_NtRaiseException(rec, ctx, firstChance)`, and `NtRaiseException` outside MinGW.
- `signal_to_exception`, `exception_to_signal`, `context_to_ucontext`, `ucontext_to_context` (Linux build).
- `except_in_dispatch` and `except_set_in_dispatch`: a per-thread dispatch flag, `__thread` outside MinGW.

Constants: `EXCEPTION_NONCONTINUABLE`, `EXCEPTION_UNWINDING`, `EXCEPTION_EXIT_UNWIND`, and the other `EXCEPTION_*` flags from `except.h`. `EXCEPTION_DISPOSITION` values: `ExceptionContinueExecution` (0), `ExceptionContinueSearch` (1), `ExceptionNestedException` (2), `ExceptionCollidedUnwind` (3).

## Behaviour as implemented

**Vectored handlers.** Nodes form a doubly linked list. `First != 0` prepends, otherwise the node is appended. A handle is a sequence number, and `Remove*` returns 1 when the handle is found. Handlers are called with a `EXCEPTION_POINTERS` pair. A handler that returns -1 (`EXCEPTION_CONTINUE_EXECUTION`) makes `RtlDispatchException` return `ExceptionContinueExecution` at once. Returning 0 continues the search.

**Frame dispatch.** After the vectored stage, the walk starts at `ctx.Rip` and runs for at most 128 frames. For each frame it:
- looks up the `RUNTIME_FUNCTION`;
- calls `RtlVirtualUnwind` on a copy to get the handler and the establisher frame;
- calls the handler with the record, establisher, context and a `DISPATCHER_CONTEXT`;
- on `ExceptionContinueExecution`, copies the context back, runs the continue handlers, and returns;
- otherwise unwinds the real context one frame and continues.

When the walk ends without a resume, `UnhandledExceptionFilter` is called and the function returns `ExceptionContinueSearch`.

**Nested dispatch.** A global depth counter stops recursion beyond 16 levels.

**Signals** (Linux build). `sig_handler` is installed by `except_install_signal_handlers`. It classifies the fault address against a guard page list and decodes the access type from the faulting instruction bytes.

## Invariants and locking

- The handler lists and the nesting depth are process-wide and unsynchronised (EXC-4). Callers must not add or remove handlers concurrently with dispatch.
- Handler nodes are freed on removal, even when the dispatch loop might still reference them (EXC-5).

## Known gaps and defects (see docs/review/ntdll.md)

- **No unwind pass.** Termination handlers and `__finally` blocks never run (EXC-3). `RtlUnwindExWrapper` is a partial implementation with no caller (EXC-6).
- **Leaf frames stop the walk** (EXC-2). A PC with no `RUNTIME_FUNCTION` gives no unwind, and the walk spins until its frame limit.
- **Unsynchronised global state** (EXC-4).
- **Exit status** from `_NtRaiseException` loses the NTSTATUS (EXC-7).
- The vectored-continue value bug (EXC-1) is fixed in `655c9ca`.
