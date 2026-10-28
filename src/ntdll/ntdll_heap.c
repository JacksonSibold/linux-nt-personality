/*
 * ntdll_heap.c - the Rtl heap API: RtlCreateHeap, RtlDestroyHeap, RtlAllocateHeap, RtlFreeHeap,
 * RtlReAllocateHeap, RtlSizeHeap, RtlValidateHeap, RtlCompactHeap, RtlLockHeap, RtlUnlockHeap,
 * RtlWalkHeap, RtlSetHeapInformation, RtlQueryHeapInformation and RtlGetProcessHeaps.
 *
 * The allocator is rtl_heap_pure.h (host tests: tests/ntdll-host/test_rtl_heap_pure.c). Segments come
 * from NtAllocateVirtualMemory, one reserve and commit per segment, and go back with NtFreeVirtualMemory.
 *
 * The process heap is created on first use (or by RtlGetProcessHeaps) and recorded in PEB.ProcessHeap. A
 * NULL heap handle means the process heap, so malloc in ntdll_runtime.c does not need to find it. The
 * heap handle is the address of the heap structure; kernel32 HeapWalk relies on that.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "ntstatus.h"
#include "nt_syscalls.h"
#include "rtl_heap_pure.h"
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME

#define RTLH_HEAP_GENERATE_EXCEPTIONS 0x00000004u
#define RTLH_MEM_COMMIT_RESERVE 0x3000u
#define RTLH_MEM_RELEASE 0x8000u
#define RTLH_PAGE_READWRITE 0x04u
#define RTLH_MAX_HEAPS 64u

#ifndef STATUS_NO_MORE_ENTRIES
#define STATUS_NO_MORE_ENTRIES ((NTSTATUS)0x8000001A)
#endif

NTSTATUS __stdcall RtlRaiseException(PEXCEPTION_RECORD rec);

static void *seg_alloc(size_t size)
{
    PVOID base = NULL;
    SIZE_T sz = size;
    if (!NT_SUCCESS(NtAllocateVirtualMemory((HANDLE)(intptr_t)-1, &base, 0, &sz, RTLH_MEM_COMMIT_RESERVE,
                                            RTLH_PAGE_READWRITE)))
        return NULL;
    return base;
}

static void seg_free(void *base, size_t size)
{
    PVOID b = base;
    SIZE_T sz = 0; /* MEM_RELEASE takes size 0: the whole reservation */
    (void)size;
    NtFreeVirtualMemory((HANDLE)(intptr_t)-1, &b, &sz, RTLH_MEM_RELEASE);
}

static void heap_lock(volatile long *w)
{
    unsigned spins = 0;
    while (__sync_lock_test_and_set(w, 1)) {
        if (++spins > 64) {
            NtYieldExecution();
            spins = 0;
        }
    }
}

static void heap_unlock(volatile long *w)
{
    __sync_lock_release(w);
}

static const rh_os g_heap_os = {seg_alloc, seg_free, heap_lock, heap_unlock};

static rh_heap *g_heaps[RTLH_MAX_HEAPS]; /* [0] is the process heap once it exists */
static volatile long g_heaps_lock;

static PEB *current_peb(void)
{
    uint64_t peb;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
    return (PEB *)(uintptr_t)peb;
}

static int register_heap(rh_heap *h)
{
    unsigned i;
    int ok = 0;
    heap_lock(&g_heaps_lock);
    for (i = 1; i < RTLH_MAX_HEAPS; ++i) {
        if (!g_heaps[i]) {
            g_heaps[i] = h;
            ok = 1;
            break;
        }
    }
    heap_unlock(&g_heaps_lock);
    return ok;
}

static void unregister_heap(rh_heap *h)
{
    heap_lock(&g_heaps_lock);
    for (unsigned i = 1; i < RTLH_MAX_HEAPS; ++i)
        if (g_heaps[i] == h)
            g_heaps[i] = NULL;
    heap_unlock(&g_heaps_lock);
}

static rh_heap *process_heap(void)
{
    rh_heap *h = g_heaps[0];
    PEB *peb;
    if (h)
        return h;
    heap_lock(&g_heaps_lock);
    h = g_heaps[0];
    if (!h) {
        h = rh_create(&g_heap_os, RH_GROWABLE, RH_GROW, 0);
        g_heaps[0] = h;
        peb = current_peb();
        if (h && peb)
            peb->ProcessHeap = h;
    }
    heap_unlock(&g_heaps_lock);
    return h;
}

/* A heap handle names a heap this module created; NULL names the process heap. */
static rh_heap *heap_from(PVOID handle)
{
    rh_heap *h = handle ? (rh_heap *)handle : process_heap();
    if (h && h->magic == RH_HEAP_MAGIC)
        return h;
    return NULL;
}

static void raise_no_memory(void)
{
    EXCEPTION_RECORD rec;
    rh_zero(&rec, sizeof(rec));
    rec.ExceptionCode = (DWORD)STATUS_NO_MEMORY;
    rec.ExceptionFlags = 1; /* EXCEPTION_NONCONTINUABLE */
    RtlRaiseException(&rec);
}

PVOID __stdcall RtlCreateHeap(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit, PVOID lock, PVOID params)
{
    rh_heap *h;
    SIZE_T size = reserve ? reserve : commit;
    (void)base;
    (void)lock;
    (void)params;
    if (flags & RH_GROWABLE)
        h = rh_create(&g_heap_os, flags, size, 0);
    else
        h = rh_create(&g_heap_os, flags, size, size ? size : RH_GROW);
    if (h && !register_heap(h)) {
        rh_destroy(h);
        return NULL;
    }
    return h;
}

PVOID __stdcall RtlDestroyHeap(PVOID heap)
{
    rh_heap *h = heap_from(heap);
    if (!h || h == g_heaps[0])
        return heap; /* the process heap cannot be destroyed */
    unregister_heap(h);
    rh_destroy(h);
    return NULL;
}

PVOID __stdcall RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T size)
{
    rh_heap *h = heap_from(heap);
    void *p;
    if (!h)
        return NULL;
    p = rh_alloc(h, size, flags & RH_ZERO_MEMORY);
    if (!p && ((flags | h->options) & RTLH_HEAP_GENERATE_EXCEPTIONS))
        raise_no_memory();
    return p;
}

BOOLEAN __stdcall RtlFreeHeap(PVOID heap, ULONG flags, PVOID ptr)
{
    rh_heap *h = heap_from(heap);
    (void)flags;
    if (!h)
        return 0;
    if (!ptr)
        return 1; /* freeing NULL succeeds */
    return rh_free(h, ptr) != 0;
}

PVOID __stdcall RtlReAllocateHeap(PVOID heap, ULONG flags, PVOID ptr, SIZE_T size)
{
    rh_heap *h = heap_from(heap);
    void *p;
    if (!h || !ptr)
        return NULL;
    p = rh_realloc(h, ptr, size, flags & (RH_ZERO_MEMORY | RH_REALLOC_IN_PLACE_ONLY));
    if (!p && ((flags | h->options) & RTLH_HEAP_GENERATE_EXCEPTIONS))
        raise_no_memory();
    return p;
}

SIZE_T __stdcall RtlSizeHeap(PVOID heap, ULONG flags, PVOID ptr)
{
    rh_heap *h = heap_from(heap);
    (void)flags;
    if (!h)
        return (SIZE_T)-1;
    return rh_size(h, ptr);
}

BOOLEAN __stdcall RtlValidateHeap(PVOID heap, ULONG flags, PVOID ptr)
{
    rh_heap *h = heap_from(heap);
    (void)flags;
    return h && rh_validate(h, ptr);
}

/* Returns the size of the largest free block. Free blocks are already coalesced, so there is
 * nothing to compact. */
SIZE_T __stdcall RtlCompactHeap(PVOID heap, ULONG flags)
{
    rh_heap *h = heap_from(heap);
    size_t best = 0;
    (void)flags;
    if (!h)
        return 0;
    rh_lock(h);
    for (unsigned b = RH_BINS; b-- > 0 && !best;) {
        for (rh_chunk *c = h->bins[b]; c; c = rh_links(c)->next) {
            size_t sz = rh_chunk_size(c) - RH_HEADER;
            if (sz > best)
                best = sz;
        }
    }
    rh_unlock(h);
    return best;
}

BOOLEAN __stdcall RtlLockHeap(PVOID heap)
{
    rh_heap *h = heap_from(heap);
    if (!h)
        return 0;
    rh_lock(h);
    return 1;
}

BOOLEAN __stdcall RtlUnlockHeap(PVOID heap)
{
    rh_heap *h = heap_from(heap);
    if (!h)
        return 0;
    rh_unlock(h);
    return 1;
}

/* RTL_HEAP_WALK_ENTRY on x64: DataAddress 0, DataSize 8, OverheadBytes 16, SegmentIndex 17, Flags 18,
 * then a union at 24 whose Segment form holds CommittedSize, UnCommittedSize, FirstEntry (32) and
 * LastEntry (40). The walk keeps its cursor in FirstEntry and LastEntry, so the caller must pass the
 * entry back unchanged. Flags: 4 busy, 2 an uncommitted range, 1 a segment; a free block is flags 0. */
#define RTLH_WALK_BUSY 0x0004u
#define RTLH_WALK_CURSOR_SEG 32u
#define RTLH_WALK_CURSOR_CHUNK 40u

NTSTATUS __stdcall RtlWalkHeap(PVOID heap, PVOID entry)
{
    rh_heap *h = heap_from(heap);
    uint8_t *e = (uint8_t *)entry;
    rh_walk w;
    NTSTATUS st = STATUS_SUCCESS;
    unsigned idx = 0;
    if (!h || !e)
        return STATUS_INVALID_PARAMETER;
    rh_walk_init(&w);
    rh_lock(h);
    w.seg = *(rh_segment **)(e + RTLH_WALK_CURSOR_SEG);
    w.chunk = *(rh_chunk **)(e + RTLH_WALK_CURSOR_CHUNK);
    if (!rh_walk_next(h, &w)) {
        st = STATUS_NO_MORE_ENTRIES;
    } else {
        for (rh_segment *s = h->segments; s && s != w.seg; s = s->next)
            ++idx;
        *(PVOID *)(e + 0) = w.data;
        *(SIZE_T *)(e + 8) = w.size;
        e[16] = (uint8_t)w.overhead;
        e[17] = (uint8_t)idx;
        *(uint16_t *)(e + 18) = w.busy ? RTLH_WALK_BUSY : 0;
        *(rh_segment **)(e + RTLH_WALK_CURSOR_SEG) = w.seg;
        *(rh_chunk **)(e + RTLH_WALK_CURSOR_CHUNK) = w.chunk;
    }
    rh_unlock(h);
    return st;
}

/* HEAP_INFORMATION_CLASS: 0 compatibility (0 = standard heap), 1 termination on corruption,
 * 3 optimize resources. The settings are accepted; nothing changes. */
NTSTATUS __stdcall RtlSetHeapInformation(PVOID heap, ULONG info_class, PVOID info, SIZE_T len)
{
    (void)info;
    (void)len;
    if (!heap_from(heap))
        return STATUS_INVALID_PARAMETER;
    return (info_class == 0 || info_class == 1 || info_class == 3) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

NTSTATUS __stdcall RtlQueryHeapInformation(PVOID heap, ULONG info_class, PVOID info, SIZE_T len, PSIZE_T ret_len)
{
    if (!heap_from(heap))
        return STATUS_INVALID_PARAMETER;
    if (info_class != 0)
        return STATUS_INVALID_PARAMETER;
    if (ret_len)
        *ret_len = sizeof(ULONG);
    if (!info || len < sizeof(ULONG))
        return STATUS_BUFFER_TOO_SMALL;
    *(ULONG *)info = 0;
    return STATUS_SUCCESS;
}

/* Fills up to count handles, the process heap first, and returns how many heaps exist. */
ULONG __stdcall RtlGetProcessHeaps(ULONG count, PVOID *out)
{
    ULONG n = 0;
    process_heap();
    heap_lock(&g_heaps_lock);
    for (unsigned i = 0; i < RTLH_MAX_HEAPS; ++i) {
        if (g_heaps[i]) {
            if (out && n < count)
                out[n] = g_heaps[i];
            ++n;
        }
    }
    heap_unlock(&g_heaps_lock);
    return n;
}
#endif
