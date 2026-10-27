/* drvhost_svcdb.c - service records in the personality registry.  See drvhost_svcdb.h.
 *
 * Uses only the kernel-side registry API (NtCreateKey, NtOpenKey, NtSetValueKey,
 * NtQueryValueKey, NtDeleteKey) and the handle table of the current process.
 */
#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif

#include "registry.h"
#include "handle.h"
#include "object.h"
#include "kshim.h"
#include "drvhost_svcdb.h"
#include "nt_types.h"
#include "ntstatus.h"

#define SVC_NAME_MAX 128
#define SVC_DATA_MAX 512
#define SVC_DELETE_ACCESS 0x00010000UL   /* DELETE, the standard access bit */

/* Builds "<prefix><name>" as a UNICODE_STRING over buf.  Returns 0 if it does
 * not fit or the name has a backslash (a service name is one key component). */
static int svc_key_path(const char *name, WCHAR *buf, size_t cap, UNICODE_STRING *out)
{
    static const char prefix[] = DRVHOST_SVC_KEY_PREFIX;
    size_t pl = sizeof(prefix) - 1, nl = strlen(name);
    if (nl == 0 || nl > SVC_NAME_MAX || memchr(name, '\\', nl)) return 0;
    if (pl + nl + 1 > cap) return 0;
    size_t i = 0;
    for (; i < pl; i++) buf[i] = (WCHAR)(unsigned char)prefix[i];
    for (size_t j = 0; j < nl; j++) buf[i++] = (WCHAR)(unsigned char)name[j];
    buf[i] = 0;
    out->Buffer = buf;
    out->Length = (USHORT)(i * sizeof(WCHAR));
    out->MaximumLength = (USHORT)((i + 1) * sizeof(WCHAR));
    return 1;
}

static void make_wname(const char *s, WCHAR *buf, size_t cap, UNICODE_STRING *out)
{
    size_t n = strlen(s);
    if (n + 1 > cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) buf[i] = (WCHAR)(unsigned char)s[i];
    buf[n] = 0;
    out->Buffer = buf;
    out->Length = (USHORT)(n * sizeof(WCHAR));
    out->MaximumLength = (USHORT)((n + 1) * sizeof(WCHAR));
}

/* Opens (or creates) the service key; the caller closes the handle. */
static NTSTATUS open_service_key(const char *name, ACCESS_MASK access, int create, HANDLE *out)
{
    WCHAR pbuf[sizeof(DRVHOST_SVC_KEY_PREFIX) + SVC_NAME_MAX + 2];
    UNICODE_STRING pu;
    if (!svc_key_path(name, pbuf, sizeof pbuf / sizeof pbuf[0], &pu)) return STATUS_OBJECT_NAME_INVALID;
    /* Writes under \Registry\Machine are privileged: the driver host registers
     * and removes service records itself, so it uses the trusted open.  A read
     * is an ordinary NtOpenKey. */
    if (create || (access & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | SVC_DELETE_ACCESS)))
        return ntreg_open_key_trusted(&pu, access, create, out);
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof oa);
    oa.Length = sizeof oa;
    oa.ObjectName = &pu;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    return NtOpenKey(out, access, &oa);
}

static void close_key(HANDLE h)
{
    ObCloseHandle(kshim_current_process(), h);
}

static NTSTATUS set_str(HANDLE key, const char *value, const char *s)
{
    WCHAR vbuf[64], dbuf[256];
    UNICODE_STRING vn, dn;
    make_wname(value, vbuf, sizeof vbuf / sizeof vbuf[0], &vn);
    make_wname(s, dbuf, sizeof dbuf / sizeof dbuf[0], &dn);
    /* REG_SZ data includes the terminating NUL. */
    return NtSetValueKey(key, &vn, 0, REG_SZ, dbuf, dn.Length + (ULONG)sizeof(WCHAR));
}

static NTSTATUS set_dword(HANDLE key, const char *value, unsigned long v)
{
    WCHAR vbuf[64];
    UNICODE_STRING vn;
    make_wname(value, vbuf, sizeof vbuf / sizeof vbuf[0], &vn);
    ULONG d = (ULONG)v;
    return NtSetValueKey(key, &vn, 0, REG_DWORD, &d, sizeof d);
}

/* Reads a value's partial information into buf.  On success *type and *dlen
 * hold the value type and data length. */
static NTSTATUS read_value(HANDLE key, const char *value, unsigned char *buf, ULONG len,
                           ULONG *type, ULONG *dlen)
{
    WCHAR vbuf[64];
    UNICODE_STRING vn;
    make_wname(value, vbuf, sizeof vbuf / sizeof vbuf[0], &vn);
    ULONG rlen = 0;
    NTSTATUS st = NtQueryValueKey(key, &vn, KeyValuePartialInformation, buf, len, &rlen);
    /* The key exists (the caller opened it), so a missing value means the record
     * is incomplete, not that the service is absent. */
    if (st == STATUS_OBJECT_NAME_NOT_FOUND) return STATUS_OBJECT_NAME_INVALID;
    if (!NT_SUCCESS(st)) return st;
    if (rlen < offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data)) return STATUS_OBJECT_NAME_INVALID;
    KEY_VALUE_PARTIAL_INFORMATION *pi = (KEY_VALUE_PARTIAL_INFORMATION *)buf;
    if (pi->DataLength > rlen - offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data))
        return STATUS_OBJECT_NAME_INVALID;
    *type = pi->Type;
    *dlen = pi->DataLength;
    return STATUS_SUCCESS;
}

long drvhost_svcdb_register(const char *name, const char *image_path,
                            unsigned long type, unsigned long start)
{
    if (!name || !image_path || strlen(image_path) >= 256) return STATUS_INVALID_PARAMETER;
    HANDLE key = NULL;
    NTSTATUS st = open_service_key(name, KEY_ALL_ACCESS, 1, &key);
    if (!NT_SUCCESS(st)) return st;
    st = set_str(key, "ImagePath", image_path);
    if (NT_SUCCESS(st)) st = set_dword(key, "Type", type);
    if (NT_SUCCESS(st)) st = set_dword(key, "Start", start);
    close_key(key);
    return st;
}

long drvhost_svcdb_lookup(const char *name, drvhost_svc_record_t *out)
{
    if (!name || !out) return STATUS_INVALID_PARAMETER;
    HANDLE key = NULL;
    NTSTATUS st = open_service_key(name, KEY_READ, 0, &key);
    if (!NT_SUCCESS(st)) return st;

    unsigned char buf[offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) + SVC_DATA_MAX];
    ULONG type = 0, dlen = 0;
    const size_t data_off = offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data);

    st = read_value(key, "ImagePath", buf, sizeof buf, &type, &dlen);
    if (NT_SUCCESS(st) && (type != REG_SZ || dlen < sizeof(WCHAR) || (dlen & 1)))
        st = STATUS_OBJECT_NAME_INVALID;
    if (NT_SUCCESS(st)) {
        const WCHAR *w = (const WCHAR *)(buf + data_off);
        size_t n = dlen / sizeof(WCHAR);
        if (n && w[n - 1] == 0) n--;              /* drop the stored NUL */
        if (n >= sizeof out->image_path) st = STATUS_OBJECT_NAME_INVALID;
        for (size_t i = 0; NT_SUCCESS(st) && i < n; i++) {
            if (w[i] >= 0x80) st = STATUS_OBJECT_NAME_INVALID;
            else out->image_path[i] = (char)w[i];
        }
        if (NT_SUCCESS(st)) out->image_path[n] = 0;
    }
    if (NT_SUCCESS(st)) {
        st = read_value(key, "Type", buf, sizeof buf, &type, &dlen);
        if (NT_SUCCESS(st) && (type != REG_DWORD || dlen != 4)) st = STATUS_OBJECT_NAME_INVALID;
        if (NT_SUCCESS(st)) memcpy(&out->type, buf + data_off, 4);
    }
    if (NT_SUCCESS(st)) {
        st = read_value(key, "Start", buf, sizeof buf, &type, &dlen);
        if (NT_SUCCESS(st) && (type != REG_DWORD || dlen != 4)) st = STATUS_OBJECT_NAME_INVALID;
        if (NT_SUCCESS(st)) memcpy(&out->start, buf + data_off, 4);
    }
    close_key(key);
    return st;
}

long drvhost_svcdb_remove(const char *name)
{
    HANDLE key = NULL;
    NTSTATUS st = open_service_key(name, KEY_ALL_ACCESS, 0, &key);
    if (!NT_SUCCESS(st)) return st;
    st = NtDeleteKey(key);
    close_key(key);
    return st;
}
