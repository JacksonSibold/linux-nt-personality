/*
 * kernel32_global.c - kernel32 Global* and Local* memory APIs (the handle table is in
 * k32_global_pure.h; the rules are checked by tests/ntdll-host/test_k32_global_pure.c).
 *
 * Storage comes from VirtualAlloc/VirtualFree, one commit per block, so a freed block
 * is returned to the kernel. The process heap in kernel32.c is a bump allocator whose
 * HeapFree does nothing, which would make a clipboard copy loop run out of heap.
 *
 * One table serves every caller in the process. Access takes a spin lock: the
 * critical sections are a few table updates, and the allocator is only reached from
 * here. Last-error values come from the table and are copied to SetLastError.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "k32_global_pure.h"
#include <stddef.h>
#include <stdint.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif

typedef PVOID LPVOID;
typedef LPVOID HGLOBAL;
typedef LPVOID HLOCAL;
typedef const void *LPCVOID;
typedef uint32_t UINT;

#define K32GM_MEM_COMMIT  0x1000u
#define K32GM_MEM_RESERVE 0x2000u
#define K32GM_MEM_RELEASE 0x8000u
#define K32GM_PAGE_READWRITE 0x04u

/* Defined in kernel32.c and kernel32_mm.c (VirtualAlloc, VirtualFree, SetLastError). */
LPVOID WINAPI VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot);
BOOL WINAPI VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
void SetLastError(DWORD e);

static void *k32gm_alloc(void *ctx, size_t bytes)
{
    (void)ctx;
    return VirtualAlloc(NULL, (SIZE_T)bytes, K32GM_MEM_COMMIT | K32GM_MEM_RESERVE, K32GM_PAGE_READWRITE);
}

static void k32gm_release(void *ctx, void *p)
{
    (void)ctx;
    /* MEM_RELEASE takes size 0: the whole reservation is released. */
    VirtualFree(p, 0, K32GM_MEM_RELEASE);
}

static k32g_table g_k32_global;
static volatile int g_k32_global_ready;
static volatile int g_k32_global_lock;

static void k32gm_acquire(void)
{
    while (__atomic_test_and_set(&g_k32_global_lock, __ATOMIC_ACQUIRE)) {
        /* Contention is brief: each holder only updates the table. */
    }
}

static void k32gm_release_lock(void)
{
    __atomic_clear(&g_k32_global_lock, __ATOMIC_RELEASE);
}

/* Initialises the table on first use. Called with the lock held. */
static void k32gm_ensure(void)
{
    if (!g_k32_global_ready) {
        k32g_ops ops;
        ops.alloc = k32gm_alloc;
        ops.release = k32gm_release;
        ops.ctx = NULL;
        k32g_init(&g_k32_global, ops);
        __atomic_store_n(&g_k32_global_ready, 1, __ATOMIC_RELEASE);
    }
}

/* Runs one table operation under the lock and copies its last error out. */
#define K32GM_BEGIN() do { k32gm_acquire(); k32gm_ensure(); } while (0)
#define K32GM_END(err) do { DWORD k32gm_e_ = (DWORD)(err); k32gm_release_lock(); SetLastError(k32gm_e_); } while (0)

static DWORD k32gm_table_error(void)
{
    return (DWORD)k32g_get_last_error(&g_k32_global);
}

/* ---- Global* -------------------------------------------------------------------- */

HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T bytes)
{
    uintptr_t h;
    K32GM_BEGIN();
    h = k32g_alloc(&g_k32_global, (uint32_t)flags, (size_t)bytes);
    K32GM_END(k32gm_table_error());
    return (HGLOBAL)h;
}

HGLOBAL WINAPI GlobalFree(HGLOBAL mem)
{
    uintptr_t r;
    K32GM_BEGIN();
    r = k32g_free(&g_k32_global, (uintptr_t)mem);
    K32GM_END(k32gm_table_error());
    return (HGLOBAL)r;
}

LPVOID WINAPI GlobalLock(HGLOBAL mem)
{
    void *p;
    K32GM_BEGIN();
    p = k32g_lock(&g_k32_global, (uintptr_t)mem);
    K32GM_END(k32gm_table_error());
    return p;
}

BOOL WINAPI GlobalUnlock(HGLOBAL mem)
{
    int still;
    K32GM_BEGIN();
    still = k32g_unlock(&g_k32_global, (uintptr_t)mem);
    /* Win32: zero means the count reached zero or an error happened; the error is set. */
    K32GM_END(k32gm_table_error());
    return still != 0;
}

SIZE_T WINAPI GlobalSize(HGLOBAL mem)
{
    size_t n;
    int ok;
    K32GM_BEGIN();
    ok = k32g_handle_valid(&g_k32_global, (uintptr_t)mem);
    n = k32g_size(&g_k32_global, (uintptr_t)mem);
    K32GM_END(ok ? 0u : K32G_ERROR_INVALID_HANDLE);
    return (SIZE_T)n;
}

UINT WINAPI GlobalFlags(HGLOBAL mem)
{
    uint32_t f;
    K32GM_BEGIN();
    f = k32g_flags(&g_k32_global, (uintptr_t)mem);
    K32GM_END(f == K32G_GMEM_INVALID_HANDLE ? K32G_ERROR_INVALID_HANDLE : 0u);
    return (UINT)f;
}

HGLOBAL WINAPI GlobalHandle(LPCVOID p)
{
    uintptr_t h;
    K32GM_BEGIN();
    h = k32g_handle_of_ptr(&g_k32_global, p);
    K32GM_END(h ? 0u : K32G_ERROR_INVALID_HANDLE);
    return (HGLOBAL)h;
}

HGLOBAL WINAPI GlobalReAlloc(HGLOBAL mem, SIZE_T bytes, UINT flags)
{
    uintptr_t r;
    K32GM_BEGIN();
    r = k32g_realloc(&g_k32_global, (uintptr_t)mem, (size_t)bytes, (uint32_t)flags);
    K32GM_END(k32gm_table_error());
    return (HGLOBAL)r;
}

/* Returns the handle on success and NULL on failure (the Win32 page for GlobalDiscard
 * does not fix the success value; the table itself returns the discarded size). */
HGLOBAL WINAPI GlobalDiscard(HGLOBAL mem)
{
    size_t n;
    DWORD err;
    K32GM_BEGIN();
    n = k32g_discard(&g_k32_global, (uintptr_t)mem);
    err = k32gm_table_error();
    K32GM_END(err);
    (void)n;
    return err == 0 ? mem : NULL;
}

/* ---- Local* --------------------------------------------------------------------- */

HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T bytes)
{
    uintptr_t h;
    K32GM_BEGIN();
    h = k32g_alloc(&g_k32_global, k32g_local_to_global_flags((uint32_t)flags), (size_t)bytes);
    K32GM_END(k32gm_table_error());
    return (HLOCAL)h;
}

HLOCAL WINAPI LocalFree(HLOCAL mem)
{
    uintptr_t r;
    K32GM_BEGIN();
    r = k32g_free(&g_k32_global, (uintptr_t)mem);
    K32GM_END(k32gm_table_error());
    return (HLOCAL)r;
}

LPVOID WINAPI LocalLock(HLOCAL mem)
{
    void *p;
    K32GM_BEGIN();
    p = k32g_lock(&g_k32_global, (uintptr_t)mem);
    K32GM_END(k32gm_table_error());
    return p;
}

BOOL WINAPI LocalUnlock(HLOCAL mem)
{
    int still;
    K32GM_BEGIN();
    still = k32g_unlock(&g_k32_global, (uintptr_t)mem);
    K32GM_END(k32gm_table_error());
    return still != 0;
}

SIZE_T WINAPI LocalSize(HLOCAL mem)
{
    size_t n;
    int ok;
    K32GM_BEGIN();
    ok = k32g_handle_valid(&g_k32_global, (uintptr_t)mem);
    n = k32g_size(&g_k32_global, (uintptr_t)mem);
    K32GM_END(ok ? 0u : K32G_ERROR_INVALID_HANDLE);
    return (SIZE_T)n;
}

UINT WINAPI LocalFlags(HLOCAL mem)
{
    uint32_t f;
    K32GM_BEGIN();
    f = k32g_flags(&g_k32_global, (uintptr_t)mem);
    K32GM_END(f == K32G_LMEM_INVALID_HANDLE ? K32G_ERROR_INVALID_HANDLE : 0u);
    return (UINT)f;
}

HLOCAL WINAPI LocalHandle(LPCVOID p)
{
    uintptr_t h;
    K32GM_BEGIN();
    h = k32g_handle_of_ptr(&g_k32_global, p);
    K32GM_END(h ? 0u : K32G_ERROR_INVALID_HANDLE);
    return (HLOCAL)h;
}

HLOCAL WINAPI LocalReAlloc(HLOCAL mem, SIZE_T bytes, UINT flags)
{
    uintptr_t r;
    K32GM_BEGIN();
    r = k32g_realloc(&g_k32_global, (uintptr_t)mem, (size_t)bytes,
                     k32g_local_to_global_flags((uint32_t)flags));
    K32GM_END(k32gm_table_error());
    return (HLOCAL)r;
}

HLOCAL WINAPI LocalDiscard(HLOCAL mem)
{
    size_t n;
    DWORD err;
    K32GM_BEGIN();
    n = k32g_discard(&g_k32_global, (uintptr_t)mem);
    err = k32gm_table_error();
    K32GM_END(err);
    (void)n;
    return err == 0 ? mem : NULL;
}

#endif /* __MINGW32__ */
