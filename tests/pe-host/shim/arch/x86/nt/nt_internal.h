/* Stand-in for the kernel tree's arch/x86/nt/nt_internal.h, which binfmt_pe.c includes when CONFIG_NT_PERSONALITY is set.
 * The loader needs nothing from it in the host build: its NT hooks are declared in binfmt_pe.c itself. */
#ifndef PEHOST_NT_INTERNAL_STUB_H
#define PEHOST_NT_INTERNAL_STUB_H
#endif
