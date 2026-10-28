/*
 * Data that Wine's ntdll keeps in loader.c and that the ported Rtl sources read.
 *
 * Original code of this repository; the names and meaning follow Wine's dlls/ntdll/loader.c (LGPL-2.1-or-later).
 */
#include <stdarg.h>

#include "windef.h"
#include "winnt.h"
#include "ntdll_misc.h"

/* The Windows directory of the personality: the same value that kernel32's GetWindowsDirectory returns. */
const WCHAR windows_dir[] = L"C:\\Windows";

/*
 * NtGetCurrentProcessorNumber
 *
 * The number of the processor the thread runs on. rdtscp returns IA32_TSC_AUX in ecx, which the kernel sets to the processor
 * number in the low 12 bits (Linux does this for vDSO getcpu, and the personality runs on it).
 */
ULONG WINAPI NtGetCurrentProcessorNumber(void)
{
    unsigned int lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return aux & 0xfff;
}
