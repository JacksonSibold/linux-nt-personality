/*
 * seh.c - __C_specific_handler, the language handler of C functions with __try/__except/__finally on x64.
 *
 * The compiler emits, per function, a SCOPE_TABLE after the handler address in the unwind data:
 *     ULONG Count;
 *     struct { ULONG BeginAddress, EndAddress, HandlerAddress, JumpTarget; } ScopeRecord[Count];   (RVAs)
 * A scope covers the code range [Begin, End). With a JumpTarget it is a __try/__except: HandlerAddress is the filter
 * function (or the constant 1, EXCEPTION_EXECUTE_HANDLER, for a filter that always accepts) and JumpTarget the first
 * instruction of the __except body. Without one it is a __try/__finally and HandlerAddress is the termination handler.
 * Scopes are listed inner first, so the first match is the innermost.
 *
 * First pass (the exception is being dispatched): the first scope containing the faulting address whose filter
 * accepts gets RtlUnwindEx, which runs the termination handlers of the frames in between (this function again, with
 * the unwind flag) and continues at the __except body. Second pass (unwinding): every termination scope containing
 * the address runs its handler, innermost first, until the scope that holds the unwind target.
 *
 * ScopeIndex in the dispatcher context records how far the second pass got, so that a handler that itself unwinds
 * and comes back here does not run the same handler twice.
 */
#include "except.h"
#include "nt_types.h"
#include "ntstatus.h"
#include "unwind.h"

#include <stdint.h>

typedef struct seh_scope_record {
    ULONG BeginAddress, EndAddress, HandlerAddress, JumpTarget;
} seh_scope_record;

typedef struct seh_scope_table {
    ULONG Count;
    seh_scope_record ScopeRecord[1];
} seh_scope_table;

#define SEH_EXECUTE_HANDLER 1
#define SEH_CONTINUE_SEARCH 0
#define SEH_CONTINUE_EXECUTION (-1)

typedef LONG (*seh_filter_fn)(PEXCEPTION_POINTERS pointers, ULONG64 frame);
typedef void (*seh_termination_fn)(unsigned char abnormal, ULONG64 frame);

EXCEPTION_DISPOSITION __C_specific_handler(PEXCEPTION_RECORD rec, PVOID frame, PCONTEXT context, PVOID dispatcher)
{
    DISPATCHER_CONTEXT *dc = (DISPATCHER_CONTEXT *)dispatcher;
    const seh_scope_table *table = (const seh_scope_table *)dc->HandlerData;
    const ULONG64 base = dc->ImageBase, pc = dc->ControlPc, establisher = (ULONG64)(uintptr_t)frame;
    ULONG i;

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        for (i = dc->ScopeIndex; i < table->Count; ++i) {
            const seh_scope_record *s = &table->ScopeRecord[i];
            if (pc < base + s->BeginAddress || pc >= base + s->EndAddress)
                continue;
            if (s->JumpTarget)
                continue; /* a __try/__except takes no part in unwinding */
            if ((rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) && dc->TargetIp >= base + s->BeginAddress &&
                dc->TargetIp < base + s->EndAddress)
                break; /* the unwind ends inside this scope: it and the outer ones stay live */
            dc->ScopeIndex = i + 1;
            ((seh_termination_fn)(uintptr_t)(base + s->HandlerAddress))(1, establisher);
        }
        return ExceptionContinueSearch;
    }

    for (i = dc->ScopeIndex; i < table->Count; ++i) {
        const seh_scope_record *s = &table->ScopeRecord[i];
        if (pc < base + s->BeginAddress || pc >= base + s->EndAddress)
            continue;
        if (!s->JumpTarget)
            continue; /* a __try/__finally has no say in whether the exception is handled */
        if (s->HandlerAddress != SEH_EXECUTE_HANDLER) {
            EXCEPTION_POINTERS pointers;
            LONG verdict;
            pointers.ExceptionRecord = rec;
            pointers.ContextRecord = context;
            verdict = ((seh_filter_fn)(uintptr_t)(base + s->HandlerAddress))(&pointers, establisher);
            if (verdict == SEH_CONTINUE_SEARCH)
                continue;
            if (verdict == SEH_CONTINUE_EXECUTION)
                return ExceptionContinueExecution;
        }
        /* The filter accepted: unwind to this frame and run the __except body; Rax carries the exception code. */
        RtlUnwindEx(frame, (PVOID)(uintptr_t)(base + s->JumpTarget), rec, (PVOID)(uintptr_t)rec->ExceptionCode,
                    dc->ContextRecord, dc->HistoryTable);
    }
    return ExceptionContinueSearch;
}
