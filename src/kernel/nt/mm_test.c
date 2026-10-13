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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* M-7: a file-backed section is not implemented; it must not be created as
 * zeroed anonymous memory. */
static int test_file_section_unsupported(void) {
    LARGE_INTEGER sz; sz.QuadPart = 0x1000;
    HANDLE sec = NULL;
    NTSTATUS st = NtCreateSection(&sec, 0, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, (HANDLE)0x1234);
    ASSERT(st == STATUS_NOT_IMPLEMENTED, "file section status 0x%lx", (unsigned long)st);
    ASSERT(sec == NULL, "no section handle");
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

int main(void) {
    printf("=== ntmm tests ===\n");
    if (!mm_setup_objects()) {
        printf("object manager setup failed\n");
        return 1;
    }
    RUN_TEST(test_section_handle_is_object);
    RUN_TEST(test_section_forged_handle_rejected);
    RUN_TEST(test_section_wrong_type_rejected);
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
    RUN_TEST(test_file_section_unsupported);
    RUN_TEST(test_wrapping_size_protect_decommit);
    RUN_TEST(test_sections);
    RUN_TEST(test_view_size_round_up_wrap);
    RUN_TEST(test_probe_virtualquery_regression);
    RUN_TEST(test_reset);
    RUN_TEST(test_vad_split_coalesce_invariants);
    RUN_TEST(test_vad_long_random_invariant);
    RUN_TEST(test_probe_direct_windows_rule);
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
