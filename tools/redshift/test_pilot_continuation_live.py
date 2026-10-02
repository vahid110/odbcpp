"""Synthetic remaining-window runner tests; no cloud, credentials or SQL."""
import copy
from contextlib import ExitStack
from datetime import timedelta
import json
import unittest
from unittest.mock import patch

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_continuation as continuation
from tools.redshift import pilot_continuation_live as launch
from tools.redshift import pilot_iam_credentials as iam
from tools.redshift import pilot_live as live
from tools.redshift import pilot_recovery as recovery
from tools.redshift import pilot_window_live as windows
from tools.redshift import test_pilot_continuation as fixture
from tools.redshift import test_pilot_live as config_fixture
from tools.redshift.pilot_preflight import Blocked


class ContinuationCompositionTests(unittest.TestCase):
    def setUp(self):
        self.accounting=fixture.ContinuationTests();self.accounting.setUp()
        self.addCleanup(self.accounting.doCleanups);self.f=self.accounting.f;self.directory=self.f.path.parent
        self.config=config_fixture.LiveCompositionTests();self.config.setUp();self.addCleanup(self.config.doCleanups)
        with self.accounting.session() as s:self.f.passed(s.initialize(recovery.digest(self.accounting.source)))
        self.v6=self.f.path.read_text()
        self.write('bootstrap-anchor.json',self.f.anchor.record(self.f.now))
        self.write('database-config.json',self.config.config)
        admission=copy.deepcopy(self.config.admission)
        admission.update(account_id=self.f.anchor.account_id,earliest_billable_start=self.f.anchor.earliest_billable_start,
                         observed_at=self.f.stamp(self.f.now),other_tax_usd='50')
        self.request=dict(sequence=7,reviewed=True,observed_at=self.f.stamp(self.f.now),
            hard_deadline=self.accounting.current.hard_deadline,profile='iam',
            manifest={k:self.config.manifest[k] for k in ('path','sha256')},admission=admission,
            state_digest=recovery.digest(self.v6),code_digest=launch.code_digest())
        self.sequence=7;self.write_request();self.events=[];self.failure=None
        outer=self
        class Window:
            def __init__(self,*args,**kwargs):
                self.deadline=args[2];self.cleanup_principal=kwargs.get('cleanup_principal')
                item=outer.f.read()[b.OVERLAY]['attempts'][-1]
                outer.assertEqual(item['sequence'],outer.sequence);outer.assertEqual(item['phase'],'reserved')
                outer.assertEqual(item['additional_metering_seconds'],'1200')
                outer.events.append('reserved')
            def cleanup(self):
                outer.events.append('cleanup')
                if outer.failure=='cleanup':raise Blocked('remote_cleanup_unverified')
            def driver(self,manifest,cases,**kwargs):
                outer.assertEqual(outer.f.read()[b.OVERLAY]['attempts'][-1]['phase'],'active')
                outer.assertEqual(self.cleanup_principal,iam.DB_USER)
                outer.assertEqual(kwargs['credentials'].user,iam.DB_USER)
                outer.events.append(tuple(cases));self.last_driver_summary=None
                if outer.failure=='identity':raise Blocked('driver_baseline_failed')
                return dict(cases=list(cases),passed=len(cases),failed=0)
        self.Window=Window

    def write(self,name,value):
        p=self.directory/name;p.write_text(json.dumps(value));p.chmod(0o600)
    def write_request(self):self.write(f'window-{self.request["sequence"]:03d}-request.json',self.request)

    def execute(self,controls=None):
        real_session=continuation.continuation_session
        def session(path,anchor):return real_session(path,anchor,clock=lambda:self.f.now)
        def exchange(*args):
            self.events.append('exchange')
            self.assertEqual(self.f.read()[b.OVERLAY]['attempts'][-1]['phase'],'active')
            return iam.DatabaseCredentials(iam.DB_USER,'synthetic-not-a-password',self.f.now+timedelta(seconds=900))
        with ExitStack() as stack:
            stack.enter_context(patch.object(windows,'utcnow',side_effect=lambda:self.f.now))
            stack.enter_context(patch.object(continuation,'continuation_session',side_effect=session))
            stack.enter_context(patch.object(windows,'fresh_controls',side_effect=controls))
            stack.enter_context(patch.object(live,'Window',self.Window))
            stack.enter_context(patch.object(iam,'acquire',side_effect=exchange))
            stack.enter_context(patch.object(live,'aws',side_effect=AssertionError('cloud forbidden')))
            return launch.execute(self.sequence,self.directory)

    def test_exact_iam_inventory_after_reservation_and_identity_with_preserved_history(self):
        result=self.execute();self.assertEqual(result['status'],'passed');self.assertTrue(result['cleanup_verified'])
        self.assertEqual(self.events,['reserved','cleanup','exchange',live.IDENTITY,windows.PROFILES['iam'],'cleanup'])
        overlay=self.f.read()[b.OVERLAY]
        self.assertEqual(overlay['source_recovery_raw'],self.accounting.source)
        self.assertEqual(overlay['attempts'][0]['sequence'],7)
        self.assertEqual(overlay['attempts'][0]['phase'],'cleaned_pending_billing')
        self.assertIsNone(result['actual_spend_usd']);self.assertIsNone(result['remaining_allowance_usd'])
        with self.assertRaises(Blocked):self.execute()

    def test_008_requires_new_sequence_and_new_reviewed_state_binding(self):
        self.execute();self.sequence=8;self.request['sequence']=8
        self.write_request()
        with self.assertRaisesRegex(Blocked,'continuation_source_changed'):self.execute()
        self.events=[];self.f.now+=timedelta(seconds=1)
        self.request.update(state_digest=recovery.digest(self.f.path.read_text()),
            observed_at=self.f.stamp(self.f.now),hard_deadline=self.f.stamp(self.f.now+timedelta(seconds=180)))
        self.request['admission']['observed_at']=self.request['observed_at'];self.write_request()
        result=self.execute();self.assertEqual(result['status'],'passed')
        self.assertEqual([a['sequence'] for a in self.f.read()[b.OVERLAY]['attempts']],[7,8])
        self.assertEqual(self.events,['reserved','cleanup','exchange',live.IDENTITY,windows.PROFILES['iam'],'cleanup'])

    def test_no_implicit_migration_or_original_sequence_replay(self):
        self.f.path.write_text(self.accounting.source)
        with self.assertRaises(Blocked):self.execute()
        self.assertEqual(self.f.path.read_text(),self.accounting.source);self.assertEqual(self.events,[])
        for seq in (1,6,9,True):
            with self.assertRaisesRegex(Blocked,'invalid_continuation_sequence'):launch.execute(seq,self.directory)
        self.f.path.write_text(self.v6)
        with self.assertRaises(Blocked):windows.execute(7,self.directory)

    def test_source_changed_during_controls_prevents_sql_and_reservation(self):
        def controls(*args):self.f.path.write_bytes(self.f.path.read_bytes()+b' ')
        with self.assertRaisesRegex(Blocked,'continuation_source_changed'):self.execute(controls)
        self.assertEqual(self.events,[]);self.assertEqual(self.f.read()[b.OVERLAY]['attempts'],[])

    def test_code_hash_inventory_and_unverified_controls_reject_before_sql(self):
        for key,value in [('code_digest','0'*64),('state_digest','0'*64),('reviewed',False),('profile','unbounded')]:
            original=copy.deepcopy(self.request);self.request[key]=value;self.write_request()
            with self.subTest(key=key):
                with self.assertRaises(Blocked):self.execute()
                self.assertEqual(self.events,[]);self.assertEqual(self.f.path.read_text(),self.v6)
            self.request=original
        self.write_request()
        with self.assertRaisesRegex(Blocked,'controls_failed'):
            self.execute(lambda *args: (_ for _ in ()).throw(Blocked('controls_failed')))
        self.assertEqual(self.events,[])

    def test_failed_identity_stops_profile_and_cleanup_failure_blocks_next_window(self):
        self.failure='identity';result=self.execute()
        self.assertTrue(result['cleanup_verified']);self.assertNotIn(windows.PROFILES['iam'],self.events)
        self.assertEqual(self.events,['reserved','cleanup','exchange',live.IDENTITY,'cleanup'])
        self.f.path.write_text(self.v6);(self.directory/'window-007-result.json').unlink()
        self.failure='cleanup';self.events=[];result=self.execute()
        self.assertFalse(result['cleanup_verified']);self.assertNotIn('exchange',self.events)
        self.assertEqual(self.f.read()[b.OVERLAY]['attempts'][0]['phase'],'uncertain')
        (self.directory/'window-007-result.json').unlink()
        with self.assertRaises(Blocked):self.execute()
        self.assertEqual(self.events,['reserved','cleanup','cleanup'])


if __name__=='__main__':unittest.main()
