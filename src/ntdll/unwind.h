/*
 * unwind.h - x86-64 SEH table-driven unwind (RtlVirtualUnwind) for ntpers
 *
 * Implements:
 *  - RUNTIME_FUNCTION binary search (sorted .pdata)
 *  - UNWIND_INFO / UNWIND_CODE interpretation for all UWOP codes
 *  - Mid-prologue handling (SizeOfProlog / CodeOffset)
 *  - Chained unwind info
 *  - Leaf functions
 *  - RtlVirtualUnwind, RtlLookupFunctionEntry, RtlCaptureContext,
 *    RtlUnwindEx, RtlRaiseException
 *
 * Reference: Microsoft x64 exception handling docs, winnt.h, pe_format.h
 */

#ifndef UNWIND_H
#define UNWIND_H

#include "nt_types.h"
#include "../loader/pe_format.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Re-export unwind op codes for tests */
#define UWOP_PUSH_NONVOL       0
#define UWOP_ALLOC_LARGE       1
#define UWOP_ALLOC_SMALL       2
#define UWOP_SET_FPREG         3
#define UWOP_SAVE_NONVOL       4
#define UWOP_SAVE_NONVOL_FAR   5
#define UWOP_SAVE_XMM128       8
#define UWOP_SAVE_XMM128_FAR   9
#define UWOP_PUSH_MACHFRAME    10

#define UNW_FLAG_EHANDLER  0x01
#define UNW_FLAG_UHANDLER  0x02
#define UNW_FLAG_CHAININFO 0x04

/* Mirrors Windows DISPATCHER_CONTEXT */
typedef struct _DISPATCHER_CONTEXT {
    ULONG64 ControlPc;
    ULONG64 ImageBase;
    IMAGE_RUNTIME_FUNCTION_ENTRY *FunctionEntry;
    ULONG64 EstablisherFrame;
    ULONG64 TargetIp;
    PCONTEXT ContextRecord;
    PVOID LanguageHandler;
    PVOID HandlerData;
    PVOID HistoryTable;
} DISPATCHER_CONTEXT, *PDISPATCHER_CONTEXT;

/* KNONVOLATILE_CONTEXT_POINTERS - points to where nonvolatile registers were saved.
 * Windows defines this as struct with pointers to each register slot. We provide subset. */
typedef struct _KNONVOLATILE_CONTEXT_POINTERS {
    PULONG64 IntegerContext[16]; /* Rax..R15 indexed by register number */
    PM128A   FloatingContext[16]; /* Xmm0..15 */
} KNONVOLATILE_CONTEXT_POINTERS, *PKNONVOLATILE_CONTEXT_POINTERS;

/* Module registration for tests and loader.
 * In the real OS the image is mapped at ImageBase; for tests the image memory
 * is a malloc buffer and ImageBase is the preferred virtual address (e.g. 0x140000000).
 * UnwindInfoAddress RVAs are resolved as image + RVA.
 */
int unwind_register_module(ULONG64 imageBase, ULONG sizeOfImage,
                           IMAGE_RUNTIME_FUNCTION_ENTRY *funcTable, ULONG funcCount,
                           uint8_t *image);
void unwind_clear_modules(void);
int unwind_module_count(void);

/* Lookup */
IMAGE_RUNTIME_FUNCTION_ENTRY *RtlLookupFunctionEntry(ULONG64 ControlPc, PULONG64 ImageBase, PVOID HistoryTable);
PVOID RtlLookupFunctionEntryEx(ULONG64 ControlPc, PULONG64 ImageBase, PVOID HistoryTable);

/* Virtual unwind - returns exception handler address or NULL. EstablisherFrame is set.
 * ContextRecord is updated to caller context (RIP/RSP and nonvolatiles restored).
 * HandlerData receives language-specific data if any.
 */
PVOID RtlVirtualUnwind(ULONG HandlerType, ULONG64 ImageBase, ULONG64 ControlPc,
                       IMAGE_RUNTIME_FUNCTION_ENTRY *FunctionEntry,
                       PCONTEXT ContextRecord, PVOID *HandlerData,
                       PULONG64 EstablisherFrame,
                       PKNONVOLATILE_CONTEXT_POINTERS ContextPointers);

/* Additional helpers defined by spec */
void RtlCaptureContext(PCONTEXT ContextRecord);
void RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);
void RtlUnwindEx(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord,
                 PVOID ReturnValue, PCONTEXT ContextRecord, PVOID HistoryTable);

/* Low-level helper for tests: unwind one frame given explicit FunctionEntry */
int unwind_one_frame(PCONTEXT ctx, ULONG64 imageBase, IMAGE_RUNTIME_FUNCTION_ENTRY *entry,
                     uint8_t *image, ULONG64 *establisherFrame);

#ifdef __cplusplus
}
#endif

#endif /* UNWIND_H */
