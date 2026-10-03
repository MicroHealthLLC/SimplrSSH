"""
AI_RULES.md is the one rules file: CLAUDE.md must stay a symbolic link to it, AGENTS.md must
point to it, and nothing else may name CLAUDE.md as the place where the rules live.
"""

import os
import subprocess
import unittest

import repo

# Files that may mention CLAUDE.md: they describe the link itself
MAY_MENTION_LINK = {"AI_RULES.md", "AGENTS.md", "CLAUDE.md", "test/guards/test_rules_file.py"}


class RulesFile(unittest.TestCase):
    def test_ai_rules_is_a_regular_file(self):
        path = repo.path("AI_RULES.md")
        self.assertTrue(os.path.isfile(path) and not os.path.islink(path))

    def test_claude_md_links_to_ai_rules(self):
        path = repo.path("CLAUDE.md")
        self.assertTrue(os.path.islink(path), "CLAUDE.md must be a symlink to AI_RULES.md, not a copy")
        self.assertEqual(os.readlink(path), "AI_RULES.md")

    def test_symlink_committed_as_link(self):
        # Mode 120000 = symlink in git (a checkout with core.symlinks=false would show a file)
        out = subprocess.run(["git", "ls-files", "-s", "CLAUDE.md"], cwd=repo.ROOT,
                             capture_output=True, text=True).stdout
        if not out:
            self.skipTest("not a git checkout")
        self.assertTrue(out.startswith("120000 "), out)

    def test_agents_md_points_to_ai_rules(self):
        self.assertIn("(AI_RULES.md)", repo.read("AGENTS.md"))

    def test_nothing_else_names_claude_md(self):
        out = subprocess.run(["git", "ls-files"], cwd=repo.ROOT, capture_output=True, text=True).stdout
        if not out:
            self.skipTest("not a git checkout")
        offenders = []
        for name in out.splitlines():
            full = repo.path(name)
            if name in MAY_MENTION_LINK or os.path.islink(full) or not os.path.isfile(full):
                continue
            try:
                with open(full, encoding="utf-8") as f:
                    if "CLAUDE.md" in f.read():
                        offenders.append(name)
            except UnicodeDecodeError:
                continue
        self.assertEqual(offenders, [], "refer to AI_RULES.md, the rules file")


if __name__ == "__main__":
    unittest.main()
