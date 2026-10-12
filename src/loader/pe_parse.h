/* pe_parse.h - hardened PE32+ parser for untrusted input.
 *
 * The parser operates on a caller-provided byte buffer.  No heap allocations
 * are performed; every field access is bounds-checked against the supplied
 * file size.  This header compiles in both kernel and userspace contexts.
 */
#ifndef PE_PARSE_H
#define PE_PARSE_H

#include "pe_format.h"

#ifdef __KERNEL__
#  include <linux/types.h>
#else
#  include <stddef.h>
#endif

/* -------------------------------------------------------------------------- */
/* Status codes                                                               */
/* -------------------------------------------------------------------------- */
typedef enum pe_status {
    PE_OK = 0,
    PE_ERR_TRUNCATED,     /* buffer too small for requested structure          */
    PE_ERR_MZ,            /* missing or bad DOS 'MZ' signature                 */
    PE_ERR_PE_SIG,        /* missing or bad 'PE\0\0' signature                */
    PE_ERR_MACHINE,       /* unsupported machine type (only AMD64 in scope)    */
    PE_ERR_MAGIC,         /* optional header magic is not PE32+ (0x20B)        */
    PE_ERR_BOUNDS,        /* pointer/offset arithmetic out of buffer           */
    PE_ERR_SECTIONS,      /* section table malformed or inconsistent           */
    PE_ERR_DIRECTORY,     /* data directory entry invalid                      */
    PE_ERR_CORRUPT,       /* inconsistent PE content                           */
    PE_ERR_NOT_FOUND,     /* requested RVA/section/name not found              */
} pe_status_t;

/* -------------------------------------------------------------------------- */
/* Input limits (docs/review/loader.md, L-1 and L-2)                          */
/* -------------------------------------------------------------------------- */
/* Import and export names and forwarder strings may be at most this many
 * bytes, not counting the terminating NUL.  Longer names, and names with no NUL
 * before end of file, fail with PE_ERR_CORRUPT. */
#define PE_MAX_SYMBOL_NAME 4096

/* -------------------------------------------------------------------------- */
/* Context                                                                    */
/* -------------------------------------------------------------------------- */
typedef struct pe_context {
    const u8 *data;
    size_t         size;

    /* Offsets into data; set by pe_validate()/pe_map_headers().  Client code
     * should use the API below rather than dereferencing these directly.       */
    u32  dos_off;
    u32  nt_off;
    u32  section_table_off;

    u16  num_sections;
    u16  optional_magic;
    u32  section_alignment;
    u32  file_alignment;
    u64  image_base;
    u32  size_of_image;
    u32  size_of_headers;
    u32  entry_point;
    u16  subsystem;
    u16  dll_characteristics;
    u32  num_data_directories;
} pe_context_t;

/* -------------------------------------------------------------------------- */
/* Views / descriptors returned by walk helpers                               */
/* -------------------------------------------------------------------------- */
typedef struct pe_section_view {
    IMAGE_SECTION_HEADER hdr;
    u32 index;
    u32 file_offset;
    u32 file_size;      /* bytes actually present in file                  */
    u32 virtual_address;
    u32 virtual_size;
} pe_section_view_t;

typedef struct pe_data_directory {
    u32 index;
    u32 rva;
    u32 size;
} pe_data_directory_t;

typedef struct pe_import_symbol {
    u64     ordinal_flag;
    u16     ordinal;
    u32     hint;
    const char  *name;       /* NULL for ordinal imports                        */
    size_t       name_len;
} pe_import_symbol_t;

typedef struct pe_export_entry {
    u32     ordinal;
    u32     rva;
    const char  *name;       /* NULL if exported by ordinal only                */
    size_t       name_len;
    const char  *forwarder;  /* NULL if not a forwarded export                  */
    size_t       forwarder_len;
} pe_export_entry_t;

typedef struct pe_reloc_entry {
    u32 page_rva;
    u16 type;
    u16 offset;
} pe_reloc_entry_t;

typedef struct pe_runtime_function {
    u32 begin;
    u32 end;
    u32 unwind;
} pe_runtime_function_t;

/* -------------------------------------------------------------------------- */
/* Low-level readers (also used by the loader)                                */
/* -------------------------------------------------------------------------- */
pe_status_t pe_check_read(const pe_context_t *ctx,
                          u32 off,
                          size_t n);

u16 pe_load_u16(const u8 *p);
u32 pe_load_u32(const u8 *p);
u64 pe_load_u64(const u8 *p);

u16 pe_file_u16(const pe_context_t *ctx, u32 off);
u32 pe_file_u32(const pe_context_t *ctx, u32 off);

pe_status_t pe_read_asciiz(const pe_context_t *ctx,
                           u32 off,
                           const char **out,
                           size_t *len);

pe_status_t pe_read_thunk_data64(const pe_context_t *ctx,
                                 u32 off,
                                 IMAGE_THUNK_DATA64 *out);

pe_status_t pe_read_export_directory(const pe_context_t *ctx,
                                     u32 off,
                                     IMAGE_EXPORT_DIRECTORY *out);

pe_status_t pe_read_base_relocation(const pe_context_t *ctx,
                                     u32 off,
                                     IMAGE_BASE_RELOCATION *out);

pe_status_t pe_read_import_descriptor(const pe_context_t *ctx,
                                     u32 off,
                                     IMAGE_IMPORT_DESCRIPTOR *out);

pe_status_t pe_read_tls_directory64(const pe_context_t *ctx,
                                   u32 off,
                                   IMAGE_TLS_DIRECTORY64 *out);

/* -------------------------------------------------------------------------- */
/* Core parser                                                                */
/* -------------------------------------------------------------------------- */
pe_status_t pe_context_init(pe_context_t *ctx,
                            const u8 *data,
                            size_t size);

pe_status_t pe_validate(pe_context_t *ctx);

/* Diagnostic: human-readable reason for the last pe_validate() failure.
 * Returns "" if the last validation succeeded.  Valid until the next
 * pe_validate() call. */
const char *pe_validate_diag(void);

/* Recompute internal pointers after init or after a context copy.  Called by
 * pe_validate(); may be called alone if validation was already performed.      */
pe_status_t pe_map_headers(pe_context_t *ctx);

/* -------------------------------------------------------------------------- */
/* Section table / RVA translation                                            */
/* -------------------------------------------------------------------------- */
pe_status_t pe_section_count(const pe_context_t *ctx, u16 *count);
pe_status_t pe_section_get(const pe_context_t *ctx,
                           u16 idx,
                           pe_section_view_t *out);

/* Translate an RVA to a file offset.  RVAs inside headers map to the same
 * file offset.  RVAs inside sections with no raw data (e.g. .bss) fail.       */
pe_status_t pe_rva_to_offset(const pe_context_t *ctx,
                             u32 rva,
                             u32 *offset);

/* -------------------------------------------------------------------------- */
/* Data directories                                                           */
/* -------------------------------------------------------------------------- */
#define IMAGE_DIRECTORY_ENTRY_EXPORT          0
#define IMAGE_DIRECTORY_ENTRY_IMPORT          1
#define IMAGE_DIRECTORY_ENTRY_RESOURCE        2
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION       3
#define IMAGE_DIRECTORY_ENTRY_SECURITY        4
#define IMAGE_DIRECTORY_ENTRY_BASERELOC       5
#define IMAGE_DIRECTORY_ENTRY_DEBUG           6
#define IMAGE_DIRECTORY_ENTRY_ARCHITECTURE    7
#define IMAGE_DIRECTORY_ENTRY_GLOBALPTR       8
#define IMAGE_DIRECTORY_ENTRY_TLS             9
#define IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG    10
#define IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT   11
#define IMAGE_DIRECTORY_ENTRY_IAT            12
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT   13
#define IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR 14
#define IMAGE_DIRECTORY_ENTRY_RESERVED       15

pe_status_t pe_directory_get(const pe_context_t *ctx,
                             u32 dir_idx,
                             pe_data_directory_t *out);

/* -------------------------------------------------------------------------- */
/* Import walk                                                                */
/* -------------------------------------------------------------------------- */
typedef pe_status_t (*pe_import_dll_cb)(void *user,
                                        const char *dll_name,
                                        size_t dll_len,
                                        u32 iat_rva,
                                        u32 ilt_rva);

typedef pe_status_t (*pe_import_symbol_cb)(void *user,
                                           const pe_import_symbol_t *sym);

/* Walk every import descriptor and every thunk.  Either callback may be NULL. */
pe_status_t pe_imports_walk(const pe_context_t *ctx,
                            pe_import_dll_cb dll_cb,
                            pe_import_symbol_cb sym_cb,
                            void *user);

/* -------------------------------------------------------------------------- */
/* Export walk                                                                */
/* -------------------------------------------------------------------------- */
typedef pe_status_t (*pe_export_cb)(void *user, const pe_export_entry_t *entry);

pe_status_t pe_exports_walk(const pe_context_t *ctx,
                            pe_export_cb cb,
                            void *user);

/* -------------------------------------------------------------------------- */
/* Relocation walk                                                            */
/* -------------------------------------------------------------------------- */
typedef pe_status_t (*pe_reloc_cb)(void *user, const pe_reloc_entry_t *entry);

pe_status_t pe_relocs_walk(const pe_context_t *ctx,
                           pe_reloc_cb cb,
                           void *user);

/* -------------------------------------------------------------------------- */
/* Exception / unwind directory                                               */
/* -------------------------------------------------------------------------- */
typedef pe_status_t (*pe_runtime_function_cb)(void *user,
                                              const pe_runtime_function_t *rf);

pe_status_t pe_exception_walk(const pe_context_t *ctx,
                              pe_runtime_function_cb cb,
                              void *user);

/* -------------------------------------------------------------------------- */
/* TLS directory                                                              */
/* -------------------------------------------------------------------------- */
pe_status_t pe_tls_directory(const pe_context_t *ctx,
                             IMAGE_TLS_DIRECTORY64 *out);

#endif /* PE_PARSE_H */
