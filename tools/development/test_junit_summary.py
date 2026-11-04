#!/usr/bin/env python3
"""Tests for junit_summary.py: exact counts, names, and refusal of incomplete files."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

_spec = importlib.util.spec_from_file_location(
    'junit_summary', Path(__file__).resolve().parent / 'junit_summary.py')
junit_summary = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(junit_summary)

# The shape CTest writes with --output-junit.
CTEST = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="Darwin-c++" tests="5" failures="1" disabled="1" skipped="1">
  <testcase name="test_a" classname="test_a" status="run"/>
  <testcase name="test_b" classname="test_b" status="run"/>
  <testcase name="test_c" classname="test_c" status="fail"><failure message="Failed"/></testcase>
  <testcase name="test_d" classname="test_d" status="notrun"><skipped message="SKIP_RETURN_CODE=77"/></testcase>
  <testcase name="test_e" classname="test_e" status="disabled"><skipped message="Disabled"/></testcase>
</testsuite>
"""

# The shape GoogleTest writes with --gtest_output=xml.
GTEST = """<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="4" failures="1" disabled="1" errors="0" name="AllTests">
  <testsuite name="Suite" tests="4" failures="1" disabled="1" skipped="1" errors="0">
    <testcase name="Passes" status="run" result="completed" classname="Suite"/>
    <testcase name="Fails" status="run" result="completed" classname="Suite"><failure message="x"/></testcase>
    <testcase name="Skips" status="run" result="skipped" classname="Suite"><skipped message="no server"/></testcase>
    <testcase name="DISABLED_Off" status="notrun" result="suppressed" classname="Suite"/>
  </testsuite>
</testsuites>
"""


class JunitSummaryTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def write(self, text):
        path = Path(self.directory.name) / 'result.xml'
        path.write_text(text, encoding='utf-8')
        return path

    def names(self, result, category):
        return [name for name, _ in result[category]]

    def test_ctest_counts_and_names(self):
        result = junit_summary.summarise(self.write(CTEST))
        self.assertEqual(self.names(result, 'passed'), ['test_a', 'test_b'])
        self.assertEqual(self.names(result, 'failed'), ['test_c'])
        self.assertEqual(result['skipped'], [('test_d', 'SKIP_RETURN_CODE=77')])
        self.assertEqual(self.names(result, 'disabled'), ['test_e'])

    def test_gtest_counts_and_names(self):
        result = junit_summary.summarise(self.write(GTEST))
        self.assertEqual(self.names(result, 'passed'), ['Suite.Passes'])
        self.assertEqual(self.names(result, 'failed'), ['Suite.Fails'])
        self.assertEqual(result['skipped'], [('Suite.Skips', 'no server')])
        self.assertEqual(self.names(result, 'disabled'), ['Suite.DISABLED_Off'])

    def test_render_lists_every_case_that_did_not_pass(self):
        text = junit_summary.render('pg unit', junit_summary.summarise(self.write(CTEST)))
        self.assertEqual(text.splitlines(), [
            '**pg unit**: 5 tests: 2 passed, 1 failed, 1 skipped, 1 disabled',
            '- failed: `test_c`',
            '- skipped: `test_d` (`SKIP_RETURN_CODE=77`)',
            '- disabled: `test_e`',
        ])

    def test_render_keeps_each_case_on_one_line(self):
        long_message = 'file.cpp:206\nThe loader kept the image.\n' + 'x' * 300
        text = junit_summary.render('gtest', {
            'passed': [], 'failed': [], 'disabled': [],
            'skipped': [('Suite.Skips', long_message)]})
        lines = text.splitlines()
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[1].startswith('- skipped: `Suite.Skips` (`file.cpp:206 The loader kept the image. x'))
        self.assertTrue(lines[1].endswith('...`)'))
        self.assertLessEqual(len(lines[1]), len('- skipped: `Suite.Skips` (``)') + junit_summary.MAX_DETAIL)

    def test_ctest_not_run_without_skip_code_is_a_failure(self):
        # CTest marks a missing executable, a failed fixture or missing files
        # "notrun", outside failures=, but fails the run for them.
        for message in ('Unable to find executable', 'Fixture dependency failed', 'Required Files Missing'):
            with self.subTest(message=message):
                text = CTEST.replace('message="SKIP_RETURN_CODE=77"', f'message="{message}"')
                result = junit_summary.summarise(self.write(text))
                self.assertEqual(result['failed'], [('test_c', None), ('test_d', message)])
                self.assertEqual(result['skipped'], [])
        text = CTEST.replace('message="SKIP_RETURN_CODE=77"', 'message="SKIP_REGULAR_EXPRESSION_MATCHED"')
        self.assertEqual(junit_summary.summarise(self.write(text))['skipped'],
                         [('test_d', 'SKIP_REGULAR_EXPRESSION_MATCHED')])

    def test_render_neutralises_markdown_and_control_characters(self):
        text = junit_summary.render('pg', {
            'passed': [], 'failed': [], 'disabled': [],
            'skipped': [('Suite.`odd`', 'ping @someone <!-- hide \x1b[31mred\x07')]})
        lines = text.splitlines()
        self.assertEqual(len(lines), 2)
        # Name and message are each one code span, where GitHub renders
        # '@name' and '<!--' literally; no backtick can close a span early.
        self.assertEqual(lines[1], "- skipped: `Suite.'odd'` (`ping @someone <!-- hide [31mred`)")

    def test_root_totals_must_match(self):
        with self.assertRaisesRegex(junit_summary.SummaryError, 'declares 5 tests'):
            junit_summary.summarise(self.write(GTEST.replace('<testsuites tests="4"', '<testsuites tests="5"')))

    def test_errors_without_failures_attribute_are_checked(self):
        text = CTEST.replace('failures="1"', 'errors="2"')
        with self.assertRaisesRegex(junit_summary.SummaryError, 'declares 2 failures'):
            junit_summary.summarise(self.write(text))

    def test_unknown_gtest_result_is_refused(self):
        with self.assertRaisesRegex(junit_summary.SummaryError, 'unknown result'):
            junit_summary.summarise(self.write(GTEST.replace('result="completed" classname="Suite"/>',
                                                             'result="odd" classname="Suite"/>', 1)))

    def test_non_ascii_digits_are_refused(self):
        with self.assertRaisesRegex(junit_summary.SummaryError, 'not a count'):
            junit_summary.summarise(self.write(CTEST.replace('tests="5"', 'tests="²"')))

    def test_main_puts_refusal_in_the_summary_too(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(junit_summary.main(['redshift=/no/such/file.xml']), 1)
        self.assertIn('**redshift**: cannot summarise', out.getvalue())

    def test_truncated_file_is_refused(self):
        # A suite that declares more tests than it holds is not complete.
        with self.assertRaisesRegex(junit_summary.SummaryError, 'declares 6 tests'):
            junit_summary.summarise(self.write(CTEST.replace('tests="5"', 'tests="6"')))

    def test_hidden_failure_is_refused(self):
        with self.assertRaisesRegex(junit_summary.SummaryError, 'declares 2 failures'):
            junit_summary.summarise(self.write(CTEST.replace('failures="1"', 'failures="2"')))

    def test_unknown_status_is_refused(self):
        with self.assertRaisesRegex(junit_summary.SummaryError, 'unknown status'):
            junit_summary.summarise(self.write(CTEST.replace('status="run"/>', 'status="odd"/>', 1)))

    def test_empty_file_is_refused(self):
        with self.assertRaisesRegex(junit_summary.SummaryError, 'no test cases'):
            junit_summary.summarise(self.write('<testsuite tests="0"/>'))

    def test_dtd_is_refused(self):
        hostile = '<?xml version="1.0"?><!DOCTYPE x [<!ENTITY e "y">]><testsuite/>'
        with self.assertRaises(junit_summary.ET.ParseError):
            junit_summary.summarise(self.write(hostile))

    def test_main_reports_failure_for_bad_input(self):
        good = self.write(CTEST)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(junit_summary.main([f'pg={good}', 'redshift=/no/such/file.xml']), 1)
        self.assertIn('**pg**: 5 tests', out.getvalue())
        self.assertIn('**redshift**: cannot summarise', err.getvalue())
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(junit_summary.main(['no-label']), 2)
            self.assertEqual(junit_summary.main([]), 2)


if __name__ == '__main__':
    unittest.main()
