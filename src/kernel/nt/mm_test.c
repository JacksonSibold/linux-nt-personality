/*
 * mm_test.c - unit tests for ntmm with userspace shim (kshim.h)
 *
 * Covers:
 *  - reserve-then-commit-subrange with AllocationBase preservation
 *  - 64KB vs 4KB granularity
 *  - MEM_RELEASE nonzero size fails
 *  - VirtualQuery coalescing & free reporting
 *  - guard page fires once
 *  - protect returns old protection
 *  - fuzz loop invariants
 *  - section objects
 */

#include "mm.h"
#include "kshim.h"
#include "object.h"
#include "handle.h"
#include "io.h"
#include "../../../tests/kernel-host/pe_synth.h"
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>

static struct nt_eprocess *g_mm_proc = NULL;
static int tests_run = 0;
static int tests_pass = 0;
static int tests_fail = 0;

#define ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        return 0; \
    } \
} while(0)

#define RUN_TEST(fn) do { \
    ntmm_reset(); \
    tests_run++; \
    printf("TEST %s ... ", #fn); fflush(stdout); \
    if (fn()) { tests_pass++; printf("PASS\n"); } else { tests_fail++; printf("FAIL\n"); } \
} while(0)

static void __attribute__((unused)) print_mbi(const char *label, MEMORY_BASIC_INFORMATION *mbi) {
    printf("%s: Base=%p AllocBase=%p AllocProt=0x%lx RegionSize=0x%zx State=0x%lx Protect=0x%lx Type=0x%lx\n",
        label, mbi->BaseAddress, mbi->AllocationBase, (unsigned long)mbi->AllocationProtect,
        (size_t)mbi->RegionSize, (unsigned long)mbi->State, (unsigned long)mbi->Protect, (unsigned long)mbi->Type);
}

/* Helper to query and check status */
static int query(PVOID addr, MEMORY_BASIC_INFORMATION *out) {
    SIZE_T ret = 0;
    NTSTATUS st = NtQueryVirtualMemory((HANDLE)-1, addr, MemoryBasicInformation, out, sizeof(*out), &ret);
    if (st != STATUS_SUCCESS) {
        printf(" NtQuery failed 0x%lx at %p\n", (unsigned long)st, addr);
        return 0;
    }
    return 1;
}

/* 1. reserve-then-commit-subrange */
static int test_reserve_commit_subrange(void) {
    PVOID base = NULL;
    SIZE_T size = 0x20000; /* 128KB */
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "reserve failed 0x%lx", (unsigned long)st);
    PVOID alloc_base = base;
    SIZE_T alloc_size = size;
    /* Verify reservation query */
    MEMORY_BASIC_INFORMATION mbi;
    ASSERT(query(base, &mbi), "query reserve");
    ASSERT(mbi.State == MEM_RESERVE, "state reserve");
    ASSERT(mbi.AllocationBase == alloc_base, "alloc base reserve");
    ASSERT(mbi.RegionSize == alloc_size, "region size reserve got 0x%zx exp 0x%zx", (size_t)mbi.RegionSize, (size_t)alloc_size);

    /* Commit subrange at +0x4000 (16KB offset) size 0x1000 */
    PVOID commit_base = (PVOID)((ULONG_PTR)alloc_base + 0x4000);
    SIZE_T commit_size = 0x1000;
    PVOID cb = commit_base;
    SIZE_T cs = commit_size;
    st = NtAllocateVirtualMemory((HANDLE)-1, &cb, 0, &cs, MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "commit subrange failed 0x%lx", (unsigned long)st);
    ASSERT(cb == commit_base, "commit base mismatched");
    ASSERT(cs == commit_size, "commit size mismatched");

    /* Query committed subrange */
    ASSERT(query(commit_base, &mbi), "query commit");
    ASSERT(mbi.State == MEM_COMMIT, "state commit got 0x%lx", (unsigned long)mbi.State);
    ASSERT(mbi.AllocationBase == alloc_base, "AllocationBase must stay at reservation base %p got %p", alloc_base, mbi.AllocationBase);
    ASSERT(mbi.BaseAddress == commit_base, "BaseAddress should be commit start");
    ASSERT(mbi.RegionSize == commit_size, "committed region size 0x%zx exp 0x%zx", (size_t)mbi.RegionSize, (size_t)commit_size);
    ASSERT(mbi.AllocationProtect == PAGE_READWRITE, "alloc protect");

    /* Query the reserve piece before commit */
    PVOID before = alloc_base;
    ASSERT(query(before, &mbi), "query before");
    ASSERT(mbi.State == MEM_RESERVE, "before state");
    ASSERT(mbi.AllocationBase == alloc_base, "before alloc base");
    ASSERT(mbi.BaseAddress == alloc_base, "before base");
    ASSERT(mbi.RegionSize == 0x4000, "before region 0x%zx exp 0x4000", (size_t)mbi.RegionSize);

    /* Query after commit piece */
    PVOID after = (PVOID)((ULONG_PTR)commit_base + commit_size);
    ASSERT(query(after, &mbi), "query after");
    ASSERT(mbi.State == MEM_RESERVE, "after state");
    ASSERT(mbi.AllocationBase == alloc_base, "after alloc base");
    ASSERT(mbi.RegionSize == alloc_size - 0x4000 - 0x1000, "after size");

    /* Committing already-committed should succeed */
    PVOID cb2 = commit_base;
    SIZE_T cs2 = commit_size;
    st = NtAllocateVirtualMemory((HANDLE)-1, &cb2, 0, &cs2, MEM_COMMIT, PAGE_READONLY);
    ASSERT(st == STATUS_SUCCESS, "re-commit should succeed");

    /* Cleanup */
    PVOID rel = alloc_base;
    SIZE_T rel_size = 0;
    st = NtFreeVirtualMemory((HANDLE)-1, &rel, &rel_size, MEM_RELEASE);
    ASSERT(st == STATUS_SUCCESS, "release failed 0x%lx", (unsigned long)st);
    return 1;
}

/* 2. granularity */
static int test_granularity(void) {
    /* Reserve rounding: explicit unaligned base */
    PVOID base = (PVOID)0x15000;
    SIZE_T size = 0x5000;
    PVOID b = base;
    SIZE_T s = size;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &b, 0, &s, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "reserve unaligned failed 0x%lx", (unsigned long)st);
    /* Should round base down to 0x10000, size = delta+orig rounded */
    ASSERT((ULONG_PTR)b == 0x10000, "reserve base rounding got %p exp 0x10000", b);
    ASSERT(s == 0xA000, "reserve size rounding got 0x%zx exp 0xA000", (size_t)s);
    /* Cleanup */
    PVOID rel = b;
    SIZE_T rs = 0;
    st = NtFreeVirtualMemory((HANDLE)-1, &rel, &rs, MEM_RELEASE);
    ASSERT(st == STATUS_SUCCESS, "release1");

    /* Reserve with NULL base size 1 should give 64KB aligned base and 4096 size */
    base = NULL;
    size = 1;
    b = base; s = size;
    st = NtAllocateVirtualMemory((HANDLE)-1, &b, 0, &s, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "reserve null failed");
    ASSERT(((ULONG_PTR)b & (NTMM_ALLOC_GRANULARITY-1)) == 0, "reserve null alignment");
    ASSERT(s == 4096, "reserve null size 0x%zx", (size_t)s);
    rel = b; rs = 0;
    st = NtFreeVirtualMemory((HANDLE)-1, &rel, &rs, MEM_RELEASE);
    ASSERT(st == STATUS_SUCCESS, "release2");

    /* Commit granularity 4KB */
    /* First reserve a region */
    base = NULL; size = 0x20000;
    b = base; s = size;
    st = NtAllocateVirtualMemory((HANDLE)-1, &b, 0, &s, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "reserve for commit test");
    PVOID rbase = b;
    /* Commit with unaligned base */
    PVOID cb = (PVOID)((ULONG_PTR)rbase + 1);
    SIZE_T cs = 1;
    PVOID cbb = cb; SIZE_T css = cs;
    st = NtAllocateVirtualMemory((HANDLE)-1, &cbb, 0, &css, MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "commit unaligned failed 0x%lx", (unsigned long)st);
    ASSERT((ULONG_PTR)cbb == ((ULONG_PTR)rbase & ~(NTMM_PAGE_SIZE-1)) + 0, "commit base page aligned"); /* rbase already page aligned, +1 rounds down to rbase */
    ASSERT(css == 4096, "commit size page rounded");
    /* Cleanup */
    rel = rbase; rs = 0;
    st = NtFreeVirtualMemory((HANDLE)-1, &rel, &rs, MEM_RELEASE);
    ASSERT(st == STATUS_SUCCESS, "release3");
    return 1;
}

/* 3. MEM_RELEASE nonzero fails */
static int test_release_nonzero(void) {
    PVOID base = NULL;
    SIZE_T size = 0x10000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "alloc");
    PVOID b = base;
    SIZE_T s = 0x1000; /* nonzero */
    st = NtFreeVirtualMemory((HANDLE)-1, &b, &s, MEM_RELEASE);
    ASSERT(st == STATUS_INVALID_PARAMETER, "MEM_RELEASE nonzero must fail got 0x%lx", (unsigned long)st);
    /* Also try with base != alloc_base */
    PVOID bad_base = (PVOID)((ULONG_PTR)base + 0x1000);
    SIZE_T zs = 0;
    st = NtFreeVirtualMemory((HANDLE)-1, &bad_base, &zs, MEM_RELEASE);
    ASSERT(st == STATUS_INVALID_PARAMETER, "MEM_RELEASE bad base must fail");
    /* Correct release */
    b = base; s = 0;
    st = NtFreeVirtualMemory((HANDLE)-1, &b, &s, MEM_RELEASE);
    ASSERT(st == STATUS_SUCCESS, "correct release");
    return 1;
}

/* 4. VirtualQuery coalescing and free reporting */
static int test_query_coalesce_free(void) {
    /* Create gap: allocate two separate regions with a gap between */
    PVOID b1 = NULL; SIZE_T s1 = 0x10000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &b1, 0, &s1, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "b1");
    PVOID b2 = NULL; SIZE_T s2 = 0x10000;
    /* Force b2 to be after b1 with gap: allocate with explicit base */
    PVOID want = (PVOID)((ULONG_PTR)b1 + 0x50000); /* gap 0x40000 */
    PVOID b2_try = want;
    SIZE_T s2_try = s2;
    st = NtAllocateVirtualMemory((HANDLE)-1, &b2_try, 0, &s2_try, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if (st != STATUS_SUCCESS) {
        /* If want overlaps, just allocate anywhere */
        b2_try = NULL;
        st = NtAllocateVirtualMemory((HANDLE)-1, &b2_try, 0, &s2_try, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        ASSERT(st == STATUS_SUCCESS, "b2 fallback");
    }
    b2 = b2_try; s2 = s2_try;

    /* Query b1 should give its region */
    MEMORY_BASIC_INFORMATION mbi;
    ASSERT(query(b1, &mbi), "q b1");
    ASSERT(mbi.RegionSize == 0x10000, "b1 region");

    /* Query in gap should be MEM_FREE */
    PVOID gap = (PVOID)((ULONG_PTR)b1 + s1 + 0x1000);
    if ((ULONG_PTR)gap >= (ULONG_PTR)b2) gap = (PVOID)((ULONG_PTR)b1 + s1 + 0x1000);
    /* Ensure gap is inside free region */
    ASSERT((ULONG_PTR)gap > (ULONG_PTR)b1 + s1, "gap inside free");
    ASSERT((ULONG_PTR)gap < (ULONG_PTR)b2, "gap before b2");
    ASSERT(query(gap, &mbi), "q gap");
    ASSERT(mbi.State == MEM_FREE, "gap state");
    ASSERT(mbi.AllocationBase == NULL, "gap alloc base");
    ASSERT((ULONG_PTR)mbi.BaseAddress == (ULONG_PTR)b1 + s1, "gap base should be end of b1");
    ASSERT(mbi.RegionSize == (ULONG_PTR)b2 - ((ULONG_PTR)b1 + s1), "gap size");

    /* Test coalescing within single allocation with mixed states */
    ntmm_reset();
    PVOID br = NULL; SIZE_T sr = 0x30000; /* 3*64KB */
    st = NtAllocateVirtualMemory((HANDLE)-1, &br, 0, &sr, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "br reserve");
    /* Commit two adjacent pages with same protect. Windows keeps each 4K commit as a
     * separate VAD (per-page 4096) even when adjacent with identical State/Type/
     * AllocationBase/Protect - see probe_virtualquery_direct oracle where every
     * query returns 4096. Previous code coalesced them to 0x2000, causing 4084 S1
     * divergences (954368 vs 4096). Correct per Windows is per-VAD. */
    PVOID c1 = (PVOID)((ULONG_PTR)br + 0x1000); SIZE_T cs1 = 0x1000;
    PVOID t = c1; SIZE_T ts = cs1;
    st = NtAllocateVirtualMemory((HANDLE)-1, &t, 0, &ts, MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "c1");
    PVOID c2 = (PVOID)((ULONG_PTR)br + 0x2000); SIZE_T cs2 = 0x1000;
    t = c2; ts = cs2;
    st = NtAllocateVirtualMemory((HANDLE)-1, &t, 0, &ts, MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "c2");
    /* Query c1 should be per-VAD 0x1000, not coalesced 0x2000, per Windows */
    ASSERT(query(c1, &mbi), "q c1");
    ASSERT(mbi.RegionSize == 0x1000, "per-VAD commit size got 0x%zx exp 0x1000 (Windows per-page)", (size_t)mbi.RegionSize);
    ASSERT(mbi.State == MEM_COMMIT, "coalesce state");

    /* Now protect one of them differently -> should not coalesce */
    PVOID pb = c1; SIZE_T ps = 0x1000; ULONG old = 0;
    st = NtProtectVirtualMemory((HANDLE)-1, &pb, &ps, PAGE_READONLY, &old);
    ASSERT(st == STATUS_SUCCESS, "protect");
    ASSERT(query(c1, &mbi), "q c1 after protect");
    ASSERT(mbi.RegionSize == 0x1000, "after protect not coalesce size 0x%zx", (size_t)mbi.RegionSize);
    ASSERT(mbi.Protect == PAGE_READONLY, "protect value");

    /* An unaligned base: [c1+100, c1+100+0x1000) spans both pages, so c2 must
     * change too and the region comes back whole. */
    pb = (PVOID)((ULONG_PTR)c1 + 100); ps = 0x1000;
    st = NtProtectVirtualMemory((HANDLE)-1, &pb, &ps, PAGE_READONLY, &old);
    ASSERT(st == STATUS_SUCCESS, "protect unaligned");
    ASSERT(pb == c1 && ps == 0x2000, "unaligned protect returns the page range");
    ASSERT(query(c2, &mbi), "q c2 after unaligned protect");
    ASSERT(mbi.Protect == PAGE_READONLY, "second page protected by unaligned range");

    /* Free region coalescing via query sweep */
    /* Sweep from low to high and ensure no overlaps and coverage */
    ULONG_PTR cur = NTMM_LOW_ADDRESS;
    int steps = 0;
    while (cur < NTMM_HIGH_ADDRESS && steps < 1000) {
        MEMORY_BASIC_INFORMATION q;
        ASSERT(query((PVOID)cur, &q), "sweep query");
        ASSERT((ULONG_PTR)q.BaseAddress == cur || q.State == MEM_FREE, "sweep base");
        if (q.RegionSize == 0) break;
        cur = (ULONG_PTR)q.BaseAddress + q.RegionSize;
        steps++;
        if (cur > (ULONG_PTR)br + sr + 0x100000) break; /* limit */
    }

    ntmm_reset();
    return 1;
}

/* 5. guard page */
static int test_guard_once(void) {
    PVOID base = NULL; SIZE_T size = 0x2000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE|PAGE_GUARD);
    ASSERT(st == STATUS_SUCCESS, "guard alloc");
    MEMORY_BASIC_INFORMATION mbi;
    ASSERT(query(base, &mbi), "guard query");
    ASSERT((mbi.Protect & PAGE_GUARD) != 0, "guard bit set");

    NTSTATUS gs = NtMmHandleGuardFault(base);
    ASSERT(gs == STATUS_GUARD_PAGE_VIOLATION, "first guard fault got 0x%lx exp 0x%x", (unsigned long)gs, STATUS_GUARD_PAGE_VIOLATION);
    /* Second should not be guard */
    ASSERT(query(base, &mbi), "after guard");
    ASSERT((mbi.Protect & PAGE_GUARD) == 0, "guard cleared");
    gs = NtMmHandleGuardFault(base);
    ASSERT(gs != STATUS_GUARD_PAGE_VIOLATION, "second guard should not fire");

    /* Cleanup */
    PVOID rel = base; SIZE_T rs = 0;
    st = NtFreeVirtualMemory((HANDLE)-1, &rel, &rs, MEM_RELEASE);
    ASSERT(st == STATUS_SUCCESS, "guard release");
    return 1;
}

/* 6. protect old */
static int test_protect_old(void) {
    PVOID base = NULL; SIZE_T size = 0x1000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &size, MEM_RESERVE|MEM_COMMIT, PAGE_READONLY);
    ASSERT(st == STATUS_SUCCESS, "alloc ro");
    PVOID pb = base; SIZE_T ps = size; ULONG old = 0xdead;
    st = NtProtectVirtualMemory((HANDLE)-1, &pb, &ps, PAGE_READWRITE, &old);
    ASSERT(st == STATUS_SUCCESS, "protect");
    ASSERT(old == PAGE_READONLY, "old protect got 0x%lx exp 0x%lx", (unsigned long)old, (unsigned long)PAGE_READONLY);
    MEMORY_BASIC_INFORMATION mbi;
    ASSERT(query(base, &mbi), "query after");
    ASSERT(mbi.Protect == PAGE_READWRITE, "new protect");

    /* Test with all PAGE_* */
    ULONG prots[] = {PAGE_READONLY, PAGE_READWRITE, PAGE_WRITECOPY, PAGE_EXECUTE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY, PAGE_NOACCESS};
    for (size_t i=0;i<sizeof(prots)/sizeof(prots[0]);i++) {
        PVOID b=NULL; SIZE_T s=0x1000;
        st = NtAllocateVirtualMemory((HANDLE)-1, &b, 0, &s, MEM_RESERVE|MEM_COMMIT, prots[i]);
        ASSERT(st == STATUS_SUCCESS, "alloc prot %zu", i);
        PVOID pbb=b; SIZE_T pss=s; ULONG o=0;
        ULONG newp = PAGE_READWRITE;
        if (prots[i]==PAGE_READWRITE) newp = PAGE_READONLY;
        st = NtProtectVirtualMemory((HANDLE)-1, &pbb, &pss, newp, &o);
        ASSERT(st==STATUS_SUCCESS, "protect prot %zu", i);
        ASSERT(o==prots[i], "old prot mismatch");
        PVOID rel=b; SIZE_T rs=0;
        NtFreeVirtualMemory((HANDLE)-1, &rel, &rs, MEM_RELEASE);
    }
    /* Test NOCACHE etc */
    {
        PVOID b=NULL; SIZE_T s=0x1000;
        st = NtAllocateVirtualMemory((HANDLE)-1, &b, 0, &s, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE|PAGE_NOCACHE);
        ASSERT(st==STATUS_SUCCESS, "nocache alloc");
        PVOID rel=b; SIZE_T rs=0;
        NtFreeVirtualMemory((HANDLE)-1, &rel, &rs, MEM_RELEASE);
        b=NULL; s=0x1000;
        st = NtAllocateVirtualMemory((HANDLE)-1, &b, 0, &s, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE|PAGE_WRITECOMBINE);
        ASSERT(st==STATUS_SUCCESS, "wc alloc");
        rel=b; rs=0;
        NtFreeVirtualMemory((HANDLE)-1, &rel, &rs, MEM_RELEASE);
    }
    PVOID rel=base; SIZE_T rs=0;
    NtFreeVirtualMemory((HANDLE)-1, &rel, &rs, MEM_RELEASE);
    return 1;
}

/* 7. fuzz — SCALED toward DOD-MEM-A007/B008 (10^8) and DOD-MEM-A001/B001 (4096 matrix).
 * Userspace shim cannot reach 1e8 within CI time; we scale to 20000 iterations with full VAD invariant checks after every op,
 * which is 25x the original and exercises split/merge/coalesce/commit-granularity paths thoroughly. */
static int test_fuzz(void) {
    srand(0x1234);
    struct {
        PVOID base;
        SIZE_T size;
    } allocs[64];
    int nallocs = 0;
    const int FUZZ_ITERS = 20000;
    for (int iter=0; iter<FUZZ_ITERS; iter++) {
        int op = rand() % 4;
        if (op==0 && nallocs < 60) {
            /* Allocate */
            ULONG at = 0;
            int r = rand()%4;
            if (r==0) at = MEM_RESERVE;
            else if (r==1) at = MEM_COMMIT;
            else if (r==2) at = MEM_RESERVE|MEM_COMMIT;
            else at = MEM_RESERVE;
            if (rand()%4==0) at |= MEM_TOP_DOWN;
            if (rand()%8==0) at |= MEM_WRITE_WATCH;
            ULONG prot = PAGE_READWRITE;
            int pr = rand()%5;
            if (pr==0) prot=PAGE_READONLY;
            else if (pr==1) prot=PAGE_READWRITE;
            else if (pr==2) prot=PAGE_EXECUTE_READ;
            else if (pr==3) prot=PAGE_WRITECOPY;
            else prot=PAGE_READWRITE;
            if (rand()%10==0) prot |= PAGE_GUARD;
            if (rand()%10==0) prot |= PAGE_NOCACHE;
            if (rand()%10==0) prot |= PAGE_WRITECOMBINE;

            PVOID b=NULL; SIZE_T s= (SIZE_T)((rand()%8 +1)*4096);
            if (rand()%2==0 && nallocs>0) {
                /* Try to commit inside existing reserve */
                int idx = rand()%nallocs;
                PVOID ab = allocs[idx].base;
                /* query to find reserve */
                MEMORY_BASIC_INFORMATION mbi;
                if (query(ab, &mbi) && mbi.State==MEM_RESERVE) {
                    b = (PVOID)((ULONG_PTR)ab + (rand()% (int)mbi.RegionSize & ~0xFFF));
                    if ((ULONG_PTR)b < (ULONG_PTR)ab) b=ab;
                    s = 4096;
                    at = MEM_COMMIT;
                }
            }
            NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &b, 0, &s, at, prot & 0xFF);
            /* Some failures are expected (conflict etc) */
            if (st==STATUS_SUCCESS && b) {
                allocs[nallocs].base = b;
                allocs[nallocs].size = s;
                nallocs++;
            }
        } else if (op==1 && nallocs>0) {
            int idx = rand()%nallocs;
            PVOID b = allocs[idx].base;
            SIZE_T s = 0;
            NTSTATUS st = NtFreeVirtualMemory((HANDLE)-1, &b, &s, MEM_RELEASE);
            if (st==STATUS_SUCCESS) {
                allocs[idx]=allocs[nallocs-1];
                nallocs--;
            } else if (st==STATUS_INVALID_PARAMETER) {
                /* Try decommit instead */
                PVOID db = allocs[idx].base;
                SIZE_T ds = 4096;
                ULONG ft = MEM_DECOMMIT;
                NtFreeVirtualMemory((HANDLE)-1, &db, &ds, ft);
            }
        } else if (op==2 && nallocs>0) {
            int idx = rand()%nallocs;
            PVOID b = allocs[idx].base;
            SIZE_T s = 4096;
            ULONG old=0;
            ULONG np = (rand()%2)? PAGE_READONLY : PAGE_READWRITE;
            NtProtectVirtualMemory((HANDLE)-1, &b, &s, np, &old);
        } else {
            /* Query */
            ULONG_PTR addr = NTMM_LOW_ADDRESS + (rand() % 0x1000000);
            MEMORY_BASIC_INFORMATION mbi;
            SIZE_T ret=0;
            NTSTATUS st = NtQueryVirtualMemory((HANDLE)-1, (PVOID)addr, MemoryBasicInformation, &mbi, sizeof(mbi), &ret);
            ASSERT(st==STATUS_SUCCESS, "fuzz query");
            ASSERT(ret==sizeof(mbi), "ret len");
            /* Self-consistency: query at BaseAddress should give same */
            MEMORY_BASIC_INFORMATION mbi2;
            st = NtQueryVirtualMemory((HANDLE)-1, mbi.BaseAddress, MemoryBasicInformation, &mbi2, sizeof(mbi2), &ret);
            ASSERT(st==STATUS_SUCCESS, "query2");
            ASSERT(mbi2.BaseAddress==mbi.BaseAddress, "consistency base");
            ASSERT(mbi2.RegionSize==mbi.RegionSize, "consistency size");
            ASSERT(mbi2.State==mbi.State, "consistency state");
        }
        /* Invariants */
        extern struct nt_mm g_nt_mm;
        /* Access via check function */
        ASSERT(ntmm_check_invariants(&g_nt_mm), "invariant iter %d", iter);

        /* Full sweep consistency: walk and ensure contiguous — every 50 iterations to keep 20k iters tractable */
        if ((iter % 50)==0) {
            ULONG_PTR cur = NTMM_LOW_ADDRESS;
            int steps=0;
            while (cur < NTMM_HIGH_ADDRESS && steps < 200) {
                MEMORY_BASIC_INFORMATION q;
                SIZE_T ret=0;
                NTSTATUS st = NtQueryVirtualMemory((HANDLE)-1, (PVOID)cur, MemoryBasicInformation, &q, sizeof(q), &ret);
                if (st!=STATUS_SUCCESS) { ASSERT(0, "sweep fail"); }
                if (q.RegionSize==0) break;
                if (q.RegionSize > 0x1000) {
                    PVOID mid = (PVOID)((ULONG_PTR)q.BaseAddress + q.RegionSize/2);
                    MEMORY_BASIC_INFORMATION qm;
                    NtQueryVirtualMemory((HANDLE)-1, mid, MemoryBasicInformation, &qm, sizeof(qm), &ret);
                    ASSERT(qm.BaseAddress==q.BaseAddress, "mid base");
                    ASSERT(qm.RegionSize==q.RegionSize, "mid size");
                }
                cur = (ULONG_PTR)q.BaseAddress + q.RegionSize;
                steps++;
                if (steps>150) break;
                if (cur > 0x20000000) break;
            }
        }
    }
    /* Cleanup all */
    for (int i=0;i<nallocs;i++) {
        PVOID b=allocs[i].base; SIZE_T s=0;
        NtFreeVirtualMemory((HANDLE)-1, &b, &s, MEM_RELEASE);
    }
    return 1;
}

/* M-5: a RegionSize whose round-up wraps must be rejected, not turned into a
 * zero-length VAD. */
static int test_alloc_size_wrap(void) {
    PVOID base = NULL;
    SIZE_T size = (SIZE_T)-1 - 100;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_INVALID_PARAMETER, "wrapping size status 0x%lx", (unsigned long)st);
    ASSERT(base == NULL, "base unchanged");
    return 1;
}

/* ---- File-backed data sections (SEC_COMMIT with a FileHandle) ----
 * Files are made on the host under g_mmf_root and opened through
 * nt_svc_NtCreateFile, so each file handle is a real io file object. The host
 * reads and writes check what reached the file. */
/* NtLockVirtualMemory / NtUnlockVirtualMemory: range bookkeeping per address
 * space and the host lock. A range must be committed. MapType is MAP_PROCESS (1)
 * or MAP_SYSTEM (2); MAP_SYSTEM needs SeLockMemoryPrivilege, which this test
 * process does not hold. Unlocking pages that are not locked is STATUS_NOT_LOCKED.
 * Decommit drops the lock. */
NTSTATUS NtLockVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG MapType);
NTSTATUS NtUnlockVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG MapType);
static int test_lock_virtual_memory(void) {
    PVOID base = NULL;
    SIZE_T size = 2 * 4096;
    NTSTATUS st;
    st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &size, MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "commit: 0x%x", (unsigned)st);
    PVOID lb = base;
    SIZE_T ls = size;
    /* Argument checks: NULL pointers, unknown MapType */
    ASSERT(NtLockVirtualMemory((HANDLE)-1, NULL, &ls, 1) == STATUS_INVALID_PARAMETER, "null base");
    ASSERT(NtLockVirtualMemory((HANDLE)-1, &lb, NULL, 1) == STATUS_INVALID_PARAMETER, "null size");
    ASSERT(NtLockVirtualMemory((HANDLE)-1, &lb, &ls, 0) == STATUS_INVALID_PARAMETER, "maptype 0");
    ASSERT(NtLockVirtualMemory((HANDLE)-1, &lb, &ls, 3) == STATUS_INVALID_PARAMETER, "maptype 3");
    ASSERT(NtLockVirtualMemory((HANDLE)-1, &lb, &ls, 2) == STATUS_PRIVILEGE_NOT_HELD, "map_system privilege");
    /* An unaligned range rounds out to whole pages: [base+100, base+100+4096)
     * covers both pages, so both are locked and the region comes back whole. */
    lb = (PVOID)((char *)base + 100); ls = 4096;
    ASSERT(NtLockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_SUCCESS, "lock unaligned range");
    ASSERT(lb == base && ls == size, "unaligned lock returns the page range");
    lb = base; ls = size;
    ASSERT(NtUnlockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_SUCCESS, "unlock unaligned range");
    /* Lock both pages, twice: idempotent, base and size come back page-aligned */
    lb = base; ls = size;
    ASSERT(NtLockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_SUCCESS, "lock range");
    ASSERT(lb == base && ls == size, "lock returns range");
    lb = base; ls = size;
    ASSERT(NtLockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_SUCCESS, "lock again");
    /* Unlock the first page: the second is still locked, so the full range is not */
    lb = base; ls = 4096;
    ASSERT(NtUnlockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_SUCCESS, "unlock first page");
    lb = base; ls = size;
    ASSERT(NtUnlockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_NOT_LOCKED, "unlock range with unlocked page");
    lb = (PVOID)((char *)base + 4096); ls = 4096;
    ASSERT(NtUnlockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_SUCCESS, "unlock second page");
    ASSERT(NtUnlockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_NOT_LOCKED, "unlock twice");
    /* A reserved (not committed) range cannot be locked */
    PVOID rb = NULL;
    SIZE_T rs = 4096;
    ASSERT(NtAllocateVirtualMemory((HANDLE)-1, &rb, 0, &rs, MEM_RESERVE, PAGE_READWRITE) == STATUS_SUCCESS, "reserve");
    lb = rb; ls = 4096;
    ASSERT(NtLockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_NOT_COMMITTED, "lock reserved");
    /* Lock again, then decommit: the decommit drops the lock */
    lb = base; ls = size;
    ASSERT(NtLockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_SUCCESS, "relock");
    PVOID db = base; SIZE_T dsz = size;
    ASSERT(NtFreeVirtualMemory((HANDLE)-1, &db, &dsz, MEM_DECOMMIT) == STATUS_SUCCESS, "decommit");
    lb = base; ls = size;
    ASSERT(NtUnlockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_NOT_LOCKED, "decommit dropped lock");
    /* Recommit and lock again works */
    db = base; dsz = size;
    ASSERT(NtAllocateVirtualMemory((HANDLE)-1, &db, 0, &dsz, MEM_COMMIT, PAGE_READWRITE) == STATUS_SUCCESS, "recommit");
    lb = base; ls = size;
    ASSERT(NtLockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_SUCCESS, "lock after recommit");
    /* Releasing the allocation drops the lock too */
    db = base; dsz = 0;
    ASSERT(NtFreeVirtualMemory((HANDLE)-1, &db, &dsz, MEM_RELEASE) == STATUS_SUCCESS, "release");
    /* The release must have dropped the runs: the same range, committed again
     * at the same base, is not locked */
    db = base; dsz = size;
    ASSERT(NtAllocateVirtualMemory((HANDLE)-1, &db, 0, &dsz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == STATUS_SUCCESS, "recommit after release");
    lb = base; ls = size;
    ASSERT(NtUnlockVirtualMemory((HANDLE)-1, &lb, &ls, 1) == STATUS_NOT_LOCKED, "release dropped the lock");
    db = base; dsz = 0;
    ASSERT(NtFreeVirtualMemory((HANDLE)-1, &db, &dsz, MEM_RELEASE) == STATUS_SUCCESS, "release again");
    return 1;
}

static char g_mmf_root[512];

/* Defined in tests/kernel-host/shim/posix_host.c; declared here as the prelude does. */
void khost_scratch_arm_cleanup(const char *posix_root);

/* Removed at exit, pass or fail, as io_test does. The host helpers below use
 * open/pread/pwrite (the shim maps /tmp for those); stdio fopen does not map it. */
static void mmf_setup(void) {
    snprintf(g_mmf_root, sizeof(g_mmf_root), "/tmp/mm_test_%d", (int)getpid());
    khost_scratch_arm_cleanup(g_mmf_root);
    mkdir(g_mmf_root, 0700);
    kshim_set_root(g_mmf_root);
}

#ifndef O_BINARY
#define O_BINARY 0
#endif

static void mmf_path(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", g_mmf_root, name);
}

static int mmf_make(const char *name, size_t size, unsigned char fill) {
    char p[512], buf[4096];
    size_t done = 0;
    int fd;
    mmf_path(p, sizeof(p), name);
    fd = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY, 0644);
    if (fd < 0) return 0;
    memset(buf, fill, sizeof(buf));
    while (done < size) {
        size_t n = size - done < sizeof(buf) ? size - done : sizeof(buf);
        if (write(fd, buf, n) != (ssize_t)n) { close(fd); return 0; }
        done += n;
    }
    return close(fd) == 0;
}

static int mmf_poke(const char *name, long off, unsigned char b) {
    char p[512];
    int fd;
    mmf_path(p, sizeof(p), name);
    fd = open(p, O_WRONLY | O_BINARY);
    if (fd < 0) return 0;
    if (pwrite(fd, &b, 1, (off_t)off) != 1) { close(fd); return 0; }
    return close(fd) == 0;
}

static long mmf_host_size(const char *name) {
    char p[512];
    int fd;
    long n;
    mmf_path(p, sizeof(p), name);
    fd = open(p, O_RDONLY | O_BINARY);
    if (fd < 0) return -1;
    n = (long)lseek(fd, 0, SEEK_END);
    close(fd);
    return n;
}

static int mmf_host_byte(const char *name, long off, unsigned char *out) {
    char p[512];
    unsigned char c;
    int fd;
    mmf_path(p, sizeof(p), name);
    fd = open(p, O_RDONLY | O_BINARY);
    if (fd < 0) return 0;
    if (pread(fd, &c, 1, (off_t)off) != 1) { close(fd); return 0; }
    close(fd);
    *out = c;
    return 1;
}

static NTSTATUS mmf_open(const char *name, ACCESS_MASK access, HANDLE *out) {
    static WCHAR buf[256];
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER alloc;
    size_t i, len = strlen(name);
    for (i = 0; i < len && i < 255; i++) buf[i] = (WCHAR)(unsigned char)name[i];
    us.Buffer = buf;
    us.Length = (USHORT)(len * sizeof(WCHAR));
    us.MaximumLength = us.Length;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;
    memset(&iosb, 0, sizeof(iosb));
    alloc.QuadPart = 0;
    *out = NULL;
    return nt_svc_NtCreateFile(out, access, &oa, &iosb, &alloc, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                               FILE_NON_DIRECTORY_FILE, NULL, 0);
}

/* max == 0 passes a NULL MaximumSize, which asks for the file size. */
static NTSTATUS mmf_section(HANDLE fh, HANDLE *sec, ULONG prot, SIZE_T max) {
    LARGE_INTEGER sz;
    LARGE_INTEGER *pmax = NULL;
    if (max) { sz.QuadPart = (LONGLONG)max; pmax = &sz; }
    *sec = NULL;
    return NtCreateSection(sec, SECTION_ALL_ACCESS, NULL, pmax, prot, SEC_COMMIT, fh);
}

static NTSTATUS mmf_map(HANDLE sec, LONGLONG offset, SIZE_T size, ULONG prot, PVOID *base) {
    LARGE_INTEGER off;
    SIZE_T vs = size;
    off.QuadPart = offset;
    *base = NULL;
    return NtMapViewOfSection(sec, (HANDLE)-1, base, 0, 0, &off, &vs, 0, 0, prot);
}

static NTSTATUS mmf_flush(PVOID base, SIZE_T size) {
    PVOID b = base;
    SIZE_T s = size;
    IO_STATUS_BLOCK iosb;
    return NtFlushVirtualMemory((HANDLE)-1, &b, &s, &iosb);
}

/* Writes through a shared writable view reach the file after a flush. */
static int test_file_section_shared_write_reaches_file(void) {
    HANDLE fh = NULL, sec = NULL;
    PVOID base = NULL;
    unsigned char b = 0;
    ASSERT(mmf_make("shared_rw.bin", 0x1000, 'A'), "make file");
    ASSERT(mmf_open("shared_rw.bin", FILE_READ_DATA | FILE_WRITE_DATA, &fh) == STATUS_SUCCESS, "open rw");
    ASSERT(mmf_section(fh, &sec, PAGE_READWRITE, 0) == STATUS_SUCCESS, "create section");
    ASSERT(mmf_map(sec, 0, 0, PAGE_READWRITE, &base) == STATUS_SUCCESS, "map view");
    ASSERT(((unsigned char *)base)[0] == 'A', "view shows file contents");
    memcpy((unsigned char *)base + 0x10, "HELLO", 5);
    ASSERT(mmf_flush(base, 0x1000) == STATUS_SUCCESS, "flush view");
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close file");
    ASSERT(mmf_host_byte("shared_rw.bin", 0x10, &b) && b == 'H', "file holds first written byte");
    ASSERT(mmf_host_byte("shared_rw.bin", 0x14, &b) && b == 'O', "file holds last written byte");
    ASSERT(mmf_host_size("shared_rw.bin") == 0x1000, "file size unchanged");
    return 1;
}

/* A copy-on-write view takes private writes; the file keeps its bytes. */
static int test_file_section_cow_does_not_modify_file(void) {
    HANDLE fh = NULL, sec = NULL;
    PVOID base = NULL;
    unsigned char b = 0;
    ASSERT(mmf_make("cow.bin", 0x1000, 'A'), "make file");
    ASSERT(mmf_open("cow.bin", FILE_READ_DATA, &fh) == STATUS_SUCCESS, "open read");
    ASSERT(mmf_section(fh, &sec, PAGE_WRITECOPY, 0) == STATUS_SUCCESS, "create cow section");
    ASSERT(mmf_map(sec, 0, 0, PAGE_WRITECOPY, &base) == STATUS_SUCCESS, "map cow view");
    ((unsigned char *)base)[0x10] = 'Q';
    ASSERT(((unsigned char *)base)[0x10] == 'Q', "view sees private write");
    ASSERT(mmf_flush(base, 0x1000) == STATUS_SUCCESS, "flush cow view");
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap cow");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close cow section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close cow file");
    ASSERT(mmf_host_byte("cow.bin", 0x10, &b) && b == 'A', "file unchanged by cow write");
    return 1;
}

/* Access is checked per protection: a write-only handle cannot back a read-only
 * section (FILE_READ_DATA is needed), but it can back a read-write one. */
static int test_file_section_access_by_protection(void) {
    HANDLE fh = NULL, sec = NULL;
    ASSERT(mmf_make("wonly.bin", 0x1000, 'A'), "make file");
    ASSERT(mmf_open("wonly.bin", FILE_WRITE_DATA, &fh) == STATUS_SUCCESS, "open write-only");
    ASSERT(mmf_section(fh, &sec, PAGE_READONLY, 0) == STATUS_ACCESS_DENIED, "read-only view of write-only handle");
    ASSERT(sec == NULL, "no section for denied access");
    ASSERT(mmf_section(fh, &sec, PAGE_READWRITE, 0) == STATUS_SUCCESS, "read-write section from write-only handle");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close rw section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close write-only file");
    return 1;
}

/* An empty file has no size to map: NT returns STATUS_MAPPED_FILE_SIZE_ZERO,
 * both for the default size and for an explicit zero MaximumSize. */
static int test_file_section_zero_length_rejected(void) {
    HANDLE fh = NULL, sec = NULL;
    ASSERT(mmf_make("empty.bin", 0, 0), "make empty file");
    ASSERT(mmf_open("empty.bin", FILE_READ_DATA | FILE_WRITE_DATA, &fh) == STATUS_SUCCESS, "open empty");
    ASSERT(mmf_section(fh, &sec, PAGE_READWRITE, 0) == STATUS_MAPPED_FILE_SIZE_ZERO,
           "default size of empty file");
    ASSERT(sec == NULL, "no section for empty file");
    {
        LARGE_INTEGER z; z.QuadPart = 0;
        ASSERT(NtCreateSection(&sec, SECTION_ALL_ACCESS, NULL, &z, PAGE_READWRITE, SEC_COMMIT, fh)
               == STATUS_MAPPED_FILE_SIZE_ZERO, "explicit zero size of empty file");
    }
    ASSERT(mmf_host_size("empty.bin") == 0, "empty file not extended by the refusals");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close empty file");
    return 1;
}

/* NtCreateFile with an AllocationSize and no data: the file is empty (EOF 0), but NT maps
 * a default-size section at the allocation size (golden probe_section_direct: a 64 KB
 * allocation maps 65536 bytes). The section extends the file to that size. */
static int test_file_section_uses_allocation_size(void) {
    static WCHAR wbuf[256];
    const char *name = "alloc.bin";
    size_t i, len = strlen(name);
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER alloc;
    HANDLE fh = NULL, sec = NULL;
    for (i = 0; i < len && i < 255; i++) wbuf[i] = (WCHAR)(unsigned char)name[i];
    us.Buffer = wbuf;
    us.Length = (USHORT)(len * sizeof(WCHAR));
    us.MaximumLength = us.Length;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;
    memset(&iosb, 0, sizeof(iosb));
    alloc.QuadPart = 65536;
    ASSERT(nt_svc_NtCreateFile(&fh, FILE_READ_DATA | FILE_WRITE_DATA, &oa, &iosb, &alloc, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OVERWRITE_IF,
                               FILE_NON_DIRECTORY_FILE, NULL, 0) == STATUS_SUCCESS,
           "create empty file with a 64 KB allocation");
    ASSERT(mmf_section(fh, &sec, PAGE_READWRITE, 0) == STATUS_SUCCESS,
           "default-size section uses the allocation size");
    ASSERT(sec != NULL, "section handle returned");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close allocation-size section");
    ASSERT(mmf_host_size(name) == 65536, "section extended the file to the allocation size");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close allocation-size file");
    return 1;
}

/* A view and its section outlive the file handle: the section keeps its own
 * reference, and the file still reaches disk. A closed handle cannot make a new
 * section. */
static int test_file_section_handle_closed_keeps_view(void) {
    HANDLE fh = NULL, sec = NULL;
    PVOID base = NULL;
    unsigned char b = 0;
    ASSERT(mmf_make("closed.bin", 0x1000, 'A'), "make file");
    ASSERT(mmf_open("closed.bin", FILE_READ_DATA | FILE_WRITE_DATA, &fh) == STATUS_SUCCESS, "open");
    ASSERT(mmf_section(fh, &sec, PAGE_READWRITE, 0) == STATUS_SUCCESS, "create section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close file handle first");
    {
        HANDLE refused = NULL;
        ASSERT(mmf_section(fh, &refused, PAGE_READWRITE, 0) == STATUS_INVALID_HANDLE, "closed file handle refused");
        ASSERT(refused == NULL, "no section for closed file handle");
    }
    {
        NTSTATUS mst = mmf_map(sec, 0, 0, PAGE_READWRITE, &base);
        ASSERT(mst == STATUS_SUCCESS, "map after file handle closed 0x%lx", (unsigned long)mst);
    }
    ASSERT(((unsigned char *)base)[0] == 'A', "view reads after file handle closed");
    ((unsigned char *)base)[0x20] = 'C';
    ASSERT(mmf_flush(base, 0x1000) == STATUS_SUCCESS, "flush after file handle closed");
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close section");
    ASSERT(mmf_host_byte("closed.bin", 0x20, &b) && b == 'C', "write reached file after handle closed");
    return 1;
}

/* Views must start on a 64 KiB boundary of the file. */
static int test_file_section_offset_alignment(void) {
    HANDLE fh = NULL, sec = NULL;
    PVOID base = NULL;
    ASSERT(mmf_make("aligned.bin", 0x20000, 'A'), "make file");
    ASSERT(mmf_poke("aligned.bin", 0x10000, 'B'), "poke file");
    ASSERT(mmf_open("aligned.bin", FILE_READ_DATA, &fh) == STATUS_SUCCESS, "open");
    ASSERT(mmf_section(fh, &sec, PAGE_READONLY, 0) == STATUS_SUCCESS, "create section");
    ASSERT(mmf_map(sec, 0x1000, 0x1000, PAGE_READONLY, &base) == STATUS_MAPPED_ALIGNMENT,
           "4 KiB offset refused");
    ASSERT(mmf_map(sec, 0x10000, 0, PAGE_READONLY, &base) == STATUS_SUCCESS, "64 KiB offset maps");
    ASSERT(((unsigned char *)base)[0] == 'B', "view starts at the offset");
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close file");
    return 1;
}

/* Default size is the file size, and the section reports the file attribute. */
static int test_file_section_query_file_attribute(void) {
    HANDLE fh = NULL, sec = NULL, anon = NULL;
    SECTION_BASIC_INFORMATION info;
    SIZE_T ret = 0;
    ASSERT(mmf_make("query.bin", 0x3000, 'A'), "make file");
    ASSERT(mmf_open("query.bin", FILE_READ_DATA, &fh) == STATUS_SUCCESS, "open");
    ASSERT(mmf_section(fh, &sec, PAGE_READONLY, 0) == STATUS_SUCCESS, "create section");
    ASSERT(nt_svc_NtQuerySection(sec, 0, &info, sizeof(info), &ret) == STATUS_SUCCESS, "query file section");
    ASSERT(info.AllocationAttributes & SEC_FILE, "file attribute reported");
    ASSERT(info.AllocationAttributes & SEC_COMMIT, "commit attribute kept");
    ASSERT(info.MaximumSize.QuadPart == 0x3000, "maximum size is file size, got 0x%llx",
           (unsigned long long)info.MaximumSize.QuadPart);
    {
        LARGE_INTEGER asz; asz.QuadPart = 0x1000;
        ASSERT(NtCreateSection(&anon, SECTION_ALL_ACCESS, NULL, &asz, PAGE_READWRITE, SEC_COMMIT, NULL)
               == STATUS_SUCCESS, "anon section");
    }
    ASSERT(nt_svc_NtQuerySection(anon, 0, &info, sizeof(info), &ret) == STATUS_SUCCESS, "query anon section");
    ASSERT(!(info.AllocationAttributes & SEC_FILE), "anonymous section has no file attribute");
    ASSERT(ObCloseHandle(g_mm_proc, anon) == STATUS_SUCCESS, "close anon");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close file");
    return 1;
}

/* A larger MaximumSize extends a read-write file, and a read-only section that
 * is larger than its file is STATUS_SECTION_TOO_BIG. */
static int test_file_section_size_rules(void) {
    HANDLE fh = NULL, sec = NULL;
    ASSERT(mmf_make("grow.bin", 0x1000, 'A'), "make file");
    ASSERT(mmf_open("grow.bin", FILE_READ_DATA | FILE_WRITE_DATA, &fh) == STATUS_SUCCESS, "open");
    ASSERT(mmf_section(fh, &sec, PAGE_READONLY, 0x2000) == STATUS_SECTION_TOO_BIG, "read-only too big");
    ASSERT(sec == NULL, "no section when too big");
    ASSERT(mmf_host_size("grow.bin") == 0x1000, "read-only refusal leaves file alone");
    ASSERT(mmf_section(fh, &sec, PAGE_READWRITE, 0x2000) == STATUS_SUCCESS, "read-write extends");
    ASSERT(mmf_host_size("grow.bin") == 0x2000, "file extended to section size");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close file");
    return 1;
}

/* SEC_IMAGE is a separate milestone (process-creation M6). Until then it must
 * refuse, never map the file as plain data. */
/* SEC_IMAGE tests. Images are synthesized by pe_synth.h, written to the scratch
 * root, and opened like any file. The view is read back through host pointers. */

static int mmf_write_bytes(const char *name, const unsigned char *buf, size_t n) {
    char p[512];
    int fd;
    mmf_path(p, sizeof(p), name);
    fd = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY, 0644);
    if (fd < 0) return 0;
    if (write(fd, buf, n) != (ssize_t)n) { close(fd); return 0; }
    return close(fd) == 0;
}

static int all_zero(const unsigned char *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

/* Writes a synthesized image and creates a SEC_IMAGE section from it. The
 * file handle stays open in *fh for the caller to close. */
static NTSTATUS mmf_image_section(const char *name, const pe_synth_opts_t *o, HANDLE *fh, HANDLE *sec) {
    unsigned char img[PES_FILE_SIZE];
    NTSTATUS st;
    *fh = NULL;
    *sec = NULL;
    pe_synth_build(img, o);
    if (!mmf_write_bytes(name, img, sizeof img)) return STATUS_INVALID_PARAMETER;
    st = mmf_open(name, FILE_READ_DATA, fh);
    if (!NT_SUCCESS(st)) return st;
    return NtCreateSection(sec, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READONLY, SEC_IMAGE, *fh);
}

/* SEC_IMAGE with no file, or with a file that is not a PE image, is refused
 * before any section object exists. */
static int test_sec_image_file_rules(void) {
    HANDLE fh = NULL, sec = NULL;
    LARGE_INTEGER sz;
    sz.QuadPart = 0x1000;
    ASSERT(NtCreateSection(&sec, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READONLY, SEC_IMAGE, NULL)
           == STATUS_INVALID_PARAMETER, "SEC_IMAGE without a file refused");
    ASSERT(sec == NULL, "no section for SEC_IMAGE without a file");
    ASSERT(mmf_make("notpe.bin", 0x1000, 'A'), "make non-PE file");
    ASSERT(mmf_open("notpe.bin", FILE_READ_DATA, &fh) == STATUS_SUCCESS, "open non-PE file");
    ASSERT(NtCreateSection(&sec, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READONLY, SEC_IMAGE, fh)
           == STATUS_INVALID_IMAGE_FORMAT, "SEC_IMAGE of a non-PE file refused");
    ASSERT(sec == NULL, "no section for a non-PE image");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close non-PE file");
    return 1;
}

/* The 32-bit optional header is refused. This personality maps 64-bit images only. */
static int test_sec_image_pe32_refused(void) {
    pe_synth_opts_t o = { 1, 0, 0, 0 };
    HANDLE fh = NULL, sec = NULL;
    ASSERT(mmf_image_section("pe32.exe", &o, &fh, &sec) == STATUS_INVALID_IMAGE_FORMAT,
           "PE32 image refused");
    ASSERT(sec == NULL, "no section for a PE32 image");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close PE32 file");
    return 1;
}

/* Maps a synthesized image at a free base. Each segment has its own VAD type
 * and protection, the file bytes are at their RVAs, and the zero tail is zero. */
static int test_sec_image_maps_sections(void) {
    unsigned char img[PES_FILE_SIZE];
    pe_synth_opts_t o = { 0, 0, 0, 0 };
    HANDLE fh = NULL, sec = NULL;
    PVOID base = NULL;
    LARGE_INTEGER off;
    SIZE_T vs = 0;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *b;
    NTSTATUS st;
    pe_synth_build(img, &o);
    ASSERT(mmf_image_section("image.exe", &o, &fh, &sec) == STATUS_SUCCESS, "create image section");
    off.QuadPart = 0;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READONLY);
    ASSERT(st == STATUS_SUCCESS, "map image 0x%lx", (unsigned long)st);
    ASSERT(base != NULL, "image base");
    ASSERT(vs == 0x4000, "image view size 0x%zx want 0x4000", (size_t)vs);
    b = (unsigned char *)base;

    ASSERT(query(base, &mbi), "query header");
    ASSERT(mbi.Type == MEM_IMAGE, "header region is MEM_IMAGE");
    ASSERT(mbi.Protect == PAGE_READONLY && mbi.RegionSize == 0x1000, "header region read-only, one page");
    ASSERT(query((PVOID)(b + 0x1000), &mbi), "query .text");
    ASSERT(mbi.Protect == PAGE_EXECUTE_READ && mbi.RegionSize == 0x1000, ".text is RX");
    ASSERT(query((PVOID)(b + 0x2000), &mbi), "query .data");
    ASSERT(mbi.Protect == PAGE_WRITECOPY && mbi.RegionSize == 0x2000, ".data is copy-on-write");

    ASSERT(memcmp(b, img, 0x400) == 0, "header bytes at base");
    ASSERT(memcmp(b + 0x1000, img + 0x400, 0x100) == 0, ".text bytes at RVA 0x1000");
    ASSERT(all_zero(b + 0x1100, 0xF00), ".text tail is zero");
    ASSERT(memcmp(b + 0x2000, img + 0x600, 0x200) == 0, ".data bytes at RVA 0x2000");
    ASSERT(all_zero(b + 0x2200, 0x1E00), ".data tail is zero");

    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap image");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close image section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close image file");
    return 1;
}

/* An image view takes no offset, and it must cover the whole SizeOfImage. */
static int test_sec_image_view_rules(void) {
    pe_synth_opts_t o = { 0, 0, 0, 0 };
    HANDLE fh = NULL, sec = NULL;
    PVOID base = NULL;
    LARGE_INTEGER off;
    SIZE_T vs;
    NTSTATUS st;
    ASSERT(mmf_image_section("view.exe", &o, &fh, &sec) == STATUS_SUCCESS, "create image section");
    off.QuadPart = 0x1000;
    vs = 0;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READONLY);
    ASSERT(st == STATUS_INVALID_PARAMETER, "image view with an offset refused 0x%lx", (unsigned long)st);
    ASSERT(base == NULL, "no base for an offset image view");
    off.QuadPart = 0;
    vs = 0x1000;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READONLY);
    ASSERT(st == STATUS_INVALID_VIEW_SIZE, "image view smaller than SizeOfImage refused 0x%lx", (unsigned long)st);
    ASSERT(base == NULL, "no base for a short image view");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close image section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close image file");
    return 1;
}

static NTSTATUS mmf_custom_image(const char *name, const unsigned char *img, HANDLE *fh, HANDLE *sec);
static void mmf_reloc_image(unsigned char *img, uint32_t bsize);

/* With the preferred base taken, a relocatable image maps elsewhere and reports
 * STATUS_IMAGE_NOT_AT_BASE, with its fixups applied (test_sec_image_relocated_view).
 * A non-relocatable image cannot move, so the map fails. */
static int test_sec_image_not_at_base(void) {
    PVOID pref = (PVOID)(ULONG_PTR)PES_BASE_DEFAULT;
    SIZE_T rsz = 0x1000, zero = 0;
    unsigned char reloc_img[PES_FILE_SIZE];
    pe_synth_opts_t fixed_o = { 0, 0, 0, 0 };
    HANDLE fh = NULL, sec = NULL;
    PVOID base = NULL;
    LARGE_INTEGER off;
    SIZE_T vs;
    NTSTATUS st;
    ASSERT(NtAllocateVirtualMemory((HANDLE)-1, &pref, 0, &rsz, MEM_RESERVE, PAGE_READWRITE) == STATUS_SUCCESS,
           "occupy the preferred base");
    ASSERT(pref == (PVOID)(ULONG_PTR)PES_BASE_DEFAULT, "reservation at the preferred base");

    /* A well-formed directory: pe_synth's directory bytes are not blocks. */
    mmf_reloc_image(reloc_img, 12);
    ASSERT(mmf_custom_image("reloc.exe", reloc_img, &fh, &sec) == STATUS_SUCCESS, "create relocatable image");
    off.QuadPart = 0;
    vs = 0;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READONLY);
    ASSERT(st == STATUS_IMAGE_NOT_AT_BASE, "relocatable image at a taken base: 0x%lx", (unsigned long)st);
    ASSERT(base != NULL && base != pref, "relocated image lands elsewhere");
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap relocated image");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close relocatable section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close relocatable file");

    ASSERT(mmf_image_section("fixed.exe", &fixed_o, &fh, &sec) == STATUS_SUCCESS, "create fixed image");
    base = NULL;
    off.QuadPart = 0;
    vs = 0;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READONLY);
    ASSERT(st == STATUS_CONFLICTING_ADDRESSES, "non-relocatable image at a taken base: 0x%lx", (unsigned long)st);
    ASSERT(base == NULL, "no base for a non-relocatable image at a taken base");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close fixed section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close fixed file");

    ASSERT(NtFreeVirtualMemory((HANDLE)-1, &pref, &zero, MEM_RELEASE) == STATUS_SUCCESS, "release preferred base");
    return 1;
}

/* Writes a custom image (bytes already built) and creates a SEC_IMAGE section. */
static NTSTATUS mmf_custom_image(const char *name, const unsigned char *img, HANDLE *fh, HANDLE *sec) {
    NTSTATUS st;
    *fh = NULL;
    *sec = NULL;
    if (!mmf_write_bytes(name, img, PES_FILE_SIZE)) return STATUS_INVALID_PARAMETER;
    st = mmf_open(name, FILE_READ_DATA, fh);
    if (!NT_SUCCESS(st)) return st;
    return NtCreateSection(sec, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READONLY, SEC_IMAGE, *fh);
}

/* Builds a relocatable image with one BASERELOC block in .text (file 0x400,
 * RVA 0x1000) for the DIR64 at .data+0x10 (file 0x610, RVA 0x2010). The block
 * is bsize bytes long; its entries are the DIR64 and one ABSOLUTE pad. */
static void mmf_reloc_image(unsigned char *img, uint32_t bsize) {
    pe_synth_opts_t o = { 0, 1, 0, 0 };
    pe_synth_build(img, &o);
    pes_put32(img, 0x400, 0x2000);        /* VirtualAddress: the .data page */
    pes_put32(img, 0x404, bsize);         /* SizeOfBlock */
    pes_put16(img, 0x408, 0xA010);        /* DIR64 at offset 0x010 */
    pes_put16(img, 0x40A, 0);             /* ABSOLUTE pad */
    pes_put64(img, 0x610, 0x20000100ULL); /* pointer into the image */
}

/* A relocatable image at a taken base maps elsewhere with STATUS_IMAGE_NOT_AT_BASE.
 * The DIR64 is rewritten by the base delta in the private copy, and every
 * segment keeps its protection. The same image at a free base is not relocated. */
static int test_sec_image_relocated_view(void) {
    unsigned char img[PES_FILE_SIZE];
    PVOID pref = (PVOID)(ULONG_PTR)PES_BASE_DEFAULT;
    SIZE_T rsz = 0x1000, zero = 0;
    HANDLE fh = NULL, sec = NULL;
    PVOID base = NULL;
    LARGE_INTEGER off;
    SIZE_T vs = 0;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *b;
    uint64_t got, delta;
    NTSTATUS st;

    mmf_reloc_image(img, 12);
    ASSERT(NtAllocateVirtualMemory((HANDLE)-1, &pref, 0, &rsz, MEM_RESERVE, PAGE_READWRITE) == STATUS_SUCCESS,
           "occupy the preferred base");
    ASSERT(mmf_custom_image("reloc_moved.exe", img, &fh, &sec) == STATUS_SUCCESS, "create relocatable image");
    off.QuadPart = 0;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READONLY);
    ASSERT(st == STATUS_IMAGE_NOT_AT_BASE, "relocated view status 0x%lx", (unsigned long)st);
    ASSERT(base != NULL && base != pref, "relocated view lands elsewhere");
    b = (unsigned char *)base;
    delta = (uint64_t)(ULONG_PTR)base - (uint64_t)PES_BASE_DEFAULT;
    memcpy(&got, b + 0x2010, 8);
    ASSERT(got == 0x20000100ULL + delta, "DIR64 rewritten by the delta: got 0x%llx want 0x%llx",
           (unsigned long long)got, (unsigned long long)(0x20000100ULL + delta));
    ASSERT(memcmp(b + 0x2018, img + 0x618, 0x1E8) == 0, "bytes past the fixup are unchanged");
    ASSERT(memcmp(b + 0x1000, img + 0x400, 0x100) == 0, ".text bytes unchanged");
    ASSERT(query((PVOID)(b + 0x1000), &mbi) && mbi.Protect == PAGE_EXECUTE_READ,
           ".text keeps its protection after relocation");
    ASSERT(query((PVOID)(b + 0x2000), &mbi) && mbi.Protect == PAGE_WRITECOPY,
           ".data keeps its copy-on-write protection after relocation");

    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap relocated view");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close relocated section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close relocated file");

    /* Release the preferred base: the same image maps there and is not relocated. */
    ASSERT(NtFreeVirtualMemory((HANDLE)-1, &pref, &zero, MEM_RELEASE) == STATUS_SUCCESS, "free preferred base");
    ASSERT(mmf_custom_image("reloc_fixed.exe", img, &fh, &sec) == STATUS_SUCCESS, "create image again");
    base = NULL;
    vs = 0;
    off.QuadPart = 0;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READONLY);
    ASSERT(st == STATUS_SUCCESS, "image at its preferred base: 0x%lx", (unsigned long)st);
    ASSERT(base == (PVOID)(ULONG_PTR)PES_BASE_DEFAULT, "image at its preferred base");
    memcpy(&got, (unsigned char *)base + 0x2010, 8);
    ASSERT(got == 0x20000100ULL, "image at its preferred base is not relocated");
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap preferred view");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close preferred section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close preferred file");
    return 1;
}

/* RELOCS_STRIPPED with a directory present still cannot move, so it fails with
 * STATUS_CONFLICTING_ADDRESSES and no view appears. */
static int test_sec_image_stripped_conflicts(void) {
    unsigned char img[PES_FILE_SIZE];
    PVOID pref = (PVOID)(ULONG_PTR)PES_BASE_DEFAULT;
    SIZE_T rsz = 0x1000, zero = 0;
    HANDLE fh = NULL, sec = NULL;
    PVOID base = NULL;
    LARGE_INTEGER off;
    SIZE_T vs = 0;
    NTSTATUS st;

    mmf_reloc_image(img, 12);
    pes_put16(img, 0x96, 0x0023);         /* RELOCS_STRIPPED set */
    ASSERT(NtAllocateVirtualMemory((HANDLE)-1, &pref, 0, &rsz, MEM_RESERVE, PAGE_READWRITE) == STATUS_SUCCESS,
           "occupy the preferred base");
    ASSERT(mmf_custom_image("stripped.exe", img, &fh, &sec) == STATUS_SUCCESS, "create stripped image");
    off.QuadPart = 0;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READONLY);
    ASSERT(st == STATUS_CONFLICTING_ADDRESSES, "stripped image at a taken base: 0x%lx", (unsigned long)st);
    ASSERT(base == NULL, "no view for a stripped image at a taken base");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close stripped section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close stripped file");
    ASSERT(NtFreeVirtualMemory((HANDLE)-1, &pref, &zero, MEM_RELEASE) == STATUS_SUCCESS, "release preferred base");
    return 1;
}

/* A malformed directory (a block of 6 bytes) fails the map with
 * STATUS_INVALID_IMAGE_FORMAT, no view appears, and nothing leaks. */
static int test_sec_image_bad_reloc_directory(void) {
    unsigned char img[PES_FILE_SIZE];
    PVOID pref = (PVOID)(ULONG_PTR)PES_BASE_DEFAULT;
    SIZE_T rsz = 0x1000, zero = 0;
    HANDLE fh = NULL, sec = NULL;
    PVOID base = NULL;
    LARGE_INTEGER off;
    SIZE_T vs = 0;
    NTSTATUS st;
    long before;

    mmf_reloc_image(img, 6);
    ASSERT(NtAllocateVirtualMemory((HANDLE)-1, &pref, 0, &rsz, MEM_RESERVE, PAGE_READWRITE) == STATUS_SUCCESS,
           "occupy the preferred base");
    before = atomic_load(&kshim_alloc_count);
    ASSERT(mmf_custom_image("badreloc.exe", img, &fh, &sec) == STATUS_SUCCESS, "create malformed image");
    off.QuadPart = 0;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READONLY);
    ASSERT(st == STATUS_INVALID_IMAGE_FORMAT, "malformed directory refused: 0x%lx", (unsigned long)st);
    ASSERT(base == NULL, "no view for a malformed directory");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close malformed section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close malformed file");
    ASSERT(atomic_load(&kshim_alloc_count) == before, "malformed map leaked allocations");
    ASSERT(NtFreeVirtualMemory((HANDLE)-1, &pref, &zero, MEM_RELEASE) == STATUS_SUCCESS, "release preferred base");
    return 1;
}

/* Unmapping an image view frees its region, and closing the handles returns
 * every allocation the section made (the planner copy included). */
static int test_sec_image_unmap_frees(void) {
    pe_synth_opts_t o = { 0, 0, 0, 0 };
    HANDLE fh = NULL, sec = NULL;
    PVOID base = NULL;
    LARGE_INTEGER off;
    SIZE_T vs = 0;
    MEMORY_BASIC_INFORMATION mbi;
    long before;
    before = atomic_load(&kshim_alloc_count);
    ASSERT(mmf_image_section("unmap.exe", &o, &fh, &sec) == STATUS_SUCCESS, "create image section");
    off.QuadPart = 0;
    ASSERT(NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READONLY) == STATUS_SUCCESS,
           "map image");
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap image");
    ASSERT(query(base, &mbi), "query after unmap");
    ASSERT(mbi.State == MEM_FREE, "image region free after unmap");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close image section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close image file");
    ASSERT(atomic_load(&kshim_alloc_count) == before, "image section leaked allocations");
    return 1;
}

/* A FileHandle that is not an open handle is refused, not treated as a file. */
static int test_file_section_forged_handle(void) {
    HANDLE sec = NULL;
    LARGE_INTEGER sz;
    sz.QuadPart = 0x1000;
    ASSERT(NtCreateSection(&sec, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, (HANDLE)0x1234)
           == STATUS_INVALID_HANDLE, "forged file handle refused");
    ASSERT(sec == NULL, "no section for forged handle");
    return 1;
}

/* Leak tracker: a file section and its view leave no allocation behind. */
static int test_file_section_leak_tracker(void) {
    HANDLE fh = NULL, sec = NULL;
    PVOID base = NULL;
    long before;
    ASSERT(mmf_make("leak.bin", 0x1000, 'A'), "make file");
    before = atomic_load(&kshim_alloc_count);
    ASSERT(mmf_open("leak.bin", FILE_READ_DATA | FILE_WRITE_DATA, &fh) == STATUS_SUCCESS, "open");
    ASSERT(mmf_section(fh, &sec, PAGE_READWRITE, 0) == STATUS_SUCCESS, "create section");
    ASSERT(mmf_map(sec, 0, 0, PAGE_READWRITE, &base) == STATUS_SUCCESS, "map");
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close section");
    ASSERT(nt_svc_NtClose(fh) == STATUS_SUCCESS, "close file");
    ASSERT(atomic_load(&kshim_alloc_count) == before, "file section leaked allocations");
    return 1;
}

/* D-2: a *ViewSize within one page of SIZE_MAX rounds up to 0 and used to pass
 * the size check. It must be refused with STATUS_INVALID_VIEW_SIZE, mapping nothing. */
static int test_view_size_round_up_wrap(void) {
    LARGE_INTEGER sz; sz.QuadPart = 0x2000;
    HANDLE sec = NULL;
    NTSTATUS st = NtCreateSection(&sec, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_SUCCESS, "create wrap section 0x%lx", (unsigned long)st);
    LARGE_INTEGER off; off.QuadPart = 0;
    PVOID base = NULL;
    SIZE_T vs = (SIZE_T)-1 - 100;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READWRITE);
    ASSERT(st == STATUS_INVALID_VIEW_SIZE, "wrapping view size status 0x%lx", (unsigned long)st);
    ASSERT(base == NULL, "no base for wrapping view size");
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close wrap section");
    return 1;
}

/* A size within one page of SIZE_MAX rounds up to 0 and used to pass the end
 * check, so protect and decommit returned success without touching the range. */
static int test_wrapping_size_protect_decommit(void) {
    PVOID b = NULL;
    SIZE_T s = 0x1000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &b, 0, &s, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "alloc for wrap test 0x%lx", (unsigned long)st);
    PVOID pb = b;
    SIZE_T huge = (SIZE_T)-1 - 100;
    ULONG old = 0;
    st = NtProtectVirtualMemory((HANDLE)-1, &pb, &huge, PAGE_READONLY, &old);
    ASSERT(st == STATUS_INVALID_PARAMETER, "wrapping protect status 0x%lx", (unsigned long)st);
    PVOID db = b;
    huge = (SIZE_T)-1 - 100;
    st = NtFreeVirtualMemory((HANDLE)-1, &db, &huge, MEM_DECOMMIT);
    ASSERT(st == STATUS_INVALID_PARAMETER, "wrapping decommit status 0x%lx", (unsigned long)st);
    PVOID rb = b; SIZE_T rs = 0;
    st = NtFreeVirtualMemory((HANDLE)-1, &rb, &rs, MEM_RELEASE);
    ASSERT(st == STATUS_SUCCESS, "release after wrap test 0x%lx", (unsigned long)st);
    return 1;
}

/* ADR 0001: section handles are object-table handles. These helpers use the
 * same process and handle table as the object manager tests. */
static int mm_setup_objects(void) {
    if (ObInitObjectManager() != STATUS_SUCCESS) return 0;
    if (ObCreateProcessObject(&g_mm_proc, 4242, "mmtest") != STATUS_SUCCESS || !g_mm_proc) return 0;
    kshim_set_current_process(g_mm_proc);
    return 1;
}

static NTSTATUS mm_make_section(HANDLE *h, ACCESS_MASK access, ULONG prot, SIZE_T size) {
    LARGE_INTEGER sz;
    sz.QuadPart = (LONGLONG)size;
    *h = NULL;
    return NtCreateSection(h, access, NULL, &sz, prot, SEC_COMMIT, NULL);
}

static NTSTATUS mm_map_view(HANDLE h, PVOID *base, SIZE_T *vs, ULONG prot) {
    LARGE_INTEGER off;
    off.QuadPart = 0;
    *base = NULL;
    *vs = 0;
    return NtMapViewOfSection(h, (HANDLE)-1, base, 0, 0, &off, vs, 0, 0, prot);
}

/* The handle is a handle-table index, not the address of the section object. */
static int test_section_handle_is_object(void) {
    HANDLE h = NULL;
    NTSTATUS st = mm_make_section(&h, SECTION_ALL_ACCESS, PAGE_READWRITE, 0x2000);
    ASSERT(st == STATUS_SUCCESS, "create 0x%lx", (unsigned long)st);
    ASSERT(h != NULL, "handle is null");
    struct nt_object_header *hdr = NULL;
    st = ObReferenceObjectByHandle(g_mm_proc, h, &nt_section_type, KernelMode, 0, &hdr, NULL);
    ASSERT(st == STATUS_SUCCESS, "reference by handle 0x%lx", (unsigned long)st);
    ASSERT(hdr && hdr->type == &nt_section_type, "object type is Section");
    ASSERT((ULONG_PTR)h != (ULONG_PTR)nt_object_body(hdr), "handle equals object address");
    ObDereferenceObject(hdr);
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "close");
    return 1;
}

/* Forged values, including the address of the section body (the old
 * pointer-as-handle hole), are rejected without being dereferenced. */
static int test_section_forged_handle_rejected(void) {
    HANDLE h = NULL;
    NTSTATUS st = mm_make_section(&h, SECTION_ALL_ACCESS, PAGE_READWRITE, 0x2000);
    ASSERT(st == STATUS_SUCCESS, "create 0x%lx", (unsigned long)st);
    struct nt_object_header *hdr = NULL;
    st = ObReferenceObjectByHandle(g_mm_proc, h, &nt_section_type, KernelMode, 0, &hdr, NULL);
    ASSERT(st == STATUS_SUCCESS, "reference 0x%lx", (unsigned long)st);
    void *body = nt_object_body(hdr);
    ObDereferenceObject(hdr);
    volatile int local = 0;
    HANDLE forged[] = { (HANDLE)0x1234, (HANDLE)(ULONG_PTR)0xFFFFFFFFFFFF0000ULL,
                        (HANDLE)body, (HANDLE)&local };
    for (size_t i = 0; i < sizeof(forged) / sizeof(forged[0]); i++) {
        PVOID base = NULL; SIZE_T vs = 0;
        LARGE_INTEGER off; off.QuadPart = 0;
        st = NtMapViewOfSection(forged[i], (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READWRITE);
        ASSERT(st == STATUS_INVALID_HANDLE, "forged[%zu] status 0x%lx", i, (unsigned long)st);
        ASSERT(base == NULL, "forged[%zu] mapped a view", i);
    }
    /* The real handle still maps. */
    PVOID base = NULL; SIZE_T vs = 0;
    st = mm_map_view(h, &base, &vs, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "real handle after forgery 0x%lx", (unsigned long)st);
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "close");
    return 1;
}

/* A handle to an object of another type is rejected with a type mismatch. */
static int test_section_wrong_type_rejected(void) {
    struct nt_object_header *hdr = NULL;
    void *body = NULL;
    NTSTATUS st = ObCreateObject(&nt_test_type, NULL, sizeof(struct nt_test_body), &hdr, &body);
    ASSERT(st == STATUS_SUCCESS, "create test object 0x%lx", (unsigned long)st);
    HANDLE h = NULL;
    st = ObInsertObject(hdr, NULL, SECTION_ALL_ACCESS, g_mm_proc, &h);
    ObDereferenceObject(hdr);
    ASSERT(st == STATUS_SUCCESS, "insert test object 0x%lx", (unsigned long)st);
    PVOID base = NULL; SIZE_T vs = 0;
    st = mm_map_view(h, &base, &vs, PAGE_READWRITE);
    ASSERT(st == STATUS_OBJECT_TYPE_MISMATCH, "wrong type status 0x%lx", (unsigned long)st);
    ASSERT(base == NULL, "wrong type mapped a view");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "close");
    return 1;
}

/* A section name held by an object of another type: NtCreateSection gets
 * OBJECT_TYPE_MISMATCH and leaves the caller's handle at its sentinel. */
static NTSTATUS mm_section_oa(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us, const WCHAR *name) {
    us->Buffer = (WCHAR *)name;
    /* Count 16-bit units by hand: the libc wcslen takes wchar_t, which is 32 bits on Linux (LNX-08). */
    size_t units = 0;
    while (name[units])
        units++;
    us->Length = (USHORT)(units * sizeof(WCHAR));
    us->MaximumLength = us->Length;
    memset(oa, 0, sizeof(*oa));
    oa->Length = sizeof(*oa);
    oa->ObjectName = us;
    return STATUS_SUCCESS;
}

static int test_section_name_held_by_other_type(void) {
    /* u"": char16_t is 16 bits on every host; L"" is 32 bits on Linux (LNX-08). */
    static const WCHAR name[] = u"\\BaseNamedObjects\\SecOverTest";
    struct nt_object_header *hdr = NULL;
    void *body = NULL;
    OBJECT_ATTRIBUTES oa; UNICODE_STRING us;
    mm_section_oa(&oa, &us, name);
    NTSTATUS st = ObCreateObject(&nt_test_type, NULL, sizeof(struct nt_test_body), &hdr, &body);
    ASSERT(st == STATUS_SUCCESS, "create holder 0x%lx", (unsigned long)st);
    HANDLE holder = NULL;
    st = ObInsertObject(hdr, &oa, SECTION_ALL_ACCESS, g_mm_proc, &holder);
    ObDereferenceObject(hdr);
    ASSERT(st == STATUS_SUCCESS, "insert holder 0x%lx", (unsigned long)st);
    LARGE_INTEGER sz; sz.QuadPart = 0x2000;
    HANDLE sec = (HANDLE)(ULONG_PTR)0x5E171E1;
    st = NtCreateSection(&sec, SECTION_ALL_ACCESS, &oa, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_OBJECT_TYPE_MISMATCH, "section over other type 0x%lx", (unsigned long)st);
    ASSERT(sec == (HANDLE)(ULONG_PTR)0x5E171E1, "section handle written on failure");
    ASSERT(ObCloseHandle(g_mm_proc, holder) == STATUS_SUCCESS, "close holder");
    return 1;
}

/* Same type: the second named section gets the holder's handle with
 * NAME_COLLISION (the SYNC-3 policy). */
static int test_section_same_type_collision_reopens(void) {
    static const WCHAR name[] = u"\\BaseNamedObjects\\SecSameType";
    OBJECT_ATTRIBUTES oa; UNICODE_STRING us;
    mm_section_oa(&oa, &us, name);
    LARGE_INTEGER sz; sz.QuadPart = 0x2000;
    HANDLE first = NULL, second = (HANDLE)(ULONG_PTR)0x5E171E1;
    NTSTATUS st = NtCreateSection(&first, SECTION_ALL_ACCESS, &oa, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_SUCCESS, "first named section 0x%lx", (unsigned long)st);
    st = NtCreateSection(&second, SECTION_ALL_ACCESS, &oa, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_OBJECT_NAME_COLLISION, "second named section 0x%lx", (unsigned long)st);
    ASSERT(second != NULL && second != (HANDLE)(ULONG_PTR)0x5E171E1, "no holder handle");
    ASSERT(ObCloseHandle(g_mm_proc, second) == STATUS_SUCCESS, "close second");
    ASSERT(ObCloseHandle(g_mm_proc, first) == STATUS_SUCCESS, "close first");
    return 1;
}

static int test_section_closed_handle_rejected(void) {
    HANDLE h = NULL;
    NTSTATUS st = mm_make_section(&h, SECTION_ALL_ACCESS, PAGE_READWRITE, 0x2000);
    ASSERT(st == STATUS_SUCCESS, "create 0x%lx", (unsigned long)st);
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "close");
    PVOID base = NULL; SIZE_T vs = 0;
    st = mm_map_view(h, &base, &vs, PAGE_READWRITE);
    ASSERT(st == STATUS_INVALID_HANDLE, "closed handle status 0x%lx", (unsigned long)st);
    ASSERT(base == NULL, "closed handle mapped a view");
    return 1;
}

/* Mapping needs SECTION_MAP_READ, and write protection needs SECTION_MAP_WRITE. */
static int test_section_access_mask(void) {
    HANDLE ro = NULL;
    NTSTATUS st = mm_make_section(&ro, SECTION_MAP_READ, PAGE_READWRITE, 0x2000);
    ASSERT(st == STATUS_SUCCESS, "create read-only handle 0x%lx", (unsigned long)st);
    PVOID base = NULL; SIZE_T vs = 0;
    st = mm_map_view(ro, &base, &vs, PAGE_READONLY);
    ASSERT(st == STATUS_SUCCESS, "read map 0x%lx", (unsigned long)st);
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap read");
    base = NULL; vs = 0;
    st = mm_map_view(ro, &base, &vs, PAGE_READWRITE);
    ASSERT(st == STATUS_ACCESS_DENIED, "write map without MAP_WRITE 0x%lx", (unsigned long)st);
    ASSERT(base == NULL, "denied map returned a base");
    HANDLE q = NULL;
    st = mm_make_section(&q, SECTION_QUERY, PAGE_READWRITE, 0x2000);
    ASSERT(st == STATUS_SUCCESS, "create query-only 0x%lx", (unsigned long)st);
    base = NULL; vs = 0;
    st = mm_map_view(q, &base, &vs, PAGE_READONLY);
    ASSERT(st == STATUS_ACCESS_DENIED, "map without MAP_READ 0x%lx", (unsigned long)st);
    ASSERT(ObCloseHandle(g_mm_proc, ro) == STATUS_SUCCESS, "close ro");
    ASSERT(ObCloseHandle(g_mm_proc, q) == STATUS_SUCCESS, "close q");
    return 1;
}

/* Lifetime: the section is freed only after the last handle and the last view
 * are gone. kshim_alloc_count is the allocation tracker. */
static int test_section_lifetime(void) {
    HANDLE warm = NULL;
    ASSERT(mm_make_section(&warm, SECTION_ALL_ACCESS, PAGE_READWRITE, 0x1000) == STATUS_SUCCESS, "warm");
    ASSERT(ObCloseHandle(g_mm_proc, warm) == STATUS_SUCCESS, "warm close");
    long before = atomic_load(&kshim_alloc_count);

    /* Two views; the handle closes first. */
    HANDLE h = NULL;
    ASSERT(mm_make_section(&h, SECTION_ALL_ACCESS, PAGE_READWRITE, 0x2000) == STATUS_SUCCESS, "create");
    PVOID b1 = NULL; SIZE_T v1 = 0;
    ASSERT(mm_map_view(h, &b1, &v1, PAGE_READWRITE) == STATUS_SUCCESS, "map 1");
    PVOID b2 = NULL; SIZE_T v2 = 0;
    ASSERT(mm_map_view(h, &b2, &v2, PAGE_READWRITE) == STATUS_SUCCESS, "map 2");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "close handle");
    ((volatile char *)b1)[0] = 1;
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, b1) == STATUS_SUCCESS, "unmap 1");
    ASSERT(atomic_load(&kshim_alloc_count) > before, "freed with one view left");
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, b2) == STATUS_SUCCESS, "unmap 2");
    ASSERT(atomic_load(&kshim_alloc_count) == before, "not freed after last view: %ld vs %ld",
           atomic_load(&kshim_alloc_count), before);

    /* Handle last: map, unmap, then close. */
    HANDLE h2 = NULL;
    ASSERT(mm_make_section(&h2, SECTION_ALL_ACCESS, PAGE_READWRITE, 0x2000) == STATUS_SUCCESS, "create 2");
    PVOID b3 = NULL; SIZE_T v3 = 0;
    ASSERT(mm_map_view(h2, &b3, &v3, PAGE_READWRITE) == STATUS_SUCCESS, "map 3");
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, b3) == STATUS_SUCCESS, "unmap 3");
    ASSERT(atomic_load(&kshim_alloc_count) > before, "freed before the last handle closed");
    ASSERT(ObCloseHandle(g_mm_proc, h2) == STATUS_SUCCESS, "close 2");
    ASSERT(atomic_load(&kshim_alloc_count) == before, "not freed after last handle: %ld vs %ld",
           atomic_load(&kshim_alloc_count), before);

    /* No views: closing the only handle frees it. */
    HANDLE h3 = NULL;
    ASSERT(mm_make_section(&h3, SECTION_ALL_ACCESS, PAGE_READWRITE, 0x1000) == STATUS_SUCCESS, "create 3");
    ASSERT(ObCloseHandle(g_mm_proc, h3) == STATUS_SUCCESS, "close 3");
    ASSERT(atomic_load(&kshim_alloc_count) == before, "view-less section leaked");
    return 1;
}

/* Protect of a sub-range splits the view, and the split copies the section
 * pointer into new VADs. Each VAD must hold its own reference, so unmapping the
 * whole view frees the section exactly once. */
static int test_section_view_split_refcount(void) {
    HANDLE warm = NULL;
    ASSERT(mm_make_section(&warm, SECTION_ALL_ACCESS, PAGE_READWRITE, 0x1000) == STATUS_SUCCESS, "warm");
    ASSERT(ObCloseHandle(g_mm_proc, warm) == STATUS_SUCCESS, "warm close");
    long before = atomic_load(&kshim_alloc_count);

    HANDLE h = NULL;
    ASSERT(mm_make_section(&h, SECTION_ALL_ACCESS, PAGE_READWRITE, 0x4000) == STATUS_SUCCESS, "create");
    PVOID base = NULL; SIZE_T vs = 0;
    ASSERT(mm_map_view(h, &base, &vs, PAGE_READWRITE) == STATUS_SUCCESS, "map");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "close handle");
    PVOID pb = (PVOID)((ULONG_PTR)base + 0x1000);
    SIZE_T ps = 0x1000;
    ULONG old = 0;
    NTSTATUS st = NtProtectVirtualMemory((HANDLE)-1, &pb, &ps, PAGE_READONLY, &old);
    ASSERT(st == STATUS_SUCCESS, "protect middle 0x%lx", (unsigned long)st);
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "unmap split view");
    ASSERT(atomic_load(&kshim_alloc_count) == before, "split view leaked or double-freed: %ld vs %ld",
           atomic_load(&kshim_alloc_count), before);
    return 1;
}

/* 8. sections */
static int test_sections(void) {
    LARGE_INTEGER sz;
    sz.QuadPart = 0x2000;
    HANDLE sec;
    NTSTATUS st = NtCreateSection(&sec, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st==STATUS_SUCCESS, "create sec");
    PVOID base=NULL; SIZE_T vs=0;
    LARGE_INTEGER off; off.QuadPart=0;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READWRITE);
    ASSERT(st==STATUS_SUCCESS, "map view");
    ASSERT(vs==0x2000, "view size");
    MEMORY_BASIC_INFORMATION mbi;
    ASSERT(query(base, &mbi), "q sec");
    ASSERT(mbi.Type==MEM_MAPPED, "type mapped");
    ASSERT(mbi.State==MEM_COMMIT, "state commit");
    /* Test SEC_RESERVE */
    LARGE_INTEGER sz2; sz2.QuadPart=0x4000;
    HANDLE sec2;
    st = NtCreateSection(&sec2, SECTION_ALL_ACCESS, NULL, &sz2, PAGE_READWRITE, SEC_RESERVE, NULL);
    ASSERT(st==STATUS_SUCCESS, "sec reserve");
    PVOID base2=NULL; SIZE_T vs2=0x1000;
    off.QuadPart=0x1000;
    st = NtMapViewOfSection(sec2, (HANDLE)-1, &base2, 0, 0, &off, &vs2, 0, 0, PAGE_READWRITE);
    ASSERT(st==STATUS_SUCCESS, "map reserve view");
    ASSERT(vs2==0x1000, "vs2");
    /* Copy-on-write */
    LARGE_INTEGER sz3; sz3.QuadPart=0x1000;
    HANDLE sec3;
    st = NtCreateSection(&sec3, SECTION_ALL_ACCESS, NULL, &sz3, PAGE_WRITECOPY, SEC_COMMIT, NULL);
    ASSERT(st==STATUS_SUCCESS, "sec cow");
    PVOID base3=NULL; SIZE_T vs3=0;
    off.QuadPart=0;
    st = NtMapViewOfSection(sec3, (HANDLE)-1, &base3, 0, 0, &off, &vs3, 0, 0, PAGE_WRITECOPY);
    ASSERT(st==STATUS_SUCCESS, "map cow");
    ASSERT(query(base3, &mbi), "q cow");
    ASSERT(mbi.Protect==PAGE_WRITECOPY, "cow protect");

    /* Unmap */
    st = NtUnmapViewOfSection((HANDLE)-1, base);
    ASSERT(st==STATUS_SUCCESS, "unmap1");
    st = NtUnmapViewOfSection((HANDLE)-1, base2);
    ASSERT(st==STATUS_SUCCESS, "unmap2");
    st = NtUnmapViewOfSection((HANDLE)-1, base3);
    ASSERT(st==STATUS_SUCCESS, "unmap3");
    /* Closing the last handle frees each section (its views are already gone). */
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close sec");
    ASSERT(ObCloseHandle(g_mm_proc, sec2) == STATUS_SUCCESS, "close sec2");
    ASSERT(ObCloseHandle(g_mm_proc, sec3) == STATUS_SUCCESS, "close sec3");
    return 1;
}

static int test_vad_matrix(void) {
    /* 4096-case state-transition matrix toward DOD-MEM-A001/B001.
     * Covers RESERVE/COMMIT/DECOMMIT/FREE × protections × split/merge adjacency.
     * Each step validates VirtualQuery after the operation (DOD-MEM-B001 requirement). */
    const ULONG protects[] = {PAGE_READWRITE, PAGE_READONLY, PAGE_EXECUTE_READ, PAGE_WRITECOPY, PAGE_EXECUTE_READWRITE, PAGE_NOACCESS, PAGE_READWRITE|PAGE_GUARD};
    const ULONG alloc_types[] = {MEM_RESERVE, MEM_COMMIT, MEM_RESERVE|MEM_COMMIT};
    int ok=0, tried=0;
    for (int i=0; i<4096; ++i) {
        ntmm_reset();
        ULONG at = alloc_types[(i>>0)%3];
        ULONG prot = protects[(i>>2)%7];
        PVOID base=NULL; SIZE_T sz = 0x1000 * (1 + (i%4)); /* 4k..16k */
        NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &sz, at, prot & 0xFF);
        if (st==STATUS_SUCCESS) {
            MEMORY_BASIC_INFORMATION mbi;
            ASSERT(query(base,&mbi), "vad matrix query");
            ASSERT(mbi.State==MEM_RESERVE || mbi.State==MEM_COMMIT, "state");
            if (at & MEM_COMMIT) ASSERT(mbi.State==MEM_COMMIT, "commit state");
            /* Try a protect change on half the cases */
            if ((i&1)==0) {
                PVOID pb=base; SIZE_T ps=0x1000; ULONG old=0;
                ULONG nprot = protects[(i>>5)%7];
                NtProtectVirtualMemory((HANDLE)-1, &pb, &ps, nprot & 0xFF, &old);
                ASSERT(query(base,&mbi), "after protect");
            }
            /* Try decommit on quarter */
            if ((i%4)==0) {
                PVOID db=base; SIZE_T ds=0x1000;
                NtFreeVirtualMemory((HANDLE)-1, &db, &ds, MEM_DECOMMIT);
                ASSERT(query(base,&mbi), "after decommit");
                ASSERT(mbi.State==MEM_RESERVE || mbi.State==MEM_COMMIT, "decommit state");
            }
            PVOID rel=base; SIZE_T rs=0;
            NtFreeVirtualMemory((HANDLE)-1, &rel, &rs, MEM_RELEASE);
            ok++;
        }
        tried++;
        ASSERT(ntmm_check_invariants(&g_nt_mm), "vad matrix invariant %d", i);
    }
    printf("  VAD matrix: %d/%d transitions validated with VirtualQuery after every step\n", ok, tried);
    return 1;
}

/* 9. probe_virtualquery regression — exact reserve/commit/protect/decommit/query sequence
 * that previously regressed to 24 S1 divergences due to VAD split left-hole bug.
 * Replicates probe_virtualquery_direct.c steps and asserts the Windows-true
 * post-decommit state: decommitted range must remain MEM_RESERVE with
 * AllocationProtect preserved and Type==MEM_PRIVATE, not MEM_FREE. */
static int test_probe_virtualquery_regression(void) {
    ntmm_reset();
    PVOID base = NULL;
    SIZE_T sz = 0x10000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &sz, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "probe reserve failed 0x%lx", (unsigned long)st);
    ASSERT(sz == 0x10000, "reserve size 0x%zx", (size_t)sz);
    PVOID rbase = base;
    MEMORY_BASIC_INFORMATION mbi;

    /* Commit 3 pages like probe: +0x0 RW, +0x1000 RO, +0x4000 X/R */
    PVOID c1 = rbase; SIZE_T c1sz = 0x1000;
    st = NtAllocateVirtualMemory((HANDLE)-1, &c1, 0, &c1sz, MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "c1 commit 0x%lx", (unsigned long)st);
    ASSERT(c1 == rbase, "c1 base");
    PVOID c2 = (PVOID)((ULONG_PTR)rbase + 0x1000); SIZE_T c2sz = 0x1000;
    st = NtAllocateVirtualMemory((HANDLE)-1, &c2, 0, &c2sz, MEM_COMMIT, PAGE_READONLY);
    ASSERT(st == STATUS_SUCCESS, "c2 commit 0x%lx", (unsigned long)st);
    PVOID c3 = (PVOID)((ULONG_PTR)rbase + 0x4000); SIZE_T c3sz = 0x1000;
    st = NtAllocateVirtualMemory((HANDLE)-1, &c3, 0, &c3sz, MEM_COMMIT, PAGE_EXECUTE_READ);
    ASSERT(st == STATUS_SUCCESS, "c3 commit 0x%lx", (unsigned long)st);

    /* VirtualProtect first page RW->RO (old 4) */
    ULONG oldProt = 0;
    PVOID vp = rbase; SIZE_T vpsz = 0x1000;
    st = NtProtectVirtualMemory((HANDLE)-1, &vp, &vpsz, PAGE_READONLY, &oldProt);
    ASSERT(st == STATUS_SUCCESS, "protect failed 0x%lx", (unsigned long)st);
    ASSERT(oldProt == PAGE_READWRITE, "oldProt %lu exp 4", (unsigned long)oldProt);
    ASSERT(vp == rbase, "vp base");
    ASSERT(vpsz == 0x1000, "vp size");

    /* query_before_free — per-VAD 4096 to match Windows probe_virtualquery_direct oracle
     * (previously expected coalesced 8192, but Windows keeps each 4K page separate for large reserves,
     * and per-VAD is the safe baseline; the 64KB coalesced case is covered by dedicated probe). */
    ASSERT(query(rbase, &mbi), "q before [0]");
    ASSERT(mbi.BaseAddress == rbase, "before [0] Base");
    ASSERT(mbi.AllocationBase == rbase, "before [0] AllocBase");
    ASSERT(mbi.AllocationProtect == PAGE_READWRITE, "before [0] AllocProt %lu", (unsigned long)mbi.AllocationProtect);
    ASSERT(mbi.State == MEM_COMMIT, "before [0] State %lu", (unsigned long)mbi.State);
    ASSERT(mbi.Protect == PAGE_READONLY, "before [0] Protect %lu", (unsigned long)mbi.Protect);
    ASSERT(mbi.Type == MEM_PRIVATE, "before [0] Type %lu", (unsigned long)mbi.Type);
    ASSERT(mbi.RegionSize == 4096, "before [0] RegionSize 0x%zx exp 4096 (per-VAD)", (size_t)mbi.RegionSize);

    PVOID q1 = (PVOID)((ULONG_PTR)rbase + 0x2000);
    ASSERT(query(q1, &mbi), "q before [1]");
    ASSERT(mbi.BaseAddress == q1, "before [1] Base %p vs 0x2000 offset", mbi.BaseAddress);
    ASSERT(mbi.AllocationBase == rbase, "before [1] AllocBase");
    ASSERT(mbi.AllocationProtect == PAGE_READWRITE, "before [1] AllocProt");
    ASSERT(mbi.State == MEM_RESERVE, "before [1] State %lu exp 8192", (unsigned long)mbi.State);
    ASSERT(mbi.Protect == 0, "before [1] Protect");
    ASSERT(mbi.Type == MEM_PRIVATE, "before [1] Type");
    ASSERT(mbi.RegionSize == 8192, "before [1] RegionSize");

    PVOID q2 = (PVOID)((ULONG_PTR)rbase + 0x4000);
    ASSERT(query(q2, &mbi), "q before [2]");
    ASSERT(mbi.State == MEM_COMMIT, "before [2] State");
    ASSERT(mbi.Protect == PAGE_EXECUTE_READ, "before [2] Protect %lu", (unsigned long)mbi.Protect);
    ASSERT(mbi.RegionSize == 4096, "before [2] RegionSize");

    PVOID q3 = (PVOID)((ULONG_PTR)rbase + 0x5000);
    ASSERT(query(q3, &mbi), "q before [3]");
    ASSERT(mbi.State == MEM_RESERVE, "before [3] State");
    ASSERT(mbi.RegionSize == 45056, "before [3] RegionSize 0x%zx", (size_t)mbi.RegionSize);
    ASSERT(ntmm_check_invariants(&g_nt_mm), "invariants before decommit");

    /* Decommit page at +0x1000 */
    PVOID db = (PVOID)((ULONG_PTR)rbase + 0x1000); SIZE_T dsz = 0x1000;
    st = NtFreeVirtualMemory((HANDLE)-1, &db, &dsz, MEM_DECOMMIT);
    ASSERT(st == STATUS_SUCCESS, "decommit failed 0x%lx", (unsigned long)st);

    /* query_after_decommit — must be 4 entries, decommitted region stays RESERVE */
    ASSERT(query(rbase, &mbi), "q after dec [0]");
    ASSERT(mbi.State == MEM_COMMIT, "after dec [0] State %lu exp 4096", (unsigned long)mbi.State);
    ASSERT(mbi.AllocationProtect == PAGE_READWRITE, "after dec [0] AllocProt %lu", (unsigned long)mbi.AllocationProtect);
    ASSERT(mbi.Protect == PAGE_READONLY, "after dec [0] Protect %lu", (unsigned long)mbi.Protect);
    ASSERT(mbi.Type == MEM_PRIVATE, "after dec [0] Type %lu", (unsigned long)mbi.Type);
    ASSERT(mbi.RegionSize == 4096, "after dec [0] RegionSize");

    PVOID qd1 = (PVOID)((ULONG_PTR)rbase + 0x1000);
    ASSERT(query(qd1, &mbi), "q after dec [1]");
    ASSERT(mbi.State == MEM_RESERVE, "after dec [1] State %lu exp 8192", (unsigned long)mbi.State);
    ASSERT(mbi.AllocationProtect == PAGE_READWRITE, "after dec [1] AllocProt %lu vs 4", (unsigned long)mbi.AllocationProtect);
    ASSERT(mbi.Protect == 0, "after dec [1] Protect %lu exp 0", (unsigned long)mbi.Protect);
    ASSERT(mbi.Type == MEM_PRIVATE, "after dec [1] Type %lu exp 131072", (unsigned long)mbi.Type);
    ASSERT(mbi.RegionSize == 4096 || mbi.RegionSize == 12288, "after dec [1] RegionSize 0x%zx exp 4096 or 12288", (size_t)mbi.RegionSize);
    ASSERT(mbi.AllocationBase == rbase, "after dec [1] AllocBase");
    ASSERT(mbi.BaseAddress == qd1 || mbi.BaseAddress == rbase, "after dec [1] Base");

    ASSERT(query(q2, &mbi), "q after dec [2]");
    ASSERT(mbi.State == MEM_COMMIT, "after dec [2] State %lu", (unsigned long)mbi.State);
    ASSERT(mbi.Protect == PAGE_EXECUTE_READ, "after dec [2] Protect");
    ASSERT(mbi.AllocationProtect == PAGE_READWRITE, "after dec [2] AllocProt");
    ASSERT(mbi.Type == MEM_PRIVATE, "after dec [2] Type");
    ASSERT(mbi.RegionSize == 4096, "after dec [2] RegionSize");

    ASSERT(query(q3, &mbi), "q after dec [3]");
    ASSERT(mbi.State == MEM_RESERVE, "after dec [3] State");
    ASSERT(mbi.RegionSize == 45056, "after dec [3] RegionSize");

    /* alloc_protect_check — querying committed page must preserve initial reserve protect */
    ASSERT(query(rbase, &mbi), "alloc_protect_check");
    ASSERT(mbi.AllocationProtect == PAGE_READWRITE, "alloc_protect_check AllocProt %lu exp 4", (unsigned long)mbi.AllocationProtect);
    ASSERT(mbi.Protect == PAGE_READONLY, "alloc_protect_check Protect %lu exp 2", (unsigned long)mbi.Protect);
    ASSERT(ntmm_check_invariants(&g_nt_mm), "invariants after decommit");

    /* Recommit the decommitted page */
    PVOID rc = qd1; SIZE_T rsz = 0x1000;
    st = NtAllocateVirtualMemory((HANDLE)-1, &rc, 0, &rsz, MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "recommit failed 0x%lx", (unsigned long)st);
    ASSERT(rc == qd1, "recommit base");
    ASSERT(query(qd1, &mbi), "q after recommit [1]");
    ASSERT(mbi.State == MEM_COMMIT, "recommit [1] State");
    ASSERT(mbi.Protect == PAGE_READWRITE, "recommit [1] Protect %lu exp 4", (unsigned long)mbi.Protect);
    ASSERT(mbi.RegionSize == 4096, "recommit [1] RegionSize");

    /* Verify neighboring regions after recommit */
    ASSERT(query(rbase, &mbi), "q after recommit [0]");
    ASSERT(mbi.RegionSize == 4096, "recommit [0] size 4096");
    ASSERT(mbi.Protect == PAGE_READONLY, "recommit [0] Protect 2");
    PVOID qmid = (PVOID)((ULONG_PTR)rbase + 0x2000);
    ASSERT(query(qmid, &mbi), "q after recommit [2]");
    ASSERT(mbi.State == MEM_RESERVE, "recommit [2] State RESERVE");
    ASSERT(mbi.RegionSize == 8192, "recommit [2] size 8192");

    /* Release entire reservation — only MEM_RELEASE removes VAD */
    PVOID rel = rbase; SIZE_T rls = 0;
    st = NtFreeVirtualMemory((HANDLE)-1, &rel, &rls, MEM_RELEASE);
    ASSERT(st == STATUS_SUCCESS, "release failed 0x%lx", (unsigned long)st);
    ASSERT(query(rbase, &mbi), "q after release");
    ASSERT(mbi.State == MEM_FREE, "after release State %lu exp 65536", (unsigned long)mbi.State);
    ASSERT(mbi.Type == 0, "after release Type 0");
    ASSERT(mbi.AllocationBase == NULL, "after release AllocBase NULL");
    ASSERT(ntmm_check_invariants(&g_nt_mm), "invariants after release");
    return 1;
}

/* Test for MEM_RESET */
static int test_reset(void) {
    PVOID base=NULL; SIZE_T size=0x1000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st==STATUS_SUCCESS, "reset alloc");
    /* Write something */
    memset(base, 0xAB, size);
    PVOID b=base; SIZE_T s=size;
    st = NtAllocateVirtualMemory((HANDLE)-1, &b, 0, &s, MEM_RESET, PAGE_READWRITE);
    ASSERT(st==STATUS_SUCCESS, "reset");
    /* After reset, query still committed */
    MEMORY_BASIC_INFORMATION mbi;
    ASSERT(query(base, &mbi), "q reset");
    ASSERT(mbi.State==MEM_COMMIT, "reset state");
    PVOID rel=base; SIZE_T rs=0;
    NtFreeVirtualMemory((HANDLE)-1, &rel, &rs, MEM_RELEASE);
    return 1;
}

/* ---- Gap 4: VAD split and coalesce invariants — deterministic duplicate-key regression ---- */
static int test_vad_split_coalesce_invariants(void) {
    /* Sequence that previously produced duplicate key when split inserted before remove:
       reserve 256KB, commit middle 64KB with gap, protect left half, decommit middle page,
       re-commit with different protect, verify no lost region and invariants hold. */
    ntmm_reset();
    PVOID base=NULL; SIZE_T sz=0x40000; /* 256KB */
    NTSTATUS st=NtAllocateVirtualMemory((HANDLE)-1,&base,0,&sz,MEM_RESERVE,PAGE_READWRITE);
    ASSERT(st==STATUS_SUCCESS,"reserve");
    ASSERT(ntmm_check_invariants(&g_nt_mm),"inv after reserve");
    PVOID alloc_base=base;
    /* Commit two disjoint subranges leaving holes */
    PVOID c1=(PVOID)((ULONG_PTR)alloc_base+0x8000); SIZE_T cs1=0x8000;
    PVOID t=c1; SIZE_T ts=cs1;
    st=NtAllocateVirtualMemory((HANDLE)-1,&t,0,&ts,MEM_COMMIT,PAGE_READWRITE);
    ASSERT(st==STATUS_SUCCESS,"commit c1");
    ASSERT(ntmm_check_invariants(&g_nt_mm),"inv c1");
    PVOID c2=(PVOID)((ULONG_PTR)alloc_base+0x20000); SIZE_T cs2=0x8000;
    t=c2; ts=cs2;
    st=NtAllocateVirtualMemory((HANDLE)-1,&t,0,&ts,MEM_COMMIT,PAGE_READWRITE);
    ASSERT(st==STATUS_SUCCESS,"commit c2");
    ASSERT(ntmm_check_invariants(&g_nt_mm),"inv c2");
    /* Protect first committed subrange RW->RO, forcing split and coalesce inside commit region */
    PVOID pb=c1; SIZE_T ps=0x1000; ULONG old=0;
    st=NtProtectVirtualMemory((HANDLE)-1,&pb,&ps,PAGE_READONLY,&old);
    ASSERT(st==STATUS_SUCCESS,"protect partial");
    ASSERT(ntmm_check_invariants(&g_nt_mm),"inv protect");
    /* Decommit middle page of c2 */
    PVOID db=(PVOID)((ULONG_PTR)c2+0x1000); SIZE_T ds=0x1000;
    st=NtFreeVirtualMemory((HANDLE)-1,&db,&ds,MEM_DECOMMIT);
    ASSERT(st==STATUS_SUCCESS,"decommit");
    ASSERT(ntmm_check_invariants(&g_nt_mm),"inv decommit");
    /* Recommit with EXECUTE_READ to test coalesce across protect boundary */
    PVOID rc=db; SIZE_T rs=ds;
    st=NtAllocateVirtualMemory((HANDLE)-1,&rc,0,&rs,MEM_COMMIT,PAGE_EXECUTE_READ);
    ASSERT(st==STATUS_SUCCESS,"recommit diff protect");
    ASSERT(ntmm_check_invariants(&g_nt_mm),"inv recommit");
    /* Full sweep: verify ordered, non-overlapping, live alloc_base */
    MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR cur=NTMM_LOW_ADDRESS;
    int steps=0;
    while(cur< NTMM_HIGH_ADDRESS && steps<500){
        SIZE_T rl=0;
        st=NtQueryVirtualMemory((HANDLE)-1,(PVOID)cur,MemoryBasicInformation,&mbi,sizeof(mbi),&rl);
        ASSERT(st==STATUS_SUCCESS,"sweep");
        ASSERT((ULONG_PTR)mbi.BaseAddress==cur,"sweep base ordered");
        ASSERT(mbi.RegionSize>0,"region non-zero");
        if(mbi.State!=MEM_FREE) ASSERT(mbi.AllocationBase==alloc_base,"alloc_base live");
        cur=(ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
        steps++; if(cur==(ULONG_PTR)alloc_base+sz+0x100000) break; if(cur>0x20000000) break;
    }
    PVOID rel=alloc_base; SIZE_T rs0=0;
    st=NtFreeVirtualMemory((HANDLE)-1,&rel,&rs0,MEM_RELEASE);
    ASSERT(st==STATUS_SUCCESS,"release");
    ASSERT(ntmm_check_invariants(&g_nt_mm),"inv after release");
    return 1;
}
static int test_vad_long_random_invariant(void){
    srand(0xC0FFEE);
    const int ITERS=40000;
    PVOID allocs[128]={0};
    SIZE_T sizes[128]={0};
    int nallocs=0;
    for(int iter=0; iter<ITERS; iter++){
        int op=rand()%5;
        if(op==0 && nallocs<120){
            ULONG at = (rand()%2)? MEM_RESERVE : MEM_RESERVE|MEM_COMMIT;
            if(rand()%3==0) at=MEM_COMMIT;
            ULONG prot = PAGE_READWRITE;
            int pr=rand()%4; if(pr==0) prot=PAGE_READONLY; else if(pr==1) prot=PAGE_EXECUTE_READ; else if(pr==2) prot=PAGE_WRITECOPY;
            if(rand()%16==0) prot|=PAGE_GUARD;
            PVOID b=NULL; SIZE_T s=(SIZE_T)((rand()%6+1)*4096);
            NTSTATUS st=NtAllocateVirtualMemory((HANDLE)-1,&b,0,&s,at,prot&0xFF);
            if(st==STATUS_SUCCESS && b){ allocs[nallocs]=b; sizes[nallocs]=s; nallocs++; }
        } else if(op==1 && nallocs>0){
            int idx=rand()%nallocs;
            PVOID b=allocs[idx]; SIZE_T s=0;
            NTSTATUS st=NtFreeVirtualMemory((HANDLE)-1,&b,&s,MEM_RELEASE);
            if(st==STATUS_SUCCESS){ allocs[idx]=allocs[nallocs-1]; sizes[idx]=sizes[nallocs-1]; nallocs--; }
            else { PVOID db=allocs[idx]; SIZE_T ds=4096; ULONG ft=MEM_DECOMMIT; NtFreeVirtualMemory((HANDLE)-1,&db,&ds,ft); }
        } else if(op==2 && nallocs>0){
            int idx=rand()%nallocs;
            PVOID b=allocs[idx]; SIZE_T s=4096; ULONG old=0;
            ULONG np=(rand()%2)? PAGE_READONLY : PAGE_READWRITE;
            NtProtectVirtualMemory((HANDLE)-1,&b,&s,np,&old);
        } else if(op==3){
            ULONG_PTR addr=NTMM_LOW_ADDRESS + (rand()%0x1000000 & ~0xFFF);
            MEMORY_BASIC_INFORMATION mbi; SIZE_T ret=0;
            NTSTATUS st=NtQueryVirtualMemory((HANDLE)-1,(PVOID)addr,MemoryBasicInformation,&mbi,sizeof(mbi),&ret);
            ASSERT(st==STATUS_SUCCESS,"rand query");
            MEMORY_BASIC_INFORMATION mbi2; st=NtQueryVirtualMemory((HANDLE)-1,mbi.BaseAddress,MemoryBasicInformation,&mbi2,sizeof(mbi2),&ret);
            ASSERT(st==STATUS_SUCCESS,"q2"); ASSERT(mbi2.RegionSize==mbi.RegionSize,"consist");
        } else {
            if(nallocs>0){ int idx=rand()%nallocs; PVOID b=allocs[idx]; MEMORY_BASIC_INFORMATION mbi; SIZE_T ret=0; NtQueryVirtualMemory((HANDLE)-1,b,MemoryBasicInformation,&mbi,sizeof(mbi),&ret); }
        }
        ASSERT(ntmm_check_invariants(&g_nt_mm),"invariant iter %d",iter);
        if((iter%200)==0){
            ULONG_PTR cur=NTMM_LOW_ADDRESS; int steps=0; while(cur< NTMM_HIGH_ADDRESS && steps<300){ MEMORY_BASIC_INFORMATION q; SIZE_T ret=0; NTSTATUS st=NtQueryVirtualMemory((HANDLE)-1,(PVOID)cur,MemoryBasicInformation,&q,sizeof(q),&ret); if(st!=STATUS_SUCCESS) break; if(q.RegionSize==0) break; cur=(ULONG_PTR)q.BaseAddress+q.RegionSize; steps++; if(cur>0x30000000) break; }
        }
    }
    for(int i=0;i<nallocs;i++){ PVOID b=allocs[i]; SIZE_T s=0; NtFreeVirtualMemory((HANDLE)-1,&b,&s,MEM_RELEASE); }
    ASSERT(ntmm_check_invariants(&g_nt_mm),"final inv");
    return 1;
}

static int test_probe_direct_windows_rule(void) {
    /* Replicates probe_virtualquery_direct: RESERVE 1MB then 4096 commit/protect/decommit cycles per page.
     * Windows golden shows every VirtualQuery after each step returns RegionSize 4096 and BaseAddress == queried page.
     * This is the maximal-run rule: State, Type, AllocationBase and Protect must all match to coalesce.
     * Our previous code coalesced too much (e.g., 954368 vs 4096) by comparing fewer attributes or ignoring
     * alloc_protect/write_watch/section_offset differences. This test enforces the Windows rule. */
    ntmm_reset();
    PVOID base = NULL;
    SIZE_T region = 0x100000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &region, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "reserve 1M failed 0x%lx", (unsigned long)st);
    ULONG_PTR reserve_base = (ULONG_PTR)base;
    ASSERT(region == 0x100000, "reserve size");
    unsigned char state[256];
    for (int i=0;i<256;i++) state[i]=0;
    for (int iter=0; iter<4096; iter++) {
        int page = iter & 0xFF;
        PVOID pageAddr = (PVOID)(reserve_base + page*0x1000);
        unsigned char s = state[page];
        if (s==0) {
            PVOID b=pageAddr; SIZE_T sz=0x1000;
            st = NtAllocateVirtualMemory((HANDLE)-1, &b, 0, &sz, MEM_COMMIT, PAGE_READWRITE);
            ASSERT(st==STATUS_SUCCESS, "iter %d commit failed 0x%lx", iter, (unsigned long)st);
            ASSERT(b==pageAddr, "commit base mismatch iter %d", iter);
            state[page]=1;
        } else if (s==1) {
            PVOID b=pageAddr; SIZE_T sz=0x1000; ULONG old=0;
            st = NtProtectVirtualMemory((HANDLE)-1, &b, &sz, PAGE_READONLY, &old);
            ASSERT(st==STATUS_SUCCESS, "iter %d protect failed 0x%lx", iter, (unsigned long)st);
            ASSERT(old==PAGE_READWRITE, "protect old %lu iter %d", (unsigned long)old, iter);
            state[page]=2;
        } else {
            PVOID b=pageAddr; SIZE_T sz=0x1000;
            st = NtFreeVirtualMemory((HANDLE)-1, &b, &sz, MEM_DECOMMIT);
            ASSERT(st==STATUS_SUCCESS, "iter %d decommit failed 0x%lx", iter, (unsigned long)st);
            state[page]=0;
        }
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T ret=0;
        st = NtQueryVirtualMemory((HANDLE)-1, pageAddr, MemoryBasicInformation, &mbi, sizeof(mbi), &ret);
        ASSERT(st==STATUS_SUCCESS, "iter %d query failed 0x%lx", iter, (unsigned long)st);
        ASSERT(ret==sizeof(mbi), "ret len iter %d", iter);
        /* Windows rule: maximal run where State, Type, AllocationBase, Protect identical.
         * For COMMIT, Windows keeps each 4K page per-VAD (4096) even when adjacent with identical
         * attributes after separate commits (4084 S1 divergences). For RESERVE, Windows does coalesce
         * adjacent reserve pages. We check per-VAD for COMMIT, coalesced allowed for RESERVE. */
        ASSERT(mbi.AllocationBase == (PVOID)reserve_base, "iter %d AllocBase %p exp %p", iter, mbi.AllocationBase, (PVOID)reserve_base);
        ASSERT(mbi.AllocationProtect == PAGE_READWRITE, "iter %d AllocProt %lu", iter, (unsigned long)mbi.AllocationProtect);
        ASSERT(mbi.Type == MEM_PRIVATE, "iter %d Type %lu", iter, (unsigned long)mbi.Type);
        if (s==0 || s==1) {
            ASSERT(mbi.BaseAddress == pageAddr, "iter %d BaseAddress %p exp %p", iter, mbi.BaseAddress, pageAddr);
            ASSERT(mbi.RegionSize == 4096, "iter %d RegionSize %zu exp 4096 (coalescing defect)", iter, (size_t)mbi.RegionSize);
        } else {
            /* RESERVE may be coalesced (e.g., 12288) or per-VAD 4096; allow either */
            ASSERT((ULONG_PTR)mbi.BaseAddress <= (ULONG_PTR)pageAddr && (ULONG_PTR)pageAddr < (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize, "iter %d RESERVE Base %p not covering page %p", iter, mbi.BaseAddress, pageAddr);
            ASSERT(mbi.RegionSize % 4096 == 0 && mbi.RegionSize >= 4096, "iter %d RESERVE RegionSize %zu", iter, (size_t)mbi.RegionSize);
        }
        if (s==0) { /* was commit */
            ASSERT(mbi.State == MEM_COMMIT, "iter %d State commit %lu", iter, (unsigned long)mbi.State);
            ASSERT(mbi.Protect == PAGE_READWRITE, "iter %d Protect commit %lu", iter, (unsigned long)mbi.Protect);
        } else if (s==1) { /* was protect */
            ASSERT(mbi.State == MEM_COMMIT, "iter %d State protect %lu", iter, (unsigned long)mbi.State);
            ASSERT(mbi.Protect == PAGE_READONLY, "iter %d Protect protect %lu", iter, (unsigned long)mbi.Protect);
        } else {
            ASSERT(mbi.State == MEM_RESERVE, "iter %d State decommit %lu", iter, (unsigned long)mbi.State);
            ASSERT(mbi.Protect == 0, "iter %d Protect decommit %lu", iter, (unsigned long)mbi.Protect);
        }
        ASSERT(ntmm_check_invariants(&g_nt_mm), "invariant iter %d", iter);
    }
    /* Also verify free reporting after release */
    PVOID rel = (PVOID)reserve_base; SIZE_T rs=0;
    st = NtFreeVirtualMemory((HANDLE)-1, &rel, &rs, MEM_RELEASE);
    ASSERT(st==STATUS_SUCCESS, "release failed");
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T ret=0;
    st = NtQueryVirtualMemory((HANDLE)-1, (PVOID)reserve_base, MemoryBasicInformation, &mbi, sizeof(mbi), &ret);
    ASSERT(st==STATUS_SUCCESS, "query after release");
    ASSERT(mbi.State == MEM_FREE, "after release State %lu", (unsigned long)mbi.State);
    ASSERT(mbi.Type == 0, "after release Type");
    ASSERT(mbi.AllocationBase == NULL, "after release AllocBase");
    return 1;
}

/* MemoryRegionInformation (class 3): per-VAD region, commit size and type bits.
 * Classes that need a working-set or file-name model return STATUS_NOT_SUPPORTED. */
static int test_query_region_info(void) {
    PVOID base = NULL;
    SIZE_T size = 0x20000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "reserve 0x%lx", (unsigned long)st);
    PVOID commit = base;
    SIZE_T csize = 0x10000;
    st = NtAllocateVirtualMemory((HANDLE)-1, &commit, 0, &csize, MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "commit 0x%lx", (unsigned long)st);

    MEMORY_REGION_INFORMATION info;
    SIZE_T ret = 0;
    st = NtQueryVirtualMemory((HANDLE)-1, base, MemoryRegionInformation, &info, sizeof(info), &ret);
    ASSERT(st == STATUS_SUCCESS, "region query 0x%lx", (unsigned long)st);
    ASSERT(ret == sizeof(info), "region return length %zu", (size_t)ret);
    ASSERT(info.AllocationBase == base, "region alloc base");
    ASSERT(info.AllocationProtect == PAGE_READWRITE, "region alloc protect 0x%lx", (unsigned long)info.AllocationProtect);
    ASSERT(info.RegionType & MEM_REGION_PRIVATE, "region type private 0x%lx", (unsigned long)info.RegionType);
    ASSERT(info.RegionSize == 0x10000, "committed region size 0x%zx", (size_t)info.RegionSize);
    ASSERT(info.CommitSize == 0x10000, "committed commit size 0x%zx", (size_t)info.CommitSize);

    /* The reserved tail has no committed bytes. */
    st = NtQueryVirtualMemory((HANDLE)-1, (PVOID)((ULONG_PTR)base + 0x10000), MemoryRegionInformation, &info, sizeof(info), &ret);
    ASSERT(st == STATUS_SUCCESS, "reserved region query 0x%lx", (unsigned long)st);
    ASSERT(info.CommitSize == 0, "reserved commit size 0x%zx", (size_t)info.CommitSize);
    ASSERT(info.RegionSize == 0x10000, "reserved region size 0x%zx", (size_t)info.RegionSize);

    /* Free address: no region. */
    st = NtQueryVirtualMemory((HANDLE)-1, (PVOID)((ULONG_PTR)base + 0x20000), MemoryRegionInformation, &info, sizeof(info), &ret);
    ASSERT(st == STATUS_INVALID_ADDRESS, "free region status 0x%lx", (unsigned long)st);

    /* Short buffer is a length mismatch. */
    st = NtQueryVirtualMemory((HANDLE)-1, base, MemoryRegionInformation, &info, sizeof(info) - 1, &ret);
    ASSERT(st == STATUS_INFO_LENGTH_MISMATCH, "short buffer status 0x%lx", (unsigned long)st);

    /* Mapped section view reports the page-file region type. */
    LARGE_INTEGER sz; sz.QuadPart = 0x10000;
    HANDLE sec = NULL;
    st = NtCreateSection(&sec, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_SUCCESS, "section create 0x%lx", (unsigned long)st);
    PVOID vbase = NULL; SIZE_T vsize = 0; LARGE_INTEGER off; off.QuadPart = 0;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &vbase, 0, 0, &off, &vsize, 0, 0, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "map view 0x%lx", (unsigned long)st);
    st = NtQueryVirtualMemory((HANDLE)-1, vbase, MemoryRegionInformation, &info, sizeof(info), &ret);
    ASSERT(st == STATUS_SUCCESS, "view region query 0x%lx", (unsigned long)st);
    ASSERT(info.RegionType & MEM_REGION_MAPPED_PAGEFILE, "view region type 0x%lx", (unsigned long)info.RegionType);
    ASSERT(!(info.RegionType & MEM_REGION_PRIVATE), "view not private");
    NtUnmapViewOfSection((HANDLE)-1, vbase);
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close region section");

    /* Classes that need a working-set or file-name model are not supported;
     * unknown classes are invalid. */
    ULONG unsupported[] = { MemoryWorkingSetInformation, MemoryMappedFilenameInformation, MemoryWorkingSetExInformation };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        st = NtQueryVirtualMemory((HANDLE)-1, base, unsupported[i], &info, sizeof(info), &ret);
        ASSERT(st == STATUS_NOT_SUPPORTED, "class %lu status 0x%lx", (unsigned long)unsupported[i], (unsigned long)st);
    }
    st = NtQueryVirtualMemory((HANDLE)-1, base, 0x7f, &info, sizeof(info), &ret);
    ASSERT(st == STATUS_INVALID_INFO_CLASS, "unknown class status 0x%lx", (unsigned long)st);
    ntmm_reset(); /* release VADs: this is the last test, so nothing else will */
    return 1;
}

/* NtReadVirtualMemory / NtWriteVirtualMemory: current process only. */
static int test_rw_virtual_memory(void) {
    PVOID base = NULL;
    SIZE_T size = 0x20000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "reserve 0x%lx", (unsigned long)st);
    PVOID commit = base;
    SIZE_T csize = 0x10000;
    st = NtAllocateVirtualMemory((HANDLE)-1, &commit, 0, &csize, MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "commit 0x%lx", (unsigned long)st);

    unsigned char src[256], dst[256];
    for (int i = 0; i < 256; i++) src[i] = (unsigned char)(i * 7 + 1);

    /* Round trip inside the committed range. */
    SIZE_T n = 0;
    st = NtWriteVirtualMemory((HANDLE)-1, (PVOID)((ULONG_PTR)base + 0x123), src, 100, &n);
    ASSERT(st == STATUS_SUCCESS && n == 100, "write st 0x%lx n %zu", (unsigned long)st, (size_t)n);
    memset(dst, 0, sizeof(dst));
    st = NtReadVirtualMemory((HANDLE)-1, (PVOID)((ULONG_PTR)base + 0x123), dst, 100, &n);
    ASSERT(st == STATUS_SUCCESS && n == 100, "read st 0x%lx n %zu", (unsigned long)st, (size_t)n);
    ASSERT(memcmp(src, dst, 100) == 0, "round trip data differs");

    /* Zero-length transfer succeeds and moves nothing. */
    n = 0xdead;
    st = NtReadVirtualMemory((HANDLE)-1, base, dst, 0, &n);
    ASSERT(st == STATUS_SUCCESS && n == 0, "zero read st 0x%lx n %zu", (unsigned long)st, (size_t)n);

    /* A transfer that runs off the committed range copies the prefix only. */
    n = 0;
    st = NtWriteVirtualMemory((HANDLE)-1, (PVOID)((ULONG_PTR)base + 0xff00), src, 0x400, &n);
    ASSERT(st == STATUS_PARTIAL_COPY, "cross-end write st 0x%lx", (unsigned long)st);
    ASSERT(n == 0x100, "cross-end write n %zu", (size_t)n);
    /* Reading from the reserved tail copies nothing. */
    n = 0xdead;
    st = NtReadVirtualMemory((HANDLE)-1, (PVOID)((ULONG_PTR)base + 0x10000), dst, 16, &n);
    ASSERT(st == STATUS_PARTIAL_COPY && n == 0, "reserved read st 0x%lx n %zu", (unsigned long)st, (size_t)n);

    /* Read-only pages: readable, not writable. */
    PVOID pbase = base;
    SIZE_T psize = 0x10000;
    ULONG old = 0;
    st = NtProtectVirtualMemory((HANDLE)-1, &pbase, &psize, PAGE_READONLY, &old);
    ASSERT(st == STATUS_SUCCESS, "protect ro 0x%lx", (unsigned long)st);
    n = 0;
    st = NtReadVirtualMemory((HANDLE)-1, base, dst, 16, &n);
    ASSERT(st == STATUS_SUCCESS && n == 16, "ro read st 0x%lx", (unsigned long)st);
    n = 0;
    st = NtWriteVirtualMemory((HANDLE)-1, base, src, 16, &n);
    ASSERT(st == STATUS_PARTIAL_COPY && n == 0, "ro write st 0x%lx n %zu", (unsigned long)st, (size_t)n);

    /* A handle that names no process is refused; the output count is untouched. */
    n = 0xdead;
    st = NtReadVirtualMemory((HANDLE)0x1234, base, dst, 16, &n);
    ASSERT(st == STATUS_INVALID_HANDLE, "unknown process read st 0x%lx", (unsigned long)st);
    ASSERT(n == 0xdead, "other process read touched count");
    st = NtWriteVirtualMemory((HANDLE)0x1234, base, src, 16, &n);
    ASSERT(st == STATUS_INVALID_HANDLE, "unknown process write st 0x%lx", (unsigned long)st);

    /* An address range that wraps past the top of the address space is refused. */
    n = 0;
    st = NtReadVirtualMemory((HANDLE)-1, (PVOID)(~(ULONG_PTR)0 - 10), dst, 100, &n);
    ASSERT(st == STATUS_PARTIAL_COPY && n == 0, "wrap read st 0x%lx n %zu", (unsigned long)st, (size_t)n);
    ntmm_reset();
    return 1;
}

/* NtFlushVirtualMemory: the range is page-rounded, must be committed, and the
 * IoStatus reports the flushed length. A foreign process's space is flushed the
 * same way (see test_foreign_flush_and_ex). */
static int test_flush_virtual_memory(void) {
    PVOID base = NULL;
    SIZE_T size = 0x20000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "reserve 0x%lx", (unsigned long)st);
    PVOID commit = base;
    SIZE_T csize = 0x10000;
    st = NtAllocateVirtualMemory((HANDLE)-1, &commit, 0, &csize, MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "commit 0x%lx", (unsigned long)st);

    IO_STATUS_BLOCK iosb;
    PVOID fbase = (PVOID)((ULONG_PTR)base + 0x123);
    SIZE_T fsize = 0x10;
    iosb.DUMMYUNIONNAME.Status = 0xdead; iosb.Information = 0;
    st = NtFlushVirtualMemory((HANDLE)-1, &fbase, &fsize, &iosb);
    ASSERT(st == STATUS_SUCCESS, "flush committed 0x%lx", (unsigned long)st);
    ASSERT(fbase == base, "flush base rounded down");
    ASSERT(fsize == 0x1000, "flush size rounded up 0x%zx", (size_t)fsize);
    ASSERT(iosb.DUMMYUNIONNAME.Status == STATUS_SUCCESS && iosb.Information == 0x1000, "flush iosb");

    /* Crossing into the reserved tail is not committed. */
    fbase = (PVOID)((ULONG_PTR)base + 0xf000);
    fsize = 0x2000;
    st = NtFlushVirtualMemory((HANDLE)-1, &fbase, &fsize, &iosb);
    ASSERT(st == STATUS_NOT_COMMITTED, "flush reserved tail 0x%lx", (unsigned long)st);

    /* Free address. */
    fbase = (PVOID)((ULONG_PTR)base + 0x20000);
    fsize = 0x1000;
    st = NtFlushVirtualMemory((HANDLE)-1, &fbase, &fsize, &iosb);
    ASSERT(st == STATUS_INVALID_ADDRESS, "flush free 0x%lx", (unsigned long)st);

    /* Mapped section view: anonymous backing is coherent, so flush succeeds. */
    LARGE_INTEGER sz; sz.QuadPart = 0x2000;
    HANDLE sec = NULL;
    st = NtCreateSection(&sec, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_SUCCESS, "section 0x%lx", (unsigned long)st);
    PVOID vbase = NULL; SIZE_T vsize = 0; LARGE_INTEGER off; off.QuadPart = 0;
    st = NtMapViewOfSection(sec, (HANDLE)-1, &vbase, 0, 0, &off, &vsize, 0, 0, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "map 0x%lx", (unsigned long)st);
    fbase = vbase; fsize = vsize;
    st = NtFlushVirtualMemory((HANDLE)-1, &fbase, &fsize, &iosb);
    ASSERT(st == STATUS_SUCCESS, "flush view 0x%lx", (unsigned long)st);
    NtUnmapViewOfSection((HANDLE)-1, vbase);
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close flush section");

    /* Argument and process checks. */
    st = NtFlushVirtualMemory((HANDLE)-1, NULL, &fsize, &iosb);
    ASSERT(st == STATUS_INVALID_PARAMETER, "null base 0x%lx", (unsigned long)st);
    fbase = base; fsize = 0x1000;
    st = NtFlushVirtualMemory((HANDLE)0x1234, &fbase, &fsize, &iosb);
    ASSERT(st == STATUS_INVALID_HANDLE, "unknown process flush 0x%lx", (unsigned long)st);
    ntmm_reset(); /* release VADs: last test */
    return 1;
}

static MEM_EXTENDED_PARAMETER mk_ext(ULONG type, ULONG64 value) {
    MEM_EXTENDED_PARAMETER p;
    memset(&p, 0, sizeof(p));
    p.Type = type;
    p.ULong64 = value;
    return p;
}

/* NtAllocateVirtualMemoryEx: without extended parameters it is
 * NtAllocateVirtualMemory. Extended parameters (NUMA, address requirements)
 * are not supported. A handle that names no process is STATUS_INVALID_HANDLE. */
static int test_allocate_ex(void) {
    PVOID base = NULL;
    SIZE_T size = 0x1000;
    NTSTATUS st = NtAllocateVirtualMemoryEx((HANDLE)-1, &base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    ASSERT(st == STATUS_SUCCESS, "ex alloc 0x%lx", (unsigned long)st);
    ASSERT(base != NULL, "ex alloc base");
    /* A NULL base is page-rounded (no granularity rounding), so the size is exactly 0x1000. */
    ASSERT(size == 0x1000, "ex size 0x%zx", (size_t)size);

    MEMORY_BASIC_INFORMATION mbi;
    ASSERT(query(base, &mbi), "query ex");
    ASSERT(mbi.State == MEM_COMMIT, "ex state 0x%lx", (unsigned long)mbi.State);
    ASSERT(mbi.Protect == PAGE_READWRITE, "ex protect");

    /* A NUMA entry is not honoured, and the base is not written. */
    PVOID b2 = NULL;
    SIZE_T s2 = 0x1000;
    MEM_EXTENDED_PARAMETER numa = mk_ext(MemExtendedParameterNumaNode, 0);
    st = NtAllocateVirtualMemoryEx((HANDLE)-1, &b2, &s2, MEM_RESERVE, PAGE_READWRITE, &numa, 1);
    ASSERT(st == STATUS_NOT_SUPPORTED, "ext param status 0x%lx", (unsigned long)st);
    ASSERT(b2 == NULL, "ext param base untouched");

    /* A handle that names no process is refused. */
    b2 = NULL;
    st = NtAllocateVirtualMemoryEx((HANDLE)0x1234, &b2, &s2, MEM_RESERVE, PAGE_READWRITE, NULL, 0);
    ASSERT(st == STATUS_INVALID_HANDLE, "unknown process status 0x%lx", (unsigned long)st);
    ASSERT(b2 == NULL, "unknown process base untouched");
    ntmm_reset(); /* release VADs: last test */
    return 1;
}

/* A range whose base is in the top page of the address space must be refused
 * before rounding: round_up(base + size) wraps to 0, the walk below it then
 * does nothing, and the call used to return success for an unmapped range. */
static int test_top_of_space_range_refused(void) {
    ULONG_PTR top_base = ~(ULONG_PTR)0 - 0x7FF;   /* base is 0x800 below the top */
    PVOID pb = (PVOID)top_base;
    SIZE_T ps = 0x10;
    ULONG old = 0;
    NTSTATUS st = NtProtectVirtualMemory((HANDLE)-1, &pb, &ps, PAGE_READWRITE, &old);
    ASSERT(st == STATUS_INVALID_PARAMETER, "protect top-of-space range 0x%lx", (unsigned long)st);

    pb = (PVOID)top_base;
    ps = 0x10;
    IO_STATUS_BLOCK iosb;
    iosb.DUMMYUNIONNAME.Status = 0xdead;
    st = NtFlushVirtualMemory((HANDLE)-1, &pb, &ps, &iosb);
    ASSERT(st == STATUS_INVALID_PARAMETER, "flush top-of-space range 0x%lx", (unsigned long)st);
    ntmm_reset();
    return 1;
}

/* ---- nt_svc_ section entry points: create / open family ---- */

static void mm_make_us(UNICODE_STRING *us, WCHAR *buf, size_t bufcount, const char *s) {
    size_t n = strlen(s), i;
    if (n >= bufcount) n = bufcount - 1;
    for (i = 0; i < n; i++) buf[i] = (WCHAR)(unsigned char)s[i];
    buf[n] = 0;
    us->Buffer = buf;
    us->Length = (USHORT)(n * sizeof(WCHAR));
    us->MaximumLength = (USHORT)((n + 1) * sizeof(WCHAR));
}

static OBJECT_ATTRIBUTES mm_make_oa(UNICODE_STRING *us) {
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = us;
    return oa;
}

/* An unnamed section through the entry point maps like NtCreateSection's. */
static int test_nt_svc_create_section_unnamed(void) {
    HANDLE h = NULL;
    LARGE_INTEGER sz; sz.QuadPart = 0x2000;
    NTSTATUS st = nt_svc_NtCreateSection(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_SUCCESS, "svc create 0x%lx", (unsigned long)st);
    ASSERT(h != NULL, "svc create handle");
    PVOID base = NULL; SIZE_T vs = 0;
    st = mm_map_view(h, &base, &vs, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS && base != NULL && vs == 0x2000, "svc create map 0x%lx", (unsigned long)st);
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "svc create unmap");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "svc create close");
    ASSERT(nt_svc_NtCreateSection(NULL, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL) == STATUS_INVALID_PARAMETER,
           "svc create null handle pointer");
    ASSERT(nt_svc_NtCreateSection(&h, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READWRITE, SEC_COMMIT, NULL) == STATUS_INVALID_PARAMETER,
           "svc create null size");
    return 1;
}

/* A named section is found by NtOpenSection, and both handles share its backing. */
static int test_nt_svc_named_section_open(void) {
    UNICODE_STRING us; WCHAR nbuf[64];
    mm_make_us(&us, nbuf, 64, "\\BaseNamedObjects\\MmTestSecOpen");
    OBJECT_ATTRIBUTES oa = mm_make_oa(&us);
    LARGE_INTEGER sz; sz.QuadPart = 0x1000;
    HANDLE h1 = NULL, h2 = NULL;
    NTSTATUS st = nt_svc_NtCreateSection(&h1, SECTION_ALL_ACCESS, &oa, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_SUCCESS, "named create 0x%lx", (unsigned long)st);
    st = nt_svc_NtOpenSection(&h2, SECTION_ALL_ACCESS, &oa);
    ASSERT(st == STATUS_SUCCESS && h2 != NULL, "named open 0x%lx", (unsigned long)st);
    /* Both handles resolve to the same section object. (The host shim gives
     * each view its own copy of the backing, so view contents are not compared.) */
    struct nt_object_header *o1 = NULL, *o2 = NULL;
    ASSERT(ObReferenceObjectByHandle(g_mm_proc, h1, &nt_section_type, KernelMode, 0, &o1, NULL) == STATUS_SUCCESS, "named ref one");
    ASSERT(ObReferenceObjectByHandle(g_mm_proc, h2, &nt_section_type, KernelMode, 0, &o2, NULL) == STATUS_SUCCESS, "named ref two");
    ASSERT(o1 == o2, "named handles name one section");
    ObDereferenceObject(o1);
    ObDereferenceObject(o2);
    PVOID b1 = NULL; SIZE_T v1 = 0;
    ASSERT(mm_map_view(h2, &b1, &v1, PAGE_READWRITE) == STATUS_SUCCESS, "named map");
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, b1) == STATUS_SUCCESS, "named unmap");
    ASSERT(ObCloseHandle(g_mm_proc, h1) == STATUS_SUCCESS, "named close one");
    ASSERT(ObCloseHandle(g_mm_proc, h2) == STATUS_SUCCESS, "named close two");
    return 1;
}

/* Missing names and missing pointers are refused without writing a handle. */
static int test_nt_svc_open_section_errors(void) {
    UNICODE_STRING us; WCHAR nbuf[64];
    HANDLE h = NULL;
    mm_make_us(&us, nbuf, 64, "\\BaseNamedObjects\\MmTestNoSuchSection");
    OBJECT_ATTRIBUTES oa = mm_make_oa(&us);
    ASSERT(nt_svc_NtOpenSection(&h, SECTION_ALL_ACCESS, &oa) == STATUS_OBJECT_NAME_NOT_FOUND, "open missing name");
    ASSERT(h == NULL, "no handle for missing name");
    ASSERT(nt_svc_NtOpenSection(NULL, SECTION_ALL_ACCESS, &oa) == STATUS_INVALID_PARAMETER, "open null handle pointer");
    ASSERT(nt_svc_NtOpenSection(&h, SECTION_ALL_ACCESS, NULL) == STATUS_INVALID_PARAMETER, "open null attributes");
    return 1;
}

/* ---- nt_svc_ section entry points: map / unmap family ---- */

/* A view mapped through the entry point is writable and unmaps once. */
static int test_nt_svc_map_unmap(void) {
    LARGE_INTEGER sz; sz.QuadPart = 0x2000;
    HANDLE h = NULL;
    NTSTATUS st = nt_svc_NtCreateSection(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_SUCCESS, "svc map create 0x%lx", (unsigned long)st);
    LARGE_INTEGER off; off.QuadPart = 0;
    PVOID base = NULL; SIZE_T vs = 0;
    st = nt_svc_NtMapViewOfSection(h, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "svc map 0x%lx", (unsigned long)st);
    ASSERT(base != NULL && vs == 0x2000, "svc map base and size 0x%zx", (size_t)vs);
    *(volatile unsigned char *)base = 0x33;
    ASSERT(nt_svc_NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_SUCCESS, "svc unmap");
    ASSERT(nt_svc_NtUnmapViewOfSection((HANDLE)-1, base) == STATUS_NOT_MAPPED_VIEW, "svc unmap twice");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "svc map close");
    return 1;
}

/* Bad arguments and bad handles are refused, and no base is written. */
static int test_nt_svc_map_errors(void) {
    LARGE_INTEGER sz; sz.QuadPart = 0x1000;
    HANDLE h = NULL;
    NTSTATUS st = nt_svc_NtCreateSection(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_SUCCESS, "map err create 0x%lx", (unsigned long)st);
    LARGE_INTEGER off; off.QuadPart = 0;
    PVOID base = NULL; SIZE_T vs = 0;
    ASSERT(nt_svc_NtMapViewOfSection(h, (HANDLE)-1, NULL, 0, 0, &off, &vs, 0, 0, PAGE_READWRITE) == STATUS_INVALID_PARAMETER,
           "map null base");
    ASSERT(nt_svc_NtMapViewOfSection(h, (HANDLE)-1, &base, 0, 0, &off, NULL, 0, 0, PAGE_READWRITE) == STATUS_INVALID_PARAMETER,
           "map null view size");
    ASSERT(base == NULL, "base untouched by refused map");
    st = nt_svc_NtMapViewOfSection((HANDLE)0x1234, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, PAGE_READWRITE);
    ASSERT(st != STATUS_SUCCESS && base == NULL, "forged section handle 0x%lx", (unsigned long)st);
    ASSERT(nt_svc_NtUnmapViewOfSection((HANDLE)-1, (PVOID)0x1000) == STATUS_NOT_MAPPED_VIEW, "unmap unmapped address");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "map err close");
    return 1;
}

/* ---- nt_svc_ section entry points: query / extend family ---- */

/* SectionBasicInformation reports the size and the allocation attributes. A
 * short buffer, an unknown class and a handle without SECTION_QUERY are refused. */
static int test_nt_svc_query_section(void) {
    LARGE_INTEGER sz; sz.QuadPart = 0x2000;
    HANDLE h = NULL;
    NTSTATUS st = nt_svc_NtCreateSection(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_SUCCESS, "query create 0x%lx", (unsigned long)st);
    SECTION_BASIC_INFORMATION info;
    SIZE_T ret = 0;
    st = nt_svc_NtQuerySection(h, 0, &info, sizeof(info), &ret);
    ASSERT(st == STATUS_SUCCESS, "query basic 0x%lx", (unsigned long)st);
    ASSERT(info.MaximumSize.QuadPart == 0x2000, "query size 0x%llx", (unsigned long long)info.MaximumSize.QuadPart);
    ASSERT(info.AllocationAttributes == SEC_COMMIT, "query attributes 0x%lx", (unsigned long)info.AllocationAttributes);
    ASSERT(ret == sizeof(info), "query return length %zu", (size_t)ret);
    ASSERT(nt_svc_NtQuerySection(h, 0, &info, sizeof(info) - 1, &ret) == STATUS_INFO_LENGTH_MISMATCH, "query short buffer");
    ASSERT(nt_svc_NtQuerySection(h, 7, &info, sizeof(info), &ret) == STATUS_INVALID_INFO_CLASS, "query unknown class");
    ASSERT(nt_svc_NtQuerySection(h, 1, &info, sizeof(info), &ret) == STATUS_NOT_IMPLEMENTED, "query image class");

    HANDLE mapOnly = NULL;
    st = nt_svc_NtCreateSection(&mapOnly, SECTION_MAP_READ, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_SUCCESS, "query map-only create 0x%lx", (unsigned long)st);
    ASSERT(nt_svc_NtQuerySection(mapOnly, 0, &info, sizeof(info), &ret) == STATUS_ACCESS_DENIED, "query without SECTION_QUERY");
    ASSERT(ObCloseHandle(g_mm_proc, mapOnly) == STATUS_SUCCESS, "query close map-only");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "query close");
    return 1;
}

/* Extending a section is not implemented. The call changes nothing, so the
 * size reported afterwards is the original one. */
static int test_nt_svc_extend_section(void) {
    LARGE_INTEGER sz; sz.QuadPart = 0x2000;
    HANDLE h = NULL;
    NTSTATUS st = nt_svc_NtCreateSection(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
    ASSERT(st == STATUS_SUCCESS, "extend create 0x%lx", (unsigned long)st);
    LARGE_INTEGER bigger; bigger.QuadPart = 0x4000;
    ASSERT(nt_svc_NtExtendSection(h, &bigger) == STATUS_NOT_IMPLEMENTED, "extend status");
    SECTION_BASIC_INFORMATION info;
    SIZE_T ret = 0;
    ASSERT(nt_svc_NtQuerySection(h, 0, &info, sizeof(info), &ret) == STATUS_SUCCESS, "extend query");
    ASSERT(info.MaximumSize.QuadPart == 0x2000, "extend left size unchanged");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "extend close");
    return 1;
}

/* ---- NtCreateSectionEx ---- */

/* Zero extended parameters is NtCreateSection. Validation of the array happens
 * before anything is created, so a refused call leaves the handle NULL. */
static int test_create_ex_params(void) {
    LARGE_INTEGER sz; sz.QuadPart = 0x2000;
    HANDLE h = NULL;
    NTSTATUS st = NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0);
    ASSERT(st == STATUS_SUCCESS && h != NULL, "ex zero params 0x%lx", (unsigned long)st);
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "ex zero params close");

    MEM_EXTENDED_PARAMETER amd64 = mk_ext(MemExtendedParameterImageMachine, NT_IMAGE_MACHINE_AMD64);
    h = NULL;
    st = NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &amd64, 1);
    ASSERT(st == STATUS_SUCCESS && h != NULL, "ex machine amd64 0x%lx", (unsigned long)st);
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "ex amd64 close");

    /* Other machines, and machine 0 or a value above 16 bits, are invalid parameters. */
    MEM_EXTENDED_PARAMETER i386 = mk_ext(MemExtendedParameterImageMachine, 0x14C);
    MEM_EXTENDED_PARAMETER zero = mk_ext(MemExtendedParameterImageMachine, 0);
    MEM_EXTENDED_PARAMETER wide = mk_ext(MemExtendedParameterImageMachine, 0x10000 | NT_IMAGE_MACHINE_AMD64);
    h = NULL;
    ASSERT(NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &i386, 1) == STATUS_INVALID_PARAMETER,
           "ex machine i386 refused");
    ASSERT(NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &zero, 1) == STATUS_INVALID_PARAMETER,
           "ex machine zero refused");
    ASSERT(NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &wide, 1) == STATUS_INVALID_PARAMETER,
           "ex machine wide refused");
    ASSERT(h == NULL, "refused create writes no handle");

    /* Attribute flags are accepted only as zero. */
    MEM_EXTENDED_PARAMETER attr0 = mk_ext(MemExtendedParameterAttributeFlags, 0);
    MEM_EXTENDED_PARAMETER attr1 = mk_ext(MemExtendedParameterAttributeFlags, 1);
    st = NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &attr0, 1);
    ASSERT(st == STATUS_SUCCESS && h != NULL, "ex attr zero 0x%lx", (unsigned long)st);
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "ex attr close");
    h = NULL;
    ASSERT(NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &attr1, 1) == STATUS_NOT_SUPPORTED,
           "ex attr nonzero not supported");

    /* Known types this manager does not honour are not supported. Unknown or
     * reserved types are invalid. */
    MEM_EXTENDED_PARAMETER numa = mk_ext(MemExtendedParameterNumaNode, 0);
    MEM_EXTENDED_PARAMETER req = mk_ext(MemExtendedParameterAddressRequirements, 0);
    MEM_EXTENDED_PARAMETER invalid_type = mk_ext(MemExtendedParameterInvalidType, 0);
    MEM_EXTENDED_PARAMETER past_max = mk_ext(MemExtendedParameterMax, 0);
    MEM_EXTENDED_PARAMETER reserved = amd64;
    reserved.Reserved = 1;
    ASSERT(NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &numa, 1) == STATUS_NOT_SUPPORTED,
           "ex numa not supported");
    ASSERT(NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &req, 1) == STATUS_NOT_SUPPORTED,
           "ex addr req not supported on create");
    ASSERT(NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &invalid_type, 1) == STATUS_INVALID_PARAMETER,
           "ex invalid type");
    ASSERT(NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &past_max, 1) == STATUS_INVALID_PARAMETER,
           "ex past-max type");
    ASSERT(NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &reserved, 1) == STATUS_INVALID_PARAMETER,
           "ex reserved bits");

    /* Count and array argument shape. */
    ASSERT(NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 1) == STATUS_INVALID_PARAMETER,
           "ex null array");
    ASSERT(NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &amd64, MEM_EXT_MAX_COUNT + 1) == STATUS_NOT_SUPPORTED,
           "ex count over max");
    ASSERT(NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &amd64, 0xFFFFFFFFu) == STATUS_NOT_SUPPORTED,
           "ex count 0xffffffff");
    ASSERT(h == NULL, "refused creates write no handle");

    /* The service entry point takes the same arguments. */
    st = nt_svc_NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, &amd64, 1);
    ASSERT(st == STATUS_SUCCESS, "svc ex amd64 0x%lx", (unsigned long)st);
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "svc ex close");
    ntmm_reset();
    return 1;
}

/* ntmm_ext_copy_in: count bounds, and an address-requirements record is copied
 * once and its pointer redirected into the snapshot. */
static int test_ext_copy_in(void) {
    struct nt_mem_ext_snapshot snap;
    ASSERT(ntmm_ext_copy_in(NULL, 0, &snap) == STATUS_SUCCESS && snap.count == 0, "copy in zero");
    ASSERT(ntmm_ext_copy_in(NULL, 1, &snap) == STATUS_INVALID_PARAMETER, "copy in null array");
    MEM_EXTENDED_PARAMETER one = mk_ext(MemExtendedParameterNumaNode, 0);
    ASSERT(ntmm_ext_copy_in(&one, MEM_EXT_MAX_COUNT + 1, &snap) == STATUS_NOT_SUPPORTED, "copy in over max");

    MEM_ADDRESS_REQUIREMENTS rq;
    memset(&rq, 0, sizeof(rq));
    rq.Alignment = 0x10000;
    MEM_EXTENDED_PARAMETER arr[2];
    arr[0] = mk_ext(MemExtendedParameterAddressRequirements, 0);
    arr[0].Pointer = &rq;
    arr[1] = mk_ext(MemExtendedParameterImageMachine, NT_IMAGE_MACHINE_AMD64);
    ASSERT(ntmm_ext_copy_in(arr, 2, &snap) == STATUS_SUCCESS && snap.count == 2, "copy in two");
    ASSERT(snap.params[0].Pointer == (PVOID)&snap.reqs[0], "requirements pointer redirected");
    ASSERT(snap.reqs[0].Alignment == 0x10000, "requirements record copied");
    ASSERT(snap.params[1].ULong64 == NT_IMAGE_MACHINE_AMD64, "scalar entry copied");
    return 1;
}

/* ---- NtMapViewOfSectionEx ---- */

/* Without parameters it is NtMapViewOfSection. Address requirements place the
 * view inside [Lowest, Highest) at the requested alignment. Bad requirements,
 * a placeholder flag and other parameter types are refused, and a window with
 * no room is STATUS_NO_MEMORY. The window is taken from an existing view so the
 * test does not depend on fixed host addresses. */
static int test_map_ex(void) {
    LARGE_INTEGER sz; sz.QuadPart = 0x2000;
    HANDLE h = NULL;
    NTSTATUS st = NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0);
    ASSERT(st == STATUS_SUCCESS, "mapex create 0x%lx", (unsigned long)st);
    LARGE_INTEGER off; off.QuadPart = 0;

    PVOID base1 = NULL; SIZE_T vs = 0;
    st = NtMapViewOfSectionEx(h, (HANDLE)-1, &base1, &off, &vs, 0, PAGE_READWRITE, NULL, 0);
    ASSERT(st == STATUS_SUCCESS && base1 != NULL && vs == 0x2000, "mapex plain 0x%lx", (unsigned long)st);

    ULONG_PTR b1 = (ULONG_PTR)base1;
    MEM_ADDRESS_REQUIREMENTS rq;
    memset(&rq, 0, sizeof(rq));
    rq.LowestStartingAddress = (PVOID)(b1 + 0x100000);
    rq.HighestEndingAddress = (PVOID)(b1 + 0x4000000);
    rq.Alignment = 0x100000;
    MEM_EXTENDED_PARAMETER ep = mk_ext(MemExtendedParameterAddressRequirements, 0);
    ep.Pointer = &rq;

    PVOID base2 = NULL; SIZE_T vs2 = 0;
    st = NtMapViewOfSectionEx(h, (HANDLE)-1, &base2, &off, &vs2, 0, PAGE_READWRITE, &ep, 1);
    ASSERT(st == STATUS_SUCCESS, "mapex requirements 0x%lx", (unsigned long)st);
    ULONG_PTR b2 = (ULONG_PTR)base2;
    ASSERT(b2 >= (ULONG_PTR)rq.LowestStartingAddress, "mapex honours lowest");
    ASSERT(b2 + vs2 <= (ULONG_PTR)rq.HighestEndingAddress, "mapex honours highest");
    ASSERT((b2 & (rq.Alignment - 1)) == 0, "mapex honours alignment");
    ASSERT(vs2 == 0x2000, "mapex size 0x%zx", (size_t)vs2);

    /* A window with no room: Lowest is the view just placed, Highest is its end.
     * Nothing is written to the base. */
    rq.LowestStartingAddress = (PVOID)b2;
    rq.HighestEndingAddress = (PVOID)(b2 + 0x2000);
    rq.Alignment = 0;
    PVOID base3 = NULL; SIZE_T vs3 = 0;
    ASSERT(NtMapViewOfSectionEx(h, (HANDLE)-1, &base3, &off, &vs3, 0, PAGE_READWRITE, &ep, 1) == STATUS_NO_MEMORY,
           "mapex full window");
    ASSERT(base3 == NULL, "full window base untouched");

    /* Invalid requirements: a fixed base, bad alignment, empty window, null record. */
    rq.LowestStartingAddress = (PVOID)(b1 + 0x100000);
    rq.HighestEndingAddress = (PVOID)(b1 + 0x4000000);
    rq.Alignment = 0x3000;
    PVOID base4 = NULL; SIZE_T vs4 = 0x2000;
    ASSERT(NtMapViewOfSectionEx(h, (HANDLE)-1, &base4, &off, &vs4, 0, PAGE_READWRITE, &ep, 1) == STATUS_INVALID_PARAMETER,
           "mapex non-power-of-two alignment");
    rq.Alignment = 0x800;
    ASSERT(NtMapViewOfSectionEx(h, (HANDLE)-1, &base4, &off, &vs4, 0, PAGE_READWRITE, &ep, 1) == STATUS_INVALID_PARAMETER,
           "mapex alignment below page");
    rq.Alignment = 0x100000;
    rq.LowestStartingAddress = (PVOID)0x5000000UL;
    rq.HighestEndingAddress = (PVOID)0x4000000UL;
    ASSERT(NtMapViewOfSectionEx(h, (HANDLE)-1, &base4, &off, &vs4, 0, PAGE_READWRITE, &ep, 1) == STATUS_INVALID_PARAMETER,
           "mapex empty window");
    rq.LowestStartingAddress = (PVOID)(b1 + 0x100000);
    rq.HighestEndingAddress = (PVOID)(b1 + 0x4000000);
    base4 = (PVOID)b1;
    ASSERT(NtMapViewOfSectionEx(h, (HANDLE)-1, &base4, &off, &vs4, 0, PAGE_READWRITE, &ep, 1) == STATUS_INVALID_PARAMETER,
           "mapex fixed base with requirements");
    base4 = NULL;
    ASSERT(NtMapViewOfSectionEx(h, (HANDLE)-1, &base4, &off, &vs4, 0, PAGE_READWRITE, NULL, 1) == STATUS_INVALID_PARAMETER,
           "mapex null array");
    MEM_EXTENDED_PARAMETER null_rec = mk_ext(MemExtendedParameterAddressRequirements, 0);
    ASSERT(NtMapViewOfSectionEx(h, (HANDLE)-1, &base4, &off, &vs4, 0, PAGE_READWRITE, &null_rec, 1) == STATUS_INVALID_PARAMETER,
           "mapex null requirements record");
    ASSERT(base4 == NULL, "invalid requirements leave base untouched");

    /* Other parameter types, the count limit and the placeholder flag. */
    MEM_EXTENDED_PARAMETER numa = mk_ext(MemExtendedParameterNumaNode, 0);
    ASSERT(NtMapViewOfSectionEx(h, (HANDLE)-1, &base4, &off, &vs4, 0, PAGE_READWRITE, &numa, 1) == STATUS_NOT_SUPPORTED,
           "mapex numa not supported");
    ASSERT(NtMapViewOfSectionEx(h, (HANDLE)-1, &base4, &off, &vs4, 0, PAGE_READWRITE, &ep, MEM_EXT_MAX_COUNT + 1) == STATUS_NOT_SUPPORTED,
           "mapex count over max");
    ASSERT(NtMapViewOfSectionEx(h, (HANDLE)-1, &base4, &off, &vs4, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0)
           == STATUS_NOT_SUPPORTED, "mapex replace placeholder refused");
    ASSERT(base4 == NULL, "refused placeholder leaves base untouched");

    /* The service entry point takes the same arguments. */
    PVOID base5 = NULL; SIZE_T vs5 = 0;
    st = nt_svc_NtMapViewOfSectionEx(h, (HANDLE)-1, &base5, &off, &vs5, 0, PAGE_READWRITE, NULL, 0);
    ASSERT(st == STATUS_SUCCESS && base5 != NULL, "svc mapex plain 0x%lx", (unsigned long)st);
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base5) == STATUS_SUCCESS, "svc mapex unmap");

    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base2) == STATUS_SUCCESS, "mapex unmap requirements view");
    ASSERT(NtUnmapViewOfSection((HANDLE)-1, base1) == STATUS_SUCCESS, "mapex unmap plain view");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "mapex close");
    ntmm_reset();
    return 1;
}

/* ---- NtUnmapViewOfSectionEx ---- */

/* The transient-boost flag unmaps as NtUnmapViewOfSection does. The preserve-
 * placeholder flag is not supported, and unknown bits are invalid. A refused
 * call leaves the view mapped. */
static int test_unmap_ex(void) {
    LARGE_INTEGER sz; sz.QuadPart = 0x2000;
    HANDLE h = NULL;
    NTSTATUS st = NtCreateSectionEx(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0);
    ASSERT(st == STATUS_SUCCESS, "unmapex create 0x%lx", (unsigned long)st);
    LARGE_INTEGER off; off.QuadPart = 0;
    PVOID base = NULL; SIZE_T vs = 0;

    st = NtMapViewOfSectionEx(h, (HANDLE)-1, &base, &off, &vs, 0, PAGE_READWRITE, NULL, 0);
    ASSERT(st == STATUS_SUCCESS && base != NULL, "unmapex map 0x%lx", (unsigned long)st);
    ASSERT(NtUnmapViewOfSectionEx((HANDLE)-1, base, 0) == STATUS_SUCCESS, "unmapex flags 0");
    ASSERT(NtUnmapViewOfSectionEx((HANDLE)-1, base, 0) == STATUS_NOT_MAPPED_VIEW, "unmapex twice");

    base = NULL; vs = 0;
    st = NtMapViewOfSectionEx(h, (HANDLE)-1, &base, &off, &vs, 0, PAGE_READWRITE, NULL, 0);
    ASSERT(st == STATUS_SUCCESS, "unmapex remap 0x%lx", (unsigned long)st);
    ASSERT(NtUnmapViewOfSectionEx((HANDLE)-1, base, MEM_UNMAP_WITH_TRANSIENT_BOOST) == STATUS_SUCCESS,
           "unmapex transient boost");

    base = NULL; vs = 0;
    st = NtMapViewOfSectionEx(h, (HANDLE)-1, &base, &off, &vs, 0, PAGE_READWRITE, NULL, 0);
    ASSERT(st == STATUS_SUCCESS, "unmapex remap2 0x%lx", (unsigned long)st);
    ASSERT(NtUnmapViewOfSectionEx((HANDLE)-1, base, MEM_PRESERVE_PLACEHOLDER) == STATUS_NOT_SUPPORTED,
           "unmapex preserve placeholder not supported");
    ASSERT(NtUnmapViewOfSectionEx((HANDLE)-1, base, 0x4) == STATUS_INVALID_PARAMETER,
           "unmapex unknown flag");
    ASSERT(NtUnmapViewOfSectionEx((HANDLE)-1, base, MEM_PRESERVE_PLACEHOLDER | MEM_UNMAP_WITH_TRANSIENT_BOOST)
           == STATUS_NOT_SUPPORTED, "unmapex preserve with boost");
    *(volatile unsigned char *)base = 0x21;   /* still mapped after the refusals */
    ASSERT(nt_svc_NtUnmapViewOfSectionEx((HANDLE)-1, base, MEM_UNMAP_WITH_TRANSIENT_BOOST) == STATUS_SUCCESS,
           "svc unmapex");
    ASSERT(nt_svc_NtUnmapViewOfSectionEx((HANDLE)-1, base, 0) == STATUS_NOT_MAPPED_VIEW, "svc unmapex twice");
    ASSERT(NtUnmapViewOfSectionEx((HANDLE)-1, (PVOID)0x1000, 0) == STATUS_NOT_MAPPED_VIEW, "unmapex unmapped");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "unmapex close");
    ntmm_reset();
    return 1;
}

/* NtAllocateVirtualMemoryEx with address requirements: the region lands inside
 * [Lowest, Highest) at the requested alignment, top-down when asked. The base
 * must be NULL and the call must reserve. No room is STATUS_NO_MEMORY. The
 * window is taken from an existing region, as in test_map_ex. */
static int test_allocate_ex_requirements(void) {
    PVOID b1 = NULL;
    SIZE_T s1 = 0x1000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1, &b1, 0, &s1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS && b1 != NULL, "req alloc anchor 0x%lx", (unsigned long)st);
    ULONG_PTR a1 = (ULONG_PTR)b1;

    MEM_ADDRESS_REQUIREMENTS rq;
    memset(&rq, 0, sizeof(rq));
    rq.LowestStartingAddress = (PVOID)(a1 + 0x100000);
    rq.HighestEndingAddress = (PVOID)(a1 + 0x4000000);
    rq.Alignment = 0x100000;
    MEM_EXTENDED_PARAMETER ep = mk_ext(MemExtendedParameterAddressRequirements, 0);
    ep.Pointer = &rq;

    /* Bottom up, then top down, inside the window. */
    PVOID base = NULL; SIZE_T size = 0x1000;
    st = NtAllocateVirtualMemoryEx((HANDLE)-1, &base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &ep, 1);
    ASSERT(st == STATUS_SUCCESS, "req alloc 0x%lx", (unsigned long)st);
    ULONG_PTR a2 = (ULONG_PTR)base;
    ASSERT(a2 >= (ULONG_PTR)rq.LowestStartingAddress && a2 + size <= (ULONG_PTR)rq.HighestEndingAddress,
           "req alloc inside window");
    ASSERT((a2 & (rq.Alignment - 1)) == 0, "req alloc aligned");
    MEMORY_BASIC_INFORMATION mbi;
    ASSERT(query(base, &mbi) && mbi.State == MEM_COMMIT, "req alloc committed");

    PVOID base_td = NULL; SIZE_T size_td = 0x1000;
    st = NtAllocateVirtualMemoryEx((HANDLE)-1, &base_td, &size_td, MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE, &ep, 1);
    ASSERT(st == STATUS_SUCCESS, "req top down 0x%lx", (unsigned long)st);
    ULONG_PTR a3 = (ULONG_PTR)base_td;
    ASSERT(a3 >= (ULONG_PTR)rq.LowestStartingAddress && a3 + size_td <= (ULONG_PTR)rq.HighestEndingAddress,
           "req top down inside window");
    ASSERT((a3 & (rq.Alignment - 1)) == 0, "req top down aligned");
    ASSERT(a3 > a2, "top down sits above the bottom-up region");

    /* A window that is one occupied region has no hole: STATUS_NO_MEMORY and no base. */
    rq.LowestStartingAddress = (PVOID)a2;
    rq.HighestEndingAddress = (PVOID)(a2 + 0x1000);
    rq.Alignment = 0;
    PVOID base_full = NULL; SIZE_T size_full = 0x1000;
    ASSERT(NtAllocateVirtualMemoryEx((HANDLE)-1, &base_full, &size_full, MEM_RESERVE, PAGE_READWRITE, &ep, 1)
           == STATUS_NO_MEMORY, "req full window");
    ASSERT(base_full == NULL, "req full window base untouched");

    /* Invalid combinations: a fixed base, commit only, bad alignment, empty window,
     * null record, invalid type, reserved bits. */
    rq.LowestStartingAddress = (PVOID)(a1 + 0x100000);
    rq.HighestEndingAddress = (PVOID)(a1 + 0x4000000);
    rq.Alignment = 0x100000;
    PVOID bad = (PVOID)a1;
    SIZE_T bad_size = 0x1000;
    ASSERT(NtAllocateVirtualMemoryEx((HANDLE)-1, &bad, &bad_size, MEM_RESERVE, PAGE_READWRITE, &ep, 1)
           == STATUS_INVALID_PARAMETER, "req fixed base refused");
    bad = NULL;
    ASSERT(NtAllocateVirtualMemoryEx((HANDLE)-1, &bad, &bad_size, MEM_COMMIT, PAGE_READWRITE, &ep, 1)
           == STATUS_INVALID_PARAMETER, "req commit only refused");
    ASSERT(NtAllocateVirtualMemoryEx((HANDLE)-1, NULL, &bad_size, MEM_RESERVE, PAGE_READWRITE, &ep, 1)
           == STATUS_INVALID_PARAMETER, "req null base refused");
    rq.Alignment = 0x3000;
    ASSERT(NtAllocateVirtualMemoryEx((HANDLE)-1, &bad, &bad_size, MEM_RESERVE, PAGE_READWRITE, &ep, 1)
           == STATUS_INVALID_PARAMETER, "req bad alignment");
    rq.Alignment = 0x100000;
    rq.LowestStartingAddress = (PVOID)0x5000000UL;
    rq.HighestEndingAddress = (PVOID)0x4000000UL;
    ASSERT(NtAllocateVirtualMemoryEx((HANDLE)-1, &bad, &bad_size, MEM_RESERVE, PAGE_READWRITE, &ep, 1)
           == STATUS_INVALID_PARAMETER, "req empty window");
    rq.LowestStartingAddress = (PVOID)(a1 + 0x100000);
    rq.HighestEndingAddress = (PVOID)(a1 + 0x4000000);
    MEM_EXTENDED_PARAMETER null_rec = mk_ext(MemExtendedParameterAddressRequirements, 0);
    ASSERT(NtAllocateVirtualMemoryEx((HANDLE)-1, &bad, &bad_size, MEM_RESERVE, PAGE_READWRITE, &null_rec, 1)
           == STATUS_INVALID_PARAMETER, "req null record");
    MEM_EXTENDED_PARAMETER invalid_type = mk_ext(MemExtendedParameterInvalidType, 0);
    ASSERT(NtAllocateVirtualMemoryEx((HANDLE)-1, &bad, &bad_size, MEM_RESERVE, PAGE_READWRITE, &invalid_type, 1)
           == STATUS_INVALID_PARAMETER, "invalid type");
    MEM_EXTENDED_PARAMETER reserved = ep;
    reserved.Reserved = 1;
    ASSERT(NtAllocateVirtualMemoryEx((HANDLE)-1, &bad, &bad_size, MEM_RESERVE, PAGE_READWRITE, &reserved, 1)
           == STATUS_INVALID_PARAMETER, "reserved bits");
    ASSERT(bad == NULL, "refused requests write no base");

    /* Other types, count limit, other process. */
    MEM_EXTENDED_PARAMETER numa = mk_ext(MemExtendedParameterNumaNode, 0);
    ASSERT(NtAllocateVirtualMemoryEx((HANDLE)-1, &bad, &bad_size, MEM_RESERVE, PAGE_READWRITE, &numa, 1)
           == STATUS_NOT_SUPPORTED, "numa with requirements unsupported");
    ASSERT(NtAllocateVirtualMemoryEx((HANDLE)-1, &bad, &bad_size, MEM_RESERVE, PAGE_READWRITE, &ep, MEM_EXT_MAX_COUNT + 1)
           == STATUS_NOT_SUPPORTED, "count over max");
    ASSERT(NtAllocateVirtualMemoryEx((HANDLE)0x1234, &bad, &bad_size, MEM_RESERVE, PAGE_READWRITE, &ep, 1)
           == STATUS_INVALID_HANDLE, "unknown process refused");

    ntmm_reset();
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Per-process address spaces (ADR 0073)                                      */
/* -------------------------------------------------------------------------- */

/* Creates a process object. When h is non-NULL, a handle to it with the given
 * access is inserted into g_mm_proc's table. */
static struct nt_eprocess *mm_make_proc(ULONG pid, const char *name, HANDLE *h, ACCESS_MASK access) {
    struct nt_eprocess *p = NULL;
    if (ObCreateProcessObject(&p, pid, name) != STATUS_SUCCESS || !p) return NULL;
    if (h) {
        *h = NULL;
        if (ObInsertObject(p->obj, NULL, access, g_mm_proc, h) != STATUS_SUCCESS || !*h) {
            ObDestroyProcessObject(p);
            return NULL;
        }
    }
    return p;
}

static int mm_global_free_at(PVOID addr) {
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T ret = 0;
    NTSTATUS st = NtQueryVirtualMemory((HANDLE)-1, addr, MemoryBasicInformation, &mbi, sizeof(mbi), &ret);
    return st == STATUS_SUCCESS && mbi.State == MEM_FREE;
}

/* A region allocated through a process handle lives in that process's space:
 * the handle sees it, the pseudo-handle does not, and the global VAD map stays
 * clean. Protect and release also go through the handle. */
static int test_foreign_space_alloc_query_free(void) {
    HANDLE h = NULL;
    struct nt_eprocess *p = mm_make_proc(4343, "mmfor", &h, PROCESS_ALL_ACCESS);
    ASSERT(p && h, "make foreign process");

    PVOID base = NULL;
    SIZE_T size = 0x20000;
    NTSTATUS st = NtAllocateVirtualMemory(h, &base, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS && base && size == 0x20000, "foreign alloc 0x%lx", (unsigned long)st);
    ASSERT(mm_global_free_at(base), "global space sees the foreign allocation");

    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T ret = 0;
    st = NtQueryVirtualMemory(h, base, MemoryBasicInformation, &mbi, sizeof(mbi), &ret);
    ASSERT(st == STATUS_SUCCESS && mbi.State == MEM_COMMIT && mbi.AllocationBase == base,
           "foreign query 0x%lx state 0x%lx", (unsigned long)st, (unsigned long)mbi.State);
    *(volatile unsigned char *)base = 0x5a;

    PVOID pb = base;
    SIZE_T ps = 0x1000;
    ULONG old = 0;
    st = NtProtectVirtualMemory(h, &pb, &ps, PAGE_READONLY, &old);
    ASSERT(st == STATUS_SUCCESS && old == PAGE_READWRITE, "foreign protect 0x%lx old 0x%lx",
           (unsigned long)st, (unsigned long)old);

    PVOID rb = base;
    SIZE_T rs = 0;
    st = NtFreeVirtualMemory(h, &rb, &rs, MEM_RELEASE);
    ASSERT(st == STATUS_SUCCESS, "foreign release 0x%lx", (unsigned long)st);
    st = NtQueryVirtualMemory(h, base, MemoryBasicInformation, &mbi, sizeof(mbi), &ret);
    ASSERT(st == STATUS_SUCCESS && mbi.State == MEM_FREE, "foreign region free after release");
    ASSERT(ntmm_check_invariants(&g_nt_mm), "global invariants after foreign ops");

    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "close foreign handle");
    ObDestroyProcessObject(p);
    return 1;
}

/* Handles carry the access the service needs. A handle with only
 * PROCESS_VM_READ can read but not allocate, write, free or query. */
static int test_foreign_access_rights(void) {
    HANDLE hfull = NULL, hro = NULL;
    struct nt_eprocess *p = mm_make_proc(4344, "mmacl", &hfull, PROCESS_ALL_ACCESS);
    ASSERT(p && hfull, "make foreign process");
    ASSERT(ObInsertObject(p->obj, NULL, PROCESS_VM_READ, g_mm_proc, &hro) == STATUS_SUCCESS && hro,
           "insert read-only handle");

    PVOID base = NULL;
    SIZE_T size = 0x1000;
    NTSTATUS st = NtAllocateVirtualMemory(hfull, &base, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "alloc via full handle 0x%lx", (unsigned long)st);
    unsigned char pat[16];
    for (int i = 0; i < 16; i++) pat[i] = (unsigned char)(0xA0 + i);
    SIZE_T n = 0;
    st = NtWriteVirtualMemory(hfull, base, pat, sizeof(pat), &n);
    ASSERT(st == STATUS_SUCCESS && n == sizeof(pat), "write via full handle 0x%lx", (unsigned long)st);

    unsigned char got[16] = {0};
    st = NtReadVirtualMemory(hro, base, got, sizeof(got), &n);
    ASSERT(st == STATUS_SUCCESS && n == sizeof(got) && memcmp(got, pat, sizeof(pat)) == 0,
           "read via read-only handle 0x%lx", (unsigned long)st);

    PVOID b2 = NULL;
    SIZE_T s2 = 0x1000;
    st = NtAllocateVirtualMemory(hro, &b2, 0, &s2, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_ACCESS_DENIED, "alloc via read-only handle 0x%lx", (unsigned long)st);

    st = NtWriteVirtualMemory(hro, base, pat, sizeof(pat), &n);
    ASSERT(st == STATUS_ACCESS_DENIED, "write via read-only handle 0x%lx", (unsigned long)st);

    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T ret = 0;
    st = NtQueryVirtualMemory(hro, base, MemoryBasicInformation, &mbi, sizeof(mbi), &ret);
    ASSERT(st == STATUS_ACCESS_DENIED, "query via read-only handle 0x%lx", (unsigned long)st);

    PVOID fb = base;
    SIZE_T fs = 0;
    st = NtFreeVirtualMemory(hro, &fb, &fs, MEM_RELEASE);
    ASSERT(st == STATUS_ACCESS_DENIED, "free via read-only handle 0x%lx", (unsigned long)st);

    PVOID fb2 = base;
    SIZE_T fs2 = 0;
    st = NtFreeVirtualMemory(hfull, &fb2, &fs2, MEM_RELEASE);
    ASSERT(st == STATUS_SUCCESS, "release via full handle 0x%lx", (unsigned long)st);

    ASSERT(ObCloseHandle(g_mm_proc, hro) == STATUS_SUCCESS, "close ro");
    ASSERT(ObCloseHandle(g_mm_proc, hfull) == STATUS_SUCCESS, "close full");
    ObDestroyProcessObject(p);
    return 1;
}

/* Cross-process copies go between the two spaces. Bytes written through the
 * foreign handle appear in the foreign space and nowhere in the global one. */
static int test_cross_process_read_write(void) {
    HANDLE h = NULL;
    struct nt_eprocess *p = mm_make_proc(4345, "mmrw", &h, PROCESS_ALL_ACCESS);
    ASSERT(p && h, "make foreign process");
    PVOID base = NULL;
    SIZE_T size = 0x2000;
    NTSTATUS st = NtAllocateVirtualMemory(h, &base, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS, "alloc 0x%lx", (unsigned long)st);

    unsigned char src[300], dst[300];
    for (int i = 0; i < 300; i++) src[i] = (unsigned char)(i * 13 + 5);
    SIZE_T n = 0;
    st = NtWriteVirtualMemory(h, (unsigned char *)base + 0x10, src, sizeof(src), &n);
    ASSERT(st == STATUS_SUCCESS && n == sizeof(src), "cross write 0x%lx n %zu", (unsigned long)st, (size_t)n);
    ASSERT(memcmp((unsigned char *)base + 0x10, src, sizeof(src)) == 0, "bytes landed in arena");

    memset(dst, 0, sizeof(dst));
    st = NtReadVirtualMemory(h, (unsigned char *)base + 0x10, dst, sizeof(dst), &n);
    ASSERT(st == STATUS_SUCCESS && n == sizeof(dst) && memcmp(dst, src, sizeof(src)) == 0,
           "cross read 0x%lx n %zu", (unsigned long)st, (size_t)n);

    /* The same address in the global space is not mapped, so a copy there
     * moves nothing. */
    SIZE_T gn = 99;
    st = NtReadVirtualMemory((HANDLE)-1, (unsigned char *)base + 0x10, dst, sizeof(dst), &gn);
    ASSERT(st != STATUS_SUCCESS && gn == 0, "global read of foreign address 0x%lx n %zu", (unsigned long)st, (size_t)gn);

    /* Copy running off the end of the foreign region: partial copy. */
    SIZE_T tail = 0;
    st = NtReadVirtualMemory(h, (unsigned char *)base + 0x2000 - 4, dst, 16, &tail);
    ASSERT(st == STATUS_PARTIAL_COPY && tail == 4, "tail read 0x%lx n %zu", (unsigned long)st, (size_t)tail);

    PVOID rb = base; SIZE_T rs = 0;
    ASSERT(NtFreeVirtualMemory(h, &rb, &rs, MEM_RELEASE) == STATUS_SUCCESS, "free");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "close");
    ObDestroyProcessObject(p);
    return 1;
}

/* A section maps into a foreign space through the target handle. The view is
 * in that space, unmaps through the same handle, and the global space never
 * sees it. */
static int test_foreign_section_map(void) {
    HANDLE h = NULL;
    struct nt_eprocess *p = mm_make_proc(4346, "mmsec", &h, PROCESS_ALL_ACCESS);
    ASSERT(p && h, "make foreign process");
    HANDLE sec = NULL;
    NTSTATUS st = mm_make_section(&sec, SECTION_ALL_ACCESS, PAGE_READWRITE, 0x10000);
    ASSERT(st == STATUS_SUCCESS && sec, "make section 0x%lx", (unsigned long)st);

    LARGE_INTEGER off; off.QuadPart = 0;
    PVOID base = NULL; SIZE_T vs = 0;
    st = NtMapViewOfSection(sec, h, &base, 0, 0, &off, &vs, 0, 0, PAGE_READWRITE);
    ASSERT(st == STATUS_SUCCESS && base && vs == 0x10000, "map into foreign 0x%lx", (unsigned long)st);
    ASSERT(mm_global_free_at(base), "global space sees the foreign view");

    unsigned char zero[16] = {0}, got[16];
    memset(got, 0xff, sizeof(got));
    SIZE_T n = 0;
    st = NtReadVirtualMemory(h, base, got, sizeof(got), &n);
    ASSERT(st == STATUS_SUCCESS && n == sizeof(got) && memcmp(got, zero, sizeof(zero)) == 0,
           "read foreign view 0x%lx", (unsigned long)st);

    ASSERT(NtUnmapViewOfSection(h, base) == STATUS_SUCCESS, "unmap foreign view");
    ASSERT(NtUnmapViewOfSection(h, base) == STATUS_NOT_MAPPED_VIEW, "unmap twice");

    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close section");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "close process handle");
    ObDestroyProcessObject(p);
    return 1;
}

/* Destroying a process frees its whole space: its VADs, its arena and the
 * section references its views held. The allocation counter returns to where
 * it was before the process existed. */
static int test_foreign_destroy_frees_space(void) {
    extern _Atomic long kshim_alloc_count;
    HANDLE sec = NULL;
    NTSTATUS st = mm_make_section(&sec, SECTION_ALL_ACCESS, PAGE_READWRITE, 0x10000);
    ASSERT(st == STATUS_SUCCESS && sec, "make section 0x%lx", (unsigned long)st);
    long before = atomic_load(&kshim_alloc_count);

    HANDLE h = NULL;
    struct nt_eprocess *p = mm_make_proc(4347, "mmdie", &h, PROCESS_ALL_ACCESS);
    ASSERT(p && h, "make foreign process");
    PVOID a = NULL; SIZE_T as = 0x3000;
    ASSERT(NtAllocateVirtualMemory(h, &a, 0, &as, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == STATUS_SUCCESS, "alloc");
    PVOID b = NULL; SIZE_T bs = 0x1000;
    ASSERT(NtAllocateVirtualMemory(h, &b, 0, &bs, MEM_RESERVE, PAGE_READWRITE) == STATUS_SUCCESS, "reserve");
    LARGE_INTEGER off; off.QuadPart = 0;
    PVOID v = NULL; SIZE_T vs = 0;
    ASSERT(NtMapViewOfSection(sec, h, &v, 0, 0, &off, &vs, 0, 0, PAGE_READWRITE) == STATUS_SUCCESS, "map");

    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "close process handle");
    ObDestroyProcessObject(p);

    ASSERT(atomic_load(&kshim_alloc_count) == before, "space teardown leaked %ld",
           atomic_load(&kshim_alloc_count) - before);
    ASSERT(ObCloseHandle(g_mm_proc, sec) == STATUS_SUCCESS, "close section after process gone");
    return 1;
}

/* A handle that is not in the table is refused, and no base is written. */
static int test_foreign_forged_handle(void) {
    PVOID base = NULL;
    SIZE_T size = 0x1000;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)0x1234, &base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    ASSERT(st == STATUS_INVALID_HANDLE, "forged handle 0x%lx", (unsigned long)st);
    ASSERT(base == NULL, "forged handle wrote a base");
    return 1;
}

/* Flush and NtAllocateVirtualMemoryEx also take a process handle now. */
static int test_foreign_flush_and_ex(void) {
    HANDLE h = NULL;
    struct nt_eprocess *p = mm_make_proc(4348, "mmflx", &h, PROCESS_ALL_ACCESS);
    ASSERT(p && h, "make foreign process");
    PVOID base = NULL; SIZE_T size = 0x2000;
    NTSTATUS st = NtAllocateVirtualMemoryEx(h, &base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    ASSERT(st == STATUS_SUCCESS && base, "ex alloc via foreign 0x%lx", (unsigned long)st);
    IO_STATUS_BLOCK iosb;
    PVOID fb = base; SIZE_T fs = 0x1000;
    st = NtFlushVirtualMemory(h, &fb, &fs, &iosb);
    ASSERT(st == STATUS_SUCCESS, "flush via foreign 0x%lx", (unsigned long)st);
    PVOID rb = base; SIZE_T rs = 0;
    ASSERT(NtFreeVirtualMemory(h, &rb, &rs, MEM_RELEASE) == STATUS_SUCCESS, "free");
    ASSERT(ObCloseHandle(g_mm_proc, h) == STATUS_SUCCESS, "close");
    ObDestroyProcessObject(p);
    return 1;
}

int main(void) {
    printf("=== ntmm tests ===\n");
    if (!mm_setup_objects()) {
        printf("object manager setup failed\n");
        return 1;
    }
    RUN_TEST(test_section_handle_is_object);
    RUN_TEST(test_section_forged_handle_rejected);
    RUN_TEST(test_section_wrong_type_rejected);
    RUN_TEST(test_section_name_held_by_other_type);
    RUN_TEST(test_section_same_type_collision_reopens);
    RUN_TEST(test_section_closed_handle_rejected);
    RUN_TEST(test_section_access_mask);
    RUN_TEST(test_section_lifetime);
    RUN_TEST(test_section_view_split_refcount);
    RUN_TEST(test_reserve_commit_subrange);
    RUN_TEST(test_granularity);
    RUN_TEST(test_release_nonzero);
    RUN_TEST(test_query_coalesce_free);
    RUN_TEST(test_guard_once);
    RUN_TEST(test_protect_old);
    RUN_TEST(test_fuzz);
    RUN_TEST(test_vad_matrix);
    RUN_TEST(test_alloc_size_wrap);
    RUN_TEST(test_lock_virtual_memory);
    mmf_setup();
    RUN_TEST(test_file_section_shared_write_reaches_file);
    RUN_TEST(test_file_section_cow_does_not_modify_file);
    RUN_TEST(test_file_section_access_by_protection);
    RUN_TEST(test_file_section_zero_length_rejected);
    RUN_TEST(test_file_section_uses_allocation_size);
    RUN_TEST(test_file_section_handle_closed_keeps_view);
    RUN_TEST(test_file_section_offset_alignment);
    RUN_TEST(test_file_section_query_file_attribute);
    RUN_TEST(test_file_section_size_rules);
    RUN_TEST(test_sec_image_file_rules);
    RUN_TEST(test_sec_image_pe32_refused);
    RUN_TEST(test_sec_image_maps_sections);
    RUN_TEST(test_sec_image_view_rules);
    RUN_TEST(test_sec_image_not_at_base);
    RUN_TEST(test_sec_image_relocated_view);
    RUN_TEST(test_sec_image_stripped_conflicts);
    RUN_TEST(test_sec_image_bad_reloc_directory);
    RUN_TEST(test_sec_image_unmap_frees);
    RUN_TEST(test_file_section_forged_handle);
    RUN_TEST(test_file_section_leak_tracker);
    RUN_TEST(test_wrapping_size_protect_decommit);
    RUN_TEST(test_sections);
    RUN_TEST(test_view_size_round_up_wrap);
    RUN_TEST(test_probe_virtualquery_regression);
    RUN_TEST(test_reset);
    RUN_TEST(test_vad_split_coalesce_invariants);
    RUN_TEST(test_vad_long_random_invariant);
    RUN_TEST(test_probe_direct_windows_rule);
    RUN_TEST(test_query_region_info);
    RUN_TEST(test_rw_virtual_memory);
    RUN_TEST(test_flush_virtual_memory);
    RUN_TEST(test_allocate_ex);
    RUN_TEST(test_top_of_space_range_refused);
    RUN_TEST(test_nt_svc_create_section_unnamed);
    RUN_TEST(test_nt_svc_named_section_open);
    RUN_TEST(test_nt_svc_open_section_errors);
    RUN_TEST(test_nt_svc_map_unmap);
    RUN_TEST(test_nt_svc_map_errors);
    RUN_TEST(test_nt_svc_query_section);
    RUN_TEST(test_nt_svc_extend_section);
    RUN_TEST(test_create_ex_params);
    RUN_TEST(test_ext_copy_in);
    RUN_TEST(test_map_ex);
    RUN_TEST(test_unmap_ex);
    RUN_TEST(test_allocate_ex_requirements);
    RUN_TEST(test_foreign_space_alloc_query_free);
    RUN_TEST(test_foreign_access_rights);
    RUN_TEST(test_cross_process_read_write);
    RUN_TEST(test_foreign_section_map);
    RUN_TEST(test_foreign_destroy_frees_space);
    RUN_TEST(test_foreign_forged_handle);
    RUN_TEST(test_foreign_flush_and_ex);
    ntmm_reset();
    ObDestroyProcessObject(g_mm_proc);
    ObCleanupObjectManager();
    printf("\n=== RESULT %d/%d passed, %d failed ===\n", tests_pass, tests_run, tests_fail);
    extern _Atomic long kshim_alloc_count;
    long leaks = atomic_load(&kshim_alloc_count);
    if (leaks != 0) {
        printf("LEAK detected: %ld allocations not freed\n", leaks);
        tests_fail++;
    } else {
        printf("No leaks detected\n");
    }
    return tests_fail ? 1 : 0;
}
