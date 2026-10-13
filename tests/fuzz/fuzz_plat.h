/* fuzz_plat.h - platform hooks for fuzz_pe.c (see fuzz_plat.c). */
#ifndef FUZZ_PLAT_H
#define FUZZ_PLAT_H

#include <stddef.h>
#include <stdint.h>

#define FUZZ_PAGE 4096u

#ifdef _WIN32
#define FUZZ_DEVNULL "NUL"
#else
#define FUZZ_DEVNULL "/dev/null"
#endif

/* Called with the faulting address from the fault handler.  It must not return
 * when it wants the process to stop. */
typedef void (*fuzz_fault_fn)(const void *addr);

/* Returns data_bytes of read-write memory with a PROT_NONE guard page before
 * and after it, or NULL.  The returned pointer is the first data byte. */
uint8_t *plat_alloc_guarded(size_t data_bytes);

void plat_install_fault_handler(fuzz_fault_fn fn);

/* Sends stderr to the null device unless FUZZ_DIAG is set.  pe_validate logs
 * every rejection there. */
void plat_silence_stderr(void);

#endif /* FUZZ_PLAT_H */
