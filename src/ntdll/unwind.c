/*
 * unwind.c - x64 SEH unwinder
 *
 * See unwind.h for API. Implements full UNWIND_INFO interpretation including
 * chained info, mid-prologue, leaf functions, and all UWOP codes required by
 * the workstream.
 *
 * Notes on Windows semantics reproduced:
 *  - RUNTIME_FUNCTION entries are sorted by BeginAddress; EndAddress low 2 bits are flags.
 *  - UNWIND_INFO.CountOfCodes counts 16-bit slots (including extra slots for large offsets).
 *  - UnwindCode array is sorted descending by CodeOffset; final entry padded to even count.
 *  - EstablisherFrame = base of fixed allocation (lowest address of frame).
 *  - Offsets for SAVE_* are scaled (8 for integer, 16 for XMM) when small, unscaled when FAR.
 *  - FrameRegister offset is scaled by 16.
 *  - UWOP_PUSH_MACHFRAME simulates hardware trap frame (40 or 48 bytes).
 */

#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "unwind.h"
#include "except.h"
#include "unwind_pure.h"
#include "nt_types.h"
#include "ntstatus.h"
#include <stddef.h>
#include "../loader/pe_format.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME

/* We replicate CONTEXT register indexing used by UNWIND_CODE OpInfo.
 * 0=RAX 1=RCX 2=RDX 3=RBX 4=RSP 5=RBP 6=RSI 7=RDI 8=R8 9=R9 10=R10 11=R11 12=R12 13=R13 14=R14 15=R15
 */
static ULONG64 *ctx_gpr_ptr(PCONTEXT ctx, unsigned reg) {
    switch (reg) {
        case 0: return &ctx->Rax;
        case 1: return &ctx->Rcx;
        case 2: return &ctx->Rdx;
        case 3: return &ctx->Rbx;
        case 4: return &ctx->Rsp;
        case 5: return &ctx->Rbp;
        case 6: return &ctx->Rsi;
        case 7: return &ctx->Rdi;
        case 8: return &ctx->R8;
        case 9: return &ctx->R9;
        case 10: return &ctx->R10;
        case 11: return &ctx->R11;
        case 12: return &ctx->R12;
        case 13: return &ctx->R13;
        case 14: return &ctx->R14;
        case 15: return &ctx->R15;
        default: return NULL;
    }
}

/* XMM pointer */
static M128A *ctx_xmm_ptr(PCONTEXT ctx, unsigned reg) {
    if (reg < 16) return &ctx->DUMMYUNIONNAME.FltSave.XmmRegisters[reg];
    return NULL;
}

/* ---------- Module registry ---------- */

#define MAX_MODULES 64
typedef struct {
    ULONG64 imageBase;
    ULONG sizeOfImage;
    IMAGE_RUNTIME_FUNCTION_ENTRY *funcTable;
    ULONG funcCount;
    uint8_t *image; /* malloc base for RVA resolution; if NULL use imageBase as pointer */
} unwind_module_t;

static unwind_module_t g_modules[MAX_MODULES];
static int g_module_count = 0;

int unwind_register_module(ULONG64 imageBase, ULONG sizeOfImage,
                           IMAGE_RUNTIME_FUNCTION_ENTRY *funcTable, ULONG funcCount,
                           uint8_t *image) {
    if (g_module_count >= MAX_MODULES) return -1;
    /* Keep sorted by imageBase for quick lookup? Not needed, linear scan is fine for tests. */
    g_modules[g_module_count].imageBase = imageBase;
    g_modules[g_module_count].sizeOfImage = sizeOfImage;
    g_modules[g_module_count].funcTable = funcTable;
    g_modules[g_module_count].funcCount = funcCount;
    g_modules[g_module_count].image = image;
    g_module_count++;
    return 0;
}

int unwind_unregister_table(IMAGE_RUNTIME_FUNCTION_ENTRY *funcTable) {
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i].funcTable != funcTable) continue;
        for (int j = i; j + 1 < g_module_count; j++) g_modules[j] = g_modules[j + 1];
        g_module_count--;
        memset(&g_modules[g_module_count], 0, sizeof(g_modules[0]));
        return 0;
    }
    return -1;
}

void unwind_clear_modules(void) {
    g_module_count = 0;
    memset(g_modules, 0, sizeof(g_modules));
}

int unwind_module_count(void) { return g_module_count; }

/* Translate RVA to pointer within module's image.
 * If module->image is non-NULL, RVA is offset into that buffer.
 * Otherwise we treat imageBase as direct pointer (for real mapped images where VA==pointer).
 */
static uint8_t *rva_to_ptr(unwind_module_t *mod, ULONG rva) {
    if (mod->image) return mod->image + rva;
    return (uint8_t *)(uintptr_t)(mod->imageBase + rva);
}

/* Binary search within a single module's table.
 * Table is sorted by BeginAddress. x64 EndAddress has no flag bits (UNW-4).
 * Returns entry where Begin <= pc < End, or NULL.
 */
static IMAGE_RUNTIME_FUNCTION_ENTRY *lookup_in_module(unwind_module_t *mod, ULONG64 pc) {
    /* sizeOfImage == 0 means unknown (RtlAddFunctionTable): the table bounds the match. */
    ULONG rva = 0;
    if (!uwp_module_rva(mod->imageBase, mod->sizeOfImage, pc, &rva)) return NULL;
    long idx = uwp_find_function(mod->funcTable, mod->funcCount, rva);
    return idx < 0 ? NULL : &mod->funcTable[idx];
}

IMAGE_RUNTIME_FUNCTION_ENTRY *RtlLookupFunctionEntry(ULONG64 ControlPc, PULONG64 ImageBase, PVOID HistoryTable) {
    (void)HistoryTable;
    for (int i = 0; i < g_module_count; i++) {
        IMAGE_RUNTIME_FUNCTION_ENTRY *e = lookup_in_module(&g_modules[i], ControlPc);
        if (e) {
            if (ImageBase) *ImageBase = g_modules[i].imageBase;
            return e;
        }
    }
    if (ImageBase) *ImageBase = 0;
    return NULL;
}

PVOID RtlLookupFunctionEntryEx(ULONG64 ControlPc, PULONG64 ImageBase, PVOID HistoryTable) {
    return RtlLookupFunctionEntry(ControlPc, ImageBase, HistoryTable);
}

/* Find module by ImageBase */
static unwind_module_t *find_module_by_base(ULONG64 imageBase) {
    for (int i = 0; i < g_module_count; i++) if (g_modules[i].imageBase == imageBase) return &g_modules[i];
    return NULL;
}

/* Helper to get UNWIND_INFO pointer from FunctionEntry */
static UNWIND_INFO *get_unwind_info(unwind_module_t *mod, IMAGE_RUNTIME_FUNCTION_ENTRY *entry) {
    ULONG rva = entry->DUMMYUNIONNAME.UnwindInfoAddress;
    return (UNWIND_INFO *)rva_to_ptr(mod, rva);
}

/* Chained info: follow UNW_FLAG_CHAININFO from the entry the PC resolved to
 * until the primary UNWIND_INFO (flag clear). chain[0] is the resolved entry,
 * chain[depth-1] the primary. The concatenated code sequence is the codes of
 * chain[0], then chain[1], and so on (x64 exception handling, "Unwind
 * procedure"). Each chained entry's prolog test uses its own Begin and
 * SizeOfProlog. */

#define MAX_CHAIN 8

typedef struct {
    UNWIND_INFO *info;
    IMAGE_RUNTIME_FUNCTION_ENTRY *entry;
    unwind_module_t *mod;
} chain_elem_t;

static int build_chain(unwind_module_t *mod, IMAGE_RUNTIME_FUNCTION_ENTRY *entry, chain_elem_t *chain, int *outDepth) {
    int depth = 0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *curEntry = entry;
    unwind_module_t *curMod = mod;
    while (depth < MAX_CHAIN) {
        UNWIND_INFO *info = get_unwind_info(curMod, curEntry);
        chain[depth].info = info;
        chain[depth].entry = curEntry;
        chain[depth].mod = curMod;
        depth++;
        if (!(info->Flags & UNW_FLAG_CHAININFO)) break;
        /* Chained entry follows codes */
        unsigned aligned = uwp_chain_slots(info->CountOfCodes);
        /* After codes, there is RUNTIME_FUNCTION */
        IMAGE_RUNTIME_FUNCTION_ENTRY *chained = (IMAGE_RUNTIME_FUNCTION_ENTRY *)&info->UnwindCode[aligned];
        /* chained unwind info address is in chained->UnwindInfoAddress; Begin/End in chained */
        /* The chained's Begin/End are RVAs; we need to find unw info for it */
        /* The chained entry's UnwindData is RVA of next info */
        /* Update curEntry to chained */
        curEntry = chained;
        /* mod stays same (image) */
        /* Need to get info for chained entry next iteration; but chained entry itself is not in table,
         * it's embedded. So we use its UnwindData to get info. */
        /* For next iteration, info will be fetched from curEntry's UnwindData */
        /* However curEntry's Begin/End are used for prolog offset check? */
        /* Keep curMod same */
        /* Continue loop; next iteration will fetch info via get_unwind_info */
        /* We need to keep curEntry as pointer to chained struct (stack allocated? no, it's inside previous info)
         * So point curEntry to that embedded entry. For next get_unwind_info, it will use curEntry->UnwindInfoAddress
         * which points to primary's info.
         */
        /* But we have allocated chained inside previous info's memory, which is fine as we keep pointer. */
        /* Need to ensure we don't infinite loop */
        if (depth >= MAX_CHAIN) break;
        /* Fetch next info for checking flag */
        UNWIND_INFO *nextInfo = (UNWIND_INFO *)rva_to_ptr(curMod, chained->DUMMYUNIONNAME.UnwindInfoAddress);
        (void)nextInfo;
        /* Loop will fetch it next iteration via get_unwind_info */
    }
    *outDepth = depth;
    return 0;
}

/* The code offset of the SET_FPREG operation of an unwind info, or -1 when it has none. Steps over multi-slot codes. */
static int uw_set_fpreg_offset(const UNWIND_INFO *info)
{
    unsigned i = 0;
    while (i < info->CountOfCodes) {
        unsigned op = info->UnwindCode[i].DUMMYSTRUCTNAME.UnwindOp;
        unsigned oi = info->UnwindCode[i].DUMMYSTRUCTNAME.OpInfo;
        if (op == UWOP_SET_FPREG)
            return info->UnwindCode[i].DUMMYSTRUCTNAME.CodeOffset;
        switch (op) {
        case UWOP_ALLOC_LARGE: i += oi == 0 ? 2 : 3; break;
        case UWOP_SAVE_NONVOL:
        case UWOP_SAVE_XMM128: i += 2; break;
        case UWOP_SAVE_NONVOL_FAR:
        case UWOP_SAVE_XMM128_FAR: i += 3; break;
        default: i += 1; break;
        }
    }
    return -1;
}

/* Unwind one frame using chain.
 * Returns 0 on success.
 */
int unwind_one_frame(PCONTEXT ctx, ULONG64 imageBase, IMAGE_RUNTIME_FUNCTION_ENTRY *entry,
                     uint8_t *image, ULONG64 *establisherFrame) {
    /* Leaf: no RUNTIME_FUNCTION, so pop the return address from RSP. This must
     * not depend on a registered module: an unknown PC is still a leaf (EXC-2). */
    if (!entry) {
        ULONG64 rsp = ctx->Rsp;
        ULONG64 ret = *(ULONG64 *)(uintptr_t)rsp;
        ctx->Rip = ret;
        ctx->Rsp = rsp + 8;
        if (establisherFrame) *establisherFrame = rsp;
        return 0;
    }

    /* Find module */
    unwind_module_t *mod = find_module_by_base(imageBase);
    if (!mod) {
        /* Fallback: if image provided directly, create temp module */
        if (image) {
            unwind_module_t tmp;
            tmp.imageBase = imageBase;
            tmp.image = image;
            tmp.funcTable = entry;
            tmp.funcCount = 1;
            tmp.sizeOfImage = 0x1000000;
            mod = &tmp;
            /* we will not use build_chain's need for module array; handle locally */
            /* Instead handle simple case without chain for fallback */
            goto simple;
        }
        return -1;
    }
simple:
    if (!mod) return -1;

    ULONG64 controlPc = ctx->Rip;

    /* Build chain */
    chain_elem_t chain[MAX_CHAIN];
    int depth = 0;
    UNWIND_INFO *infos[MAX_CHAIN];
    IMAGE_RUNTIME_FUNCTION_ENTRY *entries[MAX_CHAIN];
    int infoCount = 0;
    build_chain(mod, entry, chain, &depth);
    for (int i = 0; i < depth; i++) {
        infos[i] = chain[i].info;
        entries[i] = chain[i].entry;
    }
    infoCount = depth;

    /* Determine prolog handling per chain element that contains ControlPc.
     * For each info, compute ripOffset = controlPc - (imageBase + entry->Begin)
     * If ripOffset < SizeOfProlog => in prolog, so only codes with CodeOffset <= ripOffset have executed.
     * If chained and controlPc not in that entry's range, we treat as not in prolog? But for shrink-wrapping,
     * secondary's range may be contiguous? We'll check: if controlPc outside entry's range, then its codes
     * shouldn't apply? However secondary is invoked only if controlPc in secondary's range OR primary? Spec says
     * chained info used for noncontiguous segments; then lookup would have returned the secondary's entry directly.
     * Since we started from lookup result, controlPc is within initial entry's range. For subsequent chained primaries,
     * their Begin may be different; but we still need to apply their codes fully (not prolog-limited) because
     * controlPc is not in their Begin range? Actually primary's Begin is start of function, which always contains controlPc
     * if secondary is shrink-wrap? So primary contains controlPc as well, so its prolog check will be based on offset from primary Begin.
     * That's correct.
     */

    /* Track RSP evolution */
    ULONG64 rsp = ctx->Rsp;
    ULONG64 establisher = 0;
    int useFrame = 0;
    unsigned frameReg = 0;
    unsigned frameOffsetScaled = 0;
    /* Need frame register index if any info has FrameRegister !=0. Primary's frame reg is authoritative.
     * If chain, they must match primary's per spec.
     */
    int fpInfoIdx = -1;
    for (int i = infoCount - 1; i >= 0; i--) {
        if (infos[i]->FrameRegister != 0) {
            useFrame = 1;
            frameReg = infos[i]->FrameRegister;
            frameOffsetScaled = infos[i]->FrameOffset;
            fpInfoIdx = i;
            break;
        }
    }

    /* The establisher frame is the base of the fixed stack allocation: the stack pointer once the prologue has run.
     * With a frame register that is the register minus its scaled offset (read before any code restores the register);
     * otherwise it is the stack pointer at the call. Frame handlers locate the function's locals from it. A PC still
     * inside the prologue, before the frame register is set, falls back to the stack pointer. */
    establisher = rsp;
    if (useFrame && fpInfoIdx >= 0) {
        ULONG64 begin = imageBase + entries[fpInfoIdx]->BeginAddress;
        ULONG64 off = controlPc >= begin ? controlPc - begin : 0;
        int established = !uwp_in_prolog(controlPc, begin, infos[fpInfoIdx]->SizeOfProlog);
        if (!established) {
            int sf = uw_set_fpreg_offset(infos[fpInfoIdx]);
            established = sf >= 0 && (ULONG64)sf <= off;
        }
        if (established) {
            ULONG64 *fpReg = ctx_gpr_ptr(ctx, frameReg);
            if (fpReg)
                establisher = uwp_fp_base(*fpReg, frameOffsetScaled);
        }
    }

    /* For SAVE_* address calculation we need base for offset.
     * If using frame register, base = value of frame register - frameOffsetScaled*16.
     * Otherwise base = rsp (current rsp before any unwind? Actually after prolog, rsp points to top,
     * and fixed allocation base = rsp + something? Let's use approach: base for non-FP is rsp after all allocs?
     * But during unwind we are incrementally adjusting rsp, so base should be final establisher frame.
     * Simpler: compute base as establisher candidate: For non-FP, establisher = rsp + total alloc? But we adjust rsp stepwise.
     * For SAVE offsets, Windows defines offset from base of fixed allocation (the lowest address before any dynamic alloca?).
     * In our incremental unwind, after we undo allocs, rsp moves up, but save slots are at fixed offset from base,
     * which we can compute as establisher = rsp (current) after all allocs undone? Hmm ordering.
     *
     * Standard approach seen in implementations: For SAVE_NONVOL, address = establisher + offset.
     * Where establisher = rsp (for non-FP) or FP - offset*16 (for FP). But during unwind, establisher is final
     * value after unwinding allocs. However SAVE slots are within frame, so to restore we can compute address as
     * establishFrame + offset.
     *
     * But establisher is unknown until we process allocs. However we can compute establisher early as rsp + totalFixedAlloc?
     * Alternative: process codes in order they were executed, but for SAVE we use current rsp plus offset?
     *
     * Simpler to follow Wine's logic: Keep rsp variable, and for SAVE_NONVOL with offset O, location = rsp + O*8
     * where rsp is current rsp at that point in unwind? Let's test with example:
     * Prolog: sub rsp, 0x20 ; mov [rsp+0x10], rbx . So rsp after alloc is originalRsp -0x20,
     * and rbx saved at rsp+0x10 (which is originalRsp -0x10). When unwinding stepwise starting from current rsp (= originalRsp -0x20),
     * if we first undo SAVE (rbx = *(rsp+0x10)), we get correct. Then undo ALLOC (rsp+=0x20) => restored.
     * So SAVE address should be rsp + offset where rsp is current rsp at that moment (before alloc undo). That yields originalRsp -0x20 +0x10 = originalRsp -0x10 correct.
     * If we did reverse order (SAVE after ALLOC undo), then rsp would be originalRsp, and rsp+0x10 = originalRsp+0x10 wrong.
     * So order matters: In array descending order, which appears first? For this example, push/alloc then save? The CodeOffsets: alloc at offset maybe 3, save at offset 5 etc? Not sure order.
     * Let's assume array order is descending offset, which corresponds to reverse prolog order: last prolog op first in array.
     * If save occurs after alloc in prolog, then save's offset larger than alloc's? Actually prolog sequence: alloc then save. So save has larger offset than alloc. So in descending array, save appears before alloc. Thus processing array 0..n will do save before alloc undo, which matches above correct order (save before alloc).
     * So using current rsp for save address works if we process in array order.
     */

    /* Order (MS x64 spec, "Unwind procedure"): the walk starts at the
     * UNWIND_INFO the PC resolved to and, when its codes run out, links to the
     * parent and walks that array next, until the primary (no CHAININFO flag).
     * infos[0] is the entry the PC resolved to, infos[depth-1] the primary, so
     * the codes are applied in index order 0..depth-1, each in array order. */

    /* Context pointers tracking */
    PKNONVOLATILE_CONTEXT_POINTERS ctxPtrs = NULL; /* not used now */

    /* First pass: handle frame register establishment?
     * Actually SET_FPREG handling needs frame register value. We'll handle within loop.
     */

    for (int ci = 0; ci < infoCount; ci++) {
        UNWIND_INFO *info = infos[ci];
        IMAGE_RUNTIME_FUNCTION_ENTRY *curEntry = entries[ci];
        ULONG64 funcBegin = imageBase + curEntry->BeginAddress;
        ULONG64 ripOffset = controlPc >= funcBegin ? controlPc - funcBegin : 0;
        int inProlog = uwp_in_prolog(controlPc, funcBegin, info->SizeOfProlog);

        /* Find start index for prolog case */
        int startIdx = 0;
        if (inProlog) {
            /* First code with CodeOffset <= ripOffset */
            int found = uwp_prolog_start(info->UnwindCode, info->CountOfCodes, ripOffset);
            if (found == -1) {
                /* No codes have executed yet, skip this info entirely */
                continue;
            }
            startIdx = found;
        }

        /* Iterate codes from startIdx to CountOfCodes, but we need to handle slot stepping */
        for (unsigned idx = (unsigned)startIdx; idx < info->CountOfCodes; ) {
            UNWIND_CODE uc = info->UnwindCode[idx];
            unsigned op = uc.DUMMYSTRUCTNAME.UnwindOp;
            unsigned infoOp = uc.DUMMYSTRUCTNAME.OpInfo;
            unsigned nextIdx = idx + 1;

            switch (op) {
                case UWOP_PUSH_NONVOL: {
                    /* Restore register from stack, increment RSP */
                    ULONG64 *loc = (ULONG64 *)(uintptr_t)rsp;
                    ULONG64 val = *loc;
                    ULONG64 *reg = ctx_gpr_ptr(ctx, infoOp);
                    if (reg) *reg = val;
                    if (ctxPtrs) ctxPtrs->IntegerContext[infoOp] = loc;
                    rsp += 8;
                    break;
                }
                case UWOP_ALLOC_SMALL: {
                    rsp += uwp_alloc_small_size(infoOp);
                    break;
                }
                case UWOP_ALLOC_LARGE: {
                    if (infoOp == 0) {
                        /* next slot holds size/8 */
                        if (nextIdx >= info->CountOfCodes) break;
                        UNWIND_CODE nxt = info->UnwindCode[nextIdx];
                        rsp += uwp_alloc_large_size8(nxt.FrameOffset);
                        nextIdx++;
                    } else if (infoOp == 1) {
                        if (nextIdx + 1 >= info->CountOfCodes) break;
                        UNWIND_CODE n1 = info->UnwindCode[nextIdx];
                        UNWIND_CODE n2 = info->UnwindCode[nextIdx+1];
                        rsp += uwp_far_operand(n1.FrameOffset, n2.FrameOffset);
                        nextIdx += 2;
                    } else {
                        /* reserved */
                    }
                    break;
                }
                case UWOP_SET_FPREG: {
                    /* RSP = FrameReg - FrameOffset*16 */
                    if (useFrame) {
                        ULONG64 *fpReg = ctx_gpr_ptr(ctx, frameReg);
                        if (fpReg) rsp = uwp_fp_base(*fpReg, frameOffsetScaled);
                    }
                    break;
                }
                case UWOP_SAVE_NONVOL: {
                    if (nextIdx >= info->CountOfCodes) break;
                    UNWIND_CODE nxt = info->UnwindCode[nextIdx];
                    ULONG64 offset = uwp_save_nonvol_offset(nxt.FrameOffset);
                    ULONG64 addr;
                    if (useFrame) {
                        ULONG64 *fpReg = ctx_gpr_ptr(ctx, frameReg);
                        ULONG64 fp = fpReg ? *fpReg : 0;
                        addr = uwp_fp_base(fp, frameOffsetScaled) + offset;
                    } else {
                        addr = rsp + offset;
                    }
                    ULONG64 val = *(ULONG64 *)(uintptr_t)addr;
                    ULONG64 *reg = ctx_gpr_ptr(ctx, infoOp);
                    if (reg) *reg = val;
                    if (ctxPtrs && reg) ctxPtrs->IntegerContext[infoOp] = (ULONG64 *)(uintptr_t)addr;
                    nextIdx++;
                    break;
                }
                case UWOP_SAVE_NONVOL_FAR: {
                    if (nextIdx + 1 >= info->CountOfCodes) break;
                    UNWIND_CODE n1 = info->UnwindCode[nextIdx];
                    UNWIND_CODE n2 = info->UnwindCode[nextIdx+1];
                    ULONG64 offset = uwp_far_operand(n1.FrameOffset, n2.FrameOffset);
                    ULONG64 addr;
                    if (useFrame) {
                        ULONG64 *fpReg = ctx_gpr_ptr(ctx, frameReg);
                        ULONG64 fp = fpReg ? *fpReg : 0;
                        addr = uwp_fp_base(fp, frameOffsetScaled) + offset;
                    } else {
                        addr = rsp + offset;
                    }
                    ULONG64 val = *(ULONG64 *)(uintptr_t)addr;
                    ULONG64 *reg = ctx_gpr_ptr(ctx, infoOp);
                    if (reg) *reg = val;
                    if (ctxPtrs) ctxPtrs->IntegerContext[infoOp] = (ULONG64 *)(uintptr_t)addr;
                    nextIdx += 2;
                    break;
                }
                case UWOP_SAVE_XMM128: {
                    if (nextIdx >= info->CountOfCodes) break;
                    UNWIND_CODE nxt = info->UnwindCode[nextIdx];
                    ULONG64 offset = uwp_save_xmm_offset(nxt.FrameOffset);
                    ULONG64 addr;
                    if (useFrame) {
                        ULONG64 *fpReg = ctx_gpr_ptr(ctx, frameReg);
                        ULONG64 fp = fpReg ? *fpReg : 0;
                        addr = uwp_fp_base(fp, frameOffsetScaled) + offset;
                    } else {
                        addr = rsp + offset;
                    }
                    M128A val = *(M128A *)(uintptr_t)addr;
                    M128A *xreg = ctx_xmm_ptr(ctx, infoOp);
                    if (xreg) *xreg = val;
                    if (ctxPtrs) ctxPtrs->FloatingContext[infoOp] = (M128A *)(uintptr_t)addr;
                    nextIdx++;
                    break;
                }
                case UWOP_SAVE_XMM128_FAR: {
                    if (nextIdx + 1 >= info->CountOfCodes) break;
                    UNWIND_CODE n1 = info->UnwindCode[nextIdx];
                    UNWIND_CODE n2 = info->UnwindCode[nextIdx+1];
                    ULONG64 offset = uwp_far_operand(n1.FrameOffset, n2.FrameOffset);
                    ULONG64 addr;
                    if (useFrame) {
                        ULONG64 *fpReg = ctx_gpr_ptr(ctx, frameReg);
                        ULONG64 fp = fpReg ? *fpReg : 0;
                        addr = uwp_fp_base(fp, frameOffsetScaled) + offset;
                    } else {
                        addr = rsp + offset;
                    }
                    M128A val = *(M128A *)(uintptr_t)addr;
                    M128A *xreg = ctx_xmm_ptr(ctx, infoOp);
                    if (xreg) *xreg = val;
                    if (ctxPtrs) ctxPtrs->FloatingContext[infoOp] = (M128A *)(uintptr_t)addr;
                    nextIdx += 2;
                    break;
                }
                case UWOP_PUSH_MACHFRAME: {
                    int hasError = infoOp;
                    ULONG64 newRip;
                    ULONG64 newRsp;
                    newRip = *(ULONG64 *)(uintptr_t)(rsp + uwp_machframe_rip_offset(hasError));
                    newRsp = *(ULONG64 *)(uintptr_t)(rsp + uwp_machframe_rsp_offset(hasError));
                    rsp += uwp_machframe_size(hasError);
                    ctx->Rip = newRip;
                    /* For machframe, RSP is restored to saved old RSP, not just incremented?
                     * Spec says push is simulated, so unwinding should set RSP to oldRSP
                     * But if oldRSP is same as rsp+40? For same stack, oldRsp == rsp+40
                     * So both give same. We'll use oldRsp.
                     */
                    rsp = newRsp;
                    /* Also restore? EFLAGS etc not needed */
                    break;
                }
                default: {
                    /* Unknown op - skip */
                    break;
                }
            }
            idx = nextIdx;
        }
    }

    /* After processing all allocs/saves/pushes/frame, handle return address pop.
     * For non-leaf, the return address is at [RSP].
     */
    /* The last step of unwind is to pop RIP. But need to consider if last op was MACHFRAME, RIP already set.
     * For normal functions, RIP is at [RSP].
     */
    /* Detect if last chain had MACHFRAME - we already handled RIP. Otherwise pop. */
    int hadMachFrame = 0;
    for (int i = 0; i < infoCount; i++) {
        if (uwp_has_machframe(infos[i]->UnwindCode, infos[i]->CountOfCodes)) { hadMachFrame = 1; break; }
    }
    if (!hadMachFrame) {
        ULONG64 ret = *(ULONG64 *)(uintptr_t)rsp;
        ctx->Rip = ret;
        rsp += 8;
    }
    ctx->Rsp = rsp;
    if (establisherFrame) *establisherFrame = establisher;
    /* Also restore RSP via frame handling: already done */
    return 0;
}

PVOID RtlVirtualUnwind(ULONG HandlerType, ULONG64 ImageBase, ULONG64 ControlPc,
                       IMAGE_RUNTIME_FUNCTION_ENTRY *FunctionEntry,
                       PCONTEXT ContextRecord, PVOID *HandlerData,
                       PULONG64 EstablisherFrame,
                       PKNONVOLATILE_CONTEXT_POINTERS ContextPointers) {
    // HandlerType selects which handler kind is returned (UNW-3).
    (void)ControlPc;
    if (!ContextRecord) return NULL;
    unwind_module_t *mod = find_module_by_base(ImageBase);
    uint8_t *image = NULL;
    if (mod) image = mod->image;
    else {
        /* No module registered - try to treat ImageBase as pointer? For tests with direct blobs,
         * FunctionEntry's UnwindInfoAddress may be RVA relative to malloc image.
         * But we need image pointer. If caller passes imageBase but no module, we cannot resolve.
         * Try fallback where image == (uint8_t*)ImageBase?
         */
        image = (uint8_t *)(uintptr_t)ImageBase;
        /* But for pre-registered modules, ImageBase is virtual 0x140..., not pointer. So we need mod.
         * If not found, treat as leaf?
         */
    }

    PVOID handler = NULL;
    ULONG64 establisher = 0;

    if (!FunctionEntry) {
        unwind_one_frame(ContextRecord, ImageBase, NULL, image, &establisher);
        if (EstablisherFrame) *EstablisherFrame = establisher;
        if (HandlerData) *HandlerData = NULL;
        return NULL;
    }

    UNWIND_INFO *info = NULL;
    if (mod) info = get_unwind_info(mod, FunctionEntry);
    else {
        /* fallback direct */
        info = (UNWIND_INFO *)(image + FunctionEntry->DUMMYUNIONNAME.UnwindInfoAddress);
    }

    /* Determine handler */
    if (info && (info->Flags & HandlerType)) {
        /* Handler RVA after codes */
        unsigned aligned = uwp_chain_slots(info->CountOfCodes);
        ULONG handlerRVA = *(ULONG *)&info->UnwindCode[aligned];
        if (handlerRVA) {
            if (mod) handler = rva_to_ptr(mod, handlerRVA);
            else handler = (PVOID)(image + handlerRVA);
        }
        if (HandlerData) {
            /* Handler data follows handler RVA */
            ULONG *p = (ULONG *)&info->UnwindCode[aligned];
            /* If handler present, data is after 1 ULONG */
            if (handlerRVA) *HandlerData = (PVOID)(p + 1);
            else *HandlerData = (PVOID)p;
        }
    } else {
        if (HandlerData) *HandlerData = NULL;
    }

    /* Handle chained handler? Chained info has no handler per spec (flags cleared) */
    /* But if chained info's primary has handler, it will be found via chain? For now handle only top-level */

    unwind_one_frame(ContextRecord, ImageBase, FunctionEntry, image, &establisher);
    if (EstablisherFrame) *EstablisherFrame = establisher;
    if (ContextPointers) {
        /* We didn't fill but could zero */
        memset(ContextPointers, 0, sizeof(*ContextPointers));
    }
    return handler;
}

/* ---- Capturing a context: RtlCaptureContext and RtlRaiseException --------------------------------------------------------- */

/*
 * RtlCaptureContext stores the registers of its caller as they were at the call: Rip is the return address, Rsp the stack
 * pointer after the return, and every other register has its value at entry (the function uses none before storing them).
 * It is written in assembly because the caller's state is exactly the state at entry. The first argument is in RCX on
 * Windows and in RDI on the host's System V ABI.
 *
 * RtlRaiseException builds a context of its caller the same way, calls _NtRaiseException with it, and then continues
 * the caller through the possibly edited context, as a handler's ExceptionContinueExecution requires.
 *
 * Offsets are those of CONTEXT (nt_types.h asserts the first ones; the x87/SSE area starts at 0x100 and Xmm0 at 0x1A0).
 */
#if defined(__MINGW32__)
#define UW_A0 "%rcx"
#define UW_A1 "%rdx"
#define UW_A2 "%r8"
#define UW_A2D "%r8d"
#define UW_A1D "%edx"
#define UW_ASM_FUNC(name) ".globl " #name "\n\t.def " #name "; .scl 2; .type 32; .endef\n" #name ":\n\t"
#else
#define UW_A0 "%rdi"
#define UW_A1 "%rsi"
#define UW_A2 "%rdx"
#define UW_A2D "%edx"
#define UW_A1D "%esi"
#define UW_ASM_FUNC(name) ".globl " #name "\n\t.type " #name ",@function\n" #name ":\n\t"
#endif

#define UW_CTX_FLAGS 0x30
#define UW_CTX_MXCSR 0x34
#define UW_CTX_RIP 0xF8
#define UW_CTX_RSP 0x98
_Static_assert(offsetof(CONTEXT, ContextFlags) == UW_CTX_FLAGS && offsetof(CONTEXT, MxCsr) == UW_CTX_MXCSR &&
                   offsetof(CONTEXT, Rip) == UW_CTX_RIP && offsetof(CONTEXT, Rsp) == UW_CTX_RSP &&
                   offsetof(CONTEXT, Rax) == 0x78 && offsetof(CONTEXT, R15) == 0xF0 && offsetof(CONTEXT, DUMMYUNIONNAME.FltSave.XmmRegisters) == 0x1A0 &&
                   offsetof(CONTEXT, EFlags) == 0x44 && offsetof(CONTEXT, SegCs) == 0x38 && sizeof(CONTEXT) == 0x4D0,
               "CONTEXT layout assumed by RtlCaptureContext");

__asm__(".text\n\t"
        UW_ASM_FUNC(RtlCaptureContext)
        "movq %rax, 0x78(" UW_A0 ")\n\t"
        "movq %rcx, 0x80(" UW_A0 ")\n\t"
        "movq %rdx, 0x88(" UW_A0 ")\n\t"
        "movq %rbx, 0x90(" UW_A0 ")\n\t"
        "leaq 8(%rsp), %rax\n\t"
        "movq %rax, 0x98(" UW_A0 ")\n\t"
        "movq %rbp, 0xA0(" UW_A0 ")\n\t"
        "movq %rsi, 0xA8(" UW_A0 ")\n\t"
        "movq %rdi, 0xB0(" UW_A0 ")\n\t"
        "movq %r8,  0xB8(" UW_A0 ")\n\t"
        "movq %r9,  0xC0(" UW_A0 ")\n\t"
        "movq %r10, 0xC8(" UW_A0 ")\n\t"
        "movq %r11, 0xD0(" UW_A0 ")\n\t"
        "movq %r12, 0xD8(" UW_A0 ")\n\t"
        "movq %r13, 0xE0(" UW_A0 ")\n\t"
        "movq %r14, 0xE8(" UW_A0 ")\n\t"
        "movq %r15, 0xF0(" UW_A0 ")\n\t"
        "movq (%rsp), %rax\n\t"
        "movq %rax, 0xF8(" UW_A0 ")\n\t"
        "pushfq\n\t"
        "popq %rax\n\t"
        "movl %eax, 0x44(" UW_A0 ")\n\t"
        "stmxcsr 0x34(" UW_A0 ")\n\t"
        "stmxcsr 0x118(" UW_A0 ")\n\t"
        "fnstcw 0x100(" UW_A0 ")\n\t"
        "movups %xmm0,  0x1A0(" UW_A0 ")\n\t"
        "movups %xmm1,  0x1B0(" UW_A0 ")\n\t"
        "movups %xmm2,  0x1C0(" UW_A0 ")\n\t"
        "movups %xmm3,  0x1D0(" UW_A0 ")\n\t"
        "movups %xmm4,  0x1E0(" UW_A0 ")\n\t"
        "movups %xmm5,  0x1F0(" UW_A0 ")\n\t"
        "movups %xmm6,  0x200(" UW_A0 ")\n\t"
        "movups %xmm7,  0x210(" UW_A0 ")\n\t"
        "movups %xmm8,  0x220(" UW_A0 ")\n\t"
        "movups %xmm9,  0x230(" UW_A0 ")\n\t"
        "movups %xmm10, 0x240(" UW_A0 ")\n\t"
        "movups %xmm11, 0x250(" UW_A0 ")\n\t"
        "movups %xmm12, 0x260(" UW_A0 ")\n\t"
        "movups %xmm13, 0x270(" UW_A0 ")\n\t"
        "movups %xmm14, 0x280(" UW_A0 ")\n\t"
        "movups %xmm15, 0x290(" UW_A0 ")\n\t"
        "movw %cs, 0x38(" UW_A0 ")\n\t"
        "movw %ds, 0x3A(" UW_A0 ")\n\t"
        "movw %es, 0x3C(" UW_A0 ")\n\t"
        "movw %fs, 0x3E(" UW_A0 ")\n\t"
        "movw %gs, 0x40(" UW_A0 ")\n\t"
        "movw %ss, 0x42(" UW_A0 ")\n\t"
        "movl $0x10000F, 0x30(" UW_A0 ")\n\t" /* CONTEXT_FULL | CONTEXT_SEGMENTS */
        "ret\n\t");

__asm__(".text\n\t"
        UW_ASM_FUNC(RtlRaiseException)
        "subq $0x4F8, %rsp\n\t"                   /* shadow space, a CONTEXT at 0x20, the record pointer at 0x4F0 */
        "movq " UW_A0 ", 0x4F0(%rsp)\n\t"
        "leaq 0x20(%rsp), " UW_A0 "\n\t"
        "call RtlCaptureContext\n\t"
        "leaq 0x500(%rsp), %rax\n\t"              /* the caller's stack pointer after the return */
        "movq %rax, 0xB8(%rsp)\n\t"               /* 0x20 + Rsp */
        "movq 0x4F8(%rsp), %rax\n\t"              /* its return address is the faulting address */
        "movq %rax, 0x118(%rsp)\n\t"              /* 0x20 + Rip */
        "movq 0x4F0(%rsp), " UW_A0 "\n\t"
        "movq %rax, 0x10(" UW_A0 ")\n\t"          /* ExceptionRecord->ExceptionAddress */
        "leaq 0x20(%rsp), " UW_A1 "\n\t"
        "movl $1, " UW_A2D "\n\t"
        "call _NtRaiseException\n\t"
        "leaq 0x20(%rsp), " UW_A0 "\n\t"          /* resumed: continue the caller through the edited context */
        "xorl " UW_A1D ", " UW_A1D "\n\t"
        "call RtlRestoreContext\n\t"
        "ud2\n\t");

/* ---- Transferring control: RtlRestoreContext ---------------------------------------------------------------------------- */

/* Loads every register of the context and continues at its Rip with its Rsp. The return frame is built on the current
 * stack and left by IRETQ, which loads the instruction pointer, flags and stack pointer in one step; the registers are
 * loaded first, the one that addresses the context last. The context may live below the target stack pointer; it is
 * not read after the final instruction. Segment registers and flags are not taken blindly from a context built by hand:
 * zero segments mean the user segments, and the flags keep only the arithmetic bits, DF and the always-set bit 1. */
__attribute__((noreturn)) static void uw_resume(const CONTEXT *c)
{
    uint64_t cs = c->SegCs ? c->SegCs : 0x33;
    uint64_t ss = c->SegSs ? c->SegSs : 0x2b;
    uint64_t fl = (c->EFlags & 0xCD5u) | 0x202u;
    uint64_t sp = c->Rsp, ip = c->Rip;
    if ((c->ContextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT) {
        const M128A *x = c->DUMMYUNIONNAME.FltSave.XmmRegisters;
        __asm__ volatile("ldmxcsr %0" : : "m"(c->MxCsr));
        __asm__ volatile("movups 0(%0), %%xmm0\n\tmovups 16(%0), %%xmm1\n\tmovups 32(%0), %%xmm2\n\tmovups 48(%0), %%xmm3\n\t"
                         "movups 64(%0), %%xmm4\n\tmovups 80(%0), %%xmm5\n\tmovups 96(%0), %%xmm6\n\tmovups 112(%0), %%xmm7\n\t"
                         "movups 128(%0), %%xmm8\n\tmovups 144(%0), %%xmm9\n\tmovups 160(%0), %%xmm10\n\tmovups 176(%0), %%xmm11\n\t"
                         "movups 192(%0), %%xmm12\n\tmovups 208(%0), %%xmm13\n\tmovups 224(%0), %%xmm14\n\tmovups 240(%0), %%xmm15"
                         : : "r"(x)
                         : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
                           "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    }
    __asm__ volatile(
        "pushq %[ss]\n\t"
        "pushq %[sp]\n\t"
        "pushq %[fl]\n\t"
        "pushq %[cs]\n\t"
        "pushq %[ip]\n\t"
        "movq %c[rcx](%%rax), %%rcx\n\t"
        "movq %c[rdx](%%rax), %%rdx\n\t"
        "movq %c[rbx](%%rax), %%rbx\n\t"
        "movq %c[rbp](%%rax), %%rbp\n\t"
        "movq %c[rsi](%%rax), %%rsi\n\t"
        "movq %c[rdi](%%rax), %%rdi\n\t"
        "movq %c[r8](%%rax), %%r8\n\t"
        "movq %c[r9](%%rax), %%r9\n\t"
        "movq %c[r10](%%rax), %%r10\n\t"
        "movq %c[r11](%%rax), %%r11\n\t"
        "movq %c[r12](%%rax), %%r12\n\t"
        "movq %c[r13](%%rax), %%r13\n\t"
        "movq %c[r14](%%rax), %%r14\n\t"
        "movq %c[r15](%%rax), %%r15\n\t"
        "movq %c[rax](%%rax), %%rax\n\t"
        "iretq"
        :
        : "a"(c), [ss] "r"(ss), [sp] "r"(sp), [fl] "r"(fl), [cs] "r"(cs), [ip] "r"(ip),
          [rax] "i"(offsetof(CONTEXT, Rax)), [rcx] "i"(offsetof(CONTEXT, Rcx)), [rdx] "i"(offsetof(CONTEXT, Rdx)),
          [rbx] "i"(offsetof(CONTEXT, Rbx)), [rbp] "i"(offsetof(CONTEXT, Rbp)), [rsi] "i"(offsetof(CONTEXT, Rsi)),
          [rdi] "i"(offsetof(CONTEXT, Rdi)), [r8] "i"(offsetof(CONTEXT, R8)), [r9] "i"(offsetof(CONTEXT, R9)),
          [r10] "i"(offsetof(CONTEXT, R10)), [r11] "i"(offsetof(CONTEXT, R11)), [r12] "i"(offsetof(CONTEXT, R12)),
          [r13] "i"(offsetof(CONTEXT, R13)), [r14] "i"(offsetof(CONTEXT, R14)), [r15] "i"(offsetof(CONTEXT, R15))
        : "memory");
    __builtin_unreachable();
}

void RtlRestoreContext(PCONTEXT ContextRecord, PEXCEPTION_RECORD ExceptionRecord)
{
    (void)ExceptionRecord;
    uw_resume(ContextRecord);
}

/* ---- Unwinding to a target frame: RtlUnwindEx ------------------------------------------------------------------------------- */

#define UW_MAX_FRAMES 4096

/*
 * RtlUnwindEx walks the stack from the caller of RtlUnwindEx up to TargetFrame, calling each frame's language handler with
 * the unwind flags set so that termination handlers (__finally, destructors) run, then continues at TargetIp with
 * ReturnValue in Rax. A frame is the target when its establisher frame equals TargetFrame; its handler is called with
 * EXCEPTION_TARGET_UNWIND. With TargetFrame == NULL the unwind is an exit unwind and goes on to the end of the stack.
 * When TargetIp is NULL the context stays at the call site in the target frame (the function continues after the
 * call, as if the unwound callees had returned).
 *
 * ContextRecord is where the walk keeps its context, as on Windows. Its input matters in one case: a frame handler
 * called by RtlDispatchException passes DispatcherContext->ContextRecord, the state of the frame being dispatched, and
 * the unwind starts there (the dispatcher marks that context in P6Home, a home slot that is no machine state). For any
 * other buffer the assembly entry captures the caller's registers into it before the C code runs, because compiled
 * code has already used the callee-saved registers by then.
 */

/* P6Home of a context made by the dispatcher or by the entry stub: a value that depends on the context's address. */
#define uw_magic uw_context_mark

void RtlpUnwindEx(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue,
                  PCONTEXT ContextRecord, PVOID HistoryTable);

#if defined(__MINGW32__)
#define UW_CTXREC "0x28(%rsp)"
#else
#define UW_CTXREC "%r8"
#endif

__asm__(".text\n\t"
        UW_ASM_FUNC(RtlUnwindEx)
        "movq " UW_CTXREC ", %rax\n\t"
        "testq %rax, %rax\n\t"
        "jz 2f\n\t"
        "movq %rax, %r10\n\t"
        "movabsq $0x9E3779B97F4A7C15, %r11\n\t"
        "imulq %r11, %r10\n\t"
        "movabsq $0x4E54505F53454821, %r11\n\t"
        "xorq %r11, %r10\n\t"
        "cmpq %r10, 0x28(%rax)\n\t"                /* already the dispatcher's context: use it as it is */
        "je 2f\n\t"
        "pushq " UW_A0 "\n\t"
        "movq %rax, " UW_A0 "\n\t"
        "call RtlCaptureContext\n\t"
        "popq " UW_A0 "\n\t"
        "movq " UW_CTXREC ", %rax\n\t"
        "leaq 8(%rsp), %r10\n\t"                   /* the caller's stack pointer after the return */
        "movq %r10, 0x98(%rax)\n\t"
        "movq (%rsp), %r10\n\t"                    /* and its resume address */
        "movq %r10, 0xF8(%rax)\n\t"
        "movq %rax, %r10\n\t"
        "movabsq $0x9E3779B97F4A7C15, %r11\n\t"
        "imulq %r11, %r10\n\t"
        "movabsq $0x4E54505F53454821, %r11\n\t"
        "xorq %r11, %r10\n\t"
        "movq %r10, 0x28(%rax)\n\t"                /* P6Home: the mark */
        "movq $0, 0x20(%rax)\n\t"                  /* P5Home: no original context to start from */
        "2:\n\t"
        "jmp RtlpUnwindEx\n\t");

void RtlpUnwindEx(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue,
                  PCONTEXT ContextRecord, PVOID HistoryTable)
{
    EXCEPTION_RECORD local;
    PEXCEPTION_RECORD rec = ExceptionRecord;
    DWORD base_flags;

    if (!ContextRecord || ContextRecord->P6Home != uw_magic(ContextRecord))
        return; /* no context to start from: the entry stub fills one in whenever the caller supplies a buffer */
    /* The walk advances the caller's buffer itself: it is both the handler's ContextRecord argument and
     * DispatcherContext->ContextRecord, and shows the state of the frame being unwound. */
    CONTEXT *const uc_p = ContextRecord;
#define uc (*uc_p)
    if (uc.P5Home) {
        /* Called by a frame handler during dispatch: the walk covers every frame from the faulting instruction up to the
         * target, not only those above the handler's own frame, so it starts from the context of the exception. */
        const CONTEXT *original = (const CONTEXT *)(uintptr_t)uc.P5Home;
        const ULONG64 mark = uc.P6Home;
        uc = *original;
        uc.P5Home = 0;
        uc.P6Home = mark;
    }
    if (!rec) {
        memset(&local, 0, sizeof local);
        local.ExceptionCode = (DWORD)STATUS_UNWIND;
        local.ExceptionAddress = (PVOID)(uintptr_t)uc.Rip;
        rec = &local;
    }
    base_flags = EXCEPTION_UNWINDING | (TargetFrame ? 0 : EXCEPTION_EXIT_UNWIND);
    rec->ExceptionFlags |= base_flags;

    for (int frames = 0; frames < UW_MAX_FRAMES; ++frames) {
        ULONG64 image = 0, establisher = 0;
        PVOID hdata = NULL;
        PVOID handler = NULL;
        CONTEXT next = uc;
        IMAGE_RUNTIME_FUNCTION_ENTRY *entry = RtlLookupFunctionEntry(uc.Rip, &image, HistoryTable);
        if (entry) {
            handler = RtlVirtualUnwind(UNW_FLAG_UHANDLER, image, uc.Rip, entry, &next, &hdata, &establisher, NULL);
        } else { /* a leaf: the return address is on top of the stack */
            next.Rip = *(ULONG64 *)(uintptr_t)uc.Rsp;
            next.Rsp = uc.Rsp + 8;
            establisher = uc.Rsp;
        }
        if (TargetFrame && establisher > (ULONG64)(uintptr_t)TargetFrame) /* walked past the target: a bad stack */
            break;
        if (handler) {
            DISPATCHER_CONTEXT dc;
            EXCEPTION_DISPOSITION d;
            memset(&dc, 0, sizeof dc);
            dc.ControlPc = uc.Rip;
            dc.ImageBase = image;
            dc.FunctionEntry = entry;
            dc.EstablisherFrame = establisher;
            dc.TargetIp = (ULONG64)(uintptr_t)TargetIp;
            dc.ContextRecord = uc_p;
            dc.LanguageHandler = handler;
            dc.HandlerData = hdata;
            dc.HistoryTable = HistoryTable;
            uc.P6Home = uw_magic(uc_p);
            rec->ExceptionFlags = (rec->ExceptionFlags & ~EXCEPTION_TARGET_UNWIND) | base_flags |
                                  (TargetFrame && establisher == (ULONG64)(uintptr_t)TargetFrame ? EXCEPTION_TARGET_UNWIND : 0);
            d = ((PEXCEPTION_HANDLER)handler)(rec, (PVOID)(uintptr_t)establisher, uc_p, &dc);
            if (d == ExceptionCollidedUnwind) {
                /* a handler unwound across us; its context is where we go on from */
                if (dc.ContextRecord != uc_p)
                    uc = *dc.ContextRecord;
                continue;
            }
        }
        if (TargetFrame && establisher == (ULONG64)(uintptr_t)TargetFrame)
            break; /* uc is the state in the target frame */
        if (next.Rip == 0 || next.Rsp == 0 || next.Rsp <= uc.Rsp)
            break; /* the end of the stack (an exit unwind), or a corrupt chain */
        {
            const ULONG64 mark = uc.P6Home;
            uc = next;
            uc.P6Home = mark;
        }
    }
    rec->ExceptionFlags &= ~EXCEPTION_TARGET_UNWIND;
    if (TargetIp)
        uc.Rip = (ULONG64)(uintptr_t)TargetIp;
    uc.Rax = (ULONG64)(uintptr_t)ReturnValue;
    uw_resume(uc_p);
#undef uc
}

/* The older entry point: unwinds to TargetFrame and continues at TargetIp. */
__asm__(".text\n\t"
        UW_ASM_FUNC(RtlUnwind)
        "subq $0x508, %rsp\n\t"                   /* a scratch CONTEXT at 0x30, the arguments' space below */
        "leaq 0x30(%rsp), %rax\n\t"
#if defined(__MINGW32__)
        "movq %rax, 0x20(%rsp)\n\t"               /* the fifth argument */
        "movq $0, 0x28(%rsp)\n\t"                 /* the sixth: no history table */
#else
        "movq %rax, %r8\n\t"
        "xorl %r9d, %r9d\n\t"
#endif
        "call RtlUnwindEx\n\t"                    /* returns only if there is no frame to resume */
        "addq $0x508, %rsp\n\t"
        "ret\n\t");
