/*
 * kernel32_res.c - FindResource, LoadResource, LockResource, SizeofResource, FreeResource and the EnumResource functions,
 * on the resource directory of a loaded module (rules in k32_res_pure.h).
 *
 * A module handle is the image base. A handle with the low bit set is a module mapped as a data file (flat file layout),
 * with bit 1 set an image mapped only for its resources; neither is an executable image. HRSRC is the address of the
 * resource's data entry inside the mapping, HGLOBAL from LoadResource the address of the data.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "ntstatus.h"
#include "k32_res_pure.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
typedef HANDLE HMODULE;
typedef HANDLE HRSRC;
typedef HANDLE HGLOBAL;
typedef PVOID LPVOID;

#define KR_ERROR_INVALID_PARAMETER 87u
#define KR_ERROR_RESOURCE_DATA_NOT_FOUND 1812u
#define KR_ERROR_RESOURCE_TYPE_NOT_FOUND 1813u
#define KR_ERROR_RESOURCE_NAME_NOT_FOUND 1814u
#define KR_ERROR_RESOURCE_LANG_NOT_FOUND 1815u

void SetLastError(DWORD e);

typedef struct {
    kres_view v;
    const uint8_t *base;
} kr_mod;

static const uint8_t *kr_main_base(void)
{
    PTEB teb;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb) :: "memory");
    return (const uint8_t *)teb->ProcessEnvironmentBlock->ImageBaseAddress;
}

/* Opens the module for resource access; FALSE (with the last error set) when it has no resource directory. */
static BOOL kr_open(HMODULE h, kr_mod *m)
{
    uintptr_t raw = (uintptr_t)h;
    const uint8_t *b = raw ? (const uint8_t *)(raw & ~(uintptr_t)3) : kr_main_base();
    unsigned flags = raw ? (unsigned)(raw & 3) : 0;
    uint32_t nt, soi, nsec, optsize, hdrs, end = 0;
    size_t size;
    if (!b || b[0] != 'M' || b[1] != 'Z') {
        SetLastError(KR_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    nt = kres_u32(b + 0x3C);
    soi = kres_u32(b + nt + 24 + 56);
    hdrs = kres_u32(b + nt + 24 + 60);
    nsec = kres_u16(b + nt + 6);
    optsize = kres_u16(b + nt + 20);
    size = soi;
    if (flags & 1) { /* a flat file: as long as the last section's raw data ends */
        end = hdrs;
        for (uint32_t i = 0; i < nsec; ++i) {
            const uint8_t *s = b + nt + 24 + optsize + (size_t)i * 40;
            uint32_t e = kres_u32(s + 20) + kres_u32(s + 16);
            if (e > end)
                end = e;
        }
        size = end;
    }
    m->base = b;
    if (!kres_open(&m->v, b, size, (flags & 1) ? 0 : 1)) {
        SetLastError(KR_ERROR_RESOURCE_DATA_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

#define KR_IS_INTRESOURCE(p) (((uintptr_t)(p) >> 16) == 0)

static kres_key kr_key_w(const WCHAR *p)
{
    return KR_IS_INTRESOURCE(p) ? kres_key_id((uint32_t)(uintptr_t)p) : kres_key_string((const uint16_t *)p);
}

/* ---- Finding and loading ------------------------------------------------------------------------------------------------ */

HRSRC WINAPI FindResourceExW(HMODULE h, const WCHAR *type, const WCHAR *name, WORD lang)
{
    kr_mod m;
    kres_key t, n;
    uint32_t off;
    int r;
    const uint8_t *entry;
    if (!type || !name) {
        SetLastError(KR_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!kr_open(h, &m))
        return NULL;
    t = kr_key_w(type);
    n = kr_key_w(name);
    r = kres_find(&m.v, &t, &n, lang, &off, NULL);
    if (r <= 0) {
        SetLastError(r == 0 ? KR_ERROR_RESOURCE_TYPE_NOT_FOUND : r == -1 ? KR_ERROR_RESOURCE_NAME_NOT_FOUND : KR_ERROR_RESOURCE_LANG_NOT_FOUND);
        return NULL;
    }
    entry = kres_at(&m.v, off, 16);
    if (!entry) {
        SetLastError(KR_ERROR_RESOURCE_DATA_NOT_FOUND);
        return NULL;
    }
    return (HRSRC)entry;
}

HRSRC WINAPI FindResourceW(HMODULE h, const WCHAR *name, const WCHAR *type) { return FindResourceExW(h, type, name, 0); }

/* A narrow name becomes wide for the lookup; an integer resource passes through. */
static const WCHAR *kr_widen(const char *s, WCHAR *buf, size_t cap)
{
    size_t i;
    if (KR_IS_INTRESOURCE(s))
        return (const WCHAR *)s;
    for (i = 0; s[i] && i + 1 < cap; ++i)
        buf[i] = (unsigned char)s[i];
    buf[i] = 0;
    return buf;
}

HRSRC WINAPI FindResourceExA(HMODULE h, const char *type, const char *name, WORD lang)
{
    WCHAR tb[260], nb[260];
    if (!type || !name) {
        SetLastError(KR_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return FindResourceExW(h, kr_widen(type, tb, 260), kr_widen(name, nb, 260), lang);
}

HRSRC WINAPI FindResourceA(HMODULE h, const char *name, const char *type) { return FindResourceExA(h, type, name, 0); }

HGLOBAL WINAPI LoadResource(HMODULE h, HRSRC res)
{
    kr_mod m;
    kres_data d;
    const uint8_t *entry = (const uint8_t *)res, *p;
    if (!res) {
        SetLastError(KR_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!kr_open(h, &m))
        return NULL;
    /* the entry must lie in this module's resource directory */
    if (entry < m.v.base || entry + 16 > m.v.base + m.v.size) {
        SetLastError(KR_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    d.rva = kres_u32(entry);
    d.size = kres_u32(entry + 4);
    p = kres_ptr(&m.v, d.rva, d.size);
    if (!p) {
        SetLastError(KR_ERROR_RESOURCE_DATA_NOT_FOUND);
        return NULL;
    }
    return (HGLOBAL)p;
}

LPVOID WINAPI LockResource(HGLOBAL h) { return (LPVOID)h; }

DWORD WINAPI SizeofResource(HMODULE h, HRSRC res)
{
    (void)h;
    if (!res) {
        SetLastError(KR_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return kres_u32((const uint8_t *)res + 4);
}

BOOL WINAPI FreeResource(HGLOBAL h)
{
    (void)h;
    return FALSE; /* resources in a loaded module are never freed; the call returns 0 as it has since Win32 */
}

/* ---- Enumeration ---------------------------------------------------------------------------------------------------------- */

typedef BOOL (WINAPI *KR_TYPEPROC)(HMODULE, WCHAR *, LONG_PTR);
typedef BOOL (WINAPI *KR_NAMEPROC)(HMODULE, const WCHAR *, WCHAR *, LONG_PTR);
typedef BOOL (WINAPI *KR_LANGPROC)(HMODULE, const WCHAR *, const WCHAR *, WORD, LONG_PTR);
typedef BOOL (WINAPI *KR_TYPEPROCA)(HMODULE, char *, LONG_PTR);
typedef BOOL (WINAPI *KR_NAMEPROCA)(HMODULE, const char *, char *, LONG_PTR);
typedef BOOL (WINAPI *KR_LANGPROCA)(HMODULE, const char *, const char *, WORD, LONG_PTR);

/* The name of an entry as the callback receives it: MAKEINTRESOURCE(id) for numbers, else a NUL-terminated copy in buf. */
static const WCHAR *kr_entry_key(const kres_view *v, const kres_entry *e, WCHAR *buf, size_t cap)
{
    if (!e->is_name)
        return (const WCHAR *)(uintptr_t)e->id;
    {
        size_t n = kres_entry_name(v, e, (uint16_t *)buf, cap - 1);
        buf[n < cap - 1 ? n : cap - 1] = 0;
    }
    return buf;
}

static void kr_narrow(const WCHAR *w, char *out, size_t cap)
{
    size_t i;
    for (i = 0; w[i] && i + 1 < cap; ++i)
        out[i] = w[i] < 0x100 ? (char)w[i] : '?';
    out[i] = 0;
}

/* One generic walker: level 0 lists types, 1 names of a type, 2 languages of a name. ansi selects the narrow callbacks. */
static BOOL kr_enum(HMODULE h, const WCHAR *type, const WCHAR *name, void *proc, LONG_PTR param, int level, int ansi,
                    const void *atype, const void *aname)
{
    kr_mod m;
    kres_entry t, n;
    uint32_t dir = 0;
    kres_counts c;
    WCHAR wb[260];
    char nb[260];
    if (!proc) {
        SetLastError(KR_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!kr_open(h, &m))
        return FALSE;
    if (level >= 1) {
        kres_key tk = kr_key_w(type);
        if (!kres_find_in(&m.v, 0, &tk, &t) || !t.is_dir) {
            SetLastError(KR_ERROR_RESOURCE_TYPE_NOT_FOUND);
            return FALSE;
        }
        dir = t.target;
    }
    if (level >= 2) {
        kres_key nk = kr_key_w(name);
        if (!kres_find_in(&m.v, dir, &nk, &n) || !n.is_dir) {
            SetLastError(KR_ERROR_RESOURCE_NAME_NOT_FOUND);
            return FALSE;
        }
        dir = n.target;
    }
    if (!kres_dir_counts(&m.v, dir, &c))
        return TRUE;
    for (unsigned i = 0; i < c.named + c.ids; ++i) {
        kres_entry e;
        BOOL go;
        if (!kres_dir_entry(&m.v, dir, i, &e))
            break;
        if (level == 2) {
            go = ansi ? ((KR_LANGPROCA)proc)(h, (const char *)atype, (const char *)aname, (WORD)e.id, param)
                      : ((KR_LANGPROC)proc)(h, type, name, (WORD)e.id, param);
        } else {
            const WCHAR *key = kr_entry_key(&m.v, &e, wb, 260);
            const char *akey = (const char *)key;
            if (ansi && e.is_name) {
                kr_narrow(key, nb, sizeof nb);
                akey = nb;
            }
            if (level == 0)
                go = ansi ? ((KR_TYPEPROCA)proc)(h, (char *)akey, param) : ((KR_TYPEPROC)proc)(h, (WCHAR *)key, param);
            else if (ansi) {
                go = ((KR_NAMEPROCA)proc)(h, (const char *)atype, (char *)akey, param);
            } else {
                go = ((KR_NAMEPROC)proc)(h, type, (WCHAR *)key, param);
            }
        }
        if (!go)
            return FALSE;
    }
    return TRUE;
}

BOOL WINAPI EnumResourceTypesW(HMODULE h, KR_TYPEPROC p, LONG_PTR l) { return kr_enum(h, NULL, NULL, (void *)p, l, 0, 0, NULL, NULL); }
BOOL WINAPI EnumResourceTypesExW(HMODULE h, KR_TYPEPROC p, LONG_PTR l, DWORD flags, WORD lang)
{
    (void)flags;
    (void)lang;
    return kr_enum(h, NULL, NULL, (void *)p, l, 0, 0, NULL, NULL);
}
BOOL WINAPI EnumResourceTypesA(HMODULE h, KR_TYPEPROCA p, LONG_PTR l) { return kr_enum(h, NULL, NULL, (void *)p, l, 0, 1, NULL, NULL); }
BOOL WINAPI EnumResourceTypesExA(HMODULE h, KR_TYPEPROCA p, LONG_PTR l, DWORD flags, WORD lang)
{
    (void)flags;
    (void)lang;
    return kr_enum(h, NULL, NULL, (void *)p, l, 0, 1, NULL, NULL);
}

BOOL WINAPI EnumResourceNamesW(HMODULE h, const WCHAR *type, KR_NAMEPROC p, LONG_PTR l) { return kr_enum(h, type, NULL, (void *)p, l, 1, 0, NULL, NULL); }
BOOL WINAPI EnumResourceNamesExW(HMODULE h, const WCHAR *type, KR_NAMEPROC p, LONG_PTR l, DWORD flags, WORD lang)
{
    (void)flags;
    (void)lang;
    return kr_enum(h, type, NULL, (void *)p, l, 1, 0, NULL, NULL);
}
BOOL WINAPI EnumResourceNamesA(HMODULE h, const char *type, KR_NAMEPROCA p, LONG_PTR l)
{
    WCHAR tb[260];
    return kr_enum(h, kr_widen(type, tb, 260), NULL, (void *)p, l, 1, 1, type, NULL);
}
BOOL WINAPI EnumResourceNamesExA(HMODULE h, const char *type, KR_NAMEPROCA p, LONG_PTR l, DWORD flags, WORD lang)
{
    WCHAR tb[260];
    (void)flags;
    (void)lang;
    return kr_enum(h, kr_widen(type, tb, 260), NULL, (void *)p, l, 1, 1, type, NULL);
}

BOOL WINAPI EnumResourceLanguagesW(HMODULE h, const WCHAR *type, const WCHAR *name, KR_LANGPROC p, LONG_PTR l)
{
    return kr_enum(h, type, name, (void *)p, l, 2, 0, NULL, NULL);
}
BOOL WINAPI EnumResourceLanguagesExW(HMODULE h, const WCHAR *type, const WCHAR *name, KR_LANGPROC p, LONG_PTR l, DWORD flags, WORD lang)
{
    (void)flags;
    (void)lang;
    return kr_enum(h, type, name, (void *)p, l, 2, 0, NULL, NULL);
}
BOOL WINAPI EnumResourceLanguagesA(HMODULE h, const char *type, const char *name, KR_LANGPROCA p, LONG_PTR l)
{
    WCHAR tb[260], nb[260];
    return kr_enum(h, kr_widen(type, tb, 260), kr_widen(name, nb, 260), (void *)p, l, 2, 1, type, name);
}
BOOL WINAPI EnumResourceLanguagesExA(HMODULE h, const char *type, const char *name, KR_LANGPROCA p, LONG_PTR l, DWORD flags, WORD lang)
{
    WCHAR tb[260], nb[260];
    (void)flags;
    (void)lang;
    return kr_enum(h, kr_widen(type, tb, 260), kr_widen(name, nb, 260), (void *)p, l, 2, 1, type, name);
}

#endif /* __MINGW32__ */
