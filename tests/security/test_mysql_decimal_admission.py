"""Offline fail-closed inventory and child admission checks; no database."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('mysql_live', Path(__file__).with_name('test_mysql_auth_live.py'))
live = importlib.util.module_from_spec(spec)
spec.loader.exec_module(live)

VALID = '<testsuites><testsuite name="MySqlDecimalResultsIntegrationTest" tests="1" failures="0" errors="0" disabled="0" skipped="0"><testcase name="DeclaredSignedBoundsNullAndOwnershipAgreeAcrossProtocols" classname="MySqlDecimalResultsIntegrationTest" status="run" result="completed"/></testsuite></testsuites>'

class DecimalAdmissionTest(unittest.TestCase):
    def test_inventory_rejects_missing_malformed_empty_duplicate_skipped_and_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'result.xml'
            with self.assertRaises(FileNotFoundError):
                live.validate_decimal_xml(path)
            for data in ('bad XML', '<testsuites/>', VALID.replace('tests="1"', 'tests="0"'),
                         VALID.replace('status="run"', 'status="notrun"'),
                         VALID.replace('result="completed"', 'result="skipped"'),
                         VALID.replace('/></testsuite>', '><failure/></testcase></testsuite>'),
                         VALID.replace('skipped="0"', 'skipped="1"'),
                         VALID.replace('</testsuites>', VALID + '</testsuites>')):
                path.write_text(data)
                with self.assertRaises(Exception):
                    live.validate_decimal_xml(path)
            path.write_text(VALID)
            live.validate_decimal_xml(path)

    def test_cleanup_requires_confirmed_absence_even_if_remove_fails(self):
        failed_remove = subprocess.CompletedProcess([], 1)
        for remaining in ('', 'owned-container-id'):
            with patch.object(live.subprocess, 'run', return_value=failed_remove):
                with patch.object(live, 'run', return_value=subprocess.CompletedProcess([], 0, stdout=remaining)) as observe:
                    if remaining:
                        with self.assertRaisesRegex(RuntimeError, 'cleanup failed'):
                            live.cleanup_fixture('owned-name')
                    else:
                        live.cleanup_fixture('owned-name')
                    self.assertEqual(observe.call_args[0][0][4], 'name=^/owned-name$')
        with patch.object(live.subprocess, 'run', return_value=failed_remove):
            with patch.object(live, 'run', side_effect=subprocess.CalledProcessError(1, 'docker')):
                with self.assertRaises(Exception):
                    live.cleanup_fixture('owned-name')

    def test_fixed_inventory_environment_timeout_and_no_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'result.xml'
            def child(args, **kwargs):
                self.assertEqual(kwargs['timeout'], 60)
                self.assertEqual(args[1], '--gtest_filter=' + live.DECIMAL_SUITE + '.' + live.DECIMAL_CASE)
                self.assertEqual(args[2], '--gtest_repeat=1')
                env = kwargs['env']
                self.assertFalse(any(k.startswith('GTEST_') for k in env))
                self.assertEqual(env['ODBCPP_MYSQL_TEST_HOST'], 'localhost')
                self.assertEqual(env['ODBCPP_MYSQL_TEST_PORT'], '12345')
                self.assertEqual(env['ODBCPP_MYSQL_DECIMAL_FIXTURE_ADMITTED'], 'pinned-8.4.11-temporary-decimal')
                path.write_text(VALID)
            with patch.dict(live.os.environ, {'GTEST_FILTER': '*', 'GTEST_REPEAT': '99', 'ODBCPP_MYSQL_TEST_HOST': 'wrong'}):
                with patch.object(live, 'run', side_effect=child) as run:
                    live.run_decimal(Path('/fake/binary'), path, '12345', Path('/fake/ca'))
                    self.assertEqual(run.call_count, 1)
            for error in (subprocess.TimeoutExpired('secret command', 60), subprocess.CalledProcessError(1, 'secret', stderr='private')):
                with patch.object(live, 'run', side_effect=error) as run:
                    with self.assertRaisesRegex(RuntimeError, '^MySQL decimal result proof failed$'):
                        live.run_decimal(Path('/fake/binary'), path, '12345', Path('/fake/ca'))
                    self.assertEqual(run.call_count, 1)

if __name__ == '__main__':
    unittest.main()
