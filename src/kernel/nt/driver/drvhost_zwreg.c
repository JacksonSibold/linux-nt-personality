/* drvhost_zwreg.c - the registry services drivers call: the Zw key and value functions, and the Rtl registry helpers
 * built on them (RtlQueryRegistryValues and friends).
 *
 * The Zw functions are thin: they translate the driver's handle to a key of the personality registry through
 * drvhost_regbridge. The Rtl helpers are written here from their documented behaviour, with the edge cases checked
 * against ReactOS (see docs/policy/THIRD-PARTY.md); no ReactOS source is copied.
 *
 * Environment expansion: a REG_EXPAND_SZ value is expanded against the Environment block the caller passes. A kernel
 * caller usually passes none, and then a variable name is left as written, as it is for any name the block lacks.
 */

#include "drvhost_int.h"
#include "drvhost_regbridge.h"

/* -------------------------------------------------------------------------- */
/* Handles and the Zw functions                                               */
/* -------------------------------------------------------------------------- */
static dhreg_key_t *key_of(HANDLE h)
{
    return dh_handle_lookup(h, DH_KIND_KEY);
}

/* Called by ZwClose for key handles. */
void dh_reg_key_closed(void *key)
{
    dhreg_close(key);
}

static NTSTATUS open_key(PHANDLE KeyHandle, ACCESS_MASK Access, POBJECT_ATTRIBUTES oa, int create,
                         PULONG Disposition)
{
    if (!KeyHandle || !oa || !oa->ObjectName || !oa->ObjectName->Buffer) return STATUS_INVALID_PARAMETER;
    dhreg_key_t *root = NULL;
    if (oa->RootDirectory) {
        root = key_of(oa->RootDirectory);
        if (!root) return STATUS_INVALID_HANDLE;
    }
    unsigned long disp = 0;
    dhreg_key_t *k = NULL;
    NTSTATUS st = dhreg_open(root, (const unsigned short *)oa->ObjectName->Buffer, oa->ObjectName->Length, Access,
                             create, &disp, &k);
    if (!NT_SUCCESS(st)) return st;
    HANDLE h = dh_handle_insert(DH_KIND_KEY, k);
    if (!h) { dhreg_close(k); return STATUS_INSUFFICIENT_RESOURCES; }
    *KeyHandle = h;
    if (Disposition) *Disposition = (ULONG)disp;
    return STATUS_SUCCESS;
}

DRVCALL NTSTATUS ZwOpenKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes)
{
    return open_key(KeyHandle, DesiredAccess, ObjectAttributes, 0, NULL);
}

DRVCALL NTSTATUS ZwCreateKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                             ULONG TitleIndex, PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition)
{
    (void)TitleIndex; (void)Class; (void)CreateOptions;
    return open_key(KeyHandle, DesiredAccess, ObjectAttributes, 1, Disposition);
}

DRVCALL NTSTATUS ZwQueryValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG InfoClass, PVOID Info, ULONG Length,
                                 PULONG ResultLength)
{
    dhreg_key_t *k = key_of(KeyHandle);
    if (!k) return STATUS_INVALID_HANDLE;
    if (!ValueName || !ResultLength) return STATUS_INVALID_PARAMETER;
    unsigned long rl = 0;
    NTSTATUS st = dhreg_query_value(k, (const unsigned short *)ValueName->Buffer, ValueName->Length, InfoClass, Info,
                                    Length, &rl);
    *ResultLength = (ULONG)rl;
    return st;
}

DRVCALL NTSTATUS ZwSetValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG TitleIndex, ULONG Type, PVOID Data,
                               ULONG DataSize)
{
    (void)TitleIndex;
    dhreg_key_t *k = key_of(KeyHandle);
    if (!k) return STATUS_INVALID_HANDLE;
    if (!ValueName) return STATUS_INVALID_PARAMETER;
    return dhreg_set_value(k, (const unsigned short *)ValueName->Buffer, ValueName->Length, Type, Data, DataSize);
}

DRVCALL NTSTATUS ZwDeleteValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName)
{
    dhreg_key_t *k = key_of(KeyHandle);
    if (!k) return STATUS_INVALID_HANDLE;
    if (!ValueName) return STATUS_INVALID_PARAMETER;
    return dhreg_delete_value(k, (const unsigned short *)ValueName->Buffer, ValueName->Length);
}

DRVCALL NTSTATUS ZwEnumerateValueKey(HANDLE KeyHandle, ULONG Index, ULONG InfoClass, PVOID Info, ULONG Length,
                                     PULONG ResultLength)
{
    dhreg_key_t *k = key_of(KeyHandle);
    if (!k) return STATUS_INVALID_HANDLE;
    if (!ResultLength) return STATUS_INVALID_PARAMETER;
    unsigned long rl = 0;
    NTSTATUS st = dhreg_enum_value(k, Index, InfoClass, Info, Length, &rl);
    *ResultLength = (ULONG)rl;
    return st;
}

DRVCALL NTSTATUS ZwEnumerateKey(HANDLE KeyHandle, ULONG Index, ULONG InfoClass, PVOID Info, ULONG Length,
                                PULONG ResultLength)
{
    dhreg_key_t *k = key_of(KeyHandle);
    if (!k) return STATUS_INVALID_HANDLE;
    if (!ResultLength) return STATUS_INVALID_PARAMETER;
    unsigned long rl = 0;
    NTSTATUS st = dhreg_enum_key(k, Index, InfoClass, Info, Length, &rl);
    *ResultLength = (ULONG)rl;
    return st;
}

DRVCALL NTSTATUS ZwQueryKey(HANDLE KeyHandle, ULONG InfoClass, PVOID Info, ULONG Length, PULONG ResultLength)
{
    dhreg_key_t *k = key_of(KeyHandle);
    if (!k) return STATUS_INVALID_HANDLE;
    if (!ResultLength) return STATUS_INVALID_PARAMETER;
    unsigned long rl = 0;
    NTSTATUS st = dhreg_query_key(k, InfoClass, Info, Length, &rl);
    *ResultLength = (ULONG)rl;
    return st;
}

DRVCALL NTSTATUS ZwDeleteKey(HANDLE KeyHandle)
{
    dhreg_key_t *k = key_of(KeyHandle);
    return k ? dhreg_delete_key(k) : STATUS_INVALID_HANDLE;
}

DRVCALL NTSTATUS ZwFlushKey(HANDLE KeyHandle)
{
    dhreg_key_t *k = key_of(KeyHandle);
    return k ? dhreg_flush(k) : STATUS_INVALID_HANDLE;
}

/* -------------------------------------------------------------------------- */
/* Rtl registry helpers                                                       */
/* -------------------------------------------------------------------------- */
#define W(s) ((const WCHAR *)u##s)
static const WCHAR *const k_rel_base[RTL_REGISTRY_MAXIMUM] = {
    W(""),
    W("\\Registry\\Machine\\System\\CurrentControlSet\\Services"),
    W("\\Registry\\Machine\\System\\CurrentControlSet\\Control"),
    W("\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion"),
    W("\\Registry\\Machine\\Hardware\\DeviceMap"),
    W("\\Registry\\User\\.Default"),
};
#undef W


/* Opens the key that (RelativeTo, Path) names, or with RTL_REGISTRY_HANDLE takes Path as an open key handle. *own says
 * whether the caller has to close it. With create the key is created and opened for writing. */
static NTSTATUS rtl_key(ULONG RelativeTo, PCWSTR Path, int create, HANDLE *out, int *own)
{
    if (RelativeTo & RTL_REGISTRY_HANDLE) {
        *out = (HANDLE)Path;
        *own = 0;
        return STATUS_SUCCESS;
    }
    RelativeTo &= ~(ULONG)RTL_REGISTRY_OPTIONAL;
    if (RelativeTo >= RTL_REGISTRY_MAXIMUM) return STATUS_INVALID_PARAMETER;

    WCHAR full[512];
    size_t o = 0;
    const WCHAR *base = k_rel_base[RelativeTo];
    size_t bl = drv_wcslen(base);
    if (bl >= sizeof full / sizeof full[0]) return STATUS_NAME_TOO_LONG;
    memcpy(full, base, bl * sizeof(WCHAR));
    o = bl;
    if (Path) {
        if (RelativeTo != RTL_REGISTRY_ABSOLUTE) {
            if (o + 1 >= sizeof full / sizeof full[0]) return STATUS_NAME_TOO_LONG;
            full[o++] = '\\';
            if (*Path == '\\') Path++;                  /* a leading backslash on a relative path is not a second one */
        }
        size_t pl = drv_wcslen(Path);
        if (o + pl + 1 > sizeof full / sizeof full[0]) return STATUS_NAME_TOO_LONG;
        memcpy(full + o, Path, pl * sizeof(WCHAR));
        o += pl;
    }
    full[o] = 0;

    UNICODE_STRING name = { (USHORT)(o * sizeof(WCHAR)), (USHORT)((o + 1) * sizeof(WCHAR)), full };
    OBJECT_ATTRIBUTES oa = { sizeof oa, NULL, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL };
    *own = 1;
    return create ? ZwCreateKey(out, GENERIC_WRITE, &oa, 0, NULL, 0, NULL)
                  : ZwOpenKey(out, MAXIMUM_ALLOWED | GENERIC_READ, &oa);
}

static void rtl_key_release(HANDLE h, int own)
{
    if (own) ZwClose(h);
}

DRVCALL NTSTATUS RtlCheckRegistryKey(ULONG RelativeTo, PWSTR Path)
{
    HANDLE h; int own;
    NTSTATUS st = rtl_key(RelativeTo, Path, 0, &h, &own);
    if (!NT_SUCCESS(st)) return st;
    ZwClose(h);                                     /* closed even for RTL_REGISTRY_HANDLE, as Windows does */
    return STATUS_SUCCESS;
}

DRVCALL NTSTATUS RtlCreateRegistryKey(ULONG RelativeTo, PWSTR Path)
{
    HANDLE h; int own;
    NTSTATUS st = rtl_key(RelativeTo, Path, 1, &h, &own);
    if (!NT_SUCCESS(st)) return st;
    rtl_key_release(h, own);
    return STATUS_SUCCESS;
}

DRVCALL NTSTATUS RtlWriteRegistryValue(ULONG RelativeTo, PCWSTR Path, PCWSTR ValueName, ULONG ValueType,
                                       PVOID ValueData, ULONG ValueLength)
{
    HANDLE h; int own;
    NTSTATUS st = rtl_key(RelativeTo, Path, 1, &h, &own);
    if (!NT_SUCCESS(st)) return st;
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, ValueName);
    st = ZwSetValueKey(h, &n, 0, ValueType, ValueData, ValueLength);
    rtl_key_release(h, own);
    return st;
}

DRVCALL NTSTATUS RtlDeleteRegistryValue(ULONG RelativeTo, PCWSTR Path, PCWSTR ValueName)
{
    HANDLE h; int own;
    NTSTATUS st = rtl_key(RelativeTo, Path, 1, &h, &own);
    if (!NT_SUCCESS(st)) return st;
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, ValueName);
    st = ZwDeleteValueKey(h, &n);
    rtl_key_release(h, own);
    return st;
}

/* ---- RtlQueryRegistryValues ---------------------------------------------- */

/* Expands %NAME% against a NAME=VALUE block (wide, NUL separated, double-NUL terminated), or none. A name the block lacks
 * is kept with its percent signs. Writes at most cap characters and always returns the length needed, so a caller can
 * size a buffer. */
static size_t expand_env(const WCHAR *src, size_t n, const WCHAR *env, WCHAR *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        if (src[i] == '%') {
            while (j < n && src[j] != '%') j++;
            if (j < n && j > i + 1) {                    /* %name% */
                const WCHAR *val = NULL; size_t vl = 0;
                for (const WCHAR *e = env; e && *e; e += drv_wcslen(e) + 1) {
                    size_t k = 0;
                    while (k < j - i - 1 && e[k] && dh_upcase(e[k]) == dh_upcase(src[i + 1 + k])) k++;
                    if (k == j - i - 1 && e[k] == '=') { val = e + k + 1; vl = drv_wcslen(val); break; }
                }
                if (val) {
                    if (o + vl <= cap && out) memcpy(out + o, val, vl * sizeof(WCHAR));
                    o += vl;
                    i = j + 1;
                    continue;
                }
                if (o + (j - i + 1) <= cap && out) memcpy(out + o, src + i, (j - i + 1) * sizeof(WCHAR));
                o += j - i + 1;
                i = j + 1;
                continue;
            }
        }
        if (o + 1 <= cap && out) out[o] = src[i];
        o++;
        i++;
    }
    return o;
}

/* The DIRECT flag: where a value goes without a callback. EntryContext is a UNICODE_STRING for the string types and a
 * buffer for everything else. */
static NTSTATUS store_direct(ULONG type, const void *data, ULONG len, PVOID ctx)
{
    if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ || type == REG_LINK) {
        PUNICODE_STRING us = ctx;
        USHORT n = len > 0xffff ? 0xffff : (USHORT)len;
        if (!us->Buffer) {
            us->Buffer = ExAllocatePoolWithTag(PagedPool, n ? n : 2, DH_RTL_TAG_UNICODE);
            if (!us->Buffer) return STATUS_NO_MEMORY;
            us->MaximumLength = n;
        } else if (n > us->MaximumLength) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        memcpy(us->Buffer, data, n);
        us->Length = n >= sizeof(WCHAR) ? (USHORT)(n - sizeof(WCHAR)) : n;
        return STATUS_SUCCESS;
    }
    if (len <= sizeof(ULONG)) {
        if (len && ctx != data) memcpy(ctx, data, len);
        return STATUS_SUCCESS;
    }
    LONG *first = ctx;
    if (*first < 0) {                                    /* the caller gives the size as a negative count: raw data */
        if ((ULONG)(-*first) < len) return STATUS_BUFFER_TOO_SMALL;
        memcpy(ctx, data, len);
    } else if (type == REG_BINARY) {
        memcpy(ctx, data, len);
    } else {                                              /* size, type, then the data */
        if ((ULONG)*first < 2 * sizeof(ULONG) + len) return STATUS_BUFFER_TOO_SMALL;
        ULONG *w = ctx;
        w[0] = len;
        w[1] = type;
        memcpy(w + 2, data, len);
    }
    return STATUS_SUCCESS;
}

/* Delivers one value, or the entry's default when `info` is NULL, to the entry's routine or EntryContext. */
static NTSTATUS deliver(PRTL_QUERY_REGISTRY_TABLE q, const KEY_VALUE_FULL_INFORMATION *info, PVOID Context,
                        PVOID Environment)
{
    ULONG type, len;
    const void *data;
    WCHAR *namebuf = NULL;
    PCWSTR name = q->Name;
    NTSTATUS st;

    if (!info || info->Type == REG_NONE || (info->DataLength == 0 && info->Type == q->DefaultType)) {
        if (q->DefaultType == REG_NONE)
            return (q->Flags & RTL_QUERY_REGISTRY_REQUIRED) ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_SUCCESS;
        type = q->DefaultType;
        data = q->DefaultData;
        len = q->DefaultLength;
        if (!data) return STATUS_DATA_OVERRUN;
        if (!len) {
            const WCHAR *w = data;
            if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_LINK) {
                len = (ULONG)((drv_wcslen(w) + 1) * sizeof(WCHAR));
            } else if (type == REG_MULTI_SZ) {
                while (*w) w += drv_wcslen(w) + 1;
                len = (ULONG)((w - (const WCHAR *)data + 1) * sizeof(WCHAR));
            }
        }
    } else {
        type = info->Type;
        data = (const char *)info + info->DataOffset;
        len = info->DataLength;
        if (!(q->Flags & RTL_QUERY_REGISTRY_DIRECT)) {   /* the routine gets the value's own name, terminated */
            namebuf = dh_alloc(info->NameLength + sizeof(WCHAR));
            if (!namebuf) return STATUS_NO_MEMORY;
            memcpy(namebuf, info->Name, info->NameLength);
            name = namebuf;
        }
    }

    WCHAR *expanded = NULL;
    st = STATUS_SUCCESS;
    if (!(q->Flags & RTL_QUERY_REGISTRY_NOEXPAND)) {
        if (type == REG_MULTI_SZ) {
            /* One call, or one UNICODE_STRING, for each string of the list. */
            const WCHAR *p = data, *end = (const WCHAR *)((const char *)data + len);
            while (p < end && *p) {
                size_t sl = drv_wcslen(p) + 1;
                if (q->Flags & RTL_QUERY_REGISTRY_DIRECT) {
                    st = store_direct(REG_SZ, p, (ULONG)(sl * sizeof(WCHAR)), q->EntryContext);
                    q->EntryContext = (char *)q->EntryContext + sizeof(UNICODE_STRING);
                } else {
                    st = q->QueryRoutine(name, REG_SZ, (PVOID)p, (ULONG)(sl * sizeof(WCHAR)), Context, q->EntryContext);
                }
                if (st == STATUS_BUFFER_TOO_SMALL) st = STATUS_SUCCESS;
                if (!NT_SUCCESS(st)) break;
                p += sl;
            }
            dh_free(namebuf);
            return st;
        }
        if (type == REG_EXPAND_SZ && len >= sizeof(WCHAR)) {
            size_t chars = len / sizeof(WCHAR);
            if (((const WCHAR *)data)[chars - 1] == 0) chars--;
            size_t need = expand_env(data, chars, Environment, NULL, 0);
            expanded = dh_alloc((need + 1) * sizeof(WCHAR));
            if (!expanded) { dh_free(namebuf); return STATUS_NO_MEMORY; }
            expand_env(data, chars, Environment, expanded, need);
            expanded[need] = 0;
            data = expanded;
            len = (ULONG)((need + 1) * sizeof(WCHAR));
            type = REG_SZ;
        }
    }

    if (q->Flags & RTL_QUERY_REGISTRY_DIRECT)
        st = store_direct(type, data, len, q->EntryContext);
    else
        st = q->QueryRoutine(name, type, (PVOID)data, len, Context, q->EntryContext);
    dh_free(expanded);
    dh_free(namebuf);
    return st == STATUS_BUFFER_TOO_SMALL ? STATUS_SUCCESS : st;
}

/* Reads one value into a buffer that grows to fit. The caller frees *out with dh_free. */
static NTSTATUS fetch(HANDLE key, PUNICODE_STRING name, ULONG index, KEY_VALUE_FULL_INFORMATION **out)
{
    ULONG cap = 512, rl = 0;
    for (int tries = 0; tries < 6; tries++) {
        KEY_VALUE_FULL_INFORMATION *b = dh_alloc(cap + sizeof(WCHAR) * 2);
        if (!b) return STATUS_NO_MEMORY;
        NTSTATUS st = name ? ZwQueryValueKey(key, name, KeyValueFullInformation, b, cap, &rl)
                           : ZwEnumerateValueKey(key, index, KeyValueFullInformation, b, cap, &rl);
        if (st == STATUS_BUFFER_OVERFLOW || st == STATUS_BUFFER_TOO_SMALL) {
            dh_free(b);
            cap = rl + 16;
            continue;
        }
        if (!NT_SUCCESS(st)) { dh_free(b); return st; }
        if (b->Type == REG_MULTI_SZ) {
            /* A multi-string from the registry is not always double terminated. The room for the extra characters was
             * allocated above the size asked for. */
            WCHAR *d = (WCHAR *)((char *)b + b->DataOffset);
            size_t n = b->DataLength / sizeof(WCHAR);
            d[n] = 0;
            d[n + 1] = 0;
            while (n < 2 || d[n - 2] || d[n - 1]) n++;
            b->DataLength = (ULONG)(n * sizeof(WCHAR));
        }
        *out = b;
        return STATUS_SUCCESS;
    }
    return STATUS_UNSUCCESSFUL;
}

DRVCALL NTSTATUS RtlQueryRegistryValues(ULONG RelativeTo, PCWSTR Path, PRTL_QUERY_REGISTRY_TABLE QueryTable,
                                        PVOID Context, PVOID Environment)
{
    HANDLE top, cur;
    int own;
    NTSTATUS st = rtl_key(RelativeTo, Path, 0, &top, &own);
    if (!NT_SUCCESS(st)) return st;
    cur = top;

    /* The table ends at an entry with no routine that neither selects a subkey nor stores directly. */
    for (; QueryTable->QueryRoutine || (QueryTable->Flags & (RTL_QUERY_REGISTRY_SUBKEY | RTL_QUERY_REGISTRY_DIRECT));
         QueryTable++) {
        PRTL_QUERY_REGISTRY_TABLE q = QueryTable;
        st = STATUS_SUCCESS;

        if ((q->Flags & RTL_QUERY_REGISTRY_DIRECT) &&
            (!q->Name || (q->Flags & RTL_QUERY_REGISTRY_SUBKEY) || q->QueryRoutine)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        if (q->Flags & (RTL_QUERY_REGISTRY_TOPKEY | RTL_QUERY_REGISTRY_SUBKEY)) {
            if (cur != top) { ZwClose(cur); cur = top; }
        }

        int enumerate = 0;
        if (q->Flags & RTL_QUERY_REGISTRY_SUBKEY) {
            if (!q->Name) { st = STATUS_INVALID_PARAMETER; break; }
            UNICODE_STRING sub;
            RtlInitUnicodeString(&sub, q->Name);
            OBJECT_ATTRIBUTES oa = { sizeof oa, top, &sub, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL };
            st = ZwOpenKey(&cur, MAXIMUM_ALLOWED, &oa);
            if (!NT_SUCCESS(st)) { cur = top; break; }
            enumerate = q->QueryRoutine != NULL;          /* a subkey entry with a routine reports the subkey's values */
        } else if (q->Name) {
            UNICODE_STRING vn;
            RtlInitUnicodeString(&vn, q->Name);
            KEY_VALUE_FULL_INFORMATION *info = NULL;
            st = fetch(cur, &vn, 0, &info);
            if (NT_SUCCESS(st)) {
                st = deliver(q, info, Context, Environment);
                if (NT_SUCCESS(st) && (q->Flags & RTL_QUERY_REGISTRY_DELETE)) ZwDeleteValueKey(cur, &vn);
                dh_free(info);
            } else if (st == STATUS_OBJECT_NAME_NOT_FOUND) {
                st = deliver(q, NULL, Context, Environment);      /* the default, or the REQUIRED failure */
            }
        } else if (q->Flags & RTL_QUERY_REGISTRY_NOVALUE) {
            st = q->QueryRoutine(NULL, REG_NONE, NULL, 0, Context, q->EntryContext);
        } else {
            enumerate = 1;
        }

        if (enumerate) {
            ULONG index = 0;
            for (;;) {
                KEY_VALUE_FULL_INFORMATION *info = NULL;
                st = fetch(cur, NULL, index, &info);
                if (st == STATUS_NO_MORE_ENTRIES) {
                    st = (index == 0 && (q->Flags & RTL_QUERY_REGISTRY_REQUIRED)) ? STATUS_OBJECT_NAME_NOT_FOUND
                                                                                   : STATUS_SUCCESS;
                    break;
                }
                if (!NT_SUCCESS(st)) break;
                st = deliver(q, info, Context, Environment);
                if (NT_SUCCESS(st) && (q->Flags & RTL_QUERY_REGISTRY_DELETE)) {
                    UNICODE_STRING vn = { (USHORT)info->NameLength, (USHORT)info->NameLength, info->Name };
                    if (NT_SUCCESS(ZwDeleteValueKey(cur, &vn))) index--;   /* the next value has moved into this slot */
                }
                dh_free(info);
                if (!NT_SUCCESS(st)) break;
                index++;
            }
        }
        if (!NT_SUCCESS(st)) break;
    }

    if (cur != top) ZwClose(cur);
    rtl_key_release(top, own);
    return st;
}

DRVCALL NTSTATUS RtlQueryRegistryValuesEx(ULONG RelativeTo, PCWSTR Path, PRTL_QUERY_REGISTRY_TABLE QueryTable,
                                          PVOID Context, PVOID Environment)
{
    return RtlQueryRegistryValues(RelativeTo, Path, QueryTable, Context, Environment);
}
