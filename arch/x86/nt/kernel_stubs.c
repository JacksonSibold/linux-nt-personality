// SPDX-License-Identifier: GPL-2.0
/*
 * kernel_stubs.c - NT personality banner.
 *
 * Defines nt_banner_init(), the one symbol the arch NT objects need from this
 * file. Callers and checks: tests/kernel/check_kernel_complete.sh and
 * build/verify_nt_symbols.sh look for nt_banner_init in vmlinux, and the
 * banner text is what appears in dmesg (vm/README.md).
 *
 * vm/rebuild_kdir.sh and vm/kbuild.sh write a file with the same
 * nt_banner_init() and banner text into KDIR/arch/x86/nt/kernel_stubs.c. The
 * from-scratch path overwrites it unconditionally; kbuild writes it only when
 * missing. Keep the banner text identical in all three places.
 */

#include <linux/kernel.h>

void nt_banner_init(void)
{
	pr_info("NT personality: ntpers v1.0 banner\n");
}
