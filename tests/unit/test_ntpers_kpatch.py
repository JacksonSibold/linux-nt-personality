"""Tests for tools/ntpers_kpatch.py.

Each test builds a synthetic project (the NT sources) and a synthetic kernel
tree that carries the anchors the edits look for, both in a temporary
directory. Nothing outside that directory is read or written, and no real
kernel source is needed.
"""
import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "ntpers_kpatch.py"

_spec = importlib.util.spec_from_file_location("ntpers_kpatch", SCRIPT)
kp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(kp)


def _write(root, rel, data):
    p = pathlib.Path(root) / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, str):
        data = data.encode("utf-8")
    p.write_bytes(data)


def make_project(root):
    """The NT sources, as a repository checkout would hold them (CRLF stubs)."""
    _write(root, "arch/x86/nt/Makefile",
           "ccflags-y += -w\n"
           "obj-$(CONFIG_NT_PERSONALITY) += dispatch.o\n"
           "obj-$(CONFIG_NT_PERSONALITY) += handle.o\n"
           "obj-$(CONFIG_NT_PERSONALITY) += mm.o\n"
           "obj-$(CONFIG_NT_PERSONALITY) += kernel_stubs.o\n"
           "obj-$(CONFIG_NT_PERSONALITY) += stubs.o\n")
    _write(root, "arch/x86/nt/dispatch.c",
           '#include "nt_internal.h"\n#include "handle.h"\nvoid do_nt_syscall(void) {}\n')
    _write(root, "arch/x86/nt/nt_internal.h",
           "#ifndef NT_INTERNAL_H\n#define NT_INTERNAL_H\n"
           "#define NT_SYSCALL_SKIP ((long)-1)\n#endif\n")
    _write(root, "arch/x86/nt/kernel_stubs.c",
           "#include <linux/kernel.h>\r\n\r\nvoid nt_banner_init(void)\r\n"
           "{\r\n\tpr_info(\"banner\\n\");\r\n}\r\n")
    _write(root, "src/kernel/nt/handle.c", '#include "handle.h"\n/* handle */\n')
    _write(root, "src/kernel/nt/handle.h", "#ifndef HANDLE_H\n#define HANDLE_H\n#endif\n")
    _write(root, "src/kernel/nt/mm.c", "/* src copy of mm */\n")
    _write(root, "src/kernel/nt/debugfs_tests.c", "/* not in the Makefile */\n")
    _write(root, "src/kernel/fs/binfmt_pe.c", "/* PE loader */\n")
    _write(root, "src/ntdll/nt_types.h", "/* types */\n")


def make_tree(root, tif_addr32_bit=28, elf_em_extra=None):
    """A minimal kernel tree with every anchor the edits use."""
    _write(root, "Makefile", "all:\n")
    _write(root, "arch/x86/Kconfig",
           "config X86\n\tbool\nif HYPERVISOR_GUEST\nendif # HYPERVISOR_GUEST\n")
    _write(root, "arch/x86/Kbuild", "obj-y += kernel/\n")
    _write(root, "fs/Makefile", "obj-y += open.o\n")
    _write(root, "fs/Kconfig.binfmt", "config BINFMT_ELF\n\tbool\n")
    _write(root, "include/linux/sched.h",
           "struct task_struct {\n"
           "\t/* CPU-specific state of this task: */\n"
           "\tstruct thread_struct\t\tthread;\n"
           "\tint pid;\n};\n")
    _write(root, "kernel/fork.c",
           "static struct task_struct *dup_task_struct(struct task_struct *orig, int node)\n"
           "{\n\tstruct task_struct *tsk;\n\tint err;\n\n"
           "\terr = arch_dup_task_struct(tsk, orig);\n"
           "\tif (err)\n\t\tgoto free_tsk;\n\treturn tsk;\n}\n")
    _write(root, "kernel/exit.c",
           "void __noreturn do_exit(long code)\n{\n"
           "\tstruct task_struct *tsk = current;\n\n\tcode = code;\n}\n")
    _write(root, "arch/x86/include/asm/thread_info.h",
           "#ifndef _ASM_X86_THREAD_INFO_H\n#define _ASM_X86_THREAD_INFO_H\n"
           "#define TIF_BLOCKSTEP\t\t25\t/* set when we want DEBUGCTLMSR_BTF */\n"
           "#define TIF_ADDR32\t\t%d\t/* 32-bit address space on 64 bits */\n"
           "#define _TIF_FORCED_TF\t\tBIT(TIF_FORCED_TF)\n#endif\n" % tif_addr32_bit)
    _write(root, "arch/x86/entry/syscall_64.c",
           "#include <linux/entry-common.h>\n\n"
           "__visible noinstr bool do_syscall_64(struct pt_regs *regs, int nr)\n{\n"
           "\tadd_random_kstack_offset();\n\n"
           "\tnr = syscall_enter_from_user_mode(regs, nr);\n"
           "\treturn true;\n}\n")
    _write(root, "arch/x86/include/asm/syscall.h",
           "static inline int syscall_get_arch(struct task_struct *task)\n{\n"
           "\t/* x32 tasks should be considered AUDIT_ARCH_X86_64. */\n"
           "\treturn AUDIT_ARCH_X86_64;\n}\n")
    _write(root, "include/uapi/linux/audit.h",
           "#define EM_X86_64\t62\n"
           "#define AUDIT_ARCH_X86_64\t(EM_X86_64|__AUDIT_ARCH_64BIT|__AUDIT_ARCH_LE)\n")
    if elf_em_extra is not None:
        _write(root, "include/uapi/linux/elf-em.h",
               "#define EM_X86_64\t62\n#define %s\n" % elf_em_extra)


def snapshot(root):
    out = {}
    for p in sorted(pathlib.Path(root).rglob("*")):
        if p.is_file():
            out[p.relative_to(root).as_posix()] = p.read_bytes()
    return out


def run_main(*args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = kp.main(list(args))
    return rc, buf.getvalue()


class FullPlanTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self._tmp.name)
        self.project = base / "project"
        self.tree = base / "tree"
        make_project(self.project)
        make_tree(self.tree)

    def tearDown(self):
        self._tmp.cleanup()

    def apply(self, allow_todo=True):
        return run_main("--project", str(self.project), "--tree", str(self.tree),
                        *(["--allow-todo"] if allow_todo else []))

    def test_apply_writes_every_edit_and_is_idempotent(self):
        rc, out = self.apply()
        self.assertEqual(rc, 0, out)
        self.assertIn("APPLIED:", out)
        first = snapshot(self.tree)
        rc2, out2 = self.apply()
        self.assertEqual(rc2, 0, out2)
        self.assertEqual(snapshot(self.tree), first, "second run changed the tree")
        self.assertNotIn("[applicable]", out2)

    def test_edits_land_in_the_expected_files(self):
        self.apply()
        t = self.tree
        ti = (t / "arch/x86/include/asm/thread_info.h").read_text()
        self.assertIn("#define TIF_NTABI\t\t29", ti)
        self.assertIn("#define TIF_NTSUSPEND\t30", ti)
        self.assertIn("#define _TIF_NTABI\t\tBIT(TIF_NTABI)", ti)
        sc = (t / "arch/x86/entry/syscall_64.c").read_text()
        self.assertEqual(sc.count("void do_nt_syscall(struct pt_regs *regs, unsigned int ssn);"), 1)
        self.assertLess(sc.index("do_nt_syscall(struct pt_regs"),
                        sc.index("__visible noinstr bool do_syscall_64"))
        # NT branch uses the 6.18 generic entry (IRQs on, seccomp/ptrace/audit)
        self.assertEqual(sc.count("nr = syscall_enter_from_user_mode(regs, nr);"), 2)
        self.assertIn("if (nr != -1) {", sc)
        self.assertIn("syscall_exit_to_user_mode(regs);\n\t\treturn nt_can_sysret(regs);", sc)
        self.assertFalse((t / "arch/x86/entry/nt").exists())
        self.assertIn("late_initcall(nt_banner_initcall);",
                      (t / "arch/x86/nt/kernel_stubs.c").read_text())
        self.assertIn("tsk->nt_thread = NULL;", (t / "kernel/fork.c").read_text())
        self.assertIn("nt_thread_exit_abandon();", (t / "kernel/exit.c").read_text())
        self.assertIn("*nt_thread;", (t / "include/linux/sched.h").read_text())
        self.assertIn('source "arch/x86/Kconfig.nt"', (t / "arch/x86/Kconfig").read_text())
        self.assertIn("obj-$(CONFIG_NT_PERSONALITY) += nt/", (t / "arch/x86/Kbuild").read_text())
        self.assertIn("obj-$(CONFIG_BINFMT_PE)\t+= binfmt_pe.o", (t / "fs/Makefile").read_text())
        self.assertIn("config BINFMT_PE", (t / "fs/Kconfig.binfmt").read_text())
        self.assertIn("AUDIT_ARCH_NTX86_64", (t / "include/uapi/linux/audit.h").read_text())
        self.assertIn("return AUDIT_ARCH_NTX86_64;",
                      (t / "arch/x86/include/asm/syscall.h").read_text())
        self.assertTrue((t / "fs/binfmt_pe.c").is_file())
        self.assertTrue((t / "arch/ntdll/nt_types.h").is_file())
        self.assertEqual((t / "arch/x86/nt/stubs.c").read_bytes(), b"")

    def test_crlf_source_is_copied_as_lf(self):
        self.apply()
        data = (self.tree / "arch/x86/nt/kernel_stubs.c").read_bytes()
        self.assertNotIn(b"\r\n", data)
        self.assertIn(b"void nt_banner_init(void)", data)

    def test_build_closure_copies_needed_headers_only(self):
        self.apply()
        self.assertTrue((self.tree / "arch/x86/nt/handle.c").is_file())
        self.assertTrue((self.tree / "arch/x86/nt/handle.h").is_file())
        self.assertFalse((self.tree / "arch/x86/nt/debugfs_tests.c").exists())

    def test_arch_duplicate_of_a_canonical_file_blocks_the_run(self):
        _write(self.project, "arch/x86/nt/mm.c", "/* stale arch copy of mm */\n")
        rc, out = run_main("--project", str(self.project), "--tree", str(self.tree), "--check")
        self.assertEqual(rc, 1, out)
        self.assertIn("duplicate of src/kernel/nt/mm.c", out)
        rc, out = run_main("--project", str(self.project), "--tree", str(self.tree))
        self.assertEqual(rc, 1, out)
        self.assertFalse((self.tree / "arch/x86/nt").exists(), "a blocked run wrote files")

    def test_canonical_src_copy_is_used_for_shared_names(self):
        self.apply()
        self.assertEqual((self.tree / "arch/x86/nt/mm.c").read_text(), "/* src copy of mm */\n")

    def test_strict_apply_succeeds_with_no_todo_edits(self):
        rc, out = self.apply(allow_todo=False)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("[todo", out)
        self.assertNotIn("enter.c", out)

    def test_check_reports_each_state_and_writes_nothing(self):
        rc, out = run_main("--project", str(self.project), "--tree", str(self.tree), "--check")
        self.assertEqual(rc, 0, out)
        self.assertNotIn("[todo", out)
        self.assertIn("[applicable]", out)
        self.assertFalse((self.tree / "arch/x86/nt").exists(), "--check wrote files")

    def test_check_is_clean_after_apply(self):
        self.apply()
        rc, out = run_main("--project", str(self.project), "--tree", str(self.tree),
                           "--check")
        self.assertEqual(rc, 0, out)
        self.assertNotIn("[applicable]", out)
        self.assertNotIn("[blocked", out)

    def test_missing_anchor_blocks_and_names_the_file(self):
        kconf = self.tree / "arch/x86/Kconfig"
        kconf.write_text("config X86\n\tbool\n")  # no HYPERVISOR_GUEST endif
        before = snapshot(self.tree)
        rc, out = self.apply()
        self.assertEqual(rc, 1)
        self.assertIn("[blocked   ] E", out)
        self.assertIn("arch/x86/Kconfig", out)
        self.assertIn("matches 0 times", out)
        self.assertIn("NOT APPLIED", out)
        self.assertEqual(snapshot(self.tree), before)

    def test_taken_bit_blocks_with_the_gap(self):
        make_tree(self.tree, tif_addr32_bit=29)  # rebuild the tree with 29 in use
        rc, out = self.apply()
        self.assertEqual(rc, 1)
        self.assertIn("TIF_NTABI: bit 29 is already used by TIF_ADDR32", out)
        self.assertIn("(BUILD-RECIPE G4)", out)

    def test_em_collision_blocks_audit_edit(self):
        make_tree(self.tree, elf_em_extra="EM_FAKE\t0x9040")
        rc, out = self.apply()
        self.assertEqual(rc, 1)
        self.assertIn("EM value 0x9040 is already EM_FAKE", out)

    def test_banner_initcall_is_not_appended_twice(self):
        self.apply()
        self.apply()
        text = (self.tree / "arch/x86/nt/kernel_stubs.c").read_text()
        self.assertEqual(text.count("late_initcall(nt_banner_initcall)"), 1)
        self.assertEqual(text.count("#include <linux/init.h>"), 1)


class ArgumentTest(unittest.TestCase):
    def test_not_a_kernel_tree_is_a_usage_error(self):
        with tempfile.TemporaryDirectory() as d, contextlib.redirect_stderr(io.StringIO()):
            rc, _ = run_main("--tree", d, "--check")
        self.assertEqual(rc, 2)

    def test_tree_is_required_without_selftest(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as cm:
            kp.main([])
        self.assertEqual(cm.exception.code, 2)

    def test_selftest_passes(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = kp.selftest()
        self.assertEqual(rc, 0, buf.getvalue())


class PrimitiveTest(unittest.TestCase):
    def test_insert_before_anchor_and_marker_is_idempotent(self):
        with tempfile.TemporaryDirectory() as d:
            _write(d, "a.h", "one\ntwo\n")
            tree = kp.Tree(d)
            e = kp.InsertEdit("X", "a.h", r"^two\n", "inserted\n", "inserted",
                              "t", where="before")
            self.assertEqual(e.run(tree).status, kp.APPLICABLE)
            self.assertEqual(tree.current("a.h"), b"one\ninserted\ntwo\n")

    def test_expect_after_mismatch_blocks(self):
        with tempfile.TemporaryDirectory() as d:
            _write(d, "a.h", "one\ntwo\n")
            e = kp.InsertEdit("X", "a.h", r"^one\n", "x\n", "x", "t",
                              expect_after=r"\{\n")
            r = e.run(kp.Tree(d))
            self.assertEqual(r.status, kp.BLOCKED)
            self.assertIn("text after the anchor", r.message)

    def test_append_adds_newline_when_missing(self):
        with tempfile.TemporaryDirectory() as d:
            _write(d, "m.mk", "x := 1")
            tree = kp.Tree(d)
            kp.AppendEdit("A", "m.mk", "y := 2\n", "y := 2", "t").run(tree)
            self.assertEqual(tree.current("m.mk"), b"x := 1\ny := 2\n")

    def test_todo_names_file_and_gap(self):
        r = kp.TodoEdit("T", "arch/x86/entry/nt/enter.c", "d", "G3", "why").run(
            kp.Tree(tempfile.gettempdir()))
        self.assertEqual(r.status, kp.TODO)
        self.assertIn("arch/x86/entry/nt/enter.c", r.message)
        self.assertIn("G3", r.message)


if __name__ == "__main__":
    unittest.main()
