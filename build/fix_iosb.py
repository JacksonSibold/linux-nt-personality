#!/usr/bin/env python3
"""
Fix a systemic user-pointer dereference bug class in nt_io.c.

IO_STATUS_BLOCK is supplied by user space. The kernel path was writing through
it directly:

    iosb->Information = Length;
    iosb->DUMMYUNIONNAME.Status = STATUS_SUCCESS;

Under SMAP that is an immediate oops (this is the ntio_console_write crash:
"BUG: unable to handle page fault ... RIP: ntio_console_write.cold+0x35,
mov %eax,(%r15)"). Even without SMAP it is wrong: the address may be unmapped,
read-only, or racing with another thread, and a fault while holding a lock or
with IRQs disabled escalates to "exited with irqs disabled".

There were 21 such sites. Rather than patch them one at a time, introduce
accessors that are correct in both build modes and route every access through
them. Reads of IoStatusBlock->Information have the same defect and are fixed
too.
"""
import re
import sys

PATH = "/root/ntpers/linux-6.18/arch/x86/nt/nt_io.c"

HELPER = r"""
/*
 * IO_STATUS_BLOCK accessors.
 *
 * IoStatusBlock always points into user space. Never dereference it directly
 * from the kernel path: SMAP turns that into an oops, and a fault taken while
 * holding a lock is far worse than a returned error. Every access goes through
 * these two helpers so the unsafe pattern cannot reappear by copy-paste.
 */
static inline NTSTATUS ntio_put_iosb(PIO_STATUS_BLOCK iosb, NTSTATUS st,
                                     ULONG_PTR info)
{
	if (!iosb)
		return STATUS_ACCESS_VIOLATION;
#ifdef __KERNEL__
	{
		IO_STATUS_BLOCK k;

		memset(&k, 0, sizeof(k));
		k.DUMMYUNIONNAME.Status = st;
		k.Information = info;
		if (copy_to_user(iosb, &k, sizeof(k)))
			return STATUS_ACCESS_VIOLATION;
	}
#else
	iosb->DUMMYUNIONNAME.Status = st;
	iosb->Information = info;
#endif
	return st;
}

static inline ULONG_PTR ntio_get_iosb_info(PIO_STATUS_BLOCK iosb)
{
	if (!iosb)
		return 0;
#ifdef __KERNEL__
	{
		IO_STATUS_BLOCK k;

		if (copy_from_user(&k, iosb, sizeof(k)))
			return 0;
		return k.Information;
	}
#else
	return iosb->Information;
#endif
}
"""

# (regex, replacement) applied in order. Paired Status+Information writes are
# collapsed into a single ntio_put_iosb() call.
PAIRS = [
    # Information then Status
    (r"[ \t]*(?P<v>iosb|IoStatusBlock)->Information = (?P<info>[^;]+);\s*\n"
     r"[ \t]*(?P=v)->DUMMYUNIONNAME\.Status = (?P<st>[^;]+);",
     lambda m: "\tntio_put_iosb(%s, %s, (ULONG_PTR)(%s));"
               % (m.group("v"), m.group("st").strip(), m.group("info").strip())),
    # Status then Information
    (r"[ \t]*(?P<v>iosb|IoStatusBlock)->DUMMYUNIONNAME\.Status = (?P<st>[^;]+);\s*\n"
     r"[ \t]*(?P=v)->Information = (?P<info>[^;]+);",
     lambda m: "\tntio_put_iosb(%s, %s, (ULONG_PTR)(%s));"
               % (m.group("v"), m.group("st").strip(), m.group("info").strip())),
]

SINGLES = [
    # remaining lone Status writes
    (r"[ \t]*(?P<v>iosb|IoStatusBlock)->DUMMYUNIONNAME\.Status = (?P<st>[^;]+);",
     lambda m: "\tntio_put_iosb(%s, %s, ntio_get_iosb_info(%s));"
               % (m.group("v"), m.group("st").strip(), m.group("v"))),
    # remaining lone Information writes
    (r"[ \t]*(?P<v>iosb|IoStatusBlock)->Information = (?P<info>[^;]+);",
     lambda m: "\tntio_put_iosb(%s, STATUS_SUCCESS, (ULONG_PTR)(%s));"
               % (m.group("v"), m.group("info").strip())),
]

# reads
READS = [
    (r"(?P<v>IoStatusBlock|iosb)->Information\b(?!\s*=)",
     lambda m: "ntio_get_iosb_info(%s)" % m.group("v")),
]


def main():
    with open(PATH, encoding="utf-8", errors="replace") as fh:
        src = fh.read()

    before = len(re.findall(r"(iosb|IoStatusBlock)->", src))
    print("direct iosb accesses before:", before)

    # Insert helpers after the last #include so the types are available.
    if "ntio_put_iosb" not in src:
        idx = src.rfind("#include")
        end = src.find("\n", idx) + 1
        src = src[:end] + HELPER + src[end:]
        print("inserted accessors")

    # Reads first: the write regexes use a negative lookahead for '=', but
    # doing reads first avoids any ambiguity.
    for pat, rep in READS:
        src, n = re.subn(pat, rep, src)
        if n:
            print("  reads rewritten:", n)

    for pat, rep in PAIRS:
        src, n = re.subn(pat, rep, src)
        if n:
            print("  paired writes collapsed:", n)

    for pat, rep in SINGLES:
        src, n = re.subn(pat, rep, src)
        if n:
            print("  single writes rewritten:", n)

    # Do not rewrite inside the helper itself.
    src = src.replace(
        "\tntio_put_iosb(iosb, st, ntio_get_iosb_info(iosb));",
        "\tiosb->DUMMYUNIONNAME.Status = st;\n\tiosb->Information = info;")

    after = len(re.findall(r"(iosb|IoStatusBlock)->(?!DUMMYUNION)", src))
    print("remaining direct accesses (should be helper-internal only):", after)

    with open(PATH, "w", encoding="utf-8") as fh:
        fh.write(src)
    print("written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
