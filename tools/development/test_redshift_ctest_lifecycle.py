"""Offline lifecycle regression; all SQL/cloud/private-input methods are mocked."""
from contextlib import contextmanager
import importlib.util
from pathlib import Path
import tempfile
import types
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    'redshift_ctest_lifecycle', Path(__file__).with_name('redshift_ctest_lifecycle.py'))
lifecycle = importlib.util.module_from_spec(spec)
import sys
sys.modules[spec.name] = lifecycle
spec.loader.exec_module(lifecycle)


class Ops:
    def __init__(self, directory, outcome=8, crash=False):
        self.directory = directory
        self.outcome = outcome
        self.crash = crash
        self.calls = []
        self.clock = 100
        self.readers_known = True

    def now(self):
        return self.clock

    @contextmanager
    def lock(self, state):
        self.calls.append('lock')
        yield
        self.calls.append('unlock')

    def verify_source(self, *args):
        self.calls.append('verify_source')

    def open_inputs(self, *args):
        self.calls.append('open_inputs')

    def security_paused(self, *args):
        self.calls.append('security_paused')

    def resume_available(self, *args):
        self.calls.append('resume')

    def stage_profiles(self, index, run, state, logging, end):
        self.calls.append('stage_profiles')
        assert state == {'owned': 'fixture'}
        assert logging['LogQueries'] == 'false' and logging['LogSink'] == 'Stderr'
        result = run / 'staged-index.json'
        result.write_text('{}')
        return result

    def run_process(self, command, **kwargs):
        assert kwargs['umask'] == 0o077
        if command[0] == '/mock/cmake':
            self.calls.append('configure')
            return types.SimpleNamespace(returncode=0, timed_out=False,
                                         reaped=True, group_absent=True)
        self.calls.append('ctest')
        self.command = command
        if self.crash:
            raise RuntimeError('mock process supervisor exception')
        return types.SimpleNamespace(returncode=self.outcome, timed_out=False,
                                     reaped=True, group_absent=True)

    def reap(self, end):
        self.calls.append('reap')
        return True

    def cleanup_principals(self, end):
        self.calls.append('principal_cleanup')
        self.principals_quiescent = True
        return {'complete': True}

    def final_zero(self, end):
        self.calls.append('final_zero')
        return {'complete': True}

    def pause_until(self, end):
        self.calls.append('pause')
        return True

    def close_inputs(self, end):
        self.calls.append('close_inputs')
        return True


class Fixtures:
    def plan(self):
        return {'total_helpers': 4, 'total_SQL': 20, 'total_logins': 4,
                'total_inclusive': 24}

    def prepare(self, ops, end):
        ops.calls.append('fixture_prepare')
        ops.fixture_state = {'owned': 'fixture'}
        return ops.fixture_state

    def cleanup(self, ops, state, end):
        ops.calls.append('fixture_cleanup')
        assert ops.principals_quiescent and ops.readers_known
        assert state == {'owned': 'fixture'}
        return {'complete': True}


def config(directory):
    return lifecycle.Config(Path(directory) / 'run', Path('/mock/source'), 'a' * 40,
                            Path('/mock/index'), Path('/mock/expected'),
                            Path('/mock/cmake'), Path('/mock/ctest'), {})


class Lifecycle(unittest.TestCase):
    def run_mock(self, directory, ops, fixtures=None):
        with mock.patch.object(lifecycle, 'verify_config'):
            return lifecycle.run_once(config(directory), ops, fixtures or Fixtures())

    def test_nonzero_CTest_collects_once_and_always_closes(self):
        for code in (0, 8, -6):
            with self.subTest(code=code), tempfile.TemporaryDirectory() as directory:
                ops = Ops(directory, code)
                result = self.run_mock(directory, ops)
                self.assertEqual(result['ctest']['returncode'], code)
                self.assertTrue(result['lifecycle_cleanup_complete'])
                self.assertFalse(result['feature_results_parsed'])
                self.assertEqual(ops.calls.count('ctest'), 1)
                self.assertEqual(ops.calls[-7:], ['reap', 'principal_cleanup',
                    'fixture_cleanup', 'final_zero', 'pause', 'close_inputs', 'unlock'])
                self.assertEqual(ops.command.count('-j1'), 1)
                self.assertIn('--output-on-failure', ops.command)
                self.assertIn('--output-junit', ops.command)
                self.assertNotIn('--stop-on-failure', ops.command)

    def test_full_collection_has_execution_cleanup_and_independent_recovery_reserves(self):
        class TimelyOps(Ops):
            def run_process(self, command, **kwargs):
                result = super().run_process(command, **kwargs)
                if command[0] == '/mock/ctest':
                    self.clock = kwargs['deadline'] - 1
                return result

            def final_zero(self, end):
                self.clock = end - 800
                return super().final_zero(end)

            def pause_until(self, end):
                self.recovery_deadline = end
                self.clock += 850
                assert self.clock < end
                return super().pause_until(end)

        bounds = lifecycle.Bounds()
        self.assertEqual((bounds.execution, bounds.cleanup_reserve, bounds.aggregate,
                          bounds.availability, bounds.recovery, bounds.child, bounds.owner,
                          bounds.case), (3600, 900, 4500, 900, 900, 6300, 7200, 40))
        with tempfile.TemporaryDirectory() as directory:
            ops = TimelyOps(directory, outcome=0)
            result = self.run_mock(directory, ops)
            self.assertEqual(result['execution_end'], 3700)
            self.assertEqual(result['aggregate_end'], 4600)
            self.assertEqual(ops.recovery_deadline, 4700)
            self.assertEqual(result['ctest_invocations'], 1)
            self.assertTrue(result['paused_verified'] and result['private_inputs_closed'])
            self.assertFalse(result['lifecycle_cleanup_complete'])
            self.assertTrue((Path(directory) / 'run' / 'publication-expired.json').exists())

    def test_supervisor_exception_still_runs_all_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = Ops(directory, crash=True)
            result = self.run_mock(directory, ops)
            self.assertFalse(result['lifecycle_cleanup_complete'])
            self.assertTrue(result['paused_verified'] and result['private_inputs_closed'])
            self.assertEqual(result['errors'], [{'phase': 'ctest', 'error_type': 'RuntimeError'}])
            self.assertEqual(ops.calls[-7:], ['reap', 'principal_cleanup',
                'fixture_cleanup', 'final_zero', 'pause', 'close_inputs', 'unlock'])

    def test_partial_fixture_failure_keeps_ownership_for_finally(self):
        class Partial(Fixtures):
            def prepare(self, ops, end):
                super().prepare(ops, end)
                error = RuntimeError('private error text must not be serialized')
                error.state = ops.fixture_state
                raise error
        with tempfile.TemporaryDirectory() as directory:
            ops = Ops(directory)
            result = self.run_mock(directory, ops, Partial())
            self.assertFalse(result['ctest_invoked'])
            self.assertIn('fixture_cleanup', ops.calls)
            self.assertTrue(result['paused_verified'])
            self.assertNotIn('private error text', str(result))

    def test_cleanup_error_never_skips_pause_and_input_closure(self):
        class BadCleanup(Fixtures):
            def cleanup(self, ops, state, end):
                ops.calls.append('fixture_cleanup')
                raise RuntimeError('mock cleanup failure')
        with tempfile.TemporaryDirectory() as directory:
            ops = Ops(directory)
            result = self.run_mock(directory, ops, BadCleanup())
            self.assertFalse(result['lifecycle_cleanup_complete'])
            self.assertTrue(result['paused_verified'] and result['private_inputs_closed'])

    def test_oneuse_directory_and_unchanged_caps(self):
        lifecycle.Bounds().validate()
        for changed in ({'owner': 7201}, {'child': 6301}, {'aggregate': 4501},
                        {'execution': 3601}, {'availability': 901}, {'recovery': 901},
                        {'case': 41}, {'cleanup_reserve': 899}):
            with self.subTest(changed=changed), self.assertRaises(lifecycle.Refused):
                lifecycle.Bounds(**changed).validate()
        with tempfile.TemporaryDirectory() as directory:
            self.run_mock(directory, Ops(directory))
            second = Ops(directory)
            with self.assertRaises(FileExistsError):
                self.run_mock(directory, second)
            self.assertNotIn('open_inputs', second.calls)

    def test_complete_overcap_plan_refuses_before_inputs_or_cloud(self):
        class OverCap(Fixtures):
            def plan(self):
                return {'total_helpers': 41, 'total_SQL': 369,
                        'total_logins': 41, 'total_inclusive': 410}
        with tempfile.TemporaryDirectory() as directory:
            ops = Ops(directory)
            with self.assertRaises(lifecycle.Refused):
                self.run_mock(directory, ops, OverCap())
            self.assertEqual(ops.calls, [])
            self.assertFalse((Path(directory) / 'run').exists())

    def test_approved_complete_plan_and_both_boundaries(self):
        plan = {'total_helpers': 38, 'network_helpers_separate': 1,
                'total_SQL': 372, 'total_logins': 38, 'total_inclusive': 410}
        self.assertEqual(lifecycle.validate_fixture_plan(plan), plan)
        for changed in ({'network_helpers_separate': 3},
                        {'total_SQL': 413, 'total_inclusive': 451}):
            with self.subTest(changed=changed), self.assertRaises(lifecycle.Refused):
                lifecycle.validate_fixture_plan({**plan, **changed})

    def test_unknown_reader_retains_inputs_but_still_pauses(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = Ops(directory)
            ops.reap = lambda deadline: False
            result = self.run_mock(directory, ops)
            self.assertFalse(result['lifecycle_cleanup_complete'])
            self.assertFalse(result['private_inputs_closed'])
            self.assertNotIn('close_inputs', ops.calls)
            self.assertIn('pause', ops.calls)

    def test_begin_cleanup_precedes_first_cleanup_clock_and_reap(self):
        class GuardedOps(Ops):
            cleanup_pending = False
            signals_masked = False

            def run_process(self, command, **kwargs):
                result = super().run_process(command, **kwargs)
                if command[0] != '/mock/cmake':
                    self.cleanup_pending = True
                return result

            def begin_cleanup(self):
                self.calls.append('begin_cleanup')
                self.signals_masked = True

            def now(self):
                if self.cleanup_pending:
                    assert self.signals_masked, 'cleanup clock before stop signals masked'
                    self.calls.append('cleanup_clock')
                return super().now()

        with tempfile.TemporaryDirectory() as directory:
            ops = GuardedOps(directory)
            result = self.run_mock(directory, ops)
            self.assertTrue(result['lifecycle_cleanup_complete'])
            self.assertEqual(ops.calls.count('begin_cleanup'), 1)
            self.assertLess(ops.calls.index('begin_cleanup'), ops.calls.index('cleanup_clock'))
            self.assertLess(ops.calls.index('begin_cleanup'), ops.calls.index('reap'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
