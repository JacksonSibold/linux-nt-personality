/*
 * handle.c - NT per-process handle table.
 *
 * Three-level paginated table.  NOT a wrapper around Linux fds.
 *
 * Handle values
 * -------------
 * User-visible HANDLE values are multiples of four.  Low two bits encode
 * per-handle flags:
 *   bit 0 - HANDLE_FLAG_PROTECT_FROM_CLOSE
 *   bit 1 - HANDLE_FLAG_INHERIT
 * Remaining bits are index <<2.  No generation counter; stale handles can
 * alias a reused slot, exactly as on real NT.
 *
 * Table layout
 * ------------
 * Level 1: 256 pointers to L2 directories.
 * Level 2: 256 pointers to L3 pages.
 * Level 3: 256 entries of 16+ bytes.
 * Capacity: 2^24 indices (16M handles).  Pages allocated on demand.
 *
 * Free list: LIFO stack of free indices.  Recycles most-recently-freed.
 *
 * Linux fd vs NT handle differences (why we cannot reuse fds)
 * -----------------------------------------------------------
 * 1. Typed universality.  One HANDLE names files, events, mutants, sections,
 *    processes, threads, tokens, directories, symlinks, etc., via one
 *    reference+access primitive ObReferenceObjectByHandle.  fds name only open
 *    files; futexes/timerfd/signalfd are per-feature patches, not a typed
 *    object contract.
 *
 * 2. Sentinel values.  NT NULL handle == 0 and INVALID_HANDLE_VALUE == -1 both
 *    mean "no object", while fd 0 is valid stdin.  handle==fd identity is
 *    structurally wrong at the CreateFile boundary (returns -1 on failure).
 *
 * 3. Pseudo-handles.  GetCurrentProcess() == -1, GetCurrentThread() == -2 are
 *    resolved inside ObReferenceObjectByHandle, never stored in the table.
 *    CloseHandle(-1) must fail with STATUS_INVALID_HANDLE.  Linux has no fd
 *    pseudo-handle concept.
 *
 * 4. Per-handle granted access.  Every handle entry stores ACCESS_MASK.
 *    Use-time checks verify desired ⊆ granted; type mismatch -> 
 *    STATUS_OBJECT_TYPE_MISMATCH; generic mapping via type's GENERIC_MAPPING
 *    happens at open.  fds carry zero introspectable access; EBADF is the
 *    entire error vocabulary.
 *
 * 5. Inheritance is opt-in and value-preserving.  Only OBJ_INHERIT-flagged
 *    handles are copied to a child, and the child sees the SAME numeric value.
 *    Linux default is inverted (FD_CLOEXEC opts out, inheritance is default)
 *    and open/dup returns the lowest free int, so values are NOT preserved
 *    across fork/exec.
 *
 * 6. Cross-process duplication is a kernel primitive.  DuplicateHandle can
 *    copy/move a handle from one process's table to another without any
 *    cooperation between peers, authorized by PROCESS_DUP_HANDLE.  fd analog
 *    is SCM_RIGHTS over a unix socket, which requires cooperation and cannot
 *    enumerate a target's table.
 *
 * 7. Allocation/reuse policy.  NT handles are drawn from a per-process free
 *    list (LIFO, most-recently-freed first) and a high-water index; they are
 *    NOT the lowest free integer.  fd allocation (open/dup) always returns the
 *    lowest free int.
 *
 * 8. Close semantics + close-pending.  CloseHandle removes the table entry
 *    immediately; the object survives until last PointerCount drops.  A file
 *    object can enter close-pending on last handle close while other refs keep
 *    it alive (FILE_FLAG_DELETE_ON_CLOSE, pending I/O).  Closing an fd always
 *    drops the file reference immediately; no pending-close state.
 *
 * 9. Introspection.  NtQueryObject returns HandleCount, PointerCount, name,
 *    type, granted access per handle.  /proc/self/fd is a symlink listing of
 *    open files, not a typed object introspection contract.
 *
 * 10. No RLIMIT_NOFILE quota.  NT table grows to tens of thousands; services
 *     historically leaked 100k handles.  Imposing EMFILE on the handle table
 *     would break apps.  We use a very high quota (memory-limited) not a
 *     fixed fd limit.
 *
 * 11. dup2 vs DuplicateHandle.  dup2 atomically replaces a target fd with a
 *     source fd, sharing the same file description.  DuplicateHandle creates
 *     a new independent handle to the same object with its own granted access
 *     and attributes; it can also move (DUPLICATE_CLOSE_SOURCE) the source.
 */

#include "object.h"
#include "handle.h"
#ifdef __KERNEL__
struct nt_ethread *nt_current_ethread(void);
#endif

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static inline u32 handle_to_index(HANDLE handle)
{
    ULONG_PTR v = (ULONG_PTR)handle >> 2;
    /* Out-of-range values map to 0, which every caller rejects as invalid.
     * Without this, ht_entry() asserts on i1 >= HANDLE_L1_SIZE (BUG_ON in
     * kernel builds), and the u32 truncation aliases high-bit values onto
     * valid slots. */
    if (v > HANDLE_MAX_INDEX)
        return 0;
    return (u32)v;
}

static inline HANDLE index_to_handle(u32 idx, ULONG attr)
{
    return (HANDLE)(((ULONG_PTR)idx << 2) | (attr & 0x3UL));
}

static struct nt_handle_entry *ht_entry(struct nt_handle_table *table, u32 index, bool alloc)
{
    u32 i1 = index >> (HANDLE_L2_BITS + HANDLE_L3_BITS);
    u32 i2 = (index >> HANDLE_L3_BITS) & (HANDLE_L2_SIZE - 1);
    u32 i3 = index & (HANDLE_L3_SIZE - 1);
    struct nt_handle_l2 *l2;
    struct nt_handle_l3 *l3;
    KASSERT(i1 < HANDLE_L1_SIZE);
    l2 = table->l1[i1];
    if (!l2) {
        if (!alloc) return NULL;
        l2 = (struct nt_handle_l2 *)kzalloc(sizeof(*l2), GFP_ATOMIC);
        if (!l2) return NULL;
        table->l1[i1] = l2;
    }
    l3 = l2->l3[i2];
    if (!l3) {
        if (!alloc) return NULL;
        l3 = (struct nt_handle_l3 *)kzalloc(sizeof(*l3), GFP_ATOMIC);
        if (!l3) return NULL;
        /* New page: entries zeroed -> mark all as free? But we manage via high_water/free list, so just zero is fine.
         * Initialize free flags to true for not-yet-used entries beyond high_water? Instead we rely on high_water to allocate unused indices.
         * For simplicity, mark all entries in new page as free initially, but high_water will still allocate sequentially.
         */
        for (u32 k = 0; k < HANDLE_L3_SIZE; k++) {
            l3->entries[k].free = true;
            l3->entries[k].next_free = HANDLE_INVALID_FREE;
        }
        l2->l3[i2] = l3;
    }
    return &l3->entries[i3];
}

static NTSTATUS ht_alloc_index(struct nt_handle_table *table, u32 *out_index,
                                struct nt_handle_entry **out_entry)
{
    struct nt_handle_entry *e = NULL;
    u32 idx = 0;
    spin_lock(&table->lock);
    /* Reuse a closed slot only after HANDLE_QUARANTINE others have been freed (FIFO). */
    if (table->free_count > HANDLE_QUARANTINE && table->first_free != HANDLE_INVALID_FREE) {
        idx = table->first_free;
        e = ht_entry(table, idx, false);
        KASSERT(e && e->free);
        table->first_free = e->next_free;
        if (table->first_free == HANDLE_INVALID_FREE)
            table->last_free = HANDLE_INVALID_FREE;
        table->free_count--;
    } else {
        idx = table->high_water;
        if (idx > HANDLE_MAX_INDEX) {
            spin_unlock(&table->lock);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        e = ht_entry(table, idx, true);
        if (!e) {
            spin_unlock(&table->lock);
            return STATUS_NO_MEMORY;
        }
        table->high_water++;
    }
    KASSERT(e);
    e->free = false;
    e->obj = NULL;
    e->granted_access = 0;
    e->attr = 0;
    e->next_free = HANDLE_INVALID_FREE;
    table->num_handles++;
    spin_unlock(&table->lock);
    *out_index = idx;
    *out_entry = e;
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Table lifecycle                                                            */
/* -------------------------------------------------------------------------- */

NTSTATUS ObCreateHandleTable(struct nt_eprocess *proc, struct nt_handle_table **out_table)
{
    struct nt_handle_table *table;
    if (!proc || !out_table) return STATUS_INVALID_PARAMETER;
    table = (struct nt_handle_table *)kzalloc(sizeof(*table), GFP_KERNEL);
    if (!table) return STATUS_NO_MEMORY;
    spin_lock_init(&table->lock);
    table->owner = proc;
    table->first_free = HANDLE_INVALID_FREE;
    table->last_free = HANDLE_INVALID_FREE;
    table->free_count = 0;
    /* Windows starts handles around 0xC8 (index 50) for a fresh process.
     * We reserve indices 1..49 as already-used system handles (stdin/out, etc.)
     * so the first user handle is 0xC8, matching Windows shape.
     * Invariant: handles are multiples of 4 and increment by 4.
     * We match the exact starting value 0xC8 (index 50) to reduce delta vs golden;
     * if Windows ever changes base we still satisfy the invariant. */
    table->high_water = 50; /* reserve 0..49, first user handle = 50<<2 = 0xC8 */
    table->num_handles = 0;
    /* Mark index 0 as permanently free/reserved */
    struct nt_handle_entry *e0 = ht_entry(table, 0, true);
    if (e0) {
        e0->free = true;
        e0->next_free = HANDLE_INVALID_FREE;
        e0->obj = NULL;
    }
    *out_table = table;
    return STATUS_SUCCESS;
}

void ObDestroyHandleTable(struct nt_handle_table *table)
{
    u32 idx;
    if (!table) return;
    for (idx = 1; idx < table->high_water; idx++) {
        struct nt_handle_entry *e = ht_entry(table, idx, false);
        if (!e || e->free) continue;
        struct nt_object_header *obj = e->obj;
        if (obj) {
            HANDLE h = index_to_handle(idx, e->attr);
            ObpDecrementHandleCount(obj, h);
            if (obj->type && obj->type->ops.close)
                obj->type->ops.close(obj, h);
            ObDereferenceObject(obj);
        }
    }
    for (u32 i = 0; i < HANDLE_L1_SIZE; i++) {
        struct nt_handle_l2 *l2 = table->l1[i];
        if (!l2) continue;
        for (u32 j = 0; j < HANDLE_L2_SIZE; j++) {
            if (l2->l3[j]) kfree(l2->l3[j]);
        }
        kfree(l2);
    }
    kfree(table);
}

/* -------------------------------------------------------------------------- */
/* Handle creation / lookup / close                                           */
/* -------------------------------------------------------------------------- */

NTSTATUS ObCreateHandle(struct nt_eprocess *proc,
                         struct nt_object_header *obj,
                         ACCESS_MASK granted_access,
                         ULONG attributes,
                         PHANDLE out_handle)
{
    struct nt_handle_table *table;
    struct nt_handle_entry *e;
    u32 idx;
    NTSTATUS st;
    if (!proc || !obj || !out_handle) return STATUS_INVALID_PARAMETER;
    table = proc->handle_table;
    if (!table) return STATUS_INVALID_HANDLE;
    st = ht_alloc_index(table, &idx, &e);
    if (!NT_SUCCESS(st)) return st;
    KASSERT(e && !e->free);
    e->obj = obj;
    e->granted_access = granted_access;
    e->attr = 0;
    if (attributes & OBJ_INHERIT) e->attr |= HANDLE_FLAG_INHERIT;
    if (attributes & OBJ_PROTECT_FROM_CLOSE) e->attr |= HANDLE_FLAG_PROTECT_FROM_CLOSE;
    ObReferenceObject(obj);
    ObpIncrementHandleCount(obj);
    *out_handle = index_to_handle(idx, e->attr);
    return STATUS_SUCCESS;
}

NTSTATUS ObpCreateHandleAtIndex(struct nt_handle_table *table,
                                 struct nt_object_header *obj,
                                 ACCESS_MASK granted_access,
                                 ULONG attributes,
                                 ULONG index,
                                 PHANDLE out_handle)
{
    struct nt_handle_entry *e;
    if (!table || !obj) return STATUS_INVALID_PARAMETER;
    if (index == 0 || index > HANDLE_MAX_INDEX) return STATUS_INVALID_PARAMETER;
    spin_lock(&table->lock);
    /* Ensure page exists */
    e = ht_entry(table, index, true);
    if (!e) { spin_unlock(&table->lock); return STATUS_NO_MEMORY; }
    if (!e->free) { spin_unlock(&table->lock); return STATUS_OBJECT_NAME_COLLISION; }
    /* Remove from free list if present (it could be in free list) */
    /* For simplicity, if index < high_water and was free, we need to unlink it from free list.
     * Instead of unlinking, we check if this index is the first_free or somewhere.
     * Walk free list to remove.
     */
    if (table->first_free != HANDLE_INVALID_FREE) {
        u32 prev = HANDLE_INVALID_FREE;
        u32 cur = table->first_free;
        while (cur != HANDLE_INVALID_FREE) {
            struct nt_handle_entry *fe = ht_entry(table, cur, false);
            if (!fe) break;
            if (cur == index) {
                if (prev == HANDLE_INVALID_FREE) table->first_free = fe->next_free;
                else {
                    struct nt_handle_entry *pe = ht_entry(table, prev, false);
                    if (pe) pe->next_free = fe->next_free;
                }
                if (table->last_free == cur) table->last_free = prev;
                table->free_count--;
                break;
            }
            prev = cur;
            cur = fe->next_free;
        }
    }
    /* If index >= high_water, bump high_water */
    if (index >= table->high_water) {
        /* Ensure all gaps up to index are marked free? high_water is contiguous used region, so if we jump ahead we need to fill gap.
         * Simplest: set high_water = index+1 and assume gaps are allocatable via free list? But they are zeroed pages with free=true, so they are effectively free but not in list.
         * We'll handle by initializing gaps as free list entries.
         */
        for (u32 g = table->high_water; g < index; g++) {
            struct nt_handle_entry *ge = ht_entry(table, g, true);
            if (ge && ge->free) {
                ge->next_free = table->first_free;
                if (table->first_free == HANDLE_INVALID_FREE)
                    table->last_free = g;
                table->first_free = g;
                table->free_count++;
            }
        }
        table->high_water = index + 1;
    }
    e->free = false;
    e->obj = obj;
    e->granted_access = granted_access;
    e->attr = 0;
    if (attributes & OBJ_INHERIT) e->attr |= HANDLE_FLAG_INHERIT;
    if (attributes & OBJ_PROTECT_FROM_CLOSE) e->attr |= HANDLE_FLAG_PROTECT_FROM_CLOSE;
    e->next_free = HANDLE_INVALID_FREE;
    table->num_handles++;
    spin_unlock(&table->lock);
    ObReferenceObject(obj);
    ObpIncrementHandleCount(obj);
    if (out_handle) *out_handle = index_to_handle(index, e->attr);
    return STATUS_SUCCESS;
}

NTSTATUS ObCloseHandle(struct nt_eprocess *proc, HANDLE handle)
{
    struct nt_handle_table *table;
    struct nt_handle_entry *e;
    struct nt_object_header *obj = NULL;
    u32 idx;
    NTSTATUS st = STATUS_SUCCESS;

    /* Closing a pseudo handle is a successful no-op (probe_handles golden:
     * CloseHandle(GetCurrentProcess()) returns TRUE). */
    if (handle == NtCurrentProcess() || handle == NtCurrentThread())
        return STATUS_SUCCESS;
    if (!proc) return STATUS_INVALID_PARAMETER;
    table = proc->handle_table;
    if (!table) return STATUS_INVALID_HANDLE;
    if (handle == NULL) return STATUS_INVALID_HANDLE;
    idx = handle_to_index(handle);
    if (idx == 0) return STATUS_INVALID_HANDLE;

    spin_lock(&table->lock);
    e = ht_entry(table, idx, false);
    if (!e || e->free) { spin_unlock(&table->lock); return STATUS_INVALID_HANDLE; }
    if (e->attr & HANDLE_FLAG_PROTECT_FROM_CLOSE) { spin_unlock(&table->lock); return STATUS_HANDLE_NOT_CLOSABLE; }
    obj = e->obj;
    if (obj && obj->type && obj->type->ops.okay_to_close) {
        st = obj->type->ops.okay_to_close(obj, handle, UserMode);
        if (!NT_SUCCESS(st)) { spin_unlock(&table->lock); return st; }
    }
    /* Remove entry. Reuse is quarantined: the slot goes to the tail of a FIFO free
     * list and is handed out again only after HANDLE_QUARANTINE other slots have
     * been freed. A stale handle value can still alias after enough churn, which is
     * the W1 concern narrowed, not removed (see docs/decisions/0003). */
    e->free = true;
    e->obj = NULL;
    e->granted_access = 0;
    e->attr = 0;
    /* Append to the tail of the FIFO free list. ht_alloc_index pops from the head
     * only when free_count > HANDLE_QUARANTINE, so the value is not reissued soon. */
    e->next_free = HANDLE_INVALID_FREE;
    if (table->last_free != HANDLE_INVALID_FREE) {
        struct nt_handle_entry *te = ht_entry(table, table->last_free, false);
        KASSERT(te);
        te->next_free = idx;
    } else {
        table->first_free = idx;
    }
    table->last_free = idx;
    table->free_count++;
    table->num_handles--;
    spin_unlock(&table->lock);

    if (obj) {
        ObpDecrementHandleCount(obj, handle);
        if (obj->type && obj->type->ops.close)
            obj->type->ops.close(obj, handle);
        ObDereferenceObject(obj);
    }
    return STATUS_SUCCESS;
}

/* Set the Inherit and ProtectFromClose bits of an existing handle entry. */
NTSTATUS ObGetHandleFlags(struct nt_eprocess *proc, HANDLE handle,
                          bool *inherit, bool *protect_from_close)
{
    struct nt_handle_table *table;
    struct nt_handle_entry *e;
    u32 idx;

    if (handle == NtCurrentProcess() || handle == NtCurrentThread())
        return STATUS_INVALID_HANDLE;
    if (!proc || !inherit || !protect_from_close) return STATUS_INVALID_PARAMETER;
    table = proc->handle_table;
    if (!table || handle == NULL) return STATUS_INVALID_HANDLE;
    idx = handle_to_index(handle);
    if (idx == 0) return STATUS_INVALID_HANDLE;

    spin_lock(&table->lock);
    e = ht_entry(table, idx, false);
    if (!e || e->free) { spin_unlock(&table->lock); return STATUS_INVALID_HANDLE; }
    *inherit = (e->attr & HANDLE_FLAG_INHERIT) != 0;
    *protect_from_close = (e->attr & HANDLE_FLAG_PROTECT_FROM_CLOSE) != 0;
    spin_unlock(&table->lock);
    return STATUS_SUCCESS;
}

NTSTATUS ObSetHandleFlags(struct nt_eprocess *proc, HANDLE handle,
                          bool inherit, bool protect_from_close)
{
    struct nt_handle_table *table;
    struct nt_handle_entry *e;
    u32 idx;

    if (handle == NtCurrentProcess() || handle == NtCurrentThread())
        return STATUS_INVALID_HANDLE;
    if (!proc) return STATUS_INVALID_PARAMETER;
    table = proc->handle_table;
    if (!table || handle == NULL) return STATUS_INVALID_HANDLE;
    idx = handle_to_index(handle);
    if (idx == 0) return STATUS_INVALID_HANDLE;

    spin_lock(&table->lock);
    e = ht_entry(table, idx, false);
    if (!e || e->free) { spin_unlock(&table->lock); return STATUS_INVALID_HANDLE; }
    e->attr &= ~(HANDLE_FLAG_INHERIT | HANDLE_FLAG_PROTECT_FROM_CLOSE);
    if (inherit) e->attr |= HANDLE_FLAG_INHERIT;
    if (protect_from_close) e->attr |= HANDLE_FLAG_PROTECT_FROM_CLOSE;
    spin_unlock(&table->lock);
    return STATUS_SUCCESS;
}

NTSTATUS ObReferenceObjectByHandle(struct nt_eprocess *proc,
                                    HANDLE handle,
                                    struct nt_object_type *type,
                                    KPROCESSOR_MODE mode,
                                    ACCESS_MASK desired_access,
                                    struct nt_object_header **out_obj,
                                    ACCESS_MASK *granted_access)
{
    struct nt_handle_table *table;
    struct nt_handle_entry *e;
    struct nt_object_header *obj;
    u32 idx;
    (void)mode;
    if (!proc || !out_obj) return STATUS_INVALID_PARAMETER;
    *out_obj = NULL;
    if (handle == NtCurrentProcess()) {
        obj = proc->obj;
        if (!obj) return STATUS_INVALID_HANDLE;
        if (type && obj->type != type) return STATUS_OBJECT_TYPE_MISMATCH;
        ObReferenceObject(obj);
        *out_obj = obj;
        if (granted_access) *granted_access = 0xFFFFFFFFUL;
        return STATUS_SUCCESS;
    }
    if (handle == NtCurrentThread()) {
#ifdef __KERNEL__
        struct nt_ethread *thr = nt_current_ethread();
#else
        struct nt_ethread *thr = kshim_current_thread();
#endif
        if (!thr || !thr->obj) return STATUS_INVALID_HANDLE;
        obj = thr->obj;
        if (type && obj->type != type) return STATUS_OBJECT_TYPE_MISMATCH;
        ObReferenceObject(obj);
        *out_obj = obj;
        if (granted_access) *granted_access = 0xFFFFFFFFUL;
        return STATUS_SUCCESS;
    }
    if (handle == NULL) return STATUS_INVALID_HANDLE;
    table = proc->handle_table;
    if (!table) return STATUS_INVALID_HANDLE;
    idx = handle_to_index(handle);
    if (idx == 0) return STATUS_INVALID_HANDLE;
    spin_lock(&table->lock);
    e = ht_entry(table, idx, false);
    if (!e || e->free) { spin_unlock(&table->lock); return STATUS_INVALID_HANDLE; }
    obj = e->obj;
    if (!obj) { spin_unlock(&table->lock); return STATUS_INVALID_HANDLE; }
    if (type && obj->type != type) { spin_unlock(&table->lock); return STATUS_OBJECT_TYPE_MISMATCH; }
    if ((desired_access & ~e->granted_access) != 0) { spin_unlock(&table->lock); return STATUS_ACCESS_DENIED; }
    ObReferenceObject(obj);
    if (granted_access) *granted_access = e->granted_access;
    spin_unlock(&table->lock);
    *out_obj = obj;
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Duplication and inheritance                                                */
/* -------------------------------------------------------------------------- */

/* Maps generic rights to the type's specific rights (Windows applies the type's
 * GENERIC_MAPPING before the granted-access check). Types without a mapping
 * keep the access as given. Checked against the probe_handles golden:
 * DuplicateHandle(GENERIC_READ) on a file grants FILE_GENERIC_READ (0x120089). */
#define OBP_GENERIC_READ    0x80000000UL
#define OBP_GENERIC_WRITE   0x40000000UL
#define OBP_GENERIC_EXECUTE 0x20000000UL
#define OBP_GENERIC_ALL     0x10000000UL

static ACCESS_MASK ObpMapGenericAccess(const struct nt_object_type *type, ACCESS_MASK access)
{
    const struct _GENERIC_MAPPING *gm = type ? type->generic_map : NULL;
    ACCESS_MASK out;
    if (!gm) return access;
    out = access & 0x0FFFFFFFUL;
    if (access & OBP_GENERIC_READ)    out |= gm->GenericRead;
    if (access & OBP_GENERIC_WRITE)   out |= gm->GenericWrite;
    if (access & OBP_GENERIC_EXECUTE) out |= gm->GenericExecute;
    if (access & OBP_GENERIC_ALL)     out |= gm->GenericAll;
    return out;
}

NTSTATUS NtDuplicateObject(struct nt_eprocess *src_proc,
                            HANDLE src_handle,
                            struct nt_eprocess *dst_proc,
                            PHANDLE dst_handle,
                            ACCESS_MASK desired_access,
                            ULONG attributes,
                            ULONG options)
{
    struct nt_handle_table *src, *dst;
    struct nt_handle_entry *se = NULL;
    struct nt_object_header *obj = NULL;
    ACCESS_MASK new_access = 0;
    u32 src_idx;
    NTSTATUS st;

    if (!src_proc || !dst_proc || !dst_handle) return STATUS_INVALID_PARAMETER;
    *dst_handle = NULL;
    src = src_proc->handle_table;
    dst = dst_proc->handle_table;
    if (!src || !dst) return STATUS_INVALID_HANDLE;
    if (src_handle == NtCurrentProcess() || src_handle == NtCurrentThread())
        return STATUS_INVALID_HANDLE;
    if (src_handle == NULL) return STATUS_INVALID_HANDLE;
    src_idx = handle_to_index(src_handle);
    if (src_idx == 0) return STATUS_INVALID_HANDLE;

    /* Lock both tables in address order to avoid deadlock */
    struct nt_handle_table *first = src < dst ? src : dst;
    struct nt_handle_table *second = src < dst ? dst : src;
    spin_lock(&first->lock);
    if (second != first) spin_lock(&second->lock);

    se = ht_entry(src, src_idx, false);
    if (!se || se->free) {
        if (second != first) spin_unlock(&second->lock);
        spin_unlock(&first->lock);
        return STATUS_INVALID_HANDLE;
    }
    obj = se->obj;
    ObReferenceObject(obj);
    if (options & DUPLICATE_SAME_ACCESS) {
        new_access = se->granted_access;
    } else {
        desired_access = ObpMapGenericAccess(obj->type, desired_access);
        if ((desired_access & ~se->granted_access) != 0) {
            ObDereferenceObject(obj);
            if (second != first) spin_unlock(&second->lock);
            spin_unlock(&first->lock);
            return STATUS_ACCESS_DENIED;
        }
        new_access = desired_access;
        /* If desired_access is 0, treat as same as source (common call pattern) */
        if (desired_access == 0 && !(options & DUPLICATE_SAME_ACCESS)) {
            new_access = se->granted_access;
        }
    }
    if (second != first) spin_unlock(&second->lock);
    spin_unlock(&first->lock);

    st = ObCreateHandle(dst_proc, obj, new_access, attributes, dst_handle);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(obj);
        return st;
    }
    if (options & DUPLICATE_CLOSE_SOURCE) {
        /* If the source refuses to close (e.g. PROTECT_FROM_CLOSE), undo the duplicate
         * so the caller gets an error and no new handle. */
        st = ObCloseHandle(src_proc, src_handle);
        if (!NT_SUCCESS(st)) {
            ObCloseHandle(dst_proc, *dst_handle);
            *dst_handle = NULL;
            ObDereferenceObject(obj);
            return st;
        }
    }
    ObDereferenceObject(obj); /* extra ref from above */
    return STATUS_SUCCESS;
}

/* Undoes one copy made by ObInheritHandleTable: frees the slot in dst and
 * releases the reference and handle count the copy took. Called with the source
 * table locked. The source entry still holds its own reference, so the object
 * cannot be freed here, and no type callback runs: the copy was never visible. */
static void ht_undo_copy(struct nt_handle_table *dst, u32 idx)
{
    struct nt_handle_entry *e;
    struct nt_object_header *obj;

    spin_lock(&dst->lock);
    e = ht_entry(dst, idx, false);
    KASSERT(e && !e->free);
    obj = e->obj;
    e->free = true;
    e->obj = NULL;
    e->granted_access = 0;
    e->attr = 0;
    e->next_free = HANDLE_INVALID_FREE;
    if (dst->last_free != HANDLE_INVALID_FREE) {
        struct nt_handle_entry *te = ht_entry(dst, dst->last_free, false);
        KASSERT(te);
        te->next_free = idx;
    } else {
        dst->first_free = idx;
    }
    dst->last_free = idx;
    dst->free_count++;
    dst->num_handles--;
    spin_unlock(&dst->lock);

    ObpDecrementHandleCount(obj, NULL);
    ObDereferenceObject(obj);
}

/*
 * Copies every inheritable entry of src into dst at the same index, with the
 * same granted access, so the child sees the parent's handle values. Only
 * HANDLE_FLAG_INHERIT is carried over: PROTECT_FROM_CLOSE is not. Each copy takes
 * one reference and one handle count, as ObCreateHandle does. *copied (optional)
 * receives the number of copies on success.
 *
 * All or nothing: if a copy fails (a slot in dst is already in use, or a page
 * cannot be allocated), the copies made by this call are undone and the error is
 * returned. src stays locked for the whole walk, so the set of inheritable
 * entries cannot change under the undo. The lock order is src, then dst. dst
 * must not be locked by anyone else, which holds for a table being built for a
 * new process.
 */
NTSTATUS ObInheritHandleTable(struct nt_handle_table *src, struct nt_handle_table *dst,
                              u32 *copied)
{
    NTSTATUS st = STATUS_SUCCESS;
    u32 idx, done = 0, fail_idx = 0;

    if (copied) *copied = 0;
    if (!src || !dst || src == dst) return STATUS_INVALID_PARAMETER;

    spin_lock(&src->lock);
    for (idx = 1; idx < src->high_water; idx++) {
        struct nt_handle_entry *e = ht_entry(src, idx, false);
        if (!e || e->free || !(e->attr & HANDLE_FLAG_INHERIT))
            continue;
        /* ObpCreateHandleAtIndex takes the object reference and handle count. */
        st = ObpCreateHandleAtIndex(dst, e->obj, e->granted_access, OBJ_INHERIT, idx, NULL);
        if (!NT_SUCCESS(st)) {
            fail_idx = idx;
            break;
        }
        done++;
    }

    if (!NT_SUCCESS(st)) {
        for (u32 i = 1; i < fail_idx; i++) {
            struct nt_handle_entry *e = ht_entry(src, i, false);
            if (e && !e->free && (e->attr & HANDLE_FLAG_INHERIT))
                ht_undo_copy(dst, i);
        }
    }
    spin_unlock(&src->lock);

    if (NT_SUCCESS(st) && copied) *copied = done;
    return st;
}

NTSTATUS ObInheritHandles(struct nt_eprocess *src, struct nt_eprocess *dst)
{
    if (!src || !dst) return STATUS_INVALID_PARAMETER;
    if (!src->handle_table || !dst->handle_table) return STATUS_INVALID_HANDLE;
    return ObInheritHandleTable(src->handle_table, dst->handle_table, NULL);
}

ULONG ObHandleTableCount(struct nt_handle_table *table)
{
    if (!table) return 0;
    return table->num_handles;
}

bool ObHandleIsValid(struct nt_eprocess *proc, HANDLE h)
{
    if (!proc || !proc->handle_table || h==NULL) return false;
    u32 idx = handle_to_index(h);
    if (idx==0) return false;
    spin_lock(&proc->handle_table->lock);
    struct nt_handle_entry *e = ht_entry(proc->handle_table, idx, false);
    bool ok = e && !e->free;
    spin_unlock(&proc->handle_table->lock);
    return ok;
}
