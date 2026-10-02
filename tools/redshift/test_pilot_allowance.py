"""Offline allowance migration using private synthetic state; never AWS/SQL."""
import copy
from datetime import timedelta
import unittest
from unittest.mock import patch

from tools.redshift import pilot_allowance as a
from tools.redshift import pilot_bootstrap as b
from tools.redshift import test_pilot_cumulative as fixture


class AllowanceTests(unittest.TestCase):
    def setUp(self):
        self.f = fixture.CumulativeTests()
        self.f.setUp()
        self.addCleanup(self.f.tearDown)
        with self.f.session() as s:
            self.f.passed(self.f.migrate(s))
            self.f.passed(self.f.reserve(s))
            self.f.passed(s.transition('cleaned_pending_billing', self.f.cleanup()))
        self.original = copy.deepcopy(self.f.read())
        self.f.now += timedelta(seconds=60)
        self.authorization = dict(record_type='user_allowance_amendment',
            recorded_at=self.f.now.isoformat(), account_id=self.f.anchor.account_id,
            total_initial_allowance_usd='150', previous_total_initial_allowance_usd='15',
            scope='One initial Redshift testing allowance shared across all chats; not monthly or per chat. Prior costs and reservations count toward this total.',
            authorization=a.AUTHORIZATION_TEXT, capacity_change_authorized=False,
            resource_expansion_authorized=False,
            implementation_status='recorded_pending_reviewed_accounting_and_aws_control_migration',
            planning_compute_envelope_usd='100', planning_other_tax_reserve_usd='50')

    def session(self):
        return a.allowance_session(self.f.path, self.f.anchor, clock=lambda:self.f.now)

    def evidence(self):
        return dict(verified=True, observed_at=self.f.stamp(self.f.now),
            account_id=self.f.anchor.account_id, region='eu-north-1',
            workgroup_id=self.f.anchor.workgroup_id, namespace_id=self.f.anchor.namespace_id,
            base_rpus=4, max_rpus=4, workgroup_count=1, namespace_count=1,
            budget_total_usd='150', budget_start='2026-10-02', budget_end='2026-10-12',
            budget_alerts_usd=['100','125','150'], usage_amount_rpu_hours=267,
            usage_period='monthly', usage_type='serverless-compute', usage_action='deactivate',
            usd_per_rpu_hour='.374', require_ssl=True, max_query_execution_seconds=30,
            network_verified=True)

    def test_migrate_preserves_all_history_and_unknown_costs(self):
        with self.session() as s:
            self.f.passed(s.amend(self.authorization))
        state=self.f.read();o=state[b.OVERLAY]
        self.assertEqual(o['previous_overlay'],self.original[b.OVERLAY])
        self.assertEqual({k:v for k,v in state.items() if k!=b.OVERLAY},self.f.legacy)
        self.assertEqual(o['authorization'],self.authorization)
        self.assertEqual(o['compute_upper_bound_usd'],self.original[b.OVERLAY]['compute_upper_bound_usd'])
        self.assertGreater(float(o['total_upper_bound_usd']),float(self.original[b.OVERLAY]['total_upper_bound_usd']))
        self.assertEqual(o['controls_status'],'pending')
        self.assertIsNone(o['control_observation'])
        self.assertIsNone(state['actual_spend_usd_verified'])
        self.assertIsNone(state['remaining_allowance_usd_verified'])
        with self.f.session() as old:
            self.f.blocked(self.f.reserve(old),'invalid_cumulative_overlay')

    def test_controls_verified_separately_without_enabling_live_sql(self):
        with self.session() as s:
            self.f.blocked(s.verify_controls(self.evidence()),'invalid_allowance_overlay')
            self.f.passed(s.amend(self.authorization))
            self.f.passed(s.verify_controls(self.evidence()))
            self.f.blocked(s.verify_controls(self.evidence()),'controls_already_recorded')
        self.assertEqual(self.f.read()[b.OVERLAY]['controls_status'],'verified')
        self.assertFalse(self.f.read()['live_testing_enabled'])

    def test_duplicate_migration_never_resets_history(self):
        with self.session() as s:
            self.f.passed(s.amend(self.authorization));before=self.f.path.read_bytes()
            self.f.blocked(s.amend(self.authorization))
            self.assertEqual(before,self.f.path.read_bytes())

    def test_wrong_scope_limits_authority_account_or_timestamp_block(self):
        changes=[('scope','per-month'),('authorization','secret-canary'),
            ('total_initial_allowance_usd','151'),('previous_total_initial_allowance_usd','0'),
            ('planning_compute_envelope_usd','150'),('planning_other_tax_reserve_usd','0'),
            ('capacity_change_authorized',True),('resource_expansion_authorized',True),
            ('account_id','999999999999'),('recorded_at',self.f.stamp(self.f.now+timedelta(seconds=1)))]
        with self.session() as s:
            for key,value in changes:
                auth=copy.deepcopy(self.authorization);auth[key]=value
                before=self.f.path.read_bytes();self.f.blocked(s.amend(auth))
                self.assertEqual(before,self.f.path.read_bytes())

    def test_pending_attempt_blocks_amendment(self):
        state=copy.deepcopy(self.original)
        attempt=state[b.OVERLAY]['attempts'][-1]
        attempt.update(phase='reserved',cleanup_at=None)
        self.f.write(state)
        with self.session() as s:self.f.blocked(s.amend(self.authorization),'prior_attempt_unresolved')

    def test_stale_incorrect_or_unrestricted_cloud_controls_block(self):
        with self.session() as s:
            self.f.passed(s.amend(self.authorization))
            changes=[('budget_total_usd','15'),('budget_total_usd','151'),
                ('base_rpus',8),('max_rpus',8),('workgroup_count',2),('namespace_count',2),
                ('usage_amount_rpu_hours',268),('usage_action','log'),('usage_period','daily'),
                ('require_ssl',False),('network_verified',False),('verified',False),
                ('budget_alerts_usd',['100','150']),('budget_end','2026-11-01'),
                ('usd_per_rpu_hour','.375'),('max_query_execution_seconds',0),
                ('observed_at',self.f.stamp(self.f.now-timedelta(seconds=301)))]
            for key,value in changes:
                e=self.evidence();e[key]=value
                before=self.f.path.read_bytes();self.f.blocked(s.verify_controls(e))
                self.assertEqual(before,self.f.path.read_bytes())

    def test_tampered_history_authorization_or_bounds_block(self):
        with self.session() as s:self.f.passed(s.amend(self.authorization))
        original=self.f.read()
        for mutate in (lambda o:o.update(compute_upper_bound_usd='0'),
            lambda o:o.update(total_upper_bound_usd='50'),
            lambda o:o['previous_overlay'].update(compute_upper_bound_usd='0'),
            lambda o:o['authorization'].update(scope='reset'),
            lambda o:o.update(other_tax_reserve_usd='5')):
            state=copy.deepcopy(original);mutate(state[b.OVERLAY]);self.f.write(state)
            with self.session() as s:self.f.blocked(s.verify_controls(self.evidence()))

    def test_interrupted_write_blocks_restart_without_recovery_guess(self):
        with self.session() as s:
            unlink=b.os.unlink
            def fail(name,*args,**kwargs):
                if name==b.INTENT:raise OSError('secret-canary')
                return unlink(name,*args,**kwargs)
            with patch.object(b.os,'unlink',side_effect=fail):self.f.blocked(s.amend(self.authorization))
        self.assertEqual(self.f.read()[b.OVERLAY]['schema_version'],3)
        with self.session() as s:self.f.blocked(s.verify_controls(self.evidence()),'interrupted_persistence')


if __name__=='__main__':unittest.main()
