/* pe_prot.h - convert PE section Characteristics to page protections.
 *
 * Used by both the kernel mapper (fs/binfmt_pe.c) and userspace tests.
 */
#ifndef PE_PROT_H
#define PE_PROT_H

#include "pe_format.h"
#include "pe_parse.h"

#define PE_PROT_NONE  0x00000000U
#define PE_PROT_READ  0x00000001U
#define PE_PROT_WRITE 0x00000002U
#define PE_PROT_EXEC  0x00000004U

typedef struct pe_page_protection {
    u32 prot;       /* bitmask of PE_PROT_*                            */
    int      shared;     /* IMAGE_SCN_MEM_SHARED                            */
    int      discardable;/* IMAGE_SCN_MEM_DISCARDABLE                       */
    int      cached;     /* 0 if IMAGE_SCN_MEM_NOT_CACHED, else 1           */
} pe_page_protection_t;

/* Derive page protection from a section header.  If no memory-protection bits
 * are set, infer them from the section content class the same way Windows does
 * for legacy images.                                                        */
pe_status_t pe_prot_from_section(const IMAGE_SECTION_HEADER *sec,
                                 pe_page_protection_t *out);

#ifndef __KERNEL__
#include <sys/mman.h>
/* PE_PROT_* bits to the PROT_* bits mmap() takes (userspace only). */
static inline unsigned long pe_prot_to_mmap(u32 prot)
{
    unsigned long p = 0;
    if (prot & PE_PROT_READ)  p |= PROT_READ;
    if (prot & PE_PROT_WRITE) p |= PROT_WRITE;
    if (prot & PE_PROT_EXEC)  p |= PROT_EXEC;
    return p;
}
#endif

#endif /* PE_PROT_H */
