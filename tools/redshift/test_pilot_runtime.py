import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from tools.redshift.pilot_preflight import Blocked
from tools.redshift import pilot_runtime as runtime


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name).resolve()
        self.directory.chmod(0o700)

    def run_child(self, script, seconds=2):
        return runtime.bounded_process([sys.executable, '-c', script], env={},
            seconds=seconds, output=self.directory/'output')

    def test_output_is_private_and_not_returned(self):
        result = self.run_child("print('secret-password')")
        self.assertEqual(result.returncode, 0)
        self.assertFalse(result.timed_out)
        self.assertNotIn('secret-password', repr(result))
        self.assertEqual((self.directory/'output').stat().st_mode & 0o777, 0o600)

    def test_timeout_kills_background_group(self):
        marker = str(self.directory/'escaped')
        script = ("import subprocess,time; subprocess.Popen([" + repr(sys.executable) +
                  ",'-c'," + repr("import time,pathlib;time.sleep(2);pathlib.Path(" +
                  repr(marker) + ").touch()") + "]);time.sleep(10)")
        result = self.run_child(script, seconds=1)
        self.assertTrue(result.timed_out)
        import time
        time.sleep(1.2)
        self.assertFalse(Path(marker).exists())

    def test_normal_parent_exit_also_stops_background_child(self):
        marker = str(self.directory/'escaped')
        script = ("import subprocess; subprocess.Popen([" + repr(sys.executable) +
                  ",'-c'," + repr("import time,pathlib;time.sleep(1);pathlib.Path(" +
                  repr(marker) + ").touch()") + "])" )
        self.assertEqual(self.run_child(script).returncode, 0)
        import time
        time.sleep(1.1)
        self.assertFalse(Path(marker).exists())

    def test_sigterm_ignoring_child_cannot_keep_executing(self):
        marker = str(self.directory/'escaped')
        child = ("import signal,time,pathlib;signal.signal(signal.SIGTERM,signal.SIG_IGN);"
                 "time.sleep(3);pathlib.Path(" + repr(marker) + ").touch()")
        script = ("import subprocess,os,time;print(os.getpgrp(),flush=True);"
                  "subprocess.Popen([" + repr(sys.executable) + ",'-c'," + repr(child) +
                  "]);time.sleep(10)")
        self.assertTrue(self.run_child(script, seconds=1).timed_out)
        group_id = int((self.directory/'output').read_text().strip())
        self.assertFalse(runtime._group_alive(group_id))
        self.assertFalse(Path(marker).exists())

    def test_existing_output_is_not_overwritten(self):
        (self.directory/'output').write_text('preserve')
        with self.assertRaisesRegex(Blocked, 'process_unavailable'):
            self.run_child("print('new')")
        self.assertEqual((self.directory/'output').read_text(), 'preserve')

    def report(self, cases):
        path = self.directory/'results.xml'
        path.write_text('<testsuites><testsuite>' + cases + '</testsuite></testsuites>')
        path.chmod(0o600)
        return path

    def case(self, extra='', status='run', result='completed'):
        return f'<testcase classname="Pilot" name="One" status="{status}" result="{result}">{extra}</testcase>'

    def test_exact_inventory_and_failure_sanitization(self):
        result = runtime.test_summary(self.report(self.case('<failure message="password">native-secret</failure>')), ('Pilot.One',))
        self.assertEqual(result, {'cases':['Pilot.One'], 'passed':0, 'failed':1})
        self.assertNotIn('secret', str(result))

    def test_missing_duplicate_and_skipped_cases_block(self):
        for xml, code in [('', 'test_inventory_mismatch'), (self.case()*2, 'test_inventory_mismatch'),
                          (self.case('<skipped/>'), 'incomplete_test_run'),
                          (self.case(status='notrun'), 'incomplete_test_run')]:
            with self.subTest(xml=xml):
                with self.assertRaisesRegex(Blocked, code):
                    runtime.test_summary(self.report(xml), ('Pilot.One',))

    def test_modified_executable_blocks(self):
        with self.assertRaisesRegex(Blocked, 'executable_changed'):
            runtime.verify_executable(Path(sys.executable).resolve(), '0'*64)

    def test_malformed_outcomes_and_duplicate_expected_block(self):
        for extra in ('<error/>', '<properties><failure/></properties>', '<unexpected/>',
                      '<failure><failure/></failure>'):
            with self.subTest(extra=extra):
                with self.assertRaisesRegex(Blocked, 'invalid_test_report'):
                    runtime.test_summary(self.report(self.case(extra)), ('Pilot.One',))
        with self.assertRaisesRegex(Blocked, 'invalid_test_report'):
            runtime.test_summary(self.report(self.case()), ('Pilot.One', 'Pilot.One'))

    def test_persistent_or_unobservable_group_blocks(self):
        with patch.object(runtime, '_group_alive', return_value=True), \
             patch.object(runtime.os, 'killpg', side_effect=PermissionError):
            with self.assertRaisesRegex(Blocked, 'process_cleanup_unverified'):
                runtime._stop_group(type('Process', (), {'pid': 999999})())
        with patch.object(runtime.subprocess, 'run', side_effect=OSError):
            with self.assertRaisesRegex(Blocked, 'process_cleanup_unverified'):
                runtime._group_alive(999999)


if __name__ == '__main__':
    unittest.main()
