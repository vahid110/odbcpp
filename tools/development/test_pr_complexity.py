#!/usr/bin/env python3
"""Offline unit tests for PR code complexity (Lizard) and duplication (jscpd) checks."""
from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

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

    def test_exemption_comment_suppresses_complexity_warning(self):
        branches = _make_complex_body(27)
        code = f"""// NOLINT(complexity)\nint exempt_complex_fn(int x) {{\n{branches}\n    return -1;\n}}\n"""
        findings = checker.check_source_complexity("test.cpp", code, changed_lines={1, 2}, ccn_limit=25, nloc_limit=100)
        self.assertEqual([], findings)

    def test_lizard_ignore_comment_suppresses_warning(self):
        branches = _make_complex_body(27)
        code = f"""// lizard: ignore\nint exempt_complex_fn2(int x) {{\n{branches}\n    return -1;\n}}\n"""
        findings = checker.check_source_complexity("test.cpp", code, changed_lines={1, 2}, ccn_limit=25, nloc_limit=100)
        self.assertEqual([], findings)


class DuplicationCheckTests(unittest.TestCase):
    def test_duplicate_overlapping_changed_lines_is_flagged(self):
        mock_report = _make_mock_report(tokens=60, lines=15)
        changed_by_file = {
            "src/a.cpp": {15},
            "src/b.cpp": set(),
        }
        findings = checker.evaluate_duplication_report(mock_report, changed_by_file, min_tokens=50)
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
        findings = checker.evaluate_duplication_report(mock_report, changed_by_file, min_tokens=50)
        self.assertEqual([], findings)

    def test_duplicate_below_token_threshold_is_ignored(self):
        mock_report = _make_mock_report(tokens=40, lines=8)
        changed_by_file = {
            "src/a.cpp": {15},
        }
        findings = checker.evaluate_duplication_report(mock_report, changed_by_file, min_tokens=50)
        self.assertEqual([], findings)


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
