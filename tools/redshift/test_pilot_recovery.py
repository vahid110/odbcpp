"""Synthetic cleanup-only recovery accounting; no SQL/cloud/credentials."""
import copy
from dataclasses import replace
from datetime import timedelta
from decimal import Decimal
import json
import unittest
from unittest.mock import patch

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_recovery as r
from tools.redshift import test_pilot_windows as fixture


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.windows=fixture.WindowTests();self.windows.setUp()
        self.addCleanup(self.windows.doCleanups);self.f=self.windows.f
        with self.windows.session() as s:
            self.f.passed(s.initialize())
            for index in range(6):
                evidence=self.windows.evidence();evidence['additional_metering_seconds']=1200
                self.f.passed(s.reserve(evidence,self.windows.current))
                if index==5:self.f.passed(s.transition('uncertain'))
                else:self.f.passed(s.transition('cleaned_pending_billing',self.windows.cleanup()))
                self.windows.previous_cleanup=self.windows.cleanup()
                self.f.now+=timedelta(seconds=1)
                self.windows.current=replace(self.windows.current,
                    hard_deadline=self.f.stamp(self.f.now+timedelta(minutes=5)))
        self.source=self.f.path.read_text()
        self.report=json.dumps(dict(status='blocked',reason='remote_cleanup_unverified',
            cleanup_verified=False,results=[],actual_spend_usd=None,remaining_allowance_usd=None,sequence=6),indent=2)+'\n'
        self.result=self.f.path.parent/'window-006-result.json'
        self.result.write_text(self.report);self.result.chmod(0o600)
        self.f.now+=timedelta(minutes=6)
        self.current=replace(self.f.anchor,hard_deadline=self.f.stamp(self.f.now+timedelta(seconds=60)))

    def session(self):return r.recovery_session(self.f.path,self.f.anchor,clock=lambda:self.f.now)

    def evidence(self):
        return dict(verified=True,observed_at=self.f.stamp(self.f.now),anchor=self.current.record(self.f.now),
            base_rpus=4,max_rpus=4,usd_per_rpu_hour='.374',no_additional_resources=True,
            no_other_billable_activity=True,controls_verified=True,network_verified=True,
            other_tax_usd='50',scope='cleanup_only')

    def cleanup(self):
        return dict(verified=True,observed_at=self.f.stamp(self.f.now),anchor=self.current.record(self.f.now),
            no_active_queries=True,no_active_sessions=True,transactions_closed=True)

    def test_preserves_exact_history_and_adds_durable_retained_headroom(self):
        previous=json.loads(self.source)[b.OVERLAY]
        with self.session() as s:
            self.f.passed(s.reserve_recovery(self.evidence(),self.current))
            reserved=copy.deepcopy(self.f.read())
            o=reserved[b.OVERLAY];item=o['recovery']
            self.assertEqual(o['source_state_raw'],self.source)
            self.assertEqual(o['source_result_raw'],self.report)
            self.assertEqual(item['metering_seconds'],str(Decimal(previous['metering_seconds'])+1200))
            self.assertEqual(item['cleanup_seconds'],str(Decimal(previous['cleanup_seconds'])+60))
            self.assertGreater(float(item['compute_upper_bound_usd']),float(previous['compute_upper_bound_usd']))
            self.f.passed(s.start_recovery());self.f.now+=timedelta(seconds=1)
            self.f.passed(s.finish_recovery(self.cleanup()))
        final=self.f.read();done=final[b.OVERLAY]['recovery']
        self.assertEqual(done['phase'],'cleaned_pending_billing')
        for key in ('metering_seconds','cleanup_seconds','compute_upper_bound_usd','total_upper_bound_usd'):
            self.assertEqual(done[key],item[key])
        self.assertEqual(final[b.OVERLAY]['source_state_raw'],self.source)
        self.assertEqual(final[b.OVERLAY]['source_result_raw'],self.report)
        self.assertEqual(self.result.read_text(),self.report)
        self.assertEqual(json.loads(final[b.OVERLAY]['source_state_raw'])[b.OVERLAY]['attempts'][-1]['phase'],'uncertain')
        self.assertIsNone(final['actual_spend_usd_verified']);self.assertIsNone(final['remaining_allowance_usd_verified'])
        with self.windows.session() as s:self.f.blocked(s.reserve(self.windows.evidence(),self.windows.current))

    def test_verbatim_history_preserves_crlf_bytes(self):
        source=self.source.replace('\n','\r\n');report=self.report.replace('\n','\r\n')
        self.f.path.write_bytes(source.encode());self.result.write_bytes(report.encode())
        with self.session() as s:self.f.passed(s.reserve_recovery(self.evidence(),self.current))
        o=self.f.read()[b.OVERLAY]
        self.assertEqual(o['source_state_raw'].encode(),source.encode())
        self.assertEqual(o['source_result_raw'].encode(),report.encode())
        self.assertEqual(o['source_state_digest'],r.digest(source))
        self.assertEqual(o['source_result_digest'],r.digest(report))

    def test_no_replay_or_qualification_operations(self):
        with self.session() as s:
            for fn in (s.initialize,s.reserve,s.transition,s.migrate):self.f.blocked(fn(),'cleanup_only_scope')
            self.f.blocked(s.start_recovery())
            self.f.passed(s.reserve_recovery(self.evidence(),self.current))
            self.f.blocked(s.finish_recovery(self.cleanup()))
            self.f.passed(s.start_recovery())
            self.f.blocked(s.start_recovery(),'recovery_already_consumed')
            self.f.passed(s.finish_recovery())
            before=self.f.path.read_bytes()
            self.f.blocked(s.reserve_recovery(self.evidence(),self.current))
            self.f.blocked(s.finish_recovery(self.cleanup()))
            self.assertEqual(self.f.path.read_bytes(),before)

    def test_bad_controls_scope_or_horizon_preserve_original(self):
        with self.session() as s:
            for key,value in [('scope','iam'),('base_rpus',8),('max_rpus',8),('other_tax_usd',5),
                ('usd_per_rpu_hour','.375'),('controls_verified',False),('network_verified',False),
                ('no_other_billable_activity',False),('verified',False),
                ('observed_at',self.f.stamp(self.f.now-timedelta(seconds=301)))]:
                e=self.evidence();e[key]=value
                self.f.blocked(s.reserve_recovery(e,self.current))
                self.assertEqual(self.f.path.read_text(),self.source)
            for seconds in (0,61,86401):
                current=replace(self.current,hard_deadline=self.f.stamp(self.f.now+timedelta(seconds=seconds)))
                e=self.evidence();e['anchor']=current.record(self.f.now) if seconds<86401 else dict(current.__dict__)
                self.f.blocked(s.reserve_recovery(e,current))
            self.f.now+=timedelta(days=1)
            current=replace(self.current,hard_deadline=self.f.stamp(self.f.now+timedelta(seconds=60)))
            self.f.blocked(s.reserve_recovery(self.evidence(),current))

    def test_retained_floors_cannot_bypass_budget_caps(self):
        with self.session() as s:
            for compute,total in ((Decimal(101),Decimal(150)),(Decimal(100),Decimal(151))):
                with self.assertRaisesRegex(r.r0.Blocked,'budget_exhausted'):
                    s._amounts(self.current.hard_deadline,Decimal(9000),Decimal(600),
                               compute,total,self.f.now)
        self.assertEqual(self.f.path.read_text(),self.source)

    def test_each_absence_required_and_late_success_rejected(self):
        with self.session() as s:
            self.f.passed(s.reserve_recovery(self.evidence(),self.current));self.f.passed(s.start_recovery())
            for key in ('verified','no_active_queries','no_active_sessions','transactions_closed'):
                e=self.cleanup();e[key]=False
                self.f.blocked(s.finish_recovery(e));self.assertEqual(self.f.read()[b.OVERLAY]['recovery']['phase'],'active')
            self.f.now+=timedelta(seconds=61)
            self.f.blocked(s.finish_recovery(self.cleanup()),'deadline_expired')
            self.f.passed(s.finish_recovery())
            self.assertEqual(self.f.read()[b.OVERLAY]['recovery']['phase'],'uncertain')

    def test_tamper_or_inconsistent_original_result_blocks(self):
        report=json.loads(self.report)
        for key,value in [('sequence',7),('results',[{}]),('cleanup_verified',True),('actual_spend_usd',0)]:
            changed=copy.deepcopy(report);changed[key]=value;self.result.write_text(json.dumps(changed))
            with self.session() as s:self.f.blocked(s.reserve_recovery(self.evidence(),self.current))
            self.assertEqual(self.f.path.read_text(),self.source)
        self.result.write_text(self.report)
        with self.session() as s:self.f.passed(s.reserve_recovery(self.evidence(),self.current))
        reserved=self.f.read()
        for change in (lambda o:o.update(source_state_raw=o['source_state_raw']+' '),
            lambda o:o['recovery'].update(total_upper_bound_usd='50'),
            lambda o:o['recovery'].update(additional_metering_seconds='0'),
            lambda o:o['recovery'].update(scope='iam')):
            state=copy.deepcopy(reserved);change(state[b.OVERLAY]);self.f.write(state)
            with self.session() as s:self.f.blocked(s.start_recovery())

    def test_interrupted_persistence_never_grants_start(self):
        unlink=b.os.unlink
        def fail(name,*args,**kwargs):
            if name==b.INTENT:raise OSError('synthetic')
            return unlink(name,*args,**kwargs)
        with self.session() as s:
            with patch.object(b.os,'unlink',side_effect=fail):
                self.f.blocked(s.reserve_recovery(self.evidence(),self.current))
        self.assertEqual(self.f.read()[b.OVERLAY]['recovery']['phase'],'reserved')
        with self.session() as s:self.f.blocked(s.start_recovery(),'interrupted_persistence')


if __name__=='__main__':unittest.main()
