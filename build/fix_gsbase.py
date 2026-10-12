#!/usr/bin/env python3
"""
Repair the build-blocking regression in fs/binfmt_pe.c.

F3's patch called set_user_gsbase(), which does not exist in the kernel. The
real API for establishing a user GS base on the current task is the one
do_arch_prctl_64() uses for ARCH_SET_GS (arch/x86/kernel/process_64.c):

    preempt_disable();
    loadseg(GS, 0);                       # index must be 0 when using a base
    x86_gsbase_write_cpu_inactive(base);  # writes MSR_KERNEL_GS_BASE
    task->thread.gsbase = base;
    preempt_enable();

MSR_KERNEL_GS_BASE holds the *user* GS base while the CPU is in kernel mode;
SWAPGS moves it into GS on the return to user space. That is precisely what a
PE process needs, because the TEB is addressed GS-relative.

loadseg() is file-local to process_64.c, so we set thread.gsindex directly
instead; start_thread() has already reset the segment registers by this point.

We also bounds-check the TEB against TASK_SIZE_MAX. Writing a non-canonical or
kernel address into a GS base is a privilege-escalation primitive, and the
value ultimately derives from the PE image layout, so it must be validated.
"""
import os
import re
import sys

# Linux 6.18 tree in WSL. NTPERS_LINUX overrides the default ~/ntpers/linux-6.18.
LINUX_TREE = os.environ.get("NTPERS_LINUX") or os.path.join(
    os.path.expanduser("~"), "ntpers", "linux-6.18")
PATH = os.path.join(LINUX_TREE, "fs", "binfmt_pe.c")

OLD = """        // start_thread zeroes thread.gsbase via elf_common_init, so set it after
        current->thread.gsbase = (unsigned long)state.teb;
        set_user_gsbase(current, (unsigned long)state.teb);"""

NEW = """        /*
         * start_thread() resets the segment state, so the TEB base must be
         * installed afterwards. This mirrors do_arch_prctl_64()'s ARCH_SET_GS
         * path: index 0 plus an explicit base write.
         *
         * x86_gsbase_write_cpu_inactive() writes MSR_KERNEL_GS_BASE, which is
         * where the *user* GS base lives while we are in the kernel; SWAPGS
         * installs it on the way out. The TEB is addressed GS-relative by
         * every PE binary, so this is the load-bearing register for the whole
         * Windows personality.
         */
        if ((unsigned long)state.teb < TASK_SIZE_MAX) {
                preempt_disable();
                current->thread.gsindex = 0;
                current->thread.gsbase = (unsigned long)state.teb;
                x86_gsbase_write_cpu_inactive((unsigned long)state.teb);
                preempt_enable();
        } else {
                /*
                 * Refuse rather than install a kernel or non-canonical base:
                 * a bad GS base is an escalation primitive, not a cosmetic
                 * error, and the value derives from untrusted image layout.
                 */
                pr_err("binfmt_pe: refusing out-of-range TEB %px\\n", state.teb);
                ret = -EINVAL;
                goto out;
        }"""


def main():
    with open(PATH, encoding="utf-8", errors="replace") as fh:
        src = fh.read()

    if "set_user_gsbase" not in src:
        print("set_user_gsbase already gone; nothing to do")
        return 0

    if OLD not in src:
        # Fall back to a targeted line replacement if whitespace drifted.
        pat = re.compile(r"[ \t]*set_user_gsbase\([^;]*\);")
        if not pat.search(src):
            print("FAILED: could not locate the call site", file=sys.stderr)
            return 1
        # Same guarded block as the exact-match path. A lambda keeps re.sub
        # from interpreting the backslash escapes in the pr_err format string.
        src = pat.sub(lambda _m: NEW, src)
        print("patched via fallback regex")
    else:
        src = src.replace(OLD, NEW)
        print("patched via exact match")

    with open(PATH, "w", encoding="utf-8") as fh:
        fh.write(src)

    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
