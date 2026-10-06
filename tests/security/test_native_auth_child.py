import unittest
import xml.etree.ElementTree as ET
import hashlib
import json
from pathlib import Path
import tempfile
from unittest.mock import Mock, patch
from tools.redshift import native_auth_child as child
from tools.redshift.native_auth_child import CASE, KEYS, completed_case, selected_environment


def selection():
    result = {key: 'selected' for key in KEYS}
    for key in ('DB_CA_FILE', 'CREDENTIALS_FILE', 'CONFIG_FILE'):
        result[key] = '/private/selected/' + key
    return result


def report():
    suite, case = CASE.split('.')
    return (f'<testsuites tests="1" failures="0" errors="0" disabled="0">'
            f'<testsuite tests="1" failures="0" errors="0" disabled="0">'
            f'<testcase classname="{suite}" name="{case}" status="run" result="completed"/>'
            '</testsuite></testsuites>').encode()


class SelectionAndCompletion(unittest.TestCase):
    def test_closed_environment(self):
        env = selected_environment(selection())
        self.assertEqual(env['ODBCPP_REDSHIFT_NATIVE_AUTH_ADMISSION'],
                         'provisioned-native-acquisition-login001')
        self.assertNotIn('HOME', env)
        self.assertNotIn('AWS_PROFILE', env)
        self.assertNotIn('HTTPS_PROXY', env)
        self.assertNotIn('ODBCPP_AUTH_SDK_OFFLINE_FIXTURE', env)

    def test_missing_extra_and_nontext_selection(self):
        for mutation in ('missing', 'extra', 'nontext'):
            spec = selection()
            if mutation == 'missing': del spec['PROFILE']
            if mutation == 'extra': spec['PASSWORD'] = 'forbidden'
            if mutation == 'nontext': spec['PROFILE'] = 1
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                selected_environment(spec)

    def test_path_length_and_control_refusal(self):
        for key, value in (('PROFILE', 'a' * 257), ('PROFILE', 'a\n'),
                           ('PROFILE', ''), ('CONFIG_FILE', 'relative')):
            spec = selection(); spec[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                selected_environment(spec)

    def test_exact_completed_case(self):
        self.assertEqual(completed_case(report()), {'case': CASE, 'passed': 1})

    def test_skip_failure_wrong_inventory_and_counts(self):
        for bad in (report().replace(b'result="completed"', b'result="skipped"'),
                    report().replace(b'failures="0"', b'failures="1"', 1),
                    report().replace(b'tests="1"', b'tests="2"', 1),
                    report().replace(b'VerifiedTlsLogin', b'WrongCase'),
                    report().replace(b'/>', b'><skipped/></testcase>')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                completed_case(bad)

    def test_declarations_oversize_empty_and_partial_refused(self):
        for bad in (b'', b'x' * (1024 * 1024 + 1), report()[:-1],
                    b'<!DOCTYPE x>' + report(), b'<?instruction x?>' + report()):
            with self.subTest(size=len(bad)), self.assertRaises((ValueError, ET.ParseError)):
                completed_case(bad)


class Selector:
    def __init__(self): self.live = False
    def __enter__(self): return self
    def __exit__(self, *unused): pass
    def register(self, *unused): self.live = True
    def unregister(self, *unused): self.live = False
    def get_map(self): return {1: True} if self.live else {}
    def select(self, timeout): return [(Mock(fd=99, fileobj=99), 1)]


class ProcessControls(unittest.TestCase):
    def exercise(self, *, times, read_error=None, poll=None, kill_error=None,
                 wait_error=None, close_error=None):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            executable = base / 'not-executed'
            executable.write_bytes(b'hash-selected mock executable')
            process = Mock()
            process.stdout.fileno.return_value = 99
            process.poll.return_value = poll
            process.kill.side_effect = kill_error
            process.wait.return_value = 0
            process.wait.side_effect = wait_error
            process.stdout.close.side_effect = close_error
            def launch(*args, **kwargs):
                (base / 'tests.xml').write_bytes(report())
                return process
            with patch.object(child.time, 'monotonic', side_effect=times), \
                 patch.object(child.subprocess, 'Popen', side_effect=launch) as spawn, \
                 patch.object(child.selectors, 'DefaultSelector', Selector), \
                 patch.object(child.os, 'set_blocking'), \
                 patch.object(child.os, 'read', return_value=b'', side_effect=read_error):
                if (read_error or kill_error or wait_error or close_error
                        or times[-1] >= 65):
                    with self.assertRaises(ValueError):
                        child.run_selected_child(executable,
                            hashlib.sha256(executable.read_bytes()).hexdigest(),
                            selection(), base)
                    self.assertFalse((base / 'local-result.json').exists())
                else:
                    self.assertEqual(child.run_selected_child(executable,
                        hashlib.sha256(executable.read_bytes()).hexdigest(),
                        selection(), base), {'case': CASE, 'passed': 1})
                self.assertEqual(spawn.call_count, 1)
                status = json.loads((base / 'child-status.json').read_text())
                self.assertFalse(status['remote_cleanup_verified'])
                return status, process

    def test_timely_completion(self):
        status, _ = self.exercise(times=[0, 1, 2, 3, 4, 5], poll=0)
        self.assertEqual((status['reason'], status['returncode']), ('completed', 0))

    def test_successful_wait_at_original_deadline_refuses(self):
        status, _ = self.exercise(times=[0, 1, 2, 3, 65], poll=0)
        self.assertEqual((status['reason'], status['returncode']), ('deadline', 0))

    def test_cleanup_crossing_original_deadline_refuses(self):
        status, _ = self.exercise(times=[0, 1, 2, 3, 4, 66], poll=0)
        self.assertEqual(status['reason'], 'deadline')

    def test_read_exception_kill_exit_race_still_records_wait(self):
        status, process = self.exercise(times=[0, 1, 2],
            read_error=OSError('synthetic read'), kill_error=ProcessLookupError())
        self.assertEqual((status['reason'], status['returncode']), ('execution_failure', 0))
        process.wait.assert_called_once_with(timeout=1)

    def test_kill_and_wait_failure_leave_exit_unknown(self):
        status, process = self.exercise(times=[0, 1, 2],
            read_error=OSError(), kill_error=PermissionError(), wait_error=OSError())
        self.assertEqual(status['reason'], 'local_cleanup_unverified')
        self.assertIsNone(status['returncode'])
        process.stdout.close.assert_called_once()

    def test_pipe_close_exception_records_noncompleted_status(self):
        status, _ = self.exercise(times=[0, 1, 2, 3, 4], poll=0,
                                  close_error=OSError())
        self.assertEqual(status['reason'], 'local_cleanup_unverified')

    def test_poll_failure_still_attempts_kill_and_wait(self):
        process = Mock(); process.poll.side_effect = OSError()
        process.wait.return_value = 0
        reason, status = child.cleanup_child(process, 'completed', None)
        self.assertEqual((reason, status), ('local_cleanup_unverified', 0))
        process.kill.assert_called_once()
        process.wait.assert_called_once_with(timeout=1)


if __name__ == '__main__':
    unittest.main()
