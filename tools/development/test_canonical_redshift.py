"""Offline canonical resource/input/fixture composition; no provider or database operations."""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import json
import copy
import datetime as dt
import dataclasses
import hashlib
import os
import shutil

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.development import redshift_fixture_factory as factory
from tools.development import redshift_profile_inputs as inputs
from tools.development.redshift_timeout_factory import TimeoutFactory
from tools.development.redshift_resource import NativeOps
from tools.development.redshift_ctest_ops import Operations
from tools.development.redshift_ctest_lifecycle import Refused
from tools.development import live_checkpoint as checkpoint


class CanonicalTests(unittest.TestCase):
    def test_Unicode_fixture_identifiers_and_SQL_match_original_contract(self):
        plan=factory.closed_fixture_plan()
        schema='odbcpp_u_\u00e9\u8868'
        table='t"\u00e9%_\u8868'
        self.assertEqual(plan['objects']['UNICODE_SCHEMA']['name'],schema)
        row=plan['objects']['UNICODE_TWO_ROWS']
        self.assertEqual(row['schema'],schema)
        self.assertEqual(row['name'],table)
        self.assertIn(schema,row['create_if_absent_SQL'])
        self.assertIn('"t""\u00e9%_\u8868"',row['create_if_absent_SQL'])
        self.assertEqual(factory._whole_ident(table),'"t""\u00e9%_\u8868"')

    def test_normal_composition_uses_fixed_modules_and_keeps_entire_cleanup_horizon(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);head=checkpoint.subprocess.check_output(
                ['git','-C',str(checkpoint.ROOT),'rev-parse','HEAD'],text=True).strip()
            def ref(name,value):
                path=root/(name+'.json');path.write_text(json.dumps(value))
                return {'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
            image=root/'synthetic-image';image.write_bytes(b'not executable')
            row=dict(id='synthetic-only',image='synthetic',profile='synthetic',gtest_filter='Probe.Case',timeout_seconds=40)
            tools={key:{'path':str(Path(shutil.which(key) if key in ('cmake','ctest') else sys.executable).resolve()),
                         'sha256':hashlib.sha256(Path(shutil.which(key) if key in ('cmake','ctest') else sys.executable).resolve().read_bytes()).hexdigest()}
                   for key in ('cmake','ctest','aws','psql','python')}
            plan=dict(schema_version=1,source_directory=str(checkpoint.ROOT),source_head=head,
                state_directory=str(root),run_name='synthetic-only',tools=tools,
                index=ref('index',dict(schema_version=1,images={'synthetic':dict(executable=str(image),sha256=hashlib.sha256(image.read_bytes()).hexdigest())},profiles={},cases=[row])),
                expected=ref('expected',dict(schema_version=1,cases=[{k:v for k,v in row.items() if k!='timeout_seconds'}])),
                profiles=ref('profiles',{}),specs=ref('specs',[]),
                ci_receipt=ref('ci',{'source_head':head,'applicable_ci_green':True,'synthetic_offline_only':True}),
                authority_receipts={k:ref(k,{'synthetic_offline_only':True}) for k in ('authority','applicability','cleanup_delegation')},
                resource=dict(account='385207570137',arn='arn:aws:iam::385207570137:user/odbcpp-redshift-pilot',cluster='odbcpp-redshift-provisioned-pilot',created='2026-10-03T13:29:23.907Z',region='eu-north-1',profile='odbcpp-redshift-pilot',node_type='synthetic-only'),
                ca_sha256='0'*64,security_groups=[{'synthetic_offline_only':True}],
                timeout_fixture=dict(table='odbcpp_timeout_gate_synthetic',expected_principal=dict(database='odbcpp_pilot',owner='odbcpp_pilot_test',owner_oid=202,schema_oid=101,schema_owner_oid=303)))
            plan_path=root/'plan.json';plan_path.write_text(json.dumps(plan))
            now=dt.datetime.now(dt.timezone.utc)
            admission=dict(reviewed=True,plan_sha256=hashlib.sha256(plan_path.read_bytes()).hexdigest(),
                source_head=head,inventory_scope='complete_applicable',run_name='synthetic-only',
                nonce='synthetic_offline_only_12345',expires_utc=(now+dt.timedelta(hours=3)).isoformat(),
                bounds=dataclasses.asdict(checkpoint.lifecycle.Bounds()),
                credential_source=str(root/'never-read-credentials'),database_config=str(root/'never-read-config'))
            admission_path=root/'synthetic-admission.json';admission_path.write_text(json.dumps(admission));admission_path.chmod(0o600)
            def fake_git(command,**kwargs):return head if command[-1]=='HEAD' else ''
            def no_resources(config,ops,fixtures):
                self.assertIs(ops.factory,factory)
                self.assertIs(ops.launcher,inputs)
                self.assertEqual(ops.preparation_manifest['security_groups'],plan['security_groups'])
                self.assertEqual(config.state_directory,root)
                self.assertFalse(ops.keep_cluster_running)
                self.assertLessEqual(fixtures.plan()['total_inclusive'],450)
                return {'synthetic_offline_only':True}
            with patch.object(checkpoint.subprocess,'check_output',side_effect=fake_git), \
                 patch.object(checkpoint.lifecycle,'run_once',side_effect=no_resources) as run, \
                 patch('tools.development.redshift_ctest_ops.load_public',side_effect=AssertionError('private Python import')):
                self.assertTrue(checkpoint.execute(plan,plan_path,admission_path)['synthetic_offline_only'])
                self.assertEqual(run.call_count,1)
                # Initial remaining horizon covers owner; preparation consumes
                # five seconds, so acquisition must refuse before any resource.
                admission['expires_utc']=(now+dt.timedelta(seconds=checkpoint.lifecycle.Bounds().owner+3)).isoformat()
                admission_path.write_text(json.dumps(admission));admission_path.chmod(0o600)
                class AdvancingClock(dt.datetime):
                    calls=0
                    @classmethod
                    def now(cls,tz=None):
                        cls.calls+=1
                        return now+dt.timedelta(seconds=0 if cls.calls==1 else 5)
                with patch.object(checkpoint.dt,'datetime',AdvancingClock), \
                     self.assertRaisesRegex(Refused,'remaining_operational_horizon'):
                    checkpoint.execute(plan,plan_path,admission_path)
                self.assertEqual(run.call_count,1)
            self.assertFalse((root/'never-read-credentials').exists())
            self.assertFalse((root/'synthetic-only').exists())

    def test_rejected_identity_SG_probe_and_TLS_never_reach_resume(self):
        for failure,reason in (('identity','principal'),('SG','SG_changed'),
                               ('probe','network_probe_failed_or_late'),('TLS','TLS_required')):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                root=Path(directory);calls=[]
                resource=dict(account='synthetic-account',arn='synthetic-arn',cluster='synthetic-cluster',
                    created='2026-10-03T13:29:23.907Z',node_type='synthetic-type')
                groups=[{'IpPermissions':[dict(IpProtocol='tcp',FromPort=5439,ToPort=5439,
                    IpRanges=[{'CidrIp':'8.8.8.8/32'}],Ipv6Ranges=[],UserIdGroupPairs=[],PrefixListIds=[])]}]
                native=NativeOps({'resource':resource},{'python':sys.executable,
                    'pins':{sys.executable:'0'*64},'security_groups':groups},root,2000)
                native.config={'host':'synthetic.invalid'};native.now=lambda:100
                def fake_aws(args,end):
                    operation=tuple(args[:2]);calls.append(operation)
                    if operation==('sts','get-caller-identity'):
                        return {'Account':'wrong' if failure=='identity' else resource['account'],'Arn':resource['arn']}
                    if operation==('redshift','describe-clusters'):
                        return {'Clusters':[dict(ClusterIdentifier=resource['cluster'],
                            NodeType=resource['node_type'],NumberOfNodes=1,Encrypted=True,
                            Endpoint={'Address':'synthetic.invalid','Port':5439},
                            ClusterCreateTime=resource['created'],ClusterStatus='paused',
                            VpcSecurityGroups=[{'VpcSecurityGroupId':'sg-synthetic'}],
                            ClusterParameterGroups=[{'ParameterGroupName':'synthetic'}]) ]}
                    if operation==('ec2','describe-security-groups'):
                        return {'SecurityGroups':[] if failure=='SG' else groups}
                    if operation==('redshift','describe-cluster-parameters'):
                        return {'Parameters':[{'ParameterName':'require_ssl','ParameterValue':'false' if failure=='TLS' else 'true'}]}
                    raise AssertionError('Unexpected mutation')
                def fake_probe(argv,*,output,**kwargs):
                    if failure!='probe':output.write_text('8.8.8.8\n')
                    return SimpleNamespace(returncode=1 if failure=='probe' else 0,timed_out=False)
                native.aws=fake_aws;native.owned_process=fake_probe
                with self.assertRaisesRegex(Refused,reason):native.security_paused()
                self.assertNotIn(('redshift','resume-cluster'),calls)

    def test_paused_security_network_TLS_resume_and_pause_with_fake_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);clock=[100.0];status=['paused'];calls=[]
            resource=dict(account='synthetic-account',arn='synthetic-arn',cluster='synthetic-cluster',
                created='2026-10-03T13:29:23.907Z',node_type='synthetic-type',
                profile='synthetic-profile',region='synthetic-region')
            groups=[{'IpPermissions':[dict(IpProtocol='tcp',FromPort=5439,ToPort=5439,
                IpRanges=[{'CidrIp':'8.8.8.8/32'}],Ipv6Ranges=[],UserIdGroupPairs=[],PrefixListIds=[])]}]
            native=NativeOps({'resource':resource},{'aws':sys.executable,'python':sys.executable,
                'pins':{sys.executable:'0'*64},'security_groups':copy.deepcopy(groups)},root,2000)
            native.aws_env={};native.config={'host':'synthetic.invalid'};native.now=lambda:clock[0]
            def fake_process(argv,*,output,**kwargs):
                if argv[1:3]==['-I','-c']:
                    output.write_text('8.8.8.8\n')
                else:
                    operation=next(tuple(argv[i:i+2]) for i in range(len(argv)-1)
                        if argv[i] in ('sts','redshift','ec2'))
                    calls.append(operation)
                    if operation==('sts','get-caller-identity'):
                        value={'Account':resource['account'],'Arn':resource['arn']}
                    elif operation==('redshift','describe-clusters'):
                        value={'Clusters':[dict(ClusterIdentifier=resource['cluster'],
                            NodeType=resource['node_type'],NumberOfNodes=1,Encrypted=True,
                            Endpoint={'Address':'synthetic.invalid','Port':5439},
                            ClusterCreateTime=resource['created'],ClusterStatus=status[0],
                            ClusterAvailabilityStatus='Available',
                            VpcSecurityGroups=[{'VpcSecurityGroupId':'sg-synthetic'}],
                            ClusterParameterGroups=[{'ParameterGroupName':'synthetic'}]) ]}
                    elif operation==('ec2','describe-security-groups'):
                        value={'SecurityGroups':groups}
                    elif operation==('redshift','describe-cluster-parameters'):
                        value={'Parameters':[{'ParameterName':'require_ssl','ParameterValue':'true'}]}
                    elif operation==('redshift','resume-cluster'):
                        status[0]='available';value={}
                    elif operation==('redshift','pause-cluster'):
                        status[0]='paused';value={}
                    else:
                        raise AssertionError('Unexpected operation')
                    output.write_text(json.dumps(value))
                return SimpleNamespace(returncode=0,timed_out=False)
            native.owned_process=fake_process
            with patch('tools.development.redshift_resource.time.sleep',side_effect=lambda seconds:clock.__setitem__(0,clock[0]+seconds)):
                native.security_paused()
                self.assertEqual(status[0],'paused')
                self.assertTrue(json.loads((root/'network-controls.json').read_text())['exact_single_ipv4_rule'])
                native.resume_available(500)
                self.assertEqual(status[0],'available')
                self.assertTrue(native.pause_until(600))
            self.assertEqual(status[0],'paused')
            self.assertEqual(calls.count(('redshift','resume-cluster')),1)
            self.assertEqual(calls.count(('redshift','pause-cluster')),1)
            with self.assertRaises(Exception):native.aws(['redshift','delete-cluster'],600)
            self.assertNotIn(('redshift','delete-cluster'),calls)

    def test_resource_and_profile_deadlines_share_the_existing_absolute_clock(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch('time.clock_gettime',return_value=100):
            native=NativeOps({'resource':{}},{},Path(directory),200)
            self.assertEqual(native.now(),100)
            inputs._profile_input_deadline(native.now()+1)
            with self.assertRaises(Exception):
                inputs._profile_input_deadline(native.now())

    def test_timeout_SQL_uses_current_identity_without_cascading_replacement(self):
        expected=dict(database='odbcpp_pilot',owner='odbcpp_pilot_test',
                      owner_oid='202',schema_oid='101',schema_owner_oid='303')
        timeout=TimeoutFactory('odbcpp_timeout_gate_synthetic',expected)
        permission=dict(expected,usage='t',create='f',absent='t')
        queries=timeout.query_set(timeout.TABLE,{'permission':permission})
        self.assertIn('oid=101',queries['grant'][0])
        self.assertIn('nspowner=303',queries['grant'][0])
        self.assertIn('usesysid=202',queries['grant'][0])
        self.assertNotIn('107830',queries['grant'][0])
        self.assertEqual(timeout.preflight(dict(permission,create='t')),dict(permission,create='t'))
        with self.assertRaises(Exception):
            timeout.permission(dict(permission,owner_oid='999'),permission)

    def test_fixed_components_bypass_all_private_source_loading(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch('tools.development.redshift_ctest_ops.load_public',side_effect=AssertionError('private code import')):
            timeout=TimeoutFactory('odbcpp_timeout_gate_synthetic',dict(
                database='odbcpp_pilot',owner='odbcpp_pilot_test',owner_oid=202,schema_oid=101,schema_owner_oid=303))
            ops=Operations(native_source=None,native_sha256=None,factory_source=None,
                factory_sha256=None,launcher_source=None,launcher_sha256=None,
                approval={'run_directory':directory},preparation_manifest={},profile_manifest={},
                profiles={},specs=[],blueprint={},authority_receipts={},
                components=(timeout,factory,inputs),executables=(sys.executable,))
            self.assertIs(ops.factory,factory)
            self.assertIs(ops.launcher,inputs)
            self.assertEqual(len(factory.closed_fixture_plan()['cases']),155)
            self.assertLessEqual(ops.combined_fixture_plan()['total_inclusive'],450)

    def test_fake_provider_identity_and_resource_mismatches_refuse(self):
        with tempfile.TemporaryDirectory() as directory:
            resource=dict(account='synthetic-account',arn='synthetic-arn',cluster='synthetic-cluster',
                          created='2026-10-03T13:29:23.907Z',node_type='synthetic-type')
            native=NativeOps({'resource':resource},{},Path(directory),100)
            native.config={'host':'synthetic.invalid'}
            native.aws=lambda args,end:{'Account':'wrong','Arn':'synthetic-arn'}
            with self.assertRaises(Exception):native.identity(100)
            native.aws=lambda args,end:{'Clusters':[dict(ClusterIdentifier='synthetic-cluster',
                NodeType='synthetic-type',NumberOfNodes=1,Encrypted=True,
                Endpoint={'Address':'wrong.invalid','Port':5439},ClusterCreateTime=resource['created'])]}
            with self.assertRaises(Exception):native.cluster(100)


if __name__ == '__main__':
    unittest.main()
