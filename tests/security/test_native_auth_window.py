import traceback
import unittest
from tools.redshift.native_auth_window import NativeAuthWindow, WindowBlocked
from tools.redshift.native_auth_child import CASE
from tools.redshift.native_auth_cleanup import SqlReply


def selection():
    return {'ACCOUNT': '123456789012', 'REGION': 'eu-north-1', 'CLUSTER': 'fixture',
            'DATABASE': 'odbcpp_pilot', 'REQUESTED_DB_USER': 'fixture_user',
            'EXPECTED_SQL_USER': 'IAM:fixture_user', 'DB_HOST': 'fixture.invalid',
            'DB_CA_FILE': '/synthetic/ca', 'PROFILE': 'fixture',
            'CREDENTIALS_FILE': '/synthetic/credentials', 'CONFIG_FILE': '/synthetic/config',
            'SOURCE_IDENTITY': 'synthetic-source', 'SOURCE_GENERATION': 'g1'}


class Setup:
    def __init__(self):
        self.now = 100
        self.reads = 0
        self.calls = []
        self.child_calls = []
        self.replies = [SqlReply(True, 'f'), SqlReply(True, 'f'),
                        SqlReply(True, ''), SqlReply(True, 'f'), SqlReply(True, 'f')]
        self.child_reply = {'case': CASE, 'passed': 1}
        self.sql_hook = None
        self.child_hook = None
        self.clock_hook = None
        self.window = NativeAuthWindow(executable='/synthetic/test', expected_sha256='a' * 64,
            selection=selection(), directory='/synthetic/output', sql=self.sql,
            clock=self.clock, runner=self.runner)

    def clock(self):
        self.reads += 1
        if self.clock_hook:
            self.clock_hook()
        return self.now

    def sql(self, step, query, timeout):
        self.calls.append((step, query, timeout))
        if self.sql_hook:
            self.sql_hook(step)
        value = self.replies.pop(0)
        if isinstance(value, Exception):
            raise value
        return value

    def runner(self, *args):
        self.child_calls.append(args)
        if self.child_hook:
            self.child_hook()
        if isinstance(self.child_reply, Exception):
            raise self.child_reply
        return self.child_reply


class NativeAuthWindowTests(unittest.TestCase):
    def test_happy_exact_one_case_after_preflight_then_independent_cleanup(self):
        s = Setup()
        result = s.window.run()
        self.assertEqual(result, {'status': 'pass', 'primary': None, 'child': 'passed',
                                 'cleanup': 'observed_zero', 'case': CASE})
        self.assertEqual([c[0] for c in s.calls], ['preflight_sessions', 'preflight_queries',
            'cleanup_pids', 'cleanup_sessions', 'cleanup_queries'])
        self.assertEqual(len(s.child_calls), 1)
        self.assertEqual(s.child_calls[0], ('/synthetic/test', 'a' * 64, selection(), '/synthetic/output'))
        for _, query, _ in s.calls:
            self.assertIn("user_name='IAM:fixture_user'", query)
            self.assertIn("db_name='odbcpp_pilot'", query)
        before = (s.reads, len(s.calls), len(s.child_calls))
        self.assertEqual(s.window.run()['primary'], 'already_consumed')
        self.assertEqual((s.reads, len(s.calls), len(s.child_calls)), before)

    def test_preexisting_or_invalid_preflight_never_launches_or_terminates(self):
        for replies in ([SqlReply(True, 't')], [SqlReply(True, 'f'), SqlReply(True, 't')],
                        [SqlReply(False, 'private-server-detail')], [None]):
            s = Setup()
            s.replies = replies
            result = s.window.run()
            self.assertEqual(result['primary'], 'preflight_failed')
            self.assertEqual(result['cleanup'], 'not_attempted')
            self.assertEqual(s.child_calls, [])
            self.assertTrue(all(c[0].startswith('preflight_') for c in s.calls))
            self.assertFalse(any('pg_terminate_backend' in c[1] for c in s.calls))

    def test_child_exception_and_malformed_result_still_cleanup_preserve_primary(self):
        for reply, expected in ((RuntimeError('private-runner-detail'), 'child_failed'),
                                ({'case': CASE, 'passed': True}, 'child_result_invalid'),
                                ({'case': 'foreign', 'passed': 1}, 'child_result_invalid')):
            s = Setup()
            s.child_reply = reply
            result = s.window.run()
            self.assertEqual(result['status'], 'blocked')
            self.assertEqual(result['primary'], expected)
            self.assertEqual(result['cleanup'], 'observed_zero')
            self.assertEqual(len(s.child_calls), 1)
            self.assertEqual(s.calls[-1][0], 'cleanup_queries')
            self.assertNotIn('private', repr(result))

    def test_cleanup_failure_cannot_heal_child_or_retry(self):
        for child in ({'case': CASE, 'passed': 1}, RuntimeError('private-child')):
            s = Setup()
            s.child_reply = child
            s.replies = [SqlReply(True, 'f'), SqlReply(True, 'f'),
                         SqlReply(True, '5'), SqlReply(True, 'f')]
            result = s.window.run()
            self.assertEqual(result['status'], 'blocked')
            self.assertEqual(result['cleanup'], 'remote_cleanup_failed')
            self.assertEqual(result['primary'], None if isinstance(child, dict) else 'child_failed')
            self.assertEqual(s.calls[-1][0], 'terminate_5')
            before = (s.reads, len(s.calls), len(s.child_calls))
            self.assertEqual(s.window.run()['primary'], 'already_consumed')
            self.assertEqual((s.reads, len(s.calls), len(s.child_calls)), before)

    def test_entry_cutoffs_headroom_and_late_child_do_not_reset_cleanup_budget(self):
        for entry in (100.1, 100.25):
            fractional = Setup()
            fractional.now = entry
            self.assertEqual(fractional.window.run()['status'], 'pass')
        s = Setup()
        s.sql_hook = lambda step: setattr(s, 'now', 154) if step == 'preflight_queries' else None
        self.assertEqual(s.window.run()['primary'], 'execution_headroom')  # exactly66s
        self.assertFalse(s.child_calls)
        self.assertEqual(len(s.calls), 2)
        s = Setup()
        s.child_hook = lambda: setattr(s, 'now', 220)  # exact original execution cutoff
        result = s.window.run()
        self.assertEqual(result['primary'], 'execution_deadline')
        self.assertEqual(result['cleanup'], 'observed_zero')
        self.assertEqual(len(s.child_calls), 1)
        self.assertEqual([c[2] for c in s.calls[-3:]], [15, 15, 15])
        s = Setup()
        s.child_hook = lambda: setattr(s, 'now', 280)  # exact original cleanup cutoff
        result = s.window.run()
        self.assertEqual(result['primary'], 'execution_deadline')
        self.assertEqual(result['cleanup'], 'remote_cleanup_failed')
        self.assertEqual(len(s.calls), 2)

    def test_cleanup_crossing_cutoff_cannot_publish_observed_zero(self):
        for step in ('cleanup_pids', 'cleanup_queries'):
            s = Setup()
            s.sql_hook = lambda current: setattr(s, 'now', 280) if current == step else None
            result = s.window.run()
            self.assertEqual(result['status'], 'blocked')
            self.assertEqual(result['cleanup'], 'remote_cleanup_failed')
            self.assertEqual(s.calls[-1][0], step)

    def test_nonhealing_clock_failure_blocks_cleanup_callbacks(self):
        for bad, expected in ((99, 'clock_rollback'), (float('nan'), 'clock_failed')):
            s = Setup()
            s.child_hook = lambda: setattr(s, 'now', bad)
            result = s.window.run()
            self.assertEqual(result['primary'], expected)
            self.assertEqual(result['cleanup'], expected)
            self.assertEqual(len(s.calls), 2)
            before = s.reads
            s.now = 101
            self.assertEqual(s.window.run()['primary'], 'already_consumed')
            self.assertEqual(s.reads, before)

    def test_safe_tracebacks_reentry_and_reader_exception(self):
        s = Setup()
        s.clock_hook = lambda: (_ for _ in ()).throw(RuntimeError('private-clock-secret'))
        result = s.window.run()
        self.assertEqual(result['primary'], 'clock_failed')
        self.assertEqual(s.calls, [])
        self.assertNotIn('private', repr(result))
        s = Setup()
        def reenter():
            try:
                s.window.run()
            except WindowBlocked as error:
                self.assertTrue(error.__suppress_context__)
                self.assertNotIn('private', ''.join(traceback.format_exception(error)))
        s.child_hook = reenter
        result = s.window.run()
        self.assertEqual(result['primary'], 'reentrant')
        self.assertEqual(result['cleanup'], 'reentrant')
        self.assertEqual(len(s.calls), 2)
        with self.assertRaises(WindowBlocked) as caught:
            NativeAuthWindow(executable=None, expected_sha256=None, selection={'private': 'secret'},
                directory=None, sql=lambda *_: None, clock=lambda: 1, runner=lambda *_: None)
        self.assertTrue(caught.exception.__suppress_context__)
        self.assertNotIn('secret', ''.join(traceback.format_exception(caught.exception)))
