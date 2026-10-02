/*
 * except.h - x64 SEH dispatcher, vectored handlers, signal mapping
 */

#ifndef EXCEPT_NT_H
#define EXCEPT_NT_H

#include "nt_types.h"
#include "unwind.h"
#ifndef __MINGW32__
#include <signal.h>
#else
typedef struct { int dummy; } siginfo_t;
typedef struct { int dummy; } ucontext_t;
#endif
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exception disposition returned by handlers */
typedef enum _EXCEPTION_DISPOSITION {
    ExceptionContinueExecution = 0,
    ExceptionContinueSearch = 1,
    ExceptionNestedException = 2,
    ExceptionCollidedUnwind = 3
} EXCEPTION_DISPOSITION;

/* Flags for ExceptionRecord */
#define EXCEPTION_NONCONTINUABLE 0x1
#define EXCEPTION_UNWINDING      0x2
#define EXCEPTION_EXIT_UNWIND    0x4
#define EXCEPTION_STACK_INVALID  0x8
#define EXCEPTION_NESTED_CALL    0x10
#define EXCEPTION_TARGET_UNWIND  0x20
#define EXCEPTION_COLLIDED_UNWIND 0x40

/* Handler prototype */
typedef EXCEPTION_DISPOSITION (*PEXCEPTION_HANDLER)(
    PEXCEPTION_RECORD ExceptionRecord,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext);

/* For compatibility with unwind handler type */
typedef PEXCEPTION_HANDLER PEXCEPTION_ROUTINE;

/* Vectored handler */
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(PEXCEPTION_POINTERS ExceptionInfo);

/* API */
PVOID AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
ULONG RemoveVectoredExceptionHandler(PVOID Handle);
PVOID AddVectoredContinueHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
ULONG RemoveVectoredContinueHandler(PVOID Handle);

/* Unhandled filter */
typedef LONG (*PTOP_LEVEL_EXCEPTION_FILTER)(PEXCEPTION_POINTERS ExceptionInfo);
PTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(PTOP_LEVEL_EXCEPTION_FILTER Filter);
LONG UnhandledExceptionFilter(PEXCEPTION_POINTERS ExceptionInfo);

/* Dispatch */
EXCEPTION_DISPOSITION RtlDispatchException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord);
void RtlDispatchExceptionWrapper(PEXCEPTION_RECORD rec, PCONTEXT ctx);

/* Signal conversion */
#ifndef __MINGW32__
void signal_to_exception(int sig, siginfo_t *si, void *ucontext, PEXCEPTION_RECORD rec, PCONTEXT ctx);
#else
void signal_to_exception(int sig, void *si, void *ucontext, PEXCEPTION_RECORD rec, PCONTEXT ctx);
#endif
int exception_to_signal(PEXCEPTION_RECORD rec);
void context_to_ucontext(PCONTEXT ctx, void *ucontext);
void ucontext_to_context(void *ucontext, PCONTEXT ctx);

/* Conversion helpers */
NTSTATUS NtRaiseException(PEXCEPTION_RECORD rec, PCONTEXT ctx, BOOLEAN FirstChance);

/* For unwind.c to call */
void _NtRaiseException(PEXCEPTION_RECORD rec, PCONTEXT ctx, int firstChance);

/* Nested/collided handling */
int except_in_dispatch(void);
void except_set_in_dispatch(int v);

/* Install signal handlers (call at process init) */
int except_install_signal_handlers(void);

#ifdef __cplusplus
}
#endif

#endif /* EXCEPT_NT_H */
