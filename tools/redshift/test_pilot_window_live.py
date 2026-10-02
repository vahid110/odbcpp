"""Synthetic launcher composition and control tests; no paid SQL or credentials."""
import copy
from dataclasses import asdict, replace
from datetime import timedelta
import json
import unittest
from unittest.mock import patch

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_live as live
from tools.redshift import pilot_window_live as launch
from tools.redshift import pilot_windows as windows
from tools.redshift import test_pilot_windows as wf
from tools.redshift import test_pilot_live as lf
from tools.redshift.pilot_preflight import Blocked


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.w=wf.WindowTests();self.w.setUp();self.addCleanup(self.w.doCleanups)
        self.f=self.w.f;self.directory=self.f.directory
        self.f.now+=timedelta(seconds=120)
        self.l=lf.LiveCompositionTests();self.l.setUp();self.addCleanup(self.l.doCleanups)
        self.write('bootstrap-anchor.json',asdict(self.f.anchor))
        self.write('database-config.json',self.l.config)
        self.request=dict(sequence=1,reviewed=True,observed_at=self.f.stamp(self.f.now),
            hard_deadline=self.f.stamp(self.f.now+timedelta(seconds=180)),profile='scalar',
            manifest={k:self.l.manifest[k] for k in ('path','sha256')},admission=copy.deepcopy(self.l.admission))
        self.request['admission'].update(account_id=self.f.anchor.account_id,
            earliest_billable_start=self.f.anchor.earliest_billable_start,
            observed_at=self.f.stamp(self.f.now),other_tax_usd='50')
        self.write_request()
        with self.w.session() as s:self.f.passed(s.initialize())
        self.events=[];self.fail=None;self.cleanups=0
        outer=self
        class FakeWindow:
            def __init__(self,*args):
                outer.assertEqual(outer.f.read()[b.OVERLAY]['attempts'][-1]['phase'],'reserved')
                outer.events.append('reserved')
            def cleanup(self):
                outer.events.append('cleanup');outer.cleanups+=1
                if outer.fail=='cleanup':raise Blocked('remote_cleanup_unverified')
            def sql(self,*args,**kw):
                outer.assertEqual(outer.f.read()[b.OVERLAY]['attempts'][-1]['phase'],'active')
                if outer.fail=='timeout':return '0'
                return '15000'
            def driver(self,manifest,cases):
                outer.events.append(tuple(cases));self.last_driver_summary=None
                if outer.fail=='interrupt':raise KeyboardInterrupt()
                if outer.fail=='identity' and cases==live.IDENTITY:raise Blocked('driver_baseline_failed')
                summary=dict(cases=list(cases),passed=len(cases),failed=0)
                if outer.fail=='profile' and cases!=live.IDENTITY:
                    summary.update(passed=len(cases)-1,failed=1);self.last_driver_summary=summary
                    raise Blocked('driver_baseline_failed')
                return summary
        self.FakeWindow=FakeWindow

    def write(self,name,value):self.l.write.__func__(self,name,value)
    def write_request(self):self.write('window-001-request.json',self.request)

    def execute(self, *, controls=None):
        session=windows.window_session
        def fake_session(path,anchor):return session(path,anchor,clock=lambda:self.f.now)
        with patch.object(launch,'utcnow',side_effect=lambda:self.f.now), \
             patch.object(windows,'window_session',side_effect=fake_session), \
             patch.object(launch,'fresh_controls',side_effect=controls), \
             patch.object(live,'Window',self.FakeWindow):
            return launch.execute(1,self.directory)

    def test_happy_path_durable_reservation_identity_inventory_and_no_refund(self):
        result=self.execute();self.assertEqual(result['status'],'passed');self.assertTrue(result['cleanup_verified'])
        self.assertEqual(self.events,['reserved','cleanup',live.IDENTITY,launch.PROFILES['scalar'],'cleanup'])
        state=self.f.read();attempt=state[b.OVERLAY]['attempts'][0]
        self.assertEqual(attempt['additional_metering_seconds'],'1200')
        self.assertEqual(attempt['phase'],'cleaned_pending_billing')
        self.assertIsNone(result['actual_spend_usd']);self.assertIsNone(result['remaining_allowance_usd'])
        self.assertNotIn(self.l.config['test_password'],json.dumps(result))
        with self.assertRaisesRegex(Blocked,'window_already_reported'):self.execute()
        (self.directory/'window-001-result.json').unlink()
        with self.assertRaisesRegex(Blocked,'window_already_consumed'):self.execute()
        self.assertEqual(len(self.f.read()[b.OVERLAY]['attempts']),1)

    def test_bad_requests_and_old_overlay_never_launch(self):
        original=copy.deepcopy(self.request)
        for key,value in [('reviewed',False),('profile','arbitrary'),('sequence',2),
            ('observed_at',self.f.stamp(self.f.now-timedelta(seconds=301))),
            ('hard_deadline',self.f.stamp(self.f.now+timedelta(seconds=181))),
            ('hard_deadline',self.f.stamp(self.f.now+timedelta(seconds=59)))]:
            self.request=copy.deepcopy(original);self.request[key]=value;self.write_request()
            with self.assertRaises(Blocked):self.execute()
            self.assertEqual(self.events,[]);self.assertEqual(self.f.read()[b.OVERLAY]['attempts'],[])
        self.request=original;self.write_request();self.f.write(self.w.original)
        with self.assertRaisesRegex(Blocked,'invalid_window_overlay'):self.execute()
        self.assertEqual(self.events,[])

    def test_preflight_failure_and_expiry_never_reserve(self):
        def fail(*args):raise Blocked('price_changed')
        with self.assertRaisesRegex(Blocked,'price_changed'):self.execute(controls=fail)
        def slow(*args):self.f.now+=timedelta(seconds=121)
        with self.assertRaisesRegex(Blocked,'invalid_window_deadline'):self.execute(controls=slow)
        self.assertEqual(self.f.read()[b.OVERLAY]['attempts'],[]);self.assertEqual(self.events,[])

    def test_failed_identity_never_runs_profile_but_cleans(self):
        self.fail='identity';result=self.execute()
        self.assertTrue(result['cleanup_verified']);self.assertEqual(result['reason'],'driver_baseline_failed')
        self.assertNotIn(launch.PROFILES['scalar'],self.events);self.assertEqual(self.cleanups,2)

    def test_failed_profile_retains_sanitized_counts_and_liability(self):
        self.fail='profile';result=self.execute()
        self.assertEqual(result['results'][-1]['failed'],1);self.assertTrue(result['cleanup_verified'])
        self.assertGreater(float(self.f.read()[b.OVERLAY]['compute_upper_bound_usd']),0)

    def test_cleanup_failure_blocks_future_window_and_interrupt_still_cleans(self):
        self.fail='cleanup';result=self.execute();self.assertFalse(result['cleanup_verified'])
        self.assertEqual(self.f.read()[b.OVERLAY]['attempts'][-1]['phase'],'uncertain')
        self.assertEqual(self.cleanups,2)

    def test_interrupt_uses_independent_cleanup(self):
        self.fail='interrupt';result=self.execute()
        self.assertEqual(result['reason'],'execution_interrupted');self.assertTrue(result['cleanup_verified'])
        self.assertEqual(self.cleanups,2)

    def test_report_failure_cannot_reopen_consumed_window(self):
        with patch.object(launch,'save_report',side_effect=OSError('secret-canary')):
            with self.assertRaisesRegex(Blocked,'window_report_persistence_failed'):self.execute()
        with self.assertRaisesRegex(Blocked,'window_already_consumed'):self.execute()
        self.assertEqual(self.cleanups,2)

    def test_stale_executable_or_symlink_report_blocks_before_reservation(self):
        self.l.exe.write_bytes(b'changed')
        with self.assertRaisesRegex(Blocked,'executable_changed'):self.execute()
        self.assertEqual(self.events,[])
        (self.directory/'window-001-result.json').symlink_to(self.directory/'missing')
        with self.assertRaisesRegex(Blocked,'window_already_reported'):self.execute()


class FreshControlsTests(unittest.TestCase):
    def setUp(self):
        self.f=lf.LiveCompositionTests();self.f.setUp();self.addCleanup(self.f.doCleanups)
        self.anchor=b.BootstrapAnchor(**self.f.anchor)
        self.f.usage['usageLimits'][0]['amount']=267
        self.f.budget['Budget'].update(BudgetType='COST',TimeUnit='MONTHLY',CostTypes={'IncludeTax':True})
        self.f.budget['Budget']['BudgetLimit']['Amount']='150'
        self.notifications={'Notifications':[dict(NotificationType='ACTUAL',ThresholdType='ABSOLUTE_VALUE',
            ComparisonOperator='GREATER_THAN',NotificationState='OK',Threshold=n) for n in (100,125,150)]}
        self.subscribers={'Subscribers':[{'SubscriptionType':'EMAIL','Address':'synthetic@example.invalid'}]}
        self.cidr=self.f.admission['allowed_ipv4_cidr']

    def aws(self,args):
        if args[1]=='describe-notifications-for-budget':return self.notifications
        if args[1]=='describe-subscribers-for-notification':
            self.assertEqual(set(json.loads(args[-1])),{'NotificationType','ComparisonOperator','Threshold','ThresholdType'})
            return self.subscribers
        return self.f.aws(args)

    def check(self):
        with patch.object(live,'aws',side_effect=self.aws),patch.object(launch,'price_and_ip',return_value=self.cidr), \
             patch.object(launch.Path,'is_file',return_value=True):
            launch.fresh_controls(self.anchor,self.f.config,self.f.admission)

    def test_fresh_amended_controls_pass_and_mismatches_fail(self):
        self.check()
        for record,key,value in [(self.f.usage['usageLimits'][0],'amount',20),
            (self.f.budget['Budget'],'TimeUnit','ANNUALLY'),
            (self.f.budget['Budget']['CostTypes'],'IncludeTax',False),
            (self.f.workgroup['workgroup'],'maxCapacity',8)]:
            old=record[key];record[key]=value
            with self.assertRaises(Blocked):self.check()
            record[key]=old
        self.cidr='203.0.113.1/32'
        with self.assertRaisesRegex(Blocked,'network_address_changed'):self.check()

    def test_missing_subscribers_duplicate_alerts_and_pagination_fail(self):
        self.subscribers={'Subscribers':[]}
        with self.assertRaisesRegex(Blocked,'budget_subscribers_missing'):self.check()
        self.subscribers={'Subscribers':[{'SubscriptionType':'EMAIL','Address':'synthetic@example.invalid'}]}
        self.notifications['Notifications'][2]['Threshold']=125
        with self.assertRaisesRegex(Blocked,'budget_alerts_mismatch'):self.check()
        self.notifications['Notifications'][2]['Threshold']=150
        self.notifications['NextToken']='more'
        with self.assertRaisesRegex(Blocked,'budget_alerts_mismatch'):self.check()

    def test_fixed_price_ip_proof_rejects_changed_rate_and_nonpublic_address(self):
        price={'products':{'ZUJGFS2VXV3ZX482':{'attributes':{'regionCode':live.REGION}}},
            'terms':{'OnDemand':{'ZUJGFS2VXV3ZX482':{'term':{'priceDimensions':{'d':{
                'unit':'RPU-Hr','pricePerUnit':{'USD':'.374'}}}}}}}}
        with patch.object(launch,'get_https',side_effect=[json.dumps(price).encode(),b'8.8.8.8\n']):
            self.assertEqual(launch.price_and_ip(),'8.8.8.8/32')
        price['terms']['OnDemand']['ZUJGFS2VXV3ZX482']['term']['priceDimensions']['d']['pricePerUnit']['USD']='.375'
        with patch.object(launch,'get_https',return_value=json.dumps(price).encode()):
            with self.assertRaisesRegex(Blocked,'price_changed'):launch.price_and_ip()


if __name__=='__main__':unittest.main()
