/*
 * proc_create_args.h - copy-in and validation of the NtCreateUserProcess
 * PS_CREATE_INFO and PS_ATTRIBUTE_LIST arguments (x64 layout).
 *
 * The caller's structures live in user memory and can change at any time, so
 * each one is copied into a kernel buffer once, and only the copy is checked and
 * used. The copy function is the probe: in the kernel it is copy_from_user, which
 * checks the address range and returns the number of bytes it could not copy.
 * Nothing here reads user memory any other way.
 *
 * Sizes are checked before any arithmetic on them. A TotalLength is compared
 * with the maximum before it is used to compute an entry count, so a huge value
 * cannot wrap.
 *
 * This is validation only. Nothing here creates a process. NtCreateUserProcess
 * calls it, and then returns STATUS_NOT_IMPLEMENTED (see proc_thread.c).
 */

#ifndef NT_PROC_CREATE_ARGS_H
#define NT_PROC_CREATE_ARGS_H

#ifndef __KERNEL__
#include <stdbool.h>
#endif
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

/* -------------------------------------------------------------------------- */
/* Layout constants (x64)                                                     */
/* -------------------------------------------------------------------------- */

#define PCA_CREATE_INFO_SIZE     0x58UL   /* sizeof(PS_CREATE_INFO) */
#define PCA_CI_OFF_SIZE          0x00     /* SIZE_T Size */
#define PCA_CI_OFF_STATE         0x08     /* PS_CREATE_STATE State (ULONG) */
#define PCA_PS_CREATE_INITIAL    0UL      /* PsCreateInitialState, the only input state */

#define PCA_ATTRIBUTE_SIZE       0x20UL   /* sizeof(PS_ATTRIBUTE) */
#define PCA_ATTRIBUTE_LIST_HDR   0x08UL   /* offsetof(PS_ATTRIBUTE_LIST, Attributes) */
#define PCA_MAX_ATTRIBUTES       32UL
#define PCA_ATTR_LIST_MAX_BYTES  (PCA_ATTRIBUTE_LIST_HDR + PCA_MAX_ATTRIBUTES * PCA_ATTRIBUTE_SIZE)

/* Copies n bytes from user memory to kernel memory. Returns 0 on success and a
 * nonzero value if the read faulted. */
typedef int (*pca_copy_fn)(void *to, const void *from, size_t n);

/* A kernel copy of PS_CREATE_INFO. The fields are read from raw by the caller
 * or by PcaCreateInfoState/PcaCreateInfoSize. */
struct pca_create_info {
    unsigned char raw[PCA_CREATE_INFO_SIZE];
};

/* A kernel copy of PS_ATTRIBUTE_LIST: the entry count, and the bytes of the
 * whole list as they were copied. Entries are decoded by PcaAttributeAt. */
struct pca_attr_list {
    ULONG         count;
    unsigned char raw[PCA_ATTR_LIST_MAX_BYTES];
};

struct pca_attribute {
    ULONG_PTR attribute;
    ULONG_PTR size;
    ULONG_PTR value;
    ULONG_PTR return_length;
};

/* -------------------------------------------------------------------------- */
/* Little-endian field access on a kernel copy                                */
/* -------------------------------------------------------------------------- */

static inline ULONG_PTR pca_rd64(const unsigned char *b, ULONG off)
{
    ULONG_PTR v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[off + (ULONG)i];
    return v;
}

static inline ULONG pca_rd32(const unsigned char *b, ULONG off)
{
    return (ULONG)b[off] | ((ULONG)b[off + 1] << 8) |
           ((ULONG)b[off + 2] << 16) | ((ULONG)b[off + 3] << 24);
}

/* -------------------------------------------------------------------------- */
/* PS_CREATE_INFO                                                             */
/* -------------------------------------------------------------------------- */

/*
 * Copies PS_CREATE_INFO from user memory into out. Size must be exactly
 * PCA_CREATE_INFO_SIZE and State must be PsCreateInitialState. Returns
 * STATUS_ACCESS_VIOLATION if the read faults.
 */
static inline NTSTATUS PcaCopyInCreateInfo(pca_copy_fn copy, const void *user,
                                           struct pca_create_info *out)
{
    if (!copy || !user || !out) return STATUS_INVALID_PARAMETER;
    if (copy(out->raw, user, PCA_CREATE_INFO_SIZE) != 0) return STATUS_ACCESS_VIOLATION;
    if (pca_rd64(out->raw, PCA_CI_OFF_SIZE) != PCA_CREATE_INFO_SIZE)
        return STATUS_INFO_LENGTH_MISMATCH;
    if (pca_rd32(out->raw, PCA_CI_OFF_STATE) != PCA_PS_CREATE_INITIAL)
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* PS_ATTRIBUTE_LIST                                                          */
/* -------------------------------------------------------------------------- */

/*
 * Copies PS_ATTRIBUTE_LIST from user memory into out. A NULL user pointer is an
 * empty list. Otherwise the header is read first, its TotalLength is checked,
 * and then the whole list is read. The header in that second read must match
 * the first, or the list changed under the copy and is rejected.
 *
 *   TotalLength < header            STATUS_INFO_LENGTH_MISMATCH
 *   TotalLength > maximum           STATUS_INVALID_PARAMETER
 *   (TotalLength - header) % entry  STATUS_INFO_LENGTH_MISMATCH
 *   header changed between reads    STATUS_INVALID_PARAMETER
 *   a read faults                   STATUS_ACCESS_VIOLATION
 */
static inline NTSTATUS PcaCopyInAttributeList(pca_copy_fn copy, const void *user,
                                              struct pca_attr_list *out)
{
    unsigned char hdr[PCA_ATTRIBUTE_LIST_HDR];
    ULONG_PTR total, total2;

    if (!copy || !out) return STATUS_INVALID_PARAMETER;
    out->count = 0;
    if (!user) return STATUS_SUCCESS;

    if (copy(hdr, user, PCA_ATTRIBUTE_LIST_HDR) != 0) return STATUS_ACCESS_VIOLATION;
    total = pca_rd64(hdr, 0);

    /* Bounds first: nothing below multiplies or subtracts an unchecked value. */
    if (total < PCA_ATTRIBUTE_LIST_HDR) return STATUS_INFO_LENGTH_MISMATCH;
    if (total > PCA_ATTR_LIST_MAX_BYTES) return STATUS_INVALID_PARAMETER;
    if (((total - PCA_ATTRIBUTE_LIST_HDR) % PCA_ATTRIBUTE_SIZE) != 0)
        return STATUS_INFO_LENGTH_MISMATCH;

    if (copy(out->raw, user, (size_t)total) != 0) return STATUS_ACCESS_VIOLATION;
    total2 = pca_rd64(out->raw, 0);
    if (total2 != total) return STATUS_INVALID_PARAMETER;

    out->count = (ULONG)((total - PCA_ATTRIBUTE_LIST_HDR) / PCA_ATTRIBUTE_SIZE);
    return STATUS_SUCCESS;
}

/* Decodes entry i of a validated list. Out of range is STATUS_INVALID_PARAMETER. */
static inline NTSTATUS PcaAttributeAt(const struct pca_attr_list *list, ULONG i,
                                      struct pca_attribute *a)
{
    ULONG base;
    if (!list || !a) return STATUS_INVALID_PARAMETER;
    if (i >= list->count) return STATUS_INVALID_PARAMETER;
    base = (ULONG)PCA_ATTRIBUTE_LIST_HDR + i * (ULONG)PCA_ATTRIBUTE_SIZE;
    a->attribute     = pca_rd64(list->raw, base + 0);
    a->size          = pca_rd64(list->raw, base + 8);
    a->value         = pca_rd64(list->raw, base + 16);
    a->return_length = pca_rd64(list->raw, base + 24);
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Both arguments, as NtCreateUserProcess takes them                          */
/* -------------------------------------------------------------------------- */

/*
 * CreateInfo is required and is checked first. If it fails, the attribute list
 * is not read at all. AttributeList is optional. Returns the first failure.
 */
static inline NTSTATUS PcaCopyInUserProcessArgs(pca_copy_fn copy,
                                                const void *create_info_user,
                                                const void *attr_list_user,
                                                struct pca_create_info *ci,
                                                struct pca_attr_list *al)
{
    NTSTATUS st = PcaCopyInCreateInfo(copy, create_info_user, ci);
    if (!NT_SUCCESS(st)) return st;
    return PcaCopyInAttributeList(copy, attr_list_user, al);
}

#endif /* NT_PROC_CREATE_ARGS_H */
