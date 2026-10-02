/* pe_prot.c - PE section Characteristics -> page protection mapping.
 *
 * Implements the protection semantics documented in the PE spec and observed
 * in Windows loader behaviour, including shared/discardable/not-cached flags
 * and the rare write+execute combination.
 */
#include "pe_prot.h"

pe_status_t pe_prot_from_section(const IMAGE_SECTION_HEADER *sec,
                                 pe_page_protection_t *out)
{
    if (!sec || !out) return PE_ERR_BOUNDS;

    out->prot = PE_PROT_NONE;
    out->shared = (sec->Characteristics & IMAGE_SCN_MEM_SHARED) != 0;
    out->discardable = (sec->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) != 0;
    out->cached = (sec->Characteristics & IMAGE_SCN_MEM_NOT_CACHED) == 0;

    int exec  = (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    int read  = (sec->Characteristics & IMAGE_SCN_MEM_READ) != 0;
    int write = (sec->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;

    /* If no memory-protection bits are present, derive them from the content
     * class.  This matches the fallback Windows uses for older object files.   */
    if (!exec && !read && !write) {
        if (sec->Characteristics & IMAGE_SCN_CNT_CODE) {
            exec = 1;
            read = 1;
        } else if (sec->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) {
            read = 1;
            write = 1;
        } else if (sec->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) {
            /* Default data section is read/write unless the linker explicitly
             * cleared the write bit.  The memory bits are authoritative, but
             * when absent we must choose something; RW is the safer fallback. */
            read = 1;
            write = 1;
        } else {
            /* Unknown class: readable at minimum. */
            read = 1;
        }
    }

    if (read)  out->prot |= PE_PROT_READ;
    if (write) out->prot |= PE_PROT_WRITE;
    if (exec)  out->prot |= PE_PROT_EXEC;

    return PE_OK;
}
