"""Offline checks of pilot orchestration/evidence only; no product feature oracle."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('pilot', Path(__file__).with_name('ubuntu_development_pilot.py'))
pilot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pilot)


class PilotEvidenceTests(unittest.TestCase):
    def test_exact_ten_ctests_and_no_extra_native_inventory(self):
        listing = {'tests': [{'name': n} for n in pilot.SELECTED]}
        self.assertEqual(len(pilot.inventory_names(listing)), 10)
        for bad in (listing['tests'][:-1], listing['tests'] + [{'name': 'it_redshift_real'}],
                    listing['tests'][:-1] + [listing['tests'][0]]):
            with self.assertRaises(ValueError):
                pilot.inventory_names({'tests': bad})

    def test_google_test_actual_inventory_preserves_parameter_names(self):
        text = 'Running main() from gtest_main.cc\nSuite.\n  Alpha\nTyped/0.  # TypeParam = int\n  Beta\nInst/Params.\n  Gamma/0  # GetParam() = 2\n'
        self.assertEqual(pilot.gtest_names(text), ['Inst/Params.Gamma/0', 'Suite.Alpha', 'Typed/0.Beta'])
        for bad in ('', 'Suite.\n  DISABLED_Alpha\n', 'Suite.\n  Alpha\n  Alpha\n'):
            with self.assertRaises(ValueError):
                pilot.gtest_names(bad)

    def test_xml_requires_real_complete_success_no_skips_errors_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'result.xml'
            path.write_text('<testsuites><testsuite><testcase classname="Suite" name="Alpha" status="run" result="completed"/></testsuite></testsuites>')
            self.assertEqual(pilot.xml_result(path, ['Suite.Alpha'])['count'], 1)
            with self.assertRaises(ValueError):
                pilot.xml_result(path, ['Suite.Alpha', 'Suite.Missing'])
            for body in ('<failure/>', '<error/>', '<skipped/>'):
                path.write_text('<testsuites><testcase classname="Suite" name="Alpha">' + body + '</testcase></testsuites>')
                with self.assertRaises(ValueError):
                    pilot.xml_result(path, ['Suite.Alpha'])
            path.write_text('<testsuites><testcase classname="Suite" name="Alpha" status="notrun"/></testsuites>')
            with self.assertRaises(ValueError):
                pilot.xml_result(path, ['Suite.Alpha'])

    def test_reviewed_head_and_fixed_tracked_inputs(self):
        head = 'a' * 40
        with patch.object(pilot, 'output', side_effect=[head, '', 'tree']):
            self.assertEqual(pilot.source(head)['head'], head)
        for values, expected in (([head], 'b' * 40), ([head], 'branch'), ([head, ' M source.cpp'], head)):
            with patch.object(pilot, 'output', side_effect=values), self.assertRaises(ValueError):
                pilot.source(expected)

    def test_key_digest_binds_profile_dependency_and_source_inputs(self):
        original = {'source': 'a' * 40, 'dependency': '1.2', 'profile': list(pilot.PROFILE), 'runner': 'ubuntu24-x64'}
        for name in original:
            changed = copy.deepcopy(original)
            changed[name] = 'different'
            self.assertNotEqual(pilot.digest(original), pilot.digest(changed))
        self.assertEqual(pilot.digest(original), pilot.digest(dict(reversed(list(original.items())))))

    def test_real_child_failure_retains_original_log_exit_and_measurement(self):
        # Substitute only the GNU time executable on macOS; execute a real failing child.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake_time = root / 'fake-time.py'
            fake_time.write_text('import pathlib, subprocess, sys\npathlib.Path(sys.argv[3]).write_text("Maximum resident set size (kbytes): 123\\n")\nsys.exit(subprocess.call(sys.argv[4:]))\n')
            real_popen = subprocess.Popen
            def launch(cmd, **kwargs):
                return real_popen([sys.executable, str(fake_time), *cmd[1:]], **kwargs)
            with patch.object(pilot, 'EVIDENCE', root / 'evidence'), patch.object(pilot.subprocess, 'Popen', side_effect=launch):
                with self.assertRaises(RuntimeError):
                    pilot.measure('failed', [sys.executable, '-c', 'print("original failure"); raise SystemExit(7)'], 10)
                record = json.loads((pilot.EVIDENCE / 'failed.json').read_text())
                self.assertEqual(record['exit'], 7)
                self.assertEqual(record['gnu_time_max_child_rss_kib'], 123)
                self.assertIn('original failure', (pilot.EVIDENCE / 'failed.log').read_text())

    def test_timeout_kills_owned_process_group_and_records_failed_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            process = unittest.mock.Mock(pid=12345)
            process.wait.side_effect = [subprocess.TimeoutExpired('child', 1), 0]
            with patch.object(pilot, 'EVIDENCE', Path(directory)), patch.object(pilot.subprocess, 'Popen', return_value=process), patch.object(pilot.os, 'killpg') as kill:
                with self.assertRaises(RuntimeError):
                    pilot.measure('timeout', ['child'], 1)
                kill.assert_called_once_with(12345, pilot.signal.SIGKILL)
                record = json.loads((Path(directory) / 'timeout.json').read_text())
                self.assertEqual(record['exit'], 124)
                self.assertTrue(record['timed_out'])

    def test_artifact_bound_and_exclusion_of_final_binaries(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(pilot, 'EVIDENCE', Path(directory)):
            root = Path(directory)
            (root / 'identity.json').write_text('{}')
            pilot.package()
            self.assertIn('identity.json', json.loads((root / 'evidence-index.json').read_text()))
            (root / 'final.so').write_bytes(b'binary')
            with self.assertRaises(ValueError):
                pilot.package()
            (root / 'final.so').unlink()
            with (root / 'oversized.log').open('wb') as f:
                f.seek(20 * 1024 * 1024)
                f.write(b'x')
            with self.assertRaises(ValueError):
                pilot.package()


if __name__ == '__main__':
    unittest.main()
