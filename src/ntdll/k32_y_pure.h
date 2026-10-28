/*
 * k32_y_pure.h - rules behind kernel32_y.c that need no system calls: pointer encoding, the condition masks of
 * VerifyVersionInfo, and the SYSTEM_LOGICAL_PROCESSOR_INFORMATION records built from a processor topology.
 * Host tests: tests/ntdll-host/test_k32_y_pure.c.
 */
#ifndef K32_Y_PURE_H
#define K32_Y_PURE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- EncodePointer ------------------------------------------------------------------------------------------------------ */

/* A pointer is XORed with the process cookie and rotated right by the cookie's low six bits, so an encoded value is
 * useless without the cookie and decoding reverses it exactly. */
static inline uint64_t k32y_encode_pointer(uint64_t p, uint64_t cookie)
{
    unsigned r = (unsigned)(cookie & 63u);
    uint64_t x = p ^ cookie;
    return r ? (x >> r) | (x << (64u - r)) : x;
}

static inline uint64_t k32y_decode_pointer(uint64_t e, uint64_t cookie)
{
    unsigned r = (unsigned)(cookie & 63u);
    uint64_t x = r ? (e << r) | (e >> (64u - r)) : e;
    return x ^ cookie;
}

/* ---- Version conditions ------------------------------------------------------------------------------------------------- */

#define K32Y_VER_MINORVERSION 0x01u
#define K32Y_VER_MAJORVERSION 0x02u
#define K32Y_VER_BUILDNUMBER 0x04u
#define K32Y_VER_PLATFORMID 0x08u
#define K32Y_VER_SERVICEPACKMINOR 0x10u
#define K32Y_VER_SERVICEPACKMAJOR 0x20u
#define K32Y_VER_SUITENAME 0x40u
#define K32Y_VER_PRODUCT_TYPE 0x80u

#define K32Y_VER_EQUAL 1u
#define K32Y_VER_GREATER 2u
#define K32Y_VER_GREATER_EQUAL 3u
#define K32Y_VER_LESS 4u
#define K32Y_VER_LESS_EQUAL 5u
#define K32Y_VER_AND 6u
#define K32Y_VER_OR 7u

/* The mask holds three bits per version field, field n (bit n of the type mask) at bits 3n. */
static inline uint64_t k32y_set_condition(uint64_t mask, uint32_t type_mask, uint8_t condition)
{
    if (!condition)
        return mask;
    for (unsigned bit = 0; bit < 8; ++bit)
        if (type_mask & (1u << bit)) {
            mask &= ~((uint64_t)7 << (3 * bit));
            mask |= (uint64_t)(condition & 7u) << (3 * bit);
        }
    return mask;
}

static inline unsigned k32y_condition_of(uint64_t mask, unsigned bit)
{
    return (unsigned)((mask >> (3 * bit)) & 7u);
}

typedef struct {
    uint32_t major, minor, build, platform;
    uint16_t sp_major, sp_minor, suite;
    uint8_t product_type;
} k32y_version;

/* Compares one field under its condition: returns 1 satisfied, 0 not satisfied. *decided is set to 1 when the result
 * settles the whole hierarchical comparison (a strict inequality in the wanted direction). */
static inline int k32y_compare(uint32_t actual, uint32_t wanted, unsigned cond, int *decided)
{
    *decided = 0;
    switch (cond) {
    case K32Y_VER_EQUAL: return actual == wanted;
    case K32Y_VER_GREATER: *decided = actual > wanted; return actual > wanted;
    case K32Y_VER_GREATER_EQUAL: *decided = actual > wanted; return actual >= wanted;
    case K32Y_VER_LESS: *decided = actual < wanted; return actual < wanted;
    case K32Y_VER_LESS_EQUAL: *decided = actual < wanted; return actual <= wanted;
    default: return 0;
    }
}


/* Build number, platform, suite and product type, each on its own. */
static inline int k32y_verify_rest(const k32y_version *actual, const k32y_version *wanted, uint32_t type_mask, uint64_t mask)
{
    int decided;
    if (type_mask & K32Y_VER_BUILDNUMBER) {
        unsigned c = k32y_condition_of(mask, 2);
        if (c < K32Y_VER_EQUAL || c > K32Y_VER_LESS_EQUAL)
            return -1;
        if (!k32y_compare(actual->build, wanted->build, c, &decided))
            return 0;
    }
    if (type_mask & K32Y_VER_PLATFORMID) {
        unsigned c = k32y_condition_of(mask, 3);
        if (c < K32Y_VER_EQUAL || c > K32Y_VER_LESS_EQUAL)
            return -1;
        if (!k32y_compare(actual->platform, wanted->platform, c, &decided))
            return 0;
    }
    if (type_mask & K32Y_VER_PRODUCT_TYPE) {
        unsigned c = k32y_condition_of(mask, 7);
        if (c < K32Y_VER_EQUAL || c > K32Y_VER_LESS_EQUAL)
            return -1;
        if (!k32y_compare(actual->product_type, wanted->product_type, c, &decided))
            return 0;
    }
    if (type_mask & K32Y_VER_SUITENAME) {
        unsigned c = k32y_condition_of(mask, 6);
        if (c == K32Y_VER_AND) {
            if ((actual->suite & wanted->suite) != wanted->suite)
                return 0;
        } else if (c == K32Y_VER_OR) {
            if (!(actual->suite & wanted->suite))
                return 0;
        } else {
            return -1;
        }
    }
    return 1;
}

/* VerifyVersionInfo: does `actual` satisfy `wanted` under the conditions in `mask` for the fields in `type_mask`?
 * Major, minor and service pack fields are examined from the most significant down, and a strict inequality in the wanted
 * direction settles the answer; otherwise the next field must hold as well. Build, platform, suite and product type are
 * checked independently. Returns 1 when the condition holds, 0 when not, -1 for a condition that is invalid for the field. */
static inline int k32y_verify(const k32y_version *actual, const k32y_version *wanted, uint32_t type_mask, uint64_t mask)
{
    static const struct {
        unsigned bit;
        int which;
    } order[] = {{1, 0}, {0, 1}, {5, 2}, {4, 3}};
    for (size_t i = 0; i < sizeof order / sizeof order[0]; ++i) {
        unsigned bit = order[i].bit, cond;
        uint32_t a, w;
        int decided;
        if (!(type_mask & (1u << bit)))
            continue;
        cond = k32y_condition_of(mask, bit);
        if (cond < K32Y_VER_EQUAL || cond > K32Y_VER_LESS_EQUAL)
            return -1;
        switch (order[i].which) {
        case 0: a = actual->major; w = wanted->major; break;
        case 1: a = actual->minor; w = wanted->minor; break;
        case 2: a = actual->sp_major; w = wanted->sp_major; break;
        default: a = actual->sp_minor; w = wanted->sp_minor; break;
        }
        if (!k32y_compare(a, w, cond, &decided))
            return 0;
        if (decided)
            return (type_mask & (K32Y_VER_BUILDNUMBER | K32Y_VER_PLATFORMID | K32Y_VER_SUITENAME | K32Y_VER_PRODUCT_TYPE))
                       ? k32y_verify_rest(actual, wanted, type_mask, mask)
                       : 1;
    }
    return k32y_verify_rest(actual, wanted, type_mask, mask);
}

/* ---- Logical processor information ---------------------------------------------------------------------------------------- */

#define K32Y_RELATION_PROCESSOR_CORE 0u
#define K32Y_RELATION_NUMA_NODE 1u
#define K32Y_RELATION_CACHE 2u
#define K32Y_RELATION_PROCESSOR_PACKAGE 3u
#define K32Y_RELATION_GROUP 4u
#define K32Y_RELATION_ALL 0xFFFFu

#define K32Y_CACHE_UNIFIED 0u
#define K32Y_CACHE_INSTRUCTION 1u
#define K32Y_CACHE_DATA 2u

typedef struct {
    uint8_t level, associativity;
    uint16_t line_size;
    uint32_t size, type;
    unsigned shared_logical; /* how many logical processors share one instance */
} k32y_cache;

typedef struct {
    unsigned logical;  /* logical processors, at most 64 (one group) */
    unsigned smt;      /* threads per core, 1 or 2 */
    const k32y_cache *caches;
    unsigned ncaches;
} k32y_topology;

/* SYSTEM_LOGICAL_PROCESSOR_INFORMATION on x64: 32 bytes. */
typedef struct {
    uint64_t mask;
    uint32_t relationship;
    uint32_t pad_;
    union {
        struct { uint8_t flags; } core;
        struct { uint32_t node; } numa;
        struct { uint8_t level, associativity; uint16_t line_size; uint32_t size, type; } cache;
        uint64_t reserved[2];
    } u;
} k32y_slpi;

static inline uint64_t k32y_mask_of(unsigned first, unsigned count)
{
    uint64_t m = 0;
    for (unsigned i = 0; i < count && first + i < 64; ++i)
        m |= (uint64_t)1 << (first + i);
    return m;
}

static inline unsigned k32y_smt(const k32y_topology *t) { return t->smt >= 2 && t->logical >= 2 ? 2u : 1u; }

/* The legacy records for the topology, written to out while there is room (out may be NULL); returns how many there are.
 * Per core: the core, then the caches private to it; then each shared cache, the NUMA node and the package. */
static inline size_t k32y_build_legacy(const k32y_topology *t, k32y_slpi *out, size_t cap)
{
    size_t n = 0;
    unsigned smt = k32y_smt(t), cores = (t->logical + smt - 1) / smt;
#define K32Y_PUT(rec)                                                                                                   \
    do {                                                                                                                \
        if (out && n < cap)                                                                                             \
            out[n] = (rec);                                                                                             \
        ++n;                                                                                                            \
    } while (0)
    for (unsigned c = 0; c < cores; ++c) {
        k32y_slpi r;
        memset(&r, 0, sizeof r);
        r.mask = k32y_mask_of(c * smt, smt);
        r.relationship = K32Y_RELATION_PROCESSOR_CORE;
        r.u.core.flags = smt > 1 ? 1 : 0; /* LTP_PC_SMT */
        K32Y_PUT(r);
        for (unsigned i = 0; i < t->ncaches; ++i) {
            const k32y_cache *k = &t->caches[i];
            if (k->shared_logical > smt)
                continue;
            memset(&r, 0, sizeof r);
            r.mask = k32y_mask_of(c * smt, smt);
            r.relationship = K32Y_RELATION_CACHE;
            r.u.cache.level = k->level;
            r.u.cache.associativity = k->associativity;
            r.u.cache.line_size = k->line_size;
            r.u.cache.size = k->size;
            r.u.cache.type = k->type;
            K32Y_PUT(r);
        }
    }
    for (unsigned i = 0; i < t->ncaches; ++i) {
        const k32y_cache *k = &t->caches[i];
        unsigned share = k->shared_logical > smt ? k->shared_logical : 0;
        if (!share)
            continue;
        for (unsigned first = 0; first < t->logical; first += share) {
            k32y_slpi r;
            memset(&r, 0, sizeof r);
            r.mask = k32y_mask_of(first, share < t->logical - first ? share : t->logical - first);
            r.relationship = K32Y_RELATION_CACHE;
            r.u.cache.level = k->level;
            r.u.cache.associativity = k->associativity;
            r.u.cache.line_size = k->line_size;
            r.u.cache.size = k->size;
            r.u.cache.type = k->type;
            K32Y_PUT(r);
        }
    }
    {
        k32y_slpi r;
        memset(&r, 0, sizeof r);
        r.mask = k32y_mask_of(0, t->logical);
        r.relationship = K32Y_RELATION_NUMA_NODE;
        r.u.numa.node = 0;
        K32Y_PUT(r);
        memset(&r, 0, sizeof r);
        r.mask = k32y_mask_of(0, t->logical);
        r.relationship = K32Y_RELATION_PROCESSOR_PACKAGE;
        K32Y_PUT(r);
    }
#undef K32Y_PUT
    return n;
}

/* GROUP_AFFINITY: mask, group, three reserved words. */
typedef struct {
    uint64_t mask;
    uint16_t group, reserved[3];
} k32y_group_affinity;

/* Byte sizes of the SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX records on x64 (header of 8 included). */
#define K32Y_EX_PROCESSOR_SIZE 48u
#define K32Y_EX_NUMA_SIZE 48u
#define K32Y_EX_CACHE_SIZE 56u
#define K32Y_EX_PACKAGE_SIZE 48u
#define K32Y_EX_GROUP_SIZE 80u

static inline void k32y_put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void k32y_put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void k32y_put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* The _EX records for `relationship` (or all of them). Returns the bytes needed; writes them when cap allows. The count of
 * records goes to *records. */
static inline size_t k32y_build_ex(const k32y_topology *t, uint32_t relationship, uint8_t *out, size_t cap, unsigned *records)
{
    size_t off = 0;
    unsigned smt = k32y_smt(t), cores = (t->logical + smt - 1) / smt, nrec = 0;
    uint64_t all = k32y_mask_of(0, t->logical);
#define K32Y_BEGIN(size_, rel_)                                                                                        \
    uint8_t *p_ = (out && off + (size_) <= cap) ? out + off : NULL;                                                     \
    if (p_) {                                                                                                           \
        memset(p_, 0, (size_));                                                                                         \
        k32y_put32(p_, (rel_));                                                                                         \
        k32y_put32(p_ + 4, (uint32_t)(size_));                                                                          \
    }                                                                                                                   \
    off += (size_);                                                                                                     \
    ++nrec;
    if (relationship == K32Y_RELATION_PROCESSOR_CORE || relationship == K32Y_RELATION_ALL) {
        for (unsigned c = 0; c < cores; ++c) {
            K32Y_BEGIN(K32Y_EX_PROCESSOR_SIZE, K32Y_RELATION_PROCESSOR_CORE)
            if (p_) {
                p_[8] = smt > 1 ? 1 : 0;           /* Flags: LTP_PC_SMT */
                k32y_put16(p_ + 8 + 22, 1);        /* GroupCount */
                k32y_put64(p_ + 8 + 24, k32y_mask_of(c * smt, smt));
            }
        }
    }
    if (relationship == K32Y_RELATION_NUMA_NODE || relationship == K32Y_RELATION_ALL) {
        K32Y_BEGIN(K32Y_EX_NUMA_SIZE, K32Y_RELATION_NUMA_NODE)
        if (p_) {
            k32y_put32(p_ + 8, 0);                 /* NodeNumber */
            k32y_put16(p_ + 8 + 22, 1);            /* GroupCount */
            k32y_put64(p_ + 8 + 24, all);
        }
    }
    if (relationship == K32Y_RELATION_CACHE || relationship == K32Y_RELATION_ALL) {
        for (unsigned i = 0; i < t->ncaches; ++i) {
            const k32y_cache *k = &t->caches[i];
            unsigned share = k->shared_logical > 0 ? k->shared_logical : smt;
            if (share > t->logical)
                share = t->logical;
            for (unsigned first = 0; first < t->logical; first += share) {
                K32Y_BEGIN(K32Y_EX_CACHE_SIZE, K32Y_RELATION_CACHE)
                if (p_) {
                    p_[8] = k->level;
                    p_[9] = k->associativity;
                    k32y_put16(p_ + 10, k->line_size);
                    k32y_put32(p_ + 12, k->size);
                    k32y_put32(p_ + 16, k->type);
                    k32y_put16(p_ + 8 + 30, 1);    /* GroupCount */
                    k32y_put64(p_ + 8 + 32, k32y_mask_of(first, share < t->logical - first ? share : t->logical - first));
                }
            }
        }
    }
    if (relationship == K32Y_RELATION_PROCESSOR_PACKAGE || relationship == K32Y_RELATION_ALL) {
        K32Y_BEGIN(K32Y_EX_PACKAGE_SIZE, K32Y_RELATION_PROCESSOR_PACKAGE)
        if (p_) {
            p_[8] = smt > 1 ? 1 : 0;
            k32y_put16(p_ + 8 + 22, 1);
            k32y_put64(p_ + 8 + 24, all);
        }
    }
    if (relationship == K32Y_RELATION_GROUP || relationship == K32Y_RELATION_ALL) {
        K32Y_BEGIN(K32Y_EX_GROUP_SIZE, K32Y_RELATION_GROUP)
        if (p_) {
            k32y_put16(p_ + 8, 1);                  /* MaximumGroupCount */
            k32y_put16(p_ + 10, 1);                 /* ActiveGroupCount */
            p_[8 + 24] = 64;                        /* MaximumProcessorCount of group 0 */
            p_[8 + 25] = (uint8_t)t->logical;       /* ActiveProcessorCount */
            k32y_put64(p_ + 8 + 24 + 40, all);      /* ActiveProcessorMask */
        }
    }
#undef K32Y_BEGIN
    if (records)
        *records = nrec;
    return off;
}

#endif /* K32_Y_PURE_H */
