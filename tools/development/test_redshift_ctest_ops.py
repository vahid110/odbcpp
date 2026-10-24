"""Offline adapter tests: mocked provider/SQL/processes and synthetic inputs."""
import os
import hashlib
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
    ops.approval = {}
    ops.available_cluster_verified = False
    ops.security_status = None
    ops.available_cluster_identity = None
    return ops


class AdapterTests(unittest.TestCase):
    def test_serverless_zero_requires_no_session_rows_then_exact_query_false(self):
        for payload in ('f\n', 'f\nf\n', 'f|f\n', '', 't\n', '1073791076\nf\n',
                        '1073791076\nt\n', '1073791076\n', '100|102|1\nf\n', 'f', ' f\n'):
            with self.subTest(payload=payload), tempfile.TemporaryDirectory() as directory:
                ops = adapter(directory)
                ops.resource_kind = 'serverless'
                ops.timeout_factory = SimpleNamespace(EXPECTED_PRINCIPAL={
                    'schema_owner_oid': '700', 'owner_oid': '701'})
                ops._sql = mock.Mock(return_value=(payload, 123))
                if payload == 'f\n':
                    self.assertEqual(ops.final_zero(300), {'complete': True})
                else:
                    with self.assertRaisesRegex(Refused, 'exact_principal_session_query_zero'):
                        ops.final_zero(300)
                stage = ops._sql.call_args.args[0]
                self.assertEqual(stage['statements'], 3)
                self.assertEqual(stage['query'].count(';') + stage['query'].count('\\gset'), 3)
                self.assertIn('SELECT session_id FROM sys_session_history', stage['query'])
                self.assertNotIn('SELECT EXISTS(SELECT 1 FROM sys_session_history', stage['query'])

    def test_serverless_cleanup_rebinds_oid_and_captured_birth_before_termination(self):
        targets = ((42, ops_module.ORDINARY, ops_module.DATABASE, '2026-10-24 12:00:00.123'),)
        stage = ops_module.termination_stage(targets, resource_kind='serverless',
            principal_oids={'admin_oid': 700, 'test_oid': 701})
        self.assertEqual(stage['statements'], 3)
        query = stage['query']
        for expected in ('pg_user_info', 'sys_session_history', 'session_id=42',
                         'session_id<>pg_backend_pid()', '=:rs_test_oid',
                         "start_time=TIMESTAMP '2026-10-24 12:00:00.123'"):
            self.assertIn(expected, query)
        self.assertNotIn('stv_', query)
        self.assertIn("usename='odbcpp_pilot_test' THEN usesysid END)=701", query)
        self.assertLess(query.index('AS still_owned'), query.index('pg_terminate_backend(42)'))
        for changed in ((42, 'foreign', ops_module.DATABASE, targets[0][3]),
                        (42, ops_module.ORDINARY, 'foreign', targets[0][3]),
                        (42, ops_module.ORDINARY, ops_module.DATABASE, "2026-10-24 12:00:00';DROP")):
            with self.subTest(changed=changed), self.assertRaises(Refused):
                ops_module.termination_stage((changed,), resource_kind='serverless',
                    principal_oids={'admin_oid': 700, 'test_oid': 701})

    def test_serverless_idle_requires_zero_and_known_readers(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = adapter(directory)
            ops.resource_kind = 'serverless'
            ops.principals_quiescent = True
            calls = []
            ops.final_zero = lambda end: calls.append('zero')
            ops.native.verify_idle = lambda end: calls.append('metadata') or True
            self.assertTrue(ops.verify_idle(300))
            self.assertEqual(calls, ['zero', 'metadata'])
            ops.readers_known = False
            with self.assertRaises(Refused):
                ops.verify_idle(300)
            self.assertEqual(calls, ['zero', 'metadata'])

    def available_adapter(self, directory):
        ops = adapter(directory)
        terminal = Path(directory) / 'terminal.json'
        terminal.write_text(json.dumps({'cleanup_complete': True, 'readers_known': True,
            'private_inputs_closed': True, 'SQL_unknown': False, 'paused': False}))
        ops.approval['continue_available_cluster_from'] = {
            'path': str(terminal), 'sha256': hashlib.sha256(terminal.read_bytes()).hexdigest()}
        cluster = {'ClusterIdentifier': 'mock-exact-cluster', 'ClusterCreateTime': 'mock-birth',
                   'Endpoint': {'Address': 'mock.invalid', 'Port': 5439},
                   'ClusterStatus': 'available', 'ClusterAvailabilityStatus': 'Available',
                   'VpcSecurityGroups': [{'VpcSecurityGroupId': 'sg-mock'}],
                   'ClusterParameterGroups': [{'ParameterGroupName': 'tls-mock'}]}
        groups = [{'GroupId': 'sg-mock', 'IpPermissions': []}]
        ops.preparation_manifest['security_groups'] = groups
        ops.native.identity = mock.Mock()
        ops.native.cluster = mock.Mock(return_value=cluster)
        ops.native.network_guard = mock.Mock()
        ops.native.security_paused = mock.Mock()
        ops.native.resume_available = mock.Mock()
        ops.native.aws = mock.Mock(side_effect=[{'SecurityGroups': groups},
            {'Parameters': [{'ParameterName': 'require_ssl', 'ParameterValue': 'true'}]}])
        ops.final_zero = mock.Mock(return_value={'complete': True})
        return ops

    def test_normal_paused_security_and_resume_path_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = adapter(directory)
            ops.native.security_paused = mock.Mock()
            ops.native.resume_available = mock.Mock()
            ops.final_zero = mock.Mock()
            ops.security_paused(300)
            ops.final_zero.assert_not_called()
            ops.resume_available(300)
            ops.native.security_paused.assert_called_once_with()
            ops.native.resume_available.assert_called_once_with(300)
            ops.final_zero.assert_called_once_with(300)
            self.assertEqual(ops.security_status, 'paused')

    def test_explicit_available_continuation_validates_security_then_rechecks_before_SQL(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = self.available_adapter(directory)
            ops.security_paused(300)
            ops.native.identity.assert_called_once_with(300)
            ops.native.network_guard.assert_called_once_with(
                ops.preparation_manifest['security_groups'], 300)
            ops.final_zero.assert_not_called()
            self.assertEqual(ops.security_status, 'available')
            ops.resume_available(300)
            self.assertEqual(ops.native.cluster.call_count, 2)
            ops.native.security_paused.assert_not_called()
            ops.native.resume_available.assert_not_called()
            ops.final_zero.assert_called_once_with(300)
            receipt = json.loads((Path(directory) / 'available-cluster-security.json').read_text())
            self.assertEqual(receipt['cluster_status'], 'available')
            self.assertNotIn('paused', receipt)

    def test_available_continuation_refuses_unclosed_recovery_or_changed_security(self):
        for failure in ('pin', 'cleanup', 'network', 'TLS', 'changed_cluster', 'unverified'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                ops = self.available_adapter(directory)
                if failure == 'pin':
                    ops.approval['continue_available_cluster_from']['sha256'] = '0' * 64
                elif failure == 'cleanup':
                    terminal = Path(directory) / 'terminal.json'
                    terminal.write_text('{}')
                    ops.approval['continue_available_cluster_from']['sha256'] = hashlib.sha256(
                        terminal.read_bytes()).hexdigest()
                elif failure == 'network':
                    ops.native.network_guard.side_effect = Refused('network_changed')
                elif failure == 'TLS':
                    ops.native.aws.side_effect = [
                        {'SecurityGroups': ops.preparation_manifest['security_groups']},
                        {'Parameters': [{'ParameterName': 'require_ssl', 'ParameterValue': 'false'}]}]
                with self.assertRaises(Refused):
                    if failure == 'unverified':
                        ops.resume_available(300)
                    else:
                        ops.security_paused(300)
                        if failure == 'changed_cluster':
                            ops.native.cluster.return_value = {
                                **ops.native.cluster.return_value, 'ClusterStatus': 'pausing'}
                        ops.resume_available(300)
                ops.final_zero.assert_not_called()
                ops.native.resume_available.assert_not_called()
                if failure in ('pin', 'cleanup'):
                    ops.native.identity.assert_not_called()

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
                'role': 'test', 'stage': 'synthetic'}, 300)
            self.assertEqual((raw, pid), ('1\n', 123))
            self.assertEqual((ops.helpers, ops.statements, ops.logins), (1, 4, 1))
            argv, kwargs = ops.native.calls[0]
            self.assertEqual(kwargs['seconds'], 120)
            self.assertEqual(kwargs['env']['PGCONNECT_TIMEOUT'], '10')
            self.assertIn(b'SET statement_timeout=15000;', kwargs['input_bytes'])
            self.assertEqual(kwargs['env']['PGSSLMODE'], 'verify-full')
            self.assertNotIn('PGPASSWORD', kwargs['env'])
            self.assertNotIn('mock-password', str(argv))

    def test_SQL_helper_uses_remaining_deadline_and_accepts_timely_completion(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = adapter(directory)
            clock = [100.25]
            ops.now = lambda: clock[0]
            owned = ops.native.owned_process

            def complete(argv, **kwargs):
                result = owned(argv, **kwargs)
                clock[0] += 20
                return result

            ops.native.owned_process = complete
            self.assertEqual(ops._sql({'query': 'SELECT 1;', 'statements': 1,
                'role': 'test', 'stage': 'synthetic'}, 150), ('1\n', 123))
            self.assertEqual(ops.native.calls[0][1]['seconds'], 47)
            self.assertIs(type(ops.native.calls[0][1]['seconds']), int)
            self.assertFalse(ops.sql_fenced)

    def test_collection_process_longer_than_old_ceiling_succeeds_or_times_out_reaped(self):
        for timeout in (False, True):
            with self.subTest(timeout=timeout), tempfile.TemporaryDirectory() as directory:
                ops = adapter(directory)
                process = mock.Mock(pid=12345, returncode=None)

                def wait(timeout):
                    if should_timeout:
                        raise subprocess.TimeoutExpired('/mock/cmake', timeout)
                    process.returncode = 0
                    return 0

                should_timeout = timeout
                process.wait.side_effect = wait
                process.poll.side_effect = lambda: process.returncode

                def stop(child):
                    if child.returncode is None:
                        child.returncode = -15

                with mock.patch.object(ops_module.subprocess, 'Popen', return_value=process), \
                     mock.patch.object(ops_module, '_stop_group', side_effect=stop) as stopper:
                    ops._group_absent = lambda pid: True
                    result = ops.run_process(['/mock/cmake'], env=dict(ops_module.BASE_ENV),
                        deadline=3700, output=Path(directory) / 'process.log', umask=0o077)
                process.wait.assert_called_once_with(timeout=3598)
                stopper.assert_called_once_with(process)
                self.assertEqual(result.timed_out, timeout)
                self.assertTrue(result.reaped and result.group_absent)
                self.assertEqual(len(ops.processes), 1)

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
