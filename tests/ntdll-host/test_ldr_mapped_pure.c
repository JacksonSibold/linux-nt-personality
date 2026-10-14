/*
 * test_ldr_mapped_pure.c - host tests for the RVA-indexed header and export
 * lookup in src/ntdll/ldr_pure.h (ldr_mapped_headers, ldr_mapped_export).
 *
 * The startup binder (src/ntdll/ntdll_main.c) maps kernel32, msvcrt and the
 * other startup DLLs as NT sections, so their bytes are laid out by RVA, not by
 * file offset. ldr.c registers those modules in the loader list and resolves
 * GetProcAddress through these helpers. The synthetic image below is a
 * PE32+ module with three exports: "Alpha" (ordinal 1), a forwarder "Fwd"
 * (ordinal 2, "NTDLL.RtlFoo") and an ordinal-only export (ordinal 3).
 */
#include "minitest.h"
#include "ldr_pure.h"
#include <string.h>

#define IMG_SIZE 0x3000ULL
#define EXP_RVA 0x2000u
#define EXP_SIZE 0x200u
#define ALPHA_RVA 0x1000u
#define BETA_RVA 0x1100u
#define FWD_STR_RVA 0x2180u

static void put_u16(unsigned char *p, unsigned long v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put_u32(unsigned char *p, unsigned long v)
{
    for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
}
static void put_u64(unsigned char *p, unsigned long long v)
{
    for (int i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

/* Builds a valid PE32+ image laid out by RVA (headers at 0, export directory
 * at EXP_RVA, the code of Alpha at ALPHA_RVA). */
static void build_image(unsigned char *img)
{
    memset(img, 0, IMG_SIZE);
    img[0] = 'M'; img[1] = 'Z';
    put_u32(img + 0x3C, 0x80);                        /* e_lfanew */
    img[0x80] = 'P'; img[0x81] = 'E';                 /* PE\0\0 */
    put_u16(img + 0x84, 0x8664);                      /* Machine x64 */
    put_u32(img + 0x88, 0x6A000000UL);                /* TimeDateStamp */
    put_u16(img + 0x94, 0xF0);                        /* SizeOfOptionalHeader */
    put_u16(img + 0x96, 0x2022);                      /* Characteristics: DLL */
    unsigned char *oh = img + 0x98;
    put_u16(oh + 0, 0x20B);                           /* PE32+ */
    put_u32(oh + 16, 0x1234);                         /* AddressOfEntryPoint */
    put_u64(oh + 24, 0x180000000ULL);                 /* ImageBase */
    put_u32(oh + 56, (unsigned long)IMG_SIZE);        /* SizeOfImage */
    put_u32(oh + 108, 16);                            /* NumberOfRvaAndSizes */
    put_u32(oh + 112, EXP_RVA);                       /* export directory RVA */
    put_u32(oh + 116, EXP_SIZE);                      /* export directory size */

    unsigned char *ed = img + EXP_RVA;
    put_u32(ed + 0x0C, 0x2300);                       /* Name (unused) */
    put_u32(ed + 0x10, 1);                            /* Base */
    put_u32(ed + 0x14, 3);                            /* NumberOfFunctions */
    put_u32(ed + 0x18, 2);                            /* NumberOfNames */
    put_u32(ed + 0x1C, 0x2100);                       /* AddressOfFunctions */
    put_u32(ed + 0x20, 0x2110);                       /* AddressOfNames */
    put_u32(ed + 0x24, 0x2120);                       /* AddressOfNameOrdinals */

    put_u32(img + 0x2100, ALPHA_RVA);                 /* function 0: Alpha */
    put_u32(img + 0x2104, FWD_STR_RVA);               /* function 1: forwarder */
    put_u32(img + 0x2108, BETA_RVA);                  /* function 2: ordinal only */

    put_u32(img + 0x2110, 0x2150);                    /* name 0 RVA */
    put_u32(img + 0x2114, 0x2160);                    /* name 1 RVA */
    put_u16(img + 0x2120, 0);                         /* Alpha -> function 0 */
    put_u16(img + 0x2122, 1);                         /* Fwd -> function 1 */

    memcpy(img + 0x2150, "Alpha", 6);
    memcpy(img + 0x2160, "Fwd", 4);
    memcpy(img + FWD_STR_RVA, "NTDLL.RtlFoo", 13);
    img[ALPHA_RVA] = 0xC3;                            /* ret: code, never run */
    img[BETA_RVA] = 0xC3;
}

static void headers_read_entry_base_and_size(void)
{
    unsigned char img[IMG_SIZE];
    ldr_mapped_hdr_t h;
    build_image(img);
    MT_EXPECT_EQ(ldr_mapped_headers(img, IMG_SIZE, &h), 1);
    MT_EXPECT_EQ(h.image_base, 0x180000000ULL);
    MT_EXPECT_EQ(h.entry_rva, 0x1234);
    MT_EXPECT_EQ(h.size_of_image, IMG_SIZE);
    MT_EXPECT_EQ(h.export_rva, EXP_RVA);
    MT_EXPECT_EQ(h.export_size, EXP_SIZE);
    MT_EXPECT_EQ(h.time_date_stamp, 0x6A000000UL);
}

/* A bad signature, a PE32 (not PE32+) image, or an e_lfanew past the image
 * fails, and the caller must not register the module. */
static void headers_reject_bad_images(void)
{
    unsigned char img[IMG_SIZE];
    ldr_mapped_hdr_t h;

    build_image(img);
    img[0] = 'X';
    MT_EXPECT_EQ(ldr_mapped_headers(img, IMG_SIZE, &h), 0);

    build_image(img);
    img[0x80] = 'Q';
    MT_EXPECT_EQ(ldr_mapped_headers(img, IMG_SIZE, &h), 0);

    build_image(img);
    put_u16(img + 0x98, 0x10B);                       /* PE32, not PE32+ */
    MT_EXPECT_EQ(ldr_mapped_headers(img, IMG_SIZE, &h), 0);

    build_image(img);
    put_u32(img + 0x3C, 0x2FFF0);                     /* e_lfanew past the image */
    MT_EXPECT_EQ(ldr_mapped_headers(img, IMG_SIZE, &h), 0);

    build_image(img);
    MT_EXPECT_EQ(ldr_mapped_headers(img, 0x90, &h), 0); /* header span too short */
}

/* Names are case-sensitive (LDR-7). A name that is only a prefix does not
 * match. A forwarder comes back with forwarder=1 and the RVA of its string. */
static void export_by_name(void)
{
    unsigned char img[IMG_SIZE];
    unsigned long rva = 0;
    int fwd = -1;
    build_image(img);

    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, "Alpha", 0, 0, &rva, &fwd), LDR_PURE_EXPORT_FOUND);
    MT_EXPECT_EQ(rva, ALPHA_RVA);
    MT_EXPECT_EQ(fwd, 0);

    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, "alpha", 0, 0, &rva, &fwd), LDR_PURE_EXPORT_NOT_FOUND);
    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, "Alph", 0, 0, &rva, &fwd), LDR_PURE_EXPORT_NOT_FOUND);
    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, "Missing", 0, 0, &rva, &fwd), LDR_PURE_EXPORT_NOT_FOUND);

    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, "Fwd", 0, 0, &rva, &fwd), LDR_PURE_EXPORT_FOUND);
    MT_EXPECT_EQ(rva, FWD_STR_RVA);
    MT_EXPECT_EQ(fwd, 1);
}

/* Ordinals are Base-relative: ordinal 1 is function 0. Ordinal 3 has no name
 * and is still found. Ordinals outside [Base, Base + NumberOfFunctions) are not. */
static void export_by_ordinal(void)
{
    unsigned char img[IMG_SIZE];
    unsigned long rva = 0;
    int fwd = -1;
    build_image(img);

    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, NULL, 1, 1, &rva, &fwd), LDR_PURE_EXPORT_FOUND);
    MT_EXPECT_EQ(rva, ALPHA_RVA);
    MT_EXPECT_EQ(fwd, 0);

    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, NULL, 2, 1, &rva, &fwd), LDR_PURE_EXPORT_FOUND);
    MT_EXPECT_EQ(fwd, 1);

    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, NULL, 3, 1, &rva, &fwd), LDR_PURE_EXPORT_FOUND);
    MT_EXPECT_EQ(rva, BETA_RVA);
    MT_EXPECT_EQ(fwd, 0);

    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, NULL, 0, 1, &rva, &fwd), LDR_PURE_EXPORT_NOT_FOUND);
    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, NULL, 4, 1, &rva, &fwd), LDR_PURE_EXPORT_NOT_FOUND);
}

/* An image with no export directory has no exports. A directory that points
 * past the image is a bad image, not a lookup miss. A name table entry whose
 * ordinal names a function past NumberOfFunctions is also a bad image. */
static void export_bad_and_absent(void)
{
    unsigned char img[IMG_SIZE];
    unsigned long rva = 0;
    int fwd = -1;

    build_image(img);
    put_u32(img + 0x98 + 112, 0);                     /* no export directory */
    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, "Alpha", 0, 0, &rva, &fwd), LDR_PURE_EXPORT_NOT_FOUND);

    build_image(img);
    put_u32(img + 0x98 + 112, (unsigned long)IMG_SIZE - 8); /* directory runs off the end */
    put_u32(img + 0x98 + 116, 0x100);
    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, "Alpha", 0, 0, &rva, &fwd), LDR_PURE_EXPORT_BAD_IMAGE);

    build_image(img);
    put_u16(img + 0x2120, 7);                         /* Alpha names function 7 of 3 */
    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, "Alpha", 0, 0, &rva, &fwd), LDR_PURE_EXPORT_BAD_IMAGE);
}

/* A name string that is not NUL-terminated before the image end is skipped:
 * it cannot match and must not be read past the image. */
static void export_unterminated_name(void)
{
    unsigned char img[IMG_SIZE];
    unsigned long rva = 0;
    int fwd = -1;
    build_image(img);
    put_u32(img + 0x2110, (unsigned long)IMG_SIZE - 2); /* name 0 starts two bytes before the end */
    img[IMG_SIZE - 2] = 'A';
    img[IMG_SIZE - 1] = 'l';
    MT_EXPECT_EQ(ldr_mapped_export(img, IMG_SIZE, "Alpha", 0, 0, &rva, &fwd), LDR_PURE_EXPORT_NOT_FOUND);
}

/* The forwarder string is read through ldr_mapped_cstr, which refuses a string
 * that runs past the image. */
static void cstr_bounds(void)
{
    unsigned char img[IMG_SIZE];
    const char *s = NULL;
    size_t len = 0;
    build_image(img);
    MT_EXPECT_EQ(ldr_mapped_cstr(img, IMG_SIZE, FWD_STR_RVA, &s, &len), 1);
    MT_EXPECT_EQ(len, 12);
    MT_EXPECT_EQ(strcmp(s, "NTDLL.RtlFoo"), 0);
    MT_EXPECT_EQ(ldr_mapped_cstr(img, IMG_SIZE, (unsigned long)IMG_SIZE, &s, &len), 0);
    img[IMG_SIZE - 1] = 'x';
    MT_EXPECT_EQ(ldr_mapped_cstr(img, IMG_SIZE, (unsigned long)IMG_SIZE - 1, &s, &len), 0);
}

static const mt_case_t cases[] = {
    MT_CASE(headers_read_entry_base_and_size),
    MT_CASE(headers_reject_bad_images),
    MT_CASE(export_by_name),
    MT_CASE(export_by_ordinal),
    MT_CASE(export_bad_and_absent),
    MT_CASE(export_unterminated_name),
    MT_CASE(cstr_bounds),
};

int main(void)
{
    return MT_RUN("ldr_mapped_pure", cases);
}
