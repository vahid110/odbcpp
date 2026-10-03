"""Offline exact-type composition tests; cloud and credential exchange forbidden."""
from contextlib import ExitStack
from datetime import timedelta
import unittest
from unittest.mock import patch

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_live as live
from tools.redshift import pilot_iam_credentials as iam
from tools.redshift import pilot_recovery as r
from tools.redshift import pilot_type_live as launch
from tools.redshift import pilot_type_qualification as ledger
from tools.redshift import pilot_window_live as windows
from tools.redshift import test_pilot_type_qualification as accounting_fixture
from tools.redshift import test_pilot_continuation_live as runner_fixture
from tools.redshift.pilot_preflight import Blocked


class TypeLiveTests(unittest.TestCase):
    def setUp(self):
        self.accounting = accounting_fixture.TypeQualificationTests(); self.accounting.setUp()
        self.addCleanup(self.accounting.doCleanups)
        self.runner = runner_fixture.ContinuationCompositionTests(); self.runner.setUp()
        self.addCleanup(self.runner.doCleanups)
        self.f = self.accounting.f; self.c = self.runner
        self.c.f = self.f; self.c.directory = self.f.path.parent; self.c.sequence = 10
        self.c.write('bootstrap-anchor.json', self.f.anchor.record(self.f.now))
        self.c.write('database-config.json', self.c.config.config)
        review = self.accounting.review(); review['source_code_digest'] = launch.code_digest()
        with self.accounting.session() as session:
            self.f.passed(session.initialize(r.digest(self.accounting.source), review))
        self.c.request.update(sequence=10, profile='types', observed_at=self.f.stamp(self.f.now),
            hard_deadline=self.accounting.c.current.hard_deadline,
            state_digest=r.digest(self.f.path.read_text()), code_digest=launch.code_digest())
        self.c.request['admission'].update(account_id=self.f.anchor.account_id,
            earliest_billable_start=self.f.anchor.earliest_billable_start,
            observed_at=self.f.stamp(self.f.now))
        self.c.write_request()
        outer = self
        base = self.c.Window
        class Window(base):
            def sql(self, query, *, admin):
                outer.assertFalse(admin); outer.assertEqual(query, 'SHOW statement_timeout;')
                outer.c.events.append('timeout'); return '15000'
            def driver(self, manifest, cases, **kwargs):
                outer.assertEqual(kwargs, {}); outer.assertIsNone(self.cleanup_principal)
                outer.assertEqual(outer.f.read()[b.OVERLAY]['attempts'][-1]['phase'], 'active')
                outer.c.events.append(tuple(cases))
                if outer.c.failure == 'identity': raise Blocked('driver_baseline_failed')
                return dict(cases=list(cases), passed=len(cases), failed=0)
        self.Window = Window

    def execute(self, controls=None):
        real = ledger.type_qualification_session
        def session(path, anchor): return real(path, anchor, clock=lambda:self.f.now)
        with ExitStack() as stack:
            stack.enter_context(patch.object(windows, 'utcnow', side_effect=lambda:self.f.now))
            stack.enter_context(patch.object(ledger, 'type_qualification_session', side_effect=session))
            stack.enter_context(patch.object(windows, 'fresh_controls', side_effect=controls))
            stack.enter_context(patch.object(live, 'Window', self.Window))
            stack.enter_context(patch.object(iam, 'acquire', side_effect=AssertionError('IAM forbidden')))
            stack.enter_context(patch.object(live, 'aws', side_effect=AssertionError('cloud forbidden')))
            return launch.execute(self.f.path.parent)

    def test_identity_then_exact_five_cases_and_cleanup_once_no_replay(self):
        result = self.execute()
        self.assertEqual(result['status'], 'passed'); self.assertTrue(result['cleanup_verified'])
        self.assertEqual(self.c.events, ['reserved', 'cleanup', 'timeout', live.IDENTITY,
            tuple(ledger.INVENTORY[1:]), 'cleanup'])
        self.assertNotIn('types', windows.PROFILES)
        with self.assertRaises(Blocked): self.execute()

    def test_changed_state_during_controls_never_reserves_or_dispatches(self):
        def controls(*args): self.f.path.write_bytes(self.f.path.read_bytes()+b' ')
        with self.assertRaisesRegex(Blocked, 'continuation_source_changed'): self.execute(controls)
        self.assertEqual(self.c.events, [])

    def test_identity_failure_skips_type_cases_and_cleans_retained_reservation(self):
        self.c.failure = 'identity'; result = self.execute()
        self.assertEqual(result['reason'], 'driver_baseline_failed')
        self.assertTrue(result['cleanup_verified'])
        self.assertNotIn(tuple(ledger.INVENTORY[1:]), self.c.events)
        self.assertEqual(len(self.f.read()[b.OVERLAY]['attempts']), 1)

    def test_wrong_scope_or_code_rejected_before_controls(self):
        before = self.f.path.read_bytes()
        for key, value in [('code_digest', '0'*64), ('sequence', 9), ('profile', 'iam')]:
            old = self.c.request[key]; self.c.request[key] = value; self.c.write_request()
            with self.assertRaises(Blocked): self.execute()
            self.assertEqual(self.c.events, []); self.assertEqual(before, self.f.path.read_bytes())
            self.c.request[key] = old

    def test_cleanup_must_fit_original_period(self):
        horizon = b._time(self.f.anchor.earliest_billable_start, self.f.now)+timedelta(days=1)
        self.f.now = horizon-timedelta(seconds=180)
        self.c.request.update(observed_at=self.f.stamp(self.f.now),
            hard_deadline=self.f.stamp(horizon-timedelta(seconds=59)))
        self.c.request['admission']['observed_at'] = self.f.stamp(self.f.now)
        self.c.write_request()
        with self.assertRaisesRegex(Blocked, 'type_cleanup_outside_original_horizon'): self.execute()
        self.assertEqual(self.c.events, [])

    def test_stored_review_code_binding_checked_before_controls(self):
        state = self.f.read(); overlay = state[b.OVERLAY]
        overlay['review']['source_code_digest'] = '0'*64
        overlay['review_digest'] = r.digest(b._encode(overlay['review']))
        self.f.write(state)
        self.c.request['state_digest'] = r.digest(self.f.path.read_text()); self.c.write_request()
        with self.assertRaisesRegex(Blocked, 'amendment_code_review_mismatch'): self.execute()
        self.assertEqual(self.c.events, [])

if __name__ == '__main__': unittest.main()
