/* drvhost_regbridge.c - see drvhost_regbridge.h.
 *
 * Uses only the kernel-side registry API and the handle table of the current process, as drvhost_svcdb.c does.
 *
 * Known limit: the registry handles belong to the process that was current when the key was opened. A key opened on one
 * system thread and used on a thread of another process would not resolve. The driver host runs all its threads in one
 * process context, so this holds today; the in-kernel build has to keep it so.
 */
#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/slab.h>
#define DHREG_ALLOC(n) kzalloc((n), GFP_KERNEL)
#define DHREG_FREE(p)  kfree(p)
#else
#include <stddef.h>
#include <stdlib.h>
#define DHREG_ALLOC(n) calloc(1, (n))
#define DHREG_FREE(p)  free(p)
#endif

#include "registry.h"
#include "handle.h"
#include "object.h"
#include "kshim.h"
#include "drvhost_regbridge.h"
#include "nt_types.h"
#include "ntstatus.h"

#define DHREG_PATH_MAX 512            /* characters, including the terminator; the registry's own limit */
#define DHREG_GENERIC_READ    0x80000000UL
#define DHREG_GENERIC_WRITE   0x40000000UL
#define DHREG_GENERIC_EXECUTE 0x20000000UL
#define DHREG_GENERIC_ALL     0x10000000UL
#define DHREG_MAXIMUM_ALLOWED 0x02000000UL

struct dhreg_key {
    HANDLE kh;                          /* handle in the current process's table */
    unsigned len;                       /* characters in path */
    WCHAR  path[DHREG_PATH_MAX];
};

/* A kernel-mode caller names no token, so MAXIMUM_ALLOWED is every right the key has. */
static ACCESS_MASK map_access(unsigned long a)
{
    ACCESS_MASK m = a & ~(DHREG_GENERIC_READ | DHREG_GENERIC_WRITE | DHREG_GENERIC_EXECUTE | DHREG_GENERIC_ALL |
                          DHREG_MAXIMUM_ALLOWED);
    if (a & (DHREG_GENERIC_ALL | DHREG_MAXIMUM_ALLOWED)) m |= KEY_ALL_ACCESS;
    if (a & (DHREG_GENERIC_READ | DHREG_GENERIC_EXECUTE)) m |= KEY_READ;
    if (a & DHREG_GENERIC_WRITE) m |= KEY_WRITE;
    return m;
}

/* Builds the absolute path of `name` into out. Trailing backslashes are dropped. */
static NTSTATUS compose(const dhreg_key_t *root, const unsigned short *name, unsigned name_bytes, WCHAR *out,
                        unsigned *out_chars)
{
    unsigned n = name_bytes / 2, o = 0;
    if (n && name[n - 1] == 0) n--;                      /* tolerate a counted string that includes its NUL */
    while (n && name[n - 1] == '\\') n--;
    if (n && name[0] == '\\') {
        if (n + 1 > DHREG_PATH_MAX) return STATUS_NAME_TOO_LONG;
        for (unsigned i = 0; i < n; i++) out[o++] = name[i];
    } else {
        if (!root) return STATUS_OBJECT_PATH_SYNTAX_BAD;
        if (root->len + (n ? 1u : 0u) + n + 1 > DHREG_PATH_MAX) return STATUS_NAME_TOO_LONG;
        for (unsigned i = 0; i < root->len; i++) out[o++] = root->path[i];
        if (n) out[o++] = '\\';
        for (unsigned i = 0; i < n; i++) out[o++] = name[i];
    }
    out[o] = 0;
    *out_chars = o;
    return STATUS_SUCCESS;
}

static void as_ustring(UNICODE_STRING *u, const unsigned short *name, unsigned name_bytes)
{
    u->Buffer = (WCHAR *)name;
    u->Length = (USHORT)name_bytes;
    u->MaximumLength = (USHORT)name_bytes;
}

long dhreg_open(dhreg_key_t *root, const unsigned short *name, unsigned name_bytes, unsigned long access, int create,
                unsigned long *disposition, dhreg_key_t **out)
{
    if (!name || !out) return STATUS_INVALID_PARAMETER;
    dhreg_key_t *k = DHREG_ALLOC(sizeof *k);
    if (!k) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS st = compose(root, name, name_bytes, k->path, &k->len);
    if (!NT_SUCCESS(st)) { DHREG_FREE(k); return st; }

    UNICODE_STRING full;
    full.Buffer = k->path;
    full.Length = (USHORT)(k->len * sizeof(WCHAR));
    full.MaximumLength = (USHORT)((k->len + 1) * sizeof(WCHAR));

    ACCESS_MASK acc = map_access(access);
    unsigned long disp = REG_OPENED_EXISTING_KEY;
    st = ntreg_open_key_trusted(&full, acc, 0, &k->kh);          /* an existing key first, so the disposition is known */
    if (!NT_SUCCESS(st) && create) {
        st = ntreg_open_key_trusted(&full, acc, 1, &k->kh);
        disp = REG_CREATED_NEW_KEY;
    }
    if (!NT_SUCCESS(st)) { DHREG_FREE(k); return st; }
    if (disposition) *disposition = disp;
    *out = k;
    return STATUS_SUCCESS;
}

long dhreg_close(dhreg_key_t *key)
{
    if (!key) return STATUS_INVALID_HANDLE;
    NTSTATUS st = ObCloseHandle(kshim_current_process(), key->kh);
    DHREG_FREE(key);
    return st;
}

long dhreg_query_value(dhreg_key_t *key, const unsigned short *name, unsigned name_bytes, unsigned long info_class,
                       void *buf, unsigned long len, unsigned long *result_len)
{
    UNICODE_STRING n;
    ULONG rl = 0;
    as_ustring(&n, name, name_bytes);
    NTSTATUS st = NtQueryValueKey(key->kh, &n, info_class, buf, len, &rl);
    if (result_len) *result_len = rl;
    return st;
}

long dhreg_set_value(dhreg_key_t *key, const unsigned short *name, unsigned name_bytes, unsigned long type,
                     const void *data, unsigned long size)
{
    UNICODE_STRING n;
    as_ustring(&n, name, name_bytes);
    return NtSetValueKey(key->kh, &n, 0, type, (PVOID)data, size);
}

long dhreg_delete_value(dhreg_key_t *key, const unsigned short *name, unsigned name_bytes)
{
    UNICODE_STRING n;
    as_ustring(&n, name, name_bytes);
    return NtDeleteValueKey(key->kh, &n);
}

long dhreg_enum_value(dhreg_key_t *key, unsigned long index, unsigned long info_class, void *buf, unsigned long len,
                      unsigned long *result_len)
{
    ULONG rl = 0;
    NTSTATUS st = NtEnumerateValueKey(key->kh, index, info_class, buf, len, &rl);
    if (result_len) *result_len = rl;
    return st;
}

long dhreg_enum_key(dhreg_key_t *key, unsigned long index, unsigned long info_class, void *buf, unsigned long len,
                    unsigned long *result_len)
{
    ULONG rl = 0;
    NTSTATUS st = NtEnumerateKey(key->kh, index, info_class, buf, len, &rl);
    if (result_len) *result_len = rl;
    return st;
}

long dhreg_query_key(dhreg_key_t *key, unsigned long info_class, void *buf, unsigned long len,
                     unsigned long *result_len)
{
    ULONG rl = 0;
    NTSTATUS st = NtQueryKey(key->kh, info_class, buf, len, &rl);
    if (result_len) *result_len = rl;
    return st;
}

long dhreg_delete_key(dhreg_key_t *key)
{
    return NtDeleteKey(key->kh);
}

long dhreg_flush(dhreg_key_t *key)
{
    return NtFlushKey(key->kh);
}
