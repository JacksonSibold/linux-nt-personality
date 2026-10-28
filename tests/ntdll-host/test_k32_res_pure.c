/*
 * test_k32_res_pure.c - tests for src/ntdll/k32_res_pure.h on synthetic PE images: lookup by number and by name, the
 * language fallback order, enumeration, flat-file mappings and damaged directories.
 */
#include "minitest.h"
#include "k32_res_pure.h"

#include <stdlib.h>

/* ---- a tiny PE builder ---------------------------------------------------------------------------------------------------- */

#define IMG_SIZE 0x4000
#define RSRC_RVA 0x1000
#define RSRC_RAW 0x400   /* where the section's bytes sit in the flat-file form */

static void w16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* The resource section is built into rs[] (offsets relative to its start). */
static uint8_t rs[0x1000];
static uint32_t rs_len;

static uint32_t alloc_rs(uint32_t n)
{
    uint32_t o = rs_len;
    rs_len += (n + 3) & ~3u;
    return o;
}

typedef struct {
    int is_name;
    uint32_t id;
    const char *name;
    uint32_t offset; /* target, high bit added for directories by the caller */
} bentry;

static uint32_t put_string(const char *s)
{
    uint32_t o = alloc_rs(2 + 2 * (uint32_t)strlen(s));
    w16(rs + o, (uint16_t)strlen(s));
    for (size_t i = 0; s[i]; ++i)
        w16(rs + o + 2 + 2 * i, (uint16_t)(unsigned char)s[i]);
    return o;
}

/* Writes a directory of the entries (named ones first, as the format requires). */
static uint32_t put_dir(const bentry *e, unsigned n)
{
    unsigned named = 0, ids = 0;
    uint32_t o;
    for (unsigned i = 0; i < n; ++i)
        e[i].is_name ? ++named : ++ids;
    o = alloc_rs(16 + 8 * n);
    w16(rs + o + 12, (uint16_t)named);
    w16(rs + o + 14, (uint16_t)ids);
    for (unsigned i = 0; i < n; ++i) {
        uint32_t nm = e[i].is_name ? (0x80000000u | put_string(e[i].name)) : e[i].id;
        w32(rs + o + 16 + 8 * i, nm);
        w32(rs + o + 16 + 8 * i + 4, e[i].offset);
    }
    return o;
}

static uint32_t put_data(uint32_t data_rva, uint32_t size, uint32_t cp)
{
    uint32_t o = alloc_rs(16);
    w32(rs + o, data_rva);
    w32(rs + o + 4, size);
    w32(rs + o + 8, cp);
    return o;
}

/* Assembles the PE: headers at 0, one section ".rsrc" at RSRC_RVA (raw at RSRC_RAW). image: sections at their RVAs. */
static uint8_t g_img[IMG_SIZE];

static void build(int image_layout)
{
    uint8_t *b = g_img;
    memset(b, 0, IMG_SIZE);
    b[0] = 'M';
    b[1] = 'Z';
    w32(b + 0x3C, 0x80);
    w32(b + 0x80, 0x00004550);
    w16(b + 0x80 + 4, 0x8664);
    w16(b + 0x80 + 6, 1);           /* one section */
    w16(b + 0x80 + 20, 240);        /* SizeOfOptionalHeader */
    w16(b + 0x80 + 24, 0x20B);
    w32(b + 0x80 + 24 + 56, IMG_SIZE);
    w32(b + 0x80 + 24 + 108, 16);
    w32(b + 0x80 + 24 + 112 + 16, RSRC_RVA);
    w32(b + 0x80 + 24 + 112 + 20, rs_len);
    {
        uint8_t *s = b + 0x80 + 24 + 240;
        memcpy(s, ".rsrc", 5);
        w32(s + 8, rs_len);
        w32(s + 12, RSRC_RVA);
        w32(s + 16, rs_len);
        w32(s + 20, RSRC_RAW);
    }
    memcpy(b + (image_layout ? RSRC_RVA : RSRC_RAW), rs, rs_len);
}

/* A tree: RT_STRING(6) / name 7 / languages {0x0409, 0x0407}; RT_RCDATA(10) / "CONFIG" / {neutral}, / name 3 / {0x0409}. */
static void make_tree(void)
{
    uint32_t d_s7_en, d_s7_de, d_cfg, d_r3, dir_s7, dir_cfg, dir_r3, dir_rc, dir_str, root;
    bentry e1[2], e2[1], e3[1], e4[2], e5[1], e6[1];
    rs_len = 0;
    memset(rs, 0, sizeof rs);
    root = alloc_rs(16 + 8 * 2); /* reserve the root; filled last */
    d_s7_en = put_data(RSRC_RVA + 0x800, 10, 1252);
    d_s7_de = put_data(RSRC_RVA + 0x820, 12, 1252);
    d_cfg = put_data(RSRC_RVA + 0x840, 4, 0);
    d_r3 = put_data(RSRC_RVA + 0x860, 5, 0);
    e1[0] = (bentry){0, 0x0407, NULL, d_s7_de};
    e1[1] = (bentry){0, 0x0409, NULL, d_s7_en};
    dir_s7 = put_dir(e1, 2);
    e2[0] = (bentry){0, 0, NULL, d_cfg};
    dir_cfg = put_dir(e2, 1);
    e3[0] = (bentry){0, 0x0409, NULL, d_r3};
    dir_r3 = put_dir(e3, 1);
    e4[0] = (bentry){1, 0, "CONFIG", 0x80000000u | dir_cfg};
    e4[1] = (bentry){0, 3, NULL, 0x80000000u | dir_r3};
    dir_rc = put_dir(e4, 2);
    e5[0] = (bentry){0, 7, NULL, 0x80000000u | dir_s7};
    dir_str = put_dir(e5, 1);
    e6[0] = (bentry){0, 6, NULL, 0x80000000u | dir_str};
    (void)e6;
    {
        /* root: types 6 and 10, in id order */
        w16(rs + root + 14, 2);
        w32(rs + root + 16, 6);
        w32(rs + root + 20, 0x80000000u | dir_str);
        w32(rs + root + 24, 10);
        w32(rs + root + 28, 0x80000000u | dir_rc);
    }
}

static kres_view open_view(int image)
{
    kres_view v;
    make_tree();
    build(image);
    if (!kres_open(&v, g_img, IMG_SIZE, image))
        mt_fail(__FILE__, __LINE__, "kres_open");
    return v;
}

static uint16_t U[64];
static const uint16_t *wide(const char *s)
{
    size_t i = 0;
    for (; s[i]; ++i)
        U[i] = (unsigned char)s[i];
    U[i] = 0;
    return U;
}

static void finds_by_id_and_name(void)
{
    kres_view v = open_view(1);
    uint32_t off;
    kres_data d;
    kres_key t6 = kres_key_id(6), n7 = kres_key_id(7), t10 = kres_key_id(10);
    uint16_t lang;
    MT_ASSERT(kres_find(&v, &t6, &n7, 0x0409, &off, &lang));
    MT_ASSERT(kres_data_of(&v, off, &d));
    MT_EXPECT_EQ((long)d.size, 10L);
    MT_EXPECT_EQ((long)d.rva, (long)(RSRC_RVA + 0x800));
    MT_EXPECT_EQ((long)d.codepage, 1252L);
    {
        kres_key cfg = kres_key_string(wide("config")); /* names ignore case */
        MT_ASSERT(kres_find(&v, &t10, &cfg, 0, &off, &lang));
        MT_ASSERT(kres_data_of(&v, off, &d));
        MT_EXPECT_EQ((long)d.size, 4L);
    }
    {
        kres_key by_number = kres_key_string(wide("#3")); /* "#3" is the number 3 */
        MT_ASSERT(kres_find(&v, &t10, &by_number, 0x0409, &off, &lang));
        MT_ASSERT(kres_data_of(&v, off, &d));
        MT_EXPECT_EQ((long)d.size, 5L);
    }
    {
        kres_key missing = kres_key_id(99);
        MT_EXPECT_EQ(kres_find(&v, &t6, &missing, 0, &off, &lang), -1); /* the name is missing */
        MT_EXPECT_EQ(kres_find(&v, &missing, &n7, 0, &off, &lang), 0);  /* the type is missing */
    }
}

static void language_fallback(void)
{
    kres_view v = open_view(1);
    uint32_t off;
    kres_data d;
    uint16_t lang;
    kres_key t6 = kres_key_id(6), n7 = kres_key_id(7);
    MT_ASSERT(kres_find(&v, &t6, &n7, 0x0407, &off, &lang)); /* exact: German */
    MT_ASSERT(kres_data_of(&v, off, &d));
    MT_EXPECT_EQ((long)d.size, 12L);
    MT_EXPECT_EQ((long)lang, 0x0407L);
    MT_ASSERT(kres_find(&v, &t6, &n7, 0x0809, &off, &lang)); /* UK English is absent: falls to US English */
    MT_EXPECT_EQ((long)lang, 0x0409L);
    MT_ASSERT(kres_find(&v, &t6, &n7, 0, &off, &lang)); /* neutral: no neutral entry, so US English */
    MT_EXPECT_EQ((long)lang, 0x0409L);
    {
        kres_key t10 = kres_key_id(10), cfg = kres_key_string(wide("CONFIG"));
        MT_ASSERT(kres_find(&v, &t10, &cfg, 0x0407, &off, &lang)); /* only a neutral entry exists */
        MT_EXPECT_EQ((long)lang, 0L);
    }
}

static void enumerates_each_level(void)
{
    kres_view v = open_view(1);
    kres_counts c;
    kres_entry e;
    uint16_t name[16];
    MT_ASSERT(kres_dir_counts(&v, 0, &c));
    MT_EXPECT(c.named == 0 && c.ids == 2);
    MT_ASSERT(kres_dir_entry(&v, 0, 0, &e));
    MT_EXPECT(!e.is_name && e.id == 6 && e.is_dir);
    MT_ASSERT(kres_dir_entry(&v, 0, 1, &e) && e.id == 10);
    MT_ASSERT(kres_dir_counts(&v, e.target, &c));
    MT_EXPECT(c.named == 1 && c.ids == 1);
    MT_ASSERT(kres_dir_entry(&v, e.target, 0, &e));
    MT_EXPECT(e.is_name);
    MT_EXPECT_EQ((long)kres_entry_name(&v, &e, name, 16), 6L);
    MT_EXPECT(name[0] == 'C' && name[5] == 'G');
    MT_EXPECT(!kres_dir_entry(&v, 0, 2, &e)); /* past the end */
}

static void flat_file_form_gives_the_same_answers(void)
{
    kres_view v = open_view(0);
    uint32_t off;
    kres_data d;
    uint16_t lang;
    kres_key t6 = kres_key_id(6), n7 = kres_key_id(7);
    MT_ASSERT(kres_find(&v, &t6, &n7, 0x0409, &off, &lang));
    MT_ASSERT(kres_data_of(&v, off, &d));
    MT_EXPECT_EQ((long)d.size, 10L);
    /* the data itself lies in the .rsrc section too; a data RVA outside every section does not resolve */
    MT_EXPECT(kres_ptr(&v, RSRC_RVA + 0x10, 4) != NULL);
    MT_EXPECT(kres_ptr(&v, 0x3000, 4) == NULL);
}

static void damaged_directories_are_refused(void)
{
    kres_view v = open_view(1);
    uint32_t off;
    uint16_t lang;
    kres_key t6 = kres_key_id(6), n7 = kres_key_id(7);
    /* an entry that points past the section */
    w32(g_img + RSRC_RVA + 20, 0x80000000u | 0xFFFF0);
    MT_EXPECT(kres_find(&v, &t6, &n7, 0, &off, &lang) <= 0);
    /* a directory claiming thousands of entries */
    make_tree();
    build(1);
    w16(g_img + RSRC_RVA + 14, 0xFFFF);
    {
        kres_entry e;
        kres_view v2;
        MT_ASSERT(kres_open(&v2, g_img, IMG_SIZE, 1));
        MT_EXPECT(!kres_dir_entry(&v2, 0, 60000, &e));
    }
    /* not a PE */
    {
        kres_view bad;
        uint8_t junk[128] = {0};
        MT_EXPECT(!kres_open(&bad, junk, sizeof junk, 1));
        MT_EXPECT(!kres_open(&bad, g_img, 16, 1));
    }
    /* a name entry whose string runs off the end */
    make_tree();
    build(1);
    {
        kres_view v3;
        kres_entry e;
        uint16_t buf[8];
        MT_ASSERT(kres_open(&v3, g_img, IMG_SIZE, 1));
        memset(&e, 0, sizeof e);
        e.is_name = 1;
        e.name_off = rs_len + 0x7F0;
        MT_EXPECT_EQ((long)kres_entry_name(&v3, &e, buf, 8), 0L);
    }
}

static const mt_case_t cases[] = {
    MT_CASE(finds_by_id_and_name), MT_CASE(language_fallback), MT_CASE(enumerates_each_level),
    MT_CASE(flat_file_form_gives_the_same_answers), MT_CASE(damaged_directories_are_refused),
};

int main(void)
{
    return MT_RUN("k32_res_pure", cases);
}
