"""Offline orchestration safety checks; these are not Windows product proof."""
from copy import deepcopy
import importlib.util
import json
import os
from pathlib import Path, PureWindowsPath
import subprocess
import tempfile
import ctypes
import queue
import threading
import time
from types import SimpleNamespace
from io import BytesIO
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('windows_product', Path(__file__).with_name('windows_product_qualification.py'))
w = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w)


class RequestSafety(unittest.TestCase):
    def setUp(self):
        self.env = {'WINDOWS_WORKFLOW_HEAD': 'a' * 40, 'WINDOWS_PRODUCT_HEAD': w.FIRST_PRODUCT,
                    'WINDOWS_PRODUCT_BASE': 'b' * 40, 'WINDOWS_PRODUCT_TARGETS': 'test_backend_contract',
                    'WINDOWS_CONFLICTING_MODE': 'false', 'WINDOWS_CACHE_REQUEST': '{}',
                    'WINDOWS_SUBMITTED_AT': '2026-01-01T00:00:00+00:00',
                    'GITHUB_REPOSITORY': 'vahid110/odbcpp', 'GITHUB_REPOSITORY_VISIBILITY': 'public',
                    'RUNNER_ENVIRONMENT': 'github-hosted', 'RUNNER_OS': 'Windows', 'RUNNER_ARCH': 'X64'}

    def test_exact_cold_request_and_owning_selection(self):
        self.assertIsNotNone(w.request_guard(self.env))
        self.assertEqual(w.selection('test_backend_contract,test_diagnostics_apis'),
                         ['test_backend_contract', 'test_diagnostics_apis'])

    def test_reject_mutable_refs_and_injected_or_duplicate_targets(self):
        for key in ('WINDOWS_WORKFLOW_HEAD', 'WINDOWS_PRODUCT_HEAD', 'WINDOWS_PRODUCT_BASE'):
            for bad in ('main', 'a' * 39, 'A' * 40, 'a' * 40 + '; echo bad'):
                env = dict(self.env, **{key: bad})
                with self.subTest(key=key, value=bad), self.assertRaises(ValueError):
                    w.request_guard(env)
        for bad in ('', 'test_backend_contract;whoami', '.*', 'all',
                    'test_backend_contract,test_backend_contract', ' test_backend_contract', ','.join(w.TARGETS)):
            with self.subTest(targets=bad), self.assertRaises(ValueError):
                w.selection(bad)

    def test_reject_conflicts_cache_private_paid_or_wrong_platform(self):
        for key, bad in (('WINDOWS_CONFLICTING_MODE', 'true'), ('WINDOWS_CACHE_REQUEST', '{"key":"x"}'),
                         ('GITHUB_REPOSITORY', 'some/private'), ('GITHUB_REPOSITORY_VISIBILITY', 'private'),
                         ('RUNNER_ENVIRONMENT', 'self-hosted'), ('RUNNER_ARCH', 'ARM64'),
                         ('RUNNER_OS', 'Linux'), ('WINDOWS_PRODUCT_TARGETS', 'test_diagnostics_apis'),
                         ('WINDOWS_SUBMITTED_AT', '2999-01-01T00:00:00Z'),
                         ('WINDOWS_SUBMITTED_AT', '2026-01-01T00:00:00')):
            with self.subTest(key=key, value=bad), self.assertRaises(ValueError):
                w.request_guard(dict(self.env, **{key: bad}))

    def test_source_identity_rejects_tracked_and_untracked_injection(self):
        responses = ['a' * 40, ' M core/x.cpp']
        with patch.object(w, 'output', side_effect=responses), self.assertRaises(ValueError):
            w.source('a' * 40)
        responses = ['a' * 40, '', 'core/injected.cpp']
        with patch.object(w, 'output', side_effect=responses), self.assertRaises(ValueError):
            w.source('a' * 40)

    def test_source_guard_checks_both_immutable_sources(self):
        before = {'product': {'head': 'a'}, 'workflow': {'head': 'b'}}
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            w.write('identity.json', {'sources': before})
            for role in ('product', 'workflow'):
                after = deepcopy(before)
                after[role]['head'] = 'changed'
                with patch.object(w, 'sources', return_value=after), self.assertRaises(ValueError):
                    w.source_guard()


class ResultAndFailureSafety(unittest.TestCase):
    def test_real_inventory_requires_cases_and_rejects_disabled(self):
        text = 'Running main() from gtest_main.cc\nBackendContractTest.\n  One\n  Two # TypeParam = x\n'
        self.assertEqual(w.gtest_names(text), ['BackendContractTest.One', 'BackendContractTest.Two'])
        for bad in ('', 'Empty.\n', 'Suite.\n  DISABLED_case\n', 'Suite.\n  One\n  One\n'):
            with self.subTest(value=bad), self.assertRaises(ValueError):
                w.gtest_names(bad)

    def test_xml_exact_inventory_failure_skip_and_missing_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'owning.xml'
            good = '<testsuites tests="1"><testsuite><testcase classname="Suite" name="One"/></testsuite></testsuites>'
            path.write_text(good)
            self.assertEqual(w.xml_result(path, ['Suite.One'])['count'], 1)
            for bad in (good.replace('/>', '><failure/></testcase>'),
                        good.replace('/>', '><skipped/></testcase>'),
                        good.replace('name="One"', 'name="Two"'),
                        good.replace('tests="1"', 'tests="2"'),
                        good.replace('name="One"', 'name="One" status="notrun"'),
                        good.replace('tests="1"', 'tests="1" failures="1"')):
                path.write_text(bad)
                with self.subTest(xml=bad), self.assertRaises(ValueError):
                    w.xml_result(path, ['Suite.One'])

    def test_failed_and_missing_phases_never_become_completion(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            complete, phases = w.completion()
            self.assertFalse(complete)
            self.assertTrue(all(v['status'] == 'missing' for v in phases.values()))
            for phase in w.PHASES:
                w.write(phase + '.json', {'status': 'passed'})
            self.assertFalse(w.completion()[0])  # Missing actual XML-derived results.
            w.write('results.json', {'owning': 'validated'})
            self.assertTrue(w.completion()[0])
            w.write('build.json', {'status': 'failed', 'error': 'original MSVC failure'})
            self.assertFalse(w.completion()[0])
            with patch.object(w, 'source_guard'), self.assertRaises(RuntimeError):
                w.finish()
            self.assertEqual(json.loads((Path(tmp) / 'build.json').read_text())['error'], 'original MSVC failure')

    def test_failed_command_preserves_exit_and_log(self):
        # Execute only a trivial local offline child, never a product/helper phase.
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            with self.assertRaises(RuntimeError):
                w.measure('failure', [os.sys.executable, '-c', 'print("original diagnostic"); raise SystemExit(7)'], 10)
            record = json.loads((Path(tmp) / 'failure-command.json').read_text())
            self.assertEqual(record['exit'], 7)
            self.assertTrue(record['direct_terminal'])
            self.assertIn('original diagnostic', (Path(tmp) / 'failure.log').read_text())

    def test_truncated_log_keeps_original_final_diagnostic(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)), patch.object(w, 'LOG_BYTES', 16):
            with self.assertRaises(RuntimeError):
                w.measure('large', [os.sys.executable, '-c', 'print("x" * 100); print("final original error"); raise SystemExit(9)'], 10)
            self.assertIn('final original error', (Path(tmp) / 'large.log').read_text())
            self.assertTrue(json.loads((Path(tmp) / 'large-command.json').read_text())['log_truncated'])

    def test_timeout_preserves_failure_and_terminal(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            with self.assertRaises(RuntimeError):
                w.measure('timeout', [os.sys.executable, '-c', 'import time; time.sleep(10)'], 0.05)
            record = json.loads((Path(tmp) / 'timeout-command.json').read_text())
            self.assertTrue(record['timed_out'])
            self.assertTrue(record['direct_terminal'])

    def test_artifact_includes_index_in_bound_and_rejects_binary_or_symlink(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            w.write('a.json', {'text': 'evidence'})
            w.package()
            self.assertIn('a.json', json.loads((Path(tmp) / 'evidence-index.json').read_text()))
            with patch.object(w, 'MAX_BYTES', 10), self.assertRaises(ValueError):
                w.package()
            binary = Path(tmp) / 'restored.exe'
            binary.write_bytes(b'MZ')
            with self.assertRaises(ValueError):
                w.package()
            binary.unlink()
            link = Path(tmp) / 'link.txt'
            try:
                link.symlink_to(Path(tmp) / 'a.json')
            except OSError:
                return  # Windows non-admin symlink permission is not a product result.
            with self.assertRaises(ValueError):
                w.package()


class ReviewRepairSafety(unittest.TestCase):
    def test_windows_inventory_keys_match_literal_driver_and_sdk_paths(self):
        root = PureWindowsPath(r'C:\work\build-windows-product')
        driver = PureWindowsPath(r'C:\work\build-windows-product\Release\odbcpp.dll')
        sdk = PureWindowsPath(r'C:\work\build-windows-product\sdk\odbcpp_sdk_header_isolation.dir\Release\one.obj')
        self.assertEqual(w.relative_key(driver, root), 'Release/odbcpp.dll')
        self.assertIn('Release/odbcpp.dll', {w.relative_key(driver, root): 'fresh DLL'})
        self.assertEqual(w.relative_key(sdk, root), 'sdk/odbcpp_sdk_header_isolation.dir/Release/one.obj')

    def test_assignment_precedes_resume_and_immediate_child_execution(self):
        events = ['created_suspended']
        process = SimpleNamespace(launch_order=events)
        def attach(p):
            self.assertNotIn('resumed', events)
            p.assigned = True
        def resume():
            self.assertTrue(process.assigned)
            self.assertEqual(events, ['created_suspended', 'assigned_to_job'])
            events.append('resumed')
        process.resume = resume
        w.assign_and_resume(SimpleNamespace(attach=attach), process)
        self.assertEqual(events, ['created_suspended', 'assigned_to_job', 'resumed'])

    def test_assignment_and_resume_failures_reap_without_success(self):
        for failing_method in ('attach', 'resume'):
            with self.subTest(method=failing_method), tempfile.TemporaryDirectory() as tmp:
                events = []
                class Process:
                    pid = 123
                    def __init__(self, command):
                        self.launch_order = ['created_suspended']
                        self.stdout = BytesIO()
                        self.terminal = False
                    def resume(self):
                        if failing_method == 'resume':
                            raise RuntimeError('original resume failure')
                        events.append('resumed')
                    def kill(self):
                        events.append('kill')
                        self.terminal = True
                    def wait(self, timeout):
                        self.assert_terminal = self.terminal
                        events.append('wait')
                        return 124
                    def poll(self):
                        return 124 if self.terminal else None
                    def close(self):
                        events.append('close_process')
                class Job:
                    def attach(self, process):
                        if failing_method == 'attach':
                            raise RuntimeError('original attach failure')
                    def active(self):
                        return 0
                    def close(self):
                        events.append('close_job')
                with patch.object(w, 'os', SimpleNamespace(name='nt')), patch.object(w, 'EVIDENCE', Path(tmp)), \
                     patch.object(w, 'WindowsSuspendedProcess', Process), patch.object(w, 'WindowsProcessJob', Job):
                    with self.assertRaisesRegex(RuntimeError, 'original ' + failing_method + ' failure'):
                        w.measure('launch', ['command'], 10)
                record = json.loads((Path(tmp) / 'launch-command.json').read_text())
                self.assertEqual(record['status'], 'failed')
                self.assertTrue(record['cleanup']['direct_terminal'])
                self.assertEqual(record['cleanup']['job_active_after_cleanup'], 0)
                self.assertNotIn('resumed', events)
                self.assertEqual(events, ['kill', 'wait', 'close_process', 'close_job'])


class OwnedDescendantLifecycleSafety(unittest.TestCase):
    def test_successful_direct_exit_with_owned_child_still_refuses_and_retains_identity(self):
        events = []
        class Held:
            evidence = {'pid': 301, 'image_path': r'C:\tool\child.exe', 'creation_filetime': 123456,
                        'job_member': True, 'held_handle': True}
            def terminal(self):
                return 'terminate_owned_job' in events
            def close(self):
                events.append('close_held_handle')
        class Job:
            def capture_owned(self, direct_pid, known_pids=()):
                self.direct_pid = direct_pid
                return ([] if 301 in known_pids else [Held()]), []
            def active(self):
                return 0 if 'terminate_owned_job' in events else 1
            def terminate(self):
                events.append('terminate_owned_job')
        cleanup, unexpected, invalid = w.owned_exit_cleanup(Job(), 123, grace_seconds=0, cleanup_seconds=0)
        self.assertTrue(unexpected)  # Original successful-command/remaining-child guard stays strict.
        self.assertFalse(invalid)
        self.assertEqual(cleanup['active_after_direct_exit'], 1)
        self.assertEqual(cleanup['active_after_cleanup'], 0)
        self.assertEqual(cleanup['owned_descendants'][0]['pid'], 301)
        self.assertEqual(cleanup['owned_descendants'][0]['creation_filetime'], 123456)
        self.assertTrue(cleanup['owned_descendants'][0]['terminal_after_cleanup'])
        self.assertTrue(cleanup['owned_descendants'][0]['held_handle_closed'])
        self.assertEqual(events, ['terminate_owned_job', 'close_held_handle'])

    def test_unknown_and_uncontained_identity_refuse_even_when_owned_job_is_empty(self):
        for error in ('unknown image/creation identity', 'process no longer belongs to owned job'):
            with self.subTest(error=error):
                job = SimpleNamespace(capture_owned=lambda direct_pid, known_pids=(): ([], [{'pid': 301, 'error': error}]),
                                      active=lambda: 0, terminate=lambda: self.fail('empty job must not target any PID'))
                cleanup, unexpected, invalid = w.owned_exit_cleanup(job, 123, grace_seconds=0, cleanup_seconds=0)
                self.assertFalse(unexpected)
                self.assertTrue(invalid)
                self.assertEqual(cleanup['identity_errors'][0]['error'], error)
                self.assertEqual(cleanup['active_after_cleanup'], 0)

    def test_surviving_owned_process_or_unverified_held_terminal_refuses(self):
        class Held:
            evidence = {'pid': 301, 'job_member': True, 'held_handle': True}
            def terminal(self): return False
            def close(self): pass
        for remaining in (0, 1):
            with self.subTest(remaining=remaining):
                job = SimpleNamespace(capture_owned=lambda direct_pid, known_pids=(): ([Held()], []),
                                      active=lambda: remaining, terminate=lambda: None)
                cleanup, unexpected, invalid = w.owned_exit_cleanup(job, 123, grace_seconds=0, cleanup_seconds=0)
                self.assertTrue(invalid)
                self.assertFalse(cleanup['owned_descendants'][0]['terminal_after_cleanup'])

    def test_snapshot_and_held_handle_close_errors_are_not_success(self):
        def broken_snapshot(direct_pid, known_pids=()):
            raise OSError('identity query unavailable')
        job = SimpleNamespace(capture_owned=broken_snapshot, active=lambda: 0, terminate=lambda: None)
        cleanup, unexpected, invalid = w.owned_exit_cleanup(job, 123, grace_seconds=0, cleanup_seconds=0)
        self.assertTrue(invalid)
        self.assertEqual(cleanup['identity_errors'][0]['error'], 'identity query unavailable')
        class Held:
            evidence = {'pid': 301, 'job_member': True, 'held_handle': True}
            def terminal(self): return True
            def close(self): raise OSError('handle close failed')
        job.capture_owned = lambda direct_pid, known_pids=(): ([Held()], [])
        cleanup, unexpected, invalid = w.owned_exit_cleanup(job, 123, grace_seconds=0, cleanup_seconds=0)
        self.assertTrue(invalid)
        self.assertFalse(cleanup['owned_descendants'][0]['held_handle_closed'])
        self.assertEqual(cleanup['identity_errors'][0]['stage'], 'close_held_handle')

    def test_known_child_finishing_during_grace_retains_creation_and_reap_proof(self):
        events = []
        class Held:
            evidence = {'pid': 301, 'image_path': r'C:\tool\child.exe', 'creation_filetime': 123456,
                        'job_member': True, 'held_handle': True}
            def terminal(self): return True
            def close(self): events.append('closed')
        job = SimpleNamespace(capture_owned=lambda direct_pid, known_pids=(): ([Held()], []), active=lambda: 0,
                              terminate=lambda: self.fail('already completed child must not be killed'))
        cleanup, unexpected, invalid = w.owned_exit_cleanup(job, 123, grace_seconds=0, cleanup_seconds=0)
        self.assertFalse(unexpected)
        self.assertFalse(invalid)
        self.assertTrue(cleanup['owned_descendants'][0]['terminal_after_cleanup'])
        self.assertTrue(cleanup['owned_descendants'][0]['held_handle_closed'])
        self.assertEqual(events, ['closed'])


class OwnedDescendantReapSafety(unittest.TestCase):
    def exercise(self, signals, active_until=0, wait_error=None, close_error=None,
                 initially_active=True):
        clock, events = [0.0], []
        class Held:
            def __init__(self, index, signal_at):
                self.index, self.signal_at = index, signal_at
                self.evidence = {'pid': 301 + index, 'image_path': r'C:\tool\child.exe',
                                 'creation_filetime': 123456 + index,
                                 'job_member': True, 'held_handle': True}
            def terminal(self):
                if self.index == wait_error:
                    raise OSError('held process wait failed')
                return clock[0] >= self.signal_at
            def close(self):
                events.append(('close', self.index, clock[0]))
                if self.index == close_error:
                    raise OSError('held process close failed')
        class Job:
            terminated = False
            def capture_owned(self, direct_pid, known_pids=()):
                return [Held(i, signal_at) for i, signal_at in enumerate(signals)
                        if 301 + i not in known_pids], []
            def active(self):
                if not self.terminated:
                    return int(initially_active)
                return int(clock[0] < active_until)
            def terminate(self):
                self.terminated = True
                events.append(('terminate', clock[0]))
        def advance(seconds):
            self.assertGreater(seconds, 0)
            self.assertLessEqual(clock[0] + seconds, 1.000001)
            clock[0] += seconds
        with patch.object(w.time, 'monotonic', side_effect=lambda: clock[0]), \
             patch.object(w.time, 'sleep', side_effect=advance):
            result = w.owned_exit_cleanup(Job(), 123, grace_seconds=0, cleanup_seconds=1)
        return result, clock[0], events

    def test_empty_job_waits_for_all_held_process_signals_before_close(self):
        (cleanup, unexpected, invalid), elapsed, events = self.exercise([0.2, 0.4])
        self.assertTrue(unexpected)  # A reaped lingering child still refuses command success.
        self.assertFalse(invalid)
        self.assertEqual(cleanup['active_after_cleanup'], 0)
        self.assertGreaterEqual(elapsed, 0.4)
        self.assertLess(elapsed, 1)
        self.assertTrue(all(r['terminal_after_cleanup'] and r['held_handle_closed']
                            for r in cleanup['owned_descendants']))
        self.assertEqual([e[:2] for e in events], [('terminate', 0.0), ('close', 0), ('close', 1)])
        self.assertTrue(all(e[2] >= 0.4 for e in events if e[0] == 'close'))

    def test_job_and_multiple_held_processes_share_one_cleanup_deadline(self):
        (cleanup, unexpected, invalid), elapsed, events = self.exercise([0.6, 1.2], active_until=0.4)
        self.assertTrue(unexpected)
        self.assertTrue(invalid)
        self.assertAlmostEqual(elapsed, 1)
        self.assertEqual(cleanup['active_after_cleanup'], 0)
        self.assertTrue(cleanup['owned_descendants'][0]['terminal_after_cleanup'])
        self.assertFalse(cleanup['owned_descendants'][1]['terminal_after_cleanup'])
        self.assertTrue(all(r['held_handle_closed'] for r in cleanup['owned_descendants']))
        self.assertTrue(all(e[2] <= 1.000001 for e in events if e[0] == 'close'))

    def test_held_terminal_proof_is_required_even_without_job_termination(self):
        (cleanup, unexpected, invalid), elapsed, events = self.exercise([0.2], initially_active=False)
        self.assertFalse(unexpected)
        self.assertFalse(invalid)
        self.assertGreaterEqual(elapsed, 0.2)
        self.assertEqual(cleanup['active_after_cleanup'], 0)
        self.assertTrue(cleanup['owned_descendants'][0]['terminal_after_cleanup'])
        self.assertEqual([e[:2] for e in events], [('close', 0)])

    def test_wait_and_close_failure_stay_failed_while_other_handles_are_reaped(self):
        (cleanup, unexpected, invalid), elapsed, events = self.exercise(
            [0, 0.2], wait_error=0, close_error=0)
        self.assertTrue(unexpected)
        self.assertTrue(invalid)
        self.assertGreaterEqual(elapsed, 0.2)
        self.assertEqual(cleanup['active_after_cleanup'], 0)
        self.assertIsNone(cleanup['owned_descendants'][0]['terminal_after_cleanup'])
        self.assertFalse(cleanup['owned_descendants'][0]['held_handle_closed'])
        self.assertTrue(cleanup['owned_descendants'][1]['terminal_after_cleanup'])
        self.assertTrue(cleanup['owned_descendants'][1]['held_handle_closed'])
        self.assertEqual([e['stage'] for e in cleanup['identity_errors']],
                         ['held_process_reap', 'close_held_handle'])
        self.assertEqual([e[:2] for e in events if e[0] == 'close'], [('close', 0), ('close', 1)])

    def test_signaled_handles_do_not_replace_nonzero_job_count_refusal(self):
        (cleanup, unexpected, invalid), elapsed, events = self.exercise([0.2], active_until=2)
        self.assertTrue(unexpected)
        self.assertTrue(invalid)
        self.assertAlmostEqual(elapsed, 1)
        self.assertEqual(cleanup['active_after_cleanup'], 1)
        self.assertTrue(cleanup['owned_descendants'][0]['terminal_after_cleanup'])
        self.assertTrue(cleanup['owned_descendants'][0]['held_handle_closed'])


@unittest.skipUnless(os.name == 'nt', 'actual Windows process APIs unrun on non-Windows host')
class WindowsChildProcessRegression(unittest.TestCase):
    def test_native_suspension_prevents_execution_before_assignment(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / 'command-started.txt'
            command = [os.sys.executable, '-c', 'from pathlib import Path; Path(' + repr(str(marker)) + ').write_text("ran")']
            job = w.WindowsProcessJob()
            process = None
            try:
                process = w.WindowsSuspendedProcess(command)
                with self.assertRaises(subprocess.TimeoutExpired):
                    process.wait(timeout=0.1)
                self.assertFalse(marker.exists())
                w.assign_and_resume(job, process)
                self.assertEqual(process.wait(timeout=10), 0)
                self.assertEqual(marker.read_text(), 'ran')
                self.assertEqual(process.launch_order, ['created_suspended', 'assigned_to_job', 'resumed'])
            finally:
                job.terminate()
                if process is not None:
                    process.wait(timeout=10)
                    process.stdout.close()
                    process.close()
                job.close()

    def test_immediate_descendant_belongs_to_own_job_and_is_reaped(self):
        command = [os.sys.executable, '-c',
                   'import subprocess,sys,time; child=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"]); '
                   'print(child.pid,flush=True); time.sleep(60)']
        job = w.WindowsProcessJob()
        process, child_handle, reader = None, None, None
        try:
            process = w.WindowsSuspendedProcess(command)
            w.assign_and_resume(job, process)
            pid_queue = queue.Queue()
            reader = threading.Thread(target=lambda: pid_queue.put(process.stdout.readline()), daemon=True)
            reader.start()
            child_pid = int(pid_queue.get(timeout=10))
            job.api.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
            job.api.OpenProcess.restype = ctypes.c_void_p
            child_handle = job.api.OpenProcess(0x100000 | 0x1000, False, child_pid)
            self.assertTrue(child_handle)
            job.api.IsProcessInJob.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
            job.api.IsProcessInJob.restype = ctypes.c_int
            belongs = ctypes.c_int()
            self.assertTrue(job.api.IsProcessInJob(child_handle, job.handle, ctypes.byref(belongs)))
            self.assertTrue(belongs.value)
            self.assertGreaterEqual(job.active(), 2)
            job.terminate()
            process.wait(timeout=10)
            self.assertEqual(process.api.WaitForSingleObject(child_handle, 10000), 0)
            deadline = time.monotonic() + 5
            while job.active() and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertEqual(job.active(), 0)
        finally:
            job.terminate()
            if process is not None:
                process.wait(timeout=10)
            if reader is not None:
                reader.join(timeout=2)
            if process is not None:
                process.stdout.close()
                process.close()
            if child_handle:
                job.api.CloseHandle(child_handle)
            job.close()

    def test_successful_parent_exit_retains_child_identity_and_still_refuses(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            marker = Path(tmp) / 'owned-child-pid.txt'
            script = ('import subprocess,sys; from pathlib import Path; '
                      'child=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"], '
                      'stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); '
                      'Path(' + repr(str(marker)) + ').write_text(str(child.pid))')
            with self.assertRaisesRegex(RuntimeError, 'command failed'):
                w.measure('owned-child', [os.sys.executable, '-c', script], 10)
            record = json.loads((Path(tmp) / 'owned-child-command.json').read_text())
            self.assertEqual(record['exit'], 0)
            self.assertFalse(record['timed_out'])
            self.assertTrue(record['direct_terminal'])
            cleanup = record['cleanup']
            self.assertEqual(cleanup['active_after_direct_exit'], 1)
            self.assertEqual(cleanup['active_after_cleanup'], 0)
            self.assertFalse(cleanup['identity_errors'])
            self.assertEqual(len(cleanup['owned_descendants']), 1)
            identity = cleanup['owned_descendants'][0]
            self.assertEqual(identity['pid'], int(marker.read_text()))
            self.assertEqual(os.path.normcase(identity['image_path']), os.path.normcase(os.sys.executable))
            self.assertGreater(identity['creation_filetime'], 0)
            self.assertTrue(identity['job_member'])
            self.assertTrue(identity['held_handle'])
            self.assertTrue(identity['terminal_after_cleanup'])
            self.assertTrue(identity['held_handle_closed'])

    def test_native_assignment_and_resume_failures_never_execute_command(self):
        for method, owner in (('attach', w.WindowsProcessJob), ('resume', w.WindowsSuspendedProcess)):
            with self.subTest(method=method), tempfile.TemporaryDirectory() as tmp:
                marker = Path(tmp) / 'should-not-run.txt'
                command = [os.sys.executable, '-c', 'from pathlib import Path; Path(' + repr(str(marker)) + ').write_text("unexpected")']
                with patch.object(w, 'EVIDENCE', Path(tmp)), patch.object(owner, method, side_effect=RuntimeError('injected ' + method + ' failure')):
                    with self.assertRaisesRegex(RuntimeError, 'injected ' + method + ' failure'):
                        w.measure('native-launch', command, 10)
                record = json.loads((Path(tmp) / 'native-launch-command.json').read_text())
                self.assertTrue(record['cleanup']['direct_terminal'])
                self.assertEqual(record['cleanup']['job_active_after_cleanup'], 0)
                self.assertNotIn('resumed', record['cleanup']['launch_order'])
                self.assertFalse(marker.exists())


class WorkflowSafety(unittest.TestCase):
    def test_default_off_mutual_modes_and_full_body_preservation(self):
        # No YAML dependency: compare actual baseline full-job bodies byte for byte.
        ci = (ROOT / '.github/workflows/ci.yml').read_text()
        old = subprocess.check_output(['git', '-C', str(ROOT), 'show', w.FIRST_PRODUCT + ':.github/workflows/ci.yml'], text=True)
        jobs = ('build-and-test', 'redshift-build', 'windows-live', 'windows-package',
                'windows-linked-server', 'linux-sanitizers', 'linux-openssl-static',
                'awslc-adapter-proof', 'iodbc-unicode')
        import re
        def job(text, name):
            block = re.search(r'^  ' + name + r':\n(.*?)(?=^  [a-z][a-z-]*:|\Z)', text, re.M | re.S)[1]
            return '\n'.join(line for line in block.splitlines() if not line.startswith('    if:'))
        for name in jobs:
            self.assertEqual(job(ci, name), job(old, name), name)
        self.assertIn('run_windows_product_qualification:\n', ci)
        window_input = ci.split('      run_windows_product_qualification:', 1)[1].split('      windows_product_targets:', 1)[0]
        self.assertIn('default: false', window_input)
        for name in jobs:
            guard = ci.split('\n  ' + name + ':', 1)[1].splitlines()[1]
            self.assertIn('run_windows_product_qualification', guard)
        self.assertIn("'windows-product-qualification'", ci)
        self.assertIn("'ubuntu-development-pilot'", ci)
        self.assertIn('cancel-in-progress: false', ci)
        self.assertIn('inputs.run_development_pilot || inputs.run_product_qualification || inputs.run_linked_server', ci)
        self.assertIn('inputs.run_product_qualification && !inputs.run_windows_product_qualification', ci)
        self.assertIn('inputs.run_development_pilot && !inputs.run_product_qualification && !inputs.run_windows_product_qualification', ci)

    def test_windows_job_has_no_restores_services_native_or_secret_steps(self):
        workflow = (ROOT / '.github/workflows/windows-product-qualification.yml').read_text()
        self.assertIn('runs-on: windows-2025', workflow)
        self.assertIn('timeout-minutes: 30', workflow)
        self.assertIn('retention-days: 3', workflow)
        self.assertIn('if: always()', workflow)
        for forbidden in ('actions/cache', 'services:', 'secrets:', 'windows-postgres', 'windows-system-dsn',
                          'packaging/windows', 'Start-Service', 'self-hosted', 'larger-runner'):
            self.assertNotIn(forbidden, workflow)
        self.assertIn('persist-credentials: false', workflow)
        self.assertIn('github.event.repository.private == false', workflow)
        self.assertIn("'--parallel', '2'", (ROOT / 'tools/ci/windows_product_qualification.py').read_text())
        self.assertIn('-DODBCPP_CRYPTO_PROFILE_LIVE_TESTS=OFF', w.PROFILE)
        self.assertIn('-DODBCPP_CRYPTO_TLS_PROOF=OFF', w.PROFILE)
        self.assertEqual(len(w.CHECKS), 6)
        self.assertEqual(len(w.NUMERIC_CASES), 4)


if __name__ == '__main__':
    unittest.main()
