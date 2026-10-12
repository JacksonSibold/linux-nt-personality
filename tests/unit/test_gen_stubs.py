"""Unit tests for build/gen_stubs.py.

gen_stubs.py is a top-level script with no importable functions, so each test
runs it as a subprocess against small temporary .def fixtures and inspects the
generated assembly.
"""
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "build" / "gen_stubs.py"


def run_script(src_text):
    """Run gen_stubs.py on src_text. Returns (returncode, stdout, output_text)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        src = tmp_path / "syscall_table.def"
        dst = tmp_path / "stubs.S"
        src.write_text(src_text)
        proc = subprocess.run(
            [sys.executable, str(SCRIPT), str(src), str(dst)],
            capture_output=True, text=True,
        )
        out_text = dst.read_text() if dst.exists() else None
        return proc.returncode, proc.stdout, out_text


class GenStubsTests(unittest.TestCase):

    def test_generates_one_stub_per_entry(self):
        rc, stdout, out = run_script(
            "NT_SYSCALL(NtClose, 0x0f)\n"
            "NT_SYSCALL(NtCreateFile, 0x55)\n"
        )
        self.assertEqual(rc, 0)
        self.assertIn("Generated 2 stubs", stdout)
        self.assertIn("NtClose:\n", out)
        self.assertIn("NtCreateFile:\n", out)

    def test_stub_body_moves_rcx_loads_ssn_and_syscalls(self):
        rc, _, out = run_script("NT_SYSCALL(NtClose, 0x0f)\n")
        self.assertEqual(rc, 0)
        body = (
            "NtClose:\n"
            "    mov %rcx, %r10\n"
            "    mov $0x0f, %eax\n"
            "    syscall\n"
            "    ret\n"
        )
        self.assertIn(body, out)

    def test_ssn_is_copied_verbatim(self):
        _, _, out = run_script("NT_SYSCALL(NtOpenKey, 0xABcd)\n")
        self.assertIn("mov $0xABcd, %eax", out)

    def test_zw_alias_replaces_leading_nt(self):
        _, _, out = run_script("NT_SYSCALL(NtClose, 0x0f)\n")
        self.assertIn(".globl ZwClose\n", out)
        self.assertIn(".set ZwClose, NtClose\n", out)

    def test_zw_alias_prefixes_names_without_nt(self):
        _, _, out = run_script("NT_SYSCALL(Foo, 0x01)\n")
        self.assertIn(".set ZwFoo, Foo\n", out)

    def test_zw_alias_only_replaces_first_nt(self):
        # "NtNtFoo" -> "ZwNtFoo": only the leading occurrence is rewritten.
        _, _, out = run_script("NT_SYSCALL(NtNtFoo, 0x01)\n")
        self.assertIn(".set ZwNtFoo, NtNtFoo\n", out)

    def test_header_and_directives_present(self):
        _, _, out = run_script("NT_SYSCALL(NtClose, 0x0f)\n")
        self.assertTrue(out.startswith("/* auto-generated syscall stubs"))
        self.assertIn("    .text\n    .align 16\n", out)

    def test_ignores_unrelated_lines(self):
        rc, stdout, out = run_script(
            "#define OTHER 1\n"
            "NT_SYSCALL(NtOpen, 0x10)\n"
        )
        self.assertEqual(rc, 0)
        self.assertIn("Generated 1 stubs", stdout)
        self.assertIn("NtOpen:\n", out)

    def test_matches_even_inside_comments(self):
        # Known limitation: the regex has no comment awareness, so a commented
        # entry still produces a stub. Documented here so it is not silent.
        rc, stdout, out = run_script(
            "// NT_SYSCALL(NtClose, 0x0f)\n"
        )
        self.assertEqual(rc, 0)
        self.assertIn("Generated 1 stubs", stdout)

    def test_empty_input_generates_header_only(self):
        rc, stdout, out = run_script("")
        self.assertEqual(rc, 0)
        self.assertIn("Generated 0 stubs", stdout)
        self.assertNotIn("syscall\n", out)

    def test_missing_arguments_fail(self):
        proc = subprocess.run(
            [sys.executable, str(SCRIPT)], capture_output=True, text=True,
        )
        self.assertNotEqual(proc.returncode, 0)


if __name__ == "__main__":
    unittest.main()
