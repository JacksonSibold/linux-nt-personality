/* drvhost_pe.c - maps a native .sys image, applies base relocations, and binds
 * its imports against the host export table.
 *
 * Uses the hardened parser in src/loader/pe_parse.c for every structure read.
 * Imports bind by name only: ordinal imports fail the load, because the host
 * export table has no ordinals.  Every unresolved name is printed before the
 * load fails with STATUS_PROCEDURE_NOT_FOUND, so one attempt lists them all.
 */

#include "pe_parse.h"
#include "drvhost_int.h"

#define DH_IMAGE_MAX (256u * 1024u * 1024u)
#define DH_PAGE 0x1000u
#define DH_SCN_EXECUTE 0x20000000u
#define DH_SCN_WRITE   0x80000000u
#define DH_SUBSYSTEM_NATIVE 1

typedef struct load_state {
    unsigned char *mem;
    size_t         img_size;
    uintptr_t      delta;
    int            reloc_error;
    const char    *dll_name;
    size_t         dll_len;
    uint32_t       iat_rva;
    uint32_t       slot;
    int            missing;
    int            dll_supported;
} load_state_t;

static int ieq_n(const char *a, size_t na, const char *b)
{
    size_t nb = strlen(b);
    if (na != nb) return 0;
    for (size_t i = 0; i < na; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

static pe_status_t reloc_cb(void *user, const pe_reloc_entry_t *e)
{
    load_state_t *st = user;
    if (e->type == 0) return PE_OK;                       /* IMAGE_REL_BASED_ABSOLUTE */
    size_t at = (size_t)e->page_rva + e->offset;
    if (e->type == 10) {                                  /* IMAGE_REL_BASED_DIR64 */
        if (at + 8 > st->img_size) { st->reloc_error = 1; return PE_ERR_BOUNDS; }
        uint64_t v;
        memcpy(&v, st->mem + at, 8);
        v += (uint64_t)st->delta;
        memcpy(st->mem + at, &v, 8);
        return PE_OK;
    }
    if (e->type == 3) {                                   /* IMAGE_REL_BASED_HIGHLOW */
        if (at + 4 > st->img_size) { st->reloc_error = 1; return PE_ERR_BOUNDS; }
        uint32_t v;
        memcpy(&v, st->mem + at, 4);
        v += (uint32_t)st->delta;
        memcpy(st->mem + at, &v, 4);
        return PE_OK;
    }
    st->reloc_error = 1;
    return PE_ERR_CORRUPT;
}

static pe_status_t import_dll_cb(void *user, const char *dll, size_t dll_len, u32 iat_rva, u32 ilt_rva)
{
    (void)ilt_rva;
    load_state_t *st = user;
    st->dll_name = dll;
    st->dll_len = dll_len;
    st->iat_rva = iat_rva;
    st->slot = 0;
    st->dll_supported = ieq_n(dll, dll_len, "ntoskrnl.exe") || ieq_n(dll, dll_len, "hal.dll");
    if (!st->dll_supported) {
        DbgPrint("drvhost: unsupported import module %.*s\n", (int)dll_len, dll);
    }
    return PE_OK;
}

static pe_status_t import_sym_cb(void *user, const pe_import_symbol_t *sym)
{
    load_state_t *st = user;
    uint32_t slot = st->slot++;
    if (!st->dll_supported) { st->missing++; return PE_OK; }
    if (sym->name == NULL) {
        DbgPrint("drvhost: ordinal import #%u from %.*s is not supported\n",
                 (unsigned)sym->ordinal, (int)st->dll_len, st->dll_name);
        st->missing++;
        return PE_OK;
    }
    /* Module-scoped lookup: HAL names must come from hal.dll, others from ntoskrnl. */
    const char *module = ieq_n(st->dll_name, st->dll_len, "hal.dll") ? "hal.dll" : "ntoskrnl.exe";
    void *addr = dh_host_export_module(module, sym->name, sym->name_len);
    uint64_t value = (uint64_t)(uintptr_t)addr;
    size_t at = (size_t)st->iat_rva + (size_t)slot * 8u;
    if (at + 8 > st->img_size) return PE_ERR_BOUNDS;
    if (!addr) {
        DbgPrint("drvhost: unresolved import %.*s!%.*s\n",
                 (int)st->dll_len, st->dll_name, (int)sym->name_len, sym->name);
        st->missing++;
    }
    memcpy(st->mem + at, &value, 8);
    return PE_OK;
}

static void free_image_pages(dh_image_t *img)
{
    if (img->mem) dh_unmap_image(img->mem, img->size);
    img->mem = NULL;
}

NTSTATUS dh_image_load(const char *path, dh_image_t **out)
{
    if (!path || !out) return STATUS_INVALID_PARAMETER;
    *out = NULL;

    unsigned char *file = NULL;
    size_t flen = 0;
    if (dh_read_file(path, &file, &flen) != 0) return STATUS_NO_SUCH_FILE;

    pe_context_t ctx;
    if (pe_context_init(&ctx, file, flen) != PE_OK || pe_validate(&ctx) != PE_OK) {
        DbgPrint("drvhost: %s is not a valid PE image (%s)\n", path, pe_validate_diag());
        dh_free_file(file);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    if (ctx.subsystem != DH_SUBSYSTEM_NATIVE) {
        DbgPrint("drvhost: %s subsystem is %u, expected NATIVE (1)\n", path, (unsigned)ctx.subsystem);
        dh_free_file(file);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    if (ctx.size_of_image == 0 || ctx.size_of_image > DH_IMAGE_MAX) {
        dh_free_file(file);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    size_t img_size = ((size_t)ctx.size_of_image + DH_PAGE - 1u) & ~((size_t)DH_PAGE - 1u);
    unsigned char *mem = dh_map_image(img_size);
    if (!mem) { dh_free_file(file); return STATUS_INSUFFICIENT_RESOURCES; }

    dh_image_t *img = dh_alloc(sizeof *img);
    if (!img) { dh_unmap_image(mem, img_size); dh_free_file(file); return STATUS_INSUFFICIENT_RESOURCES; }
    img->mem = mem;
    img->size = img_size;

    /* Headers, then each section's raw bytes at its virtual address.  The rest
     * of each section stays zero, as the loader must provide .bss. */
    size_t hdr = ctx.size_of_headers < flen ? ctx.size_of_headers : flen;
    if (hdr > img_size) hdr = img_size;
    memcpy(mem, file, hdr);

    uint16_t nsec = ctx.num_sections;
    for (uint16_t i = 0; i < nsec; i++) {
        pe_section_view_t sv;
        if (pe_section_get(&ctx, i, &sv) != PE_OK) goto corrupt;
        size_t va = sv.virtual_address;
        if (va >= img_size) goto corrupt;
        size_t copy = sv.file_size;
        if (sv.virtual_size && copy > sv.virtual_size) copy = sv.virtual_size;
        if (copy > img_size - va) copy = img_size - va;
        if ((size_t)sv.file_offset > flen || copy > flen - sv.file_offset) goto corrupt;
        if (copy) memcpy(mem + va, file + sv.file_offset, copy);
    }

    load_state_t st;
    memset(&st, 0, sizeof st);
    st.mem = mem;
    st.img_size = img_size;
    st.delta = (uintptr_t)mem - (uintptr_t)ctx.image_base;

    if (st.delta != 0) {
        if (pe_relocs_walk(&ctx, reloc_cb, &st) != PE_OK || st.reloc_error) goto corrupt;
    }

    if (pe_imports_walk(&ctx, import_dll_cb, import_sym_cb, &st) != PE_OK) goto corrupt;
    if (st.missing) {
        DbgPrint("drvhost: %s needs %d import(s) the host does not provide\n", path, st.missing);
        free_image_pages(img);
        dh_free(img);
        dh_free_file(file);
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    /* Protections per section; headers read-only. */
    dh_protect_rx(mem, DH_PAGE, 0, 0);
    for (uint16_t i = 0; i < nsec; i++) {
        pe_section_view_t sv;
        if (pe_section_get(&ctx, i, &sv) != PE_OK) continue;
        size_t va = sv.virtual_address;
        size_t span = sv.virtual_size ? sv.virtual_size : sv.file_size;
        if (va >= img_size || span == 0) continue;
        size_t start = va & ~((size_t)DH_PAGE - 1u);
        size_t end = (va + span + DH_PAGE - 1u) & ~((size_t)DH_PAGE - 1u);
        if (end > img_size) end = img_size;
        int exec = (sv.hdr.Characteristics & DH_SCN_EXECUTE) != 0;
        int write = (sv.hdr.Characteristics & DH_SCN_WRITE) != 0;
        if (dh_protect_rx(mem + start, end - start, exec, write) != 0) {
            free_image_pages(img);
            dh_free(img);
            dh_free_file(file);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    img->entry = ctx.entry_point ? (void *)(mem + ctx.entry_point) : NULL;
    if (ctx.entry_point >= img_size) img->entry = NULL;
    dh_free_file(file);
    *out = img;
    return STATUS_SUCCESS;

corrupt:
    DbgPrint("drvhost: %s is malformed (%s)\n", path, pe_validate_diag());
    free_image_pages(img);
    dh_free(img);
    dh_free_file(file);
    return STATUS_INVALID_IMAGE_FORMAT;
}

void dh_image_unload(dh_image_t *img)
{
    if (!img) return;
    free_image_pages(img);
    dh_free(img);
}
