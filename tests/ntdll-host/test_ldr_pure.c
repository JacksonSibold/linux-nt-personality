/*
 * test_ldr_pure.c - host tests for the address translation and bounds logic
 * in src/ntdll/ldr_pure.h, used by ldr.c for the IAT patch (LDR-2).
 *
 * The image buffers here stand in for a mapped module laid out by RVA. Each
 * test keeps the file-offset location of the same bytes in view: the section
 * raw data sits at 0x400 in the file and at VirtualAddress 0x2000 in the
 * image, so an RVA write must never change the bytes at 0x400.
 */
#include "minitest.h"
#include "ldr_pure.h"
#include <string.h>

#define IMG_SIZE 0x3000ULL
#define IDATA_RVA 0x2000u
#define IDATA_RAW_OFF 0x400u
#define MARK 0xABu

static unsigned long long read_u64(const unsigned char *p)
{
    unsigned long long v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* LDR-2: the IAT base is FirstThunk. A zero FirstThunk falls back to the ILT
 * RVA (OriginalFirstThunk), also as an RVA. */
static void iat_base_is_rva_with_ilt_fallback(void)
{
    MT_EXPECT_EQ(ldr_iat_base_rva(0x2000, 0x1000), 0x2000);
    MT_EXPECT_EQ(ldr_iat_base_rva(0x2000, 0), 0x2000);
    MT_EXPECT_EQ(ldr_iat_base_rva(0, 0x1800), 0x1800);
}

/* LDR-2: the resolved address lands at IDATA_RVA + index * 8 in the image,
 * and the bytes at the file offset of the same section stay as they were. */
static void iat_write_lands_at_rva_not_file_offset(void)
{
    unsigned char image[IMG_SIZE];
    memset(image, MARK, sizeof(image));

    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, IDATA_RVA, 1, 0x1122334455667788ULL) == 1);
    MT_EXPECT(read_u64(image + IDATA_RVA + 8) == 0x1122334455667788ULL);
    MT_EXPECT(read_u64(image + IDATA_RVA) == 0xABABABABABABABABULL);

    for (unsigned i = 0; i < 8; ++i)
        MT_EXPECT(image[IDATA_RAW_OFF + i] == MARK);
}

/* The last slot that fits ends exactly at the image size. */
static void iat_write_exact_fit_at_end(void)
{
    unsigned char image[IMG_SIZE];
    memset(image, MARK, sizeof(image));

    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, 0x2FF8, 0, 0x55ULL) == 1);
    MT_EXPECT(read_u64(image + 0x2FF8) == 0x55ULL);
    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, 0x2FFC, 0, 0x66ULL) == 0);
}

/* A rejected write changes nothing, including the last valid slot. */
static void iat_write_rejected_writes_nothing(void)
{
    unsigned char image[IMG_SIZE];
    unsigned char copy[IMG_SIZE];
    memset(image, MARK, sizeof(image));
    memcpy(copy, image, sizeof(image));

    /* Fixture shape from the LDR-2 test plan: FirstThunk at image_size - 4. */
    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, IMG_SIZE - 4, 0, 0x77ULL) == 0);
    MT_EXPECT(memcmp(image, copy, sizeof(image)) == 0);
}

/* The slot address is computed without wrapping at 32 bits, and a zero-size
 * image accepts no write at all. */
static void iat_write_no_wrap(void)
{
    unsigned char image[IMG_SIZE];
    memset(image, MARK, sizeof(image));

    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, 0xFFFFFFFCULL, 0, 1) == 0);
    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, 0xFFFFFFF8ULL, 1, 1) == 0);
    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, 0, 0xFFFFFFFFFFFFULL, 1) == 0);
    MT_EXPECT(ldr_iat_write(image, 0, IDATA_RVA, 0, 1) == 0);
}

static const mt_case_t cases[] = {
    { "iat_base_is_rva_with_ilt_fallback", iat_base_is_rva_with_ilt_fallback },
    { "iat_write_lands_at_rva_not_file_offset", iat_write_lands_at_rva_not_file_offset },
    { "iat_write_exact_fit_at_end", iat_write_exact_fit_at_end },
    { "iat_write_rejected_writes_nothing", iat_write_rejected_writes_nothing },
    { "iat_write_no_wrap", iat_write_no_wrap },
};

int main(void)
{
    return MT_RUN("ldr_pure", cases);
}
