/*
 * object.c - NT object manager.
 *
 * Kernel-resident implementation: object headers, types, namespace, reference
 * counting, name lookup, and handle integration.
 *
 * Reference: Windows Internals 7th ed., ch. 3; WRK Ob* sources.
 */

#include "object.h"
#include "handle.h"
#include "security.h"
#ifndef __KERNEL__
#include <wchar.h>
#else
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/init.h>
#ifdef __KERNEL__
#include <linux/uaccess.h>
#endif
#endif

/* -------------------------------------------------------------------------- */
/* Unicode helpers                                                            */
/* -------------------------------------------------------------------------- */

static WCHAR nt_toupper(WCHAR ch)
{
    if (ch >= (WCHAR)'a' && ch <= (WCHAR)'z')
        return (WCHAR)(ch - (WCHAR)'a' + (WCHAR)'A');
    return ch;
}

/* Case-insensitive compare: ASCII only. Known gap: full Unicode casefold
 * (e.g., Turkish dotted i, German sharp s) not implemented; needed for full
 * NT \BaseNamedObjects fidelity but not for ASCII corpus.
 */
static bool nt_equal_ci(const UNICODE_STRING *a, const WCHAR *b, size_t blen)
{
    size_t alen = 0;
    size_t i;
    if (a->Length == 0 && blen == 0) return true;
    alen = (size_t)a->Length / sizeof(WCHAR);
    if (alen != blen) return false;
    for (i = 0; i < alen; i++) {
        if (nt_toupper(a->Buffer[i]) != nt_toupper(b[i]))
            return false;
    }
    return true;
}

static bool nt_equal_us_ci(const UNICODE_STRING *a, const UNICODE_STRING *b)
{
    size_t alen = (size_t)a->Length / sizeof(WCHAR);
    size_t blen = (size_t)b->Length / sizeof(WCHAR);
    size_t i;
    if (alen != blen) return false;
    for (i = 0; i < alen; i++) {
        if (nt_toupper(a->Buffer[i]) != nt_toupper(b->Buffer[i]))
            return false;
    }
    return true;
}

static NTSTATUS nt_dup_unicodestring(PCUNICODE_STRING src, PUNICODE_STRING dst)
{
    USHORT len = src->Length;
    USHORT max_len;
    if (len & (sizeof(WCHAR)-1))
        return STATUS_OBJECT_NAME_INVALID;
    /* len + the terminator must fit in USHORT: 0xFFFE + 2 wraps to 0 */
    if (len > (USHORT)(0xFFFF - sizeof(WCHAR)))
        return STATUS_NAME_TOO_LONG;
    max_len = (USHORT)(len + sizeof(WCHAR));
    /* zero-length allowed */
    if (len == 0) {
        dst->Buffer = NULL;
        dst->Length = 0;
        dst->MaximumLength = 0;
        return STATUS_SUCCESS;
    }
    dst->Buffer = (PWSTR)kmalloc((size_t)max_len, GFP_KERNEL);
    if (!dst->Buffer)
        return STATUS_NO_MEMORY;
    memcpy(dst->Buffer, src->Buffer, (size_t)len);
    dst->Buffer[len / sizeof(WCHAR)] = 0;
    dst->Length = len;
    dst->MaximumLength = max_len;
    return STATUS_SUCCESS;
}

static void nt_free_unicodestring(PUNICODE_STRING us)
{
    if (us->Buffer) {
        kfree(us->Buffer);
        us->Buffer = NULL;
    }
    us->Length = 0;
    us->MaximumLength = 0;
}

/* -------------------------------------------------------------------------- */
/* Path component iterator                                                    */
/* -------------------------------------------------------------------------- */

static bool nt_next_component(const UNICODE_STRING *path, size_t *pos, UNICODE_STRING *comp)
{
    size_t len = (size_t)path->Length / sizeof(WCHAR);
    const WCHAR *buf = path->Buffer;
    size_t start;
    while (*pos < len && buf[*pos] == (WCHAR)'\\')
        (*pos)++;
    if (*pos >= len) return false;
    start = *pos;
    while (*pos < len && buf[*pos] != (WCHAR)'\\')
        (*pos)++;
    comp->Buffer = (PWSTR)(buf + start);
    comp->Length = (USHORT)((*pos - start) * sizeof(WCHAR));
    comp->MaximumLength = comp->Length;
    return true;
}

static bool nt_is_absolute(const UNICODE_STRING *path)
{
    return path && path->Length >= sizeof(WCHAR) && path->Buffer[0] == (WCHAR)'\\';
}

/* -------------------------------------------------------------------------- */
/* Object types                                                               */
/* -------------------------------------------------------------------------- */

static void directory_delete(struct nt_object_header *hdr)
{
    struct nt_directory *dir = (struct nt_directory *)nt_object_body(hdr);
    struct nt_dir_entry *de, *tmp;
    /* Recursively free child entries if any remain (e.g., BaseNamedObjects children). */
    write_lock(&dir->lock);
    list_for_each_entry_safe(de, tmp, &dir->entries, link) {
        list_del(&de->link);
        struct nt_object_header *obj = de->obj;
        nt_free_unicodestring(&de->name);
        kfree(de);
        obj->parent_dir = NULL;
        obj->name.Buffer = NULL;
        obj->name.Length = 0;
        /* Drop directory's reference to child */
        ObDereferenceObject(obj);
    }
    write_unlock(&dir->lock);
}

static void symlink_delete(struct nt_object_header *hdr)
{
    struct nt_symlink *link = (struct nt_symlink *)nt_object_body(hdr);
    nt_free_unicodestring(&link->target);
}

static void test_delete(struct nt_object_header *hdr)
{
    struct nt_test_body *body = (struct nt_test_body *)nt_object_body(hdr);
    body->delete_called = true;
}

static void test_close(struct nt_object_header *hdr, HANDLE h)
{
    struct nt_test_body *body = (struct nt_test_body *)nt_object_body(hdr);
    (void)h;
    body->close_pending = true;
}

struct nt_object_type nt_directory_type = {
    .name = "Directory",
    .ops = { .delete_obj = directory_delete, },
    .pool_tag = 0,
};

struct nt_object_type nt_symlink_type = {
    .name = "SymbolicLink",
    .ops = { .delete_obj = symlink_delete, },
    .pool_tag = 0,
};

struct nt_object_type nt_process_type = {
    .name = "Process",
    .ops = { .delete_obj = NULL, },
    .pool_tag = 0,
};

struct nt_object_type nt_thread_type = {
    .name = "Thread",
    .ops = { .delete_obj = NULL, },
    .pool_tag = 0,
};

static void token_delete(struct nt_object_header *hdr)
{
    struct nt_token_body *b = (struct nt_token_body *)nt_object_body(hdr);
    if (b->token) {
        token_dereference(b->token);
        b->token = NULL;
    }
}

struct nt_object_type nt_token_type = {
    .name = "Token",
    .ops = { .delete_obj = token_delete, },
    .pool_tag = 0,
};

struct nt_object_type nt_test_type = {
    .name = "TestObject",
    .ops = { .delete_obj = test_delete, .close = test_close, },
    .pool_tag = 0,
};

static struct list_head ob_type_list;

/* -------------------------------------------------------------------------- */
/* Namespace roots                                                            */
/* -------------------------------------------------------------------------- */

static struct nt_directory    ob_root_dir;
static struct nt_object_header ob_root_hdr;

/* Serializes replacement of an object security descriptor against readers that
 * copy it (NtQuerySecurityObject). Initialized in ObInitObjectManager. */
static spinlock_t ob_sd_lock;

static struct nt_object_header *ob_dosdevices_hdr = NULL;
static struct nt_object_header *ob_device_hdr = NULL;
static struct nt_object_header *ob_basenamedobjects_hdr = NULL;
static struct nt_object_header *ob_knowndlls_hdr = NULL;
static struct nt_object_header *ob_sessions_hdr = NULL;

static bool ob_initialized = false;

/* -------------------------------------------------------------------------- */
/* Directory helpers - internal                                               */
/* -------------------------------------------------------------------------- */

struct nt_object_header *ObpLookupDirectoryEntry(struct nt_directory *dir, const UNICODE_STRING *comp)
{
    struct nt_dir_entry *de;
    struct list_head *pos;
    struct nt_object_header *found = NULL;
    read_lock(&dir->lock);
    list_for_each(pos, &dir->entries) {
        de = list_entry(pos, struct nt_dir_entry, link);
        if (nt_equal_ci(&de->name, comp->Buffer, (size_t)comp->Length / sizeof(WCHAR))) {
            found = de->obj;
            ObReferenceObject(found);
            break;
        }
    }
    read_unlock(&dir->lock);
    return found;
}

/* Lookup returning raw entry pointer (no ref) - used only while holding lock */
static struct nt_dir_entry *obp_find_entry_locked(struct nt_directory *dir, const UNICODE_STRING *name)
{
    struct nt_dir_entry *de;
    list_for_each_entry(de, &dir->entries, link) {
        if (nt_equal_us_ci(&de->name, name))
            return de;
    }
    return NULL;
}

static NTSTATUS ObpInsertDirectoryEntry(struct nt_directory *dir,
                                         const UNICODE_STRING *name,
                                         struct nt_object_header *obj)
{
    struct nt_dir_entry *de;
    NTSTATUS status;
    if (!name || name->Length == 0)
        return STATUS_OBJECT_NAME_INVALID;
    write_lock(&dir->lock);
    if (obp_find_entry_locked(dir, name)) {
        write_unlock(&dir->lock);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    de = (struct nt_dir_entry *)kzalloc(sizeof(*de), GFP_KERNEL);
    if (!de) {
        write_unlock(&dir->lock);
        return STATUS_NO_MEMORY;
    }
    status = nt_dup_unicodestring(name, &de->name);
    if (!NT_SUCCESS(status)) {
        kfree(de);
        write_unlock(&dir->lock);
        return status;
    }
    de->obj = obj;
    INIT_LIST_HEAD(&de->link);
    list_add_tail(&de->link, &dir->entries);
    write_unlock(&dir->lock);
    ObReferenceObject(obj);
    obj->parent_dir = dir;
    /* Save copy of name in header (points to dir entry's copy). We duplicate again for header ownership? Keep header name pointing to entry's buffer for test introspection. */
    obj->name = de->name;
    if (obj->flags & OBJ_PERMANENT)
        obj->is_permanent = true;
    if (obj->flags & OBJ_EXCLUSIVE)
        obj->is_exclusive = true;
    return STATUS_SUCCESS;
}

static void ObpRemoveFromDirectory(struct nt_object_header *obj)
{
    struct nt_directory *dir = obj->parent_dir;
    struct nt_dir_entry *de;
    bool removed = false;
    if (!dir)
        return;
    write_lock(&dir->lock);
    /* Re-check parent_dir under the directory lock. Two callers can read the same
     * non-NULL parent_dir (concurrent ObMakeTemporaryObject). Only the one that
     * still finds the entry linked removes it and drops the directory's
     * reference; the other sees NULL and does nothing. */
    if (obj->parent_dir == dir) {
        list_for_each_entry(de, &dir->entries, link) {
            if (de->obj == obj) {
                list_del(&de->link);
                /* Header name points to this entry's name buffer. Clear header name after freeing. */
                nt_free_unicodestring(&de->name);
                kfree(de);
                break;
            }
        }
        obj->parent_dir = NULL;
        obj->name.Buffer = NULL;
        obj->name.Length = 0;
        obj->name.MaximumLength = 0;
        removed = true;
    }
    write_unlock(&dir->lock);
    if (removed)
        ObDereferenceObject(obj);
}

/* -------------------------------------------------------------------------- */
/* Symlink resolution                                                         */
/* -------------------------------------------------------------------------- */

#define OB_MAX_SYMLINK_DEPTH 32

static NTSTATUS ObpParsePath(PUNICODE_STRING path,
                               ULONG attributes,
                               struct nt_object_type *type,
                               unsigned int depth,
                               struct nt_object_header **out);

static NTSTATUS ObpResolveSymlink(struct nt_object_header *link_hdr,
                                   struct nt_object_type *type,
                                   ULONG attributes,
                                   unsigned int depth,
                                   struct nt_object_header **out)
{
    struct nt_symlink *link = (struct nt_symlink *)nt_object_body(link_hdr);
    NTSTATUS status;
    bool dont_resolve = (attributes & OBJ_OPENLINK) != 0;
    if (dont_resolve) {
        ObReferenceObject(link_hdr);
        *out = link_hdr;
        return STATUS_SUCCESS;
    }
    if (link->target.Length == 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (depth >= OB_MAX_SYMLINK_DEPTH)
        return STATUS_NAME_TOO_LONG;
    status = ObpParsePath(&link->target, 0, type, depth + 1, out);
    return status;
}

/* Core path walk */
static NTSTATUS ObpParsePath(PUNICODE_STRING path,
                               ULONG attributes,
                               struct nt_object_type *type,
                               unsigned int depth,
                               struct nt_object_header **out)
{
    struct nt_object_header *cur_hdr;
    UNICODE_STRING comp;
    size_t pos = 0;
    NTSTATUS status;

    if (!path || path->Length == 0)
        return STATUS_OBJECT_NAME_INVALID;
    if (path->Length & (sizeof(WCHAR)-1))
        return STATUS_OBJECT_NAME_INVALID;
    if (!nt_is_absolute(path))
        return STATUS_OBJECT_PATH_SYNTAX_BAD;

    cur_hdr = &ob_root_hdr;
    ObReferenceObject(cur_hdr);

    while (nt_next_component(path, &pos, &comp)) {
        struct nt_object_header *next = NULL;
        size_t saved = pos;
        bool has_more = false;
        UNICODE_STRING dummy;
        size_t tmp = pos;
        if (nt_next_component(path, &tmp, &dummy))
            has_more = true;
        pos = saved;

        if (!nt_object_is_directory(cur_hdr)) {
            /* Non-directory in middle of path */
            if (cur_hdr->type && cur_hdr->type->ops.parse) {
                UNICODE_STRING rem;
                size_t rem_len = (size_t)path->Length / sizeof(WCHAR) - (size_t)(comp.Buffer - path->Buffer);
                rem.Buffer = comp.Buffer;
                rem.Length = (USHORT)(rem_len * sizeof(WCHAR));
                rem.MaximumLength = rem.Length;
                status = cur_hdr->type->ops.parse(cur_hdr, NULL, &rem, attributes, &next);
                ObDereferenceObject(cur_hdr);
                if (!NT_SUCCESS(status))
                    return status;
                cur_hdr = next;
                if (!has_more) {
                    /* parse succeeded and this was final */
                    if (type && cur_hdr->type != type) {
                        ObDereferenceObject(cur_hdr);
                        return STATUS_OBJECT_TYPE_MISMATCH;
                    }
                    *out = cur_hdr;
                    return STATUS_SUCCESS;
                }
                continue;
            }
            ObDereferenceObject(cur_hdr);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }

        /* cur_hdr is directory: look up next component */
        struct nt_directory *dir = (struct nt_directory *)nt_object_body(cur_hdr);
        struct nt_object_header *found = ObpLookupDirectoryEntry(dir, &comp);
        if (!found) {
            ObDereferenceObject(cur_hdr);
            return has_more ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
        }

        if (!has_more) {
            /* Final component: handle symlink resolution */
            if (found->type == &nt_symlink_type) {
                struct nt_object_header *resolved = NULL;
                status = ObpResolveSymlink(found, type, attributes, depth, &resolved);
                ObDereferenceObject(found);
                ObDereferenceObject(cur_hdr);
                if (!NT_SUCCESS(status))
                    return status;
                if (type && resolved->type != type) {
                    ObDereferenceObject(resolved);
                    return STATUS_OBJECT_TYPE_MISMATCH;
                }
                *out = resolved;
                return STATUS_SUCCESS;
            }
            if (type && found->type != type) {
                ObDereferenceObject(found);
                ObDereferenceObject(cur_hdr);
                return STATUS_OBJECT_TYPE_MISMATCH;
            }
            /* Exclusive handling: if object is exclusive and already has handles, open should get sharing violation - but lookup itself succeeds; caller (ObOpenObjectByName) will enforce. */
            ObDereferenceObject(cur_hdr);
            *out = found;
            return STATUS_SUCCESS;
        }

        /* has more components: if found is symlink, we do not follow symlinks in middle (known gap, return path not found) */
        if (found->type == &nt_symlink_type) {
            ObDereferenceObject(found);
            ObDereferenceObject(cur_hdr);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }

        ObDereferenceObject(cur_hdr);
        cur_hdr = found;
    }

    /* Path was "\" or all slashes -> root */
    if (type && cur_hdr->type != type) {
        ObDereferenceObject(cur_hdr);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    *out = cur_hdr;
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

NTSTATUS ObCreateObject(struct nt_object_type *type,
                         POBJECT_ATTRIBUTES attrs,
                         SIZE_T body_size,
                         struct nt_object_header **out_hdr,
                         void **out_body)
{
    struct nt_object_header *hdr;
    SIZE_T total;
    if (!type || !out_hdr)
        return STATUS_INVALID_PARAMETER;
    if (attrs && (attrs->Attributes & ~OBJ_VALID_ATTRIBUTES))
        return STATUS_INVALID_PARAMETER;
    total = sizeof(*hdr) + body_size;
    hdr = (struct nt_object_header *)kzalloc(total, GFP_KERNEL);
    if (!hdr)
        return STATUS_NO_MEMORY;
    hdr->type = type;
    atomic64_set(&hdr->ref_count, 1);
    atomic64_set(&hdr->handle_count, 0);
    hdr->body = (char *)hdr + sizeof(*hdr);
    INIT_LIST_HEAD(&hdr->type_link);
    hdr->name.Buffer = NULL;
    hdr->name.Length = 0;
    hdr->name.MaximumLength = 0;
    hdr->parent_dir = NULL;
    hdr->flags = 0;
    hdr->is_permanent = false;
    hdr->is_exclusive = false;
    hdr->exclusive_owned = false;
    /* attrs->SecurityDescriptor is not consumed here: the object never
     * borrows a caller buffer (its length is unknown). Attach an SD with
     * ObSetObjectSecurityDescriptor. */
    hdr->security_descriptor = NULL;
    hdr->security_descriptor_length = 0;
    if (attrs)
        hdr->flags = attrs->Attributes & OBJ_VALID_ATTRIBUTES;
    *out_hdr = hdr;
    if (out_body)
        *out_body = hdr->body;
    return STATUS_SUCCESS;
}

NTSTATUS ObInsertObject(struct nt_object_header *obj,
                         POBJECT_ATTRIBUTES attrs,
                         ACCESS_MASK access,
                         struct nt_eprocess *proc,
                         PHANDLE out_handle)
{
    NTSTATUS status = STATUS_SUCCESS;
    bool inserted = false;
    struct nt_eprocess *target = proc ? proc : kshim_current_process();

    if (!obj)
        return STATUS_INVALID_PARAMETER;

    if (attrs && attrs->ObjectName && attrs->ObjectName->Length) {
        UNICODE_STRING path = *attrs->ObjectName;
        if (!nt_is_absolute(&path))
            return STATUS_OBJECT_PATH_SYNTAX_BAD;
        /* Split into parent path and final component */
        size_t len = (size_t)path.Length / sizeof(WCHAR);
        ssize_t slash = -1;
        ssize_t i;
        for (i = (ssize_t)len - 1; i >= 0; i--) {
            if (path.Buffer[i] == (WCHAR)'\\') { slash = i; break; }
        }
        if (slash < 0)
            return STATUS_OBJECT_PATH_SYNTAX_BAD;
        UNICODE_STRING name;
        name.Buffer = path.Buffer + slash + 1;
        name.Length = (USHORT)((len - (size_t)slash - 1) * sizeof(WCHAR));
        name.MaximumLength = name.Length;
        if (name.Length == 0)
            return STATUS_OBJECT_NAME_INVALID;

        struct nt_object_header *parent_hdr = NULL;
        struct nt_directory *parent_dir = NULL;
        if (slash == 0) {
            parent_hdr = &ob_root_hdr;
            ObReferenceObject(parent_hdr);
        } else {
            UNICODE_STRING parent_path;
            parent_path.Buffer = path.Buffer;
            parent_path.Length = (USHORT)(slash * sizeof(WCHAR));
            parent_path.MaximumLength = parent_path.Length;
            status = ObReferenceObjectByName(&parent_path, &nt_directory_type, KernelMode, 0, &parent_hdr);
            if (!NT_SUCCESS(status))
                return status;
        }
        parent_dir = (struct nt_directory *)nt_object_body(parent_hdr);
        status = ObpInsertDirectoryEntry(parent_dir, &name, obj);
        ObDereferenceObject(parent_hdr);
        if (!NT_SUCCESS(status))
            return status;
        inserted = true;
    }

    if (out_handle) {
        ULONG hattr = 0;
        if (attrs) {
            if (attrs->Attributes & OBJ_INHERIT) hattr |= OBJ_INHERIT;
            if (attrs->Attributes & OBJ_PROTECT_FROM_CLOSE) hattr |= OBJ_PROTECT_FROM_CLOSE;
        }
        if (!target) {
            if (inserted) ObpRemoveFromDirectory(obj);
            return STATUS_INVALID_PARAMETER;
        }
        if (!target->handle_table) {
            if (inserted) ObpRemoveFromDirectory(obj);
            return STATUS_INVALID_HANDLE;
        }
        status = ObCreateHandle(target, obj, access, hattr, out_handle);
        if (!NT_SUCCESS(status) && inserted)
            ObpRemoveFromDirectory(obj);
    }
    return status;
}

NTSTATUS ObCreateDirectoryObject(POBJECT_ATTRIBUTES oa, struct nt_object_header **out_hdr)
{
    struct nt_object_header *hdr = NULL;
    struct nt_directory *dir = NULL;
    NTSTATUS st;
    if (!out_hdr)
        return STATUS_INVALID_PARAMETER;
    st = ObCreateObject(&nt_directory_type, oa, sizeof(*dir), &hdr, (void **)&dir);
    if (!NT_SUCCESS(st))
        return st;
    INIT_LIST_HEAD(&dir->entries);
    rwlock_init(&dir->lock);
    *out_hdr = hdr;
    return STATUS_SUCCESS;
}

NTSTATUS ObCreateSymbolicLinkObject(POBJECT_ATTRIBUTES oa, PCUNICODE_STRING target,
                                    struct nt_object_header **out_hdr)
{
    struct nt_object_header *hdr = NULL;
    struct nt_symlink *link = NULL;
    NTSTATUS st;
    if (!out_hdr || !target || target->Length == 0 || !target->Buffer)
        return STATUS_INVALID_PARAMETER;
    st = ObCreateObject(&nt_symlink_type, oa, sizeof(*link), &hdr, (void **)&link);
    if (!NT_SUCCESS(st))
        return st;
    st = nt_dup_unicodestring(target, &link->target);
    if (!NT_SUCCESS(st)) {
        /* symlink_delete frees whatever target holds (nothing, here). */
        ObDereferenceObject(hdr);
        return st;
    }
    *out_hdr = hdr;
    return STATUS_SUCCESS;
}

NTSTATUS ObInsertOrOpenNamedObject(struct nt_object_header *obj,
                                   POBJECT_ATTRIBUTES oa,
                                   ACCESS_MASK access,
                                   struct nt_eprocess *proc,
                                   PHANDLE out_handle)
{
    unsigned int attempt;
    if (!obj || !out_handle)
        return STATUS_INVALID_PARAMETER;
    for (attempt = 0; attempt < 2; attempt++) {
        struct nt_object_header *found = NULL;
        NTSTATUS st;
        if (oa && oa->ObjectName && oa->ObjectName->Length) {
            const UNICODE_STRING *path = oa->ObjectName;
            size_t len = (size_t)path->Length / sizeof(WCHAR);
            ssize_t slash = -1;
            ssize_t i;
            if (path->Length & (sizeof(WCHAR) - 1))
                return STATUS_OBJECT_NAME_INVALID;
            if (len == 0 || path->Buffer[len - 1] == (WCHAR)'\\')
                return STATUS_OBJECT_NAME_INVALID;
            for (i = (ssize_t)len - 1; i >= 0; i--) {
                if (path->Buffer[i] == (WCHAR)'\\') { slash = i; break; }
            }
            /* A missing parent is PATH_NOT_FOUND, not NAME_NOT_FOUND. */
            if (slash > 0) {
                UNICODE_STRING parent = *path;
                struct nt_object_header *parent_hdr = NULL;
                parent.Length = (USHORT)(slash * sizeof(WCHAR));
                parent.MaximumLength = parent.Length;
                st = ObpParsePath(&parent, 0, NULL, 0, &parent_hdr);
                if (st == STATUS_OBJECT_NAME_NOT_FOUND)
                    return STATUS_OBJECT_PATH_NOT_FOUND;
                if (NT_SUCCESS(st))
                    ObDereferenceObject(parent_hdr);
            }
            /* OBJ_OPENLINK: look at a symbolic link at the last component
             * itself, not at its target. */
            st = ObpParsePath(oa->ObjectName, OBJ_OPENLINK, NULL, 0, &found);
            if (NT_SUCCESS(st)) {
                ULONG hattr = 0;
                if (found->type != obj->type) {
                    ObDereferenceObject(found);
                    return STATUS_OBJECT_TYPE_MISMATCH;
                }
                if (!(oa->Attributes & OBJ_OPENIF)) {
                    ObDereferenceObject(found);
                    return STATUS_OBJECT_NAME_COLLISION;
                }
                if (oa->Attributes & OBJ_INHERIT) hattr |= OBJ_INHERIT;
                if (oa->Attributes & OBJ_PROTECT_FROM_CLOSE) hattr |= OBJ_PROTECT_FROM_CLOSE;
                st = ObCreateHandle(proc ? proc : kshim_current_process(), found, access, hattr, out_handle);
                ObDereferenceObject(found);
                return NT_SUCCESS(st) ? STATUS_OBJECT_NAME_EXISTS : st;
            }
        }
        {
            NTSTATUS st = ObInsertObject(obj, oa, access, proc, out_handle);
            if (st != STATUS_OBJECT_NAME_COLLISION)
                return st;
            /* Another thread inserted the same name after our lookup: look again
             * so the result matches the existing object. */
        }
    }
    return STATUS_OBJECT_NAME_COLLISION;
}

void ObReferenceObject(struct nt_object_header *obj)
{
    if (!obj) return;
    atomic64_inc(&obj->ref_count);
}

void ObDereferenceObject(struct nt_object_header *obj)
{
    long long c;
    if (!obj) return;
    c = atomic64_dec_return(&obj->ref_count);
    KASSERT(c >= 0);
    if (c > 0) return;
    /* last reference */
    KASSERT(atomic64_read(&obj->handle_count) == 0);
    if (obj->parent_dir)
        ObpRemoveFromDirectory(obj);
    if (obj->type && obj->type->ops.delete_obj)
        obj->type->ops.delete_obj(obj);
    /* The security descriptor is an owned copy: release it with the header. */
    if (obj->security_descriptor) {
        kfree(obj->security_descriptor);
        obj->security_descriptor = NULL;
        obj->security_descriptor_length = 0;
    }
    /* If this is the static root header, don't free it */
    if (obj == &ob_root_hdr)
        return;
    kfree(obj);
}

NTSTATUS ObSetObjectSecurityDescriptor(struct nt_object_header *obj,
                                        const void *sd, ULONG length)
{
    void *copy = NULL;
    void *old;
    if (!obj)
        return STATUS_INVALID_PARAMETER;
    if (sd && length == 0)
        return STATUS_INVALID_PARAMETER;
    if (sd) {
        copy = kmalloc(length, GFP_KERNEL);
        if (!copy)
            return STATUS_NO_MEMORY;
        memcpy(copy, sd, length);
    }
    spin_lock(&ob_sd_lock);
    old = obj->security_descriptor;
    obj->security_descriptor = copy;
    obj->security_descriptor_length = copy ? length : 0;
    spin_unlock(&ob_sd_lock);
    if (old)
        kfree(old);
    return STATUS_SUCCESS;
}

NTSTATUS ObCopyObjectSecurityDescriptor(struct nt_object_header *obj,
                                        void **out, ULONG *out_len)
{
    void *copy;
    ULONG len;
    if (!obj || !out || !out_len)
        return STATUS_INVALID_PARAMETER;
    /* The copy is allocated outside the lock (it may sleep), so the length is
     * re-checked under the lock and the attempt is repeated if the descriptor
     * was replaced in between. */
    for (;;) {
        bool have;
        spin_lock(&ob_sd_lock);
        have = obj->security_descriptor != NULL;
        len = obj->security_descriptor_length;
        spin_unlock(&ob_sd_lock);
        if (!have) {
            *out = NULL;
            *out_len = 0;
            return STATUS_SUCCESS;
        }
        copy = kmalloc(len, GFP_KERNEL);
        if (!copy)
            return STATUS_NO_MEMORY;
        spin_lock(&ob_sd_lock);
        if (obj->security_descriptor && obj->security_descriptor_length == len) {
            memcpy(copy, obj->security_descriptor, len);
            spin_unlock(&ob_sd_lock);
            *out = copy;
            *out_len = len;
            return STATUS_SUCCESS;
        }
        spin_unlock(&ob_sd_lock);
        kfree(copy);
    }
}

NTSTATUS ObpIncrementHandleCount(struct nt_object_header *obj)
{
    if (!obj) return STATUS_INVALID_PARAMETER;
    atomic64_inc(&obj->handle_count);
    if (obj->is_exclusive) {
        obj->exclusive_owned = true;
    }
    return STATUS_SUCCESS;
}

void ObpDecrementHandleCount(struct nt_object_header *obj, HANDLE handle)
{
    (void)handle;
    if (!obj) return;
    long long c = atomic64_dec_return(&obj->handle_count);
    KASSERT(c >= 0);
    if (c == 0 && obj->is_exclusive)
        obj->exclusive_owned = false;
}

NTSTATUS ObReferenceObjectByName(PUNICODE_STRING path,
                                  struct nt_object_type *type,
                                  KPROCESSOR_MODE mode,
                                  ACCESS_MASK access,
                                  struct nt_object_header **out)
{
    (void)mode; (void)access;
    if (!path || !out) return STATUS_INVALID_PARAMETER;
    return ObpParsePath(path, 0, type, 0, out);
}

NTSTATUS ObOpenObjectByName(POBJECT_ATTRIBUTES attrs,
                             struct nt_object_type *type,
                             KPROCESSOR_MODE mode,
                             ACCESS_MASK access,
                             struct nt_eprocess *proc,
                             PHANDLE out_handle)
{
    struct nt_object_header *obj = NULL;
    NTSTATUS status;
    (void)mode;
    if (!attrs || !attrs->ObjectName || !out_handle)
        return STATUS_INVALID_PARAMETER;
    ULONG a = attrs->Attributes;
    if (type == &nt_symlink_type)
        a |= OBJ_OPENLINK;
    status = ObpParsePath(attrs->ObjectName, a, type, 0, &obj);
    if (!NT_SUCCESS(status))
        return status;
    /* OBJ_EXCLUSIVE handling: if object is exclusive and already has handles, fail */
    if (obj->is_exclusive && atomic64_read(&obj->handle_count) > 0) {
        ObDereferenceObject(obj);
        return STATUS_SHARING_VIOLATION;
    }
    if (obj->flags & OBJ_EXCLUSIVE) {
        /* First open of an exclusive object marks it */
        obj->is_exclusive = true;
    }
    struct nt_eprocess *target = proc ? proc : kshim_current_process();
    if (!target) { ObDereferenceObject(obj); return STATUS_INVALID_PARAMETER; }
    ULONG hattr = 0;
    if (a & OBJ_INHERIT) hattr |= OBJ_INHERIT;
    if (a & OBJ_PROTECT_FROM_CLOSE) hattr |= OBJ_PROTECT_FROM_CLOSE;
    status = ObCreateHandle(target, obj, access, hattr, out_handle);
    ObDereferenceObject(obj);
    return status;
}

#ifdef __KERNEL__
/* Copies a user OBJECT_ATTRIBUTES and its name into kernel storage. */
NTSTATUS ObCopyObjectAttributesFromUser(POBJECT_ATTRIBUTES uptr, OBJECT_ATTRIBUTES *k_oa, UNICODE_STRING *k_name, WCHAR *k_buf, size_t k_buf_bytes, POBJECT_ATTRIBUTES *out_oa)
{
    if (!uptr) { memset(k_oa,0,sizeof(*k_oa)); k_oa->Length=sizeof(OBJECT_ATTRIBUTES); *out_oa=NULL; return STATUS_SUCCESS; }
    if (!access_ok(uptr,sizeof(OBJECT_ATTRIBUTES))) return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(k_oa, uptr, sizeof(OBJECT_ATTRIBUTES))) return STATUS_ACCESS_VIOLATION;
    if (k_oa->Length != sizeof(OBJECT_ATTRIBUTES)) return STATUS_INVALID_PARAMETER;
    if (k_oa->ObjectName) {
        if (!access_ok(k_oa->ObjectName, sizeof(UNICODE_STRING))) return STATUS_ACCESS_VIOLATION;
        UNICODE_STRING tmp;
        if (copy_from_user(&tmp, k_oa->ObjectName, sizeof(tmp))) return STATUS_ACCESS_VIOLATION;
        if (tmp.Length) {
            if (!access_ok(tmp.Buffer, tmp.Length)) return STATUS_ACCESS_VIOLATION;
            if ((size_t)tmp.Length > k_buf_bytes) return STATUS_NAME_TOO_LONG;
            if (copy_from_user(k_buf, tmp.Buffer, tmp.Length)) return STATUS_ACCESS_VIOLATION;
            k_name->Buffer = k_buf; k_name->Length = tmp.Length; k_name->MaximumLength = tmp.Length;
            k_oa->ObjectName = k_name;
        } else k_oa->ObjectName=NULL;
    }
    *out_oa = k_oa;
    return STATUS_SUCCESS;
}
#endif /* __KERNEL__ */

NTSTATUS ObMakeTemporaryObject(struct nt_object_header *obj)
{
    if (!obj) return STATUS_INVALID_PARAMETER;
    /* Clear the flags first: removing from the directory may drop the last
     * reference and free obj. */
    obj->flags &= ~OBJ_PERMANENT;
    obj->is_permanent = false;
    if (obj->parent_dir)
        ObpRemoveFromDirectory(obj);
    return STATUS_SUCCESS;
}

NTSTATUS ObMakePermanentObject(struct nt_object_header *obj)
{
    if (!obj) return STATUS_INVALID_PARAMETER;
    obj->flags |= OBJ_PERMANENT;
    obj->is_permanent = true;
    return STATUS_SUCCESS;
}

struct nt_object_header *ObGetRootDirectory(void) { return &ob_root_hdr; }
struct nt_object_header *ObGetDosDevicesDirectory(void) { return ob_dosdevices_hdr; }
struct nt_object_header *ObGetBaseNamedObjects(void) { return ob_basenamedobjects_hdr; }
struct nt_object_header *ObGetDeviceDirectory(void) { return ob_device_hdr; }
struct nt_object_header *ObGetKnownDllsDirectory(void) { return ob_knowndlls_hdr; }
struct nt_object_header *ObGetSessionsDirectory(void) { return ob_sessions_hdr; }

const UNICODE_STRING *ObGetObjectName(struct nt_object_header *obj)
{
    if (!obj) return NULL;
    return &obj->name;
}

struct nt_object_type *ObGetObjectType(struct nt_object_header *obj)
{
    if (!obj) return NULL;
    return obj->type;
}

/* -------------------------------------------------------------------------- */
/* Process / thread helpers                                                   */
/* -------------------------------------------------------------------------- */

/* Process registry for cross-process duplicate (pidfd-style lookup) */
#ifndef __KERNEL__
static pthread_mutex_t g_ob_proc_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list_head g_ob_proc_list;
static pthread_once_t g_ob_proc_once = PTHREAD_ONCE_INIT;
static void g_ob_proc_init(void) { INIT_LIST_HEAD(&g_ob_proc_list); }
struct ob_proc_node { struct list_head link; struct nt_eprocess *proc; };
static void ob_ensure_proc_registry(void) { pthread_once(&g_ob_proc_once, g_ob_proc_init); }
static NTSTATUS ob_register_proc(struct nt_eprocess *p) {
    ob_ensure_proc_registry();
    struct ob_proc_node *n = (struct ob_proc_node*)kzalloc(sizeof(*n), GFP_KERNEL);
    if (!n) return STATUS_NO_MEMORY;
    n->proc = p;
    pthread_mutex_lock(&g_ob_proc_lock);
    list_add_tail(&n->link, &g_ob_proc_list);
    pthread_mutex_unlock(&g_ob_proc_lock);
    return STATUS_SUCCESS;
}
static void ob_unregister_proc(struct nt_eprocess *p) {
    ob_ensure_proc_registry();
    pthread_mutex_lock(&g_ob_proc_lock);
    struct list_head *pos, *tmp;
    pos = g_ob_proc_list.next;
    while (pos != &g_ob_proc_list) {
        tmp = pos->next;
        struct ob_proc_node *n = list_entry(pos, struct ob_proc_node, link);
        if (n->proc == p) { list_del(&n->link); kfree(n); break; }
        pos = tmp;
    }
    pthread_mutex_unlock(&g_ob_proc_lock);
}
struct nt_eprocess *ObFindProcessByHeader(struct nt_object_header *hdr) {
    ob_ensure_proc_registry();
    pthread_mutex_lock(&g_ob_proc_lock);
    struct list_head *pos;
    for (pos = g_ob_proc_list.next; pos != &g_ob_proc_list; pos = pos->next) {
        struct ob_proc_node *n = list_entry(pos, struct ob_proc_node, link);
        if (n->proc->obj == hdr) { pthread_mutex_unlock(&g_ob_proc_lock); return n->proc; }
    }
    pthread_mutex_unlock(&g_ob_proc_lock);
    return NULL;
}
struct nt_eprocess *ObFindProcessByPid(ULONG pid) {
    ob_ensure_proc_registry();
    pthread_mutex_lock(&g_ob_proc_lock);
    struct list_head *pos;
    for (pos = g_ob_proc_list.next; pos != &g_ob_proc_list; pos = pos->next) {
        struct ob_proc_node *n = list_entry(pos, struct ob_proc_node, link);
        if (n->proc->pid == pid) { pthread_mutex_unlock(&g_ob_proc_lock); return n->proc; }
    }
    pthread_mutex_unlock(&g_ob_proc_lock);
    return NULL;
}
/* Thread registry - exposed for proc_thread suspend process */
pthread_mutex_t g_ob_thr_lock = PTHREAD_MUTEX_INITIALIZER;
struct list_head g_ob_thr_list;
static pthread_once_t g_ob_thr_once = PTHREAD_ONCE_INIT;
static void g_ob_thr_init(void) { INIT_LIST_HEAD(&g_ob_thr_list); }
struct ob_thr_node { struct list_head link; struct nt_ethread *thr; };
static void ob_ensure_thr_registry(void) { pthread_once(&g_ob_thr_once, g_ob_thr_init); }
NTSTATUS ObRegisterThread(struct nt_ethread *t) {
    ob_ensure_thr_registry();
    struct ob_thr_node *n = (struct ob_thr_node*)kzalloc(sizeof(*n), GFP_KERNEL);
    if (!n) return STATUS_NO_MEMORY;
    n->thr = t;
    /* Idempotent: one node per thread. A repeat registration is a no-op. */
    bool dup = false;
    pthread_mutex_lock(&g_ob_thr_lock);
    struct list_head *pos;
    for (pos = g_ob_thr_list.next; pos != &g_ob_thr_list; pos = pos->next) {
        if (list_entry(pos, struct ob_thr_node, link)->thr == t) { dup = true; break; }
    }
    if (!dup) list_add_tail(&n->link, &g_ob_thr_list);
    pthread_mutex_unlock(&g_ob_thr_lock);
    if (dup) kfree(n);
    return STATUS_SUCCESS;
}
void ObUnregisterThread(struct nt_ethread *t) {
    ob_ensure_thr_registry();
    pthread_mutex_lock(&g_ob_thr_lock);
    struct list_head *pos, *tmp;
    pos = g_ob_thr_list.next;
    while (pos != &g_ob_thr_list) {
        tmp = pos->next;
        struct ob_thr_node *n = list_entry(pos, struct ob_thr_node, link);
        if (n->thr == t) { list_del(&n->link); kfree(n); break; }
        pos = tmp;
    }
    pthread_mutex_unlock(&g_ob_thr_lock);
}
struct nt_ethread *ObFindThreadByHeader(struct nt_object_header *hdr) {
    ob_ensure_thr_registry();
    pthread_mutex_lock(&g_ob_thr_lock);
    struct list_head *pos;
    for (pos = g_ob_thr_list.next; pos != &g_ob_thr_list; pos = pos->next) {
        struct ob_thr_node *n = list_entry(pos, struct ob_thr_node, link);
        if (n->thr->obj == hdr) { pthread_mutex_unlock(&g_ob_thr_lock); return n->thr; }
    }
    pthread_mutex_unlock(&g_ob_thr_lock);
    return NULL;
}
struct nt_ethread *ObFindThreadByTid(ULONG tid) {
    ob_ensure_thr_registry();
    pthread_mutex_lock(&g_ob_thr_lock);
    struct list_head *pos;
    for (pos = g_ob_thr_list.next; pos != &g_ob_thr_list; pos = pos->next) {
        struct ob_thr_node *n = list_entry(pos, struct ob_thr_node, link);
        if (n->thr->tid == tid) { pthread_mutex_unlock(&g_ob_thr_lock); return n->thr; }
    }
    pthread_mutex_unlock(&g_ob_thr_lock);
    return NULL;
}
void ObForEachThreadOfProcess(struct nt_eprocess *proc, void (*fn)(struct nt_ethread *thr, void *ctx), void *ctx) {
    ob_ensure_thr_registry();
    pthread_mutex_lock(&g_ob_thr_lock);
    struct list_head *pos;
    for (pos = g_ob_thr_list.next; pos != &g_ob_thr_list; pos = pos->next) {
        struct ob_thr_node *n = list_entry(pos, struct ob_thr_node, link);
        if (n->thr->process == proc) fn(n->thr, ctx);
    }
    pthread_mutex_unlock(&g_ob_thr_lock);
}
void ObSuspendProcessThreads(struct nt_eprocess *proc) {
    ob_ensure_thr_registry();
    pthread_mutex_lock(&g_ob_thr_lock);
    struct list_head *pos;
    for (pos = g_ob_thr_list.next; pos != &g_ob_thr_list; pos = pos->next) {
        struct ob_thr_node *n = list_entry(pos, struct ob_thr_node, link);
        if (n->thr->process == proc) {
            pthread_mutex_lock(&n->thr->lock);
            if (!n->thr->terminated) atomic_fetch_add(&n->thr->suspend_count, 1);
            pthread_mutex_unlock(&n->thr->lock);
        }
    }
    pthread_mutex_unlock(&g_ob_thr_lock);
}
void ObResumeProcessThreads(struct nt_eprocess *proc) {
    ob_ensure_thr_registry();
    pthread_mutex_lock(&g_ob_thr_lock);
    struct list_head *pos;
    for (pos = g_ob_thr_list.next; pos != &g_ob_thr_list; pos = pos->next) {
        struct ob_thr_node *n = list_entry(pos, struct ob_thr_node, link);
        if (n->thr->process == proc) {
            pthread_mutex_lock(&n->thr->lock);
            int cnt = atomic_load(&n->thr->suspend_count);
            if (cnt>0) {
                atomic_fetch_sub(&n->thr->suspend_count, 1);
                if (cnt==1) pthread_cond_broadcast(&n->thr->suspend_cond);
            }
            pthread_mutex_unlock(&n->thr->lock);
        }
    }
    pthread_mutex_unlock(&g_ob_thr_lock);
}
void ObTerminateProcessThreads(struct nt_eprocess *proc, NTSTATUS status) {
    ob_ensure_thr_registry();
    pthread_mutex_lock(&g_ob_thr_lock);
    struct list_head *pos;
    for (pos = g_ob_thr_list.next; pos != &g_ob_thr_list; pos = pos->next) {
        struct ob_thr_node *n = list_entry(pos, struct ob_thr_node, link);
        if (n->thr->process == proc) {
            pthread_mutex_lock(&n->thr->lock);
            n->thr->exit_status = status;
            n->thr->terminated = true;
            pthread_cond_broadcast(&n->thr->exit_cond);
            pthread_cond_broadcast(&n->thr->suspend_cond);
            pthread_mutex_unlock(&n->thr->lock);
            if (n->thr->pthread_valid) pthread_cancel(n->thr->pthread);
        }
    }
    pthread_mutex_unlock(&g_ob_thr_lock);
}
static void process_delete(struct nt_object_header *hdr) {
    struct nt_eprocess *p = ObFindProcessByHeader(hdr);
    if (p) p->obj = NULL;
}
static void thread_delete(struct nt_object_header *hdr) {
    struct nt_ethread *t = ObFindThreadByHeader(hdr);
    if (t) t->obj = NULL;
}
#else
/* Kernel process/thread registry - uses spinlock + list_head */
#include <linux/spinlock.h>
#include <linux/pid.h>
#include <linux/sched.h>
static DEFINE_SPINLOCK(g_ob_proc_lock);
static LIST_HEAD(g_ob_proc_list);
struct ob_proc_node { struct list_head link; struct nt_eprocess *proc; };
static NTSTATUS ob_register_proc(struct nt_eprocess *p) {
    struct ob_proc_node *n = kzalloc(sizeof(*n), GFP_KERNEL);
    if (!n) return STATUS_NO_MEMORY;
    n->proc = p;
    spin_lock(&g_ob_proc_lock);
    list_add_tail(&n->link, &g_ob_proc_list);
    spin_unlock(&g_ob_proc_lock);
    return STATUS_SUCCESS;
}
static void ob_unregister_proc(struct nt_eprocess *p) {
    struct ob_proc_node *n, *tmp;
    spin_lock(&g_ob_proc_lock);
    list_for_each_entry_safe(n, tmp, &g_ob_proc_list, link) {
        if (n->proc == p) { list_del(&n->link); kfree(n); break; }
    }
    spin_unlock(&g_ob_proc_lock);
}
struct nt_eprocess *ObFindProcessByHeader(struct nt_object_header *hdr) {
    struct ob_proc_node *n;
    struct nt_eprocess *ret = NULL;
    spin_lock(&g_ob_proc_lock);
    list_for_each_entry(n, &g_ob_proc_list, link) {
        if (n->proc->obj == hdr) { ret = n->proc; break; }
    }
    spin_unlock(&g_ob_proc_lock);
    return ret;
}
struct nt_eprocess *ObFindProcessByPid(ULONG pid) {
    struct ob_proc_node *n;
    struct nt_eprocess *ret = NULL;
    spin_lock(&g_ob_proc_lock);
    list_for_each_entry(n, &g_ob_proc_list, link) {
        if (n->proc->pid == pid) { ret = n->proc; break; }
    }
    spin_unlock(&g_ob_proc_lock);
    return ret;
}
static DEFINE_SPINLOCK(g_ob_thr_lock);
static LIST_HEAD(g_ob_thr_list);
struct ob_thr_node { struct list_head link; struct nt_ethread *thr; };
NTSTATUS ObRegisterThread(struct nt_ethread *t) {
    struct ob_thr_node *n = kzalloc(sizeof(*n), GFP_KERNEL);
    if (!n) return STATUS_NO_MEMORY;
    n->thr = t;
    /* Idempotent: one node per thread. A repeat registration is a no-op. */
    bool dup = false;
    struct list_head *pos;
    spin_lock(&g_ob_thr_lock);
    for (pos = g_ob_thr_list.next; pos != &g_ob_thr_list; pos = pos->next) {
        if (list_entry(pos, struct ob_thr_node, link)->thr == t) { dup = true; break; }
    }
    if (!dup) list_add_tail(&n->link, &g_ob_thr_list);
    spin_unlock(&g_ob_thr_lock);
    if (dup) { kfree(n); return STATUS_SUCCESS; }
    pr_err("ObRegisterThread: thr=%p tid=%lu obj=%p\n", t, t->tid, t->obj);
    return STATUS_SUCCESS;
}
void ObUnregisterThread(struct nt_ethread *t) {
    struct ob_thr_node *n, *tmp;
    spin_lock(&g_ob_thr_lock);
    list_for_each_entry_safe(n, tmp, &g_ob_thr_list, link) {
        if (n->thr == t) { list_del(&n->link); kfree(n); break; }
    }
    spin_unlock(&g_ob_thr_lock);
}
struct nt_ethread *ObFindThreadByHeader(struct nt_object_header *hdr) {
    struct ob_thr_node *n;
    struct nt_ethread *ret = NULL;
    spin_lock(&g_ob_thr_lock);
    list_for_each_entry(n, &g_ob_thr_list, link) {
        if (n->thr->obj == hdr) { ret = n->thr; break; }
    }
    spin_unlock(&g_ob_thr_lock);
    return ret;
}
struct nt_ethread *ObFindThreadByTid(ULONG tid) {
    struct ob_thr_node *n;
    struct nt_ethread *ret = NULL;
    spin_lock(&g_ob_thr_lock);
    list_for_each_entry(n, &g_ob_thr_list, link) {
        if (n->thr->tid == tid) { ret = n->thr; break; }
    }
    spin_unlock(&g_ob_thr_lock);
    return ret;
}
void ObForEachThreadOfProcess(struct nt_eprocess *proc, void (*fn)(struct nt_ethread *thr, void *ctx), void *ctx) {
    struct ob_thr_node *n;
    spin_lock(&g_ob_thr_lock);
    list_for_each_entry(n, &g_ob_thr_list, link) {
        if (n->thr->process == proc) fn(n->thr, ctx);
    }
    spin_unlock(&g_ob_thr_lock);
}
void ObSuspendProcessThreads(struct nt_eprocess *proc) {
    struct ob_thr_node *n;
    spin_lock(&g_ob_thr_lock);
    list_for_each_entry(n, &g_ob_thr_list, link) {
        if (n->thr->process == proc) {
            spin_lock(&n->thr->lock);
            if (!n->thr->terminated) {
                atomic_inc(&n->thr->suspend_count);
                if (atomic_read(&n->thr->suspend_count) == 1) {
                    struct task_struct *tsk = pid_task(find_vpid(n->thr->tid), PIDTYPE_PID);
                    if (tsk) set_tsk_thread_flag(tsk, TIF_NTSUSPEND);
                }
            }
            spin_unlock(&n->thr->lock);
        }
    }
    spin_unlock(&g_ob_thr_lock);
}
void ObResumeProcessThreads(struct nt_eprocess *proc) {
    struct ob_thr_node *n;
    spin_lock(&g_ob_thr_lock);
    list_for_each_entry(n, &g_ob_thr_list, link) {
        if (n->thr->process == proc) {
            spin_lock(&n->thr->lock);
            int cnt = atomic_read(&n->thr->suspend_count);
            if (cnt > 0) {
                atomic_dec(&n->thr->suspend_count);
                if (cnt == 1) {
                    struct task_struct *tsk = pid_task(find_vpid(n->thr->tid), PIDTYPE_PID);
                    if (tsk) clear_tsk_thread_flag(tsk, TIF_NTSUSPEND);
                    wake_up(&n->thr->suspend_wq);
                }
            }
            spin_unlock(&n->thr->lock);
        }
    }
    spin_unlock(&g_ob_thr_lock);
}
void ObTerminateProcessThreads(struct nt_eprocess *proc, NTSTATUS status) {
    struct ob_thr_node *n;
    spin_lock(&g_ob_thr_lock);
    list_for_each_entry(n, &g_ob_thr_list, link) {
        if (n->thr->process == proc) {
            spin_lock(&n->thr->lock);
            n->thr->exit_status = status;
            n->thr->terminated = true;
            wake_up(&n->thr->exit_wq);
            wake_up(&n->thr->suspend_wq);
            spin_unlock(&n->thr->lock);
            if (n->thr->sync_thread) {
                extern void nt_abandon_owned_mutants(struct nt_thread *);
                nt_abandon_owned_mutants(n->thr->sync_thread);
            } else {
                struct task_struct *tsk = pid_task(find_vpid(n->thr->tid), PIDTYPE_PID);
                if (tsk && tsk->nt_thread) {
                    extern void nt_abandon_owned_mutants(struct nt_thread *);
                    nt_abandon_owned_mutants((struct nt_thread *)tsk->nt_thread);
                }
            }
        }
    }
    spin_unlock(&g_ob_thr_lock);
}
/* Strong overrides for nt_current_* - used by proc_thread */
struct nt_eprocess *nt_current_eprocess(void) {
    struct nt_eprocess *p = ObFindProcessByPid((ULONG)current->tgid);
    if (p) return p;
    p = ObFindProcessByPid((ULONG)current->pid);
    if (p) return p;
    /* Lazy creation for initial PE process - binfmt_pe does not explicitly create eprocess */
    NTSTATUS st = ObCreateProcessObject(&p, (ULONG)current->tgid, current->comm);
    if (!NT_SUCCESS(st) || !p) return NULL;
    /* Try to capture PEB from mm (binfmt_pe sets start_data to peb) */
    if (current->mm) {
        p->peb = (struct _PEB *)(uintptr_t)current->mm->start_data;
        p->peb_base = (ULONG_PTR)p->peb;
    }
    return p;
}
struct nt_ethread *nt_current_ethread(void) {
    struct nt_ethread *t = ObFindThreadByTid((ULONG)current->pid);
    if (t) return t;
    struct nt_eprocess *proc = nt_current_eprocess();
    if (!proc) return NULL;
    NTSTATUS st = ObCreateThreadObject(&t, proc, (ULONG)current->pid);
    if (!NT_SUCCESS(st) || !t) return NULL;
    t->teb = (struct _TEB *)(uintptr_t)current->thread.gsbase;
    t->teb_base = (ULONG_PTR)t->teb;
    /* For lazy thread, TEB is already set up by binfmt_pe (user VA).
     * Don't try to write to user TEB via kernel pointer with SMAP enabled.
     * The StackBase etc are already correct from binfmt_pe.
     * Just set process and tid for handle lookup.
     */
    /* ObCreateThreadObject has already registered t. */
    return t;
}
__attribute__((weak)) struct nt_eprocess *nt_get_current_eprocess(void) { return nt_current_eprocess(); }
#endif

#ifndef __KERNEL__
static atomic_int g_proc_pid_seq = 1000;
static atomic_int g_thread_tid_seq = 2000;
#else
static atomic_t g_proc_pid_seq = ATOMIC_INIT(1000);
static atomic_t g_thread_tid_seq = ATOMIC_INIT(2000);
#endif

NTSTATUS ObCreateProcessObject(struct nt_eprocess **out, ULONG pid, const char *name)
{
    struct nt_eprocess *p;
    NTSTATUS st;
    if (!out) return STATUS_INVALID_PARAMETER;
    p = (struct nt_eprocess *)kzalloc(sizeof(*p), GFP_KERNEL);
    if (!p) return STATUS_NO_MEMORY;
    st = ObCreateHandleTable(p, &p->handle_table);
    if (!NT_SUCCESS(st)) { kfree(p); return st; }
    struct nt_object_header *hdr;
    void *body;
    OBJECT_ATTRIBUTES oa = {0};
    oa.Length = sizeof(oa);
    st = ObCreateObject(&nt_process_type, &oa, 0, &hdr, &body);
    if (!NT_SUCCESS(st)) { ObDestroyHandleTable(p->handle_table); kfree(p); return st; }
    p->obj = hdr;
#ifndef __KERNEL__
    if (pid == 0) pid = (ULONG)atomic_fetch_add(&g_proc_pid_seq, 1);
#else
    if (pid == 0) pid = (ULONG)atomic_inc_return(&g_proc_pid_seq);
#endif
    p->pid = pid;
    if (name) {
        size_t n = strlen(name);
        if (n >= sizeof(p->name)) n = sizeof(p->name)-1;
        memcpy(p->name, name, n);
        p->name[n]=0;
    }
    p->exit_status = 259; /* STILL_ACTIVE */
    p->terminated = false;
    p->peb_base = 0x7FF00000ULL + (p->pid * 0x10000ULL);
    p->affinity_mask = 0xFF;
    p->base_priority = 8;
    p->priority_class = 2;          /* PROCESS_PRIORITY_CLASS_NORMAL */
    p->priority_foreground = 0;
    p->default_hard_error_mode = 0;
    p->break_on_termination = 0;
    p->peb = NULL;
    p->peb_teb_reservation = NULL;
    p->image_path[0]=0;
    p->image_path_w[0]=0;
    p->image_path_wlen=0;
    /* default image */
    const char *defimg = "\\Device\\HarddiskVolume3\\Windows\\System32\\ntproc.exe";
    size_t dl = strlen(defimg);
    if (dl < sizeof(p->image_path)) {
        memcpy(p->image_path, defimg, dl+1);
        for (size_t i=0;i<dl;i++) p->image_path_w[i]=(WCHAR)(unsigned char)defimg[i];
        p->image_path_w[dl]=0;
        p->image_path_wlen = dl;
    }
#ifndef __KERNEL__
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->exit_cond, NULL);
    atomic_init(&p->ref_ext, 1);
#else
    spin_lock_init(&p->lock);
    init_waitqueue_head(&p->exit_wq);
    atomic_set(&p->ref_ext, 1);
#endif
    p->primary_token = token_create_default_primary();
    if (!p->primary_token) { ObDestroyProcessObject(p); return STATUS_NO_MEMORY; }
    st = ob_register_proc(p);
    if (!NT_SUCCESS(st)) { ObDestroyProcessObject(p); return st; }
    *out = p;
    return STATUS_SUCCESS;
}

NTSTATUS ObCreateThreadObject(struct nt_ethread **out, struct nt_eprocess *proc, ULONG tid)
{
    struct nt_ethread *t;
    NTSTATUS st;
    if (!out || !proc) return STATUS_INVALID_PARAMETER;
    t = (struct nt_ethread *)kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t) return STATUS_NO_MEMORY;
    struct nt_object_header *hdr;
    void *body;
    OBJECT_ATTRIBUTES oa={0};
    oa.Length=sizeof(oa);
    st = ObCreateObject(&nt_thread_type, &oa, 0, &hdr, &body);
    if (!NT_SUCCESS(st)) { kfree(t); return st; }
    t->obj = hdr;
    t->process = proc;
#ifndef __KERNEL__
    t->tid = tid ? tid : (ULONG)atomic_fetch_add(&g_thread_tid_seq, 1);
#else
    t->tid = tid ? tid : (ULONG)atomic_inc_return(&g_thread_tid_seq);
#endif
    t->exit_status = 259;
    t->terminated = false;
    t->teb = NULL;
    t->teb_base = 0;
    t->stack_base = NULL;
    t->stack_limit = NULL;
    t->stack_reserve = 0;
    t->stack_commit = 0;
    t->start_routine = NULL;
    t->start_arg = NULL;
    t->create_flags = 0;
    t->impersonation_token = NULL;
#ifndef __KERNEL__
    t->pthread_valid = false;
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->exit_cond, NULL);
    pthread_cond_init(&t->suspend_cond, NULL);
    atomic_init(&t->suspend_count, 0);
    t->gs_saved = NULL;
#else
    spin_lock_init(&t->lock);
    init_waitqueue_head(&t->exit_wq);
    init_waitqueue_head(&t->suspend_wq);
    atomic_set(&t->suspend_count, 0);
    t->tif_flags = 0;
#endif
    st = ObRegisterThread(t);
    if (!NT_SUCCESS(st)) { ObDestroyThreadObject(t); return st; }
    *out = t;
    return STATUS_SUCCESS;
}

void ObDestroyProcessObject(struct nt_eprocess *proc)
{
    if (!proc) return;
    /* Unregister in both builds: a freed node left on the list is a use-after-free
     * for every pid lookup that follows. */
    ob_unregister_proc(proc);
#ifndef __KERNEL__
    pthread_mutex_destroy(&proc->lock);
    pthread_cond_destroy(&proc->exit_cond);
    if (proc->peb) kfree(proc->peb);
    if (proc->peb_teb_reservation) kshim_munmap(proc->peb_teb_reservation, 8192*2);
#else
    if (proc->peb) kfree(proc->peb);
#endif
    if (proc->handle_table) ObDestroyHandleTable(proc->handle_table);
    if (proc->primary_token) {
        token_dereference(proc->primary_token);
        proc->primary_token = NULL;
    }
    if (proc->obj) ObDereferenceObject(proc->obj);
    kfree(proc);
}

void ObDestroyThreadObject(struct nt_ethread *thr)
{
    if (!thr) return;
    /* Unregister in both builds, as for processes above. */
    ObUnregisterThread(thr);
#ifndef __KERNEL__
    pthread_mutex_destroy(&thr->lock);
    pthread_cond_destroy(&thr->exit_cond);
    pthread_cond_destroy(&thr->suspend_cond);
    if (thr->teb) kfree(thr->teb);
    if (thr->stack_base) {
        size_t sz = thr->stack_reserve ? thr->stack_reserve : 0x100000;
        kshim_munmap(thr->stack_limit, sz);
    }
#endif
    if (thr->impersonation_token) {
        token_dereference(thr->impersonation_token);
        thr->impersonation_token = NULL;
    }
    if (thr->obj) ObDereferenceObject(thr->obj);
    kfree(thr);
}

/* -------------------------------------------------------------------------- */
/* Initialization                                                             */
/* -------------------------------------------------------------------------- */

static NTSTATUS ObpCreateWellKnownDirectory(const WCHAR *name, struct nt_object_header **out)
{
    size_t n = 0;
    while (name[n]) n++;
    UNICODE_STRING us;
    us.Buffer = (PWSTR)name;
    us.Length = (USHORT)(n * sizeof(WCHAR));
    us.MaximumLength = us.Length;
    OBJECT_ATTRIBUTES oa = {0};
    oa.Length = sizeof(oa);
    oa.Attributes = OBJ_PERMANENT;
    oa.ObjectName = &us;
    struct nt_object_header *hdr;
    struct nt_directory *dir;
    NTSTATUS st = ObCreateObject(&nt_directory_type, &oa, sizeof(*dir), &hdr, (void**)&dir);
    if (!NT_SUCCESS(st)) return st;
    INIT_LIST_HEAD(&dir->entries);
    rwlock_init(&dir->lock);
    /* Insert into root */
    st = ObpInsertDirectoryEntry(&ob_root_dir, &us, hdr);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(hdr);
        return st;
    }
    *out = hdr;
    return STATUS_SUCCESS;
}

NTSTATUS ObRegisterObjectType(struct nt_object_type *type)
{
    if (!type) return STATUS_INVALID_PARAMETER;
    /* Avoid double-add: check if already in list */
    struct list_head *pos;
    for (pos = ob_type_list.next; pos != &ob_type_list; pos = pos->next) {
        struct nt_object_type *t = list_entry(pos, struct nt_object_type, list);
        if (t == type) return STATUS_SUCCESS;
    }
    list_add(&type->list, &ob_type_list);
    return STATUS_SUCCESS;
}

#ifdef __KERNEL__
static int __init nt_ob_manager_init(void) { return ObInitObjectManager(); }
core_initcall(nt_ob_manager_init);
#endif

NTSTATUS ObInitObjectManager(void)
{
    NTSTATUS st;
    if (ob_initialized) return STATUS_SUCCESS;
    spin_lock_init(&ob_sd_lock);
    INIT_LIST_HEAD(&ob_type_list);
    list_add(&nt_directory_type.list, &ob_type_list);
    list_add(&nt_symlink_type.list, &ob_type_list);
    list_add(&nt_process_type.list, &ob_type_list);
    list_add(&nt_thread_type.list, &ob_type_list);
    list_add(&nt_token_type.list, &ob_type_list);
    list_add(&nt_test_type.list, &ob_type_list);
#ifndef __KERNEL__
    nt_process_type.ops.delete_obj = process_delete;
    nt_thread_type.ops.delete_obj = thread_delete;
#endif

    memset(&ob_root_hdr, 0, sizeof(ob_root_hdr));
    ob_root_hdr.type = &nt_directory_type;
    atomic64_set(&ob_root_hdr.ref_count, 1);
    atomic64_set(&ob_root_hdr.handle_count, 0);
    ob_root_hdr.body = &ob_root_dir;
    INIT_LIST_HEAD(&ob_root_hdr.type_link);
    INIT_LIST_HEAD(&ob_root_dir.entries);
    rwlock_init(&ob_root_dir.lock);
    ob_root_hdr.flags = OBJ_PERMANENT;
    ob_root_hdr.is_permanent = true;

    st = ObpCreateWellKnownDirectory((const WCHAR *)u"??", &ob_dosdevices_hdr);
    if (!NT_SUCCESS(st)) return st;
    st = ObpCreateWellKnownDirectory((const WCHAR *)u"Device", &ob_device_hdr);
    if (!NT_SUCCESS(st)) return st;
    st = ObpCreateWellKnownDirectory((const WCHAR *)u"BaseNamedObjects", &ob_basenamedobjects_hdr);
    if (!NT_SUCCESS(st)) return st;
    st = ObpCreateWellKnownDirectory((const WCHAR *)u"KnownDlls", &ob_knowndlls_hdr);
    if (!NT_SUCCESS(st)) return st;
    st = ObpCreateWellKnownDirectory((const WCHAR *)u"Sessions", &ob_sessions_hdr);
    if (!NT_SUCCESS(st)) return st;

    /* Create \Sessions\0 directory for session 0 */
    {
        UNICODE_STRING sess0;
        sess0.Buffer = (PWSTR)u"0";
        sess0.Length = (USHORT)(1*sizeof(WCHAR));
        sess0.MaximumLength = sess0.Length;
        struct nt_directory *sess_dir = (struct nt_directory*)nt_object_body(ob_sessions_hdr);
        struct nt_object_header *h0;
        struct nt_directory *d0;
        OBJECT_ATTRIBUTES oa2={0};
        oa2.Length=sizeof(oa2);
        oa2.Attributes=OBJ_PERMANENT;
        NTSTATUS s2 = ObCreateObject(&nt_directory_type, &oa2, sizeof(*d0), &h0, (void**)&d0);
        if (NT_SUCCESS(s2)) {
            INIT_LIST_HEAD(&d0->entries);
            rwlock_init(&d0->lock);
            if (NT_SUCCESS(ObpInsertDirectoryEntry(sess_dir, &sess0, h0))) {
                ObDereferenceObject(h0);
            } else {
                ObDereferenceObject(h0);
            }
        }
    }

    ob_initialized = true;
    return STATUS_SUCCESS;
}

void ObCleanupObjectManager(void)
{
    struct nt_dir_entry *de;
    struct list_head *pos, *n;
    if (!ob_initialized) return;
    /* Save local copies of well-known dir pointers before clearing root */
    struct nt_object_header *dos = ob_dosdevices_hdr;
    struct nt_object_header *dev = ob_device_hdr;
    struct nt_object_header *bno = ob_basenamedobjects_hdr;
    struct nt_object_header *kdll = ob_knowndlls_hdr;
    struct nt_object_header *sess = ob_sessions_hdr;

    write_lock(&ob_root_dir.lock);
    pos = ob_root_dir.entries.next;
    while (pos != &ob_root_dir.entries) {
        n = pos->next;
        de = list_entry(pos, struct nt_dir_entry, link);
        list_del(&de->link);
        nt_free_unicodestring(&de->name);
        struct nt_object_header *obj = de->obj;
        kfree(de);
        obj->parent_dir = NULL;
        obj->name.Buffer = NULL;
        obj->name.Length = 0;
        ObDereferenceObject(obj);
        pos = n;
    }
    write_unlock(&ob_root_dir.lock);

    /* Drop the creation references for well-known directories.
     * Each had ref 2 (create + directory). The loop above dropped the directory ref (1 -> 1).
     * Now drop the creation ref to reach 0 and trigger directory_delete which cleans children.
     */
    if (dos) ObDereferenceObject(dos);
    if (dev) ObDereferenceObject(dev);
    if (bno) ObDereferenceObject(bno);
    if (kdll) ObDereferenceObject(kdll);
    if (sess) ObDereferenceObject(sess);

    atomic64_set(&ob_root_hdr.ref_count, 1);
    ObDereferenceObject(&ob_root_hdr);
    /* Reinitialize root for potential reuse without re-init */
    atomic64_set(&ob_root_hdr.ref_count, 1);
    atomic64_set(&ob_root_hdr.handle_count, 0);
    INIT_LIST_HEAD(&ob_root_dir.entries);
    /* rwlock remains initialized */

    ob_dosdevices_hdr = NULL;
    ob_device_hdr = NULL;
    ob_basenamedobjects_hdr = NULL;
    ob_knowndlls_hdr = NULL;
    ob_sessions_hdr = NULL;
    ob_initialized = false;
#ifndef __KERNEL__
    /* Free any remaining process/thread objects (test leak cleanup) */
    ob_ensure_proc_registry();
    ob_ensure_thr_registry();
    pthread_mutex_lock(&g_ob_proc_lock);
    while (!list_empty(&g_ob_proc_list)) {
        struct ob_proc_node *n = list_first_entry(&g_ob_proc_list, struct ob_proc_node, link);
        list_del(&n->link);
        struct nt_eprocess *p = n->proc;
        kfree(n);
        if (p) {
            if (p->handle_table) ObDestroyHandleTable(p->handle_table);
            if (p->primary_token) { token_dereference(p->primary_token); p->primary_token = NULL; }
            if (p->obj) {
                /* avoid double delete via header delete callback */
                p->obj->type = NULL;
                ObDereferenceObject(p->obj);
            }
            pthread_mutex_destroy(&p->lock);
            pthread_cond_destroy(&p->exit_cond);
            if (p->peb) kfree(p->peb);
            kfree(p);
        }
    }
    pthread_mutex_unlock(&g_ob_proc_lock);
    pthread_mutex_lock(&g_ob_thr_lock);
    while (!list_empty(&g_ob_thr_list)) {
        struct ob_thr_node *n = list_first_entry(&g_ob_thr_list, struct ob_thr_node, link);
        list_del(&n->link);
        struct nt_ethread *t = n->thr;
        kfree(n);
        if (t) {
            if (t->impersonation_token) { token_dereference(t->impersonation_token); t->impersonation_token = NULL; }
            if (t->obj) {
                t->obj->type = NULL;
                ObDereferenceObject(t->obj);
            }
            pthread_mutex_destroy(&t->lock);
            pthread_cond_destroy(&t->exit_cond);
            pthread_cond_destroy(&t->suspend_cond);
            if (t->teb) kfree(t->teb);
            kfree(t);
        }
    }
    pthread_mutex_unlock(&g_ob_thr_lock);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
#endif
}
