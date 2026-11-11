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


def _init_git_repo(root: Path) -> None:
    subprocess.run(["git", "init"], cwd=str(root), check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["git", "config", "user.name", "Test"], cwd=str(root), check=True)
    subprocess.run(["git", "config", "user.email", "test@test.com"], cwd=str(root), check=True)


def _make_mock_report(tokens: int = 60, lines: int = 15) -> dict:
    return {
        "statistics": {"total": {"duplicates": 1}},
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
        ],
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
        self.assertEqual({11, 12, 13, 23}, set(lines))

    def test_parses_added_lines_starting_with_plus_plus(self):
        diff = """--- a/foo.cpp
+++ b/foo.cpp
@@ -5,0 +6,2 @@
++++i;
+return i;
"""
        lines = checker.parse_changed_lines(diff)
        self.assertEqual({6, 7}, set(lines))

    def test_parses_deletion_only_hunk_anchor(self):
        diff = """--- a/foo.cpp
+++ b/foo.cpp
@@ -15,3 +14,0 @@
-del1();
-del2();
-del3();
"""
        lines = checker.parse_changed_lines(diff)
        self.assertEqual(set(), set(lines))
        self.assertEqual({(14, 15)}, lines.deleted_anchors)

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

    def test_deletion_between_functions_does_not_flag_preceding_legacy_function(self):
        branches = _make_complex_body(27)
        code = f"""int legacy_a(int x) {{\n{branches}\n    return 0;\n}}\n\nint func_b(int y) {{\n    return y;\n}}\n"""
        changed = checker.ChangedLines(added_lines=set(), deleted_anchors={(30, 31)})
        findings = checker.check_source_complexity("test.cpp", code, changed_lines=changed, ccn_limit=25, nloc_limit=100)
        self.assertEqual([], findings)

    def test_deletion_inside_complex_function_is_flagged(self):
        branches = _make_complex_body(27)
        code = f"""int legacy_a(int x) {{\n{branches}\n    return 0;\n}}\n"""
        changed = checker.ChangedLines(added_lines=set(), deleted_anchors={(5, 6)})
        findings = checker.check_source_complexity("test.cpp", code, changed_lines=changed, ccn_limit=25, nloc_limit=100)
        self.assertTrue(len(findings) >= 1)
        self.assertTrue(any("CCN" in f.reason for f in findings))


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

    def test_string_literal_on_signature_line_does_not_exempt(self):
        code_lines = [
            'int func(const char* s = "// lizard: ignore") {',
            "    return 0;",
            "}",
        ]
        self.assertFalse(checker.has_exemption(code_lines, start_line=1))

    def test_comment_above_template_exempts(self):
        code_lines = [
            "// NOLINT(complexity)",
            "template <typename T>",
            "int complex_template(T x) {",
            "    return 0;",
            "}",
        ]
        self.assertTrue(checker.has_exemption(code_lines, start_line=3))

    def test_comment_above_attribute_exempts(self):
        code_lines = [
            "// NOLINT(complexity)",
            "[[nodiscard]]",
            "int complex_nodiscard(int x) {",
            "    return 0;",
            "}",
        ]
        self.assertTrue(checker.has_exemption(code_lines, start_line=3))

    def test_comment_above_python_decorator_exempts(self):
        code_lines = [
            "# NOLINT(complexity)",
            "@property",
            "def complex_prop(self):",
            "    return 0",
        ]
        self.assertTrue(checker.has_exemption(code_lines, start_line=3))

    def test_preprocessor_directive_stops_exemption_search(self):
        code_lines = [
            "// NOLINT(complexity)",
            "#include <map>",
            "int regular_fn(int x) {",
            "    return x;",
            "}",
        ]
        self.assertFalse(checker.has_exemption(code_lines, start_line=3))

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

    def test_duplicate_at_exact_token_threshold_is_ignored(self):
        # 50 tokens is <= min_tokens (threshold is <= 50 tokens), must not be flagged
        mock_report = _make_mock_report(tokens=50, lines=10)
        changed_by_file = {
            "src/a.cpp": {15},
        }
        findings = checker.evaluate_duplication_report(mock_report, changed_by_file, min_tokens=50, root=Path("/repo"))
        self.assertEqual([], findings)

    def test_duplicate_exceeding_token_threshold_is_flagged(self):
        # 51 tokens > 50 tokens, must be flagged
        mock_report = _make_mock_report(tokens=51, lines=11)
        changed_by_file = {
            "src/a.cpp": {15},
        }
        findings = checker.evaluate_duplication_report(mock_report, changed_by_file, min_tokens=50, root=Path("/repo"))
        self.assertEqual(1, len(findings))

    def test_evaluate_duplication_report_rejects_missing_duplicates(self):
        with self.assertRaises(ValueError) as ctx:
            checker.evaluate_duplication_report({}, {})
        self.assertIn("missing 'duplicates'", str(ctx.exception))

    def test_evaluate_duplication_report_rejects_missing_statistics(self):
        with self.assertRaises(ValueError) as ctx:
            checker.evaluate_duplication_report({"duplicates": []}, {})
        self.assertIn("missing 'statistics'", str(ctx.exception))

    def test_evaluate_duplication_report_rejects_malformed_entry(self):
        bad_report = {"statistics": {}, "duplicates": [{"lines": 10}]}
        with self.assertRaises(ValueError) as ctx:
            checker.evaluate_duplication_report(bad_report, {})
        self.assertIn("missing integer 'tokens'", str(ctx.exception))

    def test_missing_tools_raises_runtime_error(self):
        with mock.patch("pathlib.Path.is_file", return_value=False):
            with mock.patch("shutil.which", return_value=None):
                with self.assertRaises(RuntimeError) as ctx:
                    checker.run_jscpd(Path("/some/path"))
                self.assertIn("Neither locked 'tools/ci/duplication/node_modules/.bin/jscpd'", str(ctx.exception))

    def test_jscpd_missing_report_raises_runtime_error(self):
        with mock.patch("pathlib.Path.is_file", return_value=True):
            with mock.patch("os.access", return_value=True):
                with mock.patch("subprocess.run") as mock_run:
                    mock_run.return_value = mock.Mock(returncode=1, stderr="jscpd crashed", stdout="")
                    with self.assertRaises(RuntimeError) as ctx:
                        checker.run_jscpd(Path("/some/path"))
                    self.assertIn("did not generate a report file", str(ctx.exception))

    def test_find_locked_jscpd_bin_and_config_resolution(self):
        repo_root = Path(__file__).resolve().parents[2]
        # In a random temporary directory without tools/ci, it resolves via repo fallback
        with tempfile.TemporaryDirectory() as tmpdir:
            td = Path(tmpdir)
            locked_bin = checker._find_locked_jscpd_bin(td)
            expected_bin = repo_root / "tools" / "ci" / "duplication" / "node_modules" / ".bin" / "jscpd"
            if expected_bin.is_file():
                self.assertEqual(expected_bin.resolve(), locked_bin)

            cfg = checker._find_jscpd_config(td)
            expected_cfg = repo_root / "tools" / "ci" / "duplication" / ".jscpd.json"
            self.assertEqual(expected_cfg.resolve(), cfg)

        # When files are completely absent, both helpers return None
        with mock.patch("pathlib.Path.is_file", return_value=False):
            self.assertIsNone(checker._find_locked_jscpd_bin(Path("/nonexistent")))
            self.assertIsNone(checker._find_jscpd_config(Path("/nonexistent")))

    def test_real_jscpd_binary_detects_duplicate_fixture(self):
        repo_root = Path(__file__).resolve().parents[2]
        local_bin = repo_root / "tools" / "ci" / "duplication" / "node_modules" / ".bin" / "jscpd"
        if not local_bin.is_file():
            self.skipTest("Locked jscpd binary not installed in worktree")

        with tempfile.TemporaryDirectory() as tmpdir:
            td = Path(tmpdir)
            block = "\n".join(f"    int compute_var_{i} = calculate_value({i}) + {i * 2};" for i in range(12))
            file_a = td / "a.cpp"
            file_b = td / "b.cpp"
            file_a.write_text(f"void computeA() {{\n{block}\n}}\n")
            file_b.write_text(f"void computeB() {{\n{block}\n}}\n")

            report = checker.run_jscpd(td, min_tokens=50, timeout_seconds=15)
            changed_by_file = {
                "a.cpp": set(range(1, 20)),
                "b.cpp": set(),
            }
            findings = checker.evaluate_duplication_report(report, changed_by_file, min_tokens=50, root=td)
            self.assertTrue(len(findings) >= 1)
            self.assertEqual("a.cpp", findings[0].file)
            self.assertEqual("DUPLICATION", findings[0].metric)


class CollectChangedFilesTests(unittest.TestCase):
    def test_identifies_new_and_modified_files(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            _init_git_repo(root)

            # Initial commit with existing.cpp
            existing = root / "existing.cpp"
            existing.write_text("int a = 1;\nint b = 2;\nint c = 3;\n")
            subprocess.run(["git", "add", "existing.cpp"], cwd=str(root), check=True)
            subprocess.run(["git", "commit", "-m", "init"], cwd=str(root), check=True, stdout=subprocess.DEVNULL)

            # Modify existing.cpp by deleting line 2, and add new.cpp
            existing.write_text("int a = 1;\nint c = 3;\n")
            new_file = root / "new.cpp"
            new_file.write_text("int x = 42;\n")

            changed_by_file, source_files = checker.collect_changed_files(
                root, base_ref="HEAD", head_ref=None, paths=[]
            )

            self.assertIn("existing.cpp", source_files)
            self.assertIn("new.cpp", source_files)

            # new.cpp has all lines changed
            self.assertEqual({1}, set(changed_by_file["new.cpp"]))

            # existing.cpp only has deletion anchor, no added lines
            self.assertEqual(set(), set(changed_by_file["existing.cpp"]))
            self.assertEqual({(1, 2)}, changed_by_file["existing.cpp"].deleted_anchors)

    def test_head_ref_reads_committed_bytes_ignoring_working_tree(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            _init_git_repo(root)

            # Commit 1 (base): simple function
            src = root / "test.cpp"
            src.write_text("int func() {\n    return 0;\n}\n")
            subprocess.run(["git", "add", "test.cpp"], cwd=str(root), check=True)
            subprocess.run(["git", "commit", "-m", "base"], cwd=str(root), check=True, stdout=subprocess.DEVNULL)

            # Commit 2 (head): complex function
            branches = _make_complex_body(27)
            src.write_text(f"int func() {{\n{branches}\n    return 0;\n}}\n")
            subprocess.run(["git", "add", "test.cpp"], cwd=str(root), check=True)
            subprocess.run(["git", "commit", "-m", "head commit"], cwd=str(root), check=True, stdout=subprocess.DEVNULL)

            # Uncommitted dirty working tree: overwrite with simple function again
            src.write_text("int func() {\n    return 0;\n}\n")

            # Running with --head HEAD must analyze the committed head commit (high CCN), NOT the dirty working tree!
            changed_by_file, source_files = checker.collect_changed_files(
                root, base_ref="HEAD~1", head_ref="HEAD", paths=[]
            )
            findings = checker.run_complexity_checks(
                root, source_files, changed_by_file, ccn_limit=25, nloc_limit=100, head_ref="HEAD"
            )
            self.assertTrue(len(findings) >= 1)
            self.assertTrue(any("CCN" in f.reason for f in findings))


class MainCliTests(unittest.TestCase):
    def test_main_returns_zero_on_pass(self):
        with mock.patch("check_pr_complexity.collect_changed_files", return_value=({}, [])):
            self.assertEqual(0, checker.main(["--skip-complexity", "--skip-duplication"]))

    def test_main_returns_one_on_finding(self):
        finding = checker.Finding(
            file="test.cpp", line=1, metric="CCN", metric_value=30, threshold=25, reason="high ccn"
        )
        with mock.patch("check_pr_complexity.collect_changed_files", return_value=({"test.cpp": checker.ChangedLines()}, ["test.cpp"])):
            with mock.patch("check_pr_complexity.run_complexity_checks", return_value=[finding]):
                self.assertEqual(1, checker.main(["--skip-duplication"]))

    def test_main_returns_one_on_error_finding(self):
        with mock.patch("check_pr_complexity.collect_changed_files", side_effect=RuntimeError("git failure")):
            self.assertEqual(1, checker.main([]))

    def test_normalize_repo_path_relative_and_absolute(self):
        root = Path("/repo/root")
        self.assertEqual("src/file.cpp", checker.normalize_repo_path("/repo/root/src/file.cpp", root))
        self.assertEqual("src/file.cpp", checker.normalize_repo_path("src/file.cpp", root))


class IntegrationTests(unittest.TestCase):
    def test_supported_extensions(self):
        for ext in (".c", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".inl", ".ipp", ".tpp", ".py", ".cs"):
            self.assertTrue(checker.is_supported_source_file(f"foo{ext}"))
        self.assertFalse(checker.is_supported_source_file("foo.md"))
        self.assertFalse(checker.is_supported_source_file("foo.json"))
        self.assertFalse(checker.is_supported_source_file("foo.xml"))
        self.assertFalse(checker.is_supported_source_file("CMakeLists.txt"))


if __name__ == "__main__":
    unittest.main()
