/*
 * k32_res_pure.h - walking the resource directory of a PE image, for FindResource, LoadResource, SizeofResource and the
 * EnumResource functions.
 *
 * A view is a module either mapped as an image (offsets are RVAs from the base) or mapped as a flat file (LOAD_LIBRARY_AS_
 * DATAFILE: offsets are RVAs that the section table turns into file offsets). Every read is bounds checked against the
 * size of the mapping, so a damaged directory yields "not found", never a wild read. The resource tree is three levels:
 * type, name, language; entries are either a number or the offset of a UTF-16 name, and point at a subdirectory or at a
 * data entry {RVA, size, code page}.
 * Host tests: tests/ntdll-host/test_k32_res_pure.c.
 */
#ifndef K32_RES_PURE_H
#define K32_RES_PURE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const uint8_t *base;
    size_t size;
    int image;                 /* 1: mapped image, 0: flat file */
    uint32_t res_rva, res_size;
    uint32_t nsections, sec_off; /* section table (offset into the mapping), for flat files */
} kres_view;

static inline uint16_t kres_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t kres_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* The bytes at an RVA, or NULL when [rva, rva + len) is not inside the mapping. */
static inline const uint8_t *kres_ptr(const kres_view *v, uint32_t rva, size_t len)
{
    if (v->image)
        return (size_t)rva <= v->size && len <= v->size - rva ? v->base + rva : NULL;
    for (uint32_t i = 0; i < v->nsections; ++i) {
        const uint8_t *s = v->base + v->sec_off + (size_t)i * 40;
        uint32_t vsize = kres_u32(s + 8), va = kres_u32(s + 12), rawsize = kres_u32(s + 16), raw = kres_u32(s + 20);
        uint32_t span = vsize > rawsize ? vsize : rawsize;
        if (rva >= va && (size_t)(rva - va) < span) {
            size_t off = (size_t)raw + (rva - va);
            if (rva - va + len > rawsize || off > v->size || len > v->size - off)
                return NULL;
            return v->base + off;
        }
    }
    return NULL;
}

/* Opens a mapping. Returns 1 when it is a PE image with a resource directory. */
static inline int kres_open(kres_view *v, const void *base, size_t size, int image)
{
    const uint8_t *b = (const uint8_t *)base;
    uint32_t nt;
    uint16_t nsec, optsize;
    uint32_t ndirs;
    memset(v, 0, sizeof *v);
    v->base = b;
    v->size = size;
    v->image = image;
    if (size < 0x40 || b[0] != 'M' || b[1] != 'Z')
        return 0;
    nt = kres_u32(b + 0x3C);
    if (nt > size || size - nt < 4 + 20 + 112 + 16 * 3 || kres_u32(b + nt) != 0x00004550u)
        return 0;
    nsec = kres_u16(b + nt + 6);
    optsize = kres_u16(b + nt + 20);
    if (kres_u16(b + nt + 24) != 0x20B)
        return 0; /* PE32+ only */
    ndirs = kres_u32(b + nt + 24 + 108);
    if (ndirs <= 2)
        return 0;
    v->res_rva = kres_u32(b + nt + 24 + 112 + 2 * 8);
    v->res_size = kres_u32(b + nt + 24 + 112 + 2 * 8 + 4);
    v->sec_off = nt + 24 + optsize;
    v->nsections = nsec;
    if (v->sec_off > size || (size - v->sec_off) / 40 < nsec)
        v->nsections = 0;
    return v->res_rva != 0 && v->res_size != 0;
}

/* ---- directory access ------------------------------------------------------------------------------------------------------ */

/* Directory offsets are relative to the start of the resource section data. */
static inline const uint8_t *kres_at(const kres_view *v, uint32_t off, size_t len)
{
    if (off > v->res_size || len > v->res_size - off)
        return NULL;
    return kres_ptr(v, v->res_rva + off, len);
}

typedef struct {
    unsigned named, ids;
} kres_counts;

static inline int kres_dir_counts(const kres_view *v, uint32_t dir, kres_counts *c)
{
    const uint8_t *d = kres_at(v, dir, 16);
    if (!d)
        return 0;
    c->named = kres_u16(d + 12);
    c->ids = kres_u16(d + 14);
    return 1;
}

typedef struct {
    int is_name;
    uint32_t id;           /* when !is_name */
    uint32_t name_off;     /* when is_name: offset of the string */
    int is_dir;
    uint32_t target;       /* subdirectory offset, or data entry offset */
} kres_entry;

static inline int kres_dir_entry(const kres_view *v, uint32_t dir, unsigned index, kres_entry *e)
{
    kres_counts c;
    const uint8_t *p;
    uint32_t name, off;
    if (!kres_dir_counts(v, dir, &c) || index >= c.named + c.ids)
        return 0;
    p = kres_at(v, dir + 16 + index * 8, 8);
    if (!p)
        return 0;
    name = kres_u32(p);
    off = kres_u32(p + 4);
    e->is_name = (name & 0x80000000u) != 0;
    e->id = e->is_name ? 0 : name;
    e->name_off = name & 0x7FFFFFFFu;
    e->is_dir = (off & 0x80000000u) != 0;
    e->target = off & 0x7FFFFFFFu;
    return 1;
}

/* The UTF-16 name of an entry: units are copied to out (cap units, no NUL stored); returns the full length. */
static inline size_t kres_entry_name(const kres_view *v, const kres_entry *e, uint16_t *out, size_t cap)
{
    const uint8_t *p = kres_at(v, e->name_off, 2);
    size_t n, i;
    if (!p)
        return 0;
    n = kres_u16(p);
    if (!kres_at(v, e->name_off + 2, n * 2))
        return 0;
    p = kres_at(v, e->name_off + 2, n * 2);
    for (i = 0; i < n && i < cap; ++i)
        out[i] = kres_u16(p + i * 2);
    return n;
}

static inline uint16_t kres_fold(uint16_t c)
{
    if (c >= 'a' && c <= 'z')
        return (uint16_t)(c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return (uint16_t)(c - 32);
    return c;
}

/* A resource type or name given by the caller: a number, or a string (which may spell a number as "#123"). */
typedef struct {
    int is_name;
    uint32_t id;
    const uint16_t *name; /* NUL-terminated when is_name */
} kres_key;

static inline kres_key kres_key_id(uint32_t id) { kres_key k = {0, id, NULL}; return k; }

static inline kres_key kres_key_string(const uint16_t *s)
{
    kres_key k = {1, 0, s};
    uint32_t v = 0;
    size_t i = 1;
    if (s[0] == '#' && s[1]) {
        for (; s[i] >= '0' && s[i] <= '9'; ++i)
            v = v * 10 + (uint32_t)(s[i] - '0');
        if (!s[i]) {
            k.is_name = 0;
            k.id = v;
            k.name = NULL;
        }
    }
    return k;
}

static inline int kres_entry_matches(const kres_view *v, const kres_entry *e, const kres_key *k)
{
    if (!k->is_name)
        return !e->is_name && e->id == k->id;
    if (!e->is_name)
        return 0;
    {
        const uint8_t *p = kres_at(v, e->name_off, 2);
        size_t n, i;
        if (!p)
            return 0;
        n = kres_u16(p);
        p = kres_at(v, e->name_off + 2, n * 2);
        if (!p)
            return 0;
        for (i = 0; i < n; ++i)
            if (!k->name[i] || kres_fold(kres_u16(p + i * 2)) != kres_fold(k->name[i]))
                return 0;
        return k->name[n] == 0;
    }
}

/* Finds the subdirectory (or entry) of `dir` that matches the key. */
static inline int kres_find_in(const kres_view *v, uint32_t dir, const kres_key *k, kres_entry *out)
{
    kres_counts c;
    if (!kres_dir_counts(v, dir, &c))
        return 0;
    for (unsigned i = 0; i < c.named + c.ids; ++i) {
        kres_entry e;
        if (kres_dir_entry(v, dir, i, &e) && kres_entry_matches(v, &e, k)) {
            *out = e;
            return 1;
        }
    }
    return 0;
}

/* ---- finding a resource -------------------------------------------------------------------------------------------------------- */

/* The offset of the data entry of resource (type, name) in language `lang`. The languages tried, in order: the one asked
 * for, its primary language with the neutral sublanguage, language 0 (neutral), US English (0x0409), then the first one
 * the resource has. Returns 1 and the offset; or 0 when the type is not there, -1 when the name is not there, -2 when
 * the name has no language entry (the three Win32 errors ERROR_RESOURCE_TYPE/NAME/LANG_NOT_FOUND). */
static inline int kres_find(const kres_view *v, const kres_key *type, const kres_key *name, uint16_t lang, uint32_t *data_entry_off,
                            uint16_t *found_lang)
{
    kres_entry t, n, l;
    kres_counts c;
    uint16_t tries[4];
    unsigned ntries = 0;
    if (!kres_find_in(v, 0, type, &t) || !t.is_dir)
        return 0;
    if (!kres_find_in(v, t.target, name, &n) || !n.is_dir)
        return -1;
    if (!kres_dir_counts(v, n.target, &c) || c.named + c.ids == 0)
        return -2;
    tries[ntries++] = lang;
    if (lang >= 0x400)
        tries[ntries++] = (uint16_t)(lang & 0x3FF);
    tries[ntries++] = 0;
    tries[ntries++] = 0x0409;
    for (unsigned i = 0; i < ntries; ++i) {
        kres_key lk = kres_key_id(tries[i]);
        if (kres_find_in(v, n.target, &lk, &l) && !l.is_dir) {
            *data_entry_off = l.target;
            if (found_lang)
                *found_lang = tries[i];
            return 1;
        }
    }
    if (kres_dir_entry(v, n.target, 0, &l) && !l.is_dir) {
        *data_entry_off = l.target;
        if (found_lang)
            *found_lang = (uint16_t)l.id;
        return 1;
    }
    return -2;
}

typedef struct {
    uint32_t rva, size, codepage;
} kres_data;

static inline int kres_data_of(const kres_view *v, uint32_t entry_off, kres_data *d)
{
    const uint8_t *p = kres_at(v, entry_off, 16);
    if (!p)
        return 0;
    d->rva = kres_u32(p);
    d->size = kres_u32(p + 4);
    d->codepage = kres_u32(p + 8);
    return 1;
}

#endif /* K32_RES_PURE_H */
