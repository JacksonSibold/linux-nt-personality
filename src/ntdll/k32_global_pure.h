/*
 * k32_global_pure.h - pure handle table behind the kernel32 Global* and Local* memory APIs.
 *
 * GlobalAlloc, GlobalLock, GlobalUnlock, GlobalFree, GlobalSize, GlobalFlags,
 * GlobalHandle, GlobalReAlloc, GlobalDiscard and the Local* family are the memory
 * service the clipboard needs: SetClipboardData takes a moveable handle, locks it,
 * and copies the bytes out. The table has no kernel behind it. A caller supplies
 * the allocator (kernel32_global.c passes the process heap; the host tests pass
 * malloc), so every rule can be checked on the host.
 *
 * Handles. GMEM_MOVEABLE and LMEM_MOVEABLE blocks get an opaque handle: an encoded
 * table index plus a generation number, so a stale handle is rejected after its
 * block is freed. GMEM_FIXED and LMEM_FIXED blocks use the data pointer as the
 * handle, as Win32 does. A live handle is never 0.
 *
 * Rules implemented here (Win32 documents them; where it is silent, the choice is
 * noted at the function):
 *  - A moveable block has a lock count, 0..255. GlobalLock on the 256th lock fails
 *    with ERROR_NOT_ENOUGH_MEMORY.
 *  - GlobalUnlock on an unlocked block fails with ERROR_NOT_LOCKED.
 *  - GlobalFree on a locked block fails and returns the handle. The Win32 page does
 *    not say what happens to a locked block, so the free is refused here instead of
 *    releasing memory a caller may still be using.
 *  - GlobalDiscard releases the storage of an unlocked discardable block. Its handle
 *    stays valid with size 0 and GMEM_DISCARDED set in GlobalFlags. GlobalLock on a
 *    discarded block fails with NULL and ERROR_NOT_ENOUGH_MEMORY.
 *  - GlobalReAlloc with GMEM_MODIFY changes only the flags. A moveable block keeps
 *    its handle across a resize. Resizing copies the common prefix. A fixed block
 *    returns a new pointer, as Win32 does.
 *  - GMEM_ZEROINIT and the GHND/GPTR/LHND/LPTR combinations zero the storage.
 */
#ifndef NTDLL_K32_GLOBAL_PURE_H
#define NTDLL_K32_GLOBAL_PURE_H

#include <stddef.h>
#include <stdint.h>

/* Global* flags (GMEM_*). LMEM_* values differ only in LMEM_DISCARDABLE. */
#define K32G_GMEM_FIXED          0x0000u
#define K32G_GMEM_MOVEABLE       0x0002u
#define K32G_GMEM_NOCOMPACT      0x0010u
#define K32G_GMEM_NODISCARD      0x0020u
#define K32G_GMEM_ZEROINIT       0x0040u
#define K32G_GMEM_MODIFY         0x0080u
#define K32G_GMEM_DISCARDABLE    0x0100u
#define K32G_GMEM_NOT_BANKED     0x1000u
#define K32G_GMEM_SHARE          0x2000u
#define K32G_GMEM_VALID_FLAGS    0x7F72u
#define K32G_GMEM_INVALID_HANDLE 0x8000u
#define K32G_GMEM_DISCARDED      0x4000u
#define K32G_GMEM_LOCKCOUNT      0x00FFu
#define K32G_GHND                (K32G_GMEM_MOVEABLE | K32G_GMEM_ZEROINIT)
#define K32G_GPTR                (K32G_GMEM_FIXED | K32G_GMEM_ZEROINIT)

/* Local* flags. LMEM_DISCARDABLE is 0x0F00 (all four bits), not 0x0100. */
#define K32G_LMEM_FIXED          0x0000u
#define K32G_LMEM_MOVEABLE       0x0002u
#define K32G_LMEM_NOCOMPACT      0x0010u
#define K32G_LMEM_NODISCARD      0x0020u
#define K32G_LMEM_ZEROINIT       0x0040u
#define K32G_LMEM_MODIFY         0x0080u
#define K32G_LMEM_DISCARDABLE    0x0F00u
#define K32G_LMEM_VALID_FLAGS    0x0F72u
#define K32G_LMEM_INVALID_HANDLE 0x8000u
#define K32G_LMEM_DISCARDED      0x4000u
#define K32G_LMEM_LOCKCOUNT      0x00FFu
#define K32G_LHND                (K32G_LMEM_MOVEABLE | K32G_LMEM_ZEROINIT)
#define K32G_LPTR                (K32G_LMEM_FIXED | K32G_LMEM_ZEROINIT)

/* Win32 error codes used here (winerror.h values). */
#define K32G_ERROR_SUCCESS            0u
#define K32G_ERROR_INVALID_HANDLE     6u
#define K32G_ERROR_NOT_ENOUGH_MEMORY  8u
#define K32G_ERROR_INVALID_PARAMETER  87u
#define K32G_ERROR_NOT_LOCKED         158u
#define K32G_ERROR_LOCK_FAILED        167u

/* Largest block the table accepts (the Win32 limit is larger; this is a sanity cap). */
#define K32G_MAX_BYTES   ((size_t)0x40000000u)
/* Table capacity. A fixed table keeps the service free of allocation of its own. */
#define K32G_MAX_BLOCKS 512u
#define K32G_LOCK_MAX   255u
/* Moveable handles: index in bits 4..12 (+1 so 0 is never a handle), generation in
 * bits 16..31. Bits 0..3 are zero, as on an aligned pointer. */
#define K32G_HANDLE_INDEX_SHIFT 4u
#define K32G_HANDLE_GEN_SHIFT   16u

typedef struct k32g_ops {
    void *(*alloc)(void *ctx, size_t bytes);      /* NULL on failure */
    void (*release)(void *ctx, void *p);
    void *ctx;
} k32g_ops;

typedef struct k32g_block {
    uint8_t used;
    uint8_t fixed;
    uint8_t discardable;
    uint8_t discarded;
    uint16_t gen;
    uint32_t flags;     /* GMEM_* flags kept from allocation (DISCARDABLE may change) */
    uint32_t locks;
    void *ptr;
    size_t size;
} k32g_block;

typedef struct k32g_table {
    k32g_block blk[K32G_MAX_BLOCKS];
    k32g_ops ops;
    uint32_t last_error;
    uint32_t live;
} k32g_table;

/* Maps LMEM_* flags to the GMEM_* values the table works with. */
static inline uint32_t k32g_local_to_global_flags(uint32_t lflags)
{
    uint32_t g = lflags & (K32G_GMEM_MOVEABLE | K32G_GMEM_NOCOMPACT | K32G_GMEM_NODISCARD |
                           K32G_GMEM_ZEROINIT | K32G_GMEM_MODIFY);
    if (lflags & K32G_LMEM_DISCARDABLE)
        g |= K32G_GMEM_DISCARDABLE;
    return g;
}

static inline void k32g_set_error(k32g_table *t, uint32_t err)
{
    t->last_error = err;
}

static inline void k32g_init(k32g_table *t, k32g_ops ops)
{
    for (uint32_t i = 0; i < K32G_MAX_BLOCKS; ++i) {
        t->blk[i].used = 0;
        t->blk[i].gen = 0;
        t->blk[i].ptr = NULL;
        t->blk[i].size = 0;
        t->blk[i].locks = 0;
        t->blk[i].flags = 0;
        t->blk[i].fixed = 0;
        t->blk[i].discardable = 0;
        t->blk[i].discarded = 0;
    }
    t->ops = ops;
    t->last_error = K32G_ERROR_SUCCESS;
    t->live = 0;
}

/* Drops every block (test teardown). Storage goes back through the release hook. */
static inline void k32g_reset(k32g_table *t)
{
    for (uint32_t i = 0; i < K32G_MAX_BLOCKS; ++i) {
        k32g_block *b = &t->blk[i];
        if (b->used && b->ptr && !b->discarded && t->ops.release)
            t->ops.release(t->ops.ctx, b->ptr);
        b->used = 0;
        b->ptr = NULL;
    }
    t->live = 0;
    t->last_error = K32G_ERROR_SUCCESS;
}

static inline uint32_t k32g_get_last_error(const k32g_table *t)
{
    return t->last_error;
}

static inline size_t k32g_live_count(const k32g_table *t)
{
    return t->live;
}

static inline uintptr_t k32g_make_handle(uint32_t index, uint16_t gen)
{
    return ((uintptr_t)gen << K32G_HANDLE_GEN_SHIFT) |
           ((uintptr_t)(index + 1u) << K32G_HANDLE_INDEX_SHIFT);
}

/* Index of a live moveable block for handle h, or -1. Checks the generation. */
static inline int k32g_index_of(const k32g_table *t, uintptr_t h)
{
    uintptr_t low = h & ((1u << K32G_HANDLE_GEN_SHIFT) - 1u);
    uint32_t idx;
    if (low == 0 || (low & 0xFu) != 0)
        return -1;
    idx = (uint32_t)(low >> K32G_HANDLE_INDEX_SHIFT);
    if (idx == 0 || idx > K32G_MAX_BLOCKS)
        return -1;
    idx -= 1u;
    if (!t->blk[idx].used || t->blk[idx].fixed)
        return -1;
    if (t->blk[idx].gen != (uint16_t)((h >> K32G_HANDLE_GEN_SHIFT) & 0xFFFFu))
        return -1;
    return (int)idx;
}

/* Index of a live fixed block whose pointer is h, or -1. */
static inline int k32g_fixed_index(const k32g_table *t, uintptr_t h)
{
    for (uint32_t i = 0; i < K32G_MAX_BLOCKS; ++i) {
        if (t->blk[i].used && t->blk[i].fixed && (uintptr_t)t->blk[i].ptr == h)
            return (int)i;
    }
    return -1;
}

/* Resolves any handle (moveable or fixed) to its block index, or -1. */
static inline int k32g_resolve(const k32g_table *t, uintptr_t h)
{
    int i;
    if (!h)
        return -1;
    i = k32g_index_of(t, h);
    if (i >= 0)
        return i;
    return k32g_fixed_index(t, h);
}

static inline int k32g_handle_valid(const k32g_table *t, uintptr_t h)
{
    return k32g_resolve(t, h) >= 0;
}

static inline int k32g_free_slot(const k32g_table *t)
{
    for (uint32_t i = 0; i < K32G_MAX_BLOCKS; ++i) {
        if (!t->blk[i].used)
            return (int)i;
    }
    return -1;
}

/* Allocates storage for n bytes; zeroes it when zero is set. */
static inline void *k32g_storage(k32g_table *t, size_t n, int zero)
{
    void *p;
    if (n > K32G_MAX_BYTES)
        return NULL;
    p = t->ops.alloc ? t->ops.alloc(t->ops.ctx, n ? n : 1u) : NULL;
    if (p && zero) {
        unsigned char *c = (unsigned char *)p;
        for (size_t i = 0; i < (n ? n : 1u); ++i)
            c[i] = 0;
    }
    return p;
}

static inline void k32g_copy_bytes(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; ++i)
        d[i] = s[i];
}

/* GlobalAlloc / LocalAlloc. Returns the handle, or 0 with last_error set. Flags are
 * GMEM_* values. Zero-size requests succeed with size 0 (Win32 behaviour). */
static inline uintptr_t k32g_alloc(k32g_table *t, uint32_t flags, size_t bytes)
{
    int slot;
    void *p;
    int fixed = (flags & K32G_GMEM_MOVEABLE) == 0;
    int zero = (flags & K32G_GMEM_ZEROINIT) != 0;
    if ((flags & ~K32G_GMEM_VALID_FLAGS) != 0 || bytes > K32G_MAX_BYTES) {
        k32g_set_error(t, bytes > K32G_MAX_BYTES ? K32G_ERROR_NOT_ENOUGH_MEMORY : K32G_ERROR_INVALID_PARAMETER);
        return 0;
    }
    slot = k32g_free_slot(t);
    if (slot < 0) {
        k32g_set_error(t, K32G_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    p = k32g_storage(t, bytes, zero);
    if (!p) {
        k32g_set_error(t, K32G_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    {
        k32g_block *b = &t->blk[slot];
        b->used = 1;
        b->fixed = (uint8_t)fixed;
        b->discardable = (flags & K32G_GMEM_DISCARDABLE) != 0;
        b->discarded = 0;
        b->flags = flags & ~K32G_GMEM_MODIFY;
        b->locks = 0;
        b->ptr = p;
        b->size = bytes;
        t->live++;
        k32g_set_error(t, K32G_ERROR_SUCCESS);
        if (fixed)
            return (uintptr_t)p;
        b->gen = (uint16_t)(b->gen + 1u); /* a new generation for every allocation */
        return k32g_make_handle((uint32_t)slot, b->gen);
    }
}

static inline void k32g_release_block(k32g_table *t, k32g_block *b)
{
    if (!b->discarded && b->ptr && t->ops.release)
        t->ops.release(t->ops.ctx, b->ptr);
    b->ptr = NULL;
    b->size = 0;
    b->discarded = 0;
    b->used = 0;
    t->live--;
}

/* GlobalFree / LocalFree. 0 on success. On failure returns h and sets last_error. */
static inline uintptr_t k32g_free(k32g_table *t, uintptr_t h)
{
    int i = k32g_resolve(t, h);
    k32g_block *b;
    if (i < 0) {
        k32g_set_error(t, K32G_ERROR_INVALID_HANDLE);
        return h;
    }
    b = &t->blk[i];
    if (b->locks != 0) {
        k32g_set_error(t, K32G_ERROR_LOCK_FAILED);
        return h;
    }
    k32g_release_block(t, b);
    k32g_set_error(t, K32G_ERROR_SUCCESS);
    return 0;
}

/* GlobalLock / LocalLock. Fixed blocks return their pointer and take no lock count. */
static inline void *k32g_lock(k32g_table *t, uintptr_t h)
{
    int i = k32g_resolve(t, h);
    k32g_block *b;
    if (i < 0) {
        k32g_set_error(t, K32G_ERROR_INVALID_HANDLE);
        return NULL;
    }
    b = &t->blk[i];
    if (b->fixed) {
        k32g_set_error(t, K32G_ERROR_SUCCESS);
        return b->ptr;
    }
    if (b->discarded) {
        k32g_set_error(t, K32G_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (b->locks >= K32G_LOCK_MAX) {
        k32g_set_error(t, K32G_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    b->locks++;
    k32g_set_error(t, K32G_ERROR_SUCCESS);
    return b->ptr;
}

/* GlobalUnlock / LocalUnlock. Nonzero while the count stays above zero. At zero, or on
 * an unlocked block, returns 0 and sets ERROR_NOT_LOCKED (a count that drops to zero
 * clears the error, as the Win32 page describes). */
static inline int k32g_unlock(k32g_table *t, uintptr_t h)
{
    int i = k32g_resolve(t, h);
    k32g_block *b;
    if (i < 0) {
        k32g_set_error(t, K32G_ERROR_INVALID_HANDLE);
        return 0;
    }
    b = &t->blk[i];
    if (b->fixed) {
        k32g_set_error(t, K32G_ERROR_NOT_LOCKED);
        return 0;
    }
    if (b->locks == 0) {
        k32g_set_error(t, K32G_ERROR_NOT_LOCKED);
        return 0;
    }
    b->locks--;
    if (b->locks == 0) {
        k32g_set_error(t, K32G_ERROR_SUCCESS);
        return 0;
    }
    k32g_set_error(t, K32G_ERROR_SUCCESS);
    return 1;
}

/* GlobalSize / LocalSize. */
static inline size_t k32g_size(const k32g_table *t, uintptr_t h)
{
    int i = k32g_resolve(t, h);
    if (i < 0)
        return 0;
    return t->blk[i].discarded ? 0 : t->blk[i].size;
}

/* GlobalFlags / LocalFlags. */
static inline uint32_t k32g_flags(const k32g_table *t, uintptr_t h)
{
    int i = k32g_resolve(t, h);
    uint32_t f;
    if (i < 0)
        return K32G_GMEM_INVALID_HANDLE;
    f = t->blk[i].locks & K32G_GMEM_LOCKCOUNT;
    if (t->blk[i].discarded)
        f |= K32G_GMEM_DISCARDED;
    return f;
}

/* GlobalHandle / LocalHandle: the handle for a data pointer a caller holds. */
static inline uintptr_t k32g_handle_of_ptr(const k32g_table *t, const void *p)
{
    for (uint32_t i = 0; i < K32G_MAX_BLOCKS; ++i) {
        const k32g_block *b = &t->blk[i];
        if (!b->used || b->discarded)
            continue;
        if (b->ptr == p) {
            if (b->fixed)
                return (uintptr_t)b->ptr;
            return k32g_make_handle(i, b->gen);
        }
    }
    return 0;
}

/* GlobalReAlloc / LocalReAlloc. MODIFY-only calls change flags and keep the handle.
 * A moveable block keeps its handle across a resize. A fixed block returns a new
 * pointer, and the old one is freed. */
static inline uintptr_t k32g_realloc(k32g_table *t, uintptr_t h, size_t bytes, uint32_t flags)
{
    int i = k32g_resolve(t, h);
    k32g_block *b;
    void *np;
    size_t keep;
    if (i < 0) {
        k32g_set_error(t, K32G_ERROR_INVALID_HANDLE);
        return 0;
    }
    b = &t->blk[i];
    if (flags & K32G_GMEM_MODIFY) {
        if (flags & K32G_GMEM_DISCARDABLE)
            b->discardable = 1;
        else
            b->discardable = 0;
        if (flags & K32G_GMEM_MOVEABLE) {
            /* Fixed to moveable is not a MODIFY change in Win32; refused. */
            if (b->fixed) {
                k32g_set_error(t, K32G_ERROR_INVALID_PARAMETER);
                return 0;
            }
        }
        k32g_set_error(t, K32G_ERROR_SUCCESS);
        return h;
    }
    if (bytes > K32G_MAX_BYTES || (flags & ~K32G_GMEM_VALID_FLAGS) != 0) {
        k32g_set_error(t, K32G_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!b->fixed && b->locks != 0) {
        /* A locked moveable block cannot move. Win32 allows a resize only when
         * GMEM_MOVEABLE is not given; that is also refused here. */
        if (!(flags & K32G_GMEM_MOVEABLE)) {
            k32g_set_error(t, K32G_ERROR_LOCK_FAILED);
            return 0;
        }
    }
    if (b->discarded) {
        /* A discarded block is reallocated from nothing; contents are gone. */
        np = k32g_storage(t, bytes, (flags & K32G_GMEM_ZEROINIT) != 0);
        if (!np) {
            k32g_set_error(t, K32G_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        b->ptr = np;
        b->size = bytes;
        b->discarded = 0;
        k32g_set_error(t, K32G_ERROR_SUCCESS);
        return b->fixed ? (uintptr_t)np : h;
    }
    np = k32g_storage(t, bytes, 0);
    if (!np) {
        k32g_set_error(t, K32G_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    keep = b->size < bytes ? b->size : bytes;
    k32g_copy_bytes(np, b->ptr, keep);
    if ((flags & K32G_GMEM_ZEROINIT) && bytes > keep) {
        unsigned char *c = (unsigned char *)np;
        for (size_t k = keep; k < bytes; ++k)
            c[k] = 0;
    }
    if (t->ops.release)
        t->ops.release(t->ops.ctx, b->ptr);
    b->ptr = np;
    b->size = bytes;
    if (b->fixed) {
        k32g_set_error(t, K32G_ERROR_SUCCESS);
        return (uintptr_t)np;
    }
    k32g_set_error(t, K32G_ERROR_SUCCESS);
    return h;
}

/* GlobalDiscard / LocalDiscard. Releases an unlocked discardable block's storage. */
static inline size_t k32g_discard(k32g_table *t, uintptr_t h)
{
    int i = k32g_resolve(t, h);
    k32g_block *b;
    size_t old;
    if (i < 0) {
        k32g_set_error(t, K32G_ERROR_INVALID_HANDLE);
        return 0;
    }
    b = &t->blk[i];
    if (b->fixed || !b->discardable || b->locks != 0) {
        k32g_set_error(t, K32G_ERROR_NOT_LOCKED);
        return 0;
    }
    old = b->size;
    if (!b->discarded && t->ops.release)
        t->ops.release(t->ops.ctx, b->ptr);
    b->ptr = NULL;
    b->size = 0;
    b->discarded = 1;
    k32g_set_error(t, K32G_ERROR_SUCCESS);
    return old;
}

#endif /* NTDLL_K32_GLOBAL_PURE_H */
