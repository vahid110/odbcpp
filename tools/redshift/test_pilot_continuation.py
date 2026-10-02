"""Synthetic remaining-window continuation, no cloud or live state access."""
import copy
from dataclasses import replace
from datetime import timedelta
from decimal import Decimal
import unittest
from unittest.mock import patch

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_continuation as c
from tools.redshift import pilot_recovery as r
from tools.redshift import test_pilot_recovery as fixture


class ContinuationTests(unittest.TestCase):
    def setUp(self):
        self.recovery=fixture.RecoveryTests();self.recovery.setUp()
        self.addCleanup(self.recovery.doCleanups);self.f=self.recovery.f
        with self.recovery.session() as s:
            self.f.passed(s.reserve_recovery(self.recovery.evidence(),self.recovery.current))
            self.f.passed(s.start_recovery())
            self.f.now+=timedelta(seconds=1)
            self.f.passed(s.finish_recovery(self.recovery.cleanup()))
        self.source=self.f.path.read_text();self.previous=self.f.read()[b.OVERLAY]['recovery']
        self.current=replace(self.f.anchor,hard_deadline=self.f.stamp(self.f.now+timedelta(seconds=180)))
        self.previous_cleanup=dict(verified=True,observed_at=self.previous['cleanup_at'],anchor=self.previous['anchor'],
            no_active_sessions=True,no_active_queries=True,transactions_closed=True)

    def session(self):return c.continuation_session(self.f.path,self.f.anchor,clock=lambda:self.f.now)

    def evidence(self):
        return dict(verified=True,observed_at=self.f.stamp(self.f.now),anchor=self.current.record(self.f.now),
            base_rpus=4,max_rpus=4,usd_per_rpu_hour='.374',no_additional_resources=True,
            no_other_billable_activity=True,controls_verified=True,network_verified=True,
            other_tax_usd=50,additional_metering_seconds=1200,additional_cleanup_seconds=60,
            cleanup_evidence=self.previous_cleanup)

    def cleanup(self):
        return dict(verified=True,observed_at=self.f.stamp(self.f.now),anchor=self.current.record(self.f.now),
            no_active_sessions=True,no_active_queries=True,transactions_closed=True)

    def test_explicit_migration_keeps_complete_history_and_only_007_008(self):
        with self.session() as s:
            self.f.blocked(s.reserve(self.evidence(),self.current))
            self.f.passed(s.initialize(r.digest(self.source)))
            initial=copy.deepcopy(self.f.read()[b.OVERLAY])
            self.assertEqual(initial['source_recovery_raw'],self.source)
            self.f.blocked(s.initialize(r.digest(self.source)))
            for seq in (7,8):
                self.f.passed(s.reserve(self.evidence(),self.current))
                self.assertEqual(self.f.read()[b.OVERLAY]['attempts'][-1]['sequence'],seq)
                self.f.passed(s.transition('active',self.cleanup()))
                self.f.passed(s.transition('cleaned_pending_billing',self.cleanup()))
                self.previous_cleanup=self.cleanup();self.f.now+=timedelta(seconds=1)
                self.current=replace(self.current,hard_deadline=self.f.stamp(self.f.now+timedelta(seconds=180)))
            self.f.blocked(s.reserve(self.evidence(),self.current),'attempt_limit')
        final=self.f.read();o=final[b.OVERLAY]
        self.assertEqual(o['source_recovery_raw'],self.source)
        self.assertEqual(Decimal(o['metering_seconds']),Decimal(self.previous['metering_seconds'])+2400)
        self.assertEqual(Decimal(o['cleanup_seconds']),Decimal(self.previous['cleanup_seconds'])+120)
        self.assertIsNone(final['actual_spend_usd_verified']);self.assertIsNone(final['remaining_allowance_usd_verified'])
        self.assertEqual(self.recovery.result.read_text(),self.recovery.report)

    def test_uncertain_recovery_or_changed_source_never_migrates(self):
        with self.session() as s:self.f.blocked(s.initialize('0'*64),'continuation_source_changed')
        state=self.f.read();state[b.OVERLAY]['recovery'].update(phase='uncertain',cleanup_at=None)
        self.f.write(state);raw=self.f.path.read_text()
        with self.session() as s:self.f.blocked(s.initialize(r.digest(raw)),'recovery_cleanup_required')
        self.assertEqual(self.f.path.read_text(),raw)

    def test_unresolved_attempt_blocks_restart_and_recovery_reentry(self):
        with self.session() as s:
            self.f.passed(s.initialize(r.digest(self.source)))
            self.f.passed(s.reserve(self.evidence(),self.current))
            self.f.passed(s.transition('uncertain'))
            for method in (s.reserve_recovery,s.start_recovery,s.finish_recovery,s.migrate):
                self.f.blocked(method(),'continuation_only_scope')
        with self.session() as s:self.f.blocked(s.reserve(self.evidence(),self.current),'prior_attempt_unresolved')

    def test_tamper_headroom_controls_and_original_horizon_fail_closed(self):
        with self.session() as s:
            self.f.passed(s.initialize(r.digest(self.source)))
            before=self.f.path.read_bytes()
            for key,value in [('max_rpus',8),('other_tax_usd',5),('additional_metering_seconds',60),
                ('additional_cleanup_seconds',0),('controls_verified',False),('network_verified',False)]:
                e=self.evidence();e[key]=value;self.f.blocked(s.reserve(e,self.current))
                self.assertEqual(self.f.path.read_bytes(),before)
            e=self.evidence()
            self.f.now+=timedelta(days=1)
            self.current=replace(self.current,hard_deadline=self.f.stamp(self.f.now+timedelta(seconds=180)))
            e.update(observed_at=self.f.stamp(self.f.now),anchor=dict(self.current.__dict__))
            self.f.blocked(s.reserve(e,self.current))
            self.assertEqual(self.f.path.read_bytes(),before)

    def test_corrupt_frozen_history_and_sequence_or_refund_block(self):
        with self.session() as s:
            self.f.passed(s.initialize(r.digest(self.source)));self.f.passed(s.reserve(self.evidence(),self.current))
        saved=self.f.read()
        for change in (lambda o:o.update(source_recovery_raw=o['source_recovery_raw']+' '),
            lambda o:o['attempts'][0].update(sequence=1),lambda o:o.update(metering_seconds='0'),
            lambda o:o.update(compute_upper_bound_usd='0')):
            state=copy.deepcopy(saved);change(state[b.OVERLAY]);self.f.write(state)
            with self.session() as s:self.f.blocked(s.transition('uncertain'))

    def test_interrupted_initialization_blocks_restart(self):
        unlink=b.os.unlink
        def fail(name,*args,**kwargs):
            if name==b.INTENT:raise OSError('synthetic')
            return unlink(name,*args,**kwargs)
        with self.session() as s:
            with patch.object(b.os,'unlink',side_effect=fail):self.f.blocked(s.initialize(r.digest(self.source)))
        with self.session() as s:self.f.blocked(s.reserve(self.evidence(),self.current),'interrupted_persistence')


if __name__=='__main__':unittest.main()
