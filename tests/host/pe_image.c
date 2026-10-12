/* pe_image.c - in-memory PE32+ image builder for the host tests. */
#include "pe_image.h"

#include <string.h>

void img_w16(uint8_t *b, size_t off, uint16_t v)
{
    b[off + 0] = (uint8_t)(v & 0xFF);
    b[off + 1] = (uint8_t)(v >> 8);
}

void img_w32(uint8_t *b, size_t off, uint32_t v)
{
    for (size_t i = 0; i < 4; ++i)
        b[off + i] = (uint8_t)(v >> (8 * i));
}

void img_w64(uint8_t *b, size_t off, uint64_t v)
{
    for (size_t i = 0; i < 8; ++i)
        b[off + i] = (uint8_t)(v >> (8 * i));
}

uint32_t img_r32(const uint8_t *b, size_t off)
{
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

void img_set_dir(uint8_t *b, unsigned idx, uint32_t rva, uint32_t size)
{
    img_w32(b, IMG_DIRS + 8 * idx, rva);
    img_w32(b, IMG_DIRS + 8 * idx + 4, size);
}

static void put_section(uint8_t *b, unsigned idx, const char *name,
                        uint32_t vsize, uint32_t va, uint32_t raw_size,
                        uint32_t raw_ptr, uint32_t chars)
{
    size_t o = IMG_SEC(idx);
    for (size_t i = 0; i < 8 && name[i] != '\0'; ++i)
        b[o + i] = (uint8_t)name[i];
    img_w32(b, o + 8, vsize);
    img_w32(b, o + 12, va);
    img_w32(b, o + 16, raw_size);
    img_w32(b, o + 20, raw_ptr);
    img_w32(b, o + 36, chars);
}

void img_build_valid(uint8_t *b)
{
    memset(b, 0, IMG_LEN);

    /* DOS header. */
    b[0] = 'M';
    b[1] = 'Z';
    img_w32(b, 60, IMG_NT);

    /* COFF file header. */
    img_w32(b, IMG_NT, 0x00004550);      /* "PE\0\0"                        */
    img_w16(b, IMG_NT + 4, 0x8664);      /* Machine = AMD64                 */
    img_w16(b, IMG_NT + 6, 2);           /* NumberOfSections                */
    img_w16(b, IMG_NT + 20, 240);        /* SizeOfOptionalHeader (PE32+)    */
    img_w16(b, IMG_NT + 22, 0x0022);     /* EXECUTABLE_IMAGE | LARGE_ADDR   */

    /* Optional header (PE32+). */
    img_w16(b, IMG_OPT + 0, 0x20B);      /* Magic                           */
    img_w32(b, IMG_OPT + 16, 0x1000);    /* AddressOfEntryPoint (.text)     */
    img_w32(b, IMG_OPT + 20, 0x1000);    /* BaseOfCode                      */
    img_w64(b, IMG_OPT + 24, 0x140000000ULL); /* ImageBase                  */
    img_w32(b, IMG_OPT + 32, 0x1000);    /* SectionAlignment                */
    img_w32(b, IMG_OPT + 36, 0x200);     /* FileAlignment                   */
    img_w16(b, IMG_OPT + 40, 6);         /* MajorOperatingSystemVersion     */
    img_w16(b, IMG_OPT + 48, 6);         /* MajorSubsystemVersion           */
    img_w32(b, IMG_OPT + 56, 0x3000);    /* SizeOfImage                     */
    img_w32(b, IMG_OPT + 60, 0x400);     /* SizeOfHeaders                   */
    img_w16(b, IMG_OPT + 68, 3);         /* Subsystem = WINDOWS_CUI         */
    img_w16(b, IMG_OPT + 70, 0x8160);    /* DllCharacteristics              */
    img_w32(b, IMG_OPT + 108, 16);       /* NumberOfRvaAndSizes             */

    /* Section table. */
    put_section(b, 0, ".text", 0x200, 0x1000, 0x200, IMG_TEXT_RAW,
                0x60000020u);            /* CODE | EXECUTE | READ           */
    put_section(b, 1, ".rdata", 0x200, 0x2000, 0x200, IMG_RDATA_RAW,
                0x40000040u);            /* INITIALIZED_DATA | READ         */

    /* .text: a single RET so the entry point is not all zeros. */
    b[IMG_TEXT_RAW] = 0xC3;
}

void img_add_import(uint8_t *b)
{
    /* Import descriptor at RVA 0x2000 (file 0x600), then a null terminator. */
    const size_t d = IMG_RDATA_RAW;
    img_w32(b, d + 0, 0x2040);           /* OriginalFirstThunk (ILT)        */
    img_w32(b, d + 12, 0x2080);          /* Name -> "KERNEL32.dll"          */
    img_w32(b, d + 16, 0x2040);          /* FirstThunk (IAT)                */

    /* ILT: one by-name thunk, then a zero terminator. */
    img_w64(b, 0x640, 0x2060);
    img_w64(b, 0x648, 0);

    /* IMAGE_IMPORT_BY_NAME at RVA 0x2060: hint 0, "ExitProcess". */
    img_w16(b, 0x660, 0);
    memcpy(b + 0x662, "ExitProcess", 12);

    memcpy(b + 0x680, "KERNEL32.dll", 13);

    img_set_dir(b, DIR_IMPORT, 0x2000, 40);
}

void img_add_reloc(uint8_t *b)
{
    /* One block for page RVA 0x1000: DIR64 at +0x010 and HIGHLOW at +0x020. */
    img_w32(b, 0x700, 0x1000);           /* VirtualAddress                  */
    img_w32(b, 0x704, 12);               /* SizeOfBlock = 8 + 2 * 2         */
    img_w16(b, 0x708, (uint16_t)((10 << 12) | 0x010));
    img_w16(b, 0x70A, (uint16_t)((3 << 12) | 0x020));

    img_set_dir(b, DIR_BASERELOC, 0x2100, 12);
}

void img_add_exception(uint8_t *b)
{
    img_w32(b, 0x720, 0x1000);           /* BeginAddress                    */
    img_w32(b, 0x724, 0x1007);           /* EndAddress (not 4-byte aligned) */
    img_w32(b, 0x728, 0x2140);           /* UnwindInfoAddress               */

    img_set_dir(b, DIR_EXCEPTION, 0x2120, 12);
}
