/*
 * advapi32_reg.c - the registry functions of advapi32.dll, wide and ANSI, on the NT registry services.
 *
 * A key handle is the NT handle. The predefined keys (HKEY_LOCAL_MACHINE and the others) stand for fixed NT paths
 * (adv_reg_pure.h); a name given with one is the path under it, with an open key it is relative to that key. The functions
 * return Win32 error codes directly and do not touch the last error. The ANSI forms convert names and the data of string
 * types through the system code page; the registry keeps UTF-16.
 *
 * The file also compiles on the host (ADV_HOST_TEST) so that tests/kernel-host/advapi_reg_test.c can run it against the kernel's
 * registry. Differences from Windows: one registry view (the WOW64 flags are dropped), HKEY_CLASSES_ROOT is only the machine's
 * Software\Classes, there is no key notification, and a key's security is neither read nor set.
 */
#if defined(__MINGW32__) || defined(ADV_HOST_TEST)
#include "nt_types.h"
#include "ntstatus.h"
#include "adv_reg_pure.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef HKEY
typedef HANDLE HKEY;
#endif
#ifndef LSTATUS
typedef LONG LSTATUS;
#endif
typedef unsigned short RWCHAR;

#define R_OBJ_CASE_INSENSITIVE 0x40UL
#define R_OBJ_OPENLINK 0x100UL
#define R_OPT_OPEN_LINK 0x8UL
#define R_MAXIMUM_ALLOWED 0x02000000UL
#define R_DELETE 0x00010000UL
#define R_KEY_QUERY_VALUE 0x1UL
#define R_KEY_SET_VALUE 0x2UL
#define R_KEY_ENUM_SUBKEYS 0x8UL
#define R_WOW_MASK 0x300UL

#define R_INFO_BASIC 0UL
#define R_INFO_NODE 1UL
#define R_INFO_FULL 2UL
#define R_VALUE_BASIC 0UL
#define R_VALUE_FULL 1UL
#define R_VALUE_PARTIAL 2UL

#define R_ERR_NOMEM 8L
#define R_ERR_NOT_IMPL 120L
#define R_ERR_BAD_NETPATH 53L

__declspec(dllimport) NTSTATUS NtOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
__declspec(dllimport) NTSTATUS NtCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, PULONG);
__declspec(dllimport) NTSTATUS NtDeleteKey(HANDLE);
__declspec(dllimport) NTSTATUS NtFlushKey(HANDLE);
__declspec(dllimport) NTSTATUS NtQueryValueKey(HANDLE, PUNICODE_STRING, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) NTSTATUS NtSetValueKey(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
__declspec(dllimport) NTSTATUS NtDeleteValueKey(HANDLE, PUNICODE_STRING);
__declspec(dllimport) NTSTATUS NtEnumerateKey(HANDLE, ULONG, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) NTSTATUS NtEnumerateValueKey(HANDLE, ULONG, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) NTSTATUS NtQueryKey(HANDLE, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) NTSTATUS NtClose(HANDLE);
__declspec(dllimport) ULONG RtlNtStatusToDosError(NTSTATUS);

HANDLE WINAPI GetProcessHeap(void);
PVOID WINAPI HeapAlloc(HANDLE, DWORD, SIZE_T);
BOOL WINAPI HeapFree(HANDLE, DWORD, PVOID);
int WINAPI MultiByteToWideChar(unsigned int, DWORD, const char *, int, RWCHAR *, int);
int WINAPI WideCharToMultiByte(unsigned int, DWORD, const RWCHAR *, int, char *, int, const char *, BOOL *);
DWORD WINAPI ExpandEnvironmentStringsW(const RWCHAR *, RWCHAR *, DWORD);

static void *radv_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }

static void radv_free(void *p)
{
    if (p)
        HeapFree(GetProcessHeap(), 0, p);
}

static size_t radv_wlen(const RWCHAR *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

static LONG radv_err(NTSTATUS s) { return NT_SUCCESS(s) ? 0 : (LONG)RtlNtStatusToDosError(s); }

/* ---- Names and handles --------------------------------------------------------------------------------------------------------- */

/* The object attributes for a key name: absolute below a predefined key's path, or relative to an open key. *mem is what to free
 * afterwards. Returns a Win32 error. */
static LONG radv_key_oa(HKEY hkey, const RWCHAR *sub, ULONG attrs, OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us, RWCHAR **mem)
{
    uint32_t id = adv_predef_id((uint64_t)(uintptr_t)hkey);
    size_t sublen = sub ? radv_wlen(sub) : 0, rootlen = 0, i, k;
    const char *root = NULL;
    RWCHAR *name;
    LONG e = (LONG)adv_check_subkey(sub);

    *mem = NULL;
    if (e)
        return e;
    if (id) {
        root = adv_predef_path(id);
        if (!root)
            return (LONG)ADV_R_INVALID_HANDLE;
        rootlen = strlen(root);
    } else if (!hkey || hkey == (HKEY)(intptr_t)-1) {
        return (LONG)ADV_R_INVALID_HANDLE;
    }
    if ((rootlen + sublen + 1) * 2 > 0xFFFEu)
        return (LONG)ADV_R_INVALID_PARAMETER;
    name = (RWCHAR *)radv_alloc((rootlen + sublen + 2) * 2);
    if (!name)
        return R_ERR_NOMEM;
    for (i = 0; i < rootlen; ++i)
        name[i] = (RWCHAR)(unsigned char)root[i];
    if (rootlen && sublen)
        name[i++] = '\\';
    for (k = 0; k < sublen; ++k)
        name[i++] = sub[k];
    name[i] = 0;
    us->Buffer = (PWSTR)name;
    us->Length = (USHORT)(i * 2);
    us->MaximumLength = (USHORT)(i * 2 + 2);
    oa->Length = sizeof *oa;
    oa->RootDirectory = id ? NULL : hkey;
    oa->ObjectName = us;
    oa->Attributes = attrs;
    oa->SecurityDescriptor = NULL;
    oa->SecurityQualityOfService = NULL;
    *mem = name;
    return 0;
}

static ACCESS_MASK radv_access(DWORD sam)
{
    sam &= ~R_WOW_MASK;
    return sam ? sam : R_MAXIMUM_ALLOWED;
}

static void radv_value_name(const RWCHAR *name, UNICODE_STRING *us)
{
    static const RWCHAR empty[1] = {0};
    size_t n = name ? radv_wlen(name) : 0;
    us->Buffer = (PWSTR)(name ? name : empty);
    us->Length = (USHORT)(n > 0x7FFE ? 0xFFFC : n * 2);
    us->MaximumLength = (USHORT)(us->Length + 2);
}

/* The NT handle to work on: an open key is used as is; a predefined key is opened on its path for the call. */
static LONG radv_resolve(HKEY hkey, DWORD access, HANDLE *h, int *opened)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    RWCHAR *mem;
    LONG e;
    *opened = 0;
    if (!adv_predef_id((uint64_t)(uintptr_t)hkey)) {
        if (!hkey || hkey == (HKEY)(intptr_t)-1)
            return (LONG)ADV_R_INVALID_HANDLE;
        *h = hkey;
        return 0;
    }
    e = radv_key_oa(hkey, NULL, R_OBJ_CASE_INSENSITIVE, &oa, &us, &mem);
    if (e)
        return e;
    e = radv_err(NtOpenKey(h, radv_access(access), &oa));
    radv_free(mem);
    *opened = e == 0;
    return e;
}

static void radv_done(HANDLE h, int opened)
{
    if (opened)
        NtClose(h);
}

/* ---- Open, create, close --------------------------------------------------------------------------------------------------------- */

LSTATUS WINAPI RegCloseKey(HKEY hkey)
{
    if (adv_predef_id((uint64_t)(uintptr_t)hkey))
        return 0;
    return radv_err(NtClose(hkey));
}

LSTATUS WINAPI RegOpenKeyExW(HKEY hkey, const RWCHAR *sub, DWORD options, DWORD sam, HKEY *out)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    RWCHAR *mem;
    HANDLE h = NULL;
    LONG e;
    if (!out)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    e = radv_key_oa(hkey, sub, R_OBJ_CASE_INSENSITIVE | ((options & R_OPT_OPEN_LINK) ? R_OBJ_OPENLINK : 0), &oa, &us, &mem);
    if (e)
        return e;
    e = radv_err(NtOpenKey(&h, radv_access(sam), &oa));
    radv_free(mem);
    if (e == 0)
        *out = h;
    return e;
}

LSTATUS WINAPI RegCreateKeyExW(HKEY hkey, const RWCHAR *sub, DWORD reserved, RWCHAR *cls, DWORD options, DWORD sam, void *sa, HKEY *out,
                               DWORD *disposition)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us, class_us;
    RWCHAR *mem;
    HANDLE h = NULL;
    ULONG disp = 0;
    LONG e;
    (void)reserved;
    (void)sa;
    if (!out)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    e = radv_key_oa(hkey, sub, R_OBJ_CASE_INSENSITIVE | ((options & R_OPT_OPEN_LINK) ? R_OBJ_OPENLINK : 0), &oa, &us, &mem);
    if (e)
        return e;
    if (cls && *cls)
        radv_value_name(cls, &class_us);
    e = radv_err(NtCreateKey(&h, radv_access(sam), &oa, 0, (cls && *cls) ? &class_us : NULL, (ULONG)(options & 0xF), &disp));
    radv_free(mem);
    if (e == 0) {
        *out = h;
        if (disposition)
            *disposition = disp;
    }
    return e;
}

/* ---- Values ---------------------------------------------------------------------------------------------------------------------- */

/* Reads a value into a heap block. The caller frees *data. */
static LONG radv_fetch(HANDLE h, const UNICODE_STRING *name, ULONG *type, uint8_t **data, ULONG *len)
{
    uint8_t stack[512];
    uint8_t *buf = stack;
    ULONG size = sizeof stack, need = 0;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    int tries;
    for (tries = 0; tries < 4; ++tries) {
        st = NtQueryValueKey(h, (PUNICODE_STRING)name, R_VALUE_PARTIAL, buf, size, &need);
        if (st != STATUS_BUFFER_OVERFLOW && st != STATUS_BUFFER_TOO_SMALL)
            break;
        if (buf != stack)
            radv_free(buf);
        size = need > size ? need : size * 2;
        buf = (uint8_t *)radv_alloc(size);
        if (!buf)
            return R_ERR_NOMEM;
    }
    if (!NT_SUCCESS(st)) {
        if (buf != stack)
            radv_free(buf);
        return radv_err(st);
    }
    {
        const ULONG *info = (const ULONG *)buf; /* TitleIndex, Type, DataLength, Data */
        uint8_t *copy = (uint8_t *)radv_alloc(info[2]);
        if (!copy) {
            if (buf != stack)
                radv_free(buf);
            return R_ERR_NOMEM;
        }
        memcpy(copy, buf + 12, info[2]);
        *type = info[1];
        *len = info[2];
        *data = copy;
    }
    if (buf != stack)
        radv_free(buf);
    return 0;
}

/* Hands a value to the caller's buffer: the type, the size, and the bytes if they fit. */
static LONG radv_deliver(ULONG type, const uint8_t *src, ULONG len, DWORD *out_type, uint8_t *dst, DWORD *cb)
{
    if (out_type)
        *out_type = type;
    if (cb) {
        if (dst) {
            if (*cb < len) {
                *cb = len;
                return (LONG)ADV_R_MORE_DATA;
            }
            memcpy(dst, src, len);
        }
        *cb = len;
    }
    return 0;
}

/* The value as the ANSI functions give it: text types in the system code page, the rest as stored. Allocated block. */
static LONG radv_to_ansi(ULONG type, const uint8_t *src, ULONG len, uint8_t **out, ULONG *outlen)
{
    if (adv_value_is_text(type)) {
        int wchars = (int)(len / 2), n;
        uint8_t *a;
        n = wchars ? WideCharToMultiByte(0, 0, (const RWCHAR *)src, wchars, NULL, 0, NULL, NULL) : 0;
        a = (uint8_t *)radv_alloc((size_t)n);
        if (!a)
            return R_ERR_NOMEM;
        if (n)
            WideCharToMultiByte(0, 0, (const RWCHAR *)src, wchars, (char *)a, n, NULL, NULL);
        *out = a;
        *outlen = (ULONG)n;
        return 0;
    }
    *out = (uint8_t *)radv_alloc(len);
    if (!*out)
        return R_ERR_NOMEM;
    memcpy(*out, src, len);
    *outlen = len;
    return 0;
}

/* The stored form of a value the ANSI functions are given. */
static LONG radv_from_ansi(ULONG type, const uint8_t *src, ULONG len, uint8_t **out, ULONG *outlen)
{
    if (adv_value_is_text(type) && src) {
        int n = len ? MultiByteToWideChar(0, 0, (const char *)src, (int)len, NULL, 0) : 0;
        RWCHAR *w = (RWCHAR *)radv_alloc((size_t)n * 2);
        if (!w)
            return R_ERR_NOMEM;
        if (n)
            MultiByteToWideChar(0, 0, (const char *)src, (int)len, w, n);
        *out = (uint8_t *)w;
        *outlen = (ULONG)n * 2;
        return 0;
    }
    *out = NULL;
    *outlen = len;
    return 0;
}

static RWCHAR *radv_wide(const char *s)
{
    int n;
    RWCHAR *w;
    if (!s)
        return NULL;
    n = MultiByteToWideChar(0, 0, s, -1, NULL, 0);
    w = (RWCHAR *)radv_alloc((size_t)(n > 0 ? n : 1) * 2);
    if (w) {
        if (n > 0)
            MultiByteToWideChar(0, 0, s, -1, w, n);
        else
            w[0] = 0;
    }
    return w;
}

LSTATUS WINAPI RegQueryValueExW(HKEY hkey, const RWCHAR *name, DWORD *reserved, DWORD *type, uint8_t *data, DWORD *cb)
{
    HANDLE h;
    int opened;
    UNICODE_STRING us;
    ULONG t = 0, len = 0;
    uint8_t *buf = NULL;
    LONG e;
    (void)reserved;
    if (data && !cb)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    e = radv_resolve(hkey, R_KEY_QUERY_VALUE, &h, &opened);
    if (e)
        return e;
    radv_value_name(name, &us);
    e = radv_fetch(h, &us, &t, &buf, &len);
    radv_done(h, opened);
    if (e)
        return e;
    e = radv_deliver(t, buf, len, type, data, cb);
    radv_free(buf);
    return e;
}

LSTATUS WINAPI RegQueryValueExA(HKEY hkey, const char *name, DWORD *reserved, DWORD *type, uint8_t *data, DWORD *cb)
{
    HANDLE h;
    int opened;
    UNICODE_STRING us;
    ULONG t = 0, len = 0, alen = 0;
    uint8_t *buf = NULL, *abuf = NULL;
    RWCHAR *wname;
    LONG e;
    (void)reserved;
    if (data && !cb)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    wname = radv_wide(name);
    if (name && !wname)
        return R_ERR_NOMEM;
    e = radv_resolve(hkey, R_KEY_QUERY_VALUE, &h, &opened);
    if (e) {
        radv_free(wname);
        return e;
    }
    radv_value_name(wname, &us);
    e = radv_fetch(h, &us, &t, &buf, &len);
    radv_done(h, opened);
    radv_free(wname);
    if (e)
        return e;
    e = radv_to_ansi(t, buf, len, &abuf, &alen);
    radv_free(buf);
    if (e)
        return e;
    e = radv_deliver(t, abuf, alen, type, data, cb);
    radv_free(abuf);
    return e;
}

LSTATUS WINAPI RegSetValueExW(HKEY hkey, const RWCHAR *name, DWORD reserved, DWORD type, const uint8_t *data, DWORD cb)
{
    HANDLE h;
    int opened;
    UNICODE_STRING us;
    static uint8_t none[4];
    LONG e;
    (void)reserved;
    if (!data && cb)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    e = radv_resolve(hkey, R_KEY_SET_VALUE, &h, &opened);
    if (e)
        return e;
    radv_value_name(name, &us);
    e = radv_err(NtSetValueKey(h, &us, 0, type, (PVOID)(data ? data : none), cb));
    radv_done(h, opened);
    return e;
}

LSTATUS WINAPI RegSetValueExA(HKEY hkey, const char *name, DWORD reserved, DWORD type, const uint8_t *data, DWORD cb)
{
    RWCHAR *wname = radv_wide(name);
    uint8_t *conv = NULL;
    ULONG clen = cb;
    LONG e;
    if (name && !wname)
        return R_ERR_NOMEM;
    e = radv_from_ansi(type, data, cb, &conv, &clen);
    if (e) {
        radv_free(wname);
        return e;
    }
    e = RegSetValueExW(hkey, wname, reserved, type, conv ? conv : data, clen);
    radv_free(conv);
    radv_free(wname);
    return e;
}

LSTATUS WINAPI RegDeleteValueW(HKEY hkey, const RWCHAR *name)
{
    HANDLE h;
    int opened;
    UNICODE_STRING us;
    LONG e = radv_resolve(hkey, R_KEY_SET_VALUE, &h, &opened);
    if (e)
        return e;
    radv_value_name(name, &us);
    e = radv_err(NtDeleteValueKey(h, &us));
    radv_done(h, opened);
    return e;
}

LSTATUS WINAPI RegDeleteValueA(HKEY hkey, const char *name)
{
    RWCHAR *w = radv_wide(name);
    LONG e;
    if (name && !w)
        return R_ERR_NOMEM;
    e = RegDeleteValueW(hkey, w);
    radv_free(w);
    return e;
}

/* ---- Deleting keys ----------------------------------------------------------------------------------------------------------------- */

LSTATUS WINAPI RegDeleteKeyExW(HKEY hkey, const RWCHAR *sub, DWORD sam, DWORD reserved)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    RWCHAR *mem;
    HANDLE h = NULL;
    LONG e;
    (void)sam;
    (void)reserved;
    if (!sub)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    e = radv_key_oa(hkey, sub, R_OBJ_CASE_INSENSITIVE, &oa, &us, &mem);
    if (e)
        return e;
    e = radv_err(NtOpenKey(&h, R_DELETE, &oa));
    radv_free(mem);
    if (e)
        return e;
    e = radv_err(NtDeleteKey(h));
    NtClose(h);
    return e;
}

LSTATUS WINAPI RegDeleteKeyW(HKEY hkey, const RWCHAR *sub) { return RegDeleteKeyExW(hkey, sub, 0, 0); }

LSTATUS WINAPI RegDeleteKeyExA(HKEY hkey, const char *sub, DWORD sam, DWORD reserved)
{
    RWCHAR *w = radv_wide(sub);
    LONG e;
    if (sub && !w)
        return R_ERR_NOMEM;
    e = RegDeleteKeyExW(hkey, w, sam, reserved);
    radv_free(w);
    return e;
}

LSTATUS WINAPI RegDeleteKeyA(HKEY hkey, const char *sub) { return RegDeleteKeyExA(hkey, sub, 0, 0); }

LSTATUS WINAPI RegFlushKey(HKEY hkey)
{
    HANDLE h;
    int opened;
    LONG e = radv_resolve(hkey, R_KEY_QUERY_VALUE, &h, &opened);
    if (e)
        return e;
    e = radv_err(NtFlushKey(h));
    radv_done(h, opened);
    return e;
}

/* ---- Enumeration ------------------------------------------------------------------------------------------------------------------- */

static LONG radv_enum(HANDLE h, ULONG index, ULONG cls, int values, uint8_t **buf, ULONG *size, uint8_t *stack, ULONG stack_size)
{
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    ULONG need = 0;
    int tries;
    *buf = stack;
    *size = stack_size;
    for (tries = 0; tries < 4; ++tries) {
        st = values ? NtEnumerateValueKey(h, index, cls, *buf, *size, &need) : NtEnumerateKey(h, index, cls, *buf, *size, &need);
        if (st != STATUS_BUFFER_OVERFLOW && st != STATUS_BUFFER_TOO_SMALL)
            break;
        if (*buf != stack)
            radv_free(*buf);
        *size = need > *size ? need : *size * 2;
        *buf = (uint8_t *)radv_alloc(*size);
        if (!*buf) {
            *buf = stack;
            return R_ERR_NOMEM;
        }
    }
    if (!NT_SUCCESS(st)) {
        if (*buf != stack)
            radv_free(*buf);
        *buf = stack;
        return radv_err(st);
    }
    return 0;
}

LSTATUS WINAPI RegEnumKeyExW(HKEY hkey, DWORD index, RWCHAR *name, DWORD *cch_name, DWORD *reserved, RWCHAR *cls, DWORD *cch_class,
                             void *last_write)
{
    HANDLE h;
    int opened;
    uint8_t stack[1024], *buf;
    ULONG size, name_len, name_off, cls_len = 0, cls_off = 0;
    int want_class = cls != NULL && cch_class != NULL;
    LONG e;
    (void)reserved;
    if (!name || !cch_name)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    e = radv_resolve(hkey, R_KEY_ENUM_SUBKEYS, &h, &opened);
    if (e)
        return e;
    e = radv_enum(h, index, want_class ? R_INFO_NODE : R_INFO_BASIC, 0, &buf, &size, stack, sizeof stack);
    radv_done(h, opened);
    if (e)
        return e;
    if (want_class) { /* LastWriteTime 8, TitleIndex 4, ClassOffset 4, ClassLength 4, NameLength 4, Name */
        const ULONG *f = (const ULONG *)(buf + 8);
        cls_off = f[1];
        cls_len = f[2];
        name_len = f[3];
        name_off = 24;
    } else { /* LastWriteTime 8, TitleIndex 4, NameLength 4, Name */
        const ULONG *f = (const ULONG *)(buf + 8);
        name_len = f[1];
        name_off = 16;
    }
    if (!adv_name_fits(name_len / 2, *cch_name)) {
        *cch_name = name_len / 2;
        e = (LONG)ADV_R_MORE_DATA;
    } else if (want_class && !adv_name_fits(cls_len / 2, *cch_class)) {
        *cch_class = cls_len / 2;
        e = (LONG)ADV_R_MORE_DATA;
    } else {
        memcpy(name, buf + name_off, name_len);
        name[name_len / 2] = 0;
        *cch_name = name_len / 2;
        if (want_class) {
            memcpy(cls, buf + cls_off, cls_len);
            cls[cls_len / 2] = 0;
            *cch_class = cls_len / 2;
        }
        if (last_write)
            memcpy(last_write, buf, 8);
        e = 0;
    }
    if (buf != stack)
        radv_free(buf);
    return e;
}

LSTATUS WINAPI RegEnumKeyW(HKEY hkey, DWORD index, RWCHAR *name, DWORD cch) { return RegEnumKeyExW(hkey, index, name, &cch, NULL, NULL, NULL, NULL); }

LSTATUS WINAPI RegEnumKeyExA(HKEY hkey, DWORD index, char *name, DWORD *cch_name, DWORD *reserved, char *cls, DWORD *cch_class, void *last_write)
{
    RWCHAR *wname, *wcls = NULL;
    DWORD wn, wc = 0;
    LONG e;
    if (!name || !cch_name)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    wn = *cch_name;
    wname = (RWCHAR *)radv_alloc((size_t)(wn ? wn : 1) * 2);
    if (cls && cch_class) {
        wc = *cch_class;
        wcls = (RWCHAR *)radv_alloc((size_t)(wc ? wc : 1) * 2);
    }
    if (!wname || (cls && cch_class && !wcls)) {
        radv_free(wname);
        radv_free(wcls);
        return R_ERR_NOMEM;
    }
    e = RegEnumKeyExW(hkey, index, wname, &wn, reserved, wcls, wcls ? &wc : NULL, last_write);
    if (e == 0) {
        int n = WideCharToMultiByte(0, 0, wname, (int)wn + 1, name, (int)*cch_name, NULL, NULL);
        if (!n) {
            e = (LONG)ADV_R_MORE_DATA;
        } else {
            *cch_name = (DWORD)n - 1;
            if (wcls) {
                int m = WideCharToMultiByte(0, 0, wcls, (int)wc + 1, cls, (int)*cch_class, NULL, NULL);
                if (!m)
                    e = (LONG)ADV_R_MORE_DATA;
                else
                    *cch_class = (DWORD)m - 1;
            }
        }
    } else if (e == (LONG)ADV_R_MORE_DATA) {
        *cch_name = wn;
        if (cch_class)
            *cch_class = wc;
    }
    radv_free(wname);
    radv_free(wcls);
    return e;
}

LSTATUS WINAPI RegEnumKeyA(HKEY hkey, DWORD index, char *name, DWORD cch) { return RegEnumKeyExA(hkey, index, name, &cch, NULL, NULL, NULL, NULL); }

LSTATUS WINAPI RegEnumValueW(HKEY hkey, DWORD index, RWCHAR *name, DWORD *cch_name, DWORD *reserved, DWORD *type, uint8_t *data, DWORD *cb)
{
    HANDLE h;
    int opened;
    uint8_t stack[1024], *buf;
    ULONG size, name_len, data_off, data_len, t;
    const ULONG *f;
    LONG e;
    (void)reserved;
    if (!name || !cch_name || (data && !cb))
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    e = radv_resolve(hkey, R_KEY_QUERY_VALUE, &h, &opened);
    if (e)
        return e;
    e = radv_enum(h, index, R_VALUE_FULL, 1, &buf, &size, stack, sizeof stack);
    radv_done(h, opened);
    if (e)
        return e;
    f = (const ULONG *)buf; /* TitleIndex, Type, DataOffset, DataLength, NameLength, Name */
    t = f[1];
    data_off = f[2];
    data_len = f[3];
    name_len = f[4];
    if (!adv_name_fits(name_len / 2, *cch_name)) {
        *cch_name = name_len / 2;
        e = (LONG)ADV_R_MORE_DATA;
    } else {
        memcpy(name, buf + 20, name_len);
        name[name_len / 2] = 0;
        *cch_name = name_len / 2;
        e = radv_deliver(t, buf + data_off, data_len, type, data, cb);
    }
    if (buf != stack)
        radv_free(buf);
    return e;
}

LSTATUS WINAPI RegEnumValueA(HKEY hkey, DWORD index, char *name, DWORD *cch_name, DWORD *reserved, DWORD *type, uint8_t *data, DWORD *cb)
{
    RWCHAR *wname;
    DWORD wn, t = 0, dlen = 0;
    uint8_t *wdata = NULL, *adata = NULL;
    ULONG alen = 0;
    LONG e;
    if (!name || !cch_name || (data && !cb))
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    wn = *cch_name;
    wname = (RWCHAR *)radv_alloc((size_t)(wn ? wn : 1) * 2);
    if (!wname)
        return R_ERR_NOMEM;
    /* fetch the data in the wide form into a block of its own size: first ask for its size */
    dlen = 0;
    e = RegEnumValueW(hkey, index, wname, &wn, reserved, &t, NULL, &dlen);
    if (e == 0) {
        int n = WideCharToMultiByte(0, 0, wname, (int)wn + 1, name, (int)*cch_name, NULL, NULL);
        if (!n) {
            e = (LONG)ADV_R_MORE_DATA;
        } else {
            *cch_name = (DWORD)n - 1;
            if (cb || data) {
                wdata = (uint8_t *)radv_alloc(dlen);
                if (!wdata) {
                    e = R_ERR_NOMEM;
                } else {
                    DWORD again = dlen;
                    DWORD nlen = wn + 1;
                    RWCHAR *scratch = (RWCHAR *)radv_alloc((size_t)(wn + 1) * 2);
                    e = scratch ? RegEnumValueW(hkey, index, scratch, &nlen, NULL, &t, wdata, &again) : R_ERR_NOMEM;
                    radv_free(scratch);
                    if (e == 0)
                        e = radv_to_ansi(t, wdata, again, &adata, &alen);
                    if (e == 0)
                        e = radv_deliver(t, adata, alen, type, data, cb);
                }
            } else if (type) {
                *type = t;
            }
        }
    } else if (e == (LONG)ADV_R_MORE_DATA) {
        *cch_name = wn;
    }
    radv_free(wname);
    radv_free(wdata);
    radv_free(adata);
    return e;
}

/* ---- Key information ------------------------------------------------------------------------------------------------------------------ */

LSTATUS WINAPI RegQueryInfoKeyW(HKEY hkey, RWCHAR *cls, DWORD *cch_class, DWORD *reserved, DWORD *subkeys, DWORD *max_subkey, DWORD *max_class,
                                DWORD *values, DWORD *max_value_name, DWORD *max_value_len, DWORD *sec_desc, void *last_write)
{
    HANDLE h;
    int opened;
    uint8_t stack[512], *buf = stack;
    ULONG size = sizeof stack, need = 0, class_off, class_len;
    const ULONG *f;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    LONG e;
    int tries;
    (void)reserved;
    if (cls && !cch_class)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    e = radv_resolve(hkey, R_KEY_QUERY_VALUE, &h, &opened);
    if (e)
        return e;
    for (tries = 0; tries < 4; ++tries) {
        st = NtQueryKey(h, R_INFO_FULL, buf, size, &need);
        if (st != STATUS_BUFFER_OVERFLOW && st != STATUS_BUFFER_TOO_SMALL)
            break;
        if (buf != stack)
            radv_free(buf);
        size = need > size ? need : size * 2;
        buf = (uint8_t *)radv_alloc(size);
        if (!buf)
            return R_ERR_NOMEM;
    }
    radv_done(h, opened);
    if (!NT_SUCCESS(st)) {
        if (buf != stack)
            radv_free(buf);
        return radv_err(st);
    }
    /* LastWriteTime 8, TitleIndex, ClassOffset, ClassLength, SubKeys, MaxNameLen, MaxClassLen, Values, MaxValueNameLen, MaxValueDataLen */
    f = (const ULONG *)(buf + 8);
    class_off = f[1];
    class_len = f[2];
    e = 0;
    if (cch_class) {
        if (cls) {
            if (!adv_name_fits(class_len / 2, *cch_class)) {
                *cch_class = class_len / 2;
                e = (LONG)ADV_R_MORE_DATA;
            } else {
                memcpy(cls, buf + class_off, class_len);
                cls[class_len / 2] = 0;
                *cch_class = class_len / 2;
            }
        } else {
            *cch_class = class_len / 2;
        }
    }
    if (e == 0) {
        if (subkeys)
            *subkeys = f[3];
        if (max_subkey)
            *max_subkey = f[4] / 2;
        if (max_class)
            *max_class = f[5] / 2;
        if (values)
            *values = f[6];
        if (max_value_name)
            *max_value_name = f[7] / 2;
        if (max_value_len)
            *max_value_len = f[8];
        if (sec_desc)
            *sec_desc = 0;
        if (last_write)
            memcpy(last_write, buf, 8);
    }
    if (buf != stack)
        radv_free(buf);
    return e;
}

LSTATUS WINAPI RegQueryInfoKeyA(HKEY hkey, char *cls, DWORD *cch_class, DWORD *reserved, DWORD *subkeys, DWORD *max_subkey, DWORD *max_class,
                                DWORD *values, DWORD *max_value_name, DWORD *max_value_len, DWORD *sec_desc, void *last_write)
{
    RWCHAR *wcls = NULL;
    DWORD wc = cch_class ? *cch_class : 0;
    LONG e;
    if (cls && !cch_class)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    if (cls) {
        wcls = (RWCHAR *)radv_alloc((size_t)(wc ? wc : 1) * 2);
        if (!wcls)
            return R_ERR_NOMEM;
    }
    e = RegQueryInfoKeyW(hkey, wcls, cch_class ? &wc : NULL, reserved, subkeys, max_subkey, max_class, values, max_value_name, max_value_len, sec_desc,
                         last_write);
    if (e == 0 && cls) {
        int n = WideCharToMultiByte(0, 0, wcls, (int)wc + 1, cls, (int)*cch_class, NULL, NULL);
        if (!n)
            e = (LONG)ADV_R_MORE_DATA;
        else
            *cch_class = (DWORD)n - 1;
    } else if (cch_class) {
        *cch_class = wc;
    }
    radv_free(wcls);
    return e;
}

/* ---- RegGetValue and the simple forms ---------------------------------------------------------------------------------------------- */

/* Reads a value as RegGetValue returns it: restricted by type, strings terminated, expandable strings expanded. The block is the
 * caller's to free. */
static LONG radv_get_value(HKEY hkey, const RWCHAR *sub, const RWCHAR *name, DWORD flags, DWORD *type, uint8_t **out, ULONG *outlen)
{
    HKEY key = hkey, opened_sub = NULL;
    HANDLE h;
    int opened = 0;
    UNICODE_STRING us;
    ULONG t = 0, len = 0, pad;
    uint8_t *buf = NULL;
    LONG e;
    DWORD f = flags;
    if ((f & 0xFFFF) == 0)
        f |= 0xFFFF;
    if (sub && *sub) {
        e = RegOpenKeyExW(hkey, sub, 0, R_KEY_QUERY_VALUE, &opened_sub);
        if (e)
            return e;
        key = opened_sub;
    }
    e = radv_resolve(key, R_KEY_QUERY_VALUE, &h, &opened);
    if (e) {
        if (opened_sub)
            RegCloseKey(opened_sub);
        return e;
    }
    radv_value_name(name, &us);
    e = radv_fetch(h, &us, &t, &buf, &len);
    radv_done(h, opened);
    if (opened_sub)
        RegCloseKey(opened_sub);
    if (e)
        return e;
    if (!adv_rrf_allows(t, f)) {
        radv_free(buf);
        return (LONG)ADV_R_UNSUPPORTED_TYPE;
    }
    if (t == ADV_REG_EXPAND_SZ && !(f & ADV_RRF_NOEXPAND) && (f & ADV_RRF_RT_REG_SZ)) {
        /* expand into a fresh block: the stored text first needs its terminator */
        uint8_t *src = (uint8_t *)radv_alloc((size_t)len + 4);
        DWORD need;
        RWCHAR *dst;
        if (!src) {
            radv_free(buf);
            return R_ERR_NOMEM;
        }
        memcpy(src, buf, len);
        memset(src + len, 0, 4);
        radv_free(buf);
        need = ExpandEnvironmentStringsW((const RWCHAR *)src, NULL, 0);
        dst = (RWCHAR *)radv_alloc((size_t)(need ? need : 1) * 2 + 4);
        if (!dst) {
            radv_free(src);
            return R_ERR_NOMEM;
        }
        if (need)
            ExpandEnvironmentStringsW((const RWCHAR *)src, dst, need);
        else
            dst[0] = 0;
        radv_free(src);
        buf = (uint8_t *)dst;
        len = (ULONG)(radv_wlen(dst) + 1) * 2;
        t = ADV_REG_SZ;
    } else if (adv_is_string_type(t)) {
        pad = (ULONG)adv_terminator_bytes(t, buf, len);
        if (pad) {
            uint8_t *grown = (uint8_t *)radv_alloc((size_t)len + pad);
            if (!grown) {
                radv_free(buf);
                return R_ERR_NOMEM;
            }
            memcpy(grown, buf, len);
            memset(grown + len, 0, pad);
            radv_free(buf);
            buf = grown;
            len += pad;
        }
    }
    *type = t;
    *out = buf;
    *outlen = len;
    return 0;
}

LSTATUS WINAPI RegGetValueW(HKEY hkey, const RWCHAR *sub, const RWCHAR *name, DWORD flags, DWORD *type, uint8_t *data, DWORD *cb)
{
    uint8_t *buf = NULL;
    ULONG len = 0;
    DWORD t = 0, cap = cb ? *cb : 0;
    LONG e;
    if (data && !cb)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    e = radv_get_value(hkey, sub, name, flags, &t, &buf, &len);
    if (e == 0) {
        e = radv_deliver(t, buf, len, type, data, cb);
        radv_free(buf);
    }
    if (e != 0 && (flags & ADV_RRF_ZEROONFAILURE) && data && cap)
        memset(data, 0, cap);
    return e;
}

LSTATUS WINAPI RegGetValueA(HKEY hkey, const char *sub, const char *name, DWORD flags, DWORD *type, uint8_t *data, DWORD *cb)
{
    RWCHAR *wsub = radv_wide(sub), *wname = radv_wide(name);
    uint8_t *buf = NULL, *abuf = NULL;
    ULONG len = 0, alen = 0;
    DWORD t = 0, cap = cb ? *cb : 0;
    LONG e;
    if (data && !cb) {
        radv_free(wsub);
        radv_free(wname);
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    }
    e = radv_get_value(hkey, wsub, wname, flags, &t, &buf, &len);
    radv_free(wsub);
    radv_free(wname);
    if (e == 0) {
        e = radv_to_ansi(t, buf, len, &abuf, &alen);
        radv_free(buf);
        if (e == 0) {
            e = radv_deliver(t, abuf, alen, type, data, cb);
            radv_free(abuf);
        }
    }
    if (e != 0 && (flags & ADV_RRF_ZEROONFAILURE) && data && cap)
        memset(data, 0, cap);
    return e;
}

LSTATUS WINAPI RegSetKeyValueW(HKEY hkey, const RWCHAR *sub, const RWCHAR *name, DWORD type, const uint8_t *data, DWORD cb)
{
    HKEY k = hkey, opened = NULL;
    LONG e;
    if (sub && *sub) {
        e = RegCreateKeyExW(hkey, sub, 0, NULL, 0, 0x2 /* KEY_SET_VALUE */, NULL, &opened, NULL);
        if (e)
            return e;
        k = opened;
    }
    e = RegSetValueExW(k, name, 0, type, data, cb);
    if (opened)
        RegCloseKey(opened);
    return e;
}

LSTATUS WINAPI RegSetKeyValueA(HKEY hkey, const char *sub, const char *name, DWORD type, const uint8_t *data, DWORD cb)
{
    RWCHAR *wsub = radv_wide(sub), *wname = radv_wide(name);
    uint8_t *conv = NULL;
    ULONG clen = cb;
    LONG e = radv_from_ansi(type, data, cb, &conv, &clen);
    if (e == 0)
        e = RegSetKeyValueW(hkey, wsub, wname, type, conv ? conv : data, clen);
    radv_free(conv);
    radv_free(wsub);
    radv_free(wname);
    return e;
}

LSTATUS WINAPI RegDeleteKeyValueW(HKEY hkey, const RWCHAR *sub, const RWCHAR *name)
{
    HKEY k = hkey, opened = NULL;
    LONG e;
    if (sub && *sub) {
        e = RegOpenKeyExW(hkey, sub, 0, R_KEY_SET_VALUE, &opened);
        if (e)
            return e;
        k = opened;
    }
    e = RegDeleteValueW(k, name);
    if (opened)
        RegCloseKey(opened);
    return e;
}

LSTATUS WINAPI RegDeleteKeyValueA(HKEY hkey, const char *sub, const char *name)
{
    RWCHAR *wsub = radv_wide(sub), *wname = radv_wide(name);
    LONG e = RegDeleteKeyValueW(hkey, wsub, wname);
    radv_free(wsub);
    radv_free(wname);
    return e;
}

/* Deletes every value and every subkey below an open key. */
static LONG radv_clear(HANDLE h)
{
    uint8_t stack[1024], *buf;
    ULONG size;
    LONG e;
    for (;;) { /* subkeys: always the first one, as each removal shifts the rest */
        OBJECT_ATTRIBUTES oa;
        UNICODE_STRING us;
        HANDLE child = NULL;
        ULONG name_len;
        RWCHAR *name;
        e = radv_enum(h, 0, R_INFO_BASIC, 0, &buf, &size, stack, sizeof stack);
        if (e == (LONG)ADV_R_NO_MORE_ITEMS)
            break;
        if (e)
            return e;
        name_len = ((const ULONG *)(buf + 8))[1];
        name = (RWCHAR *)radv_alloc(name_len + 2);
        if (!name) {
            if (buf != stack)
                radv_free(buf);
            return R_ERR_NOMEM;
        }
        memcpy(name, buf + 16, name_len);
        name[name_len / 2] = 0;
        if (buf != stack)
            radv_free(buf);
        us.Buffer = (PWSTR)name;
        us.Length = (USHORT)name_len;
        us.MaximumLength = (USHORT)(name_len + 2);
        oa.Length = sizeof oa;
        oa.RootDirectory = h;
        oa.ObjectName = &us;
        oa.Attributes = R_OBJ_CASE_INSENSITIVE;
        oa.SecurityDescriptor = NULL;
        oa.SecurityQualityOfService = NULL;
        e = radv_err(NtOpenKey(&child, R_DELETE | R_KEY_ENUM_SUBKEYS | R_KEY_QUERY_VALUE | R_KEY_SET_VALUE, &oa));
        radv_free(name);
        if (e)
            return e;
        e = radv_clear(child);
        if (e == 0)
            e = radv_err(NtDeleteKey(child));
        NtClose(child);
        if (e)
            return e;
    }
    for (;;) { /* values */
        UNICODE_STRING us;
        RWCHAR *name;
        ULONG name_len;
        e = radv_enum(h, 0, R_VALUE_BASIC, 1, &buf, &size, stack, sizeof stack);
        if (e == (LONG)ADV_R_NO_MORE_ITEMS)
            break;
        if (e)
            return e;
        name_len = ((const ULONG *)buf)[2]; /* TitleIndex, Type, NameLength, Name */
        name = (RWCHAR *)radv_alloc(name_len + 2);
        if (!name) {
            if (buf != stack)
                radv_free(buf);
            return R_ERR_NOMEM;
        }
        memcpy(name, buf + 12, name_len);
        name[name_len / 2] = 0;
        if (buf != stack)
            radv_free(buf);
        us.Buffer = (PWSTR)name;
        us.Length = (USHORT)name_len;
        us.MaximumLength = (USHORT)(name_len + 2);
        e = radv_err(NtDeleteValueKey(h, &us));
        radv_free(name);
        if (e)
            return e;
    }
    return 0;
}

LSTATUS WINAPI RegDeleteTreeW(HKEY hkey, const RWCHAR *sub)
{
    HKEY k = hkey, opened = NULL;
    HANDLE h;
    int ropened;
    LONG e;
    if (sub && *sub) {
        e = RegOpenKeyExW(hkey, sub, 0, R_DELETE | R_KEY_ENUM_SUBKEYS | R_KEY_QUERY_VALUE | R_KEY_SET_VALUE, &opened);
        if (e)
            return e;
        k = opened;
    }
    e = radv_resolve(k, R_DELETE | R_KEY_ENUM_SUBKEYS | R_KEY_QUERY_VALUE | R_KEY_SET_VALUE, &h, &ropened);
    if (e == 0) {
        e = radv_clear(h);
        if (e == 0 && opened)
            e = radv_err(NtDeleteKey(h));
        radv_done(h, ropened);
    }
    if (opened)
        RegCloseKey(opened);
    return e;
}

LSTATUS WINAPI RegDeleteTreeA(HKEY hkey, const char *sub)
{
    RWCHAR *w = radv_wide(sub);
    LONG e = RegDeleteTreeW(hkey, w);
    radv_free(w);
    return e;
}

/* ---- Create and open, ANSI and old forms --------------------------------------------------------------------------------------------- */

LSTATUS WINAPI RegOpenKeyExA(HKEY hkey, const char *sub, DWORD options, DWORD sam, HKEY *out)
{
    RWCHAR *w = radv_wide(sub);
    LONG e;
    if (sub && !w)
        return R_ERR_NOMEM;
    e = RegOpenKeyExW(hkey, w, options, sam, out);
    radv_free(w);
    return e;
}

LSTATUS WINAPI RegCreateKeyExA(HKEY hkey, const char *sub, DWORD reserved, char *cls, DWORD options, DWORD sam, void *sa, HKEY *out, DWORD *disposition)
{
    RWCHAR *w = radv_wide(sub), *wc = radv_wide(cls);
    LONG e;
    if ((sub && !w) || (cls && !wc)) {
        radv_free(w);
        radv_free(wc);
        return R_ERR_NOMEM;
    }
    e = RegCreateKeyExW(hkey, w, reserved, wc, options, sam, sa, out, disposition);
    radv_free(w);
    radv_free(wc);
    return e;
}

LSTATUS WINAPI RegOpenKeyW(HKEY hkey, const RWCHAR *sub, HKEY *out) { return RegOpenKeyExW(hkey, sub, 0, R_MAXIMUM_ALLOWED, out); }
LSTATUS WINAPI RegOpenKeyA(HKEY hkey, const char *sub, HKEY *out) { return RegOpenKeyExA(hkey, sub, 0, R_MAXIMUM_ALLOWED, out); }
LSTATUS WINAPI RegCreateKeyW(HKEY hkey, const RWCHAR *sub, HKEY *out) { return RegCreateKeyExW(hkey, sub, 0, NULL, 0, R_MAXIMUM_ALLOWED, NULL, out, NULL); }
LSTATUS WINAPI RegCreateKeyA(HKEY hkey, const char *sub, HKEY *out) { return RegCreateKeyExA(hkey, sub, 0, NULL, 0, R_MAXIMUM_ALLOWED, NULL, out, NULL); }

/* The default value of a subkey, as a string. */
LSTATUS WINAPI RegQueryValueW(HKEY hkey, const RWCHAR *sub, RWCHAR *data, LONG *cb)
{
    HKEY k = hkey, opened = NULL;
    DWORD t = 0, n = cb ? (DWORD)*cb : 0;
    LONG e;
    if (sub && *sub) {
        e = RegOpenKeyExW(hkey, sub, 0, R_KEY_QUERY_VALUE, &opened);
        if (e)
            return e;
        k = opened;
    }
    e = RegQueryValueExW(k, NULL, NULL, &t, (uint8_t *)data, cb ? &n : NULL);
    if (opened)
        RegCloseKey(opened);
    if (cb)
        *cb = (LONG)n;
    if (e == 0 && t != ADV_REG_SZ)
        e = (LONG)ADV_R_FILE_NOT_FOUND;
    return e;
}

LSTATUS WINAPI RegQueryValueA(HKEY hkey, const char *sub, char *data, LONG *cb)
{
    RWCHAR *w = radv_wide(sub);
    HKEY k = hkey, opened = NULL;
    DWORD t = 0, n = cb ? (DWORD)*cb : 0;
    LONG e = 0;
    if (sub && *sub) {
        e = RegOpenKeyExW(hkey, w, 0, R_KEY_QUERY_VALUE, &opened);
        if (e) {
            radv_free(w);
            return e;
        }
        k = opened;
    }
    e = RegQueryValueExA(k, NULL, NULL, &t, (uint8_t *)data, cb ? &n : NULL);
    if (opened)
        RegCloseKey(opened);
    radv_free(w);
    if (cb)
        *cb = (LONG)n;
    if (e == 0 && t != ADV_REG_SZ)
        e = (LONG)ADV_R_FILE_NOT_FOUND;
    return e;
}

/* Sets the default value of a subkey (created if needed) to a string. */
LSTATUS WINAPI RegSetValueW(HKEY hkey, const RWCHAR *sub, DWORD type, const RWCHAR *data, DWORD cb)
{
    HKEY k = hkey, opened = NULL;
    LONG e;
    (void)cb;
    if (type != ADV_REG_SZ || !data)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    if (sub && *sub) {
        e = RegCreateKeyExW(hkey, sub, 0, NULL, 0, R_KEY_SET_VALUE, NULL, &opened, NULL);
        if (e)
            return e;
        k = opened;
    }
    e = RegSetValueExW(k, NULL, 0, ADV_REG_SZ, (const uint8_t *)data, (DWORD)(radv_wlen(data) + 1) * 2);
    if (opened)
        RegCloseKey(opened);
    return e;
}

LSTATUS WINAPI RegSetValueA(HKEY hkey, const char *sub, DWORD type, const char *data, DWORD cb)
{
    RWCHAR *wsub = radv_wide(sub), *wdata = radv_wide(data);
    LONG e = RegSetValueW(hkey, wsub, type, wdata, cb);
    radv_free(wsub);
    radv_free(wdata);
    return e;
}

/* ---- Roots and the functions without a counterpart here ------------------------------------------------------------------------ */

static LONG radv_open_path(const char *path, DWORD sam, HKEY *out)
{
    RWCHAR w[160];
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    HANDLE h = NULL;
    size_t i;
    LONG e;
    if (!out)
        return (LONG)ADV_R_INVALID_PARAMETER;
    for (i = 0; path[i] && i < 159; ++i)
        w[i] = (RWCHAR)(unsigned char)path[i];
    w[i] = 0;
    us.Buffer = (PWSTR)w;
    us.Length = (USHORT)(i * 2);
    us.MaximumLength = (USHORT)(i * 2 + 2);
    oa.Length = sizeof oa;
    oa.RootDirectory = NULL;
    oa.ObjectName = &us;
    oa.Attributes = R_OBJ_CASE_INSENSITIVE;
    oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;
    e = radv_err(NtOpenKey(&h, radv_access(sam), &oa));
    if (e == 0)
        *out = h;
    return e;
}

LSTATUS WINAPI RegOpenCurrentUser(DWORD sam, HKEY *out) { return radv_open_path(adv_predef_path(ADV_ID_CURRENT_USER), sam, out); }

LSTATUS WINAPI RegOpenUserClassesRoot(HANDLE token, DWORD options, DWORD sam, HKEY *out)
{
    (void)token;
    (void)options;
    return radv_open_path(adv_predef_path(ADV_ID_CLASSES_ROOT), sam, out);
}

/* Only the local registry is reachable. */
LSTATUS WINAPI RegConnectRegistryW(const RWCHAR *machine, HKEY hkey, HKEY *out)
{
    if (!out)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    if (machine && *machine)
        return R_ERR_BAD_NETPATH;
    if (!adv_predef_id((uint64_t)(uintptr_t)hkey))
        return (LSTATUS)ADV_R_INVALID_HANDLE;
    *out = hkey;
    return 0;
}

LSTATUS WINAPI RegConnectRegistryA(const char *machine, HKEY hkey, HKEY *out)
{
    if (!out)
        return (LSTATUS)ADV_R_INVALID_PARAMETER;
    if (machine && *machine)
        return R_ERR_BAD_NETPATH;
    if (!adv_predef_id((uint64_t)(uintptr_t)hkey))
        return (LSTATUS)ADV_R_INVALID_HANDLE;
    *out = hkey;
    return 0;
}

/* There is no key notification: an asynchronous request is accepted and never signals; a synchronous one cannot be honoured. */
LSTATUS WINAPI RegNotifyChangeKeyValue(HKEY hkey, BOOL subtree, DWORD filter, HANDLE event, BOOL async)
{
    (void)hkey;
    (void)subtree;
    (void)filter;
    (void)event;
    return async ? 0 : R_ERR_NOT_IMPL;
}

LSTATUS WINAPI RegDisablePredefinedCache(void) { return 0; }
LSTATUS WINAPI RegOverridePredefKey(HKEY hkey, HKEY newkey) { (void)hkey; (void)newkey; return R_ERR_NOT_IMPL; }
LSTATUS WINAPI RegLoadKeyW(HKEY hkey, const RWCHAR *sub, const RWCHAR *file) { (void)hkey; (void)sub; (void)file; return R_ERR_NOT_IMPL; }
LSTATUS WINAPI RegLoadKeyA(HKEY hkey, const char *sub, const char *file) { (void)hkey; (void)sub; (void)file; return R_ERR_NOT_IMPL; }
LSTATUS WINAPI RegUnLoadKeyW(HKEY hkey, const RWCHAR *sub) { (void)hkey; (void)sub; return R_ERR_NOT_IMPL; }
LSTATUS WINAPI RegUnLoadKeyA(HKEY hkey, const char *sub) { (void)hkey; (void)sub; return R_ERR_NOT_IMPL; }
LSTATUS WINAPI RegSaveKeyW(HKEY hkey, const RWCHAR *file, void *sa) { (void)hkey; (void)file; (void)sa; return R_ERR_NOT_IMPL; }
LSTATUS WINAPI RegSaveKeyA(HKEY hkey, const char *file, void *sa) { (void)hkey; (void)file; (void)sa; return R_ERR_NOT_IMPL; }
LSTATUS WINAPI RegRestoreKeyW(HKEY hkey, const RWCHAR *file, DWORD flags) { (void)hkey; (void)file; (void)flags; return R_ERR_NOT_IMPL; }
LSTATUS WINAPI RegRestoreKeyA(HKEY hkey, const char *file, DWORD flags) { (void)hkey; (void)file; (void)flags; return R_ERR_NOT_IMPL; }
LSTATUS WINAPI RegGetKeySecurity(HKEY hkey, DWORD info, void *sd, DWORD *len) { (void)hkey; (void)info; (void)sd; (void)len; return R_ERR_NOT_IMPL; }
LSTATUS WINAPI RegSetKeySecurity(HKEY hkey, DWORD info, void *sd) { (void)hkey; (void)info; (void)sd; return R_ERR_NOT_IMPL; }

#endif /* __MINGW32__ || ADV_HOST_TEST */
