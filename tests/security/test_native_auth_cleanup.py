import unittest
import traceback
from tools.redshift.native_auth_cleanup import NativeAuthCleanup, CleanupBlocked, SqlReply


class Setup:
    def __init__(self, replies, times=None):
        self.replies = list(replies)
        self.times = list(times or [1] * 100)
        self.calls = []
        self.reads = 0
        self.hook = None
        self.owner = NativeAuthCleanup(principal='IAM:fixture_user', database='odbcpp_pilot',
                                       cutoff=10, sql=self.sql, clock=self.clock)

    def clock(self):
        self.reads += 1
        value = self.times.pop(0)
        if isinstance(value, Exception):
            raise value
        return value

    def sql(self, step, query, timeout):
        self.calls.append((step, query, timeout))
        if self.hook:
            self.hook()
        value = self.replies.pop(0)
        if isinstance(value, Exception):
            raise value
        return value


def yes(text):
    return SqlReply(True, text)


class NativeAuthCleanupTests(unittest.TestCase):
    def test_preflight_checks_two_families_and_never_terminates(self):
        s = Setup([yes('f\n'), yes('f')])
        self.assertEqual(s.owner.preflight()['status'], 'pass')
        self.assertEqual([c[0] for c in s.calls], ['preflight_sessions', 'preflight_queries'])
        for _, query, _ in s.calls:
            self.assertIn("user_name='IAM:fixture_user' AND db_name='odbcpp_pilot'", query)
            self.assertIn('<> pg_backend_pid()', query)
            self.assertNotIn('pg_terminate_backend', query)
            self.assertNotIn('IAMR:', query)
        self.assertIn("status <> 'Done'", s.calls[1][1])
        for replies in ([yes('t')], [yes('f'), yes('t')]):
            s = Setup(replies)
            with self.assertRaises(CleanupBlocked):
                s.owner.preflight()
            self.assertFalse(any('pg_terminate_backend' in q for _, q, _ in s.calls))

    def test_cleanup_exact_inventory_individual_termination_separate_verification(self):
        s = Setup([yes('2\n2147483647\n'), yes('t'), yes('t\n'), yes('f'), yes('f')])
        self.assertEqual(s.owner.cleanup(), {'status': 'pass', 'remote_activity': 'observed_zero', 'terminated': 2})
        self.assertEqual([c[0] for c in s.calls], ['cleanup_pids', 'terminate_2', 'terminate_2147483647', 'cleanup_sessions', 'cleanup_queries'])
        self.assertEqual(s.calls[1][1], 'SELECT pg_terminate_backend(2);')
        self.assertEqual(s.calls[2][1], 'SELECT pg_terminate_backend(2147483647);')
        self.assertNotIn('pg_terminate_backend', s.calls[0][1])
        for _, query, _ in (s.calls[0], s.calls[-2], s.calls[-1]):
            self.assertIn("user_name='IAM:fixture_user' AND db_name='odbcpp_pilot'", query)
        empty = Setup([yes(''), yes('f'), yes('f')])
        self.assertEqual(empty.owner.cleanup()['terminated'], 0)

    def test_malformed_overbound_and_duplicate_pids_refuse_before_termination(self):
        malformed = ['0', '-1', '01', '2147483648', '1\n1', '1\n', ' 1', '١',
                     '1\n\n2', 'x', '\n', '\n'.join(str(i) for i in range(1, 10)), '9' * 129]
        # A single final record terminator is supported, so '1\n' is valid.
        malformed.remove('1\n')
        for output in malformed:
            with self.subTest(output=output):
                s = Setup([yes(output)])
                with self.assertRaises(CleanupBlocked):
                    s.owner.cleanup()
                self.assertEqual(len(s.calls), 1)
        s = Setup([yes('\n'.join(str(i) for i in range(1, 9)))] + [yes('t')] * 8 + [yes('f'), yes('f')])
        self.assertEqual(s.owner.cleanup()['terminated'], 8)

    def test_partial_termination_and_nonzero_residual_never_pass_or_retry(self):
        for replies in ([yes('1\n2'), yes('t'), yes('f')],
                        [yes('1'), yes('t'), yes('t')],
                        [yes(''), yes('f'), yes('t')]):
            s = Setup(replies)
            with self.assertRaises(CleanupBlocked):
                s.owner.cleanup()
            before = (len(s.calls), s.reads)
            with self.assertRaises(CleanupBlocked):
                s.owner.cleanup()
            self.assertEqual((len(s.calls), s.reads), before)

    def test_original_cutoff_checks_before_each_dispatch_and_equality_after_reply(self):
        s = Setup([yes(''), yes('f'), yes('f')], [1, 2, 4, 5, 7, 8])
        self.assertEqual(s.owner.cleanup()['status'], 'pass')
        self.assertEqual([c[2] for c in s.calls], [9, 6, 3])
        for times, calls in (([10], 0), ([1, 10], 1), ([1, 2, 10], 1)):
            s = Setup([yes(''), yes('f')], times)
            with self.assertRaisesRegex(CleanupBlocked, '^deadline_exhausted$'):
                s.owner.cleanup()
            self.assertEqual(len(s.calls), calls)

    def test_clock_fault_is_permanent_after_recovery_and_no_effects(self):
        for bad, expected in ((0, 'clock_rollback'), (float('nan'), 'clock_failed'),
                              (float('inf'), 'clock_failed'), (True, 'clock_failed'),
                              (RuntimeError('secret-clock-detail'), 'clock_failed')):
            s = Setup([yes('')], [1, bad, 2, 3])
            with self.assertRaisesRegex(CleanupBlocked, '^' + expected + '$'):
                s.owner.cleanup()
            before = (s.reads, len(s.calls))
            with self.assertRaisesRegex(CleanupBlocked, '^' + expected + '$'):
                s.owner.preflight()
            self.assertEqual((s.reads, len(s.calls)), before)

    def test_invalid_selection_bad_replies_and_exception_errors_are_redacted(self):
        for principal in ("x' OR true --", '', 'x\n', 'x' * 129, '用户'):
            with self.assertRaisesRegex(CleanupBlocked, '^invalid_selection$'):
                NativeAuthCleanup(principal=principal, database='odbcpp_pilot', cutoff=10,
                                  sql=lambda *_: None, clock=lambda: 1)
        for cutoff in (True, float('nan'), float('inf'), 10 ** 1000):
            with self.assertRaisesRegex(CleanupBlocked, '^invalid_selection$'):
                NativeAuthCleanup(principal='fixture', database='odbcpp_pilot', cutoff=cutoff,
                                  sql=lambda *_: None, clock=lambda: 1)
        with self.assertRaisesRegex(CleanupBlocked, '^invalid_selection$'):
            NativeAuthCleanup(principal='fixture', database='foreign', cutoff=10,
                              sql=lambda *_: None, clock=lambda: 1)
        for reply in (None, 'f', SqlReply(1, 'f'), SqlReply(False, 'secret-server-detail'),
                      SqlReply(True, None), yes('false'), yes(' f'), yes('f\n\n'),
                      RuntimeError('secret-source-detail')):
            s = Setup([reply])
            with self.assertRaises(CleanupBlocked) as caught:
                s.owner.preflight()
            self.assertNotIn('secret', str(caught.exception))
            self.assertEqual(len(s.calls), 1)

    def test_reentry_latches_and_success_is_single_use(self):
        s = Setup([yes('')])
        def reenter():
            with self.assertRaisesRegex(CleanupBlocked, '^reentrant$'):
                s.owner.cleanup()
        s.hook = reenter
        with self.assertRaisesRegex(CleanupBlocked, '^reentrant$'):
            s.owner.cleanup()
        self.assertEqual(len(s.calls), 1)
        self.assertEqual(s.reads, 1)
        s = Setup([yes('f'), yes('f'), yes(''), yes('f'), yes('f')])
        self.assertEqual(s.owner.preflight()['status'], 'pass')
        self.assertEqual(s.owner.cleanup()['status'], 'pass')
        before = (len(s.calls), s.reads)
        with self.assertRaisesRegex(CleanupBlocked, '^already_consumed$'):
            s.owner.cleanup()
        self.assertEqual((len(s.calls), s.reads), before)


class CleanupExceptionPrivacyTests(unittest.TestCase):
    def test_clock_and_sql_tracebacks_suppress_original_secret_context(self):
        for source in ('clock', 'sql'):
            secret = 'synthetic-private-' + source + '-exception'
            setup = Setup([RuntimeError(secret)] if source == 'sql' else [yes('')],
                          [RuntimeError(secret)] if source == 'clock' else [1] * 10)
            try:
                setup.owner.cleanup()
            except CleanupBlocked as error:
                self.assertTrue(error.__suppress_context__)
                rendered = ''.join(traceback.format_exception(error))
                self.assertNotIn(secret, rendered)
                self.assertNotIn('RuntimeError: synthetic-private-', rendered)
                self.assertEqual(str(error), 'clock_failed' if source == 'clock' else 'sql_failed')
            else:
                self.fail('expected safe refusal')
            before = (setup.reads, len(setup.calls))
            try:
                setup.owner.preflight()
            except CleanupBlocked as error:
                self.assertTrue(error.__suppress_context__)
                self.assertNotIn(secret, ''.join(traceback.format_exception(error)))
            else:
                self.fail('permanent failure did not refuse')
            self.assertEqual((setup.reads, len(setup.calls)), before)
