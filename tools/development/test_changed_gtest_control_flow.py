#!/usr/bin/env python3
"""Focused offline contracts for the changed-GoogleTest source check."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import check_changed_gtest_control_flow as check


class ChangedControlTests(unittest.TestCase):
    def findings(self, new, old=""):
        return check.check_change(old, new)[0]

    def test_all_four_unbraced_controls_are_rejected(self):
        for text in ["if (ok) EXPECT_TRUE(ok);", "for (auto x : xs) ASSERT_EQ(1, x);",
                     "while (pending) ADD_FAILURE();", "if (ok) {} else FAIL();"]:
            with self.subTest(text=text):
                self.assertTrue(any("explicit braces" in f.reason for f in self.findings(text)))

    def test_braced_readable_controls_are_accepted(self):
        text = """if (ok) {
  EXPECT_TRUE(ok);
} else {
  ASSERT_FALSE(ok);
}
for (auto x : xs) {
  SCOPED_TRACE(x);
}
while (pending) {
  GTEST_SKIP();
}
"""
        self.assertEqual([], self.findings(text))

    def test_multiline_header_and_changed_body_use_whole_context(self):
        old = "if (call(\n    first, second))\n  EXPECT_EQ(1, value);\n"
        new = old.replace("EXPECT_EQ(1", "EXPECT_EQ(2")
        result = self.findings(new, old)
        self.assertTrue(any(f.line == 1 and "explicit braces" in f.reason for f in result))

    def test_outer_unbraced_nested_control_is_not_hidden_by_inner_braces(self):
        text = "for (auto x : xs)\n  if (x) {\n    EXPECT_TRUE(x);\n  }\n"
        self.assertTrue(any(f.control == "for" and "explicit braces" in f.reason for f in self.findings(text)))

    def test_else_if_assertion_chain_requires_explicit_else_body(self):
        text = "if (a) {\n  EXPECT_TRUE(a);\n} else if (b) {\n  EXPECT_TRUE(b);\n}\n"
        self.assertTrue(any(f.control == "else" and "explicit braces" in f.reason for f in self.findings(text)))

    def test_multiple_complete_controls_on_one_line_are_rejected(self):
        text = "for (auto x : xs) { EXPECT_EQ(0xa5, x); } for (auto y : ys) { EXPECT_EQ(0xa5, y); }"
        result = self.findings(text)
        self.assertTrue(any("readable line" in f.reason for f in result))

    def test_inline_compound_assertion_body_is_not_readable(self):
        self.assertTrue(any("separate lines" in f.reason for f in self.findings("if (ok) { EXPECT_TRUE(ok); }")))

    def test_constexpr_and_nested_condition_groups(self):
        text = "if constexpr (predicate([] { return x; }, pair(1, 2))) {\n  EXPECT_TRUE(ok);\n}\n"
        self.assertEqual([], self.findings(text))

    def test_range_initializer_and_streamed_diagnostic(self):
        text = "for (int i : {1, 2, 3})\n  EXPECT_EQ(i, value) << message(1, 2);\n"
        self.assertTrue(any("explicit braces" in f.reason for f in self.findings(text)))

    def test_literals_comments_and_raw_delimiters_are_not_controls(self):
        text = '''// if (x) EXPECT_TRUE(x);
/* for (;;) ASSERT_TRUE(x); */
const auto a = "while (x) EXPECT_TRUE(x);";
const auto b = R"tag(if (x) ASSERT_TRUE(x);
for (;;) FAIL();)tag";
const auto c = '\\'';
for (auto x : xs) emit("EXPECT_TRUE(x)");
'''
        self.assertEqual([], self.findings(text))

    def test_raw_literal_in_assertion_does_not_hide_real_control(self):
        self.assertTrue(any("explicit braces" in f.reason for f in self.findings(
            'if (ok) EXPECT_EQ(R"_(else for ASSERT_FALSE(x))_", value);')))

    def test_numeric_separators_and_character_braces(self):
        self.assertEqual([], self.findings("if (count > 1'000) {\n  EXPECT_EQ('{', value);\n}\n"))

    def test_unchanged_historical_body_is_not_audit_scope(self):
        old = "if (old) EXPECT_TRUE(old);\nvoid f() {}\n"
        self.assertEqual([], self.findings(old + "int added = 1;\n", old))

    def test_new_duplicate_of_historical_bad_body_is_in_scope(self):
        old = "if (old) EXPECT_TRUE(old);\n"
        self.assertTrue(self.findings(old + old, old))

    def test_comment_only_edit_on_historical_control_stays_quiet(self):
        old = "if (old) EXPECT_TRUE(old); // before\n"
        self.assertEqual([], self.findings(old.replace("before", "after"), old))

    def test_comment_hunk_does_not_hide_a_separate_new_bad_control(self):
        old = "if (old) EXPECT_TRUE(old); // before\nint separator = 0;\n"
        new = old.replace("before", "after") + "while (pending) ASSERT_TRUE(pending);\n"
        result = self.findings(new, old)
        self.assertTrue(result)
        self.assertEqual({3}, {f.line for f in result})

    def test_non_assertion_gtest_macro_still_needs_a_braced_body(self):
        self.assertTrue(any("explicit braces" in f.reason for f in self.findings(
            "for (auto x : xs) SCOPED_TRACE(x);\n")))

    def test_multiline_comment_removal_is_not_a_historical_audit(self):
        old = "if (old) /* first\nsecond */ EXPECT_TRUE(old);\n"
        self.assertEqual([], self.findings("if (old) EXPECT_TRUE(old);\n", old))

    def test_removed_only_comment_does_not_select_next_historical_control(self):
        old = "// old note\nif (old) EXPECT_TRUE(old);\n"
        self.assertEqual([], self.findings(old.split("\n", 1)[1], old))

    def test_removed_braces_select_the_surviving_body(self):
        old = "if (ok) {\n  EXPECT_TRUE(ok);\n}\n"
        self.assertTrue(any("explicit braces" in f.reason for f in self.findings(
            "if (ok)\n  EXPECT_TRUE(ok);\n", old)))

    def test_whitespace_join_that_breaks_readable_body_is_detected(self):
        old = "if (ok) {\n  EXPECT_TRUE(ok);\n}\n"
        self.assertTrue(self.findings("if (ok) { EXPECT_TRUE(ok); }\n", old))

    def test_changed_macro_definition_requires_manual_review(self):
        result = self.findings("#define CHECK(x) if (x) EXPECT_TRUE(x)\n")
        self.assertTrue(any(f.manual_review and f.control == "preprocessor" for f in result))

    def test_preprocessor_literal_does_not_invent_macro_code(self):
        self.assertEqual([], self.findings('#define TEXT "EXPECT_TRUE(x)"\n'))

    def test_unsupported_body_changed_inside_unchanged_header_requires_manual_review(self):
        old = "if (ok) switch (x) {\n  case 1: EXPECT_EQ(1, x);\n}\n"
        self.assertTrue(any(f.manual_review for f in self.findings(old.replace("EXPECT_EQ(1", "EXPECT_EQ(2"), old)))

    def test_unsupported_attribute_requires_manual_review_not_a_pass(self):
        self.assertTrue(any(f.manual_review for f in self.findings(
            "if (ok) [[likely]] {\n  EXPECT_TRUE(ok);\n}\n")))

    def test_unbalanced_changed_scope_requires_manual_review(self):
        self.assertTrue(any(f.manual_review for f in self.findings("if (ok) {\n EXPECT_TRUE(ok);\n")))


class GitDiffCliTests(unittest.TestCase):
    def test_working_untracked_committed_and_literal_path_scopes(self):
        script = Path(check.__file__).resolve()
        with tempfile.TemporaryDirectory(prefix="odbcpp-gtest-control-") as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            file = root / "changed test.cpp"
            file.write_text("if (old) EXPECT_TRUE(old);\n")
            subprocess.run(["git", "-C", str(root), "add", "--", file.name], check=True)
            commit = ["git", "-C", str(root), "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "--quiet", "-m", "fixture"]
            subprocess.run(commit, check=True)
            base = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
            file.write_text(file.read_text() + "while (new_value) ASSERT_TRUE(new_value);\n")
            command = [sys.executable, "-B", str(script), "--base", base, "--json"]
            result = subprocess.run(command + [file.name], cwd=root, capture_output=True, text=True)
            self.assertEqual(1, result.returncode)
            self.assertEqual([2], sorted({f["line"] for f in json.loads(result.stdout)["files"][0]["findings"]}))
            new = root / "untracked.cpp"
            new.write_text("for (;;) EXPECT_TRUE(ok);\n")
            self.assertEqual(1, subprocess.run(command + [new.name], cwd=root, capture_output=True).returncode)
            subprocess.run(["git", "-C", str(root), "add", "--", file.name], check=True)
            subprocess.run(commit, check=True)
            head = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
            file.write_text("// unrelated working bytes\n")
            result = subprocess.run(command + ["--head", head, file.name], cwd=root, capture_output=True, text=True)
            self.assertEqual(1, result.returncode)
            self.assertTrue(json.loads(result.stdout)["findings"])


if __name__ == "__main__":
    unittest.main()
