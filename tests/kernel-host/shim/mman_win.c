/* mman_win.c - POSIX mmap family on top of the Win32 virtual memory API.
 *
 * Host-only support for tests/kernel-host. Anonymous private mappings are
 * backed by VirtualAlloc. MAP_FIXED and MAP_FIXED_NOREPLACE are emulated
 * with VirtualQuery. munmap decommits the range (the address space stays
 * reserved, which is acceptable for short-lived unit-test processes).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

static DWORD prot_to_win(int prot)
{
    switch (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) {
    case PROT_NONE:                          return PAGE_NOACCESS;
    case PROT_READ:                          return PAGE_READONLY;
    case PROT_READ | PROT_WRITE:             return PAGE_READWRITE;
    case PROT_EXEC:                          return PAGE_EXECUTE;
    case PROT_READ | PROT_EXEC:              return PAGE_EXECUTE_READ;
    case PROT_READ | PROT_WRITE | PROT_EXEC: return PAGE_EXECUTE_READWRITE;
    case PROT_WRITE | PROT_EXEC:             return PAGE_EXECUTE_READWRITE;
    default:                                 return PAGE_READWRITE;
    }
}

/* Fixed mapping of [addr, addr+len), one VirtualQuery region at a time.
 *
 * Linux semantics: after munmap, the range is unmapped and a later
 * MAP_FIXED_NOREPLACE may use it. munmap decommits partial ranges, and Windows
 * keeps a decommitted part of a reservation in MEM_RESERVE state. So in this
 * shim MEM_RESERVE means "unmapped" and MEM_COMMIT means "mapped". Holes are
 * committed in place. MEM_FREE pieces are reserved and committed fresh. The
 * range may mix all three. Returns 0 on success, -1 with errno set on failure.
 * Pieces committed before a failure are left in place, as with a partial
 * Linux mapping failure. */
static int map_fixed_range(uintptr_t start, size_t len, int prot, int noreplace)
{
    DWORD wprot = prot_to_win(prot);
    uintptr_t p = start, end = start + len;
    MEMORY_BASIC_INFORMATION mbi;

    if (noreplace) {
        while (p < end) {
            if (VirtualQuery((void *)p, &mbi, sizeof(mbi)) == 0) {
                errno = EINVAL;
                return -1;
            }
            if (mbi.State == MEM_COMMIT) {
                errno = EEXIST;
                return -1;
            }
            p = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        }
    }
    p = start;
    while (p < end) {
        uintptr_t seg_end;
        void *r;
        if (VirtualQuery((void *)p, &mbi, sizeof(mbi)) == 0) {
            errno = EINVAL;
            return -1;
        }
        seg_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (seg_end > end)
            seg_end = end;
        if (mbi.State == MEM_FREE)
            r = VirtualAlloc((void *)p, seg_end - p, MEM_RESERVE | MEM_COMMIT, wprot);
        else
            r = VirtualAlloc((void *)p, seg_end - p, MEM_COMMIT, wprot);
        if (!r) {
            errno = ENOMEM;
            return -1;
        }
        p = seg_end;
    }
    return 0;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    void *p;
    (void)off;
    if (len == 0 || fd != -1 || !(flags & MAP_ANONYMOUS)) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if (addr && (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))) {
        if (map_fixed_range((uintptr_t)addr, len, prot,
                            (flags & MAP_FIXED_NOREPLACE) != 0) != 0)
            return MAP_FAILED;
        p = addr;
    } else {
        p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, prot_to_win(prot));
    }
    if (!p) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return p;
}

/* Size of the whole allocation that starts at base (sum of its regions). */
static size_t allocation_size(uintptr_t base)
{
    uintptr_t p = base;
    size_t total = 0;
    for (;;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void *)p, &mbi, sizeof(mbi)) == 0)
            break;
        if ((uintptr_t)mbi.AllocationBase != base)
            break;
        total += mbi.RegionSize;
        p = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return total;
}

/* A range that covers a whole allocation is released, so its address space
 * is free again for MAP_FIXED_NOREPLACE. A partial range is decommitted only;
 * Windows cannot release part of a reservation, so that part stays reserved. */
int munmap(void *addr, size_t len)
{
    uintptr_t p, end;
    if (!addr || len == 0) {
        errno = EINVAL;
        return -1;
    }
    p = (uintptr_t)addr;
    end = p + len;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t rbase, rend, abase;
        size_t asize;
        if (VirtualQuery((void *)p, &mbi, sizeof(mbi)) == 0) {
            errno = EINVAL;
            return -1;
        }
        rbase = (uintptr_t)mbi.BaseAddress;
        rend = rbase + mbi.RegionSize;
        abase = (uintptr_t)mbi.AllocationBase;
        if (mbi.State == MEM_FREE) {
            p = rend;
            continue;
        }
        asize = allocation_size(abase);
        if (p <= abase && end >= abase + asize) {
            VirtualFree((void *)abase, 0, MEM_RELEASE);
        } else {
            uintptr_t from = p > rbase ? p : rbase;
            uintptr_t to = end < rend ? end : rend;
            VirtualFree((void *)from, to - from, MEM_DECOMMIT);
        }
        p = rend;
    }
    return 0;
}

int mprotect(void *addr, size_t len, int prot)
{
    DWORD old;
    if (!VirtualProtect(addr, len, prot_to_win(prot), &old)) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

/* VirtualLock keeps the pages in the working set; failure (typically the
 * working-set quota) is ENOMEM. */
int mlock(const void *addr, size_t len)
{
    if (!VirtualLock((LPVOID)addr, len)) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int munlock(const void *addr, size_t len)
{
    if (!VirtualUnlock((LPVOID)addr, len)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int madvise(void *addr, size_t len, int advice)
{
    (void)addr; (void)len;
    /* MADV_DONTNEED / MADV_FREE are hints; treating them as no-ops is safe
     * for the unit tests, which do not depend on zero-fill-on-reuse. */
    if (advice != MADV_NORMAL && advice != MADV_DONTNEED && advice != MADV_FREE) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
