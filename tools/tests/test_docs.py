import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_docs


class DocumentationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.document = self.root / "README.md"

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def findings(self, text):
        self.write("README.md", text)
        return check_docs.check_document(self.root, self.document)

    def test_local_paths_fragments_and_encoded_spaces(self):
        self.write("docs/My Guide.md", "# API & ABI\n## Behavior\n## Behavior\n")
        self.assertEqual(self.findings("[guide](docs/My%20Guide.md#api--abi)\n[again](<docs/My Guide.md#behavior-1>)\n[folder](docs/)\n[query](docs/My%20Guide.md?raw=1#behavior)"), [])

    def test_missing_target_and_heading_report_lines(self):
        self.write("docs/guide.md", "# Valid\n")
        errors = self.findings("[bad](missing.md)\n[heading](docs/guide.md#absent)")
        self.assertEqual([item[1] for item in errors], [1, 2])
        self.assertIn("Missing local target", errors[0][2])
        self.assertIn("Missing Markdown heading", errors[1][2])

    def test_external_urls_fences_comments_and_inline_code_are_ignored(self):
        text = "[web](https://example.invalid/missing)\n[email](mailto:someone@example.invalid)\n```md\n[bad](missing.md)\n````\n~~~\n[bad](missing.md)\n~~~\n`[bad](missing.md)`\n<!--\n[bad](missing.md)\n-->\n"
        self.assertEqual(self.findings(text), [])

    def test_local_reference_definition_and_self_fragment(self):
        self.write("docs/guide.md", "# Guide\n")
        self.assertEqual(self.findings("# Start\n[self](#start)\n[guide][g]\n[g]: docs/guide.md#guide"), [])
        self.assertIn("Missing local target", self.findings("[g]: missing.md")[0][2])

    def test_links_cannot_escape_repository_or_symlink_outside(self):
        self.assertIn("escapes repository", self.findings("[bad](../outside.md)")[0][2])
        with tempfile.TemporaryDirectory() as outside:
            target = Path(outside) / "external.md"
            target.write_text("# External\n", encoding="utf-8")
            (self.root / "external.md").symlink_to(target)
            self.assertIn("escapes repository", self.findings("[bad](external.md)")[0][2])

    def test_scope_excludes_dependencies_builds_and_nested_checkouts(self):
        self.write("README.md", "# Readme\n")
        included = self.write("core/relinker/AGENTS.md", "# Guidance\n")
        for name in ("3rdparty/example/README.md", "build/README.md", "contribution/README.md", "core/relinker/investigation.md"):
            self.write(name, "[bad](missing.md)")
        files, errors = check_docs.check(self.root)
        self.assertEqual(files, sorted([self.document, included]))
        self.assertEqual(errors, [])

    def test_invalid_utf8_document_is_reported(self):
        self.document.write_bytes(b"\xff")
        self.assertIn("not valid UTF-8", check_docs.check_document(self.root, self.document)[0][2])

    def test_duplicate_headings_match_generated_suffixes(self):
        self.assertEqual(check_docs.heading_ids("# Item\n# Item\n# Item-1\n# Item\n```md\n# Hidden\n```"), {"item", "item-1", "item-1-1", "item-2"})


if __name__ == "__main__":
    unittest.main()
