/*
 * rtl_heap_pure.h - the heap manager behind RtlAllocateHeap and kernel32 HeapAlloc.
 *
 * A boundary-tag allocator with segregated free lists. It never touches the operating system
 * itself: segments come from, and go back to, the callbacks of a struct rh_os, so the same code
 * runs in ntdll.dll on NtAllocateVirtualMemory and in the host tests on malloc.
 *
 * Layout. A heap is a list of segments. A segment is one block of address space from the OS:
 *
 *     [rh_segment][chunk][chunk]...[chunk][sentinel]
 *
 * The first segment of a heap also holds the rh_heap structure; its address is the heap handle.
 * Every chunk starts with a 16-byte header (size and previous size in 16-byte units, flags and a
 * check pattern, and the slack at the end of the block), so a user pointer is 16-byte aligned and
 * the size a caller asked for can be returned by HeapSize. A free chunk keeps its list links in
 * the first 16 bytes of the user area, so the smallest chunk is 32 bytes.
 *
 * Free chunks are coalesced with free neighbours. Requests of at least RH_LARGE bytes get a
 * segment of their own, which goes back to the OS when the block is freed. Ordinary segments stay
 * with the heap until it is destroyed.
 *
 * Locking. Unless the heap was created with RH_NO_SERIALIZE, every call takes the heap lock
 * through os->lock and os->unlock (a spin lock word that the OS layer yields on).
 *
 * Errors are reported by return value: NULL or 0. A free of something that is not an allocated
 * chunk of the heap, a double free, and a header that fails its check return 0 and change nothing.
 */
#ifndef NTDLL_RTL_HEAP_PURE_H
#define NTDLL_RTL_HEAP_PURE_H

#include <stddef.h>
#include <stdint.h>

#define RH_NO_SERIALIZE 0x00000001u
#define RH_GROWABLE 0x00000002u
#define RH_GENERATE_EXCEPTIONS 0x00000004u
#define RH_ZERO_MEMORY 0x00000008u
#define RH_REALLOC_IN_PLACE_ONLY 0x00000010u

#define RH_ALIGN 16u
#define RH_HEADER 16u
#define RH_MIN_CHUNK 32u
#define RH_LARGE (512u * 1024u)       /* requests at least this size get their own segment */
#define RH_GROW (1024u * 1024u)       /* the usual segment size */
#define RH_MAX_REQUEST ((size_t)1 << 36) /* chunk sizes are stored in 16-byte units in 32 bits */

#define RH_SMALL_BINS 64u              /* exact bins for chunk sizes 16 * i, i < 64 */
#define RH_LARGE_BINS 128u             /* four bins per power of two from 1 KB */
#define RH_BINS (RH_SMALL_BINS + RH_LARGE_BINS)

/* What the heap needs from its environment. */
typedef struct rh_os {
    /* A block of at least size bytes, page aligned, or NULL. The block need not be zeroed. */
    void *(*seg_alloc)(size_t size);
    /* Returns a block obtained from seg_alloc, with the same size. */
    void (*seg_free)(void *base, size_t size);
    void (*lock)(volatile long *word);
    void (*unlock)(volatile long *word);
} rh_os;

typedef struct rh_chunk {
    uint32_t size16;  /* chunk size in 16-byte units, header included */
    uint32_t prev16;  /* size of the chunk before, in 16-byte units; 0 for the first chunk of a segment */
    uint32_t flags;   /* RH_F_* in the low byte, RH_MAGIC above */
    uint32_t slack;   /* in-use chunk: bytes between the requested size and the end of the chunk */
} rh_chunk;

typedef struct rh_free_links {
    struct rh_chunk *next, *prev;
} rh_free_links;

typedef struct rh_segment {
    struct rh_segment *next;
    size_t size;      /* the bytes obtained from the OS */
    uint32_t flags;   /* RH_SEG_* */
    uint32_t pad;
} rh_segment;

typedef struct rh_heap {
    uint32_t magic;
    uint32_t options;
    const rh_os *os;
    volatile long lock;
    size_t max_size;            /* 0: growable without limit */
    size_t committed;           /* bytes held from the OS in all segments */
    size_t in_use;              /* bytes in allocated chunks, headers included */
    uint32_t alloc_count;
    uint32_t seg_count;
    rh_segment *segments;
    uint64_t bin_bits[(RH_BINS + 63) / 64];
    rh_chunk *bins[RH_BINS];
} rh_heap;

#define RH_HEAP_MAGIC 0x50414548u /* "HEAP" */
#define RH_MAGIC 0xA5C0DE00u
#define RH_MAGIC_MASK 0xFFFFFF00u
#define RH_F_INUSE 0x01u
#define RH_F_SENTINEL 0x02u
#define RH_SEG_FIRST 0x01u     /* holds the rh_heap */
#define RH_SEG_DEDICATED 0x02u /* one large block */

static inline size_t rh_round_up(size_t n, size_t a)
{
    return (n + a - 1) & ~(a - 1);
}

static inline size_t rh_chunk_size(const rh_chunk *c)
{
    return (size_t)c->size16 * RH_ALIGN;
}

static inline rh_chunk *rh_next_chunk(const rh_chunk *c)
{
    return (rh_chunk *)((uint8_t *)c + rh_chunk_size(c));
}

static inline rh_chunk *rh_prev_chunk(const rh_chunk *c)
{
    return c->prev16 ? (rh_chunk *)((uint8_t *)c - (size_t)c->prev16 * RH_ALIGN) : NULL;
}

static inline int rh_chunk_inuse(const rh_chunk *c)
{
    return (c->flags & RH_F_INUSE) != 0;
}

static inline rh_free_links *rh_links(rh_chunk *c)
{
    return (rh_free_links *)((uint8_t *)c + RH_HEADER);
}

static inline void *rh_user(rh_chunk *c)
{
    return (uint8_t *)c + RH_HEADER;
}

static inline rh_chunk *rh_chunk_of(void *user)
{
    return (rh_chunk *)((uint8_t *)user - RH_HEADER);
}

static inline void rh_set_flags(rh_chunk *c, uint32_t f)
{
    c->flags = RH_MAGIC | f;
}

static inline void rh_zero(void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--)
        *b++ = 0;
}

static inline void rh_copy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
}

/* ---- Bins -------------------------------------------------------------------------- */

static inline unsigned rh_log2(size_t v)
{
    unsigned r = 0;
    while (v >>= 1)
        ++r;
    return r;
}

/* The bin a free chunk of the given size belongs in. */
static inline unsigned rh_bin_of(size_t size)
{
    unsigned lg, idx;
    if (size < RH_SMALL_BINS * RH_ALIGN)
        return (unsigned)(size / RH_ALIGN);
    lg = rh_log2(size);                       /* at least 10 */
    idx = RH_SMALL_BINS + (lg - 10) * 4 + (unsigned)((size >> (lg - 2)) & 3);
    return idx < RH_BINS ? idx : RH_BINS - 1;
}

static inline void rh_bin_insert(rh_heap *h, rh_chunk *c)
{
    unsigned b = rh_bin_of(rh_chunk_size(c));
    rh_free_links *l = rh_links(c);
    l->prev = NULL;
    l->next = h->bins[b];
    if (l->next)
        rh_links(l->next)->prev = c;
    h->bins[b] = c;
    h->bin_bits[b / 64] |= (uint64_t)1 << (b % 64);
}

static inline void rh_bin_remove(rh_heap *h, rh_chunk *c)
{
    unsigned b = rh_bin_of(rh_chunk_size(c));
    rh_free_links *l = rh_links(c);
    if (l->prev)
        rh_links(l->prev)->next = l->next;
    else
        h->bins[b] = l->next;
    if (l->next)
        rh_links(l->next)->prev = l->prev;
    if (!h->bins[b])
        h->bin_bits[b / 64] &= ~((uint64_t)1 << (b % 64));
}

/* The next non-empty bin at or after b, or RH_BINS. */
static inline unsigned rh_next_bin(const rh_heap *h, unsigned b)
{
    for (; b < RH_BINS; ++b) {
        uint64_t word = h->bin_bits[b / 64] >> (b % 64);
        if (word) {
            while (!(word & 1)) {
                word >>= 1;
                ++b;
            }
            return b;
        }
        b = (b | 63u); /* the rest of this word is empty; the loop increment moves to the next */
    }
    return RH_BINS;
}

/* A free chunk of at least `need` bytes, taken out of its bin; NULL when there is none. */
static inline rh_chunk *rh_take(rh_heap *h, size_t need)
{
    unsigned b = rh_bin_of(need);
    rh_chunk *c, *best = NULL;
    if (b >= RH_SMALL_BINS) { /* chunks within one large bin differ in size: take the smallest that fits */
        for (c = h->bins[b]; c; c = rh_links(c)->next) {
            if (rh_chunk_size(c) >= need && (!best || rh_chunk_size(c) < rh_chunk_size(best)))
                best = c;
        }
        if (best) {
            rh_bin_remove(h, best);
            return best;
        }
        ++b;
    }
    b = rh_next_bin(h, b);
    if (b >= RH_BINS)
        return NULL;
    c = h->bins[b]; /* every chunk in a higher bin is large enough */
    rh_bin_remove(h, c);
    return c;
}

/* ---- Segments ------------------------------------------------------------------------- */

/* Writes the chunk and sentinel headers of a new segment; returns its one free chunk. The
 * rh_heap, when this is the first segment, sits between the segment header and the chunk. */
static inline rh_chunk *rh_segment_setup(rh_segment *s, size_t size, size_t first_off)
{
    rh_chunk *c = (rh_chunk *)((uint8_t *)s + first_off);
    rh_chunk *sentinel;
    size_t chunk_bytes = (size - first_off - RH_HEADER) & ~(size_t)(RH_ALIGN - 1);
    c->size16 = (uint32_t)(chunk_bytes / RH_ALIGN);
    c->prev16 = 0;
    c->slack = 0;
    rh_set_flags(c, 0);
    sentinel = rh_next_chunk(c);
    sentinel->size16 = 1;
    sentinel->prev16 = c->size16;
    sentinel->slack = 0;
    rh_set_flags(sentinel, RH_F_INUSE | RH_F_SENTINEL);
    return c;
}

#define RH_SEG_HEADER_SIZE rh_round_up(sizeof(rh_segment), RH_ALIGN)

/* Creates a heap. size is the first segment's size (at least a page); max_size 0 lets the heap
 * grow, otherwise it never holds more than max_size bytes from the OS. Returns NULL on failure. */
static inline rh_heap *rh_create(const rh_os *os, uint32_t options, size_t size, size_t max_size)
{
    size_t first_off = RH_SEG_HEADER_SIZE + rh_round_up(sizeof(rh_heap), RH_ALIGN);
    rh_segment *s;
    rh_heap *h;
    rh_chunk *c;
    if (size < RH_GROW)
        size = RH_GROW;
    if (max_size && size > max_size)
        size = max_size;
    if (size < first_off + RH_MIN_CHUNK + RH_HEADER)
        return NULL;
    s = (rh_segment *)os->seg_alloc(size);
    if (!s)
        return NULL;
    h = (rh_heap *)((uint8_t *)s + RH_SEG_HEADER_SIZE);
    rh_zero(h, sizeof(*h));
    h->magic = RH_HEAP_MAGIC;
    h->options = options;
    h->os = os;
    h->max_size = max_size;
    h->committed = size;
    h->seg_count = 1;
    s->next = NULL;
    s->size = size;
    s->flags = RH_SEG_FIRST;
    h->segments = s;
    c = rh_segment_setup(s, size, first_off);
    rh_bin_insert(h, c);
    return h;
}

static inline void rh_lock(rh_heap *h)
{
    if (!(h->options & RH_NO_SERIALIZE))
        h->os->lock(&h->lock);
}

static inline void rh_unlock(rh_heap *h)
{
    if (!(h->options & RH_NO_SERIALIZE))
        h->os->unlock(&h->lock);
}

/* The segment a chunk belongs to. Walks the list; heaps have few segments. */
static inline rh_segment *rh_segment_of(const rh_heap *h, const rh_chunk *c)
{
    for (rh_segment *s = h->segments; s; s = s->next) {
        if ((const uint8_t *)c > (const uint8_t *)s && (const uint8_t *)c < (const uint8_t *)s + s->size)
            return s;
    }
    return NULL;
}

/* Adds a segment able to hold a chunk of `need` bytes. Returns the new free chunk, already in a bin
 * unless the segment is dedicated (then the caller takes it). NULL when the OS or the limit refuses. */
static inline rh_chunk *rh_grow(rh_heap *h, size_t need, int dedicated)
{
    size_t want = RH_SEG_HEADER_SIZE + need + RH_HEADER;
    size_t size = dedicated ? rh_round_up(want, 4096) : rh_round_up(want > RH_GROW ? want : RH_GROW, 65536);
    rh_segment *s;
    rh_chunk *c;
    if (h->max_size && h->committed + size > h->max_size) {
        /* A fixed-size heap may still add exactly what is needed, up to its limit. */
        size = rh_round_up(want, 4096);
        if (h->committed + size > h->max_size)
            return NULL;
    }
    s = (rh_segment *)h->os->seg_alloc(size);
    if (!s)
        return NULL;
    s->next = h->segments;
    s->size = size;
    s->flags = dedicated ? RH_SEG_DEDICATED : 0;
    h->segments = s;
    h->committed += size;
    h->seg_count++;
    c = rh_segment_setup(s, size, RH_SEG_HEADER_SIZE);
    if (!dedicated)
        rh_bin_insert(h, c);
    return c;
}

/* ---- Allocation --------------------------------------------------------------------------- */

/* The chunk size that holds a request of `size` bytes, or 0 when it is too big. */
static inline size_t rh_chunk_for(size_t size)
{
    size_t n;
    if (size > RH_MAX_REQUEST)
        return 0;
    n = rh_round_up(size + RH_HEADER, RH_ALIGN);
    return n < RH_MIN_CHUNK ? RH_MIN_CHUNK : n;
}

/* Marks a free chunk (already out of its bin) as in use for `size` bytes, giving back the tail
 * as a new free chunk when it is big enough to stand alone. */
static inline void rh_commit(rh_heap *h, rh_chunk *c, size_t need, size_t size)
{
    size_t have = rh_chunk_size(c);
    rh_chunk *next;
    if (have - need >= RH_MIN_CHUNK) {
        rh_chunk *rest = (rh_chunk *)((uint8_t *)c + need);
        c->size16 = (uint32_t)(need / RH_ALIGN);
        rest->size16 = (uint32_t)((have - need) / RH_ALIGN);
        rest->prev16 = c->size16;
        rest->slack = 0;
        rh_set_flags(rest, 0);
        next = rh_next_chunk(rest);
        next->prev16 = rest->size16;
        rh_bin_insert(h, rest);
        have = need;
    }
    c->slack = (uint32_t)(have - RH_HEADER - size);
    rh_set_flags(c, RH_F_INUSE);
    h->in_use += have;
    h->alloc_count++;
}

static inline void *rh_alloc_locked(rh_heap *h, size_t size, uint32_t flags)
{
    size_t need = rh_chunk_for(size);
    rh_chunk *c;
    if (!need)
        return NULL;
    if (need >= RH_LARGE) {
        c = rh_grow(h, need, 1);
        if (!c)
            return NULL;
    } else {
        c = rh_take(h, need);
        if (!c) {
            if (!rh_grow(h, need, 0))
                return NULL;
            c = rh_take(h, need);
            if (!c)
                return NULL;
        }
    }
    rh_commit(h, c, need, size);
    if (flags & RH_ZERO_MEMORY)
        rh_zero(rh_user(c), size);
    return rh_user(c);
}

/* Validates that p is an allocated chunk of this heap: a header with the right check pattern,
 * in use, and inside a segment. */
static inline rh_chunk *rh_check_inuse(const rh_heap *h, void *p)
{
    rh_chunk *c;
    rh_segment *s;
    if (!p || ((uintptr_t)p & (RH_ALIGN - 1)))
        return NULL;
    c = rh_chunk_of(p);
    s = rh_segment_of(h, c);
    if (!s)
        return NULL;
    if ((c->flags & RH_MAGIC_MASK) != RH_MAGIC || !(c->flags & RH_F_INUSE) || (c->flags & RH_F_SENTINEL))
        return NULL;
    if (c->size16 < RH_MIN_CHUNK / RH_ALIGN || (uint8_t *)c + rh_chunk_size(c) > (uint8_t *)s + s->size)
        return NULL;
    return c;
}

/* Frees chunk c (in use, validated) and coalesces it. */
static inline void rh_free_chunk(rh_heap *h, rh_chunk *c)
{
    size_t size = rh_chunk_size(c);
    rh_chunk *next = rh_next_chunk(c);
    rh_chunk *prev;
    rh_segment *s = rh_segment_of(h, c);
    h->in_use -= size;
    h->alloc_count--;
    c->slack = 0;
    rh_set_flags(c, 0);
    if (!(next->flags & RH_F_INUSE)) { /* the sentinel is marked in use, so this never runs off the end */
        rh_bin_remove(h, next);
        c->size16 += next->size16;
        next = rh_next_chunk(c);
    }
    prev = rh_prev_chunk(c);
    if (prev && !(prev->flags & RH_F_INUSE)) {
        rh_bin_remove(h, prev);
        prev->size16 += c->size16;
        c = prev;
        next = rh_next_chunk(c);
    }
    next->prev16 = c->size16;
    /* A dedicated segment, which held one large block, goes back to the OS. Ordinary segments are kept:
     * their free chunks serve later requests, and handing them back would make a heap that sits at a
     * segment boundary allocate and release a megabyte over and over. */
    if (s && (s->flags & RH_SEG_DEDICATED) && !c->prev16 && (next->flags & RH_F_SENTINEL)) {
        rh_segment **link = &h->segments;
        while (*link && *link != s)
            link = &(*link)->next;
        if (*link) {
            *link = s->next;
            h->committed -= s->size;
            h->seg_count--;
            h->os->seg_free(s, s->size);
            return;
        }
    }
    rh_bin_insert(h, c);
}

static inline void *rh_alloc(rh_heap *h, size_t size, uint32_t flags)
{
    void *p;
    rh_lock(h);
    p = rh_alloc_locked(h, size, flags);
    rh_unlock(h);
    return p;
}

static inline int rh_free(rh_heap *h, void *p)
{
    rh_chunk *c;
    rh_lock(h);
    c = rh_check_inuse(h, p);
    if (c)
        rh_free_chunk(h, c);
    rh_unlock(h);
    return c != NULL;
}

/* HeapSize: the size the block was allocated with, or (size_t)-1 for a pointer that is not an
 * allocated block of this heap. */
static inline size_t rh_size(rh_heap *h, void *p)
{
    rh_chunk *c;
    size_t r = (size_t)-1;
    rh_lock(h);
    c = rh_check_inuse(h, p);
    if (c)
        r = rh_chunk_size(c) - RH_HEADER - c->slack;
    rh_unlock(h);
    return r;
}

/* HeapReAlloc. Grows or shrinks in place when the neighbouring chunk allows it; otherwise moves the
 * block, unless RH_REALLOC_IN_PLACE_ONLY is set. The old block is left alone on failure. */
static inline void *rh_realloc(rh_heap *h, void *p, size_t size, uint32_t flags)
{
    rh_chunk *c, *next;
    size_t need, have, old_size;
    void *np = NULL;
    rh_lock(h);
    c = rh_check_inuse(h, p);
    if (!c) {
        rh_unlock(h);
        return NULL;
    }
    need = rh_chunk_for(size);
    if (!need) {
        rh_unlock(h);
        return NULL;
    }
    have = rh_chunk_size(c);
    old_size = have - RH_HEADER - c->slack;
    if (need <= have) { /* shrink, or the same chunk: give the tail back when it can stand alone */
        if (have - need >= RH_MIN_CHUNK) {
            rh_chunk *rest = (rh_chunk *)((uint8_t *)c + need);
            rh_chunk *after = rh_next_chunk(c);
            c->size16 = (uint32_t)(need / RH_ALIGN);
            rest->size16 = (uint32_t)((have - need) / RH_ALIGN);
            rest->prev16 = c->size16;
            rest->slack = 0;
            rh_set_flags(rest, RH_F_INUSE); /* marked in use so the free below coalesces it properly */
            after->prev16 = rest->size16;
            h->alloc_count++; /* rest is freed as if it were a block: rh_free_chunk takes it off the counts */
            rh_free_chunk(h, rest);
            have = need;
        }
        c->slack = (uint32_t)(have - RH_HEADER - size);
        if ((flags & RH_ZERO_MEMORY) && size > old_size)
            rh_zero((uint8_t *)rh_user(c) + old_size, size - old_size);
        rh_unlock(h);
        return rh_user(c);
    }
    /* Grow in place into a free neighbour. */
    next = rh_next_chunk(c);
    if (!(next->flags & RH_F_INUSE) && have + rh_chunk_size(next) >= need) {
        size_t total = have + rh_chunk_size(next);
        rh_chunk *after;
        rh_bin_remove(h, next);
        if (total - need >= RH_MIN_CHUNK) {
            rh_chunk *rest = (rh_chunk *)((uint8_t *)c + need);
            c->size16 = (uint32_t)(need / RH_ALIGN);
            rest->size16 = (uint32_t)((total - need) / RH_ALIGN);
            rest->prev16 = c->size16;
            rest->slack = 0;
            rh_set_flags(rest, 0);
            after = rh_next_chunk(rest);
            after->prev16 = rest->size16;
            rh_bin_insert(h, rest);
            h->in_use += need - have;
            have = need;
        } else {
            c->size16 = (uint32_t)(total / RH_ALIGN);
            after = rh_next_chunk(c);
            after->prev16 = c->size16;
            h->in_use += total - have;
            have = total;
        }
        c->slack = (uint32_t)(have - RH_HEADER - size);
        if (flags & RH_ZERO_MEMORY)
            rh_zero((uint8_t *)rh_user(c) + old_size, size - old_size);
        rh_unlock(h);
        return rh_user(c);
    }
    if (flags & RH_REALLOC_IN_PLACE_ONLY) {
        rh_unlock(h);
        return NULL;
    }
    np = rh_alloc_locked(h, size, 0);
    if (np) {
        rh_copy(np, p, old_size < size ? old_size : size);
        if ((flags & RH_ZERO_MEMORY) && size > old_size)
            rh_zero((uint8_t *)np + old_size, size - old_size);
        rh_free_chunk(h, c);
    }
    rh_unlock(h);
    return np;
}

/* HeapDestroy: returns every segment to the OS, the first last because it holds the heap. */
static inline void rh_destroy(rh_heap *h)
{
    rh_segment *s = h->segments, *first = NULL;
    const rh_os *os = h->os;
    h->magic = 0;
    while (s) {
        rh_segment *next = s->next;
        if (s->flags & RH_SEG_FIRST)
            first = s;
        else
            os->seg_free(s, s->size);
        s = next;
    }
    if (first)
        os->seg_free(first, first->size);
}

/* HeapValidate for the whole heap (p == NULL) or one block. Walks every chunk checking the headers,
 * the previous-size links, that every free chunk is in its bin, and the byte counts. */
static inline int rh_validate(rh_heap *h, void *p)
{
    int ok = 1;
    rh_lock(h);
    if (h->magic != RH_HEAP_MAGIC) {
        rh_unlock(h);
        return 0;
    }
    if (p) {
        ok = rh_check_inuse(h, p) != NULL;
        rh_unlock(h);
        return ok;
    }
    {
        size_t in_use = 0, committed = 0;
        uint32_t count = 0, segs = 0;
        for (rh_segment *s = h->segments; s && ok; s = s->next) {
            size_t first_off = (s->flags & RH_SEG_FIRST) ? RH_SEG_HEADER_SIZE + rh_round_up(sizeof(rh_heap), RH_ALIGN)
                                                         : RH_SEG_HEADER_SIZE;
            rh_chunk *c = (rh_chunk *)((uint8_t *)s + first_off);
            uint32_t prev16 = 0;
            ++segs;
            committed += s->size;
            for (;;) {
                if ((uint8_t *)c < (uint8_t *)s || (uint8_t *)c + RH_HEADER > (uint8_t *)s + s->size ||
                    (c->flags & RH_MAGIC_MASK) != RH_MAGIC || c->prev16 != prev16) {
                    ok = 0;
                    break;
                }
                if (c->flags & RH_F_SENTINEL)
                    break;
                if (c->size16 < RH_MIN_CHUNK / RH_ALIGN || (uint8_t *)c + rh_chunk_size(c) > (uint8_t *)s + s->size) {
                    ok = 0;
                    break;
                }
                if (c->flags & RH_F_INUSE) {
                    in_use += rh_chunk_size(c);
                    ++count;
                } else {
                    rh_chunk *nx = rh_next_chunk(c);
                    unsigned b = rh_bin_of(rh_chunk_size(c));
                    rh_chunk *f;
                    int found = 0;
                    if (!(nx->flags & RH_F_INUSE)) { /* two free neighbours were not coalesced */
                        ok = 0;
                        break;
                    }
                    for (f = h->bins[b]; f; f = rh_links(f)->next) {
                        if (f == c) {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        ok = 0;
                        break;
                    }
                }
                prev16 = c->size16;
                c = rh_next_chunk(c);
            }
        }
        if (ok && (in_use != h->in_use || count != h->alloc_count || committed != h->committed || segs != h->seg_count))
            ok = 0;
    }
    rh_unlock(h);
    return ok;
}

/* ---- Walking ---------------------------------------------------------------------------------- */

typedef struct rh_walk {
    void *data;         /* the block's user area */
    size_t size;        /* the usable size: the requested size for a busy block */
    unsigned overhead;  /* bytes of header (and slack for a busy block) */
    int busy;
    /* iteration state: start with rh_walk_init */
    rh_segment *seg;
    rh_chunk *chunk;
} rh_walk;

static inline void rh_walk_init(rh_walk *w)
{
    w->seg = NULL;
    w->chunk = NULL;
    w->data = NULL;
}

/* The next block in the heap (busy or free), 0 at the end. The caller holds the heap lock with
 * rh_lock/rh_unlock if it wants a consistent walk. */
static inline int rh_walk_next(rh_heap *h, rh_walk *w)
{
    rh_chunk *c;
    if (!w->seg) {
        w->seg = h->segments;
        if (!w->seg)
            return 0;
        c = NULL;
    } else {
        c = w->chunk;
    }
    for (;;) {
        if (!c) {
            size_t first_off = (w->seg->flags & RH_SEG_FIRST)
                                   ? RH_SEG_HEADER_SIZE + rh_round_up(sizeof(rh_heap), RH_ALIGN)
                                   : RH_SEG_HEADER_SIZE;
            c = (rh_chunk *)((uint8_t *)w->seg + first_off);
        } else {
            c = rh_next_chunk(c);
        }
        if (c->flags & RH_F_SENTINEL) {
            w->seg = w->seg->next;
            if (!w->seg)
                return 0;
            c = NULL;
            continue;
        }
        break;
    }
    w->chunk = c;
    w->data = rh_user(c);
    w->busy = rh_chunk_inuse(c);
    w->overhead = RH_HEADER + (w->busy ? c->slack : 0);
    w->size = rh_chunk_size(c) - RH_HEADER - (w->busy ? c->slack : 0);
    return 1;
}

#endif /* NTDLL_RTL_HEAP_PURE_H */
