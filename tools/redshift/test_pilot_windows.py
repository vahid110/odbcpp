"""Synthetic finite-window ledger tests; no cloud, credentials or SQL."""
import copy
from dataclasses import replace
from datetime import timedelta
import unittest
from unittest.mock import patch

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_windows as w
from tools.redshift import test_pilot_allowance as fixture


class WindowTests(unittest.TestCase):
    def setUp(self):
        self.allowance=fixture.AllowanceTests();self.allowance.setUp()
        self.addCleanup(self.allowance.doCleanups)
        self.f=self.allowance.f
        with self.allowance.session() as s:
            self.f.passed(s.amend(self.allowance.authorization))
            self.f.passed(s.verify_controls(self.allowance.evidence()))
        self.original=copy.deepcopy(self.f.read())
        self.f.now+=timedelta(seconds=1)
        self.current=replace(self.f.anchor,hard_deadline=self.f.stamp(self.f.now+timedelta(minutes=5)))
        last=self.original[b.OVERLAY]['previous_overlay']['attempts'][-1]
        self.previous_cleanup=dict(verified=True,observed_at=last['cleanup_at'],anchor=last['anchor'],
            no_active_queries=True,no_active_sessions=True,transactions_closed=True)

    def session(self):
        return w.window_session(self.f.path,self.f.anchor,clock=lambda:self.f.now)

    def evidence(self):
        return dict(verified=True,observed_at=self.f.stamp(self.f.now),anchor=self.current.record(self.f.now),
            base_rpus=4,max_rpus=4,usd_per_rpu_hour='.374',no_additional_resources=True,
            no_other_billable_activity=True,controls_verified=True,network_verified=True,
            other_tax_usd='50',additional_metering_seconds=60,additional_cleanup_seconds=60,
            cleanup_evidence=self.previous_cleanup)

    def cleanup(self):
        return dict(verified=True,observed_at=self.f.stamp(self.f.now),anchor=self.current.record(self.f.now),
            no_active_queries=True,no_active_sessions=True,transactions_closed=True)

    def test_explicit_migration_preserves_history_and_requires_fresh_activation(self):
        with self.session() as s:
            self.f.blocked(s.reserve(self.evidence(),self.current),'invalid_window_overlay')
            self.f.passed(s.initialize())
            self.f.passed(s.reserve(self.evidence(),self.current))
            self.f.blocked(s.transition('active',self.previous_cleanup))
            self.f.passed(s.transition('active',self.cleanup()))
            reserved=copy.deepcopy(self.f.read()[b.OVERLAY])
            self.f.passed(s.transition('cleaned_pending_billing',self.cleanup()))
        state=self.f.read();o=state[b.OVERLAY]
        self.assertEqual(o['allowance_overlay'],self.original[b.OVERLAY])
        self.assertEqual({k:v for k,v in state.items() if k!=b.OVERLAY},self.f.legacy)
        for key in ('compute_upper_bound_usd','total_upper_bound_usd','metering_seconds','cleanup_seconds'):
            self.assertEqual(o[key],reserved[key])
        self.assertIsNone(state['actual_spend_usd_verified'])
        self.assertIsNone(state['remaining_allowance_usd_verified'])
        self.assertGreater(float(o['total_upper_bound_usd']),50)

    def test_pending_controls_or_prior_attempt_block_migration(self):
        original=copy.deepcopy(self.original)
        state=copy.deepcopy(original);state[b.OVERLAY].update(controls_status='pending',control_observation=None)
        self.f.write(state)
        with self.session() as s:self.f.blocked(s.initialize(),'allowance_controls_pending')
        state=copy.deepcopy(original)
        state[b.OVERLAY]['previous_overlay']['attempts'][-1].update(phase='reserved',cleanup_at=None)
        self.f.write(state)
        with self.session() as s:self.f.blocked(s.initialize())

    def test_duplicate_initialize_never_changes_history(self):
        with self.session() as s:
            self.f.passed(s.initialize());before=self.f.path.read_bytes()
            self.f.blocked(s.initialize());self.assertEqual(before,self.f.path.read_bytes())

    def test_unresolved_windows_block_after_restart(self):
        for phase in ('reserved','active','uncertain'):
            self.f.write(copy.deepcopy(self.original))
            with self.session() as s:
                self.f.passed(s.initialize());self.f.passed(s.reserve(self.evidence(),self.current))
                if phase=='active':self.f.passed(s.transition(phase,self.cleanup()))
                elif phase=='uncertain':self.f.passed(s.transition(phase))
            with self.session() as s:self.f.blocked(s.reserve(self.evidence(),self.current),'prior_attempt_unresolved')

    def test_window_limits_binding_price_headroom_and_freshness(self):
        with self.session() as s:
            self.f.passed(s.initialize())
            for key,value in [('other_tax_usd','5'),('max_rpus',8),('usd_per_rpu_hour','.375'),
                ('no_other_billable_activity',False),('controls_verified',False),
                ('additional_cleanup_seconds',59),('additional_metering_seconds',1201),('observed_at',self.f.stamp(self.f.now-timedelta(seconds=301)))]:
                e=self.evidence();e[key]=value
                before=self.f.path.read_bytes();self.f.blocked(s.reserve(e,self.current))
                self.assertEqual(before,self.f.path.read_bytes())
            for current in [replace(self.current,account_id='999999999999'),
                replace(self.current,total_limit_usd='150'),
                replace(self.current,earliest_billable_start=self.f.stamp(self.f.now)),
                replace(self.current,hard_deadline=self.f.stamp(self.f.now+timedelta(days=2)))]:
                self.f.blocked(s.reserve(self.evidence(),current))

    def test_finite_windows_keep_cumulative_bounds_and_no_retries(self):
        with self.session() as s:
            self.f.passed(s.initialize())
            for index in range(8):
                self.f.passed(s.reserve(self.evidence(),self.current))
                self.f.passed(s.transition('cleaned_pending_billing',self.cleanup()))
                self.previous_cleanup=self.cleanup()
                self.f.now+=timedelta(seconds=1)
                self.current=replace(self.current,hard_deadline=self.f.stamp(self.f.now+timedelta(minutes=5)))
            self.f.blocked(s.reserve(self.evidence(),self.current),'attempt_limit')
        self.assertEqual(len(self.f.read()[b.OVERLAY]['attempts']),8)

    def test_full_launcher_headroom_is_retained_across_eight_windows(self):
        with self.session() as s:
            self.f.passed(s.initialize())
            initial=copy.deepcopy(self.f.read()[b.OVERLAY])
            for index in range(8):
                evidence=self.evidence();evidence['additional_metering_seconds']=1200
                self.f.passed(s.reserve(evidence,self.current))
                self.f.passed(s.transition('cleaned_pending_billing',self.cleanup()))
                self.previous_cleanup=self.cleanup()
                self.f.now+=timedelta(seconds=1)
                self.current=replace(self.current,hard_deadline=self.f.stamp(self.f.now+timedelta(minutes=5)))
            final=self.f.read()[b.OVERLAY]
            self.assertEqual(int(final['metering_seconds']),int(initial['metering_seconds'])+9600)
            self.assertEqual(int(final['cleanup_seconds']),int(initial['cleanup_seconds'])+480)
            self.assertGreater(float(final['compute_upper_bound_usd']),float(initial['compute_upper_bound_usd']))
            self.assertEqual(final['allowance_overlay'],initial['allowance_overlay'])

    def test_cleanup_cannot_predate_activation_or_reopen_completed_attempt(self):
        with self.session() as s:
            self.f.passed(s.initialize());self.f.passed(s.reserve(self.evidence(),self.current))
            self.f.now+=timedelta(seconds=60)
            self.f.passed(s.transition('active',self.cleanup()))
            cleanup=self.cleanup();cleanup['observed_at']=self.f.stamp(self.f.now-timedelta(seconds=1))
            self.f.blocked(s.transition('cleaned_pending_billing',cleanup),'cleanup_time_regression')
            self.f.passed(s.transition('cleaned_pending_billing',self.cleanup()))
            self.f.blocked(s.transition('active',self.cleanup()),'invalid_transition')

    def test_tampered_history_and_bounds_block(self):
        with self.session() as s:self.f.passed(s.initialize());self.f.passed(s.reserve(self.evidence(),self.current))
        original=self.f.read()
        for mutate in (lambda o:o.update(compute_upper_bound_usd='0'),
            lambda o:o['allowance_overlay'].update(total_limit_usd='151'),
            lambda o:o['attempts'][0].update(sequence=2),
            lambda o:o['attempts'][0].update(total_upper_bound_usd='50')):
            state=copy.deepcopy(original);mutate(state[b.OVERLAY]);self.f.write(state)
            with self.session() as s:self.f.blocked(s.transition('uncertain'))

    def test_interrupted_reservation_stays_uncertain_and_blocks_restart(self):
        with self.session() as s:
            self.f.passed(s.initialize());unlink=b.os.unlink
            def fail(name,*args,**kwargs):
                if name==b.INTENT:raise OSError('secret-canary')
                return unlink(name,*args,**kwargs)
            with patch.object(b.os,'unlink',side_effect=fail):self.f.blocked(s.reserve(self.evidence(),self.current))
        self.assertEqual(self.f.read()[b.OVERLAY]['attempts'][-1]['phase'],'reserved')
        with self.session() as s:self.f.blocked(s.reserve(self.evidence(),self.current),'interrupted_persistence')


if __name__=='__main__':unittest.main()
