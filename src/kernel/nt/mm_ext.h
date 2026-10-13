/*
 * mm_ext.h - extended-parameter types for the NtXxxEx memory services.
 *
 * MEM_EXTENDED_PARAMETER is a 16-byte record: a type in the low byte of the
 * first word, then a value union. ExtendedParameters is an array of them.
 *
 * The kernel service wrappers copy the array in once (ntmm_ext_copy_in) and
 * resolve each MEM_ADDRESS_REQUIREMENTS pointer into kernel storage. The native
 * functions in mm.c then read only kernel memory, so no user word is fetched
 * twice.
 */

#ifndef NT_MM_EXT_H
#define NT_MM_EXT_H

#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#define NT_IMAGE_MACHINE_AMD64          0x8664U

/* An array longer than this is STATUS_NOT_SUPPORTED. It bounds the kernel copy. */
#define MEM_EXT_MAX_COUNT               16U

typedef enum MEM_EXTENDED_PARAMETER_TYPE {
    MemExtendedParameterInvalidType        = 0,
    MemExtendedParameterAddressRequirements = 1,
    MemExtendedParameterNumaNode           = 2,
    MemExtendedParameterPartitionHandle    = 3,
    MemExtendedParameterUserPhysicalHandle = 4,
    MemExtendedParameterAttributeFlags     = 5,
    MemExtendedParameterImageMachine       = 6,
    MemExtendedParameterMax                = 7
} MEM_EXTENDED_PARAMETER_TYPE;

typedef struct MEM_EXTENDED_PARAMETER {
    struct {
        ULONG64 Type : 8;
        ULONG64 Reserved : 56;      /* must be zero */
    };
    union {
        ULONG64 ULong64;
        PVOID Pointer;
        SIZE_T Size;
        HANDLE Handle;
        ULONG ULong;
    };
} MEM_EXTENDED_PARAMETER;

typedef struct MEM_ADDRESS_REQUIREMENTS {
    PVOID LowestStartingAddress;
    PVOID HighestEndingAddress;     /* exclusive end; NULL means no upper bound */
    SIZE_T Alignment;               /* 0, or a power of two */
} MEM_ADDRESS_REQUIREMENTS;

/* Kernel-side copy of an extended-parameter array. params[i].Pointer for an
 * address-requirements entry points at reqs[i] after ntmm_ext_copy_in. */
struct nt_mem_ext_snapshot {
    ULONG count;
    MEM_EXTENDED_PARAMETER params[MEM_EXT_MAX_COUNT];
    MEM_ADDRESS_REQUIREMENTS reqs[MEM_EXT_MAX_COUNT];
};

/* Copies count entries from user (or, in the userspace build, plain memory)
 * into snap, once each. Returns STATUS_SUCCESS with snap->count set. A count of
 * zero copies nothing. A count above MEM_EXT_MAX_COUNT is STATUS_NOT_SUPPORTED,
 * a NULL array with a nonzero count is STATUS_INVALID_PARAMETER, and an unreadable
 * array or requirements record is STATUS_ACCESS_VIOLATION. */
NTSTATUS ntmm_ext_copy_in(const void *user, ULONG count, struct nt_mem_ext_snapshot *snap);

#endif /* NT_MM_EXT_H */
