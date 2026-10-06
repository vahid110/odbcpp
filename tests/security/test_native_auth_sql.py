import subprocess
import traceback
import unittest
from unittest.mock import patch
from tools.redshift.native_auth_sql import NativeAuthSql, SqlBlocked
from tools.redshift.native_auth_cleanup import NativeAuthCleanup, SqlReply


CONFIG = {'host': 'fixture.example.invalid', 'port': 5439, 'database': 'odbcpp_pilot',
          'user': 'fixture_admin', 'passfile': '/synthetic/admin.pgpass', 'ca_file': '/synthetic/ca'}
PRINCIPAL = 'IAM:fixture_user'
PREDICATE = "user_name='IAM:fixture_user' AND db_name='odbcpp_pilot' AND process <> pg_backend_pid()"
PIDS = 'SELECT process FROM stv_sessions WHERE ' + PREDICATE + ';'


def callback(clock=lambda: 100, config=None, **extra):
    return NativeAuthSql(config=dict(CONFIG) if config is None else config,
                         principal=PRINCIPAL, executable='/synthetic/psql',
                         expected_sha256='a' * 64, clock=clock, **extra)


def result(output=b'7\n', returncode=0):
    return subprocess.CompletedProcess(['/synthetic/psql'], returncode, stdout=b'native_auth_guard_allowed\n' + output)


class NativeAuthSqlTests(unittest.TestCase):
    def setUp(self):
        self.private_patch = patch('tools.redshift.native_auth_sql.validate_private_file')
        self.private = self.private_patch.start()
        self.addCleanup(self.private_patch.stop)

    def test_real_subprocess_budget_verified_tls_closed_environment_and_no_argv_secret(self):
        times = iter([100, 101, 102])
        sql = callback(clock=lambda: next(times))
        with patch('tools.redshift.native_auth_sql.verify_executable') as verify, patch(
                'tools.redshift.native_auth_sql.subprocess.run', return_value=result()) as run:
            self.assertEqual(sql('cleanup_pids', PIDS, 5), SqlReply(True, '7\n'))
        verify.assert_called_once()
        argv = run.call_args.args[0]
        kwargs = run.call_args.kwargs
        self.assertEqual(argv, ['/synthetic/psql', '-X', '-w', '-qAt', '-v', 'ON_ERROR_STOP=1'])
        self.assertEqual(kwargs['timeout'], 4)
        self.assertTrue(kwargs['close_fds'])
        self.assertFalse(kwargs['check'])
        self.assertEqual(kwargs['stderr'], subprocess.DEVNULL)
        self.assertNotIn('synthetic-private-password', repr(argv) + repr(kwargs['input']))
        self.assertEqual(kwargs['input'], (
            "SET statement_timeout=4000;\n\\set native_auth_guard false\n"
            "SELECT CASE WHEN current_database()='odbcpp_pilot' AND TRIM(current_user)='fixture_admin' "
            "AND EXISTS(SELECT 1 FROM pg_user WHERE usename=current_user AND usesuper) "
            "THEN 'true' ELSE 'false' END AS native_auth_guard\n\\gset\n"
            "\\if :native_auth_guard\n\\echo native_auth_guard_allowed\n" + PIDS[:-1] +
            " LIMIT 9;\n\\else\n\\echo native_auth_guard_refused\n\\endif\n").encode())
        env = kwargs['env']
        self.assertEqual(env['PGSSLMODE'], 'verify-full')
        self.assertEqual(env['PGSSLROOTCERT'], '/synthetic/ca')
        self.assertEqual(env['PGGSSENCMODE'], 'disable')
        self.assertEqual(env['PGSSLCERTMODE'], 'disable')
        self.assertEqual(env['PGPASSFILE'], '/synthetic/admin.pgpass')
        self.assertNotIn('PGPASSWORD', env)
        self.private.assert_called_once_with('/synthetic/admin.pgpass')
        self.assertFalse({'HOME', 'PGSERVICE', 'PGOPTIONS', 'AWS_PROFILE', 'HTTP_PROXY'} & set(env))

    def test_actual_cleanup_patterns_compose_with_callback_without_real_process(self):
        sql = callback()
        helper = NativeAuthCleanup(principal=PRINCIPAL, database='odbcpp_pilot',
                                   cutoff=200, clock=lambda: 100, sql=sql)
        responses = [result(b'f\n'), result(b'f\n'), result(), result(b't\n'), result(b'f\n'), result(b'f\n')]
        with patch('tools.redshift.native_auth_sql.verify_executable'), patch(
                'tools.redshift.native_auth_sql.subprocess.run', side_effect=responses) as run:
            self.assertEqual(helper.preflight()['status'], 'pass')
            self.assertEqual(helper.cleanup()['terminated'], 1)
        self.assertEqual(run.call_count, 6)
        self.assertIn(b'SELECT pg_terminate_backend(7);', run.call_args_list[3].kwargs['input'])

    def test_timeout_and_secret_bearing_errors_return_safe_failed_reply(self):
        for error in (subprocess.TimeoutExpired('psql', 2, output=b'private-error-token'),
                      RuntimeError('synthetic-private-source-error')):
            with patch('tools.redshift.native_auth_sql.verify_executable'), patch(
                    'tools.redshift.native_auth_sql.subprocess.run', side_effect=error) as run:
                observed = callback()('cleanup_pids', PIDS, 2)
            self.assertEqual(observed, SqlReply(False))
            self.assertNotIn('private', repr(observed))
            self.assertEqual(run.call_args.kwargs['timeout'], 2)
        with patch('tools.redshift.native_auth_sql.verify_executable', side_effect=RuntimeError('private-artifact')), patch(
                'tools.redshift.native_auth_sql.subprocess.run') as run:
            self.assertEqual(callback()('cleanup_pids', PIDS, 2), SqlReply(False))
            run.assert_not_called()

    def test_unknown_queries_step_pid_and_timeout_never_spawn(self):
        with patch('tools.redshift.native_auth_sql.verify_executable') as verify, patch(
                'tools.redshift.native_auth_sql.subprocess.run') as run:
            for step, query in (('cleanup_pids', PIDS + ' DROP DATABASE x;'),
                                ('identity', 'SELECT current_user;'),
                                ('terminate_0', 'SELECT pg_terminate_backend(0);'),
                                ('terminate_2147483648', 'SELECT pg_terminate_backend(2147483648);'),
                                ('terminate_8', 'SELECT pg_terminate_backend(7);'),
                                ('cleanup_pids', PIDS.replace('IAM:', 'IAMR:'))):
                with self.assertRaisesRegex(SqlBlocked, '^invalid_statement$'):
                    callback()(step, query, 2)
            for seconds in (True, 0, -.1, .0009, 16, float('nan'), float('inf'), 10 ** 1000):
                with self.assertRaisesRegex(SqlBlocked, '^invalid_timeout$'):
                    callback()('cleanup_pids', PIDS, seconds)
            verify.assert_not_called()
            run.assert_not_called()

    def test_fractional_timeout_and_preparation_or_postreply_deadline_refuse(self):
        with patch('tools.redshift.native_auth_sql.verify_executable'), patch(
                'tools.redshift.native_auth_sql.subprocess.run', return_value=result()) as run:
            self.assertTrue(callback()('cleanup_pids', PIDS, .25).succeeded)
            self.assertGreater(run.call_args.kwargs['timeout'], 0)
            self.assertLess(run.call_args.kwargs['timeout'], .25)
        for values, expected_calls in (([100, 102], 0), ([100, 100, 102], 1)):
            times = iter(values)
            with patch('tools.redshift.native_auth_sql.verify_executable'), patch(
                    'tools.redshift.native_auth_sql.subprocess.run', return_value=result()) as run:
                self.assertEqual(callback(clock=lambda: next(times))('cleanup_pids', PIDS, 2), SqlReply(False))
                self.assertEqual(run.call_count, expected_calls)

    def test_reply_validation_never_returns_server_diagnostics(self):
        for observed in (None, result(b'private-server-password'), result(b'7\n', 1),
                         result(b'\xff'), result(b'7' * 129), result(b'2147483648\n'),
                         result(b'\n'), result(b'7\n\n'), result(b't')):
            with patch('tools.redshift.native_auth_sql.verify_executable'), patch(
                    'tools.redshift.native_auth_sql.subprocess.run', return_value=observed):
                self.assertEqual(callback()('cleanup_pids', PIDS, 2), SqlReply(False))
        with patch('tools.redshift.native_auth_sql.verify_executable'), patch(
                'tools.redshift.native_auth_sql.subprocess.run', return_value=result(b'1\n2\n3\n4\n5\n6\n7\n8\n9\n')):
            self.assertTrue(callback()('cleanup_pids', PIDS, 2).succeeded)  # helper refuses >8 before termination

    def test_config_refuses_tls_bypass_fallback_and_secret_traceback(self):
        for key, value in (('host', '/socket'), ('host', 'host,foreign'), ('ca_file', 'relative'),
                           ('port', True), ('database', 'foreign'), ('passfile', ''), ('passfile', '/x\0y')):
            config = dict(CONFIG, **{key: value})
            with self.assertRaises(SqlBlocked) as caught:
                callback(config=config)
            self.assertTrue(caught.exception.__suppress_context__)
            self.assertNotIn('synthetic-private-password', ''.join(traceback.format_exception(caught.exception)))
        with self.assertRaises(SqlBlocked):
            callback(config=dict(CONFIG, sslmode='require'))
        with self.assertRaises(SqlBlocked):
            callback(config=dict(CONFIG, password='synthetic-private-password'))
        self.private.side_effect = RuntimeError('private-passfile-details')
        with patch('tools.redshift.native_auth_sql.verify_executable'), patch('tools.redshift.native_auth_sql.subprocess.run') as run:
            self.assertEqual(callback()('cleanup_pids', PIDS, 2), SqlReply(False))
            run.assert_not_called()

    def test_clock_failure_and_reentry_are_permanent_without_process_retry(self):
        times = iter([100, 99, 101, 102])
        sql = callback(clock=lambda: next(times))
        with patch('tools.redshift.native_auth_sql.verify_executable'), patch(
                'tools.redshift.native_auth_sql.subprocess.run') as run:
            self.assertEqual(sql('cleanup_pids', PIDS, 2), SqlReply(False))
            with self.assertRaisesRegex(SqlBlocked, '^clock_failed$'):
                sql('cleanup_pids', PIDS, 2)
            run.assert_not_called()
        owner = None
        def reader():
            with self.assertRaisesRegex(SqlBlocked, '^reentrant$'):
                owner('cleanup_pids', PIDS, 2)
            return 100
        owner = callback(clock=reader)
        with patch('tools.redshift.native_auth_sql.verify_executable'), patch(
                'tools.redshift.native_auth_sql.subprocess.run') as run:
            with self.assertRaises(SqlBlocked):
                owner('cleanup_pids', PIDS, 2)
            run.assert_not_called()


    def test_guard_refusal_missing_malformed_and_empty_output_fail_even_exit_zero(self):
        for raw in (b'native_auth_guard_refused\n', b'', b'7\n', b't\n',
                    b'native_auth_guard_allowed', b'native_auth_guard_refused\n7\n',
                    b'native_auth_guard_allowed\nnative_auth_guard_allowed\n'):
            with self.subTest(raw=raw), patch('tools.redshift.native_auth_sql.verify_executable'), patch(
                    'tools.redshift.native_auth_sql.subprocess.run', return_value=
                    subprocess.CompletedProcess([], 0, stdout=raw)) as run:
                self.assertEqual(callback()('cleanup_pids', PIDS, 2), SqlReply(False))
                self.assertEqual(run.call_count, 1)
        with patch('tools.redshift.native_auth_sql.verify_executable'), patch(
                'tools.redshift.native_auth_sql.subprocess.run', return_value=result(b'')):
            self.assertEqual(callback()('cleanup_pids', PIDS, 2), SqlReply(True, ''))

    def test_fixed_guard_precedes_termination_and_false_branch_contains_no_target(self):
        # This is a mock of the documented conditional semantics, not server proof.
        dispatched = []
        def psql(*args, **kwargs):
            script = kwargs['input'].decode('ascii')
            before, conditional = script.split('\\if :native_auth_guard\n')
            positive, negative = conditional.split('\\else\n')
            self.assertIn("current_database()='odbcpp_pilot'", before)
            self.assertIn("TRIM(current_user)='fixture_admin'", before)
            self.assertIn('usename=current_user AND usesuper', before)
            self.assertTrue(before.endswith('\\gset\n'))
            self.assertNotIn('pg_terminate_backend', before + negative)
            self.assertNotIn('\\quit', script)
            allowed = (identity[0] == 'odbcpp_pilot' and
                       identity[1] == 'fixture_admin' and identity[2] is True)
            if allowed:
                self.assertIn('SELECT pg_terminate_backend(7);', positive)
                dispatched.append(7)
                return result(b't\n')
            return subprocess.CompletedProcess([], 0, stdout=b'native_auth_guard_refused\n')
        for identity in (('foreign', 'fixture_admin', True),
                         ('odbcpp_pilot', 'other_admin', True),
                         ('odbcpp_pilot', 'fixture_admin', False),
                         ('odbcpp_pilot', 'fixture_admin', None),
                         ('odbcpp_pilot', 'fixture_admin', True)):
            with self.subTest(identity=identity), patch('tools.redshift.native_auth_sql.verify_executable'), patch(
                    'tools.redshift.native_auth_sql.subprocess.run', side_effect=psql):
                reply = callback()('terminate_7', 'SELECT pg_terminate_backend(7);', 2)
                self.assertEqual(reply, SqlReply(True, 't\n') if identity ==
                                 ('odbcpp_pilot', 'fixture_admin', True) else SqlReply(False))
        self.assertEqual(dispatched, [7])
