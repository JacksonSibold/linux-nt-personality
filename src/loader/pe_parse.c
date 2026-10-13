/* pe_parse.c - hardened PE32+ parser for untrusted input.
 *
 * The parser never dereferences an unaligned pointer and never reads beyond
 * the caller-supplied buffer.  All multi-byte reads are performed byte-by-byte
 * so the code is safe under UBSan and on architectures that do not tolerate
 * unaligned access.
 */
#include "pe_parse.h"

#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/string.h>
#else
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#endif

/* PE_FREESTANDING (the ntdll MinGW build) has no hosted stdio: the MinGW
 * stderr macro calls __acrt_iob_func, which the freestanding runtime does
 * not provide. The runtime fprintf ignores its stream argument. */
#ifdef PE_FREESTANDING
#define PE_ERR_STREAM ((FILE *)0)
#else
#define PE_ERR_STREAM stderr
#endif

/* -------------------------------------------------------------------------- */
/* Diagnostic buffer — records which validation failed and with what values   */
/* -------------------------------------------------------------------------- */
static char pe_diag_buf[512] = {0};

const char *pe_validate_diag(void)
{
    return pe_diag_buf;
}

static void pe_diag_clear(void)
{
    pe_diag_buf[0] = '\0';
}

#ifdef __KERNEL__
static void pe_diag_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(pe_diag_buf, sizeof(pe_diag_buf), fmt, ap);
    va_end(ap);
    pr_info("pe_validate: %s\n", pe_diag_buf);
}
#define PE_FAIL(code, fmt, ...) do { pe_diag_set(fmt, ##__VA_ARGS__); return (code); } while (0)
#else
static void pe_diag_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(pe_diag_buf, sizeof(pe_diag_buf), fmt, ap);
    va_end(ap);
    fprintf(PE_ERR_STREAM, "pe_validate: %s\n", pe_diag_buf);
}
#define PE_FAIL(code, fmt, ...) do { pe_diag_set(fmt, ##__VA_ARGS__); return (code); } while (0)
#endif


/* -------------------------------------------------------------------------- */
/* Bounds-checked little-endian readers                                       */
/* -------------------------------------------------------------------------- */
pe_status_t pe_check_read(const pe_context_t *ctx,
                          u32 off,
                          size_t n)
{
    if (!ctx || !ctx->data) return PE_ERR_BOUNDS;
    if (n == 0) return PE_OK;
    if ((size_t)off >= ctx->size) return PE_ERR_BOUNDS;
    if (n > ctx->size - (size_t)off) return PE_ERR_BOUNDS;
    return PE_OK;
}

u16 pe_load_u16(const u8 *p)
{
    return (u16)((u16)p[0] |
                       ((u16)p[1] << 8));
}

u32 pe_load_u32(const u8 *p)
{
    return (u32)p[0] |
           ((u32)p[1] << 8) |
           ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

u64 pe_load_u64(const u8 *p)
{
    return (u64)p[0] |
           ((u64)p[1] << 8) |
           ((u64)p[2] << 16) |
           ((u64)p[3] << 24) |
           ((u64)p[4] << 32) |
           ((u64)p[5] << 40) |
           ((u64)p[6] << 48) |
           ((u64)p[7] << 56);
}

/* -------------------------------------------------------------------------- */
/* Context init                                                               */
/* -------------------------------------------------------------------------- */
pe_status_t pe_context_init(pe_context_t *ctx,
                            const u8 *data,
                            size_t size)
{
    if (!ctx) return PE_ERR_BOUNDS;
    *ctx = (pe_context_t){0};
    ctx->data = data;
    ctx->size = size;
    return PE_OK;
}

/* -------------------------------------------------------------------------- */
/* Header field helpers                                                       */
/* -------------------------------------------------------------------------- */
static const u8 *pe_at(const pe_context_t *ctx, u32 off)
{
    return ctx->data + off;
}

u16 pe_file_u16(const pe_context_t *ctx, u32 off)
{
    return pe_load_u16(pe_at(ctx, off));
}

u32 pe_file_u32(const pe_context_t *ctx, u32 off)
{
    return pe_load_u32(pe_at(ctx, off));
}

static pe_status_t pe_read_dos_header(const pe_context_t *ctx,
                                      IMAGE_DOS_HEADER *out)
{
    pe_status_t s = pe_check_read(ctx, 0, sizeof(IMAGE_DOS_HEADER));
    if (s != PE_OK) return s;
    const u8 *p = ctx->data;
    out->e_magic    = pe_load_u16(p + 0);
    out->e_cblp     = pe_load_u16(p + 2);
    out->e_cp       = pe_load_u16(p + 4);
    out->e_crlc     = pe_load_u16(p + 6);
    out->e_cparhdr  = pe_load_u16(p + 8);
    out->e_minalloc = pe_load_u16(p + 10);
    out->e_maxalloc = pe_load_u16(p + 12);
    out->e_ss       = pe_load_u16(p + 14);
    out->e_sp       = pe_load_u16(p + 16);
    out->e_csum     = pe_load_u16(p + 18);
    out->e_ip       = pe_load_u16(p + 20);
    out->e_cs       = pe_load_u16(p + 22);
    out->e_lfarlc   = pe_load_u16(p + 24);
    out->e_ovno     = pe_load_u16(p + 26);
    for (int i = 0; i < 4; ++i)
        out->e_res[i] = pe_load_u16(p + 28 + 2 * (size_t)i);
    out->e_oemid    = pe_load_u16(p + 36);
    out->e_oeminfo  = pe_load_u16(p + 38);
    for (int i = 0; i < 10; ++i)
        out->e_res2[i] = pe_load_u16(p + 40 + 2 * (size_t)i);
    out->e_lfanew   = pe_load_u32(p + 60);
    return PE_OK;
}

static pe_status_t pe_read_file_header(const pe_context_t *ctx,
                                       u32 off,
                                       IMAGE_FILE_HEADER *out)
{
    pe_status_t s = pe_check_read(ctx, off, sizeof(IMAGE_FILE_HEADER));
    if (s != PE_OK) return s;
    const u8 *p = ctx->data + off;
    out->Machine              = pe_load_u16(p + 0);
    out->NumberOfSections     = pe_load_u16(p + 2);
    out->TimeDateStamp        = pe_load_u32(p + 4);
    out->PointerToSymbolTable = pe_load_u32(p + 8);
    out->NumberOfSymbols      = pe_load_u32(p + 12);
    out->SizeOfOptionalHeader = pe_load_u16(p + 16);
    out->Characteristics      = pe_load_u16(p + 18);
    return PE_OK;
}

static pe_status_t pe_read_optional_header64(const pe_context_t *ctx,
                                             u32 off,
                                             IMAGE_OPTIONAL_HEADER64 *out)
{
    pe_status_t s = pe_check_read(ctx, off, sizeof(IMAGE_OPTIONAL_HEADER64));
    if (s != PE_OK) return s;
    const u8 *p = ctx->data + off;
    out->Magic                       = pe_load_u16(p + 0);
    out->MajorLinkerVersion          = p[2];
    out->MinorLinkerVersion          = p[3];
    out->SizeOfCode                  = pe_load_u32(p + 4);
    out->SizeOfInitializedData       = pe_load_u32(p + 8);
    out->SizeOfUninitializedData     = pe_load_u32(p + 12);
    out->AddressOfEntryPoint         = pe_load_u32(p + 16);
    out->BaseOfCode                  = pe_load_u32(p + 20);
    out->ImageBase                   = pe_load_u64(p + 24);
    out->SectionAlignment            = pe_load_u32(p + 32);
    out->FileAlignment               = pe_load_u32(p + 36);
    out->MajorOperatingSystemVersion = pe_load_u16(p + 40);
    out->MinorOperatingSystemVersion = pe_load_u16(p + 42);
    out->MajorImageVersion           = pe_load_u16(p + 44);
    out->MinorImageVersion           = pe_load_u16(p + 46);
    out->MajorSubsystemVersion       = pe_load_u16(p + 48);
    out->MinorSubsystemVersion       = pe_load_u16(p + 50);
    out->Win32VersionValue           = pe_load_u32(p + 52);
    out->SizeOfImage                 = pe_load_u32(p + 56);
    out->SizeOfHeaders               = pe_load_u32(p + 60);
    out->CheckSum                    = pe_load_u32(p + 64);
    out->Subsystem                   = pe_load_u16(p + 68);
    out->DllCharacteristics          = pe_load_u16(p + 70);
    out->SizeOfStackReserve          = pe_load_u64(p + 72);
    out->SizeOfStackCommit           = pe_load_u64(p + 80);
    out->SizeOfHeapReserve           = pe_load_u64(p + 88);
    out->SizeOfHeapCommit            = pe_load_u64(p + 96);
    out->LoaderFlags                 = pe_load_u32(p + 104);
    out->NumberOfRvaAndSizes         = pe_load_u32(p + 108);
    for (int i = 0; i < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; ++i) {
        u32 base = 112 + 8 * (u32)i;
        out->DataDirectory[i].VirtualAddress = pe_load_u32(p + base);
        out->DataDirectory[i].Size           = pe_load_u32(p + base + 4);
    }
    return PE_OK;
}

static pe_status_t pe_read_section_header(const pe_context_t *ctx,
                                          u32 off,
                                          IMAGE_SECTION_HEADER *out)
{
    pe_status_t s = pe_check_read(ctx, off, sizeof(IMAGE_SECTION_HEADER));
    if (s != PE_OK) return s;
    const u8 *p = ctx->data + off;
    for (int i = 0; i < IMAGE_SIZEOF_SHORT_NAME; ++i)
        out->Name[i] = p[i];
    out->Misc.VirtualSize   = pe_load_u32(p + 8);
    out->VirtualAddress     = pe_load_u32(p + 12);
    out->SizeOfRawData      = pe_load_u32(p + 16);
    out->PointerToRawData   = pe_load_u32(p + 20);
    out->PointerToRelocations  = pe_load_u32(p + 24);
    out->PointerToLinenumbers  = pe_load_u32(p + 28);
    out->NumberOfRelocations   = pe_load_u16(p + 32);
    out->NumberOfLinenumbers   = pe_load_u16(p + 34);
    out->Characteristics    = pe_load_u32(p + 36);
    return PE_OK;
}

pe_status_t pe_read_import_descriptor(const pe_context_t *ctx,
                                             u32 off,
                                             IMAGE_IMPORT_DESCRIPTOR *out)
{
    pe_status_t s = pe_check_read(ctx, off, sizeof(IMAGE_IMPORT_DESCRIPTOR));
    if (s != PE_OK) return s;
    const u8 *p = ctx->data + off;
    out->DUMMYUNIONNAME.OriginalFirstThunk = pe_load_u32(p + 0);
    out->TimeDateStamp   = pe_load_u32(p + 4);
    out->ForwarderChain  = pe_load_u32(p + 8);
    out->Name            = pe_load_u32(p + 12);
    out->FirstThunk      = pe_load_u32(p + 16);
    return PE_OK;
}

pe_status_t pe_read_thunk_data64(const pe_context_t *ctx,
                                        u32 off,
                                        IMAGE_THUNK_DATA64 *out)
{
    pe_status_t s = pe_check_read(ctx, off, sizeof(IMAGE_THUNK_DATA64));
    if (s != PE_OK) return s;
    out->u1.Ordinal = pe_load_u64(ctx->data + off);
    return PE_OK;
}

pe_status_t pe_read_export_directory(const pe_context_t *ctx,
                                            u32 off,
                                            IMAGE_EXPORT_DIRECTORY *out)
{
    pe_status_t s = pe_check_read(ctx, off, sizeof(IMAGE_EXPORT_DIRECTORY));
    if (s != PE_OK) return s;
    const u8 *p = ctx->data + off;
    out->Characteristics       = pe_load_u32(p + 0);
    out->TimeDateStamp         = pe_load_u32(p + 4);
    out->MajorVersion          = pe_load_u16(p + 8);
    out->MinorVersion          = pe_load_u16(p + 10);
    out->Name                  = pe_load_u32(p + 12);
    out->Base                  = pe_load_u32(p + 16);
    out->NumberOfFunctions     = pe_load_u32(p + 20);
    out->NumberOfNames         = pe_load_u32(p + 24);
    out->AddressOfFunctions    = pe_load_u32(p + 28);
    out->AddressOfNames        = pe_load_u32(p + 32);
    out->AddressOfNameOrdinals = pe_load_u32(p + 36);
    return PE_OK;
}

pe_status_t pe_read_tls_directory64(const pe_context_t *ctx,
                                           u32 off,
                                           IMAGE_TLS_DIRECTORY64 *out)
{
    pe_status_t s = pe_check_read(ctx, off, sizeof(IMAGE_TLS_DIRECTORY64));
    if (s != PE_OK) return s;
    const u8 *p = ctx->data + off;
    out->StartAddressOfRawData = pe_load_u64(p + 0);
    out->EndAddressOfRawData   = pe_load_u64(p + 8);
    out->AddressOfIndex        = pe_load_u64(p + 16);
    out->AddressOfCallBacks    = pe_load_u64(p + 24);
    out->SizeOfZeroFill        = pe_load_u32(p + 32);
    out->Characteristics       = pe_load_u32(p + 36);
    return PE_OK;
}

static pe_status_t pe_read_runtime_function(const pe_context_t *ctx,
                                            u32 off,
                                            IMAGE_RUNTIME_FUNCTION_ENTRY *out)
{
    pe_status_t s = pe_check_read(ctx, off, sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY));
    if (s != PE_OK) return s;
    const u8 *p = ctx->data + off;
    out->BeginAddress = pe_load_u32(p + 0);
    out->EndAddress   = pe_load_u32(p + 4);
    out->DUMMYUNIONNAME.UnwindInfoAddress = pe_load_u32(p + 8);
    return PE_OK;
}

pe_status_t pe_read_base_relocation(const pe_context_t *ctx,
                                           u32 off,
                                           IMAGE_BASE_RELOCATION *out)
{
    pe_status_t s = pe_check_read(ctx, off, sizeof(IMAGE_BASE_RELOCATION));
    if (s != PE_OK) return s;
    const u8 *p = ctx->data + off;
    out->VirtualAddress = pe_load_u32(p + 0);
    out->SizeOfBlock    = pe_load_u32(p + 4);
    return PE_OK;
}

/* -------------------------------------------------------------------------- */
/* String helpers                                                             */
/* -------------------------------------------------------------------------- */
pe_status_t pe_read_asciiz(const pe_context_t *ctx,
                                  u32 off,
                                  const char **out,
                                  size_t *len)
{
    if (!out || !len) return PE_ERR_BOUNDS;
    *out = NULL;
    *len = 0;
    if (off >= ctx->size) return PE_ERR_BOUNDS;

    /* Scan at most PE_MAX_SYMBOL_NAME bytes plus the NUL.  Hitting the limit
     * means either no NUL before EOF or a name longer than the cap. */
    size_t limit = ctx->size - off;
    if (limit > (size_t)PE_MAX_SYMBOL_NAME + 1)
        limit = (size_t)PE_MAX_SYMBOL_NAME + 1;
    size_t n = 0;
    while (n < limit && ctx->data[off + n] != '\0') ++n;
    if (n == limit) return PE_ERR_CORRUPT;

    *out = (const char *)(ctx->data + off);
    *len = n;
    return PE_OK;
}

/* -------------------------------------------------------------------------- */
/* Power-of-two check                                                         */
/* -------------------------------------------------------------------------- */
static int pe_is_power_of_two(u32 v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

/* -------------------------------------------------------------------------- */
/* Public API: validation                                                     */
/* -------------------------------------------------------------------------- */
pe_status_t pe_map_headers(pe_context_t *ctx)
{
    if (!ctx || !ctx->data) return PE_ERR_BOUNDS;
    ctx->dos_off = 0;
    if (ctx->size < 64) return PE_ERR_TRUNCATED;
    ctx->nt_off = pe_load_u32(ctx->data + 60);
    return PE_OK;
}

pe_status_t pe_validate(pe_context_t *ctx)
{
    pe_diag_clear();
    if (!ctx || !ctx->data) PE_FAIL(PE_ERR_BOUNDS, "null context or data");

    pe_status_t s = pe_check_read(ctx, 0, sizeof(IMAGE_DOS_HEADER));
    if (s != PE_OK) PE_FAIL(s, "DOS header trunc: ctx size %zu < %zu", ctx->size, sizeof(IMAGE_DOS_HEADER));

    IMAGE_DOS_HEADER dos;
    s = pe_read_dos_header(ctx, &dos);
    if (s != PE_OK) PE_FAIL(s, "read DOS header failed at 0");
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) PE_FAIL(PE_ERR_MZ, "MZ magic mismatch: got 0x%04x expected 0x%04x", dos.e_magic, IMAGE_DOS_SIGNATURE);

    u32 nt_off = dos.e_lfanew;
    if ((u64)nt_off + 4 + sizeof(IMAGE_FILE_HEADER) > ctx->size)
        PE_FAIL(PE_ERR_TRUNCATED, "e_lfanew %u + headers exceeds file size %zu", nt_off, ctx->size);

    u32 sig = pe_file_u32(ctx, nt_off);
    if (sig != IMAGE_NT_SIGNATURE) PE_FAIL(PE_ERR_PE_SIG, "PE sig mismatch at %u: got 0x%08x", nt_off, sig);

    IMAGE_FILE_HEADER fh;
    s = pe_read_file_header(ctx, nt_off + 4, &fh);
    if (s != PE_OK) PE_FAIL(s, "read FILE header at %u failed", nt_off + 4);

    if (fh.Machine != IMAGE_FILE_MACHINE_AMD64) PE_FAIL(PE_ERR_MACHINE, "Machine 0x%04x != AMD64 0x%04x", fh.Machine, IMAGE_FILE_MACHINE_AMD64);

    u32 oh_off = nt_off + 4 + sizeof(IMAGE_FILE_HEADER);
    if (oh_off + 2 > ctx->size) PE_FAIL(PE_ERR_TRUNCATED, "optional header off %u exceeds size %zu", oh_off, ctx->size);

    u16 magic = pe_file_u16(ctx, oh_off);
    if (magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) PE_FAIL(PE_ERR_MAGIC, "optional magic 0x%04x != 0x20B at %u", magic, oh_off);

    if (fh.SizeOfOptionalHeader < sizeof(u16) ||
        fh.SizeOfOptionalHeader > sizeof(IMAGE_OPTIONAL_HEADER64))
        PE_FAIL(PE_ERR_CORRUPT, "SizeOfOptionalHeader %u out of range [%zu, %zu]", fh.SizeOfOptionalHeader, sizeof(u16), sizeof(IMAGE_OPTIONAL_HEADER64));

    if ((u64)oh_off + fh.SizeOfOptionalHeader > ctx->size)
        PE_FAIL(PE_ERR_TRUNCATED, "optional header %u+%u exceeds size %zu", oh_off, fh.SizeOfOptionalHeader, ctx->size);

    /* PE32+ requires SizeOfOptionalHeader == 240. Check it before reading the
     * fixed-size structure so a short declared header is never read past. */
    if (fh.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64))
        PE_FAIL(PE_ERR_CORRUPT, "SizeOfOptionalHeader %u != %zu (PE32+ requires 240)", fh.SizeOfOptionalHeader, sizeof(IMAGE_OPTIONAL_HEADER64));

    IMAGE_OPTIONAL_HEADER64 opt;
    s = pe_read_optional_header64(ctx, oh_off, &opt);
    if (s != PE_OK) PE_FAIL(s, "read optional header at %u failed", oh_off);

    if (opt.NumberOfRvaAndSizes > IMAGE_NUMBEROF_DIRECTORY_ENTRIES)
        PE_FAIL(PE_ERR_CORRUPT, "NumberOfRvaAndSizes %u > %d", opt.NumberOfRvaAndSizes, IMAGE_NUMBEROF_DIRECTORY_ENTRIES);

    if (fh.NumberOfSections == 0 || fh.NumberOfSections > 96)
        PE_FAIL(PE_ERR_SECTIONS, "NumberOfSections %u out of range [1,96]", fh.NumberOfSections);

    u32 sec_table_off = oh_off + fh.SizeOfOptionalHeader;
    size_t sec_table_bytes = (size_t)fh.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if ((u64)sec_table_off + sec_table_bytes > ctx->size)
        PE_FAIL(PE_ERR_TRUNCATED, "section table %u+%zu exceeds size %zu", sec_table_off, sec_table_bytes, ctx->size);

    if (opt.SizeOfHeaders < sec_table_off + sec_table_bytes)
        PE_FAIL(PE_ERR_SECTIONS, "SizeOfHeaders 0x%x < section table end 0x%x", opt.SizeOfHeaders, sec_table_off + (u32)sec_table_bytes);

    if (opt.SectionAlignment == 0 || opt.FileAlignment == 0)
        PE_FAIL(PE_ERR_SECTIONS, "SectionAlignment 0x%x or FileAlignment 0x%x is zero", opt.SectionAlignment, opt.FileAlignment);

    if (!pe_is_power_of_two(opt.FileAlignment))
        PE_FAIL(PE_ERR_SECTIONS, "FileAlignment 0x%x not power of two", opt.FileAlignment);

    if (!pe_is_power_of_two(opt.SectionAlignment))
        PE_FAIL(PE_ERR_SECTIONS, "SectionAlignment 0x%x not power of two", opt.SectionAlignment);

    if (opt.SectionAlignment < opt.FileAlignment)
        PE_FAIL(PE_ERR_SECTIONS, "SectionAlignment 0x%x < FileAlignment 0x%x", opt.SectionAlignment, opt.FileAlignment);

    if (opt.SizeOfImage == 0 ||
        opt.SizeOfImage < opt.SectionAlignment ||
        (opt.SizeOfImage % opt.SectionAlignment) != 0)
        PE_FAIL(PE_ERR_SECTIONS, "SizeOfImage 0x%x invalid: zero or < SectionAlignment 0x%x or not aligned", opt.SizeOfImage, opt.SectionAlignment);

    /* SizeOfHeaders larger than file — hard error; misalignment is tolerated
     * (Windows accepts misaligned SizeOfHeaders, observed via probe_peloader). */
    if (opt.SizeOfHeaders > ctx->size)
        PE_FAIL(PE_ERR_SECTIONS, "SizeOfHeaders 0x%x > file size %zu", opt.SizeOfHeaders, ctx->size);
    if ((opt.SizeOfHeaders % opt.FileAlignment) != 0) {
#ifdef __KERNEL__
        pr_info("pe_validate: SizeOfHeaders 0x%x not FileAlignment-aligned (FileAlign 0x%x) — tolerated (Windows accepts)\n", opt.SizeOfHeaders, opt.FileAlignment);
#else
        fprintf(PE_ERR_STREAM, "pe_validate: SizeOfHeaders 0x%x not FileAlignment-aligned (FileAlign 0x%x) — tolerated\n", opt.SizeOfHeaders, opt.FileAlignment);
#endif
    }

    /* Data directory entries pointing outside image (except SECURITY which is file offset) */
    for (u32 di = 0; di < opt.NumberOfRvaAndSizes; ++di) {
        u32 rva = opt.DataDirectory[di].VirtualAddress;
        u32 sz  = opt.DataDirectory[di].Size;
        if (rva == 0 && sz == 0) continue;
        if (di == IMAGE_DIRECTORY_ENTRY_SECURITY) {
            /* SECURITY directory uses file offset, not RVA - skip image bound check */
            continue;
        }
        if (rva >= opt.SizeOfImage)
            PE_FAIL(PE_ERR_DIRECTORY, "data dir %u RVA 0x%x >= SizeOfImage 0x%x", di, rva, opt.SizeOfImage);
        if (sz > opt.SizeOfImage - rva)
            PE_FAIL(PE_ERR_DIRECTORY, "data dir %u size 0x%x exceeds image bounds (RVA 0x%x SizeOfImage 0x%x)", di, sz, rva, opt.SizeOfImage);
        if ((u64)rva + sz < rva)
            PE_FAIL(PE_ERR_DIRECTORY, "data dir %u RVA+size overflow RVA 0x%x size 0x%x", di, rva, sz);
        if ((u64)rva + sz > opt.SizeOfImage)
            PE_FAIL(PE_ERR_DIRECTORY, "data dir %u end 0x%x > SizeOfImage 0x%x", di, (u32)((u64)rva+sz), opt.SizeOfImage);
    }

    /* Validate each section - collect for overlap / highest-end checks */
    /* Use fixed max 96 to avoid dynamic allocation */
    u32 sec_vas[96];
    u32 sec_ends[96];
    for (u16 i = 0; i < fh.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sec;
        u32 off = sec_table_off + (u32)i * sizeof(IMAGE_SECTION_HEADER);
        s = pe_read_section_header(ctx, off, &sec);
        if (s != PE_OK) PE_FAIL(s, "read section %u at 0x%x failed", i, off);

        /* VirtualAddress must be SectionAlignment-aligned */
        if ((sec.VirtualAddress % opt.SectionAlignment) != 0)
            PE_FAIL(PE_ERR_SECTIONS, "section %u VA 0x%x not SectionAlignment-aligned (align 0x%x)", i, sec.VirtualAddress, opt.SectionAlignment);

        if (sec.VirtualAddress >= opt.SizeOfImage)
            PE_FAIL(PE_ERR_SECTIONS, "section %u VA 0x%x >= SizeOfImage 0x%x", i, sec.VirtualAddress, opt.SizeOfImage);

        u32 vsize = sec.Misc.VirtualSize;
        if (vsize == 0) vsize = sec.SizeOfRawData;

        /* VA + vsize is computed in u64 here, so this also catches overflow */
        if ((u64)sec.VirtualAddress + vsize > opt.SizeOfImage)
            PE_FAIL(PE_ERR_SECTIONS, "section %u VA 0x%x + vsize 0x%x = 0x%x > SizeOfImage 0x%x", i, sec.VirtualAddress, vsize, (u32)((u64)sec.VirtualAddress+vsize), opt.SizeOfImage);

        if (sec.SizeOfRawData != 0) {
            /* SizeOfRawData + PointerToRawData overflowing */
            if ((u64)sec.PointerToRawData + sec.SizeOfRawData < sec.PointerToRawData)
                PE_FAIL(PE_ERR_BOUNDS, "section %u PointerToRawData 0x%x + SizeOfRawData 0x%x overflow", i, sec.PointerToRawData, sec.SizeOfRawData);
            if ((u64)sec.PointerToRawData + sec.SizeOfRawData > 0xFFFFFFFFULL)
                PE_FAIL(PE_ERR_BOUNDS, "section %u raw end >4GB: ptr 0x%x size 0x%x", i, sec.PointerToRawData, sec.SizeOfRawData);
            if (sec.PointerToRawData > ctx->size)
                PE_FAIL(PE_ERR_BOUNDS, "section %u PointerToRawData 0x%x > file size %zu", i, sec.PointerToRawData, ctx->size);
            if ((u64)sec.PointerToRawData + sec.SizeOfRawData > ctx->size)
                PE_FAIL(PE_ERR_BOUNDS, "section %u raw range 0x%x+0x%x > file size %zu", i, sec.PointerToRawData, sec.SizeOfRawData, ctx->size);
        } else {
            /* If SizeOfRawData==0, PointerToRawData should be 0 (except .bss) */
            if (sec.PointerToRawData != 0) {
                /* Allow non-zero pointer only if VirtualSize==0 as well? Be strict */
                if (sec.PointerToRawData > ctx->size)
                    PE_FAIL(PE_ERR_BOUNDS, "section %u SizeOfRawData 0 but PointerToRawData 0x%x > file size %zu", i, sec.PointerToRawData, ctx->size);
            }
        }

        if (sec.PointerToRelocations != 0 || sec.NumberOfRelocations != 0 ||
            sec.PointerToLinenumbers != 0 || sec.NumberOfLinenumbers != 0) {
            /* Optional COFF relics; do not fail, but require they stay in bounds
             * if present. */
        }

        sec_vas[i] = sec.VirtualAddress;
        /* Effective end for overlap check: VA + vsize (if vsize==0, use 1 to avoid zero-length overlap false negatives) */
        u32 end = sec.VirtualAddress + vsize;
        /* For zero vsize, treat as VA+1 for overlap purposes, but still allow precise SizeOfImage calc */
        if (vsize == 0) end = sec.VirtualAddress + 1;
        sec_ends[i] = end;
    }

    /* Overlapping section VA ranges */
    for (u16 i = 0; i < fh.NumberOfSections; ++i) {
        for (u16 j = i + 1; j < fh.NumberOfSections; ++j) {
            u32 a_start = sec_vas[i];
            u32 a_end = sec_ends[i];
            u32 b_start = sec_vas[j];
            u32 b_end = sec_ends[j];
            if (a_start < b_end && b_start < a_end)
                PE_FAIL(PE_ERR_SECTIONS, "sections %u [0x%x,0x%x) and %u [0x%x,0x%x) overlap", i, a_start, a_end, j, b_start, b_end);
        }
    }

    /* SizeOfImage inconsistent with highest section end — Windows allows larger
     * SizeOfImage (observed via probe_peloader + Win32 SEC_IMAGE), so we only
     * reject if SizeOfImage is smaller than required. */
    {
        u32 highest = 0;
        for (u16 i = 0; i < fh.NumberOfSections; ++i) {
            u32 vsize = 0;
            u32 off = sec_table_off + (u32)i * sizeof(IMAGE_SECTION_HEADER);
            IMAGE_SECTION_HEADER sec;
            if (pe_read_section_header(ctx, off, &sec) != PE_OK) continue;
            vsize = sec.Misc.VirtualSize;
            if (vsize == 0) vsize = sec.SizeOfRawData;
            u32 end = sec.VirtualAddress + vsize;
            if (end > highest) highest = end;
        }
        if (highest > opt.SizeOfImage)
            PE_FAIL(PE_ERR_SECTIONS, "highest section end 0x%x > SizeOfImage 0x%x", highest, opt.SizeOfImage);
        u32 aligned_highest = (highest + opt.SectionAlignment - 1) & ~(opt.SectionAlignment - 1);
        if (opt.SizeOfImage < aligned_highest)
            PE_FAIL(PE_ERR_SECTIONS, "SizeOfImage 0x%x < aligned_highest 0x%x (highest 0x%x align 0x%x)", opt.SizeOfImage, aligned_highest, highest, opt.SectionAlignment);
        if (opt.SizeOfImage != aligned_highest) {
            u32 slack = opt.SizeOfImage - aligned_highest;
            if (slack > 0x100000)
                PE_FAIL(PE_ERR_SECTIONS, "SizeOfImage 0x%x > aligned_highest 0x%x by 0x%x > 1MB slack — likely malformed", opt.SizeOfImage, aligned_highest, slack);
#ifdef __KERNEL__
            pr_info("pe_validate: SizeOfImage 0x%x > aligned_highest 0x%x (highest 0x%x) slack 0x%x — tolerated (Windows allows larger)\n", opt.SizeOfImage, aligned_highest, highest, slack);
#else
            fprintf(PE_ERR_STREAM, "pe_validate: SizeOfImage 0x%x > aligned_highest 0x%x (highest 0x%x) slack 0x%x — tolerated\n", opt.SizeOfImage, aligned_highest, highest, slack);
#endif
        }
    }

    /* AddressOfEntryPoint outside any executable section (allow 0 for DLLs) */
    {
        u32 ep = opt.AddressOfEntryPoint;
        if (ep != 0) {
            int found_exec = 0;
            for (u16 i = 0; i < fh.NumberOfSections; ++i) {
                u32 off = sec_table_off + (u32)i * sizeof(IMAGE_SECTION_HEADER);
                IMAGE_SECTION_HEADER sec;
                if (pe_read_section_header(ctx, off, &sec) != PE_OK) continue;
                u32 vsize = sec.Misc.VirtualSize;
                if (vsize == 0) vsize = sec.SizeOfRawData;
                if (vsize == 0) continue;
                if (ep >= sec.VirtualAddress && ep < sec.VirtualAddress + vsize) {
                    if (sec.Characteristics & IMAGE_SCN_MEM_EXECUTE)
                        found_exec = 1;
                    break;
                }
            }
            if (!found_exec)
                PE_FAIL(PE_ERR_CORRUPT, "EntryPoint 0x%x not in executable section", ep);
        } else {
            /* Entry 0 allowed only for DLLs */
            if ((fh.Characteristics & IMAGE_FILE_DLL) == 0) {
                PE_FAIL(PE_ERR_CORRUPT, "EntryPoint 0 for EXE (Characteristics 0x%x, not DLL)", fh.Characteristics);
            }
        }
    }

    /* RVA cycles in import/export directories and directory must be in non-executable section */
    {
        if (opt.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
            u32 imp_rva = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
            u32 imp_sz  = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
            if (imp_rva != 0 && imp_sz != 0) {
                if (opt.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
                    u32 exp_rva = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
                    u32 exp_sz  = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
                    if (exp_rva != 0 && exp_sz != 0 && imp_rva == exp_rva) {
                        PE_FAIL(PE_ERR_CORRUPT, "import and export directories share RVA 0x%x", imp_rva);
                    }
                }
                int imp_in_exec = 0, imp_found = 0;
                for (u16 i = 0; i < fh.NumberOfSections; ++i) {
                    u32 off = sec_table_off + (u32)i * sizeof(IMAGE_SECTION_HEADER);
                    IMAGE_SECTION_HEADER sec;
                    if (pe_read_section_header(ctx, off, &sec) != PE_OK) continue;
                    u32 vsize = sec.Misc.VirtualSize;
                    if (vsize == 0) vsize = sec.SizeOfRawData;
                    if (vsize == 0) continue;
                    if (imp_rva >= sec.VirtualAddress && imp_rva < sec.VirtualAddress + vsize) {
                        imp_found = 1;
                        if (sec.Characteristics & IMAGE_SCN_MEM_EXECUTE) imp_in_exec = 1;
                        break;
                    }
                }
                if (imp_found && imp_in_exec)
                    PE_FAIL(PE_ERR_CORRUPT, "import directory RVA 0x%x in executable section", imp_rva);
                if (!imp_found) {
                    if (imp_rva >= opt.SizeOfHeaders)
                        PE_FAIL(PE_ERR_CORRUPT, "import directory RVA 0x%x outside sections and headers (SizeOfHeaders 0x%x)", imp_rva, opt.SizeOfHeaders);
                }
                if (imp_sz > 0x10000) {
                    PE_FAIL(PE_ERR_CORRUPT, "import directory size 0x%x > 0x10000", imp_sz);
                }
            }
        }
        if (opt.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
            u32 exp_rva = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
            u32 exp_sz  = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
            if (exp_rva != 0 && exp_sz != 0) {
                int exp_in_exec = 0, exp_found = 0;
                for (u16 i = 0; i < fh.NumberOfSections; ++i) {
                    u32 off = sec_table_off + (u32)i * sizeof(IMAGE_SECTION_HEADER);
                    IMAGE_SECTION_HEADER sec;
                    if (pe_read_section_header(ctx, off, &sec) != PE_OK) continue;
                    u32 vsize = sec.Misc.VirtualSize;
                    if (vsize == 0) vsize = sec.SizeOfRawData;
                    if (vsize == 0) continue;
                    if (exp_rva >= sec.VirtualAddress && exp_rva < sec.VirtualAddress + vsize) {
                        exp_found = 1;
                        if (sec.Characteristics & IMAGE_SCN_MEM_EXECUTE) exp_in_exec = 1;
                        break;
                    }
                }
                if (exp_found && exp_in_exec)
                    PE_FAIL(PE_ERR_CORRUPT, "export directory RVA 0x%x in executable section", exp_rva);
                if (!exp_found && exp_rva >= opt.SizeOfHeaders)
                    PE_FAIL(PE_ERR_CORRUPT, "export directory RVA 0x%x outside sections and headers (SizeOfHeaders 0x%x)", exp_rva, opt.SizeOfHeaders);
            }
        }
    }

    /* Commit parsed values. */
    ctx->dos_off = 0;
    ctx->nt_off = nt_off;
    ctx->section_table_off = sec_table_off;
    ctx->num_sections = fh.NumberOfSections;
    ctx->optional_magic = opt.Magic;
    ctx->section_alignment = opt.SectionAlignment;
    ctx->file_alignment = opt.FileAlignment;
    ctx->image_base = opt.ImageBase;
    ctx->size_of_image = opt.SizeOfImage;
    ctx->size_of_headers = opt.SizeOfHeaders;
    ctx->entry_point = opt.AddressOfEntryPoint;
    ctx->subsystem = opt.Subsystem;
    ctx->dll_characteristics = opt.DllCharacteristics;
    ctx->num_data_directories = opt.NumberOfRvaAndSizes;

    return PE_OK;
}

/* -------------------------------------------------------------------------- */
/* Section API                                                                */
/* -------------------------------------------------------------------------- */
pe_status_t pe_section_count(const pe_context_t *ctx, u16 *count)
{
    if (!ctx || !count) return PE_ERR_BOUNDS;
    if (ctx->nt_off == 0) return PE_ERR_CORRUPT;
    *count = ctx->num_sections;
    return PE_OK;
}

pe_status_t pe_section_get(const pe_context_t *ctx,
                           u16 idx,
                           pe_section_view_t *out)
{
    if (!ctx || !out) return PE_ERR_BOUNDS;
    if (idx >= ctx->num_sections) return PE_ERR_BOUNDS;

    u32 off = ctx->section_table_off +
                   (u32)idx * sizeof(IMAGE_SECTION_HEADER);
    IMAGE_SECTION_HEADER sec;
    pe_status_t s = pe_read_section_header(ctx, off, &sec);
    if (s != PE_OK) return s;

    out->hdr = sec;
    out->index = idx;
    out->virtual_address = sec.VirtualAddress;
    out->virtual_size = sec.Misc.VirtualSize != 0 ? sec.Misc.VirtualSize : sec.SizeOfRawData;
    out->file_offset = sec.PointerToRawData;
    out->file_size = sec.SizeOfRawData;
    return PE_OK;
}

pe_status_t pe_rva_to_offset(const pe_context_t *ctx,
                             u32 rva,
                             u32 *offset)
{
    if (!ctx || !offset) return PE_ERR_BOUNDS;

    /* Headers are mapped 1:1 at the beginning of the file. */
    if (rva < ctx->size_of_headers) {
        if (rva >= ctx->size) return PE_ERR_BOUNDS;
        *offset = rva;
        return PE_OK;
    }

    for (u16 i = 0; i < ctx->num_sections; ++i) {
        IMAGE_SECTION_HEADER sec;
        u32 off = ctx->section_table_off +
                       (u32)i * sizeof(IMAGE_SECTION_HEADER);
        pe_status_t s = pe_read_section_header(ctx, off, &sec);
        if (s != PE_OK) return s;

        u32 vsize = sec.Misc.VirtualSize != 0 ? sec.Misc.VirtualSize : sec.SizeOfRawData;
        u32 sec_end = sec.VirtualAddress + vsize;
        if (sec_end < sec.VirtualAddress) continue;

        if (rva >= sec.VirtualAddress && rva < sec_end) {
            if (sec.SizeOfRawData == 0) return PE_ERR_NOT_FOUND;

            u32 rel = rva - sec.VirtualAddress;
            if (rel >= sec.SizeOfRawData) return PE_ERR_NOT_FOUND;

            u32 file_off = sec.PointerToRawData + rel;
            if (file_off < sec.PointerToRawData) return PE_ERR_BOUNDS;
            if (file_off >= ctx->size) return PE_ERR_BOUNDS;

            *offset = file_off;
            return PE_OK;
        }
    }
    return PE_ERR_NOT_FOUND;
}

/* -------------------------------------------------------------------------- */
/* Data directories                                                           */
/* -------------------------------------------------------------------------- */
pe_status_t pe_directory_get(const pe_context_t *ctx,
                             u32 dir_idx,
                             pe_data_directory_t *out)
{
    if (!ctx || !out) return PE_ERR_BOUNDS;
    if (ctx->nt_off == 0) return PE_ERR_CORRUPT;
    if (dir_idx >= ctx->num_data_directories)
        return PE_ERR_DIRECTORY;

    u32 dd_off = ctx->nt_off + 4 + sizeof(IMAGE_FILE_HEADER) + 112 +
                      dir_idx * sizeof(IMAGE_DATA_DIRECTORY);
    pe_status_t s = pe_check_read(ctx, dd_off, sizeof(IMAGE_DATA_DIRECTORY));
    if (s != PE_OK) return PE_ERR_DIRECTORY;

    u32 rva = pe_file_u32(ctx, dd_off);
    u32 sz  = pe_file_u32(ctx, dd_off + 4);

    if (rva == 0 && sz == 0) {
        out->index = dir_idx;
        out->rva = 0;
        out->size = 0;
        return PE_OK;
    }

    if (rva >= ctx->size_of_image)
        return PE_ERR_DIRECTORY;

    if (sz > ctx->size_of_image - rva)
        return PE_ERR_DIRECTORY;

    out->index = dir_idx;
    out->rva = rva;
    out->size = sz;
    return PE_OK;
}

/* -------------------------------------------------------------------------- */
/* Imports                                                                    */
/* -------------------------------------------------------------------------- */
pe_status_t pe_imports_walk(const pe_context_t *ctx,
                            pe_import_dll_cb dll_cb,
                            pe_import_symbol_cb sym_cb,
                            void *user)
{
    if (!ctx) return PE_ERR_BOUNDS;

    pe_data_directory_t imp_dir;
    pe_status_t s = pe_directory_get(ctx, IMAGE_DIRECTORY_ENTRY_IMPORT, &imp_dir);
    if (s != PE_OK) return s; /* empty directory: pe_directory_get already gave PE_OK */
    if (imp_dir.rva == 0 || imp_dir.size == 0) return PE_OK;

    u32 imp_off;
    s = pe_rva_to_offset(ctx, imp_dir.rva, &imp_off);
    if (s != PE_OK) return s;

    if (imp_dir.size < sizeof(IMAGE_IMPORT_DESCRIPTOR))
        return PE_ERR_CORRUPT;

    u32 max_desc = imp_dir.size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    if (max_desc == 0) return PE_ERR_CORRUPT;

    /* Totals for the whole walk (L-16).  Descriptors may share an ILT, so the
     * per-descriptor loop alone does not bound the callbacks. */
    u32 n_desc = 0;
    u32 n_thunks = 0;

    for (u32 d = 0; d < max_desc; ++d) {
        u32 desc_off = imp_off + d * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        IMAGE_IMPORT_DESCRIPTOR desc;
        s = pe_read_import_descriptor(ctx, desc_off, &desc);
        if (s != PE_OK) return s;

        /* Null terminator. */
        if (desc.DUMMYUNIONNAME.OriginalFirstThunk == 0 &&
            desc.TimeDateStamp == 0 &&
            desc.ForwarderChain == 0 &&
            desc.Name == 0 &&
            desc.FirstThunk == 0)
            break;

        if (++n_desc > PE_MAX_IMPORT_DESCRIPTORS)
            return PE_ERR_CORRUPT;

        u32 name_off;
        s = pe_rva_to_offset(ctx, desc.Name, &name_off);
        if (s != PE_OK) return s;

        const char *dll_name;
        size_t dll_len;
        s = pe_read_asciiz(ctx, name_off, &dll_name, &dll_len);
        if (s != PE_OK) return s;
        if (dll_len == 0) return PE_ERR_CORRUPT;

        if (dll_cb) {
            s = dll_cb(user, dll_name, dll_len, desc.FirstThunk,
                       desc.DUMMYUNIONNAME.OriginalFirstThunk);
            if (s != PE_OK) return s;
        }

        if (!sym_cb) continue;

        u32 ilt_rva = desc.DUMMYUNIONNAME.OriginalFirstThunk;
        if (ilt_rva == 0) ilt_rva = desc.FirstThunk;
        if (ilt_rva == 0) return PE_ERR_CORRUPT;

        u32 ilt_off;
        s = pe_rva_to_offset(ctx, ilt_rva, &ilt_off);
        if (s != PE_OK) return s;

        u32 iat_off;
        s = pe_rva_to_offset(ctx, desc.FirstThunk, &iat_off);
        if (s != PE_OK && desc.FirstThunk != 0) return s;

        for (u32 t = 0; ; ++t) {
            u32 thunk_off = ilt_off + t * sizeof(IMAGE_THUNK_DATA64);
            if (thunk_off < ilt_off) return PE_ERR_BOUNDS;

            IMAGE_THUNK_DATA64 thunk;
            s = pe_read_thunk_data64(ctx, thunk_off, &thunk);
            if (s != PE_OK) return s;

            if (thunk.u1.Ordinal == 0) break;

            if (++n_thunks > PE_MAX_IMPORT_THUNKS)
                return PE_ERR_CORRUPT;

            pe_import_symbol_t sym = {0};
            if (thunk.u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                sym.ordinal_flag = IMAGE_ORDINAL_FLAG64;
                sym.ordinal = (u16)(thunk.u1.Ordinal & 0xFFFFULL);
            } else {
                /* Hint/name RVA is 32 bits; reject instead of truncating */
                if (thunk.u1.Ordinal > 0xFFFFFFFFULL)
                    return PE_ERR_CORRUPT;
                u32 ibn_off;
                s = pe_rva_to_offset(ctx, (u32)thunk.u1.Ordinal, &ibn_off);
                if (s != PE_OK) return s;

                pe_status_t s2 = pe_check_read(ctx, ibn_off,
                                               sizeof(u16) + 1);
                if (s2 != PE_OK) return s2;

                sym.hint = pe_file_u16(ctx, ibn_off);
                s2 = pe_read_asciiz(ctx, ibn_off + 2, &sym.name, &sym.name_len);
                if (s2 != PE_OK) return s2;
            }

            s = sym_cb(user, &sym);
            if (s != PE_OK) return s;
        }
    }

    return PE_OK;
}

/* -------------------------------------------------------------------------- */
/* Exports                                                                    */
/* -------------------------------------------------------------------------- */
/* Export directory tables, validated once and shared by the walker and the
 * name lookup.  present is 0 when the directory is absent (not an error). */
typedef struct pe_export_tables {
    int present;
    pe_data_directory_t dir;
    u32 dir_end;               /* dir.rva + dir.size                          */
    IMAGE_EXPORT_DIRECTORY ed;
    u32 funcs_off;             /* file offset of AddressOfFunctions           */
    u32 names_off;             /* file offset of AddressOfNames (0 if none)   */
    u32 ords_off;              /* file offset of AddressOfNameOrdinals        */
} pe_export_tables_t;

/* Bounds-checked file offset of element `index` in a table whose elements are
 * `width` bytes.  The sum is done in 64 bits so it cannot wrap. */
static pe_status_t pe_export_slot(const pe_context_t *ctx, u32 base_off,
                                  u32 index, u32 width, u32 *out)
{
    u64 off = (u64)base_off + (u64)index * width;
    if (off + width > ctx->size) return PE_ERR_BOUNDS;
    *out = (u32)off;
    return PE_OK;
}

static pe_status_t pe_export_tables_get(const pe_context_t *ctx,
                                        pe_export_tables_t *t)
{
    pe_status_t s;
    u32 exp_off;

    t->present = 0;
    s = pe_directory_get(ctx, IMAGE_DIRECTORY_ENTRY_EXPORT, &t->dir);
    if (s != PE_OK) return s;
    if (t->dir.rva == 0 || t->dir.size == 0) return PE_OK;
    t->present = 1;

    if (t->dir.size < sizeof(IMAGE_EXPORT_DIRECTORY))
        return PE_ERR_CORRUPT;
    t->dir_end = t->dir.rva + t->dir.size;
    if (t->dir_end < t->dir.rva) return PE_ERR_BOUNDS;

    s = pe_rva_to_offset(ctx, t->dir.rva, &exp_off);
    if (s != PE_OK) return s;
    s = pe_read_export_directory(ctx, exp_off, &t->ed);
    if (s != PE_OK) return s;

    /* Name ordinals are 16-bit indexes into the function table, so a table
     * with more than PE_MAX_EXPORT_ENTRIES entries cannot be addressed.  The
     * walker's per-function bitmap is sized for this limit. */
    if (t->ed.NumberOfFunctions > PE_MAX_EXPORT_ENTRIES ||
        t->ed.NumberOfNames > PE_MAX_EXPORT_ENTRIES)
        return PE_ERR_CORRUPT;

    if (t->ed.NumberOfFunctions != 0 &&
        (u64)t->ed.NumberOfFunctions * 4 > ctx->size)
        return PE_ERR_BOUNDS;
    if (t->ed.NumberOfNames != 0 &&
        ((u64)t->ed.NumberOfNames * 4 > ctx->size ||
         (u64)t->ed.NumberOfNames * 2 > ctx->size))
        return PE_ERR_BOUNDS;

    s = pe_rva_to_offset(ctx, t->ed.AddressOfFunctions, &t->funcs_off);
    if (s != PE_OK) return s;

    t->names_off = 0;
    t->ords_off = 0;
    if (t->ed.NumberOfNames != 0) {
        s = pe_rva_to_offset(ctx, t->ed.AddressOfNames, &t->names_off);
        if (s != PE_OK) return s;
        s = pe_rva_to_offset(ctx, t->ed.AddressOfNameOrdinals, &t->ords_off);
        if (s != PE_OK) return s;
    }
    return PE_OK;
}

/* Reads the name-table entry n: its function index and, when it is a valid
 * name, the name bytes.  Fails if the ordinal does not index a function. */
static pe_status_t pe_export_name_at(const pe_context_t *ctx,
                                     const pe_export_tables_t *t, u32 n,
                                     u32 *ord, const char **name, size_t *len)
{
    u32 nptr, optr, name_rva, name_off;
    pe_status_t s = pe_export_slot(ctx, t->names_off, n, 4, &nptr);
    if (s != PE_OK) return s;
    s = pe_export_slot(ctx, t->ords_off, n, 2, &optr);
    if (s != PE_OK) return s;

    *ord = pe_file_u16(ctx, optr);
    if (*ord >= t->ed.NumberOfFunctions) return PE_ERR_CORRUPT;

    name_rva = pe_file_u32(ctx, nptr);
    s = pe_rva_to_offset(ctx, name_rva, &name_off);
    if (s != PE_OK) return s;
    return pe_read_asciiz(ctx, name_off, name, len);
}

/* Fills ent for function index i.  name is NULL for an unnamed export.  On
 * return ent->rva is 0 when the function slot is empty (no export). */
static pe_status_t pe_export_build(const pe_context_t *ctx,
                                   const pe_export_tables_t *t, u32 i,
                                   const char *name, size_t name_len,
                                   pe_export_entry_t *ent)
{
    u32 func_off, fwd_off;
    pe_status_t s = pe_export_slot(ctx, t->funcs_off, i, 4, &func_off);
    if (s != PE_OK) return s;

    memset(ent, 0, sizeof *ent);
    ent->ordinal = t->ed.Base + i;
    ent->rva = pe_file_u32(ctx, func_off);
    ent->name = name;
    ent->name_len = name_len;
    if (ent->rva == 0) return PE_OK;
    /* A function RVA must lie inside the image (L-15). */
    if (ent->rva >= ctx->size_of_image) return PE_ERR_CORRUPT;

    /* An RVA inside the export directory is a forwarder string. */
    if (ent->rva >= t->dir.rva && ent->rva < t->dir_end) {
        s = pe_rva_to_offset(ctx, ent->rva, &fwd_off);
        if (s != PE_OK) return s;
        s = pe_read_asciiz(ctx, fwd_off, &ent->forwarder, &ent->forwarder_len);
        if (s != PE_OK) return s;
    }
    return PE_OK;
}

/* Byte-wise ASCII order, shorter name first on a common prefix. */
static int pe_name_compare(const char *a, size_t al, const char *b, size_t bl)
{
    size_t n = al < bl ? al : bl;
    int c = n ? memcmp(a, b, n) : 0;
    if (c != 0) return c;
    return al < bl ? -1 : (al > bl ? 1 : 0);
}

/* Each export is delivered once, in two groups: first the named exports in
 * name-table order (the first name for a function wins), then the unnamed
 * functions in ordinal order.  The cost is O(NumberOfFunctions +
 * NumberOfNames), and the bitmap below is the only extra storage. */
pe_status_t pe_exports_walk(const pe_context_t *ctx,
                            pe_export_cb cb,
                            void *user)
{
    pe_export_tables_t t;
    pe_export_entry_t ent;
    u64 named[PE_MAX_EXPORT_ENTRIES / 64];
    pe_status_t s;

    if (!ctx || !cb) return PE_ERR_BOUNDS;
    s = pe_export_tables_get(ctx, &t);
    if (s != PE_OK || !t.present) return s;

    memset(named, 0, sizeof named);

    for (u32 n = 0; n < t.ed.NumberOfNames; ++n) {
        u32 ord;
        const char *name;
        size_t name_len;
        s = pe_export_name_at(ctx, &t, n, &ord, &name, &name_len);
        if (s != PE_OK) return s;
        if (named[ord / 64] & ((u64)1 << (ord % 64)))
            continue;
        named[ord / 64] |= (u64)1 << (ord % 64);

        s = pe_export_build(ctx, &t, ord, name, name_len, &ent);
        if (s != PE_OK) return s;
        if (ent.rva == 0) continue;
        s = cb(user, &ent);
        if (s != PE_OK) return s;
    }

    for (u32 i = 0; i < t.ed.NumberOfFunctions; ++i) {
        if (named[i / 64] & ((u64)1 << (i % 64)))
            continue;
        s = pe_export_build(ctx, &t, i, NULL, 0, &ent);
        if (s != PE_OK) return s;
        if (ent.rva == 0) continue;
        s = cb(user, &ent);
        if (s != PE_OK) return s;
    }

    return PE_OK;
}

/* Name lookup.  Binary search over the name table, which the PE specification
 * requires to be sorted in ASCII order.  A miss is only trusted when the table
 * is found sorted; otherwise the names are scanned linearly, which is bounded by
 * PE_MAX_EXPORT_ENTRIES names of at most PE_MAX_SYMBOL_NAME bytes each. */
pe_status_t pe_export_find(const pe_context_t *ctx,
                           const char *name,
                           size_t name_len,
                           pe_export_entry_t *out)
{
    pe_export_tables_t t;
    pe_status_t s;
    u32 lo, hi, n;
    int sorted = 1;

    if (!ctx || !name || !out) return PE_ERR_BOUNDS;
    if (name_len == 0 || name_len > PE_MAX_SYMBOL_NAME) return PE_ERR_NOT_FOUND;

    s = pe_export_tables_get(ctx, &t);
    if (s != PE_OK) return s;
    if (!t.present || t.ed.NumberOfNames == 0) return PE_ERR_NOT_FOUND;

    lo = 0;
    hi = t.ed.NumberOfNames;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        u32 ord;
        const char *mname;
        size_t mlen;
        int c;

        s = pe_export_name_at(ctx, &t, mid, &ord, &mname, &mlen);
        if (s != PE_OK) return s;
        c = pe_name_compare(mname, mlen, name, name_len);
        if (c == 0) {
            s = pe_export_build(ctx, &t, ord, mname, mlen, out);
            if (s != PE_OK) return s;
            return out->rva == 0 ? PE_ERR_NOT_FOUND : PE_OK;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }

    /* Miss.  Check that the table is sorted; if it is, the miss is final. */
    for (n = 1; n < t.ed.NumberOfNames && sorted; ++n) {
        u32 ord_a, ord_b;
        const char *a, *b;
        size_t al, bl;
        s = pe_export_name_at(ctx, &t, n - 1, &ord_a, &a, &al);
        if (s != PE_OK) return s;
        s = pe_export_name_at(ctx, &t, n, &ord_b, &b, &bl);
        if (s != PE_OK) return s;
        if (pe_name_compare(a, al, b, bl) > 0)
            sorted = 0;
    }
    if (sorted) return PE_ERR_NOT_FOUND;

    /* Unsorted table: linear fallback. */
    for (n = 0; n < t.ed.NumberOfNames; ++n) {
        u32 ord;
        const char *mname;
        size_t mlen;
        s = pe_export_name_at(ctx, &t, n, &ord, &mname, &mlen);
        if (s != PE_OK) return s;
        if (pe_name_compare(mname, mlen, name, name_len) != 0)
            continue;
        s = pe_export_build(ctx, &t, ord, mname, mlen, out);
        if (s != PE_OK) return s;
        return out->rva == 0 ? PE_ERR_NOT_FOUND : PE_OK;
    }
    return PE_ERR_NOT_FOUND;
}

/* -------------------------------------------------------------------------- */
/* Relocations                                                                */
/* -------------------------------------------------------------------------- */
pe_status_t pe_relocs_walk(const pe_context_t *ctx,
                           pe_reloc_cb cb,
                           void *user)
{
    if (!ctx || !cb) return PE_ERR_BOUNDS;

    pe_data_directory_t reloc_dir;
    pe_status_t s = pe_directory_get(ctx, IMAGE_DIRECTORY_ENTRY_BASERELOC, &reloc_dir);
    if (s != PE_OK) return s;
    if (reloc_dir.rva == 0 || reloc_dir.size == 0) return PE_OK;

    u32 reloc_off;
    s = pe_rva_to_offset(ctx, reloc_dir.rva, &reloc_off);
    if (s != PE_OK) return s;
    /* Require the whole directory inside the file so that the u32 sums
     * reloc_off + cursor (and + entry offsets) below cannot wrap. */
    if ((u64)reloc_off + reloc_dir.size > (u64)ctx->size)
        return PE_ERR_BOUNDS;

    u32 cursor = 0;
    while (cursor < reloc_dir.size) {
        if (reloc_dir.size - cursor < sizeof(IMAGE_BASE_RELOCATION))
            break;

        IMAGE_BASE_RELOCATION block;
        s = pe_read_base_relocation(ctx, reloc_off + cursor, &block);
        if (s != PE_OK) return s;
        /* The page must lie inside the image, or the loader would write
         * outside it (L-15). */
        if (block.VirtualAddress >= ctx->size_of_image)
            return PE_ERR_CORRUPT;

        if (block.SizeOfBlock == 0) break;
        if (block.SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION))
            return PE_ERR_CORRUPT;
        if ((block.SizeOfBlock % 4) != 0)
            return PE_ERR_CORRUPT;
        if (block.SizeOfBlock > reloc_dir.size - cursor)
            return PE_ERR_CORRUPT;

        u32 num_entries =
            (block.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(u16);

        for (u32 i = 0; i < num_entries; ++i) {
            u32 entry_off = reloc_off + cursor +
                                 sizeof(IMAGE_BASE_RELOCATION) + i * 2;
            if (pe_check_read(ctx, entry_off, 2) != PE_OK)
                return PE_ERR_BOUNDS;
            u16 entry = pe_file_u16(ctx, entry_off);
            u16 type   = (entry >> 12) & 0xF;
            u16 offset = entry & 0xFFF;
            /* Type 6 and 11-15 are reserved in the PE/COFF specification. */
            if (type == 6 || type > 10)
                return PE_ERR_CORRUPT;

            pe_reloc_entry_t re = {
                .page_rva = block.VirtualAddress,
                .type = type,
                .offset = offset,
            };
            s = cb(user, &re);
            if (s != PE_OK) return s;
        }

        cursor += block.SizeOfBlock;
    }

    return PE_OK;
}

/* -------------------------------------------------------------------------- */
/* Exception directory                                                        */
/* -------------------------------------------------------------------------- */
pe_status_t pe_exception_walk(const pe_context_t *ctx,
                              pe_runtime_function_cb cb,
                              void *user)
{
    if (!ctx || !cb) return PE_ERR_BOUNDS;

    pe_data_directory_t exc_dir;
    pe_status_t s = pe_directory_get(ctx, IMAGE_DIRECTORY_ENTRY_EXCEPTION, &exc_dir);
    if (s != PE_OK) return s;
    if (exc_dir.rva == 0 || exc_dir.size == 0) return PE_OK;
    if ((exc_dir.size % sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY)) != 0)
        return PE_ERR_CORRUPT;

    u32 exc_off;
    s = pe_rva_to_offset(ctx, exc_dir.rva, &exc_off);
    if (s != PE_OK) return s;

    u32 count = exc_dir.size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
    for (u32 i = 0; i < count; ++i) {
        IMAGE_RUNTIME_FUNCTION_ENTRY rf;
        u32 off = exc_off + i * sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
        s = pe_read_runtime_function(ctx, off, &rf);
        if (s != PE_OK) return s;
        /* Begin and unwind must lie inside the image; End is exclusive and may
         * equal SizeOfImage (L-15). */
        if (rf.BeginAddress >= ctx->size_of_image ||
            rf.EndAddress > ctx->size_of_image ||
            rf.DUMMYUNIONNAME.UnwindInfoAddress >= ctx->size_of_image)
            return PE_ERR_CORRUPT;

        pe_runtime_function_t out = {
            .begin = rf.BeginAddress,
            .end = rf.EndAddress,
            .unwind = rf.DUMMYUNIONNAME.UnwindInfoAddress,
        };
        s = cb(user, &out);
        if (s != PE_OK) return s;
    }

    return PE_OK;
}

/* -------------------------------------------------------------------------- */
/* TLS directory                                                              */
/* -------------------------------------------------------------------------- */
pe_status_t pe_tls_directory(const pe_context_t *ctx,
                             IMAGE_TLS_DIRECTORY64 *out)
{
    if (!ctx || !out) return PE_ERR_BOUNDS;

    pe_data_directory_t tls_dir;
    pe_status_t s = pe_directory_get(ctx, IMAGE_DIRECTORY_ENTRY_TLS, &tls_dir);
    if (s != PE_OK) return s;
    if (tls_dir.rva == 0 || tls_dir.size == 0) return PE_ERR_NOT_FOUND;
    if (tls_dir.size < sizeof(IMAGE_TLS_DIRECTORY64))
        return PE_ERR_CORRUPT;

    u32 tls_off;
    s = pe_rva_to_offset(ctx, tls_dir.rva, &tls_off);
    if (s != PE_OK) return s;

    return pe_read_tls_directory64(ctx, tls_off, out);
}
