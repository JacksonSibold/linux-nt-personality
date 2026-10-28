/*
 * kernel32_ini.c - the profile (INI file) functions: GetPrivateProfile* and WritePrivateProfile*, the win.ini forms
 * GetProfile* and WriteProfile*, with their ANSI entry points.
 *
 * A file name without a directory refers to the Windows directory, as on Windows. Each call reads the whole file,
 * works on it in memory (rules in k32_ini_pure.h) and, for a write, writes the whole file back in the encoding it was
 * read in (a new file is ANSI). A process-wide lock keeps the read-modify-write of two threads apart; another process
 * is not excluded.
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
#include "k32_ini_pure.h"
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
#ifndef UINT
#define UINT unsigned int
#endif
#ifndef INT
#define INT int
#endif
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

#define KI_MAX_FILE (16u * 1024u * 1024u)
#define KI_MAX_PATH 520

__declspec(dllimport) extern NTSTATUS NtYieldExecution(void);
void SetLastError(DWORD e);
DWORD GetLastError(void);
HANDLE WINAPI GetProcessHeap(void);
PVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T n);
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, PVOID p);
HANDLE WINAPI CreateFileW(const WCHAR *, DWORD, DWORD, PVOID, DWORD, DWORD, HANDLE);
BOOL WINAPI ReadFile(HANDLE, PVOID, DWORD, DWORD *, PVOID);
BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, PVOID);
BOOL WINAPI GetFileSizeEx(HANDLE, PLARGE_INTEGER);
BOOL WINAPI CloseHandle(HANDLE);
UINT WINAPI GetWindowsDirectoryW(WCHAR *, UINT);

static void *ki_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
static void ki_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static volatile LONG g_ki_lock;

static void ki_lock(void)
{
    while (__sync_lock_test_and_set(&g_ki_lock, 1))
        NtYieldExecution();
}

static void ki_unlock(void) { __sync_lock_release(&g_ki_lock); }

/* The path of a profile file: as given when it has a directory part, else in the Windows directory. */
static void ki_path(const WCHAR *file, WCHAR *out)
{
    static const WCHAR winini[] = {'w', 'i', 'n', '.', 'i', 'n', 'i', 0};
    size_t n = 0, i;
    int has_dir = 0;
    if (!file)
        file = winini;
    for (i = 0; file[i]; ++i)
        if (file[i] == '\\' || file[i] == '/' || file[i] == ':')
            has_dir = 1;
    if (!has_dir) {
        UINT w = GetWindowsDirectoryW(out, KI_MAX_PATH - 16);
        n = w < KI_MAX_PATH - 16 ? w : 0;
        out[n++] = '\\';
    }
    for (i = 0; file[i] && n + 1 < KI_MAX_PATH; ++i)
        out[n++] = file[i];
    out[n] = 0;
}

typedef struct {
    kini_wchar *text;
    size_t n;
    int enc;
} ki_doc;

/* Reads and decodes a file; a missing file is an empty ANSI document. Returns FALSE only on a failed allocation or read. */
static BOOL ki_load(const WCHAR *path, ki_doc *d)
{
    HANDLE h = CreateFileW(path, 0x80000000u /* GENERIC_READ */, 3, NULL, 3 /* OPEN_EXISTING */, 0x80, NULL);
    LARGE_INTEGER size;
    uint8_t *raw;
    DWORD got = 0;
    d->text = NULL;
    d->n = 0;
    d->enc = KINI_ANSI;
    if (h == INVALID_HANDLE_VALUE)
        return TRUE;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > KI_MAX_FILE) {
        CloseHandle(h);
        return FALSE;
    }
    raw = (uint8_t *)ki_alloc((size_t)size.QuadPart);
    d->text = (kini_wchar *)ki_alloc(((size_t)size.QuadPart + 1) * sizeof(kini_wchar));
    if (!raw || !d->text || (size.QuadPart && !ReadFile(h, raw, (DWORD)size.QuadPart, &got, NULL))) {
        ki_free(raw);
        ki_free(d->text);
        d->text = NULL;
        CloseHandle(h);
        return FALSE;
    }
    d->n = kini_decode(raw, got, d->text, &d->enc);
    ki_free(raw);
    CloseHandle(h);
    return TRUE;
}

static BOOL ki_store(const WCHAR *path, const kini_wchar *text, size_t n, int enc)
{
    size_t bytes = kini_encode(text, n, enc, NULL);
    uint8_t *raw = (uint8_t *)ki_alloc(bytes);
    HANDLE h;
    DWORD wrote = 0;
    BOOL ok;
    if (!raw)
        return FALSE;
    kini_encode(text, n, enc, raw);
    h = CreateFileW(path, 0x40000000u /* GENERIC_WRITE */, 1, NULL, 2 /* CREATE_ALWAYS */, 0x80, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        ki_free(raw);
        return FALSE;
    }
    ok = bytes == 0 || WriteFile(h, raw, (DWORD)bytes, &wrote, NULL);
    ok = ok && wrote == bytes;
    CloseHandle(h);
    ki_free(raw);
    return ok;
}

static void ki_release(ki_doc *d) { ki_free(d->text); d->text = NULL; }

/* ---- Reading ------------------------------------------------------------------------------------------------------------ */

DWORD WINAPI GetPrivateProfileStringW(const WCHAR *app, const WCHAR *key, const WCHAR *def, WCHAR *ret, DWORD size, const WCHAR *file)
{
    WCHAR path[KI_MAX_PATH];
    ki_doc d;
    DWORD n;
    if (!ret || !size) {
        SetLastError(87);
        return 0;
    }
    ki_path(file, path);
    ki_lock();
    if (!ki_load(path, &d)) {
        ki_unlock();
        ret[0] = 0;
        return 0;
    }
    if (!app)
        n = (DWORD)kini_get_names(d.text, d.n, NULL, (kini_wchar *)ret, size);
    else if (!key)
        n = (DWORD)kini_get_names(d.text, d.n, (const kini_wchar *)app, (kini_wchar *)ret, size);
    else
        n = (DWORD)kini_get_string(d.text, d.n, (const kini_wchar *)app, (const kini_wchar *)key, (const kini_wchar *)def,
                                   (kini_wchar *)ret, size);
    ki_release(&d);
    ki_unlock();
    return n;
}

UINT WINAPI GetPrivateProfileIntW(const WCHAR *app, const WCHAR *key, INT def, const WCHAR *file)
{
    WCHAR path[KI_MAX_PATH];
    ki_doc d;
    UINT v;
    if (!app || !key)
        return 0;
    ki_path(file, path);
    ki_lock();
    if (!ki_load(path, &d)) {
        ki_unlock();
        return (UINT)def;
    }
    v = kini_get_int(d.text, d.n, (const kini_wchar *)app, (const kini_wchar *)key, def);
    ki_release(&d);
    ki_unlock();
    return v;
}

DWORD WINAPI GetPrivateProfileSectionW(const WCHAR *app, WCHAR *ret, DWORD size, const WCHAR *file)
{
    WCHAR path[KI_MAX_PATH];
    ki_doc d;
    DWORD n;
    if (!ret || size < 2) {
        if (ret && size)
            ret[0] = 0;
        return 0;
    }
    if (!app) {
        ret[0] = ret[1] = 0;
        return 0;
    }
    ki_path(file, path);
    ki_lock();
    if (!ki_load(path, &d)) {
        ki_unlock();
        ret[0] = ret[1] = 0;
        return 0;
    }
    n = (DWORD)kini_get_section(d.text, d.n, (const kini_wchar *)app, (kini_wchar *)ret, size);
    ki_release(&d);
    ki_unlock();
    return n;
}

DWORD WINAPI GetPrivateProfileSectionNamesW(WCHAR *ret, DWORD size, const WCHAR *file)
{
    return GetPrivateProfileStringW(NULL, NULL, NULL, ret, size, file);
}

BOOL WINAPI GetPrivateProfileStructW(const WCHAR *app, const WCHAR *key, PVOID buf, UINT size, const WCHAR *file)
{
    WCHAR path[KI_MAX_PATH];
    ki_doc d;
    size_t vs, ve;
    BOOL ok = FALSE;
    if (!app || !key || !buf)
        return FALSE;
    ki_path(file, path);
    ki_lock();
    if (ki_load(path, &d)) {
        if (kini_find_value(d.text, d.n, (const kini_wchar *)app, (const kini_wchar *)key, &vs, &ve))
            ok = kini_hex_decode(d.text + vs, ve - vs, (uint8_t *)buf, size) ? TRUE : FALSE;
        ki_release(&d);
    }
    ki_unlock();
    return ok;
}

/* ---- Writing ---------------------------------------------------------------------------------------------------------------- */

typedef enum { KI_SET, KI_SECTION } ki_edit_kind;

static BOOL ki_edit(const WCHAR *file, ki_edit_kind kind, const WCHAR *app, const WCHAR *key, const WCHAR *value)
{
    WCHAR path[KI_MAX_PATH];
    ki_doc d;
    kini_wchar *out;
    size_t need;
    BOOL ok;
    ki_path(file, path);
    ki_lock();
    if (!ki_load(path, &d)) {
        ki_unlock();
        return FALSE;
    }
    if (kind == KI_SET)
        need = kini_set(d.text, d.n, (const kini_wchar *)app, (const kini_wchar *)key, (const kini_wchar *)value, NULL, 0);
    else
        need = kini_set_section(d.text, d.n, (const kini_wchar *)app, (const kini_wchar *)value, NULL, 0);
    out = (kini_wchar *)ki_alloc((need + 1) * sizeof(kini_wchar));
    if (!out) {
        ki_release(&d);
        ki_unlock();
        SetLastError(8);
        return FALSE;
    }
    if (kind == KI_SET)
        kini_set(d.text, d.n, (const kini_wchar *)app, (const kini_wchar *)key, (const kini_wchar *)value, out, need);
    else
        kini_set_section(d.text, d.n, (const kini_wchar *)app, (const kini_wchar *)value, out, need);
    /* nothing to write when removing from a file that does not exist */
    ok = (need == 0 && d.n == 0) ? TRUE : ki_store(path, out, need, d.enc);
    ki_free(out);
    ki_release(&d);
    ki_unlock();
    return ok;
}

BOOL WINAPI WritePrivateProfileStringW(const WCHAR *app, const WCHAR *key, const WCHAR *value, const WCHAR *file)
{
    if (!app)
        return TRUE; /* flushes the cache on Windows; there is none */
    return ki_edit(file, KI_SET, app, key, value);
}

BOOL WINAPI WritePrivateProfileSectionW(const WCHAR *app, const WCHAR *pairs, const WCHAR *file)
{
    if (!app)
        return TRUE;
    return ki_edit(file, KI_SECTION, app, NULL, pairs);
}

BOOL WINAPI WritePrivateProfileStructW(const WCHAR *app, const WCHAR *key, PVOID buf, UINT size, const WCHAR *file)
{
    WCHAR *hex;
    BOOL ok;
    if (!app)
        return TRUE;
    if (!key || !buf)
        return ki_edit(file, KI_SET, app, key, NULL);
    hex = (WCHAR *)ki_alloc(((size_t)size * 2 + 3) * sizeof(WCHAR));
    if (!hex) {
        SetLastError(8);
        return FALSE;
    }
    hex[kini_hex_encode((const uint8_t *)buf, size, (kini_wchar *)hex)] = 0;
    ok = ki_edit(file, KI_SET, app, key, hex);
    ki_free(hex);
    return ok;
}

/* ---- win.ini -------------------------------------------------------------------------------------------------------------------- */

DWORD WINAPI GetProfileStringW(const WCHAR *app, const WCHAR *key, const WCHAR *def, WCHAR *ret, DWORD size)
{
    return GetPrivateProfileStringW(app, key, def, ret, size, NULL);
}

UINT WINAPI GetProfileIntW(const WCHAR *app, const WCHAR *key, INT def) { return GetPrivateProfileIntW(app, key, def, NULL); }
DWORD WINAPI GetProfileSectionW(const WCHAR *app, WCHAR *ret, DWORD size) { return GetPrivateProfileSectionW(app, ret, size, NULL); }
BOOL WINAPI WriteProfileStringW(const WCHAR *app, const WCHAR *key, const WCHAR *value) { return WritePrivateProfileStringW(app, key, value, NULL); }
BOOL WINAPI WriteProfileSectionW(const WCHAR *app, const WCHAR *pairs) { return WritePrivateProfileSectionW(app, pairs, NULL); }

/* ---- ANSI entry points ---------------------------------------------------------------------------------------------------------- */

/* Narrow text is taken as Latin-1; wide results lose what Latin-1 cannot hold. */
static WCHAR *ki_wide(const char *s)
{
    size_t n = 0, i;
    WCHAR *w;
    if (!s)
        return NULL;
    while (s[n])
        ++n;
    w = (WCHAR *)ki_alloc((n + 1) * sizeof(WCHAR));
    if (!w)
        return NULL;
    for (i = 0; i <= n; ++i)
        w[i] = (unsigned char)s[i];
    return w;
}

/* A double-null list of narrow strings. */
static WCHAR *ki_wide_list(const char *s)
{
    size_t n = 0, i;
    WCHAR *w;
    if (!s)
        return NULL;
    while (s[n] || s[n + 1])
        ++n;
    n += 2;
    w = (WCHAR *)ki_alloc(n * sizeof(WCHAR));
    if (!w)
        return NULL;
    for (i = 0; i < n; ++i)
        w[i] = (unsigned char)s[i];
    return w;
}

static void ki_narrow(const WCHAR *w, char *out, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        out[i] = w[i] < 0x100 ? (char)w[i] : '?';
}

#define KI_FREE3(a, b, c) do { ki_free(a); ki_free(b); ki_free(c); } while (0)

DWORD WINAPI GetPrivateProfileStringA(const char *app, const char *key, const char *def, char *ret, DWORD size, const char *file)
{
    WCHAR *wa = ki_wide(app), *wk = ki_wide(key), *wd = ki_wide(def), *wf = ki_wide(file), *wr;
    DWORD n;
    if (!ret || !size) {
        SetLastError(87);
        return 0;
    }
    wr = (WCHAR *)ki_alloc((size_t)size * sizeof(WCHAR));
    if (!wr || (app && !wa) || (key && !wk) || (def && !wd) || (file && !wf)) {
        KI_FREE3(wa, wk, wd);
        ki_free(wf);
        ki_free(wr);
        SetLastError(8);
        return 0;
    }
    n = GetPrivateProfileStringW(wa, wk, wd, wr, size, wf);
    ki_narrow(wr, ret, (size_t)(!app || !key ? n + 2 : n + 1) <= size ? (size_t)(!app || !key ? n + 2 : n + 1) : size);
    KI_FREE3(wa, wk, wd);
    ki_free(wf);
    ki_free(wr);
    return n;
}

UINT WINAPI GetPrivateProfileIntA(const char *app, const char *key, INT def, const char *file)
{
    WCHAR *wa = ki_wide(app), *wk = ki_wide(key), *wf = ki_wide(file);
    UINT v = (app && key && (!wa || !wk)) ? (UINT)def : GetPrivateProfileIntW(wa, wk, def, wf);
    KI_FREE3(wa, wk, wf);
    return v;
}

DWORD WINAPI GetPrivateProfileSectionA(const char *app, char *ret, DWORD size, const char *file)
{
    WCHAR *wa = ki_wide(app), *wf = ki_wide(file), *wr;
    DWORD n;
    if (!ret || size < 2) {
        if (ret && size)
            ret[0] = 0;
        ki_free(wa);
        ki_free(wf);
        return 0;
    }
    wr = (WCHAR *)ki_alloc((size_t)size * sizeof(WCHAR));
    if (!wr) {
        ki_free(wa);
        ki_free(wf);
        return 0;
    }
    n = GetPrivateProfileSectionW(wa, wr, size, wf);
    ki_narrow(wr, ret, (size_t)n + 2 <= size ? (size_t)n + 2 : size);
    ki_free(wa);
    ki_free(wf);
    ki_free(wr);
    return n;
}

DWORD WINAPI GetPrivateProfileSectionNamesA(char *ret, DWORD size, const char *file)
{
    return GetPrivateProfileStringA(NULL, NULL, NULL, ret, size, file);
}

BOOL WINAPI GetPrivateProfileStructA(const char *app, const char *key, PVOID buf, UINT size, const char *file)
{
    WCHAR *wa = ki_wide(app), *wk = ki_wide(key), *wf = ki_wide(file);
    BOOL ok = (app && key && (!wa || !wk)) ? FALSE : GetPrivateProfileStructW(wa, wk, buf, size, wf);
    KI_FREE3(wa, wk, wf);
    return ok;
}

BOOL WINAPI WritePrivateProfileStringA(const char *app, const char *key, const char *value, const char *file)
{
    WCHAR *wa = ki_wide(app), *wk = ki_wide(key), *wv = ki_wide(value), *wf = ki_wide(file);
    BOOL ok;
    if ((app && !wa) || (key && !wk) || (value && !wv)) {
        KI_FREE3(wa, wk, wv);
        ki_free(wf);
        SetLastError(8);
        return FALSE;
    }
    ok = WritePrivateProfileStringW(wa, wk, wv, wf);
    KI_FREE3(wa, wk, wv);
    ki_free(wf);
    return ok;
}

BOOL WINAPI WritePrivateProfileSectionA(const char *app, const char *pairs, const char *file)
{
    WCHAR *wa = ki_wide(app), *wp = ki_wide_list(pairs), *wf = ki_wide(file);
    BOOL ok;
    if ((app && !wa) || (pairs && !wp)) {
        KI_FREE3(wa, wp, wf);
        SetLastError(8);
        return FALSE;
    }
    ok = WritePrivateProfileSectionW(wa, wp, wf);
    KI_FREE3(wa, wp, wf);
    return ok;
}

BOOL WINAPI WritePrivateProfileStructA(const char *app, const char *key, PVOID buf, UINT size, const char *file)
{
    WCHAR *wa = ki_wide(app), *wk = ki_wide(key), *wf = ki_wide(file);
    BOOL ok = (app && (!wa || (key && !wk))) ? FALSE : WritePrivateProfileStructW(wa, wk, buf, size, wf);
    KI_FREE3(wa, wk, wf);
    return ok;
}

DWORD WINAPI GetProfileStringA(const char *app, const char *key, const char *def, char *ret, DWORD size)
{
    return GetPrivateProfileStringA(app, key, def, ret, size, NULL);
}

UINT WINAPI GetProfileIntA(const char *app, const char *key, INT def) { return GetPrivateProfileIntA(app, key, def, NULL); }
DWORD WINAPI GetProfileSectionA(const char *app, char *ret, DWORD size) { return GetPrivateProfileSectionA(app, ret, size, NULL); }
BOOL WINAPI WriteProfileStringA(const char *app, const char *key, const char *value) { return WritePrivateProfileStringA(app, key, value, NULL); }
BOOL WINAPI WriteProfileSectionA(const char *app, const char *pairs) { return WritePrivateProfileSectionA(app, pairs, NULL); }

#endif /* __MINGW32__ */
