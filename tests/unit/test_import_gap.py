"""Tests for tools/import_gap.py.

Every PE file is built in memory by build_pe(); no binary is read from disk
except the repository's own text sources. Fixtures cover import-by-name,
ordinal imports, delay-load imports (RVA and VA attributes), PE32 and PE32+,
and kernel-mode .sys files that import from ntoskrnl.exe.
"""
import contextlib
import importlib.util
import io
import json
import pathlib
import shutil
import struct
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "import_gap.py"

_spec = importlib.util.spec_from_file_location("import_gap", SCRIPT)
ig = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ig)
pe_info = ig.pe_info

SEC_RVA = 0x1000
HDR = 0x400


def build_pe(imports=(), delayed=(), pe32=False, delay_va=False):
    """Build a PE with one .idata section holding the import and delay tables.

    imports / delayed: sequences of (dll, entries); an entry is a function name
    (str) or an ordinal (int). delay_va selects VA-based delay descriptors.
    """
    thunk_size = 4 if pe32 else 8
    thunk_fmt = "I" if pe32 else "Q"
    ordinal_flag = 0x80000000 if pe32 else 1 << 63
    image_base = 0x400000 if pe32 else 0x140000000
    blob = bytearray()

    def alloc(n, align=4):
        while len(blob) % align:
            blob.append(0)
        off = len(blob)
        blob.extend(b"\x00" * n)
        return off

    def put_cstring(text):
        off = alloc(len(text) + 1, 1)
        blob[off:off + len(text)] = text.encode("ascii")
        return off

    def thunk_array(entries):
        names = []
        for e in entries:
            if isinstance(e, int):
                names.append(ordinal_flag | e)
            else:
                h = alloc(2 + len(e) + 1, 2)
                blob[h + 2:h + 2 + len(e)] = e.encode("ascii")
                names.append(SEC_RVA + h)
        arr = alloc(thunk_size * (len(entries) + 1), 8)
        for k, value in enumerate(names):
            struct.pack_into("<" + thunk_fmt, blob, arr + k * thunk_size, value)
        return arr

    imp_off = alloc(20 * (len(imports) + 1)) if imports else 0
    for i, (dll, entries) in enumerate(imports):
        name = put_cstring(dll)
        ilt = thunk_array(entries)
        iat = thunk_array(entries)
        struct.pack_into("<5I", blob, imp_off + 20 * i, SEC_RVA + ilt, 0, 0,
                         SEC_RVA + name, SEC_RVA + iat)

    dl_off = alloc(32 * (len(delayed) + 1)) if delayed else 0
    for i, (dll, entries) in enumerate(delayed):
        name = put_cstring(dll)
        ilt = thunk_array(entries)
        iat = thunk_array(entries)
        handle = alloc(thunk_size, thunk_size)
        if delay_va:
            attrs = 0
            conv = lambda rva: (image_base + rva) & 0xFFFFFFFF   # noqa: E731
        else:
            attrs = 1
            conv = lambda rva: rva                                 # noqa: E731
        struct.pack_into("<8I", blob, dl_off + 32 * i, attrs, conv(SEC_RVA + name),
                         conv(SEC_RVA + handle), conv(SEC_RVA + iat),
                         conv(SEC_RVA + ilt), 0, 0, 0)

    raw_size = (len(blob) + 0x1FF) & ~0x1FF
    padded = bytes(blob) + b"\x00" * (raw_size - len(blob))
    vsize = len(blob)
    image_size = 0x1000 + ((vsize + 0xFFF) & ~0xFFF)

    opt = bytearray(struct.pack("<HBBIIII", 0x10B if pe32 else 0x20B, 14, 0, 0x200, 0, 0, 0))
    opt += struct.pack("<I", 0x1000)                                     # BaseOfCode
    opt += struct.pack("<II", 0x1000, image_base) if pe32 else struct.pack("<Q", image_base)
    opt += struct.pack("<II" + "H" * 6 + "I", 0x1000, 0x200, 6, 0, 0, 0, 6, 0, 0)
    opt += struct.pack("<IIIHH", image_size, HDR, 0, 3, 0x0140)
    opt += struct.pack("<IIII", 0x100000, 0x1000, 0x100000, 0x1000) if pe32 else \
        struct.pack("<QQQQ", 0x100000, 0x1000, 0x100000, 0x1000)
    opt += struct.pack("<II", 0, 16)
    dirs = bytearray(16 * 8)
    if imports:
        struct.pack_into("<II", dirs, 1 * 8, SEC_RVA + imp_off, 20 * (len(imports) + 1))
    if delayed:
        struct.pack_into("<II", dirs, 13 * 8, SEC_RVA + dl_off, 32 * (len(delayed) + 1))
    opt += dirs
    opt_size = len(opt)

    hdr = bytearray(HDR)
    hdr[0:2] = b"MZ"
    struct.pack_into("<I", hdr, 0x3C, 0x40)
    hdr[0x40:0x44] = b"PE\x00\x00"
    struct.pack_into("<HHIIIHH", hdr, 0x44, 0x14C if pe32 else 0x8664, 1, 1700000000,
                     0, 0, opt_size, 0x0022)
    hdr[0x58:0x58 + opt_size] = opt
    sec = struct.pack("<8sIIIIIIHHI", b".idata", vsize, SEC_RVA, raw_size, HDR,
                      0, 0, 0, 0, 0xC0000040)
    hdr[0x58 + opt_size:0x58 + opt_size + 40] = sec
    return bytes(hdr) + padded


def parse(data):
    info = pe_info.parse_pe(data)
    return info, ig.read_imports(data, info)


def write_tree(root, files):
    for rel, content in files.items():
        path = pathlib.Path(root) / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            path.write_text(content, encoding="utf-8")


SERVICES_MD = """# NT service handler inventory

| SSN | Service | Argc | Status | Handler file |
|---|---|---:|---|---|
| 0x0055 | `NtCreateFile` | 11 | IMPLEMENTED | `src/kernel/nt/io.c` |
| 0x0001 | `NtFoo` | 1 | STUB-NOT-IMPLEMENTED | `src/kernel/nt/dispatch.c` |
| 0x0002 | `NtBar` | 2 | IMPLEMENTED (userspace) / STUB-NOT-IMPLEMENTED (kernel) | `x.c` |
"""

FAKE_ROOT_FILES = {
    "src/ntdll/kernel32.def": "LIBRARY kernel32\nEXPORTS\n    ExitProcess\n    WriteFile ; comment\n",
    "src/ntdll/ntdll.def": ("LIBRARY ntdll\nEXPORTS\n    NtCreateFile\n    ZwCreateFile=NtCreateFile\n"
                            "    NtFoo\n    NtBar\n    RtlInitUnicodeString\n"),
    "src/ntdll/syscall_table.def": "/* EXPORTS in a comment only */\nint x;\n",
    "src/kernel/nt/io.c": ("#include \"x.h\"\nNTSTATUS IoCreateDevice(void *a)\n{\n    return 0;\n}\n"
                           "static int Hidden(void)\n{\n    return 1;\n}\n"
                           "/* NTSTATUS IoFakeInComment(void) */\n"),
    "src/kernel/nt/io_test.c": "NTSTATUS IoTestOnly(void)\n{\n    return 0;\n}\n",
    "docs/status/SERVICES.md": SERVICES_MD,
}


def fake_root():
    tmp = tempfile.mkdtemp(prefix="import_gap_root_")
    write_tree(tmp, FAKE_ROOT_FILES)
    return tmp


class PeFixtureParsingTests(unittest.TestCase):
    def test_import_by_name_and_ordinal_pe32_and_pe32plus(self):
        for pe32 in (False, True):
            with self.subTest(pe32=pe32):
                data = build_pe(imports=[("KERNEL32.dll", ["ExitProcess", 5])], pe32=pe32)
                info, imports = parse(data)
                self.assertEqual(info["format"], "PE32" if pe32 else "PE32+")
                self.assertEqual(imports, [("KERNEL32.dll", "ExitProcess", False),
                                           ("KERNEL32.dll", "#5", False)])

    def test_delay_load_rva_and_va_attributes(self):
        for delay_va in (False, True):
            with self.subTest(delay_va=delay_va):
                data = build_pe(delayed=[("d3d11.dll", ["D3D11CreateDevice"])],
                                delay_va=delay_va)
                _info, imports = parse(data)
                self.assertEqual(imports, [("d3d11.dll", "D3D11CreateDevice", True)])

    def test_mixed_import_and_delay(self):
        data = build_pe(imports=[("user32.dll", ["MessageBoxW"])],
                        delayed=[("ws2_32.dll", ["connect"])])
        _info, imports = parse(data)
        self.assertEqual(imports, [("user32.dll", "MessageBoxW", False),
                                   ("ws2_32.dll", "connect", True)])

    def test_no_import_directory_gives_no_entries(self):
        _info, imports = parse(build_pe())
        self.assertEqual(imports, [])

    def test_thunk_name_outside_file_is_an_error(self):
        sections = [{"va": 0x1000, "vsize": 0x100, "rawsize": 0x100, "rawptr": 0}]
        data = struct.pack("<I", 0x9000) + b"\x00" * 0xFC
        with self.assertRaisesRegex(pe_info.PEError, "import name RVA 0x9000"):
            ig.read_thunk_names(data, sections, 0x1000, True)

    def test_import_directory_outside_file_is_an_error(self):
        sections = [{"va": 0x1000, "vsize": 0x100, "rawsize": 0x100, "rawptr": 0}]
        with self.assertRaisesRegex(pe_info.PEError, "import directory RVA"):
            ig.read_import_directory(b"\x00" * 0x100, sections, 0x9000, True)

    def test_module_with_bad_import_table_is_skipped_not_crashed(self):
        tmp = tempfile.mkdtemp(prefix="import_gap_bad_")
        try:
            good = build_pe(imports=[("user32.dll", ["MessageBoxW"])])
            # Point the import directory at an RVA outside the file.
            bad = bytearray(good)
            struct.pack_into("<I", bad, 0x40 + 4 + 20 + 112 + 8, 0x9000)
            path = pathlib.Path(tmp) / "bad.exe"
            path.write_bytes(bytes(bad))
            with self.assertRaisesRegex(ig.ImportGapError, "import directory RVA"):
                ig.read_module(path, "bad.exe", 1 << 20)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class SurfaceParsingTests(unittest.TestCase):
    def test_def_exports_alias_comment_and_library(self):
        lib, exports = ig.parse_def_exports(
            "LIBRARY kernel32\nEXPORTS\n    A ; trailing\n    ZwX=NtX\n\n    B @3 NONAME\n")
        self.assertEqual(lib, "kernel32")
        self.assertEqual(exports, {"A", "ZwX", "B"})

    def test_def_without_exports_is_not_a_module(self):
        self.assertIsNone(ig.parse_def_exports("int x; /* EXPORTS */\n"))

    def test_services_all_areas_must_be_implemented(self):
        services = ig.parse_services(SERVICES_MD)
        self.assertEqual(services, {"NtCreateFile": True, "NtFoo": False, "NtBar": False})

    def test_kernel_surface_ignores_static_tests_and_comments(self):
        root = fake_root()
        try:
            names = ig.kernel_surface_names(root)
        finally:
            shutil.rmtree(root, ignore_errors=True)
        self.assertEqual(names, {"IoCreateDevice"})

    def test_kernel_surface_regex_needs_a_family_prefix(self):
        self.assertIsNotNone(ig.KERNEL_DEF_RE.match("NTSTATUS MmMapIoSpace(void *a)"))
        self.assertIsNone(ig.KERNEL_DEF_RE.match("int helper(void)"))


class ClassificationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = fake_root()
        cls.surface = ig.load_surface(cls.root)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def classify(self, key, func, shipped=frozenset()):
        return ig.classify_function(key, func, self.surface, set(shipped))

    def test_user_mode_statuses(self):
        self.assertEqual(self.classify("kernel32", "ExitProcess"), "provided")
        self.assertEqual(self.classify("kernel32", "NoSuchApi"), "missing-function")
        self.assertEqual(self.classify("d3d11", "D3D11CreateDevice"), "missing-dll")
        self.assertEqual(self.classify("d3d11", "D3D11CreateDevice", ["d3d11"]), "bundled")
        self.assertEqual(self.classify("kernel32", "#4"), "ordinal")

    def test_nt_services_use_the_service_classes(self):
        self.assertEqual(self.classify("ntdll", "NtCreateFile"), "provided")
        self.assertEqual(self.classify("ntdll", "ZwCreateFile"), "provided")
        self.assertEqual(self.classify("ntdll", "NtFoo"), "not-implemented")
        self.assertEqual(self.classify("ntdll", "RtlInitUnicodeString"), "provided")

    def test_kernel_mode_imports_use_the_kernel_surface(self):
        self.assertEqual(self.classify("ntoskrnl", "IoCreateDevice"), "provided")
        self.assertEqual(self.classify("ntoskrnl", "IoNotDefined"), "missing-function")
        self.assertEqual(self.classify("ntoskrnl", "#2"), "ordinal")
        self.assertEqual(self.classify("hal", "HalMakeAnything"), "missing-function")

    def test_api_set_names_resolve_against_all_exports(self):
        self.assertEqual(self.classify("api-ms-win-core-foo-l1-1-0", "ExitProcess"), "provided")
        self.assertEqual(self.classify("api-ms-win-core-foo-l1-1-0", "Nope"), "missing-function")

    def test_categories(self):
        cases = {
            "d3d11": "graphics", "dxgi": "graphics", "xaudio2_9": "audio",
            "dinput8": "input", "xinput1_4": "input", "ws2_32": "networking",
            "ole32": "com", "bcrypt": "crypto", "user32": "window",
            "shell32": "shell", "kernel32": "core", "vcruntime140": "core",
            "api-ms-win-core-winrt-string-l1-1-0": "com",
            "api-ms-win-core-synch-l1-2-0": "core",
            "ntoskrnl": "anti-cheat/driver", "fltmgr.sys": "anti-cheat/driver",
            "nvlddmkm.sys": "anti-cheat/driver", "easyanticheat_x64": "anti-cheat/driver",
            "somethingelse": "other",
        }
        for key, expected in cases.items():
            with self.subTest(key=key):
                self.assertEqual(ig.dll_category(key), expected)

    def test_advapi32_crypto_functions_are_crypto(self):
        self.assertEqual(ig.function_category("advapi32", "CryptAcquireContextW"), "crypto")
        self.assertEqual(ig.function_category("advapi32", "RegOpenKeyExW"), "core")

    def test_real_repository_surface(self):
        surface = ig.load_surface(REPO)
        self.assertEqual(ig.classify_function("ntdll", "NtCreateFile", surface, set()), "provided")
        self.assertIn("ntdll", surface["dlls"])


class ReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = fake_root()
        cls.surface = ig.load_surface(cls.root)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def modules(self):
        def mod(path, file, imports):
            return {"path": path, "file": file, "sha256": "0" * 64, "size": 1,
                    "format": "PE32+", "machine": "x86-64 (AMD64)", "imports": imports}
        return [
            mod("game.exe", "game.exe", [("KERNEL32.dll", "ExitProcess", False),
                                         ("d3d11.dll", "D3D11CreateDevice", True),
                                         ("helper.dll", "Help", False)]),
            mod("sub/a.dll", "a.dll", [("d3d11.dll", "D3D11CreateDevice", False),
                                       ("ws2_32.dll", "connect", False)]),
            mod("drv.sys", "drv.sys", [("ntoskrnl.exe", "IoCreateDevice", False),
                                       ("ntoskrnl.exe", "IoNope", False)]),
            mod("helper.dll", "helper.dll", [("KERNEL32.dll", "WriteFile", False)]),
        ]

    def test_ranking_orders_by_importing_modules(self):
        report = ig.build_report(self.modules(), [], self.surface)
        order = [d["key"] for d in report["dlls"]]
        self.assertEqual(order[0], "d3d11")
        self.assertEqual(report["dlls"][0]["modules"], 2)
        self.assertEqual([d["rank"] for d in report["dlls"]], list(range(1, len(order) + 1)))

    def test_summary_and_statuses(self):
        report = ig.build_report(self.modules(), [{"path": "x", "reason": "r"}], self.surface)
        dlls = {d["key"]: d for d in report["dlls"]}
        self.assertEqual(dlls["d3d11"]["status"], "missing-dll")
        self.assertEqual(dlls["helper"]["status"], "bundled")
        self.assertEqual(dlls["kernel32"]["status"], "provided")
        self.assertEqual(dlls["ntoskrnl"]["status"], "partial")
        self.assertEqual(report["summary"]["modules_read"], 4)
        self.assertEqual(report["summary"]["modules_skipped"], 1)
        self.assertEqual(report["summary"]["functions_delay_load"], 1)
        self.assertEqual(report["summary"]["dlls_bundled"], 1)
        ntos = dlls["ntoskrnl"]["functions"]
        self.assertEqual({f["name"]: f["status"] for f in ntos},
                         {"IoCreateDevice": "provided", "IoNope": "missing-function"})

    def test_categories_count_modules_with_gap(self):
        report = ig.build_report(self.modules(), [], self.surface)
        cats = {c["category"]: c for c in report["categories"]}
        self.assertEqual(cats["graphics"]["modules_with_gap"], 2)
        self.assertEqual(cats["anti-cheat/driver"]["gap_functions"], 1)

    def test_report_is_deterministic(self):
        a = ig.build_report(self.modules(), [], self.surface)
        b = ig.build_report(self.modules(), [], self.surface)
        self.assertEqual(json.dumps(a), json.dumps(b))

    def test_markdown_render_has_header_and_escapes_pipes(self):
        mods = self.modules()
        mods[0]["imports"].append(("odd|name.dll", "F|x", False))
        report = ig.build_report(mods, [], self.surface)
        text = ig.render_markdown(report, 0)
        self.assertIn("Generated by tools/import_gap.py. Do not edit by hand.", text)
        self.assertIn("## Missing and partial DLLs, ranked by importing modules", text)
        self.assertIn("odd\\|name", text)
        self.assertIn("more in the JSON report", text)


class CommandLineTests(unittest.TestCase):
    def run_main(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ig.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_directory_scan_writes_reports(self):
        root = fake_root()
        inputs = tempfile.mkdtemp(prefix="import_gap_in_")
        try:
            write_tree(inputs, {
                "game.exe": build_pe(imports=[("KERNEL32.dll", ["ExitProcess"]),
                                              ("d3d11.dll", ["D3D11CreateDevice"])],
                                     delayed=[("ws2_32.dll", ["connect"])]),
                "sub/drv.sys": build_pe(imports=[("ntoskrnl.exe", ["IoCreateDevice"])]),
                "notes.dll": b"plain text, not a PE file",
                "readme.txt": "ignored, wrong extension",
            })
            out_md = pathlib.Path(root) / "out" / "r.md"
            out_json = pathlib.Path(root) / "out" / "r.json"
            code, stdout, _ = self.run_main([inputs, "--root", root,
                                             "--out-md", str(out_md),
                                             "--out-json", str(out_json)])
            self.assertEqual(code, 0)
            self.assertIn("2 module(s) read, 1 skipped", stdout)
            report = json.loads(out_json.read_text(encoding="utf-8"))
            self.assertEqual(report["summary"]["modules_read"], 2)
            self.assertEqual(report["skipped"][0]["path"], "notes.dll")
            self.assertEqual({i["path"] for i in report["inputs"]}, {"game.exe", "sub/drv.sys"})
            self.assertTrue(out_md.read_text(encoding="utf-8").startswith("# Game import gap"))
        finally:
            shutil.rmtree(root, ignore_errors=True)
            shutil.rmtree(inputs, ignore_errors=True)

    def test_no_readable_pe_returns_one(self):
        root = fake_root()
        inputs = tempfile.mkdtemp(prefix="import_gap_none_")
        try:
            write_tree(inputs, {"junk.exe": b"MZ but nothing else"})
            code, _out, err = self.run_main([inputs, "--root", root, "--no-write"])
            self.assertEqual(code, 1)
            self.assertIn("no PE file could be read", err)
        finally:
            shutil.rmtree(root, ignore_errors=True)
            shutil.rmtree(inputs, ignore_errors=True)

    def test_missing_path_is_a_usage_error(self):
        with self.assertRaises(SystemExit):
            self.run_main(["Z:/definitely/not/here.exe", "--no-write"])

    def test_no_inputs_is_a_usage_error(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_main([])
        self.assertEqual(cm.exception.code, 2)

    def test_list_file_input(self):
        root = fake_root()
        inputs = tempfile.mkdtemp(prefix="import_gap_list_")
        try:
            exe = pathlib.Path(inputs) / "one.sys"
            exe.write_bytes(build_pe(imports=[("ntoskrnl.exe", ["IoCreateDevice"])]))
            listing = pathlib.Path(inputs) / "list.txt"
            listing.write_text(f"# comment\n{exe}\n", encoding="utf-8")
            code, _o, _e = self.run_main(["--list", str(listing), "--root", root, "--no-write"])
            self.assertEqual(code, 0)
        finally:
            shutil.rmtree(root, ignore_errors=True)
            shutil.rmtree(inputs, ignore_errors=True)

    def test_write_ntoskrnl_surface_to_fake_root(self):
        root = fake_root()
        try:
            code, out, _ = self.run_main(["--write-ntoskrnl-surface", "--root", root])
            self.assertEqual(code, 0)
            text = (pathlib.Path(root) / "docs" / "status" / "NTOSKRNL-SURFACE.md").read_text(
                encoding="utf-8")
            self.assertIn("- `IoCreateDevice`", text)
            self.assertNotIn("IoTestOnly", text)
        finally:
            shutil.rmtree(root, ignore_errors=True)

    def test_committed_kernel_surface_is_current(self):
        committed = (REPO / "docs" / "status" / "NTOSKRNL-SURFACE.md").read_text(encoding="utf-8")
        listed = set()
        for line in committed.splitlines():
            if line.startswith("- `") and line.endswith("`"):
                listed.add(line[3:-1])
        self.assertEqual(listed, ig.kernel_surface_names(REPO),
                         "run: python tools/import_gap.py --write-ntoskrnl-surface")


if __name__ == "__main__":
    unittest.main()
