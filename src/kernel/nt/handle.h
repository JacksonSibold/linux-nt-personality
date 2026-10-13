/*
 * handle.h - NT per-process handle table (kernel/nt/handle.c).
 *
 * This is a stand-alone handle space: not a wrapper around Linux file
 * descriptors.  The implementation is a three-level paginated table.
 * Handle values are multiples of four; the two low-order bits encode
 * PROTECT_FROM_CLOSE and INHERIT.
 *
 * Linux fd semantic differences are enumerated in handle.c.
 */

#ifndef NTHANDLE_H
#define NTHANDLE_H

#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Handle value flag bits (encoded in the low two bits of a HANDLE)           */
/* -------------------------------------------------------------------------- */

#define HANDLE_FLAG_PROTECT_FROM_CLOSE  0x00000001UL
#define HANDLE_FLAG_INHERIT             0x00000002UL
#define HANDLE_VALUE_MASK               (~0x3ULL)

#define NtCurrentProcess()              ((HANDLE)(LONG_PTR)-1)
#define NtCurrentThread()               ((HANDLE)(LONG_PTR)-2)

#define NT_CURRENT_PROCESS_HANDLE       ((HANDLE)((ULONG_PTR)-1))
#define NT_CURRENT_THREAD_HANDLE        ((HANDLE)((ULONG_PTR)-2)

/* -------------------------------------------------------------------------- */
/* Handle table                                                               */
/* -------------------------------------------------------------------------- */

#define HANDLE_L1_BITS        8
#define HANDLE_L2_BITS        8
#define HANDLE_L3_BITS        8
#define HANDLE_L1_SIZE        (1U << HANDLE_L1_BITS)
#define HANDLE_L2_SIZE        (1U << HANDLE_L2_BITS)
#define HANDLE_L3_SIZE        (1U << HANDLE_L3_BITS)
#define HANDLE_INDEX_BITS     (HANDLE_L1_BITS + HANDLE_L2_BITS + HANDLE_L3_BITS)
#define HANDLE_MAX_INDEX      ((1U << HANDLE_INDEX_BITS) - 1U)
#define HANDLE_INVALID_FREE   0xFFFFFFFFU

struct nt_handle_entry {
    struct nt_object_header *obj;
    ACCESS_MASK              granted_access;
    ULONG                    attr;
    bool                     free;
    u32                      next_free;
};

struct nt_handle_l3 {
    struct nt_handle_entry entries[HANDLE_L3_SIZE];
};

struct nt_handle_l2 {
    struct nt_handle_l3 *l3[HANDLE_L2_SIZE];
};

struct nt_handle_table {
    spinlock_t              lock;
    struct nt_eprocess     *owner;
    struct nt_handle_l2    *l1[HANDLE_L1_SIZE];
    u32                     first_free;
    u32                     last_free;
    u32                     free_count;
    u32                     high_water;
    u32                     num_handles;
};

/* Closed slots are reused FIFO and only after more than this many are free,
 * so a closed handle value is not reissued for at least this many allocations. */
#define HANDLE_QUARANTINE       4096U

struct nt_object_header;
struct nt_object_type;
struct nt_eprocess;

NTSTATUS ObCreateHandleTable(struct nt_eprocess *proc,
                              struct nt_handle_table **out_table);
void     ObDestroyHandleTable(struct nt_handle_table *table);

NTSTATUS ObCreateHandle(struct nt_eprocess *proc,
                         struct nt_object_header *obj,
                         ACCESS_MASK granted_access,
                         ULONG attributes,
                         PHANDLE out_handle);

NTSTATUS ObCloseHandle(struct nt_eprocess *proc, HANDLE handle);
NTSTATUS ObSetHandleFlags(struct nt_eprocess *proc, HANDLE handle,
                          bool inherit, bool protect_from_close);
/* Reads the inherit and protect-from-close flags of a handle of proc. */
NTSTATUS ObGetHandleFlags(struct nt_eprocess *proc, HANDLE handle,
                          bool *inherit, bool *protect_from_close);

NTSTATUS ObReferenceObjectByHandle(struct nt_eprocess *proc,
                                    HANDLE handle,
                                    struct nt_object_type *type,
                                    KPROCESSOR_MODE mode,
                                    ACCESS_MASK desired_access,
                                    struct nt_object_header **out_obj,
                                    ACCESS_MASK *granted_access);

NTSTATUS NtDuplicateObject(struct nt_eprocess *src_proc,
                            HANDLE src_handle,
                            struct nt_eprocess *dst_proc,
                            PHANDLE dst_handle,
                            ACCESS_MASK desired_access,
                            ULONG attributes,
                            ULONG options);

#define DUPLICATE_CLOSE_SOURCE          0x00000001UL
#define DUPLICATE_SAME_ACCESS           0x00000002UL

NTSTATUS ObInheritHandles(struct nt_eprocess *src, struct nt_eprocess *dst);

NTSTATUS ObpCreateHandleAtIndex(struct nt_handle_table *table,
                                 struct nt_object_header *obj,
                                 ACCESS_MASK granted_access,
                                 ULONG attributes,
                                 ULONG index,
                                 PHANDLE out_handle);

/* Debug helpers for tests */
ULONG    ObHandleTableCount(struct nt_handle_table *table);
bool     ObHandleIsValid(struct nt_eprocess *proc, HANDLE h);

extern struct nt_object_type nt_process_type;
extern struct nt_object_type nt_thread_type;

#ifdef __cplusplus
}
#endif

#endif /* NTHANDLE_H */
