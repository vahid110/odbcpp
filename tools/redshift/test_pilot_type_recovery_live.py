"""Recovery composition tests: synthetic records, no cloud/SQL/process calls."""
from contextlib import ExitStack
from datetime import timedelta
import copy
import json
import time
import unittest
from unittest.mock import patch

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_live as live
from tools.redshift import pilot_recovery as recovery
from tools.redshift import pilot_type_recovery as ledger
from tools.redshift import pilot_type_recovery_live as launch
from tools.redshift import pilot_iam_credentials as iam
from tools.redshift import test_pilot_type_recovery as fixture
from tools.redshift import test_pilot_live as config_fixture
from tools.redshift.pilot_preflight import Blocked


class TypeRecoveryCompositionTests(unittest.TestCase):
    def setUp(self):
        self.accounting=fixture.TypeRecoveryTests();self.accounting.setUp()
        self.addCleanup(self.accounting.doCleanups);self.f=self.accounting.f
        self.directory=self.f.path.parent
        cfg=config_fixture.LiveCompositionTests();cfg.setUp();self.addCleanup(cfg.doCleanups)
        self.write('bootstrap-anchor.json',self.f.anchor.record(self.f.now))
        self.write('database-config.json',cfg.config)
        self.request=dict(target_sequence=10,scope='cleanup_only',reviewed=True,
            observed_at=self.f.stamp(self.f.now),hard_deadline=self.accounting.current.hard_deadline,
            source_state_digest=recovery.digest(self.accounting.source),
            source_result_digest=recovery.digest(self.accounting.report),code_digest=launch.code_digest(),admission={})
        self.write('cleanup-recovery-010-request.json',self.request)
        self.events=[];self.failure=None
        outer=self
        class Window:
            def __init__(self,directory,config,deadline,**kwargs):
                state=outer.f.read()[b.OVERLAY]
                outer.assertEqual(state['schema_version'],9)
                outer.assertEqual(state['recovery']['phase'],'active')
                outer.assertEqual(state['recovery']['additional_metering_seconds'],'1200')
                outer.assertEqual(state['recovery']['additional_cleanup_seconds'],'60')
                outer.assertEqual(kwargs,dict(cleanup_principal=live.IAM_DB_USER))
                outer.assertLessEqual(deadline-time.monotonic(),60)
                outer.events.append('durable_active');self.deadline=deadline
            def cleanup(self):
                outer.events.append('cleanup')
                if outer.failure=='cleanup':raise Blocked('remote_cleanup_unverified')
                if outer.failure=='interrupt':raise KeyboardInterrupt()
            def driver(self,*args,**kwargs):raise AssertionError('driver forbidden')
            def prepare_fixture(self):raise AssertionError('fixture forbidden')
        self.Window=Window

    def write(self,name,value):
        p=self.directory/name;p.write_text(json.dumps(value));p.chmod(0o600)

    def execute(self):
        real_session=ledger.type_recovery_session
        def session(path,anchor):return real_session(path,anchor,clock=lambda:self.f.now)
        def controls(*args):
            self.events.append('controls')
            self.assertEqual(self.f.path.read_text(),self.accounting.source)
            if self.failure=='controls':raise Blocked('controls_unverified')
        with ExitStack() as stack:
            stack.enter_context(patch.object(launch,'utcnow',side_effect=lambda:self.f.now))
            stack.enter_context(patch.object(launch.windows,'utcnow',side_effect=lambda:self.f.now))
            stack.enter_context(patch.object(ledger,'type_recovery_session',side_effect=session))
            stack.enter_context(patch.object(live,'validate_admission'))
            stack.enter_context(patch.object(launch.windows,'fresh_controls',side_effect=controls))
            stack.enter_context(patch.object(live,'Window',self.Window))
            stack.enter_context(patch.object(iam,'acquire',side_effect=AssertionError('exchange forbidden')))
            stack.enter_context(patch.object(live,'aws',side_effect=AssertionError('unmocked cloud forbidden')))
            return launch.execute(self.directory)

    def test_one_cleanup_follows_durable_reservation_no_qualification(self):
        result=self.execute();self.assertEqual(result['status'],'passed')
        self.assertTrue(result['cleanup_verified']);self.assertIsNone(result['actual_spend_usd'])
        self.assertEqual(self.events,['controls','durable_active','cleanup'])
        state=self.f.read()[b.OVERLAY]
        self.assertEqual(state['source_state_raw'],self.accounting.source)
        self.assertEqual(state['source_result_raw'],self.accounting.report)
        self.assertEqual(state['recovery']['phase'],'cleaned_pending_billing')
        with self.assertRaises(Blocked):self.execute()
        self.assertEqual(self.events,['controls','durable_active','cleanup'])
        report=self.directory/'cleanup-recovery-010-result.json'
        self.assertEqual(report.stat().st_mode&0o777,0o600)

    def test_failure_or_interrupt_consumes_once_without_second_cleanup(self):
        for failure in ('cleanup','interrupt'):
            with self.subTest(failure=failure):
                self.f.path.write_text(self.accounting.source)
                report=self.directory/'cleanup-recovery-010-result.json'
                if report.exists():report.unlink()
                self.events=[];self.failure=failure
                result=self.execute()
                self.assertFalse(result['cleanup_verified']);self.assertEqual(result['status'],'blocked')
                self.assertEqual(self.events,['controls','durable_active','cleanup'])
                self.assertEqual(self.f.read()[b.OVERLAY]['recovery']['phase'],'uncertain')
                with self.assertRaises(Blocked):self.execute()

    def test_changed_source_code_scope_and_unverified_controls_prevent_reservation(self):
        for key,value in [('target_sequence',7),('scope','iam'),('reviewed',False),
            ('source_state_digest','0'*64),('source_result_digest','0'*64),('code_digest','0'*64)]:
            request=copy.deepcopy(self.request);request[key]=value
            self.write('cleanup-recovery-010-request.json',request)
            with self.subTest(key=key):
                with self.assertRaises(Blocked):self.execute()
                self.assertEqual(self.f.path.read_text(),self.accounting.source)
        self.write('cleanup-recovery-010-request.json',self.request)
        self.failure='controls'
        with self.assertRaisesRegex(Blocked,'controls_unverified'):self.execute()
        self.assertEqual(self.events,['controls'])
        self.assertEqual(self.f.path.read_text(),self.accounting.source)

    def test_mutation_during_controls_cannot_freeze_unreviewed_bytes(self):
        real_session=ledger.type_recovery_session
        for filename in ('setup.json','window-010-result.json'):
            self.f.path.write_text(self.accounting.source)
            self.accounting.result.write_text(self.accounting.report)
            def controls(*args):
                path=self.directory/filename
                path.write_bytes(path.read_bytes()+b' ')
            with self.subTest(filename=filename),                patch.object(launch,'utcnow',side_effect=lambda:self.f.now),                patch.object(ledger,'type_recovery_session',side_effect=lambda path,anchor:real_session(path,anchor,clock=lambda:self.f.now)),                patch.object(live,'validate_admission'),                patch.object(launch.windows,'fresh_controls',side_effect=controls),                patch.object(live,'Window',side_effect=AssertionError('SQL forbidden')):
                with self.assertRaisesRegex(Blocked,'recovery_source_changed'):launch.execute(self.directory)
            self.assertEqual(self.f.read()[b.OVERLAY]['schema_version'],8)

    def test_failed_reservation_or_start_persistence_prevents_sql(self):
        real_persist=recovery.RecoverySession._persist
        for fail_at in (1,2):
            self.f.path.write_text(self.accounting.source)
            report=self.directory/'cleanup-recovery-010-result.json'
            if report.exists():report.unlink()
            self.events=[];calls=[]
            def persist(session,state):
                calls.append(True)
                if len(calls)==fail_at:raise OSError('synthetic')
                return real_persist(session,state)
            with self.subTest(fail_at=fail_at),patch.object(ledger.TypeRecoverySession,'_persist',persist):
                if fail_at==1:
                    with self.assertRaises(Blocked):self.execute()
                else:self.assertFalse(self.execute()['cleanup_verified'])
            self.assertEqual(self.events,['controls'])
            self.assertEqual(self.f.read()[b.OVERLAY]['schema_version'],8 if fail_at==1 else 9)

    def test_report_failure_does_not_allow_repeat_cleanup(self):
        with patch.object(launch.windows,'save_report',side_effect=OSError('synthetic')):
            with self.assertRaisesRegex(Blocked,'recovery_report_persistence_failed'):self.execute()
        self.assertEqual(self.f.read()[b.OVERLAY]['recovery']['phase'],'cleaned_pending_billing')
        with self.assertRaises(Blocked):self.execute()
        self.assertEqual(self.events,['controls','durable_active','cleanup'])

    def test_expired_after_controls_never_reserves_or_runs_sql(self):
        real_session=ledger.type_recovery_session
        def controls(*args):self.f.now+=timedelta(seconds=61)
        with patch.object(launch,'utcnow',side_effect=lambda:self.f.now),\
            patch.object(ledger,'type_recovery_session',side_effect=lambda path,anchor:real_session(path,anchor,clock=lambda:self.f.now)),\
            patch.object(live,'validate_admission'),\
            patch.object(launch.windows,'fresh_controls',side_effect=controls),\
            patch.object(live,'Window',side_effect=AssertionError('SQL forbidden')):
            with self.assertRaisesRegex(Blocked,'invalid_recovery_deadline'):launch.execute(self.directory)
        self.assertEqual(self.f.path.read_text(),self.accounting.source)


if __name__=='__main__':unittest.main()
