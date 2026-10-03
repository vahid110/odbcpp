"""Synthetic explicit amendment tests; no private configuration/cloud/SQL."""
import copy
from dataclasses import replace
from datetime import timedelta
from decimal import Decimal
import unittest
from unittest.mock import patch

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_finite_amendment as a
from tools.redshift import pilot_recovery as r
from tools.redshift import test_pilot_accounting_review as fixture


class FiniteAmendmentTests(unittest.TestCase):
    def setUp(self):
        self.source_fixture = fixture.AccountingReviewTests(); self.source_fixture.setUp()
        self.addCleanup(self.source_fixture.doCleanups)
        self.f = self.source_fixture.f; self.c = self.source_fixture.c
        self.source = self.f.path.read_text(); self.previous = self.f.read()[b.OVERLAY]

    def session(self):
        return a.finite_amendment_session(self.f.path, self.f.anchor, clock=lambda:self.f.now)

    def review(self):
        return dict(record_type='independent_conservative_accounting_review', reviewed=True,
            observed_at=self.f.stamp(self.f.now), source_state_digest=r.digest(self.source),
            source_code_digest='1'*64, evidence_digest='2'*64,
            unknown_billing_bound_accepted=True, controls_verified=True,
            no_other_resources_or_activity=True, network_verified=True, price_verified=True,
            original_horizon_only=True, no_refunds=True, inventory=list(a.INVENTORY),
            max_new_windows=1, metering_ceiling_seconds=12360, cleanup_ceiling_seconds=720)

    def initialize(self, session):
        self.f.passed(session.initialize(r.digest(self.source), self.review()))

    def test_explicit_amendment_retains_exact_history_one_attempt_and_null_billing(self):
        with self.session() as session:
            self.f.blocked(session.reserve(self.c.evidence(), self.c.current))
            self.initialize(session)
            self.f.passed(session.reserve(self.c.evidence(), self.c.current))
            o=self.f.read()[b.OVERLAY]
            self.assertEqual(o['source_continuation_raw'], self.source)
            self.assertEqual(o['attempts'][0]['sequence'], 9)
            self.assertEqual(Decimal(o['metering_seconds']), 12360)
            self.assertEqual(Decimal(o['cleanup_seconds']), 720)
            self.assertGreaterEqual(Decimal(o['total_upper_bound_usd']),
                Decimal(self.previous['total_upper_bound_usd']))
            self.f.passed(session.transition('active',self.c.cleanup()))
            self.f.passed(session.transition('cleaned_pending_billing',self.c.cleanup()))
            before=self.f.path.read_bytes()
            self.f.blocked(session.reserve(self.c.evidence(),self.c.current),'attempt_limit')
            self.f.blocked(session.initialize(r.digest(self.source),self.review()))
            self.assertEqual(before,self.f.path.read_bytes())
        final=self.f.read()
        self.assertIsNone(final['actual_spend_usd_verified'])
        self.assertIsNone(final['remaining_allowance_usd_verified'])
        with self.c.session() as old:
            self.f.blocked(old.reserve(self.c.evidence(),self.c.current))

    def test_missing_review_scope_and_changed_source_reject_before_persistence(self):
        with self.session() as session:
            before=self.f.path.read_bytes()
            self.f.blocked(session.initialize('0'*64,self.review()),'amendment_source_changed')
            for key,value in [('unknown_billing_bound_accepted',False),('max_new_windows',2),
                ('metering_ceiling_seconds',13000),('inventory',['arbitrary']),
                ('observed_at',self.f.stamp(self.f.now-timedelta(seconds=301)))]:
                e=self.review();e[key]=value
                self.f.blocked(session.initialize(r.digest(self.source),e))
                self.assertEqual(before,self.f.path.read_bytes())

    def test_horizon_source_binding_and_activation_cleanup_are_required(self):
        with self.session() as session:
            self.initialize(session);before=self.f.path.read_bytes()
            e=self.c.evidence();e['state_digest']='0'*64
            self.f.blocked(session.reserve(e,self.c.current),'continuation_source_changed')
            future=replace(self.c.current,hard_deadline=self.f.stamp(self.f.now+timedelta(days=1)))
            self.f.blocked(session.reserve(self.c.evidence(),future))
            self.assertEqual(before,self.f.path.read_bytes())
            self.f.passed(session.reserve(self.c.evidence(),self.c.current))
            before=self.f.path.read_bytes()
            bad=self.c.cleanup();bad['no_active_sessions']=False
            self.f.blocked(session.transition('active',bad),'cleanup_unverified')
            bad=self.c.cleanup();bad['observed_at']=self.f.stamp(self.f.now-timedelta(seconds=301))
            self.f.blocked(session.transition('active',bad),'stale_cleanup')
            self.assertEqual(before,self.f.path.read_bytes())
            self.f.passed(session.transition('uncertain'))
            self.f.blocked(session.transition('active',self.c.cleanup()),'active_not_admitted')
            self.f.blocked(session.reserve(self.c.evidence(),self.c.current))

    def test_frozen_history_review_and_liability_tampering_fail_closed(self):
        with self.session() as session:self.initialize(session)
        saved=self.f.read()
        for change in (lambda o:o.update(source_continuation_raw=o['source_continuation_raw']+' '),
            lambda o:o.update(metering_seconds='0'),
            lambda o:o['review'].update(max_new_windows=2),
            lambda o:o.update(total_upper_bound_usd='0')):
            state=copy.deepcopy(saved);change(state[b.OVERLAY]);self.f.write(state)
            before=self.f.path.read_bytes()
            with self.session() as session:self.f.blocked(session.reserve(self.c.evidence(),self.c.current))
            self.assertEqual(before,self.f.path.read_bytes())

    def test_reservation_persistence_failure_retains_crash_block(self):
        with self.session() as session:
            self.initialize(session)
            with patch('tools.redshift.pilot_bootstrap.os.replace',side_effect=OSError):
                self.f.blocked(session.reserve(self.c.evidence(),self.c.current))
        with self.session() as session:
            self.f.blocked(session.reserve(self.c.evidence(),self.c.current),'interrupted_persistence')

    def test_initialization_and_activation_persistence_failure_block_restart(self):
        # Both writes use the existing intent/fsync protocol. A failed replace
        # must never be interpreted as permission to retry after reopening.
        with self.session() as session:
            with patch('tools.redshift.pilot_bootstrap.os.replace',side_effect=OSError):
                self.f.blocked(session.initialize(r.digest(self.source),self.review()))
        with self.session() as session:
            self.f.blocked(session.initialize(r.digest(self.source),self.review()),'interrupted_persistence')

    def test_activation_persistence_failure_does_not_release_reservation(self):
        with self.session() as session:
            self.initialize(session);self.f.passed(session.reserve(self.c.evidence(),self.c.current))
            before=self.f.path.read_bytes()
            with patch('tools.redshift.pilot_bootstrap.os.replace',side_effect=OSError):
                self.f.blocked(session.transition('active',self.c.cleanup()))
            self.assertEqual(before,self.f.path.read_bytes())
        with self.session() as session:
            self.f.blocked(session.transition('active',self.c.cleanup()),'interrupted_persistence')

    def test_activation_after_deadline_fails_and_headroom_cannot_be_refunded(self):
        with self.session() as session:
            self.initialize(session);self.f.passed(session.reserve(self.c.evidence(),self.c.current))
            before=self.f.path.read_bytes();self.f.now+=timedelta(seconds=181)
            self.f.blocked(session.transition('active',self.c.cleanup()),'active_not_admitted')
            self.assertEqual(before,self.f.path.read_bytes())
            self.f.passed(session.transition('uncertain'))
