/* mman_test.c - host self-test for the shim's mmap/munmap emulation (mman_win.c).
 *
 * Checks the Linux contract the kernel code relies on: a range given to munmap
 * is unmapped, so MAP_FIXED_NOREPLACE may map it again, even though Windows
 * keeps a decommitted part of a reservation reserved. Built and run by
 * tests/kernel-host/Makefile (target mman_test); not part of src/kernel/nt.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { \
    failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define RW (PROT_READ | PROT_WRITE)
#define NOREPLACE (MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE)

/* Hole left by a partial munmap can be mapped again with MAP_FIXED_NOREPLACE. */
static void test_hole_reuse(void)
{
    char *a = mmap(NULL, 0x40000, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED, "reserve 256K");
    if (a == MAP_FAILED) return;
    CHECK(munmap(a + 0x8000, 0x8000) == 0, "partial munmap");
    char *h = mmap(a + 0x8000, 0x8000, RW, NOREPLACE, -1, 0);
    CHECK(h == a + 0x8000, "NOREPLACE over partial-munmap hole (errno %d)", errno);
    if (h != MAP_FAILED) { memset(h, 1, 0x8000); CHECK(h[0x7fff] == 1, "hole writable"); }
    CHECK(munmap(a, 0x40000) == 0, "release");
}

/* NOREPLACE must refuse a range that still has a live mapping in it. */
static void test_noreplace_refuses_live(void)
{
    char *a = mmap(NULL, 0x40000, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED, "reserve 256K (live)");
    if (a == MAP_FAILED) return;
    CHECK(munmap(a + 0x8000, 0x8000) == 0, "partial munmap (live)");
    errno = 0;
    CHECK(mmap(a, 0x40000, RW, NOREPLACE, -1, 0) == MAP_FAILED && errno == EEXIST,
          "NOREPLACE over live + hole must fail with EEXIST");
    CHECK(mmap(a + 0x8000, 0x8000, RW, NOREPLACE, -1, 0) == a + 0x8000,
          "NOREPLACE over hole only after a refused range");
    CHECK(munmap(a, 0x40000) == 0, "release (live)");
}

/* A range that mixes a hole with never-mapped space is valid for NOREPLACE. */
static void test_hole_plus_free(void)
{
    char *a = mmap(NULL, 0x10000, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED, "reserve 64K (mixed)");
    if (a == MAP_FAILED) return;
    CHECK(munmap(a + 0x8000, 0x8000) == 0, "partial munmap (mixed)");
    char *m = mmap(a + 0x8000, 0x10000, RW, NOREPLACE, -1, 0);
    CHECK(m == a + 0x8000, "NOREPLACE over hole + free (errno %d)", errno);
    if (m != MAP_FAILED) { memset(m, 2, 0x10000); CHECK(m[0xffff] == 2, "mixed range writable"); }
    CHECK(munmap(a, 0x18000) == 0, "release (mixed)");
}

int main(void)
{
    test_hole_reuse();
    test_noreplace_refuses_live();
    test_hole_plus_free();
    if (failures) { printf("=== mman_test: %d failure(s) ===\n", failures); return 1; }
    printf("=== mman_test: all passed ===\n");
    return 0;
}
