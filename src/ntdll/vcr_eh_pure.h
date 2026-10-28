/*
 * vcr_eh_pure.h - the tables of Microsoft C++ exception handling on x86-64 and the decisions made from them: which state a
 * code address belongs to, which catch block takes a thrown type, which cleanup actions an unwind runs, and how the thrown
 * object reaches the catch parameter.
 *
 * Every table is reached through 32-bit offsets from the image base. Nothing here calls program code directly: the cleanup
 * actions and the copy constructor go through callbacks, so the logic runs on the host against hand-built tables. The frame
 * handler that applies it to a real stack is in vcruntime140.c.
 * Host tests: tests/ntdll-host/test_vcr_eh_pure.c.
 */
#ifndef VCR_EH_PURE_H
#define VCR_EH_PURE_H

#include "vcr_rtti_pure.h"

#include <string.h>

#define VCR_EH_MAGIC_VC6 0x19930520u
#define VCR_EH_MAGIC_VC7 0x19930521u
#define VCR_EH_MAGIC_VC8 0x19930522u
#define VCR_EH_SYNCHRONOUS 1u /* the function was built to handle synchronous exceptions only */
#define VCR_EH_NOEXCEPT 4u

#define VCR_CXX_EXCEPTION 0xE06D7363u

typedef struct {
    uint32_t magic_bbt; /* magic in the low 29 bits */
    uint32_t unwind_count;
    uint32_t unwind_table;
    uint32_t tryblock_count;
    uint32_t tryblock;
    uint32_t ipmap_count;
    uint32_t ipmap;
    int32_t unwind_help;
    uint32_t expect_list;
    uint32_t flags; /* from VC8 on */
} vcr_funcinfo;

typedef struct {
    int32_t prev;
    uint32_t handler; /* offset of the cleanup action, or 0 */
} vcr_unwind_entry;

typedef struct {
    int32_t start_level, end_level, catch_level;
    uint32_t catchblock_count;
    uint32_t catchblock;
} vcr_tryblock;

typedef struct {
    uint32_t flags; /* VCR_HT_* */
    uint32_t type_info;
    int32_t offset; /* where in the frame the catch parameter lives, 0 for none */
    uint32_t handler;
    uint32_t frame; /* where in a catch funclet's frame the parent function's frame pointer is stored */
} vcr_catchblock;

typedef struct {
    uint32_t ip;
    int32_t state;
} vcr_ipmap;

static inline uint32_t vcr_eh_magic(const vcr_funcinfo *fi) { return fi->magic_bbt & 0x1FFFFFFFu; }

static inline const vcr_tryblock *vcr_eh_tryblock(const char *base, const vcr_funcinfo *fi, uint32_t i)
{
    return (const vcr_tryblock *)(base + fi->tryblock) + i;
}

static inline const vcr_catchblock *vcr_eh_catchblock(const char *base, const vcr_tryblock *t, uint32_t j)
{
    return (const vcr_catchblock *)(base + t->catchblock) + j;
}

/* The state in effect at a code address (as an offset from the image base): that of the last map entry at or before it. */
static inline int32_t vcr_eh_ip_state(const char *base, const vcr_funcinfo *fi, uint32_t ip_rva)
{
    const vcr_ipmap *map = (const vcr_ipmap *)(base + fi->ipmap);
    uint32_t i;
    for (i = 0; i < fi->ipmap_count; ++i)
        if (map[i].ip > ip_rva)
            break;
    return i ? map[i - 1].state : -1;
}

/* The try block whose catch region contains the state: a state past a block's try range and within its catch range means the
 * code is running in one of that block's catch blocks. Blocks are searched from the last, as nested ones come first. */
static inline const vcr_tryblock *vcr_eh_in_catch(const char *base, const vcr_funcinfo *fi, int32_t state)
{
    uint32_t i;
    for (i = fi->tryblock_count; i > 0; --i) {
        const vcr_tryblock *t = vcr_eh_tryblock(base, fi, i - 1);
        if (state > t->end_level && state <= t->catch_level)
            return t;
    }
    return NULL;
}

typedef struct {
    int found;
    int32_t frame_offset; /* where the parent frame pointer is stored in the funclet's frame */
    int32_t unwind_level; /* the state to unwind the funclet's objects down to */
} vcr_nested;

/* Code that belongs to a catch funclet (func_rva is the start of the function containing the address) runs inside the
 * function's own frame, which the funclet's frame points to. */
static inline vcr_nested vcr_eh_nested(const char *base, const vcr_funcinfo *fi, int32_t state, uint32_t func_rva)
{
    vcr_nested r;
    uint32_t i, j;
    r.found = 0;
    r.frame_offset = 0;
    r.unwind_level = -1;
    for (i = fi->tryblock_count; i > 0; --i) {
        const vcr_tryblock *t = vcr_eh_tryblock(base, fi, i - 1);
        if (state > t->end_level && state <= t->catch_level)
            for (j = 0; j < t->catchblock_count; ++j) {
                const vcr_catchblock *c = vcr_eh_catchblock(base, t, j);
                if (c->handler == func_rva) {
                    r.found = 1;
                    r.frame_offset = (int32_t)c->frame;
                    r.unwind_level = t->end_level;
                }
            }
    }
    return r;
}

typedef void (*vcr_action_fn)(void *ctx, uint32_t action_rva);

/* Runs the cleanup actions from `level` down to (not including) `last_level`, following the chain of previous states.
 * Returns the level reached, or -2 if the chain leaves the table. */
static inline int32_t vcr_eh_local_unwind(const char *base, const vcr_funcinfo *fi, int32_t level, int32_t last_level, vcr_action_fn run,
                                          void *ctx)
{
    const vcr_unwind_entry *tab = (const vcr_unwind_entry *)(base + fi->unwind_table);
    while (level > last_level) {
        if (level < 0 || (uint32_t)level >= fi->unwind_count)
            return -2;
        if (tab[level].handler)
            run(ctx, tab[level].handler);
        level = tab[level].prev;
    }
    return level;
}

typedef struct {
    const vcr_tryblock *tryblock;
    const vcr_catchblock *catchblock;
    const vcr_catchable *catchable; /* the type of the thrown object that matched; NULL for catch (...) on a non-C++ exception */
} vcr_catch_match;

/* The first catch block that takes the exception at `state`. `thrown` is NULL for an exception that is not a C++ throw: only
 * catch (...) takes it. `in_catch` is the try block whose catch the code is already in, if any: blocks it contains are the
 * only candidates. */
static inline int vcr_eh_find_catch(const char *exc_base, const char *base, const vcr_funcinfo *fi, int32_t state, const vcr_tryblock *in_catch,
                                    const vcr_throwinfo *thrown, vcr_catch_match *out)
{
    uint32_t i, j;
    for (i = 0; i < fi->tryblock_count; ++i) {
        const vcr_tryblock *t = vcr_eh_tryblock(base, fi, i);
        if (state < t->start_level || state > t->end_level)
            continue;
        if (in_catch && (t->start_level <= in_catch->end_level || t->end_level > in_catch->catch_level))
            continue;
        for (j = 0; j < t->catchblock_count; ++j) {
            const vcr_catchblock *c = vcr_eh_catchblock(base, t, j);
            const vcr_typedesc *ht = c->type_info ? (const vcr_typedesc *)(base + c->type_info) : NULL;
            const vcr_catchable *ct = NULL;
            if (thrown) {
                ct = vcr_find_catchable(exc_base, ht, c->flags, thrown);
                if (!ct)
                    continue;
            } else if (ht) {
                continue;
            }
            out->tryblock = t;
            out->catchblock = c;
            out->catchable = ct;
            return 1;
        }
    }
    return 0;
}

typedef void (*vcr_copy_ctor_fn)(void *ctx, uint32_t ctor_rva, void *dest, void *src, int has_virtual_base);

/* Puts the thrown object where the catch parameter is. A reference takes the address of the right base subobject; a simple
 * type is copied (and a pointer adjusted to the base it is caught as); a class goes through its copy constructor if it has one,
 * else a byte copy. `dest` is the catch parameter's slot. */
static inline void vcr_eh_copy_catch_object(const char *exc_base, void *object, void *dest, uint32_t catch_flags, const vcr_catchable *ct,
                                            vcr_copy_ctor_fn copy, void *ctx)
{
    if (catch_flags & VCR_HT_REFERENCE) {
        *(void **)dest = vcr_adjust_pointer(object, &ct->thisDisplacement);
    } else if (ct->properties & VCR_CT_SIMPLE) {
        memmove(dest, object, (size_t)ct->sizeOrOffset);
        if (ct->sizeOrOffset == (int32_t)sizeof(void *))
            *(void **)dest = vcr_adjust_pointer(*(void **)dest, &ct->thisDisplacement);
    } else if (ct->copyFunction) {
        copy(ctx, ct->copyFunction, dest, vcr_adjust_pointer(object, &ct->thisDisplacement), (ct->properties & VCR_CT_VIRTUAL_BASE) != 0);
    } else {
        memmove(dest, vcr_adjust_pointer(object, &ct->thisDisplacement), (size_t)ct->sizeOrOffset);
    }
    (void)exc_base;
}

#endif
