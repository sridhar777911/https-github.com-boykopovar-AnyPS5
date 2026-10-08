import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_conventions


class DocumentationPolicyTests(unittest.TestCase):
    def test_maintained_guidance_is_allowed(self):
        for path in ("README.md", "CONTRIBUTING.md", "AGENTS.md", "AGENT.md", "docs/README.md", "docs/AGENTS.md", "docs/dev/TESTING.md", "docs/user/GETTING_STARTED.md", "core/relinker/AGENTS.md", "core/libs/prx/libSceAgcDriver/AGENTS.md", "tools/AGENTS.md", ".github/AGENTS.md", ".github/pull_request_template.md"):
            with self.subTest(path=path):
                self.assertTrue(check_conventions.allowed_notes(path))

    def test_transient_notes_and_alternate_agent_rules_remain_disallowed(self):
        for path in ("REPORT.md", "core/relinker/notes.md", "tools/investigation.md", "docs/random.md", "core/AGENT.md", "CLAUDE.md", "docs/dev/GEMINI.md", ".github/copilot-instructions.md", "scratch/AGENTS.md", "3rdparty/AGENTS.md", "build/AGENTS.md"):
            with self.subTest(path=path):
                self.assertFalse(check_conventions.allowed_notes(path))

    def test_non_document_source_is_not_subject_to_notes_policy(self):
        self.assertIsNone(check_conventions.allowed_notes("tools/check_docs.py"))


if __name__ == "__main__":
    unittest.main()
