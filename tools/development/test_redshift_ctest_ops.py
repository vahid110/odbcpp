"""Offline adapter tests: mocked provider/SQL/processes and synthetic inputs."""
import os
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).parents[2]))
from tools.development import redshift_ctest_ops as ops_module
from tools.development.redshift_ctest_lifecycle import Refused, validate_fixture_plan


class Native:
    def __init__(self):
        self.config = {'host': 'mock.invalid', 'ca': '/mock/ca'}
        self.snapshot = SimpleNamespace(passfile_path='/mock/admin.pgpass', close=lambda: True)
        self.test_passfile = None
        self.network_helpers = 1
        self.readers_known = True
        self.calls = []
        self.raw = b'RSD1|odbcpp_pilot|odbcpp_pilot_test|123\n1\n'
        self.returncode, self.timed_out = 0, False

    def ordinary_held(self):
        pass

    def owned_process(self, argv, **kwargs):
        self.calls.append((argv, kwargs))
        kwargs['output'].write_bytes(self.raw)
        return SimpleNamespace(returncode=self.returncode, timed_out=self.timed_out)


def adapter(directory):
    ops = ops_module.Operations.__new__(ops_module.Operations)
    ops.directory = Path(directory)
    ops.native = Native()
    ops.preparation_manifest = {'psql': '/mock/psql'}
    ops.helpers = ops.statements = ops.logins = 0
    ops.sql_unknown = ops.sql_fenced = False
    ops.readers_known = True
    ops.proofs = {}
    ops.now = lambda: 100
    ops.processes = []
    ops.executables = {'/mock/cmake'}
    ops.generated = []
    ops.env_directory = ops.profile_inputs = ops.input_residue = None
    ops.native_custody_state = None
    return ops


class AdapterTests(unittest.TestCase):
    def test_system_python_uses_shared_absolute_clock_domain(self):
        script = '''import json,sys,time
sys.path.insert(0,sys.argv[1])
from tools.development.redshift_ctest_ops import Operations
before=time.clock_gettime(time.CLOCK_MONOTONIC)
observed=Operations.now(None)
after=time.clock_gettime(time.CLOCK_MONOTONIC)
print(json.dumps([before,observed,after]))
'''
        raw = subprocess.check_output(['/usr/bin/python3', '-B', '-c', script,
            str(Path(__file__).parents[2])], stdin=subprocess.DEVNULL, timeout=5)
        before, observed, after = json.loads(raw)
        self.assertLessEqual(before, observed)
        self.assertLessEqual(observed, after)

    def test_approved_combined_plan_passes_before_adapter_construction(self):
        plan = ops_module.combined_fixture_plan()
        self.assertEqual((plan['total_helpers'], plan['total_SQL'], plan['total_logins'],
                          plan['total_inclusive']), (38, 372, 38, 410))
        self.assertEqual(plan['network_helpers_separate'], 1)
        self.assertEqual(validate_fixture_plan(plan), plan)

    def test_termination_revalidates_birth_principal_database_and_self_first(self):
        targets = tuple((n, ops_module.ORDINARY, ops_module.DATABASE,
                         '2026-10-18 10:00:00.123456') for n in range(10, 18))
        stage = ops_module.termination_stage(targets)
        self.assertEqual(stage['statements'], 16)
        self.assertEqual(stage['query'].count('process<>pg_backend_pid()'), 8)
        for pid, _, _, birth in targets:
            guard = stage['query'].index('process=' + str(pid) + ' AND')
            mutation = stage['query'].index('pg_terminate_backend(' + str(pid) + ')')
            self.assertLess(guard, mutation)
            self.assertIn("starttime=TIMESTAMP '" + birth + "'", stage['query'])
        with self.assertRaises(Refused):
            ops_module.termination_stage(((10, 'foreign', 'other', targets[0][3]),))

    def test_SQL_identity_TLS_counts_and_exact_whole_helper(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = adapter(directory)
            raw, pid = ops._sql({'query': 'SELECT 1;', 'statements': 1,
                'role': 'test', 'stage': 'synthetic'}, 120)
            self.assertEqual((raw, pid), ('1\n', 123))
            self.assertEqual((ops.helpers, ops.statements, ops.logins), (1, 4, 1))
            argv, kwargs = ops.native.calls[0]
            self.assertEqual(kwargs['seconds'], 4)
            self.assertEqual(kwargs['env']['PGSSLMODE'], 'verify-full')
            self.assertNotIn('PGPASSWORD', kwargs['env'])
            self.assertNotIn('mock-password', str(argv))

    def test_wrong_identity_timeout_and_overcap_never_retry(self):
        for failure in ('identity', 'timeout', 'cap'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                ops = adapter(directory)
                if failure == 'identity':
                    ops.native.raw = b'RSD1|other|foreign|123\n1\n'
                elif failure == 'timeout':
                    ops.native.timed_out = True
                else:
                    ops.helpers = 39  # plus network1: next helper would exceed40.
                with self.assertRaises(Refused):
                    ops._sql({'query': 'SELECT 1;', 'statements': 1,
                        'role': 'test', 'stage': 'synthetic'}, 120)
                if failure == 'cap':
                    self.assertFalse(ops.native.calls)
                else:
                    self.assertTrue(ops.sql_unknown and ops.sql_fenced)
                    with self.assertRaises(Refused):
                        ops._sql({'query': 'SELECT 1;', 'statements': 1,
                            'role': 'test', 'stage': 'synthetic'}, 120)
                    self.assertEqual(len(ops.native.calls), 1)

    def test_later_helper_PID_reuse_is_skipped_then_finalzero(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = adapter(directory)
            ops.native_custody_result = None
            calls = []
            def sql(stage, end):
                calls.append(stage)
                if stage['stage'] == 'principal_pids':
                    return '42|odbcpp_pilot_test|odbcpp_pilot|2026-10-18 10:00:00\n', 12
                return '42|SKIPPED\n', 42  # Later helper owns the reusedPID.
            ops._sql = sql
            ops.final_zero = lambda end: {'complete': True}
            ops.record = lambda event: None
            self.assertTrue(ops.cleanup_principals(120)['complete'])
            self.assertTrue(ops.principals_quiescent)
            self.assertIn('process<>pg_backend_pid()', calls[1]['query'])

    def test_nonzero_configure_still_reaps_and_tracks_generated_secret_copy(self):
        class Process:
            pid = 12345
            returncode = 1
            def wait(self, timeout=None):
                return self.returncode
            def poll(self):
                return self.returncode
        with tempfile.TemporaryDirectory() as directory:
            ops = adapter(directory)
            build = Path(directory) / 'ctest'
            build.mkdir(mode=0o700)
            generated = build / 'CTestTestfile.cmake'
            generated.write_text('synthetic private environment')
            generated.chmod(0o600)
            with mock.patch.object(ops_module.subprocess, 'Popen', return_value=Process()), \
                 mock.patch.object(ops_module, '_stop_group') as stop:
                ops._group_absent = lambda pid: True
                result = ops.run_process(['/mock/cmake'], env=dict(ops_module.BASE_ENV),
                    deadline=120, output=Path(directory) / 'configure.log', umask=0o077)
            self.assertEqual(result.returncode, 1)
            self.assertTrue(result.reaped and result.group_absent)
            stop.assert_called_once()
            self.assertEqual(len(ops.generated), 1)
            ops.reap = lambda end: True
            self.assertTrue(ops.close_inputs(120))
            self.assertFalse(generated.exists())

    def test_partial_or_nontrue_termination_ACK_fences_before_finalzero(self):
        for ack in ('', '42|f\n', 'malformed\n'):
            with self.subTest(ack=ack), tempfile.TemporaryDirectory() as directory:
                ops = adapter(directory)
                events = []
                def sql(stage, end):
                    if stage['stage'] == 'principal_pids':
                        return '42|odbcpp_pilot_test|odbcpp_pilot|2026-10-18 10:00:00\n', 12
                    return ack, 13
                ops._sql = sql
                ops.record = events.append
                ops.final_zero = mock.Mock()
                with self.assertRaises(Refused):
                    ops.cleanup_principals(120)
                self.assertTrue(ops.sql_unknown and ops.sql_fenced)
                ops.final_zero.assert_not_called()
                self.assertEqual(events[0]['captured'][0][0], 42)
                self.assertEqual(events[0]['guarded_ACK'], ack.strip().splitlines())

    def test_unknown_reader_retains_generated_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = adapter(directory)
            ops.readers_known = False
            with mock.patch.object(ops_module.inputs, 'close_generated_environment') as close:
                with self.assertRaises(Refused):
                    ops.close_inputs(120)
                close.assert_not_called()


if __name__ == '__main__':
    unittest.main(verbosity=2)
