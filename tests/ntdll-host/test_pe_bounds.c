/*
 * test_pe_bounds.c - host tests for the range checks in src/ntdll/pe_bounds.h,
 * used by the startup mapper (MAIN-1, MAIN-2) and the loader (LDR-5, LDR-6).
 */
#include "minitest.h"
#include "pe_bounds.h"

#define U64_MAX 0xFFFFFFFFFFFFFFFFULL

/* MAIN-1: the section table must lie inside the 0x1000-byte header buffer.
 * A count near 65535 puts the table far past it. */
static void section_table_must_fit_header(void)
{
    MT_EXPECT(pe_range_fits(0x178, 1 * 40ULL, 0x1000));
    MT_EXPECT(pe_range_fits(0x178, 22 * 40ULL, 0x1000));
    MT_EXPECT(!pe_range_fits(0x178, 65535ULL * 40, 0x1000));
    MT_EXPECT(!pe_range_fits(0x1000, 40, 0x1000));
}

/* Exact fit ends at the limit; one byte more does not. */
static void range_exact_fit(void)
{
    MT_EXPECT(pe_range_fits(0x1000 - 8, 8, 0x1000));
    MT_EXPECT(!pe_range_fits(0x1000 - 7, 8, 0x1000));
    MT_EXPECT(pe_range_fits(0, 0x1000, 0x1000));
    MT_EXPECT(!pe_range_fits(0, 0x1001, 0x1000));
}

/* Empty ranges fit at any offset up to the limit, and not beyond it. */
static void range_empty_and_zero_limit(void)
{
    MT_EXPECT(pe_range_fits(0, 0, 0));
    MT_EXPECT(pe_range_fits(0x1000, 0, 0x1000));
    MT_EXPECT(!pe_range_fits(0x1001, 0, 0x1000));
    MT_EXPECT(!pe_range_fits(0, 1, 0));
}

/* MAIN-2 / LDR-5: a relocation target at page RVA 0xFFFFF000 plus offset
 * 0xFFF must be rejected for a small image. The sum does not wrap at 32 bits,
 * and the 64-bit arithmetic here keeps it from wrapping too. */
static void reloc_target_past_image_rejected(void)
{
    unsigned long long image = 0x10000;
    MT_EXPECT(!pe_range_fits(0xFFFFF000ULL + 0xFFF, 8, image));
    MT_EXPECT(!pe_range_fits(0xFFFF0000ULL + 0xFFF, 8, image));
    MT_EXPECT(pe_range_fits(0xFFF8ULL, 8, image));
    MT_EXPECT(!pe_range_fits(0xFFF9ULL, 8, image));
}

/* MAIN-2: the relocation directory itself must lie inside the image, and an
 * 8-byte DIR64 target at the last valid offset must still be accepted. */
static void reloc_directory_must_fit_image(void)
{
    MT_EXPECT(pe_range_fits(0x9000, 0x200, 0x10000));
    MT_EXPECT(!pe_range_fits(0xFFF0, 0x200, 0x10000));
    MT_EXPECT(pe_range_fits(0xFFF0, 0x10, 0x10000));
}

/* LDR-5: the write width depends on the type (8 for DIR64, 4 for HIGHLOW).
 * The last 4-byte slot fits, and an 8-byte write at the same place does not. */
static void reloc_write_width_checked(void)
{
    unsigned long long image = 0x10000;
    MT_EXPECT(pe_range_fits(0xFFF8ULL, 8, image));
    MT_EXPECT(!pe_range_fits(0xFFFCULL, 8, image));
    MT_EXPECT(pe_range_fits(0xFFFCULL, 4, image));
    MT_EXPECT(!pe_range_fits(0x10000ULL, 4, image));
}

/* An offset near 2^64 must not wrap to a small value. */
static void range_no_wrap(void)
{
    MT_EXPECT(!pe_range_fits(U64_MAX - 3, 8, U64_MAX));
    MT_EXPECT(!pe_range_fits(16, U64_MAX, 0x1000));
    MT_EXPECT(pe_range_fits(U64_MAX - 8, 8, U64_MAX));
    MT_EXPECT(!pe_range_fits(U64_MAX - 7, 8, U64_MAX));
}

/* LDR-6: copy at most VirtualSize bytes of raw data. Raw size larger than
 * virtual size must not copy past the section. */
static void copy_len_is_min_of_raw_and_virtual(void)
{
    MT_EXPECT_EQ(pe_copy_len(0x400, 0x200), 0x200);
    MT_EXPECT_EQ(pe_copy_len(0x200, 0x400), 0x200);
    MT_EXPECT_EQ(pe_copy_len(0x400, 0x400), 0x400);
}

/* LDR-6: the last section of an image is copied with raw size larger than
 * virtual size. The copy must end at the virtual end, which is inside the
 * image, and a section whose virtual end passes the image is refused. */
static void section_copy_stays_in_image(void)
{
    unsigned long long image = 0x1200;
    unsigned long long copy = pe_copy_len(0x400, 0x200);
    MT_EXPECT(pe_range_fits(0x1000, copy, image));
    MT_EXPECT(pe_range_fits(0x1000, 0x200, image));
    MT_EXPECT(!pe_range_fits(0x1000, 0x400, image));
}

/* A zero VirtualSize means the raw size is used. */
static void copy_len_zero_virtual_uses_raw(void)
{
    MT_EXPECT_EQ(pe_copy_len(0x400, 0), 0x400);
    MT_EXPECT_EQ(pe_copy_len(0, 0), 0);
    MT_EXPECT_EQ(pe_copy_len(0, 0x200), 0);
}

static const mt_case_t cases[] = {
    { "section_table_must_fit_header", section_table_must_fit_header },
    { "range_exact_fit", range_exact_fit },
    { "range_empty_and_zero_limit", range_empty_and_zero_limit },
    { "reloc_target_past_image_rejected", reloc_target_past_image_rejected },
    { "reloc_directory_must_fit_image", reloc_directory_must_fit_image },
    { "reloc_write_width_checked", reloc_write_width_checked },
    { "range_no_wrap", range_no_wrap },
    { "copy_len_is_min_of_raw_and_virtual", copy_len_is_min_of_raw_and_virtual },
    { "copy_len_zero_virtual_uses_raw", copy_len_zero_virtual_uses_raw },
    { "section_copy_stays_in_image", section_copy_stays_in_image },
};

int main(void)
{
    return MT_RUN("pe_bounds", cases);
}
