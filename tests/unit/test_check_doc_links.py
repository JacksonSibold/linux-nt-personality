"""Unit tests for tools/check_doc_links.py."""
import importlib.util
import contextlib
import io
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check_doc_links.py"

_spec = importlib.util.spec_from_file_location("check_doc_links", SCRIPT)
checker = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(checker)


def write(root, rel, text):
    full = pathlib.Path(root, rel)
    full.parent.mkdir(parents=True, exist_ok=True)
    full.write_bytes(text.encode("utf-8"))


class SlugTests(unittest.TestCase):
    def test_github_style_slugs(self):
        self.assertEqual(checker.slugify("Generated file: `ntstatus_table.inc`"),
                         "generated-file-ntstatus_tableinc")
        self.assertEqual(checker.slugify("Other test suites (not run by CI)"),
                         "other-test-suites-not-run-by-ci")
        self.assertEqual(checker.slugify("A [link](x.md) here"), "a-link-here")

    def test_duplicate_headings_are_numbered(self):
        text = "# Top\n\n## Same\n\n## Same\n\n## Same\n"
        self.assertEqual(checker.anchors_in(text),
                         {"top", "same", "same-1", "same-2"})

    def test_headings_in_fences_are_ignored(self):
        text = "```\n# not a heading\n```\n\n~~~\n## also not\n~~~\n\n## real\n"
        self.assertEqual(checker.anchors_in(text), {"real"})

    def test_html_ids_count_as_anchors(self):
        text = '<a id="custom"></a>\n<a name="old-style"></a>\n'
        self.assertEqual(checker.anchors_in(text), {"custom", "old-style"})


class LinkExtractionTests(unittest.TestCase):
    def test_code_spans_and_fences_are_skipped(self):
        text = (
            "[real](a.md)\n"
            "`[code](gone.md)`\n"
            "```\n[fenced](gone.md)\n```\n"
            "![img](pic.png)\n"
            "[ref]: b.md#x\n"
        )
        targets = [t for _, t in checker.iter_links(text)]
        self.assertEqual(targets, ["a.md", "pic.png", "b.md#x"])

    def test_external_and_mailto_links_are_not_checked(self):
        self.assertIsNone(checker.split_target("https://example.com/a"))
        self.assertIsNone(checker.split_target("mailto:a@b.c"))
        self.assertEqual(checker.split_target("a.md#frag"), ("a.md", "frag"))


class TreeCheckTests(unittest.TestCase):
    def test_broken_file_anchor_and_escape_are_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(tmp, "doc.md", "# Doc\n\n[gone](nope.md)\n[bad](#missing)\n"
                                 "[up](../../out.md)\n[ok](#doc)\n")
            problems, nfiles, nlinks = checker.check_tree(tmp, ["doc.md"])
            self.assertEqual(nfiles, 1)
            self.assertEqual(nlinks, 4)
            self.assertEqual([(no, t) for _, no, t, _ in problems],
                             [(3, "nope.md"), (4, "#missing"), (5, "../../out.md")])

    def test_cross_file_anchor_is_resolved(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(tmp, "a.md", "[ok](sub/b.md#target) [bad](sub/b.md#nothing)\n")
            write(tmp, "sub/b.md", "## Target\n")
            problems, _, _ = checker.check_tree(tmp, ["a.md", "sub/b.md"])
            self.assertEqual([p[2] for p in problems], ["sub/b.md#nothing"])

    def test_relative_to_the_linking_file_and_directory_targets(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(tmp, "docs/x/page.md", "[up](../y.md) [dir](../y) [root](/docs/y.md)\n")
            write(tmp, "docs/y.md", "# y\n")
            write(tmp, "docs/y/.keep", "")
            problems, _, _ = checker.check_tree(tmp, ["docs/x/page.md"])
            self.assertEqual(problems, [])

    def test_case_must_match_the_filesystem_spelling(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(tmp, "Real.md", "x\n")
            write(tmp, "link.md", "[wrong](real.md) [right](Real.md)\n")
            problems, _, _ = checker.check_tree(tmp, ["link.md"])
            self.assertEqual([p[2] for p in problems], ["real.md"])

    def test_missing_target_in_fenced_block_is_not_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(tmp, "t.md", "```\n[x](gone.md)\n```\n")
            problems, _, links = checker.check_tree(tmp, ["t.md"])
            self.assertEqual((problems, links), ([], 0))


class SelftestAndCliTests(unittest.TestCase):
    def test_selftest_passes(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(checker.selftest(), 0)
        self.assertIn("selftest ok", out.getvalue())

    def test_repository_has_no_broken_links(self):
        problems, nfiles, _ = checker.check_tree(str(REPO))
        self.assertGreater(nfiles, 0)
        self.assertEqual(
            problems, [],
            "broken documentation links: " + "; ".join(
                f"{rel}:{no} ({target}) {detail}"
                for rel, no, target, detail in problems))

    def test_main_rejects_missing_root(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(checker.main(["--root", str(REPO / "no-such-dir")]), 2)


if __name__ == "__main__":
    unittest.main()
