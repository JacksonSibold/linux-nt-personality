/*
 * vcr_eh4_pure.h - the compressed exception handling tables ("FH4") that newer Microsoft compilers emit on x86-64, and the
 * same decisions vcr_eh_pure.h makes from the older fixed-size ones: the state at an address, the cleanup entries to run
 * between two states, and the try and catch blocks.
 *
 * A function's descriptor is a byte stream: a header of flags saying which parts exist, then the parts. Numbers are stored
 * in 1 to 5 bytes (the count of trailing zero bits in the first byte gives the length), offsets from the image base in 4 bytes.
 * Tables of a function are reached through such offsets. Nothing here runs program code; cleanup entries go through a callback.
 * Host tests: tests/ntdll-host/test_vcr_eh4_pure.c.
 */
#ifndef VCR_EH4_PURE_H
#define VCR_EH4_PURE_H

#include <stddef.h>
#include <stdint.h>

#define VCR4_IS_CATCH 0x01u     /* the descriptor is for a catch funclet */
#define VCR4_IS_SEPARATED 0x02u /* the state map is split per function part */
#define VCR4_BBT 0x04u
#define VCR4_UNWIND_MAP 0x08u
#define VCR4_TRYBLOCK_MAP 0x10u
#define VCR4_EHS 0x20u         /* synchronous exceptions only */
#define VCR4_NO_EXCEPT 0x40u
#define VCR4_RESERVED 0x80u

#define VCR4_UNWIND_NONE 0u
#define VCR4_UNWIND_DTOR_OBJ 1u /* destroy the object at frame + offset */
#define VCR4_UNWIND_DTOR_PTR 2u /* destroy the object that the pointer at frame + offset points to */
#define VCR4_UNWIND_FRAME 3u    /* run a cleanup funclet with the frame */

#define VCR4_CATCH_FLAGS 0x01u
#define VCR4_CATCH_TYPE_INFO 0x02u
#define VCR4_CATCH_OFFSET 0x04u
#define VCR4_CATCH_SEPARATED 0x08u
#define VCR4_CATCH_RET_MASK 0x30u
#define VCR4_CATCH_RET_ONE 0x10u
#define VCR4_CATCH_RET_TWO 0x20u

static inline uint32_t vcr4_uint(const uint8_t **b)
{
    const uint8_t *p = *b;
    uint32_t r;
    if ((p[0] & 1) == 0) {
        r = (uint32_t)p[0] >> 1;
        p += 1;
    } else if ((p[0] & 3) == 1) {
        r = ((uint32_t)p[0] >> 2) + ((uint32_t)p[1] << 6);
        p += 2;
    } else if ((p[0] & 7) == 3) {
        r = ((uint32_t)p[0] >> 3) + ((uint32_t)p[1] << 5) + ((uint32_t)p[2] << 13);
        p += 3;
    } else if ((p[0] & 15) == 7) {
        r = ((uint32_t)p[0] >> 4) + ((uint32_t)p[1] << 4) + ((uint32_t)p[2] << 12) + ((uint32_t)p[3] << 20);
        p += 4;
    } else { /* 1111 in the low bits: the value follows in four bytes */
        r = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
        p += 5;
    }
    *b = p;
    return r;
}

static inline uint32_t vcr4_rva(const uint8_t **b)
{
    const uint8_t *p = *b;
    uint32_t r = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    *b += 4;
    return r;
}

typedef struct {
    uint8_t header;
    uint32_t bbt_flags;
    uint32_t unwind_count;
    const uint8_t *unwind_map; /* after the count */
    uint32_t tryblock_count;
    const uint8_t *tryblock_map;
    uint32_t ip_count;
    const uint8_t *ip_map;
    uint32_t frame; /* for a catch funclet: where its frame holds the parent function's frame pointer */
} vcr_fd4;

/* Reads the descriptor at `p`. `func_rva` is the start of the function whose code is being handled (it picks the right state
 * map of a function split into parts). Returns 0 if the descriptor is not understood. */
static inline int vcr_fd4_parse(const char *base, const uint8_t *p, uint32_t func_rva, vcr_fd4 *d)
{
    const uint8_t *q;
    uint32_t rva;
    d->header = *p++;
    d->bbt_flags = 0;
    d->unwind_count = d->tryblock_count = d->ip_count = 0;
    d->unwind_map = d->tryblock_map = d->ip_map = NULL;
    d->frame = 0;
    if (d->header & VCR4_RESERVED)
        return 0;
    if (d->header & VCR4_BBT)
        d->bbt_flags = vcr4_uint(&p);
    if (d->header & VCR4_UNWIND_MAP) {
        q = (const uint8_t *)(base + vcr4_rva(&p));
        d->unwind_count = vcr4_uint(&q);
        d->unwind_map = q;
    }
    if (d->header & VCR4_TRYBLOCK_MAP) {
        q = (const uint8_t *)(base + vcr4_rva(&p));
        d->tryblock_count = vcr4_uint(&q);
        d->tryblock_map = q;
    }
    rva = vcr4_rva(&p);
    q = (const uint8_t *)(base + rva);
    if (d->header & VCR4_IS_SEPARATED) {
        uint32_t i, num = vcr4_uint(&q);
        int found = 0;
        for (i = 0; i < num; ++i) {
            uint32_t f = vcr4_rva(&q);
            uint32_t m = vcr4_rva(&q);
            if (f == func_rva) {
                q = (const uint8_t *)(base + m);
                found = 1;
                break;
            }
        }
        if (!found)
            return 0;
    }
    d->ip_count = vcr4_uint(&q);
    d->ip_map = q;
    if (d->header & VCR4_IS_CATCH)
        d->frame = vcr4_uint(&p);
    return 1;
}

/* The state at `ip` (an offset from the image base): map entries add up their address deltas from the function start. */
static inline int32_t vcr_fd4_ip_state(const vcr_fd4 *d, uint32_t func_rva, uint32_t ip)
{
    const uint8_t *p = d->ip_map;
    uint32_t i, at = func_rva;
    int32_t state = -1;
    for (i = 0; i < d->ip_count; ++i) {
        uint32_t delta = vcr4_uint(&p);
        uint32_t s = vcr4_uint(&p);
        at += delta;
        if (ip < at)
            break;
        state = (int32_t)s - 1;
    }
    return state;
}

typedef struct {
    uint32_t type;
    const uint8_t *prev; /* the entry to continue with; before the map's start when there is none */
    uint32_t handler;
    uint32_t object;
} vcr_ui4;

static inline void vcr_ui4_read(const uint8_t **b, vcr_ui4 *ui)
{
    const uint8_t *start = *b;
    uint32_t t = vcr4_uint(b);
    ui->prev = start - (t >> 2);
    ui->type = t & 3u;
    ui->handler = 0;
    ui->object = 0;
    switch (ui->type) {
    case VCR4_UNWIND_DTOR_OBJ:
    case VCR4_UNWIND_DTOR_PTR:
        ui->handler = vcr4_rva(b);
        ui->object = vcr4_uint(b);
        break;
    case VCR4_UNWIND_FRAME:
        ui->handler = vcr4_rva(b);
        break;
    default:
        break;
    }
}

typedef void (*vcr_unwind4_fn)(void *ctx, const vcr_ui4 *ui);

/* Runs the cleanup entries from state `level` down to (not including) `last_level`. Returns 0, or -1 for a state outside the
 * map. */
static inline int vcr_fd4_local_unwind(const vcr_fd4 *d, int32_t level, int32_t last_level, vcr_unwind4_fn run, void *ctx)
{
    const uint8_t *data = d->unwind_map, *last;
    uint32_t i, guard;
    vcr_ui4 ui;
    if (level < -1 || (level >= 0 && (uint32_t)level >= d->unwind_count))
        return -1;
    if (level <= last_level)
        return 0;
    last = data - 1;
    for (i = 0; i < (uint32_t)level; ++i) {
        const uint8_t *entry = data;
        vcr_ui4_read(&data, &ui);
        if ((int32_t)i == last_level)
            last = entry;
    }
    for (guard = 0; data > last && guard <= d->unwind_count; ++guard) {
        vcr_ui4_read(&data, &ui);
        data = ui.prev;
        if (ui.handler)
            run(ctx, &ui);
    }
    return 0;
}

typedef struct {
    int32_t start_level, end_level, catch_level;
    const uint8_t *catchblocks; /* after the count */
    uint32_t catch_count;
} vcr_tb4;

static inline void vcr_tb4_read(const char *base, const uint8_t **b, vcr_tb4 *t)
{
    uint32_t rva;
    t->start_level = (int32_t)vcr4_uint(b);
    t->end_level = (int32_t)vcr4_uint(b);
    t->catch_level = (int32_t)vcr4_uint(b);
    rva = vcr4_rva(b);
    if (rva) {
        const uint8_t *q = (const uint8_t *)(base + rva);
        t->catch_count = vcr4_uint(&q);
        t->catchblocks = q;
    } else {
        t->catch_count = 0;
        t->catchblocks = NULL;
    }
}

typedef struct {
    uint8_t header;
    uint32_t flags; /* the handler adjectives: const, volatile, reference */
    uint32_t type_info;
    int32_t offset;
    uint32_t handler;
    uint32_t ret_addr[2]; /* offsets of where to continue; 0 for none */
} vcr_cb4;

/* Reads one catch block entry. Returns 0 for a header this reader does not know. */
static inline int vcr_cb4_read(const uint8_t **b, uint32_t func_rva, vcr_cb4 *c)
{
    uint8_t rt;
    c->flags = c->type_info = c->handler = c->ret_addr[0] = c->ret_addr[1] = 0;
    c->offset = 0;
    c->header = **b;
    ++*b;
    if (c->header & ~(VCR4_CATCH_FLAGS | VCR4_CATCH_TYPE_INFO | VCR4_CATCH_OFFSET | VCR4_CATCH_SEPARATED | VCR4_CATCH_RET_MASK))
        return 0;
    rt = c->header & VCR4_CATCH_RET_MASK;
    if (rt == (VCR4_CATCH_RET_ONE | VCR4_CATCH_RET_TWO))
        return 0;
    if (c->header & VCR4_CATCH_FLAGS)
        c->flags = vcr4_uint(b);
    if (c->header & VCR4_CATCH_TYPE_INFO)
        c->type_info = vcr4_rva(b);
    if (c->header & VCR4_CATCH_OFFSET)
        c->offset = (int32_t)vcr4_uint(b);
    c->handler = vcr4_rva(b);
    if (c->header & VCR4_CATCH_SEPARATED) {
        if (rt == VCR4_CATCH_RET_ONE || rt == VCR4_CATCH_RET_TWO)
            c->ret_addr[0] = vcr4_rva(b);
        if (rt == VCR4_CATCH_RET_TWO)
            c->ret_addr[1] = vcr4_rva(b);
    } else {
        if (rt == VCR4_CATCH_RET_ONE || rt == VCR4_CATCH_RET_TWO)
            c->ret_addr[0] = vcr4_uint(b) + func_rva;
        if (rt == VCR4_CATCH_RET_TWO)
            c->ret_addr[1] = vcr4_uint(b) + func_rva;
    }
    return 1;
}

#endif
