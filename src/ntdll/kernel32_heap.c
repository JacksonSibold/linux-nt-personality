/*
 * kernel32_heap.c - the kernel32 heap API on the Rtl heap in ntdll (ntdll_heap.c): GetProcessHeap,
 * GetProcessHeaps, HeapCreate, HeapDestroy, HeapAlloc, HeapReAlloc, HeapFree, HeapSize, HeapValidate,
 * HeapCompact, HeapLock, HeapUnlock, HeapWalk, HeapSetInformation and HeapQueryInformation.
 *
 * Like Windows, a successful call leaves the last error alone; a failure sets it. HeapAlloc and
 * HeapReAlloc fail with ERROR_NOT_ENOUGH_MEMORY, the rest with ERROR_INVALID_PARAMETER. A heap created
 * with HEAP_GENERATE_EXCEPTIONS raises STATUS_NO_MEMORY in ntdll instead of returning NULL.
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
#include "rtl_heap_pure.h"
#include <stddef.h>
#include <stdint.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif

typedef PVOID LPVOID;
typedef const void *LPCVOID;

#define K32H_ERROR_INVALID_PARAMETER 87u
#define K32H_ERROR_NOT_ENOUGH_MEMORY 8u
#define K32H_ERROR_INSUFFICIENT_BUFFER 122u
#define K32H_HEAP_GROWABLE 0x00000002u
#define K32H_HEAP_CREATE_VALID 0x00040005u /* NO_SERIALIZE, GENERATE_EXCEPTIONS, CREATE_ENABLE_EXECUTE */

__declspec(dllimport) PVOID __stdcall RtlCreateHeap(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit, PVOID lock, PVOID params);
__declspec(dllimport) PVOID __stdcall RtlDestroyHeap(PVOID heap);
__declspec(dllimport) PVOID __stdcall RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T size);
__declspec(dllimport) BOOLEAN __stdcall RtlFreeHeap(PVOID heap, ULONG flags, PVOID ptr);
__declspec(dllimport) PVOID __stdcall RtlReAllocateHeap(PVOID heap, ULONG flags, PVOID ptr, SIZE_T size);
__declspec(dllimport) SIZE_T __stdcall RtlSizeHeap(PVOID heap, ULONG flags, PVOID ptr);
__declspec(dllimport) BOOLEAN __stdcall RtlValidateHeap(PVOID heap, ULONG flags, PVOID ptr);
__declspec(dllimport) SIZE_T __stdcall RtlCompactHeap(PVOID heap, ULONG flags);
__declspec(dllimport) BOOLEAN __stdcall RtlLockHeap(PVOID heap);
__declspec(dllimport) BOOLEAN __stdcall RtlUnlockHeap(PVOID heap);
__declspec(dllimport) NTSTATUS __stdcall RtlWalkHeap(PVOID heap, PVOID entry);
__declspec(dllimport) NTSTATUS __stdcall RtlSetHeapInformation(PVOID heap, ULONG info_class, PVOID info, SIZE_T len);
__declspec(dllimport) NTSTATUS __stdcall RtlQueryHeapInformation(PVOID heap, ULONG info_class, PVOID info, SIZE_T len, PSIZE_T ret_len);
__declspec(dllimport) ULONG __stdcall RtlGetProcessHeaps(ULONG count, PVOID *out);

void SetLastError(DWORD e);

static HANDLE g_process_heap;

HANDLE WINAPI GetProcessHeap(void)
{
    if (!g_process_heap) {
        PVOID h[1] = {NULL};
        RtlGetProcessHeaps(1, h); /* creates the process heap on first use and publishes it in the PEB */
        g_process_heap = h[0];
    }
    return g_process_heap;
}

/* Called when this module attaches: the process heap must exist and be named by PEB.ProcessHeap. */
void k32_publish_process_heap(void)
{
    GetProcessHeap();
}

DWORD WINAPI GetProcessHeaps(DWORD count, HANDLE *heaps)
{
    return RtlGetProcessHeaps(count, (PVOID *)heaps);
}

HANDLE WINAPI HeapCreate(DWORD options, SIZE_T initial, SIZE_T maximum)
{
    ULONG flags = options & K32H_HEAP_CREATE_VALID;
    PVOID h;
    if (options & ~K32H_HEAP_CREATE_VALID) { /* only the documented option bits are accepted */
        SetLastError(K32H_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (maximum && initial > maximum) {
        SetLastError(K32H_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!maximum)
        flags |= K32H_HEAP_GROWABLE;
    h = RtlCreateHeap(flags, NULL, maximum ? maximum : initial, initial, NULL, NULL);
    if (!h)
        SetLastError(K32H_ERROR_NOT_ENOUGH_MEMORY);
    return h;
}

BOOL WINAPI HeapDestroy(HANDLE heap)
{
    if (!heap || RtlDestroyHeap(heap) != NULL) {
        SetLastError(K32H_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

LPVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
{
    LPVOID p = RtlAllocateHeap(heap, flags, size);
    if (!p)
        SetLastError(K32H_ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

LPVOID WINAPI HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T size)
{
    LPVOID p = RtlReAllocateHeap(heap, flags, mem, size);
    if (!p)
        SetLastError(mem ? K32H_ERROR_NOT_ENOUGH_MEMORY : K32H_ERROR_INVALID_PARAMETER);
    return p;
}

BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID mem)
{
    if (!RtlFreeHeap(heap, flags, mem)) {
        SetLastError(K32H_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

SIZE_T WINAPI HeapSize(HANDLE heap, DWORD flags, LPCVOID mem)
{
    SIZE_T n = RtlSizeHeap(heap, flags, (PVOID)mem);
    if (n == (SIZE_T)-1)
        SetLastError(K32H_ERROR_INVALID_PARAMETER);
    return n;
}

BOOL WINAPI HeapValidate(HANDLE heap, DWORD flags, LPCVOID mem)
{
    return RtlValidateHeap(heap, flags, (PVOID)mem) != 0;
}

SIZE_T WINAPI HeapCompact(HANDLE heap, DWORD flags)
{
    return RtlCompactHeap(heap, flags);
}

BOOL WINAPI HeapLock(HANDLE heap)
{
    return RtlLockHeap(heap) != 0;
}

BOOL WINAPI HeapUnlock(HANDLE heap)
{
    return RtlUnlockHeap(heap) != 0;
}

BOOL WINAPI HeapSetInformation(HANDLE heap, DWORD info_class, PVOID info, SIZE_T len)
{
    if (!heap) { /* a NULL handle means "the process" for the termination-on-corruption setting only */
        if (info_class == 1)
            return 1;
        SetLastError(K32H_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!NT_SUCCESS(RtlSetHeapInformation(heap, info_class, info, len))) {
        SetLastError(K32H_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

BOOL WINAPI HeapQueryInformation(HANDLE heap, DWORD info_class, PVOID info, SIZE_T len, PSIZE_T ret_len)
{
    NTSTATUS st = RtlQueryHeapInformation(heap, info_class, info, len, ret_len);
    if (st == STATUS_BUFFER_TOO_SMALL) {
        SetLastError(K32H_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (!NT_SUCCESS(st)) {
        SetLastError(K32H_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

/* PROCESS_HEAP_ENTRY (40 bytes on x64): lpData 0, cbData 8, cbOverhead 12, iRegionIndex 13, wFlags 14,
 * then a union at 16 whose Region form ends with lpFirstBlock (24) and lpLastBlock (32). The walk keeps
 * its cursor in those two fields. HeapWalk fails with ERROR_NO_MORE_ITEMS (259) at the end. */
BOOL WINAPI HeapWalk(HANDLE heap, void *entry)
{
    uint8_t rtl[48];
    uint8_t *e = (uint8_t *)entry;
    NTSTATUS st;
    if (!heap || !entry) {
        SetLastError(K32H_ERROR_INVALID_PARAMETER);
        return 0;
    }
    rh_zero(rtl, sizeof(rtl));
    /* A fresh entry (lpData NULL) starts the walk; otherwise the cursor comes from the entry. */
    if (*(void **)e) {
        *(void **)(rtl + 32) = *(void **)(e + 24);
        *(void **)(rtl + 40) = *(void **)(e + 32);
    }
    st = RtlWalkHeap(heap, rtl);
    if (!NT_SUCCESS(st)) {
        SetLastError(st == STATUS_NO_MORE_ENTRIES ? 259u : K32H_ERROR_INVALID_PARAMETER);
        return 0;
    }
    *(void **)e = *(void **)rtl;
    *(DWORD *)(e + 8) = (DWORD)*(SIZE_T *)(rtl + 8);
    e[12] = rtl[16];
    e[13] = rtl[17];
    *(WORD *)(e + 14) = *(WORD *)(rtl + 18);
    *(void **)(e + 24) = *(void **)(rtl + 32);
    *(void **)(e + 32) = *(void **)(rtl + 40);
    return 1;
}
#endif
