/*
 * test_msvcrt_policy.c - host tests for the size arithmetic in
 * src/ntdll/msvcrt_policy.h, used by the msvcrt calloc (CRT-3).
 */
#include "minitest.h"
#include "msvcrt_policy.h"

#define MS_SIZE_MAX ((SIZE_T)-1)

/* CRT-3: calloc(n, s) must refuse a product that wraps. */
static void mul_accepts_products_that_fit(void)
{
    SIZE_T out = 0;
    MT_EXPECT(ms_mul_size(3, 4, &out));
    MT_EXPECT_EQ(out, 12);

    out = 0;
    MT_EXPECT(ms_mul_size(0, MS_SIZE_MAX, &out));
    MT_EXPECT(out == 0);

    out = 0;
    MT_EXPECT(ms_mul_size(MS_SIZE_MAX, 1, &out));
    MT_EXPECT(out == MS_SIZE_MAX);

    out = 0;
    MT_EXPECT(ms_mul_size(MS_SIZE_MAX / 2, 2, &out));
    MT_EXPECT(out == MS_SIZE_MAX - 1);
}

static void mul_rejects_wrapping_products(void)
{
    SIZE_T out = 0x5a5a;
    MT_EXPECT(!ms_mul_size(MS_SIZE_MAX, 2, &out));
    MT_EXPECT(out == 0x5a5a); /* *out is left unchanged on failure */

    MT_EXPECT(!ms_mul_size(MS_SIZE_MAX / 2 + 1, 2, &out));
    MT_EXPECT(!ms_mul_size(2, MS_SIZE_MAX / 2 + 1, &out));
    MT_EXPECT(!ms_mul_size(MS_SIZE_MAX, MS_SIZE_MAX, &out));
    MT_EXPECT(out == 0x5a5a);
}

/* CRT-1: sprintf never writes past MS_SPRINTF_LIMIT bytes. */
static void sprintf_write_size_is_output_plus_nul(void)
{
    MT_EXPECT_EQ(ms_sprintf_write_size(0), 1);
    MT_EXPECT_EQ(ms_sprintf_write_size(5), 6);
    MT_EXPECT_EQ(ms_sprintf_write_size(MS_SPRINTF_LIMIT - 2), MS_SPRINTF_LIMIT - 1);
    MT_EXPECT_EQ(ms_sprintf_write_size(MS_SPRINTF_LIMIT - 1), MS_SPRINTF_LIMIT);
}

static void sprintf_write_size_is_bounded(void)
{
    MT_EXPECT_EQ(ms_sprintf_write_size(MS_SPRINTF_LIMIT), MS_SPRINTF_LIMIT);
    MT_EXPECT_EQ(ms_sprintf_write_size(MS_SPRINTF_LIMIT * 8), MS_SPRINTF_LIMIT);
    MT_EXPECT(ms_sprintf_write_size((SIZE_T)-1) == MS_SPRINTF_LIMIT);
}

static const mt_case_t cases[] = {
    MT_CASE(mul_accepts_products_that_fit),
    MT_CASE(mul_rejects_wrapping_products),
    MT_CASE(sprintf_write_size_is_output_plus_nul),
    MT_CASE(sprintf_write_size_is_bounded),
};

int main(void)
{
    return MT_RUN("msvcrt_policy", cases);
}
