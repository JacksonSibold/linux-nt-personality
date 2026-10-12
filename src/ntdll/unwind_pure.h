/*
 * unwind_pure.h - pure x64 unwind-code arithmetic used by unwind.c.
 *
 * Everything here is a function of its arguments only: no module registry,
 * no context record, no memory reads through pointers it is not given. The
 * host tests in tests/ntdll-host include this header directly.
 *
 * Operand layout follows the Microsoft x64 exception-handling spec:
 *  - UNWIND_CODE slots are 16 bit. Codes are stored in descending CodeOffset
 *    order, so the last prolog operation is first in the array.
 *  - UWOP_ALLOC_LARGE OpInfo 0 is followed by one slot holding size / 8.
 *    OpInfo 1 is followed by two slots: unscaled 32-bit size, low word first.
 *  - UWOP_SAVE_NONVOL offsets are scaled by 8, SAVE_XMM128 offsets by 16.
 *    The _FAR forms are unscaled 32-bit values in two slots, low word first.
 *  - The chained-info RUNTIME_FUNCTION follows the code array, after padding
 *    to an even slot count.
 */

#ifndef UNWIND_PURE_H
#define UNWIND_PURE_H

#include "unwind.h"

#include <stddef.h>
#include <stdint.h>

/* Number of 16-bit slots one unwind code occupies, operand slots included.
 * The prolog-start search and the machine-frame scan step with this. A
 * reserved ALLOC_LARGE OpInfo (2 or more) counts as 3, as before. */
static inline unsigned uwp_op_slots(unsigned op, unsigned opinfo)
{
    switch (op) {
    case UWOP_ALLOC_LARGE:
        return opinfo == 0 ? 2u : 3u;
    case UWOP_SAVE_NONVOL:
    case UWOP_SAVE_XMM128:
        return 2u;
    case UWOP_SAVE_NONVOL_FAR:
    case UWOP_SAVE_XMM128_FAR:
        return 3u;
    default:
        return 1u;
    }
}

/* Size in bytes of UWOP_ALLOC_SMALL: OpInfo + 1 slots of 8 bytes. */
static inline ULONG64 uwp_alloc_small_size(unsigned opinfo)
{
    return (ULONG64)opinfo * 8 + 8;
}

/* Two-slot unscaled 32-bit operand: low word in the first slot. */
static inline ULONG64 uwp_far_operand(unsigned lo, unsigned hi)
{
    return (ULONG64)(ULONG)((ULONG)lo | ((ULONG)hi << 16));
}

/* Operand for UWOP_ALLOC_LARGE OpInfo 0 (one slot, size / 8) and the byte
 * size it stands for. */
static inline ULONG64 uwp_alloc_large_size8(unsigned slot)
{
    return (ULONG64)slot * 8;
}

/* Scaled offset of UWOP_SAVE_NONVOL (8) and UWOP_SAVE_XMM128 (16). */
static inline ULONG64 uwp_save_nonvol_offset(unsigned slot)
{
    return (ULONG64)slot * 8;
}

static inline ULONG64 uwp_save_xmm_offset(unsigned slot)
{
    return (ULONG64)slot * 16;
}

/* Slots of the code array with the chained RUNTIME_FUNCTION appended after
 * it: the count rounded up to an even number. */
static inline unsigned uwp_chain_slots(unsigned countOfCodes)
{
    return (countOfCodes + 1) & ~1u;
}

/* Frame base for SAVE_* operands and UWOP_SET_FPREG. When the function has a
 * frame register, the base is FrameReg - FrameOffset * 16. Otherwise the
 * caller passes the current RSP as the base. */
static inline ULONG64 uwp_fp_base(ULONG64 frameRegValue, unsigned frameOffsetScaled)
{
    return frameRegValue - (ULONG64)frameOffsetScaled * 16;
}

/* True when ControlPc is inside the prolog of the entry that starts at
 * funcBegin: at or after the start, and before the prolog's end. */
static inline int uwp_in_prolog(ULONG64 controlPc, ULONG64 funcBegin, unsigned sizeOfProlog)
{
    if (controlPc < funcBegin)
        return 0;
    return (controlPc - funcBegin) < sizeOfProlog;
}

/* Index of the first code whose CodeOffset is at or before ripOffset, or -1
 * when no prolog operation has completed yet. Because the array is sorted in
 * descending order, this index is the first operation that has run, and
 * every code from there on has run too. */
static inline int uwp_prolog_start(const UNWIND_CODE *codes, unsigned countOfCodes, ULONG64 ripOffset)
{
    for (unsigned i = 0; i < countOfCodes;) {
        unsigned op = codes[i].DUMMYSTRUCTNAME.UnwindOp;
        if (codes[i].DUMMYSTRUCTNAME.CodeOffset <= ripOffset)
            return (int)i;
        i += uwp_op_slots(op, codes[i].DUMMYSTRUCTNAME.OpInfo);
    }
    return -1;
}

/* Whether an UNWIND_CODE array holds UWOP_PUSH_MACHFRAME anywhere. The scan
 * steps by slot counts so operand slots are never read as codes. */
static inline int uwp_has_machframe(const UNWIND_CODE *codes, unsigned countOfCodes)
{
    for (unsigned k = 0; k < countOfCodes;) {
        unsigned op = codes[k].DUMMYSTRUCTNAME.UnwindOp;
        if (op == UWOP_PUSH_MACHFRAME)
            return 1;
        k += uwp_op_slots(op, codes[k].DUMMYSTRUCTNAME.OpInfo);
    }
    return 0;
}

/* Layout of the hardware trap frame pushed by UWOP_PUSH_MACHFRAME.
 * Without an error code: [RIP][CS][EFLAGS][RSP][SS], 40 bytes.
 * With one:              [error][RIP][CS][EFLAGS][RSP][SS], 48 bytes. */
static inline unsigned uwp_machframe_size(int hasError)
{
    return hasError ? 48u : 40u;
}

static inline unsigned uwp_machframe_rip_offset(int hasError)
{
    return hasError ? 8u : 0u;
}

static inline unsigned uwp_machframe_rsp_offset(int hasError)
{
    return hasError ? 32u : 24u;
}

/* Module-relative range check used by RUNTIME_FUNCTION lookup. sizeOfImage 0
 * means the size is unknown, so only the 4 GiB RVA limit applies. On success
 * *rva is set to the RVA of pc. */
static inline int uwp_module_rva(ULONG64 imageBase, ULONG sizeOfImage, ULONG64 pc, ULONG *rva)
{
    if (pc < imageBase)
        return 0;
    if (pc - imageBase > 0xFFFFFFFFULL)
        return 0;
    if (sizeOfImage != 0 && pc - imageBase >= sizeOfImage)
        return 0;
    *rva = (ULONG)(pc - imageBase);
    return 1;
}

/* Binary search of a table sorted by BeginAddress for the entry with
 * Begin <= rva < End. Returns its index, or -1. x64 EndAddress has no flag
 * bits, so it is compared as-is (UNW-4). */
static inline long uwp_find_function(const IMAGE_RUNTIME_FUNCTION_ENTRY *table, ULONG funcCount, ULONG rva)
{
    ULONG lo = 0, hi = funcCount;
    while (lo < hi) {
        ULONG mid = lo + (hi - lo) / 2;
        ULONG begin = table[mid].BeginAddress;
        ULONG end = table[mid].EndAddress;
        if (rva < begin)
            hi = mid;
        else if (rva >= end)
            lo = mid + 1;
        else
            return (long)mid;
    }
    return -1;
}

/* ---- epilog recognition ----------------------------------------------
 *
 * An epilog is the only code allowed to run an unwind through a function
 * that is returning. Its legal forms (x64 "prolog and epilog" rules):
 *   1. add rsp,imm8|imm32, or lea rsp,[FP+disp8|disp32] where FP is the
 *      function's frame register (lea without a frame register is illegal);
 *   2. zero or more 8-byte pops, never pop rsp;
 *   3. ret (C3 or C2 iw), or jmp FF /4 with ModRM mod 00.
 * No other instruction may appear. Only the bytes passed in are read. */

#define UWP_EPI_MAX_POPS 16

enum {
    UWP_EPI_NONE = 0,
    UWP_EPI_RET  = 1,
    UWP_EPI_JMP  = 2
};

typedef struct {
    unsigned bytes;                    /* length of the epilog, terminator included */
    unsigned pops;                     /* number of pops in popReg */
    unsigned popReg[UWP_EPI_MAX_POPS]; /* register numbers, in pop order */
    int      useFrame;                 /* 1: lea from the frame register; 0: add from RSP */
    int64_t  disp;                     /* the stack release constant */
    unsigned term;                     /* UWP_EPI_RET or UWP_EPI_JMP */
    unsigned retImm;                   /* ret imm16, when present */
} uwp_epilog_t;

static inline uint32_t uwp_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Match an epilog at p, reading at most n bytes. frameReg is the function's
 * frame register (0 when it has none). Returns 1 and fills *e when the bytes
 * form a legal epilog, 0 otherwise. */
static inline int uwp_decode_epilog(const uint8_t *p, size_t n, unsigned frameReg, uwp_epilog_t *e)
{
    size_t i = 0;

    e->bytes = 0;
    e->pops = 0;
    e->useFrame = 0;
    e->disp = 0;
    e->term = UWP_EPI_NONE;
    e->retImm = 0;

    /* Stack release. */
    if (n >= 4 && p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xC4) {
        e->disp = (int8_t)p[3];                       /* add rsp,imm8 */
        i = 4;
    } else if (n >= 7 && p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xC4) {
        e->disp = (int32_t)uwp_le32(p + 3);           /* add rsp,imm32 */
        i = 7;
    } else if (n >= 4 && (p[0] & 0xFE) == 0x48 && (p[0] & 0x06) == 0 &&
               p[1] == 0x8D && frameReg != 0) {       /* lea rsp,[FP+disp] */
        unsigned m = p[2];
        unsigned mod = m >> 6;
        unsigned base = (m & 7u) | ((p[0] & 1u) << 3);
        size_t dlen = mod == 1 ? 1 : 4;
        if (((m >> 3) & 7u) != 4 || (mod != 1 && mod != 2) || (m & 7u) == 4)
            return 0;
        if (base != frameReg || n < 3 + dlen)
            return 0;
        e->disp = dlen == 1 ? (int64_t)(int8_t)p[3] : (int64_t)(int32_t)uwp_le32(p + 3);
        e->useFrame = 1;
        i = 3 + dlen;
    } else {
        return 0;
    }

    /* Zero or more pops. */
    for (;;) {
        size_t j = i;
        unsigned rexB = 0;
        if (j < n && p[j] == 0x41) {
            rexB = 1;
            j++;
        }
        if (j < n && p[j] >= 0x58 && p[j] <= 0x5F) {
            unsigned reg = (p[j] & 7u) | (rexB << 3);
            if (reg == 4 || e->pops == UWP_EPI_MAX_POPS)
                return 0;                             /* pop rsp is not allowed */
            e->popReg[e->pops++] = reg;
            i = j + 1;
            continue;
        }
        break;
    }

    /* Terminator: ret, or jmp with a mod 00 memory operand. */
    if (i < n && p[i] == 0xC3) {
        e->term = UWP_EPI_RET;
        i += 1;
    } else if (i + 3 <= n && p[i] == 0xC2) {
        e->term = UWP_EPI_RET;
        e->retImm = (unsigned)p[i + 1] | ((unsigned)p[i + 2] << 8);
        i += 3;
    } else {
        size_t j = i;
        size_t len = 2;                               /* FF + ModRM */
        if (j < n && (p[j] & 0xF0) == 0x40)
            j++;                                      /* REX */
        if (!(j + 1 < n && p[j] == 0xFF))
            return 0;
        unsigned m = p[j + 1];
        if ((m >> 6) != 0 || ((m >> 3) & 7u) != 4)
            return 0;
        if ((m & 7u) == 4) {                          /* SIB follows */
            if (j + 2 >= n)
                return 0;
            len++;
            if ((p[j + 2] & 7u) == 5)                 /* base 101 with mod 00: disp32 */
                len += 4;
        } else if ((m & 7u) == 5) {                   /* RIP-relative disp32 */
            len += 4;
        }
        if (j + len > n)
            return 0;
        e->term = UWP_EPI_JMP;
        i = j + len;
    }

    e->bytes = (unsigned)i;
    return 1;
}

/* RSP after the stack release of a decoded epilog. The add form adds disp to
 * RSP; the lea form sets RSP to the frame register value plus disp. */
static inline ULONG64 uwp_epilog_release(const uwp_epilog_t *e, ULONG64 rsp, ULONG64 frameValue)
{
    ULONG64 base = e->useFrame ? frameValue : rsp;
    return base + (ULONG64)e->disp;
}

#endif /* UNWIND_PURE_H */
