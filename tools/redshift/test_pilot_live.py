"""Composition tests use synthetic AWS records and no live processes/credentials."""
from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.redshift import pilot_live as live
from tools.redshift.pilot_preflight import Blocked


class LiveCompositionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name).resolve(); self.directory.chmod(0o700)
        self.now = datetime.now(timezone.utc)
        stamp = lambda d:d.isoformat().replace('+00:00','Z')
        self.anchor = dict(pilot_id='initial-redshift-pilot',period_id='initial-20261002',
            account_id=live.ACCOUNT,region=live.REGION,workgroup_id='11111111-1111-1111-1111-111111111111',
            namespace_id='22222222-2222-2222-2222-222222222222',
            earliest_billable_start=stamp(self.now-timedelta(minutes=10)),
            hard_deadline=stamp(self.now+timedelta(minutes=3)),operator_assigned_existing_period=True)
        self.config = dict(host=f'{live.NAME}.{live.ACCOUNT}.{live.REGION}.redshift-serverless.amazonaws.com',
            port=5439,database='odbcpp_pilot',admin_user='odbcpp_pilot_admin',test_user='odbcpp_pilot_test',
            admin_password='Aa1!'+'0'*48,test_password='Aa1!'+'1'*48,schema='odbcpp_fixture',table='pilot_rows',
            ca_file='/etc/ssl/cert.pem')
        self.exe=self.directory/'exe';self.exe.write_bytes(b'synthetic never executed')
        self.manifest=dict(path=str(self.exe),sha256=hashlib.sha256(self.exe.read_bytes()).hexdigest(),
            identity_case=live.IDENTITY[0],baseline_cases=list(live.BASELINE))
        self.write('bootstrap-anchor.json',self.anchor);self.write('database-config.json',self.config)
        self.write('executable-admission.json',self.manifest)
        self.write('setup.json',dict(account_id=live.ACCOUNT,region=live.REGION,approved_total_usd=15,
            compute_envelope_usd=10,other_charges_and_tax_reserve_usd=5,reserved_compute_usd=0,
            usage_windows=[],live_testing_enabled=False,actual_spend_usd_verified=None,
            remaining_allowance_usd_verified=None,history=['retain']))
        self.workgroup={'workgroup':dict(workgroupId=self.anchor['workgroup_id'],namespaceName=live.NAME,
            creationDate=stamp(self.now-timedelta(minutes=9)),status='AVAILABLE',baseCapacity=4,maxCapacity=4,
            endpoint=dict(address=self.config['host'],port=5439),securityGroupIds=['sg-one'],workgroupArn='exact-workgroup',
            configParameters=[dict(parameterKey='require_ssl',parameterValue='true'),
                              dict(parameterKey='max_query_execution_time',parameterValue='30')])}
        self.namespace={'namespace':dict(namespaceId=self.anchor['namespace_id'],
            creationDate=stamp(self.now-timedelta(minutes=10)))}
        self.usage={'usageLimits':[dict(resourceArn='exact-workgroup',amount=20,period='monthly',
                                      usageType='serverless-compute',breachAction='deactivate')]}
        self.network={'SecurityGroups':[dict(GroupId='sg-one',OwnerId=live.ACCOUNT,VpcId='vpc-one',IpPermissions=[dict(IpProtocol='tcp',
            FromPort=5439,ToPort=5439,IpRanges=[dict(CidrIp='192.0.2.1/32')])])]}
        self.budget={'Budget':dict(BudgetLimit=dict(Amount='15',Unit='USD'),
            TimePeriod=dict(Start='2026-10-02T00:00:00Z',End='2026-10-12T00:00:00Z'))}
        self.admission=dict(account_id=live.ACCOUNT,region=live.REGION,
            workgroup_id=self.anchor['workgroup_id'],namespace_id=self.anchor['namespace_id'],
            earliest_billable_start=self.anchor['earliest_billable_start'],observed_at=stamp(self.now),
            usd_per_rpu_hour='.374',other_tax_usd='5',earliest_start_verified=True,
            no_other_billable_activity=True,other_tax_bound_verified=True,
            price_source='https://pricing.us-east-1.amazonaws.com/offers/v1.0/aws/AmazonRedshift/current/eu-north-1/index.json',
            initial_inventory_basis='synthetic initial absence',other_tax_basis='full approved reserve',
            security_group_id='sg-one',vpc_id='vpc-one',allowed_ipv4_cidr='192.0.2.1/32')
        self.write('bootstrap-admission.json',self.admission)
        self.fail_driver=False;self.fail_cleanup=False;self.launched=False
        outer=self
        class FakeWindow:
            def __init__(self,directory,config,deadline):
                state=json.loads((directory/'setup.json').read_text())
                outer.assertEqual(state['bootstrap_overlay']['window']['phase'],'active')
                outer.launched=True
            def sql(self,text,**kw):
                if 'POSITION' in text:return '1'
                if 'SHOW' in text:return '15000'
                return ''
            def prepare_fixture(self):pass
            def driver(self,manifest,cases):
                if outer.fail_driver:raise Blocked('driver_baseline_failed')
                return dict(cases=list(cases),passed=len(cases),failed=0)
            def cleanup(self):
                if outer.fail_cleanup:raise Blocked('remote_cleanup_unverified')
        self.FakeWindow=FakeWindow

    def write(self,name,value):
        q=self.directory/name;q.write_text(json.dumps(value));q.chmod(0o600)

    def aws(self,args):
        if args[:2]==['sts','get-caller-identity']:
            return dict(Account=live.ACCOUNT,Arn=f'arn:aws:iam::{live.ACCOUNT}:user/{live.NAME}')
        return {'get-workgroup':self.workgroup,'get-namespace':self.namespace,
            'list-usage-limits':self.usage,'describe-security-groups':self.network,
            'describe-budget':self.budget,
            'list-workgroups':{'workgroups':[self.workgroup['workgroup']]},
            'list-namespaces':{'namespaces':[self.namespace['namespace']]} }[args[1]]

    def execute(self):
        with patch.object(live,'aws',side_effect=self.aws),patch.object(live,'Window',self.FakeWindow):
            return live.execute(self.directory)

    def test_reservation_precedes_execution_and_billing_remains_unknown(self):
        result=self.execute();self.assertEqual(result['status'],'passed')
        self.assertTrue(result['cleanup_verified']);self.assertIsNone(result['actual_spend_usd'])
        state=json.loads((self.directory/'setup.json').read_text())
        self.assertEqual(state['bootstrap_overlay']['window']['phase'],'cleaned_pending_billing')
        self.assertIsNone(state['remaining_allowance_usd_verified'])
        self.assertNotIn(self.config['test_password'],json.dumps(result))
        with self.assertRaisesRegex(Blocked,'already_initialized'):self.execute()

    def test_capacity_or_network_mismatch_never_launches(self):
        self.workgroup['workgroup']['maxCapacity']=8
        with self.assertRaisesRegex(Blocked,'workgroup_controls_mismatch'):self.execute()
        self.assertFalse(self.launched)
        self.workgroup['workgroup']['maxCapacity']=4
        self.network['SecurityGroups'][0]['IpPermissions'][0]['IpRanges'][0]['CidrIp']='0.0.0.0/0'
        with self.assertRaisesRegex(Blocked,'network_controls_mismatch'):self.execute()
        self.assertFalse(self.launched)

    def test_driver_failure_cleans_but_does_not_refund(self):
        self.fail_driver=True;result=self.execute()
        self.assertEqual(result['reason'],'driver_baseline_failed');self.assertTrue(result['cleanup_verified'])
        state=json.loads((self.directory/'setup.json').read_text())
        self.assertGreater(float(state['bootstrap_overlay']['window']['total_upper_bound_usd']),5)

    def test_cleanup_failure_retains_uncertain_reservation(self):
        self.fail_cleanup=True;result=self.execute()
        self.assertEqual(result['reason'],'remote_cleanup_unverified');self.assertFalse(result['cleanup_verified'])
        state=json.loads((self.directory/'setup.json').read_text())
        self.assertEqual(state['bootstrap_overlay']['window']['phase'],'uncertain')

    def test_missing_provenance_never_initializes_or_launches(self):
        for key in ('earliest_start_verified','no_other_billable_activity','other_tax_bound_verified'):
            with self.subTest(key=key):
                self.admission[key]=False;self.write('bootstrap-admission.json',self.admission)
                with self.assertRaisesRegex(Blocked,'admission_provenance_mismatch'):self.execute()
                self.assertNotIn('bootstrap_overlay',json.loads((self.directory/'setup.json').read_text()))
                self.assertFalse(self.launched);self.admission[key]=True

    def test_minimal_environment_excludes_unrecognized_secrets(self):
        with patch.dict('os.environ',{'CUSTOM_TOKEN':'secret','DATABASE_URL':'secret','GITHUB_TOKEN':'secret'}):
            self.assertFalse(set(('CUSTOM_TOKEN','DATABASE_URL','GITHUB_TOKEN')) & set(live.clean_env()))

    def test_wrong_host_owner_or_vpc_blocks(self):
        group=self.network['SecurityGroups'][0]
        for key,value in [('OwnerId','000000000000'),('VpcId','vpc-other')]:
            original=group[key];group[key]=value
            with self.assertRaisesRegex(Blocked,'network_controls_mismatch'):self.execute()
            self.assertFalse(self.launched);group[key]=original
        group['IpPermissions'][0]['IpRanges'][0]['CidrIp']='192.0.2.2/32'
        with self.assertRaisesRegex(Blocked,'network_controls_mismatch'):self.execute()
        self.assertFalse(self.launched)

    def test_wrong_price_or_reserve_or_stale_provenance_blocks(self):
        for key,value in [('usd_per_rpu_hour','.001'),('other_tax_usd','0'),
                          ('price_source','untrusted'),('earliest_billable_start','changed')]:
            original=self.admission[key];self.admission[key]=value
            self.write('bootstrap-admission.json',self.admission)
            with self.assertRaisesRegex(Blocked,'admission_provenance_mismatch'):self.execute()
            self.admission[key]=original;self.assertFalse(self.launched)
        self.admission['observed_at']=(self.now-timedelta(minutes=6)).isoformat()
        self.write('bootstrap-admission.json',self.admission)
        with self.assertRaisesRegex(Blocked,'admission_provenance_stale'):self.execute()
        self.assertFalse(self.launched)

    def test_delay_after_reservation_does_not_renew_deadline(self):
        now=self.now
        class DelayedClock(datetime):
            count=0
            @classmethod
            def now(cls,tz=None):
                cls.count+=1
                return now+(timedelta(minutes=4) if cls.count>=4 else timedelta())
        with patch.object(live,'datetime',DelayedClock):
            with self.assertRaisesRegex(Blocked,'execution_deadline'):self.execute()
        self.assertFalse(self.launched)
        state=json.loads((self.directory/'setup.json').read_text())
        self.assertEqual(state['bootstrap_overlay']['window']['phase'],'uncertain')

    def test_duplicate_protected_keys_and_extra_admission_fields_block(self):
        path=self.directory/'bootstrap-admission.json'
        original=json.dumps(self.admission)
        for duplicate in ('true','false'):
            path.write_text(original[:-1]+',"earliest_start_verified":'+duplicate+'}')
            with self.assertRaises(Blocked):self.execute()
            self.assertNotIn('bootstrap_overlay',json.loads((self.directory/'setup.json').read_text()))
        self.admission['unreviewed']='extra';self.write('bootstrap-admission.json',self.admission)
        with self.assertRaisesRegex(Blocked,'admission_provenance_mismatch'):self.execute()
        self.assertFalse(self.launched)

    def test_fixture_handles_partial_setup_with_bounded_owned_ddl(self):
        w=live.Window(self.directory,self.config,0)
        with patch.object(w,'sql') as sql:w.prepare_fixture()
        text=sql.call_args.args[0]
        self.assertIn('\\if :pilot_user_exists',text)
        self.assertIn('CREATE SCHEMA IF NOT EXISTS odbcpp_fixture',text)
        self.assertIn('DROP TABLE IF EXISTS odbcpp_fixture.pilot_rows',text)
        self.assertNotIn('DROP SCHEMA',text)
        self.assertEqual(sql.call_args.kwargs,dict(limit=90,statement_timeout_ms=30000))

    def test_cleanup_uses_serverless_views_and_separate_termination(self):
        calls=[]
        w=live.Window(self.directory,self.config,0)
        def sql(text,**kw):
            calls.append((text,kw))
            return '' if len(calls)==1 else '0|0'
        with patch.object(w,'sql',side_effect=sql):w.cleanup()
        self.assertIn('sys_session_history',calls[0][0])
        self.assertIn('\\gexec',calls[0][0])
        self.assertNotIn('stv_sessions',calls[0][0])
        self.assertIn('sys_query_history',calls[1][0])
        self.assertIn("'planning','queued','running','returning'",calls[1][0])
        self.assertEqual(calls[1][1],dict(limit=35,statement_timeout_ms=30000))

    def test_changed_executable_blocks_before_reservation(self):
        self.exe.write_bytes(b'changed')
        with self.assertRaisesRegex(Blocked,'executable_changed'):self.execute()
        self.assertFalse(self.launched)
        self.assertNotIn('bootstrap_overlay',json.loads((self.directory/'setup.json').read_text()))

    def test_unsafe_password_or_changed_executable_never_launches(self):
        self.config['test_password']="wrong;injected";self.write('database-config.json',self.config)
        with self.assertRaisesRegex(Blocked,'database_config_mismatch'):self.execute()
        self.assertFalse(self.launched)

    def test_iam_cleanup_covers_exact_principal_in_termination_and_both_counts(self):
        w=live.Window(self.directory,self.config,0,cleanup_principal=live.IAM_DB_USER)
        statements=[]
        def sql(text,**kwargs):
            statements.append(text);return '0|0' if 'COUNT(*)' in text else ''
        with patch.object(w,'sql',side_effect=sql):w.cleanup()
        self.assertEqual(len(statements),2)
        for text in statements:
            self.assertIn("usename='"+live.IAM_DB_USER+"'",text)
            self.assertIn(':admin_id,:test_id,:iam_id',text)
        self.assertEqual(statements[1].count(':admin_id,:test_id,:iam_id'),2)
        with patch.object(w,'sql',side_effect=['','0|1']):
            with self.assertRaisesRegex(Blocked,'remote_cleanup_unverified'):w.cleanup()
        with self.assertRaises(Blocked):
            live.Window(self.directory,self.config,0,cleanup_principal='unrelated')

    def test_iam_driver_private_env_fixed_inventory_and_expiry_refusal(self):
        import time
        from types import SimpleNamespace
        from tools.redshift import pilot_iam_credentials as iam
        now=datetime.now(timezone.utc);end=now+timedelta(seconds=180)
        credentials=iam.DatabaseCredentials(iam.DB_USER,'synthetic-driver-password-canary',
            now+timedelta(seconds=900))
        w=live.Window(self.directory,self.config,time.monotonic()+180,
                      cleanup_principal=iam.DB_USER)
        summary=dict(cases=list(live.IDENTITY),passed=1,failed=0)
        with patch.object(live,'bounded_process',return_value=SimpleNamespace(timed_out=False,returncode=0)) as process, \
             patch.object(live,'test_summary',return_value=summary):
            self.assertEqual(w.driver(self.manifest,live.IDENTITY,credentials=credentials,
                execution_end=end),summary)
        args,kwargs=process.call_args
        self.assertNotIn(credentials.password,' '.join(args[0]))
        self.assertNotIn('AWS_ACCESS_KEY_ID',kwargs['env'])
        self.assertIn('SSL=1;',kwargs['env']['ODBCPP_REDSHIFT_TEST_CONNECTION'])
        self.assertIn('ODBCPP_IAM_NEGATIVE_INVALID_PASSWORD',
            kwargs['env']['ODBCPP_REDSHIFT_IAM_INVALID_CONNECTION'])
        expired=iam.DatabaseCredentials(iam.DB_USER,credentials.password,now+timedelta(seconds=100))
        with patch.object(live,'bounded_process') as process:
            with self.assertRaisesRegex(Blocked,'iam_credential_validity_insufficient'):
                w.driver(self.manifest,live.IDENTITY,credentials=expired,execution_end=end)
            process.assert_not_called()
            with self.assertRaisesRegex(Blocked,'iam_inventory_mismatch'):
                w.driver(self.manifest,('arbitrary',),credentials=credentials,execution_end=end)


if __name__=='__main__':unittest.main()
