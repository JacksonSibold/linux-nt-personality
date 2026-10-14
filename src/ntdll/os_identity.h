/* os_identity.h - the OS identity this personality reports (ADR 0075).
 *
 * The one definition used by RtlGetVersion (src/ntdll/ntdll_main.c), the
 * PEB written by the kernel (ensure_peb in src/kernel/nt/proc_thread.c and
 * pe_init_peb in src/kernel/fs/binfmt_pe.c) and the tests. Pure macros, so
 * kernel and userspace sources can both include it.
 *
 * tests/conformance/os-identity-policy.json declares these values. The audit
 * tools/check_os_identity.py fails if this header and the policy disagree.
 */
#ifndef NT_OS_IDENTITY_H
#define NT_OS_IDENTITY_H

#define NT_OS_MAJOR_VERSION 10
#define NT_OS_MINOR_VERSION 0
#define NT_OS_BUILD_NUMBER  26200  /* golden build 26200.9457 */
#define NT_OS_PLATFORM_ID   2      /* VER_PLATFORM_WIN32_NT */
#define NT_OS_PRODUCT_TYPE  1      /* VER_NT_WORKSTATION */

#endif /* NT_OS_IDENTITY_H */
