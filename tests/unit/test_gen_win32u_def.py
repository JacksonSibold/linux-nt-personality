"""Unit tests for build/gen_win32u_def.py: the export list of win32u.dll."""
import importlib.util
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gen_win32u_def", ROOT / "build" / "gen_win32u_def.py")
gen = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gen)

W32K = ROOT / "src" / "ntdll" / "win32k"

HEADER = """\
typedef void (*w32k_wake_fn)(uint32_t tid);
/* int w32k_in_comment(void); */
void w32k_init(void);
w32k_handle w32k_get_dc(w32k_handle hwnd);
uint32_t *w32k_bitmap_bits(w32k_handle bitmap, int32_t *w, int32_t *h);
int w32k_bitblt(w32k_handle dst, int32_t x, int32_t y, int32_t w, int32_t h,
                w32k_handle src, int32_t sx, int32_t sy, uint32_t rop);
void w32k_init(void);
"""


class ExportsTest(unittest.TestCase):
    def test_prototypes_only(self):
        self.assertEqual(gen.exports(HEADER),
                         ["w32k_init", "w32k_get_dc", "w32k_bitmap_bits", "w32k_bitblt"])

    def test_render_layout(self):
        self.assertEqual(gen.render(["w32k_a", "w32k_b"]),
                         "LIBRARY win32u\nEXPORTS\n    w32k_a\n    w32k_b\n")

    def test_real_header_exports_are_defined(self):
        names = gen.exports((W32K / "w32k.h").read_text())
        self.assertGreater(len(names), 100)
        self.assertIn("w32k_init_once", names)
        self.assertIn("w32k_get_dc", names)
        sources = "\n".join(p.read_text() for p in sorted(W32K.glob("w32k_*.c")))
        missing = [n for n in names if not re.search(r"^[^\n;]*\b%s\s*\(" % n, sources, re.M)]
        self.assertEqual(missing, [], "declared in w32k.h but defined in no w32k_*.c")


if __name__ == "__main__":
    unittest.main()
