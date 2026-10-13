/* fuzz_plat.c - platform layer for fuzz_pe.c: guarded memory and fault hooks.
 *
 * Kept in its own translation unit because windows.h defines PE image types
 * that clash with src/loader/pe_format.h, which fuzz_pe.c includes.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_plat.h"

#ifdef _WIN32
#include <windows.h>

static fuzz_fault_fn g_fault_fn;

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (g_fault_fn && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR))
        g_fault_fn((const void *)ep->ExceptionRecord->ExceptionInformation[1]);
    return EXCEPTION_CONTINUE_SEARCH;
}

uint8_t *plat_alloc_guarded(size_t data_bytes)
{
    size_t total = data_bytes + 2 * FUZZ_PAGE;
    DWORD old;
    uint8_t *base = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base)
        return NULL;
    VirtualProtect(base, FUZZ_PAGE, PAGE_NOACCESS, &old);
    VirtualProtect(base + FUZZ_PAGE + data_bytes, FUZZ_PAGE, PAGE_NOACCESS, &old);
    return base + FUZZ_PAGE;
}

void plat_install_fault_handler(fuzz_fault_fn fn)
{
    g_fault_fn = fn;
    AddVectoredExceptionHandler(1, on_exception);
}

#else
#include <signal.h>
#include <sys/mman.h>

static fuzz_fault_fn g_fault_fn;

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    if (!g_fault_fn) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_fault_fn(si->si_addr);
}

uint8_t *plat_alloc_guarded(size_t data_bytes)
{
    size_t total = data_bytes + 2 * FUZZ_PAGE;
    uint8_t *base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return NULL;
    if (mprotect(base, FUZZ_PAGE, PROT_NONE) != 0 ||
        mprotect(base + FUZZ_PAGE + data_bytes, FUZZ_PAGE, PROT_NONE) != 0)
        return NULL;
    return base + FUZZ_PAGE;
}

void plat_install_fault_handler(fuzz_fault_fn fn)
{
    struct sigaction sa;
    g_fault_fn = fn;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
#endif

void plat_silence_stderr(void)
{
    if (!getenv("FUZZ_DIAG"))
        (void)freopen(FUZZ_DEVNULL, "w", stderr);
}
