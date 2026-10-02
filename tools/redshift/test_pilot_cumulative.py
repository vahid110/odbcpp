"""Offline synthetic state only; no cloud, credentials or canonical state."""
import copy
from dataclasses import replace
from datetime import timedelta
from decimal import Decimal
import json
import os
import unittest
from unittest.mock import patch

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_cumulative as c
from tools.redshift import pilot_preflight as r0
from tools.redshift import test_pilot_bootstrap as fixture


class CumulativeTests(unittest.TestCase):
    stamp=staticmethod(fixture.BootstrapTests.stamp)
    write=fixture.BootstrapTests.write
    read=fixture.BootstrapTests.read
    passed=fixture.BootstrapTests.passed
    blocked=fixture.BootstrapTests.blocked
    tearDown=fixture.BootstrapTests.tearDown

    def setUp(self):
        fixture.BootstrapTests.setUp(self)
        with b.bootstrap_session(self.path,self.anchor,clock=lambda:self.now) as session:
            self.passed(session.initialize())
            self.passed(fixture.BootstrapTests.reserve(self,session))
            self.passed(fixture.BootstrapTests.clean(self,session))
        self.original=copy.deepcopy(self.read()[b.OVERLAY])
        self.old_cleanup=self.cleanup(self.anchor)
        self.now+=timedelta(minutes=10)  # Genuine historical proof, not fresh SQL.
        self.new_anchor=replace(self.anchor,hard_deadline=self.stamp(self.now+timedelta(minutes=5)))

    evidence=fixture.BootstrapTests.evidence

    def session(self):
        return c.currentperiod_session(self.path,self.anchor,clock=lambda:self.now)

    def cleanup(self,anchor=None):
        return dict(verified=True,observed_at=self.stamp(self.now),anchor=(anchor or self.new_anchor).record(self.now),
            no_active_queries=True,no_active_sessions=True,transactions_closed=True)

    def admission(self):
        return dict(verified=True,observed_at=self.stamp(self.now),anchor=self.new_anchor.record(self.now),
            base_rpus=4,max_rpus=4,usd_per_rpu_hour='.374',no_additional_resources=True,
            no_other_billable_activity=True,controls_verified=True,network_verified=True,other_tax_usd='5',
            additional_metering_seconds=60,additional_cleanup_seconds=60,cleanup_evidence=self.old_cleanup)

    def migrate(self,session):
        return session.migrate(self.old_cleanup)

    def reserve(self,session):
        return session.reserve(self.admission(),self.new_anchor)

    def test_historical_cleanup_migrate_reserve_then_fresh_activation(self):
        with self.session() as s:
            self.passed(self.migrate(s))
            after_migrate=self.read()[b.OVERLAY]
            self.assertEqual(after_migrate['original_bootstrap'],self.original)
            self.assertEqual(after_migrate['metering_seconds'],'300')
            self.passed(self.reserve(s))
            self.blocked(s.transition('active',self.old_cleanup))
            self.passed(s.transition('active',self.cleanup()))
            reserved=copy.deepcopy(self.read()[b.OVERLAY])
            self.passed(s.transition('cleaned_pending_billing',self.cleanup()))
        state=self.read()
        self.assertEqual({k:v for k,v in state.items() if k!=b.OVERLAY},self.legacy)
        self.assertEqual(state[b.OVERLAY]['original_bootstrap'],self.original)
        for key in ('compute_upper_bound_usd','total_upper_bound_usd','metering_seconds','cleanup_seconds'):
            self.assertEqual(state[b.OVERLAY][key],reserved[key])
        self.assertEqual(reserved['metering_seconds'],'360')
        self.assertEqual(reserved['cleanup_seconds'],'120')
        self.assertIsNone(state['actual_spend_usd_verified'])
        self.assertIsNone(state['remaining_allowance_usd_verified'])

    def test_completion_cleanup_cannot_predate_activation(self):
        with self.session() as s:
            self.passed(self.migrate(s));self.passed(self.reserve(s))
            self.now+=timedelta(seconds=60)
            self.passed(s.transition('active',self.cleanup()))
            e=self.cleanup();e['observed_at']=self.stamp(self.now-timedelta(seconds=30))
            self.blocked(s.transition('cleaned_pending_billing',e),'cleanup_time_regression')

    def test_no_auto_migration_or_duplicate_migration(self):
        with self.session() as s:
            self.blocked(self.reserve(s),'invalid_cumulative_overlay')
            self.passed(self.migrate(s)); before=self.path.read_bytes()
            self.blocked(self.migrate(s)); self.assertEqual(before,self.path.read_bytes())

    def test_pending_attempt_blocks_second_admission_after_restart(self):
        for phase in ('reserved','active','uncertain'):
            with self.subTest(phase=phase):
                state=dict(self.legacy);state[b.OVERLAY]=copy.deepcopy(self.original);self.write(state)
                with self.session() as s:
                    self.passed(self.migrate(s)); self.passed(self.reserve(s))
                    if phase=='active': self.passed(s.transition(phase,self.cleanup()))
                    elif phase=='uncertain': self.passed(s.transition(phase))
                before=self.path.read_bytes()
                with self.session() as s: self.blocked(self.reserve(s),'prior_attempt_unresolved')
                self.assertEqual(before,self.path.read_bytes())

    def test_cleaned_admission_extends_monotonic_bound_without_double_adding(self):
        with self.session() as s:
            self.passed(self.migrate(s));self.passed(self.reserve(s))
            first=self.read()[b.OVERLAY]['total_upper_bound_usd']
            self.passed(s.transition('cleaned_pending_billing',self.cleanup()))
            self.old_cleanup=self.cleanup()
            self.now+=timedelta(minutes=1)
            self.new_anchor=replace(self.new_anchor,hard_deadline=self.stamp(self.now+timedelta(minutes=5)))
            self.passed(self.reserve(s))
            state=self.read()[b.OVERLAY]
            self.assertEqual(len(state['attempts']),2)
            self.assertGreater(Decimal(state['total_upper_bound_usd']),Decimal(first))
            self.assertLess(Decimal(state['total_upper_bound_usd']),Decimal(first)*2)
            self.assertEqual(state['metering_seconds'],'420')
            self.assertEqual(state['cleanup_seconds'],'180')

    def test_foreign_ids_period_price_capacity_resources_network_block(self):
        with self.session() as s:
            self.passed(self.migrate(s))
            for key,value in [('usd_per_rpu_hour','.375'),('base_rpus',8),('max_rpus',8),
                ('no_additional_resources',False),('no_other_billable_activity',False),
                ('controls_verified',False),('network_verified',False),('other_tax_usd','4.99'),
                ('additional_metering_seconds',59),('additional_cleanup_seconds',True)]:
                e=self.admission();e[key]=value;before=self.path.read_bytes()
                self.blocked(s.reserve(e,self.new_anchor));self.assertEqual(before,self.path.read_bytes())
            for anchor in [replace(self.new_anchor,account_id='999999999999'),
                replace(self.new_anchor,period_id='new-period'),replace(self.new_anchor,compute_limit_usd='11'),
                replace(self.new_anchor,earliest_billable_start=self.stamp(self.now-timedelta(minutes=1))),
                replace(self.new_anchor,workgroup_id='33333333-3333-3333-3333-333333333333')]:
                self.blocked(s.reserve(self.admission(),anchor))

    def test_stale_future_admission_fresh_activation_required(self):
        with self.session() as s:
            self.passed(self.migrate(s))
            for offset in (-301,1):
                e=self.admission();e['observed_at']=self.stamp(self.now+timedelta(seconds=offset))
                self.blocked(s.reserve(e,self.new_anchor),'stale_admission')
            self.passed(self.reserve(s))
            self.blocked(s.transition('active'))
            e=self.cleanup();e['observed_at']=self.stamp(self.now+timedelta(seconds=1))
            self.blocked(s.transition('active',e),'stale_cleanup')
            self.now+=timedelta(seconds=301)
            self.blocked(s.transition('active',self.cleanup()),'active_not_admitted')
            self.passed(s.transition('uncertain'))

    def test_over_budget_and_exact_boundary(self):
        with self.session() as s:
            self.passed(self.migrate(s))
            for duration,status in ((23584,'pass'),(23585,'block')):
                anchor=replace(self.new_anchor,hard_deadline=self.stamp(
                    b._time(self.anchor.earliest_billable_start,self.now)+timedelta(seconds=duration)))
                e=self.admission();e['anchor']=anchor.record(self.now)
                before=self.path.read_bytes()
                result=s.reserve(e,anchor)
                self.assertEqual(result['status'],status,result)
                if status=='pass':
                    self.assertEqual(self.read()[b.OVERLAY]['compute_upper_bound_usd'],'10.00')
                    self.write(json.loads(before))  # Restore synthetic pre-reservation for boundary pair.
                else:
                    self.blocked(result,'budget_exhausted');self.assertEqual(before,self.path.read_bytes())

    def test_mutable_history_and_legacy_changes_block(self):
        with self.session() as s: self.passed(self.migrate(s));self.passed(self.reserve(s))
        original=self.read()
        mutations=[lambda st:st[b.OVERLAY]['attempts'][0].update(sequence=2),
            lambda st:st[b.OVERLAY]['attempts'][0].update(compute_upper_bound_usd='0'),
            lambda st:st[b.OVERLAY].update(metering_seconds='60'),
            lambda st:st[b.OVERLAY]['original_bootstrap']['window'].update(phase='reserved'),
            lambda st:st['history'].append({'changed':True})]
        for mutate in mutations:
            state=copy.deepcopy(original);mutate(state);self.write(state)
            with self.session() as s: self.blocked(s.transition('uncertain'))

    def test_missing_or_false_original_cleanup_and_numeric_secret_errors(self):
        with self.session() as s:
            e=copy.deepcopy(self.old_cleanup);e['no_active_queries']=False
            self.blocked(s.migrate(e),'cleanup_unverified')
            self.passed(self.migrate(s))
            for value in (None,True,Decimal('NaN'),Decimal('Infinity'),Decimal('1e999999'),'secret-canary'):
                e=self.admission();e['additional_metering_seconds']=value
                self.blocked(s.reserve(e,self.new_anchor))

    def test_crash_retains_intent_and_blocks_restart(self):
        with self.session() as s:
            self.passed(self.migrate(s));before=self.path.read_bytes()
            with patch.object(b.os,'replace',side_effect=OSError('secret-canary')):
                self.blocked(self.reserve(s))
            self.assertEqual(before,self.path.read_bytes())
        with self.session() as s: self.blocked(self.reserve(s),'interrupted_persistence')

    def test_crash_after_replace_preserves_reservation(self):
        with self.session() as s:
            self.passed(self.migrate(s))
            real_unlink=os.unlink
            def fail(name,*args,**kwargs):
                if name==b.INTENT: raise OSError('secret-canary')
                return real_unlink(name,*args,**kwargs)
            with patch.object(b.os,'unlink',side_effect=fail):self.blocked(self.reserve(s))
            self.assertEqual(self.read()[b.OVERLAY]['attempts'][-1]['phase'],'reserved')
        with self.session() as s:self.blocked(self.reserve(s),'interrupted_persistence')

    def test_finite_attempt_limit_and_deadline_regression(self):
        with self.session() as s:
            self.passed(self.migrate(s))
            for i in range(c.MAX_ATTEMPTS):
                self.passed(self.reserve(s))
                self.passed(s.transition('cleaned_pending_billing',self.cleanup()))
                self.old_cleanup=self.cleanup()
                self.now+=timedelta(seconds=1)
                self.new_anchor=replace(self.new_anchor,hard_deadline=self.stamp(self.now+timedelta(minutes=5)))
            before=self.path.read_bytes()
            self.blocked(self.reserve(s),'attempt_limit')
            self.assertEqual(before,self.path.read_bytes())
        state=dict(self.legacy);state[b.OVERLAY]=copy.deepcopy(self.original);self.write(state)
        with self.session() as s:
            self.old_cleanup=dict(verified=True,observed_at=self.original['window']['cleanup_at'],
                anchor=self.anchor.record(self.now),no_active_queries=True,no_active_sessions=True,transactions_closed=True)
            self.passed(self.migrate(s))
            self.passed(self.reserve(s));self.passed(s.transition('cleaned_pending_billing',self.cleanup()))
            self.old_cleanup=self.cleanup()
            self.new_anchor=replace(self.new_anchor,hard_deadline=self.stamp(self.now+timedelta(minutes=4)))
            self.blocked(self.reserve(s),'invalid_attempt_timestamp')

    def test_duplicate_key_private_file_contention_and_session_close(self):
        with self.session() as s:
            with self.assertRaises(r0.Blocked):
                with self.session():pass
            self.passed(self.migrate(s))
        self.blocked(s.transition('uncertain'),'session_closed')
        os.chmod(self.path,0o644)
        with self.session() as s:self.blocked(self.reserve(s),'unsafe_private_file')
        os.chmod(self.path,0o600)
        self.path.write_text('{"secret-canary":1,"secret-canary":2}')
        with self.session() as s:self.blocked(self.reserve(s),'duplicate_record_member')


if __name__=='__main__':
    unittest.main()
