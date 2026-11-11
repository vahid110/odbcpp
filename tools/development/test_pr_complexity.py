#!/usr/bin/env python3
"""Offline unit tests for PR code complexity (Lizard) and duplication (jscpd) checks."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

try:
    import check_pr_complexity as checker
except ImportError:
    from tools.development import check_pr_complexity as checker


def _make_complex_body(count: int = 27) -> str:
    return "\n".join(f"    if (x == {i}) return {i};" for i in range(count))


def _make_mock_report(tokens: int = 60, lines: int = 15) -> dict:
    return {
        "duplicates": [
            {
                "format": "cpp",
                "lines": lines,
                "tokens": tokens,
                "firstFile": {
                    "name": "src/a.cpp",
                    "start": 10,
                    "end": 10 + lines,
                },
                "secondFile": {
                    "name": "src/b.cpp",
                    "start": 50,
                    "end": 50 + lines,
                },
            }
        ]
    }


class ChangedLinesTests(unittest.TestCase):
    def test_parses_unified_diff_hunks(self):
        diff = """--- a/foo.cpp
+++ b/foo.cpp
@@ -10,0 +11,3 @@
+int a = 1;
+int b = 2;
+int c = 3;
@@ -20,2 +23,1 @@
-old_code();
-old_code2();
+new_code();
"""
        lines = checker.parse_changed_lines(diff)
        self.assertEqual({11, 12, 13, 23}, lines)

    def test_parses_deletion_only_hunk(self):
        diff = """--- a/foo.cpp
+++ b/foo.cpp
@@ -15,3 +15,0 @@
-del1();
-del2();
-del3();
"""
        lines = checker.parse_changed_lines(diff)
        self.assertEqual({15}, lines)

    def test_empty_diff_returns_empty_set(self):
        self.assertEqual(set(), checker.parse_changed_lines(""))


class ComplexityCheckTests(unittest.TestCase):
    def test_simple_function_passes(self):
        code = """int add(int a, int b) {
    return a + b;
}
"""
        findings = checker.check_source_complexity("test.cpp", code, changed_lines={1, 2, 3}, ccn_limit=25, nloc_limit=100)
        self.assertEqual([], findings)

    def test_high_ccn_in_changed_lines_is_flagged(self):
        branches = _make_complex_body(27)
        code = f"""int complex_fn(int x) {{\n{branches}\n    return -1;\n}}\n"""
        findings = checker.check_source_complexity("test.cpp", code, changed_lines={1, 2}, ccn_limit=25, nloc_limit=100)
        self.assertTrue(len(findings) >= 1)
        self.assertTrue(any("CCN" in f.reason and f.metric_value > 25 for f in findings))

    def test_high_nloc_in_changed_lines_is_flagged(self):
        body = "\n".join(f"    x += {i};" for i in range(110))
        code = f"""int long_fn(int x) {{\n{body}\n    return x;\n}}\n"""
        findings = checker.check_source_complexity("test.cpp", code, changed_lines={1, 5}, ccn_limit=25, nloc_limit=100)
        self.assertTrue(len(findings) >= 1)
        self.assertTrue(any("NLOC" in f.reason and f.metric_value > 100 for f in findings))

    def test_untouched_complex_function_is_ignored(self):
        branches = _make_complex_body(27)
        code = f"""int legacy_complex_fn(int x) {{\n{branches}\n    return -1;\n}}

int modified_simple_fn(int a) {{
    return a * 2;
}}
"""
        findings = checker.check_source_complexity("test.cpp", code, changed_lines={33, 34, 35}, ccn_limit=25, nloc_limit=100)
        self.assertEqual([], findings)

    def test_deletion_outside_complex_function_does_not_flag_it(self):
        branches = _make_complex_body(27)
        code = f"""// line 1: some comment
int legacy_complex_fn(int x) {{
{branches}
    return -1;
}}
// line 33: simple cleanup
"""
        # Deletion occurred at line 33 (outside legacy_complex_fn)
        findings = checker.check_source_complexity("test.cpp", code, changed_lines={33}, ccn_limit=25, nloc_limit=100)
        self.assertEqual([], findings)


class ExemptionTests(unittest.TestCase):
    def test_comment_immediately_preceding_function_exempts(self):
        code_lines = [
            "// NOLINT(complexity)",
            "int complex_fn(int x) {",
            "    return x;",
            "}",
        ]
        self.assertTrue(checker.has_exemption(code_lines, start_line=2))

    def test_lizard_ignore_preceding_function_exempts(self):
        code_lines = [
            "// lizard: ignore",
            "int complex_fn(int x) {",
            "    return x;",
            "}",
        ]
        self.assertTrue(checker.has_exemption(code_lines, start_line=2))

    def test_comment_on_signature_line_exempts(self):
        code_lines = [
            "int complex_fn(int x) { /* NOLINT(complexity) */",
            "    return x;",
            "}",
        ]
        self.assertTrue(checker.has_exemption(code_lines, start_line=1))

    def test_string_literal_inside_body_does_not_exempt(self):
        code_lines = [
            "int regular_fn(int x) {",
            '    log("NOLINT(complexity)");',
            "    return x;",
            "}",
        ]
        self.assertFalse(checker.has_exemption(code_lines, start_line=1))

    def test_exemption_does_not_leak_from_previous_function(self):
        code_lines = [
            "// NOLINT(complexity)",
            "int func1(int x) {",
            "    return x;",
            "}",
            "int func2(int y) {",
            "    return y;",
            "}",
        ]
        self.assertTrue(checker.has_exemption(code_lines, start_line=2))
        self.assertFalse(checker.has_exemption(code_lines, start_line=5))


class DuplicationCheckTests(unittest.TestCase):
    def test_duplicate_overlapping_changed_lines_is_flagged(self):
        mock_report = _make_mock_report(tokens=60, lines=15)
        changed_by_file = {
            "src/a.cpp": {15},
            "src/b.cpp": set(),
        }
        findings = checker.evaluate_duplication_report(mock_report, changed_by_file, min_tokens=50, root=Path("/repo"))
        self.assertEqual(1, len(findings))
        self.assertEqual("src/a.cpp", findings[0].file)
        self.assertEqual(10, findings[0].line)
        self.assertIn("60 tokens", findings[0].reason)

    def test_duplicate_outside_changed_lines_is_ignored(self):
        mock_report = _make_mock_report(tokens=60, lines=15)
        changed_by_file = {
            "src/a.cpp": {99},
            "src/b.cpp": set(),
        }
        findings = checker.evaluate_duplication_report(mock_report, changed_by_file, min_tokens=50, root=Path("/repo"))
        self.assertEqual([], findings)

    def test_duplicate_below_token_threshold_is_ignored(self):
        mock_report = _make_mock_report(tokens=40, lines=8)
        changed_by_file = {
            "src/a.cpp": {15},
        }
        findings = checker.evaluate_duplication_report(mock_report, changed_by_file, min_tokens=50, root=Path("/repo"))
        self.assertEqual([], findings)

    def test_missing_tools_raises_runtime_error(self):
        with mock.patch("shutil.which", return_value=None):
            with self.assertRaises(RuntimeError) as ctx:
                checker.run_jscpd(Path("/some/path"))
            self.assertIn("Neither 'jscpd' nor 'npx' was found", str(ctx.exception))

    def test_jscpd_missing_report_raises_runtime_error(self):
        with mock.patch("shutil.which", return_value="/usr/bin/npx"):
            with mock.patch("subprocess.run") as mock_run:
                mock_run.return_value = mock.Mock(returncode=1, stderr="jscpd crashed", stdout="")
                with self.assertRaises(RuntimeError) as ctx:
                    checker.run_jscpd(Path("/some/path"))
                self.assertIn("did not generate a report file", str(ctx.exception))


class CollectChangedFilesTests(unittest.TestCase):
    def test_identifies_new_and_modified_files(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            subprocess.run(["git", "init"], cwd=str(root), check=True, stdout=subprocess.DEVNULL)
            subprocess.run(["git", "config", "user.name", "Test"], cwd=str(root), check=True)
            subprocess.run(["git", "config", "user.email", "test@test.com"], cwd=str(root), check=True)

            # Initial commit with existing.cpp
            existing = root / "existing.cpp"
            existing.write_text("int a = 1;\nint b = 2;\nint c = 3;\n")
            subprocess.run(["git", "add", "existing.cpp"], cwd=str(root), check=True)
            subprocess.run(["git", "commit", "-m", "init"], cwd=str(root), check=True, stdout=subprocess.DEVNULL)

            # Modify existing.cpp by deleting a line, and add new.cpp
            existing.write_text("int a = 1;\nint c = 3;\n")
            new_file = root / "new.cpp"
            new_file.write_text("int x = 42;\n")

            changed_by_file, source_files = checker.collect_changed_files(
                root, base_ref="HEAD", head_ref=None, paths=[]
            )

            self.assertIn("existing.cpp", source_files)
            self.assertIn("new.cpp", source_files)

            # new.cpp has all lines changed
            self.assertEqual({1}, changed_by_file["new.cpp"])

            # existing.cpp only has deletion line changed, NOT all 2 lines!
            self.assertEqual({1}, changed_by_file["existing.cpp"])


class IntegrationTests(unittest.TestCase):
    def test_supported_extensions(self):
        self.assertTrue(checker.is_supported_source_file("foo.cpp"))
        self.assertTrue(checker.is_supported_source_file("foo.h"))
        self.assertTrue(checker.is_supported_source_file("foo.hpp"))
        self.assertTrue(checker.is_supported_source_file("foo.cc"))
        self.assertTrue(checker.is_supported_source_file("foo.cxx"))
        self.assertTrue(checker.is_supported_source_file("foo.py"))
        self.assertTrue(checker.is_supported_source_file("foo.cs"))
        self.assertFalse(checker.is_supported_source_file("foo.md"))
        self.assertFalse(checker.is_supported_source_file("foo.json"))
        self.assertFalse(checker.is_supported_source_file("foo.xml"))
        self.assertFalse(checker.is_supported_source_file("CMakeLists.txt"))


if __name__ == "__main__":
    unittest.main()
