/*
 * test_k32_mm_pure.c - host tests for src/ntdll/k32_mm_pure.h: the section
 * protection and allocation attribute checks, the view protection from
 * FILE_MAP_* access, and the offset alignment and size rules.
 */
#include "minitest.h"
#include "k32_mm_pure.h"

/* Section protections are the base values; PAGE_NOACCESS and bare
 * PAGE_EXECUTE are not section protections. */
static void section_protect_values(void)
{
    MT_EXPECT_EQ(k32m_section_protect(K32M_PAGE_READONLY), K32M_PAGE_READONLY);
    MT_EXPECT_EQ(k32m_section_protect(K32M_PAGE_READWRITE | K32M_SEC_COMMIT), K32M_PAGE_READWRITE);
    MT_EXPECT_EQ(k32m_section_protect(K32M_PAGE_EXECUTE_WRITECOPY), K32M_PAGE_EXECUTE_WRITECOPY);
    MT_EXPECT_EQ(k32m_section_protect(K32M_PAGE_NOACCESS), 0);
    MT_EXPECT_EQ(k32m_section_protect(K32M_PAGE_EXECUTE), 0);
    MT_EXPECT_EQ(k32m_section_protect(0), 0);
}

/* SEC_RESERVE and SEC_COMMIT are both set: invalid. Neither set: committed. */
static void section_alloc_attrs(void)
{
    MT_EXPECT_EQ(k32m_section_alloc_attrs(K32M_PAGE_READWRITE), K32M_SEC_COMMIT);
    MT_EXPECT_EQ(k32m_section_alloc_attrs(K32M_PAGE_READWRITE | K32M_SEC_COMMIT), K32M_SEC_COMMIT);
    MT_EXPECT_EQ(k32m_section_alloc_attrs(K32M_PAGE_READWRITE | K32M_SEC_RESERVE), K32M_SEC_RESERVE);
    MT_EXPECT_EQ(k32m_section_alloc_attrs(K32M_PAGE_READWRITE | K32M_SEC_RESERVE | K32M_SEC_COMMIT), 0);
    MT_EXPECT_EQ(k32m_section_alloc_attrs(K32M_PAGE_READONLY | K32M_SEC_IMAGE), K32M_SEC_IMAGE);
    MT_EXPECT_EQ(k32m_section_alloc_attrs(K32M_PAGE_READONLY | K32M_SEC_IMAGE | K32M_SEC_COMMIT), 0);
    MT_EXPECT_EQ(k32m_section_alloc_attrs(K32M_PAGE_READWRITE | K32M_SEC_COMMIT | K32M_SEC_NOCACHE),
                 K32M_SEC_COMMIT | K32M_SEC_NOCACHE);
}

/* A valid flProtect has a valid protection and valid attributes. */
static void section_flags_check(void)
{
    MT_EXPECT(k32m_section_flags_ok(K32M_PAGE_READWRITE | K32M_SEC_COMMIT) == 1);
    MT_EXPECT(k32m_section_flags_ok(K32M_PAGE_READWRITE | K32M_SEC_RESERVE) == 1);
    MT_EXPECT(k32m_section_flags_ok(K32M_PAGE_NOACCESS | K32M_SEC_COMMIT) == 0);
    MT_EXPECT(k32m_section_flags_ok(K32M_PAGE_READWRITE | K32M_SEC_RESERVE | K32M_SEC_COMMIT) == 0);
}

/* FILE_MAP_WRITE gives PAGE_READWRITE, COPY gives PAGE_WRITECOPY, READ gives
 * PAGE_READONLY; the execute bit adds the EXECUTE forms. */
static void view_protect_from_access(void)
{
    MT_EXPECT_EQ(k32m_view_protect(K32M_FILE_MAP_READ), K32M_PAGE_READONLY);
    MT_EXPECT_EQ(k32m_view_protect(K32M_FILE_MAP_WRITE), K32M_PAGE_READWRITE);
    MT_EXPECT_EQ(k32m_view_protect(K32M_FILE_MAP_COPY), K32M_PAGE_WRITECOPY);
    MT_EXPECT_EQ(k32m_view_protect(K32M_FILE_MAP_READ | K32M_FILE_MAP_EXECUTE), K32M_PAGE_EXECUTE_READ);
    MT_EXPECT_EQ(k32m_view_protect(K32M_FILE_MAP_WRITE | K32M_FILE_MAP_EXECUTE), K32M_PAGE_EXECUTE_READWRITE);
    MT_EXPECT_EQ(k32m_view_protect(K32M_FILE_MAP_COPY | K32M_FILE_MAP_EXECUTE), K32M_PAGE_EXECUTE_WRITECOPY);
}

/* No access bit, or a bit outside FILE_MAP_*, is invalid. */
static void view_protect_rejects_bad_access(void)
{
    MT_EXPECT_EQ(k32m_view_protect(0), 0);
    MT_EXPECT_EQ(k32m_view_protect(0x0008), 0);
    MT_EXPECT_EQ(k32m_view_protect(K32M_FILE_MAP_READ | 0x1000), 0);
}

/* Offsets must be a multiple of 64 KB, whatever the high word. */
static void offset_alignment(void)
{
    MT_EXPECT(k32m_offset_aligned(0, 0) == 1);
    MT_EXPECT(k32m_offset_aligned(0, 0x10000) == 1);
    MT_EXPECT(k32m_offset_aligned(7, 0x20000) == 1);
    MT_EXPECT(k32m_offset_aligned(0, 4096) == 0);
    MT_EXPECT(k32m_offset_aligned(1, 1) == 0);
}

/* The 64-bit offset is the high word shifted over the low word. */
static void offset_combines_halves(void)
{
    MT_EXPECT(k32m_offset(0, 0x1234) == 0x1234ULL);
    MT_EXPECT(k32m_offset(1, 0) == 0x100000000ULL);
    MT_EXPECT(k32m_offset(0xFFFFFFFFu, 0xFFFFFFFFu) == 0xFFFFFFFFFFFFFFFFULL);
}

/* An anonymous section needs a non-zero size; a file section may use zero
 * (the file's size). */
static void size_rules(void)
{
    MT_EXPECT(k32m_size_ok(1, 0, 0) == 0);
    MT_EXPECT(k32m_size_ok(1, 0, 65536) == 1);
    MT_EXPECT(k32m_size_ok(1, 1, 0) == 1);
    MT_EXPECT(k32m_size_ok(0, 0, 0) == 1);
}

static const mt_case_t cases[] = {
    { "section_protect_values", section_protect_values },
    { "section_alloc_attrs", section_alloc_attrs },
    { "section_flags_check", section_flags_check },
    { "view_protect_from_access", view_protect_from_access },
    { "view_protect_rejects_bad_access", view_protect_rejects_bad_access },
    { "offset_alignment", offset_alignment },
    { "offset_combines_halves", offset_combines_halves },
    { "size_rules", size_rules },
};

int main(void)
{
    return MT_RUN("k32_mm_pure", cases);
}
