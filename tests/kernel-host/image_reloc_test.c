/*
 * image_reloc_test.c - host tests for the SEC_IMAGE base relocation applier
 * (src/kernel/nt/image_reloc.h).
 *
 * Synthesized images are plain byte arrays laid out at their RVAs. The real
 * images are the MinGW fixtures in tests/host/out/fixtures, read as bytes and
 * never executed. The fixture test prints SKIP and passes when they are absent.
 */
#include "image_plan.h"
#include "image_reloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_fails;

#define CHECK(c) do { \
    g_checks++; \
    if (!(c)) { g_fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while (0)

#define FIX_DIR "../../host/out/fixtures/"

#define T_SIZE   0x3000u           /* SizeOfImage of the synthesized image */
#define T_PREF   0x140000000ULL    /* preferred base */
#define REL(t, o) ((uint16_t)(((t) << 12) | ((o) & 0xFFFu)))

static uint64_t ld(const uint8_t *p, unsigned w) { return ir_ld(p, w); }

static void put(uint8_t *img, uint32_t at, uint64_t v, unsigned w) { ir_st(img + at, v, w); }

/* Writes one block at `at`: VirtualAddress, SizeOfBlock, then the entries,
 * padded with an ABSOLUTE entry to a multiple of 4. Returns SizeOfBlock. */
static uint32_t put_block(uint8_t *img, uint32_t at, uint32_t page,
                          const uint16_t *ents, uint32_t n)
{
    uint32_t bsize = 8 + 2 * n;
    uint32_t i;
    if (bsize % 4) { bsize += 2; }
    put(img, at, page, 4);
    put(img, at + 4, bsize, 4);
    for (i = 0; i < n; i++) put(img, at + 8 + 2 * i, ents[i], 2);
    if (bsize != 8 + 2 * n) put(img, at + 8 + 2 * n, 0, 2);
    return bsize;
}

static void fill_pattern(uint8_t *img, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) img[i] = (uint8_t)(i * 13 + 7);
}

/* Applies with the default image and a directory at dir_rva. The seen bitmap
 * is allocated per call, as the planner caller does. */
static ir_status_t apply(uint8_t *img, size_t len, uint64_t actual,
                         uint32_t dir_rva, uint32_t dir_size, uint32_t *applied)
{
    uint8_t seen[T_SIZE / 8];
    return ir_apply(img, len, T_SIZE, T_PREF, actual, dir_rva, dir_size,
                    seen, sizeof seen, applied);
}

#define DIR_RVA  0x2000u
#define DIR_SIZE 20u

/* Every relocation kind writes the right value. The delta has low bits set, so
 * HIGH and LOW carry differently from each other and from DIR64. */
static int test_all_kinds(void)
{
    static uint8_t img[T_SIZE], snap[T_SIZE];
    uint16_t ents[5];
    uint32_t applied = 99;
    ir_status_t s;
    uint64_t delta = 0x12340ULL;

    fill_pattern(img, sizeof img);
    memset(img + DIR_RVA, 0, DIR_SIZE);
    put(img, 0x1010, 0x140001000ULL, 8);     /* DIR64 */
    put(img, 0x1020, 0xFFFFFFF0ULL, 4);      /* HIGHLOW: wraps into bit 32 */
    put(img, 0x1030, 0x1111, 2);             /* HIGH */
    put(img, 0x1032, 0xFFF0, 2);             /* LOW: wraps at 16 bits */
    ents[0] = REL(IR_REL_DIR64, 0x010);
    ents[1] = REL(IR_REL_HIGHLOW, 0x020);
    ents[2] = REL(IR_REL_HIGH, 0x030);
    ents[3] = REL(IR_REL_LOW, 0x032);
    ents[4] = REL(IR_REL_ABSOLUTE, 0x040);
    CHECK(put_block(img, DIR_RVA, 0x1000, ents, 5) == DIR_SIZE);
    memcpy(snap, img, sizeof img);

    s = apply(img, sizeof img, T_PREF + delta, DIR_RVA, DIR_SIZE, &applied);
    CHECK(s == IR_OK);
    CHECK(applied == 4);                     /* ABSOLUTE is not counted */
    CHECK(ld(img + 0x1010, 8) == 0x140001000ULL + delta);
    CHECK(ld(img + 0x1020, 4) == ((0xFFFFFFF0ULL + delta) & 0xFFFFFFFFULL));
    CHECK(ld(img + 0x1030, 2) == 0x1111 + (delta >> 16));
    CHECK(ld(img + 0x1032, 2) == ((0xFFF0ULL + (delta & 0xFFFF)) & 0xFFFF));
    CHECK(ld(img + 0x1032, 2) == 0x2330);
    /* Only the fixup bytes changed: the pattern elsewhere is intact. */
    CHECK(memcmp(img + 0x1040, snap + 0x1040, 8) == 0);
    CHECK(memcmp(img + 0x1000, snap + 0x1000, 0x10) == 0);
    return 1;
}

/* A view below the preferred base wraps negatively; the result is exact. */
static int test_negative_delta(void)
{
    static uint8_t img[T_SIZE];
    uint16_t ents[2];
    uint32_t applied = 99;
    fill_pattern(img, sizeof img);
    memset(img + DIR_RVA, 0, DIR_SIZE);
    put(img, 0x1010, 0x140001000ULL, 8);
    ents[0] = REL(IR_REL_DIR64, 0x010);
    ents[1] = REL(IR_REL_ABSOLUTE, 0);
    put_block(img, DIR_RVA, 0x1000, ents, 2);
    CHECK(apply(img, sizeof img, T_PREF - 0x1000, DIR_RVA, 12, &applied) == IR_OK);
    CHECK(applied == 1);
    CHECK(ld(img + 0x1010, 8) == 0x140000000ULL);
    return 1;
}

/* Several blocks on different pages are all applied; a block whose page is the
 * last page of SizeOfImage has a fixup that ends exactly at SizeOfImage. */
static int test_multiple_blocks_and_edge(void)
{
    static uint8_t img[T_SIZE];
    uint16_t ents[2];
    uint32_t applied = 0, n;
    uint64_t delta = 0x1000;
    fill_pattern(img, sizeof img);
    put(img, 0x1010, 0x100ULL, 8);
    put(img, 0x2010, 0x200ULL, 8);
    put(img, 0x2FF8, 0x300ULL, 8);
    ents[0] = REL(IR_REL_DIR64, 0x010);
    ents[1] = REL(IR_REL_ABSOLUTE, 0);
    n = put_block(img, 0x2800, 0x1000, ents, 2);
    ents[0] = REL(IR_REL_DIR64, 0x010);
    put_block(img, 0x2800 + n, 0x2000, ents, 1);
    ents[0] = REL(IR_REL_DIR64, 0xFF8);
    put_block(img, 0x2800 + 2 * n, 0x2000, ents, 1);
    /* Third block is at page 0x2000 with offset 0xFF8, so the fixup is 0x2FF8. */
    CHECK(apply(img, sizeof img, T_PREF + delta, 0x2800, 2 * n + 12, &applied) == IR_OK);
    CHECK(applied == 3);
    CHECK(ld(img + 0x1010, 8) == 0x100ULL + delta);
    CHECK(ld(img + 0x2010, 8) == 0x200ULL + delta);
    CHECK(ld(img + 0x2FF8, 8) == 0x300ULL + delta);
    return 1;
}

/* No directory, a zero delta and an ABSOLUTE-only directory all apply nothing. */
static int test_nothing_to_apply(void)
{
    static uint8_t img[T_SIZE], snap[T_SIZE];
    uint16_t ents[1];
    uint32_t applied = 99;
    fill_pattern(img, sizeof img);
    memset(img + DIR_RVA, 0, DIR_SIZE);
    ents[0] = REL(IR_REL_ABSOLUTE, 0x010);
    put_block(img, DIR_RVA, 0x1000, ents, 1);
    memcpy(snap, img, sizeof img);

    CHECK(apply(img, sizeof img, T_PREF, DIR_RVA, DIR_SIZE, &applied) == IR_OK);
    CHECK(applied == 0);
    CHECK(memcmp(img, snap, sizeof img) == 0);

    CHECK(apply(img, sizeof img, T_PREF + 0x1000, DIR_RVA, DIR_SIZE, &applied) == IR_OK);
    CHECK(applied == 0);
    CHECK(memcmp(img, snap, sizeof img) == 0);

    CHECK(apply(img, sizeof img, T_PREF + 0x1000, 0, 0, &applied) == IR_OK);
    CHECK(applied == 0);
    CHECK(apply(img, sizeof img, T_PREF + 0x1000, 0, 12, &applied) == IR_OK);   /* rva 0 means none */
    CHECK(applied == 0);
    return 1;
}

/* A failed call leaves the image unchanged, even when earlier fixups in the
 * same directory are valid. Each failure below is checked on a fresh copy. */
static int expect_refused(const char *what, uint8_t *img, const uint8_t *snap, size_t len,
                          uint32_t dir_rva, uint32_t dir_size, ir_status_t want)
{
    uint32_t applied = 99;
    ir_status_t s = apply(img, len, T_PREF + 0x1000, dir_rva, dir_size, &applied);
    if (s != want) fprintf(stderr, "  %s: got %d want %d\n", what, (int)s, (int)want);
    CHECK(s == want);
    CHECK(applied == 0);
    CHECK(memcmp(img, snap, len) == 0);
    return s == want;
}

static int test_truncated_and_block_sizes(void)
{
    static uint8_t img[T_SIZE], snap[T_SIZE];
    uint16_t ents[2];
    fill_pattern(img, sizeof img);
    memset(img + DIR_RVA, 0, 0x100);
    put(img, 0x1010, 0x100ULL, 8);
    ents[0] = REL(IR_REL_DIR64, 0x010);
    ents[1] = REL(IR_REL_ABSOLUTE, 0);
    put_block(img, DIR_RVA, 0x1000, ents, 2);       /* 12 bytes */
    memcpy(snap, img, sizeof img);

    /* Directory shorter than one block header. */
    memcpy(snap, img, sizeof img);
    expect_refused("dir 6 bytes", img, snap, sizeof img, DIR_RVA, 6, IR_ERR_BLOCK);
    /* Block claims more bytes than the directory has. */
    memcpy(snap, img, sizeof img);
    expect_refused("block past dir", img, snap, sizeof img, DIR_RVA, 8, IR_ERR_BLOCK);
    /* A trailing fragment of 4 bytes after a valid block. */
    memcpy(snap, img, sizeof img);
    expect_refused("trailing fragment", img, snap, sizeof img, DIR_RVA, 16, IR_ERR_BLOCK);

    /* SizeOfBlock under the header size, and not a multiple of 4. */
    put(img, DIR_RVA + 4, 4, 4);
    memcpy(snap, img, sizeof img);
    expect_refused("block size 4", img, snap, sizeof img, DIR_RVA, 12, IR_ERR_BLOCK);
    put(img, DIR_RVA + 4, 6, 4);
    memcpy(snap, img, sizeof img);
    expect_refused("block size 6", img, snap, sizeof img, DIR_RVA, 12, IR_ERR_BLOCK);
    /* Huge SizeOfBlock must not wrap the cursor. */
    put(img, DIR_RVA + 4, 0xFFFFFFFCu, 4);
    memcpy(snap, img, sizeof img);
    expect_refused("block size 0xFFFFFFFC", img, snap, sizeof img, DIR_RVA, 0x100, IR_ERR_BLOCK);
    put(img, DIR_RVA + 4, 12, 4);
    CHECK(apply(img, sizeof img, T_PREF + 0x1000, DIR_RVA, 12, NULL) == IR_ERR_PARAM);
    return 1;
}

static int test_page_and_target_range(void)
{
    static uint8_t img[T_SIZE], snap[T_SIZE];
    uint16_t ents[2];
    uint32_t n;
    fill_pattern(img, sizeof img);
    memset(img + DIR_RVA, 0, DIR_SIZE);

    /* Block VirtualAddress at SizeOfImage. */
    put(img, DIR_RVA, T_SIZE, 4);
    put(img, DIR_RVA + 4, 12, 4);
    ents[0] = REL(IR_REL_ABSOLUTE, 0);
    put(img, DIR_RVA + 8, REL(IR_REL_ABSOLUTE, 0), 2);
    put(img, DIR_RVA + 10, 0, 2);
    memcpy(snap, img, sizeof img);
    memcpy(snap, img, sizeof img);
    expect_refused("page at SizeOfImage", img, snap, sizeof img, DIR_RVA, 12, IR_ERR_PAGE);
    put(img, DIR_RVA, 0xFFFFFFF0u, 4);
    memcpy(snap, img, sizeof img);
    expect_refused("page 0xFFFFFFF0", img, snap, sizeof img, DIR_RVA, 12, IR_ERR_PAGE);

    /* A valid fixup first, then a DIR64 that runs past SizeOfImage. */
    ents[0] = REL(IR_REL_DIR64, 0x010);
    ents[1] = REL(IR_REL_DIR64, 0xFFC);
    n = put_block(img, DIR_RVA, 0x2000, ents, 2);
    CHECK(n == 12);
    memcpy(snap, img, sizeof img);
    memcpy(snap, img, sizeof img);
    expect_refused("DIR64 past SizeOfImage", img, snap, sizeof img, DIR_RVA, 12, IR_ERR_TARGET);

    /* HIGHLOW whose 4 bytes start in the last 3 bytes of SizeOfImage. */
    ents[0] = REL(IR_REL_HIGHLOW, 0xFFE);
    ents[1] = REL(IR_REL_ABSOLUTE, 0);
    put_block(img, DIR_RVA, 0x2000, ents, 2);
    memcpy(snap, img, sizeof img);
    memcpy(snap, img, sizeof img);
    expect_refused("HIGHLOW past SizeOfImage", img, snap, sizeof img, DIR_RVA, 12, IR_ERR_TARGET);

    /* A DIR64 that ends exactly at SizeOfImage is inside and applies. */
    ents[0] = REL(IR_REL_DIR64, 0xFF8);
    put_block(img, DIR_RVA, 0x2000, ents, 1);
    put(img, 0x2FF8, 0x300ULL, 8);
    {
        uint32_t applied = 0;
        CHECK(apply(img, sizeof img, T_PREF + 0x1000, DIR_RVA, 12, &applied) == IR_OK);
        CHECK(applied == 1);
        CHECK(ld(img + 0x2FF8, 8) == 0x1300ULL);
    }
    return 1;
}

static int test_unsupported_types(void)
{
    static uint8_t img[T_SIZE], snap[T_SIZE];
    static const uint16_t bad[] = { 4, 5, 6, 7, 11, 12, 13, 14, 15 };
    uint16_t ents[2];
    unsigned k;
    fill_pattern(img, sizeof img);
    memset(img + DIR_RVA, 0, DIR_SIZE);
    put(img, 0x1010, 0x100ULL, 8);
    for (k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        ents[0] = REL(IR_REL_DIR64, 0x010);    /* valid fixup first */
        ents[1] = (uint16_t)(bad[k] << 12);
        put_block(img, DIR_RVA, 0x1000, ents, 2);
        memcpy(snap, img, sizeof img);
        expect_refused("unsupported type", img, snap, sizeof img, DIR_RVA, 12, IR_ERR_TYPE);
    }
    return 1;
}

static int test_overlap(void)
{
    static uint8_t img[T_SIZE], snap[T_SIZE];
    uint16_t ents[3];
    uint32_t applied = 0;
    fill_pattern(img, sizeof img);
    memset(img + DIR_RVA, 0, DIR_SIZE);

    /* HIGHLOW at 0x10 covers 0x10..0x13; LOW at 0x12 writes 0x12..0x13. */
    ents[0] = REL(IR_REL_HIGHLOW, 0x010);
    ents[1] = REL(IR_REL_LOW, 0x012);
    put_block(img, DIR_RVA, 0x1000, ents, 2);
    memcpy(snap, img, sizeof img);
    memcpy(snap, img, sizeof img);
    expect_refused("HIGHLOW and LOW overlap", img, snap, sizeof img, DIR_RVA, 12, IR_ERR_OVERLAP);

    /* DIR64 at 0x10 covers 0x10..0x17; HIGHLOW at 0x14 is inside it. */
    ents[0] = REL(IR_REL_DIR64, 0x010);
    ents[1] = REL(IR_REL_HIGHLOW, 0x014);
    put_block(img, DIR_RVA, 0x1000, ents, 2);
    memcpy(snap, img, sizeof img);
    memcpy(snap, img, sizeof img);
    expect_refused("DIR64 and HIGHLOW overlap", img, snap, sizeof img, DIR_RVA, 12, IR_ERR_OVERLAP);

    /* Two fixups on the same page, the second one a block later. */
    ents[0] = REL(IR_REL_DIR64, 0x020);
    put_block(img, DIR_RVA, 0x1000, ents, 1);
    ents[0] = REL(IR_REL_DIR64, 0x024);
    put_block(img, DIR_RVA + 12, 0x1000, ents, 1);
    memcpy(snap, img, sizeof img);
    memcpy(snap, img, sizeof img);
    expect_refused("overlap across blocks", img, snap, sizeof img, DIR_RVA, 24, IR_ERR_OVERLAP);

    /* Adjacent DIR64s do not overlap and both apply. */
    ents[0] = REL(IR_REL_DIR64, 0x010);
    ents[1] = REL(IR_REL_DIR64, 0x018);
    put_block(img, DIR_RVA, 0x1000, ents, 2);
    put(img, 0x1010, 0x100ULL, 8);
    put(img, 0x1018, 0x200ULL, 8);
    CHECK(apply(img, sizeof img, T_PREF + 0x1000, DIR_RVA, 12, &applied) == IR_OK);
    CHECK(applied == 2);
    CHECK(ld(img + 0x1010, 8) == 0x1100ULL);
    CHECK(ld(img + 0x1018, 8) == 0x1200ULL);

    /* A fixup that writes into the directory itself. */
    ents[0] = REL(IR_REL_DIR64, 0x000);
    ents[1] = REL(IR_REL_ABSOLUTE, 0);
    put_block(img, DIR_RVA, 0x2000, ents, 2);
    memcpy(snap, img, sizeof img);
    memcpy(snap, img, sizeof img);
    expect_refused("fixup in directory", img, snap, sizeof img, DIR_RVA, 12, IR_ERR_OVERLAP);
    return 1;
}

static int test_parameters_and_overflow(void)
{
    static uint8_t img[T_SIZE];
    uint8_t seen[T_SIZE / 8];
    uint32_t applied = 0;
    fill_pattern(img, sizeof img);
    memset(img + DIR_RVA, 0, DIR_SIZE);

    CHECK(ir_apply(NULL, sizeof img, T_SIZE, T_PREF, T_PREF + 1, DIR_RVA, 12, seen, sizeof seen, &applied) == IR_ERR_PARAM);
    CHECK(ir_apply(img, sizeof img, T_SIZE, T_PREF, T_PREF + 1, DIR_RVA, 12, NULL, sizeof seen, &applied) == IR_ERR_PARAM);
    CHECK(ir_apply(img, sizeof img, T_SIZE, T_PREF, T_PREF + 1, DIR_RVA, 12, seen, sizeof seen, NULL) == IR_ERR_PARAM);
    CHECK(ir_apply(img, sizeof img, 0, T_PREF, T_PREF + 1, DIR_RVA, 12, seen, sizeof seen, &applied) == IR_ERR_PARAM);
    /* The buffer must hold all of SizeOfImage. */
    CHECK(ir_apply(img, T_SIZE - 1, T_SIZE, T_PREF, T_PREF + 1, DIR_RVA, 12, seen, sizeof seen, &applied) == IR_ERR_PARAM);
    /* The seen bitmap must cover SizeOfImage bits. */
    CHECK(ir_apply(img, sizeof img, T_SIZE, T_PREF, T_PREF + 1, DIR_RVA, 12, seen, sizeof seen - 1, &applied) == IR_ERR_PARAM);

    /* The directory must lie inside SizeOfImage, and must not wrap. */
    CHECK(apply(img, sizeof img, T_PREF + 1, 0x2FF0, 0x20, &applied) == IR_ERR_DIR);
    CHECK(apply(img, sizeof img, T_PREF + 1, 0xFFFFFFF0u, 0x20, &applied) == IR_ERR_DIR);
    CHECK(applied == 0);
    return 1;
}

/* The real fixtures: plan, lay out the bytes at their RVAs, apply, and compare
 * every fixup with the delta arithmetic. The fixups are enumerated here by a
 * second, independent walk of the directory. A reverse apply restores the
 * original bytes exactly. */
static uint8_t *read_fixture(const char *name, size_t *len)
{
    char path[256];
    FILE *f;
    long n;
    uint8_t *buf;
    snprintf(path, sizeof path, FIX_DIR "%s", name);
    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int check_fixture(const char *name)
{
    size_t len = 0;
    uint8_t *file = read_fixture(name, &len);
    ip_plan_t *p;
    uint8_t *img, *orig, *seen;
    uint64_t actual, delta;
    uint32_t expect = 0, applied = 0, cur = 0;
    uint32_t i;
    ir_status_t s;

    if (!file) {
        printf("  SKIP %s: fixture not built (run make in tests/host)\n", name);
        return 1;
    }
    p = calloc(1, sizeof *p);
    CHECK(p != NULL);
    if (!p || ip_plan_build(file, len, p) != IP_OK) { CHECK(0); free(p); free(file); return 0; }
    img = calloc(1, p->size_of_image);
    orig = calloc(1, p->size_of_image);
    seen = calloc(1, ir_seen_bytes(p->size_of_image));
    CHECK(img && orig && seen);
    if (!img || !orig || !seen) { free(p); free(file); free(img); free(orig); free(seen); return 0; }
    for (i = 0; i < p->num_segments; i++) {
        const ip_segment_t *sg = &p->seg[i];
        memcpy(img + sg->rva, file + sg->file_offset, sg->file_size);
    }
    memcpy(orig, img, p->size_of_image);

    if (!p->relocatable) {
        /* No directory (or RELOCS_STRIPPED): nothing to apply, bytes unchanged. */
        uint32_t none = 99;
        printf("  %s: no base relocations; checking the no-op path\n", name);
        CHECK(ir_apply(img, p->size_of_image, p->size_of_image, p->preferred_base,
                       p->preferred_base + 0x20000, p->reloc_rva, p->reloc_size,
                       seen, ir_seen_bytes(p->size_of_image), &none) == IR_OK);
        CHECK(none == 0);
        CHECK(memcmp(img, orig, p->size_of_image) == 0);
        free(seen); free(orig); free(img); free(p); free(file);
        return 1;
    }

    /* The fixture directory must lie inside the image (it does for MinGW output). */
    CHECK((uint64_t)p->reloc_rva + p->reloc_size <= p->size_of_image);
    if ((uint64_t)p->reloc_rva + p->reloc_size > p->size_of_image) { free(p); free(file); free(img); free(orig); free(seen); return 0; }

    /* Independent walk: count non-ABSOLUTE fixups, and check each is in range. */
    {
        uint32_t c = 0;
        while (c + 8 <= p->reloc_size) {
            uint32_t bsz = (uint32_t)ld(orig + p->reloc_rva + c + 4, 4);
            uint32_t k;
            if (bsz == 0) break;
            if (bsz < 8 || c + bsz > p->reloc_size) { CHECK(0); break; }
            for (k = 8; k < bsz; k += 2) {
                uint32_t e = (uint32_t)ld(orig + p->reloc_rva + c + k, 2);
                if ((e >> 12) != IR_REL_ABSOLUTE) expect++;
            }
            c += bsz;
        }
    }
    CHECK(expect > 0);

    delta = 0x20000ULL;
    actual = p->preferred_base + delta;
    s = ir_apply(img, p->size_of_image, p->size_of_image, p->preferred_base, actual,
                 p->reloc_rva, p->reloc_size, seen, ir_seen_bytes(p->size_of_image), &applied);
    CHECK(s == IR_OK);
    CHECK(applied == expect);
    /* Every fixup now holds old + delta. Compare the whole image: bytes that are
     * not fixup targets must be unchanged, so only fixup targets may differ. */
    {
        uint32_t c = 0;
        uint32_t changed = 0;
        for (i = 0; i < p->size_of_image; i++) if (img[i] != orig[i]) changed++;
        CHECK(changed > 0);
        while (c + 8 <= p->reloc_size) {
            uint32_t page = (uint32_t)ld(orig + p->reloc_rva + c, 4);
            uint32_t bsz = (uint32_t)ld(orig + p->reloc_rva + c + 4, 4);
            uint32_t k;
            if (bsz == 0) break;
            if (bsz < 8 || c + bsz > p->reloc_size) break;
            for (k = 8; k < bsz; k += 2) {
                uint32_t e = (uint32_t)ld(orig + p->reloc_rva + c + k, 2);
                uint32_t t = e >> 12;
                uint32_t target = page + (e & 0xFFF);
                if (t == IR_REL_DIR64) {
                    CHECK(ld(img + target, 8) == ld(orig + target, 8) + delta);
                } else if (t == IR_REL_HIGHLOW) {
                    CHECK(ld(img + target, 4) == ((ld(orig + target, 4) + delta) & 0xFFFFFFFFULL));
                } else if (t == IR_REL_HIGH) {
                    CHECK(ld(img + target, 2) == ((ld(orig + target, 2) + (delta >> 16)) & 0xFFFF));
                } else if (t == IR_REL_LOW) {
                    CHECK(ld(img + target, 2) == ((ld(orig + target, 2) + (delta & 0xFFFF)) & 0xFFFF));
                }
                cur++;
            }
            c += bsz;
        }
        CHECK(cur == expect);
    }
    /* Reverse: a view moved back to the preferred base restores the bytes. */
    s = ir_apply(img, p->size_of_image, p->size_of_image, actual, p->preferred_base,
                 p->reloc_rva, p->reloc_size, seen, ir_seen_bytes(p->size_of_image), &applied);
    CHECK(s == IR_OK);
    CHECK(memcmp(img, orig, p->size_of_image) == 0);

    free(seen); free(orig); free(img); free(p); free(file);
    return 1;
}

static int test_real_fixtures(void)
{
    static const char *names[] = { "minimal.exe", "exports.dll" };
    unsigned k;
    for (k = 0; k < sizeof names / sizeof names[0]; k++) check_fixture(names[k]);
    return 1;
}

typedef int (*test_fn)(void);

static const struct { const char *name; test_fn fn; } g_tests[] = {
    { "all_kinds", test_all_kinds },
    { "negative_delta", test_negative_delta },
    { "multiple_blocks_and_edge", test_multiple_blocks_and_edge },
    { "nothing_to_apply", test_nothing_to_apply },
    { "truncated_and_block_sizes", test_truncated_and_block_sizes },
    { "page_and_target_range", test_page_and_target_range },
    { "unsupported_types", test_unsupported_types },
    { "overlap", test_overlap },
    { "parameters_and_overflow", test_parameters_and_overflow },
    { "real_fixtures", test_real_fixtures },
};

int main(void)
{
    unsigned i;
    int before;
    for (i = 0; i < sizeof g_tests / sizeof g_tests[0]; i++) {
        before = g_fails;
        printf("TEST %s ... ", g_tests[i].name);
        g_tests[i].fn();
        printf("%s\n", g_fails == before ? "PASS" : "FAIL");
    }
    printf("image_reloc_test: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
