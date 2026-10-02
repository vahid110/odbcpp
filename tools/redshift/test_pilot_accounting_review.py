"""Synthetic read-only review tests; no AWS, paid SQL or private state."""
import copy
import os
from dataclasses import replace
from datetime import timedelta
import unittest
from unittest.mock import patch

from tools.redshift import pilot_accounting_review as review
from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_preflight as r0
from tools.redshift import pilot_recovery as r
from tools.redshift import test_pilot_continuation as fixture


class AccountingReviewTests(unittest.TestCase):
    def setUp(self):
        self.continuation = fixture.ContinuationTests(); self.continuation.setUp()
        self.addCleanup(self.continuation.doCleanups)
        self.f = self.continuation.f; self.c = self.continuation
        with self.c.session() as session:
            self.f.passed(session.initialize(r.digest(self.c.source)))
            for _ in (7, 8):
                self.f.passed(session.reserve(self.c.evidence(), self.c.current))
                self.f.passed(session.transition('active', self.c.cleanup()))
                self.f.passed(session.transition('cleaned_pending_billing', self.c.cleanup()))
                self.c.previous_cleanup = self.c.cleanup()
                self.f.now += timedelta(seconds=1)
                self.c.current = replace(self.c.current,
                    hard_deadline=self.f.stamp(self.f.now+timedelta(seconds=180)))

    def assess(self, current=None, digest=None):
        before = self.f.path.read_bytes()
        with self.c.session() as session:
            with patch.object(session, '_persist', side_effect=AssertionError('unexpected write')):
                result = review.assess_candidate(session, current or self.c.current,
                    digest or r.digest(before.decode()))
        self.assertEqual(before, self.f.path.read_bytes())
        self.assertFalse(result['live_enabled'])
        self.assertIsNone(result['actual_spend_usd'])
        self.assertIsNone(result['remaining_allowance_usd'])
        return result

    def test_exhausted_headroom_is_blocked_without_rewriting_history(self):
        result = self.assess()
        self.assertEqual(result['reasons'], ['invalid_cumulative_bound'])
        self.assertEqual(result['retained_total_upper_bound_usd'],
            self.f.read()[b.OVERLAY]['total_upper_bound_usd'])

    def test_state_digest_identity_and_horizon_mismatch(self):
        self.assertEqual(self.assess(digest='0'*64)['reasons'], ['accounting_source_changed'])
        self.assertEqual(self.assess(current=replace(self.c.current, workgroup_id='0'*36))['reasons'],
            ['invalid_anchor'])
        self.assertEqual(self.assess(current=replace(self.c.current,
            hard_deadline=self.f.stamp(self.f.now+timedelta(days=1))))['reasons'], ['invalid_lifecycle'])

    def test_incomplete_unresolved_or_refunded_state_rejected(self):
        saved = self.f.read()
        for change in (
            lambda o: o['attempts'].pop(),
            lambda o: o['attempts'][-1].update(phase='uncertain', cleanup_at=None),
            lambda o: o.update(metering_seconds='0'),
            lambda o: o.update(source_recovery_digest='0'*64),
        ):
            state = copy.deepcopy(saved); change(state[b.OVERLAY]); self.f.write(state)
            self.assertEqual(self.assess()['status'], 'blocked')

    def test_interrupted_persistence_and_closed_session_block_assessment(self):
        intent = self.f.path.parent / b.INTENT
        fd = os.open(intent, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(fd)
        self.assertEqual(self.assess()['reasons'], ['interrupted_persistence'])
        intent.unlink()
        with self.c.session() as session:
            session.closed = True
            result = review.assess_candidate(session, self.c.current,
                r.digest(self.f.path.read_text()))
        self.assertEqual(result['reasons'], ['session_closed'])

    def test_source_bytes_changed_between_load_and_binding_are_rejected(self):
        before = self.f.path.read_bytes()
        with self.c.session() as session:
            read = session._read_raw
            def changed(name):
                raw = read(name)
                return raw + ' ' if name == 'setup.json' else raw
            with patch.object(session, '_read_raw', side_effect=changed):
                result = review.assess_candidate(session, self.c.current,
                    r.digest(before.decode()))
        self.assertEqual(result['reasons'], ['accounting_source_changed'])
        self.assertEqual(self.f.path.read_bytes(), before)

    def observation(self):
        anchor = self.f.anchor
        return dict(account_id=anchor.account_id, region=anchor.region,
            workgroup_id=anchor.workgroup_id, namespace_id=anchor.namespace_id,
            metric='ChargedSeconds', dimension={'Workgroup':'odbcpp-redshift-pilot'},
            unit='Count', statistic='Sum', period_seconds=60,
            start=anchor.earliest_billable_start, end=self.f.stamp(self.f.now),
            observed_at=self.f.stamp(self.f.now), sample_count=1, reported_sum='3600',
            billing_status='DataUnavailableException', classification='observation_only')

    def test_metric_observation_never_establishes_actual_spend(self):
        for amount in ('0', '3600'):
            e = self.observation(); e['reported_sum'] = amount
            result = review.usage_observation(e, self.f.anchor, self.f.now)
            self.assertFalse(result['live_enabled'])
            self.assertIsNone(result['actual_spend_usd'])
            self.assertIsNone(result['remaining_allowance_usd'])
            self.assertEqual(result['billing_status'], 'unavailable')

    def test_wrong_resource_stale_ambiguous_partial_and_bill_claims_rejected(self):
        changes = [('account_id','000000000000'), ('region','us-east-1'),
            ('dimension',{'Workgroup':'other'}), ('unit','Seconds'),
            ('statistic','Average'), ('period_seconds',True), ('sample_count',0),
            ('reported_sum',float('nan')), ('billing_status','verified_zero'),
            ('classification','reconciled'),
            ('start',self.f.stamp(self.f.now-timedelta(seconds=1))),
            ('observed_at',self.f.stamp(self.f.now-timedelta(seconds=301))),
            ('end',self.f.stamp(self.f.now+timedelta(seconds=1)))]
        for key,value in changes:
            with self.subTest(key=key):
                e=self.observation();e[key]=value
                with self.assertRaises(r0.Blocked):
                    review.usage_observation(e,self.f.anchor,self.f.now)
