"""Offline resource proofs: injected AWS replies only, no credentials or SQL."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.development.redshift_resource import NativeOps, validate_resource
from tools.development.redshift_ctest_lifecycle import Refused


def resource():
    return dict(kind='serverless', account='385207570137',
        arn='arn:aws:iam::385207570137:user/odbcpp-redshift-pilot',
        region='eu-north-1', profile='odbcpp-redshift-pilot',
        workgroup='odbcpp-redshift-pilot',
        workgroup_id='d41cff82-7eb8-410d-be60-1d467c335756',
        namespace='odbcpp-redshift-pilot',
        namespace_id='d94a619c-bff3-42bb-8958-2405c99862db',
        namespace_created='2026-10-02T12:15:16.873Z',
        base_capacity=4, max_capacity=4,
        endpoint='odbcpp-redshift-pilot.385207570137.eu-north-1.redshift-serverless.amazonaws.com',
        port=5439, security_group_ids=['sg-06227a706295e3f05'],
        max_query_execution_time=30)


def replies():
    r = resource()
    prefix = 'arn:aws:redshift-serverless:eu-north-1:385207570137:'
    return dict(workgroup=dict(workgroupName=r['workgroup'], workgroupId=r['workgroup_id'],
        workgroupArn=prefix+'workgroup/'+r['workgroup_id'], namespaceName=r['namespace'],
        status='AVAILABLE', baseCapacity=4, maxCapacity=4, publiclyAccessible=True,
        securityGroupIds=r['security_group_ids'],
        endpoint=dict(address=r['endpoint'], port=5439),
        configParameters=[dict(parameterKey='require_ssl', parameterValue='true'),
                          dict(parameterKey='max_query_execution_time', parameterValue='30')]),
        namespace=dict(namespaceName=r['namespace'], namespaceId=r['namespace_id'],
            namespaceArn=prefix+'namespace/'+r['namespace_id'], status='AVAILABLE',
            creationDate=r['namespace_created'], dbName='odbcpp_pilot',
            adminUsername='odbcpp_pilot_admin'))


class ServerlessResource(unittest.TestCase):
    def native(self, directory, responses=None):
        r = resource()
        n = NativeOps({'resource':r}, {'security_groups':[{'GroupId':r['security_group_ids'][0]}]},
                      Path(directory), 2000)
        n.config = dict(host=r['endpoint'], port=5439, database='odbcpp_pilot',
                        admin_user='odbcpp_pilot_admin')
        n.now = lambda: 100
        n.calls = []
        responses = responses or replies()
        def aws(args, end):
            n.calls.append(args)
            if args[:2] == ['sts','get-caller-identity']:
                return {'Account':r['account'], 'Arn':r['arn']}
            if args[:2] == ['redshift-serverless','get-workgroup']:
                return {'workgroup':copy.deepcopy(responses['workgroup'])}
            if args[:2] == ['redshift-serverless','get-namespace']:
                return {'namespace':copy.deepcopy(responses['namespace'])}
            if args[:2] == ['ec2','describe-security-groups']:
                return {'SecurityGroups':copy.deepcopy(n.m['security_groups'])}
            raise AssertionError('unexpected cloud operation')
        n.aws = aws
        return n

    def test_strict_resource_rejects_expansion_or_historical_aliases(self):
        self.assertEqual(validate_resource(resource()), 'serverless')
        changes = [('kind','other'), ('max_capacity',8), ('base_capacity',True),
                   ('max_query_execution_time',40), ('endpoint','other.invalid'),
                   ('namespace_created','2026-10-03T12:15:16.873Z'),
                   ('security_group_ids',['sg-other']), ('cluster','old')]
        for key, value in changes:
            with self.subTest(key=key), self.assertRaises(Refused):
                validate_resource(dict(resource(), **{key:value}))
        missing=resource(); missing.pop('namespace_id')
        with self.assertRaises(Refused):
            validate_resource(missing)

    def test_available_security_and_final_resource_never_claim_pause_or_SQL_idle(self):
        with tempfile.TemporaryDirectory() as directory:
            n = self.native(directory)
            with patch.object(n,'network_guard') as guard:
                n.security_available(300)
            guard.assert_called_once_with(n.m['security_groups'],300)
            n.resume_available(300)
            self.assertTrue(n.verify_idle(300))
            self.assertFalse(any(call[0]=='redshift' for call in n.calls))
            final=json.loads((Path(directory)/'serverless-final-resource.json').read_bytes())
            self.assertFalse(final['paused_verified'])
            self.assertFalse(final['physical_compute_idle_verified'])
            self.assertFalse(final['SQL_quiescence_verified_by_resource_metadata'])
            for method in (n.security_paused, lambda:n.pause_until(300)):
                with self.assertRaisesRegex(Refused,'serverless_has_no_paused_state'):
                    method()

    def test_wrong_actual_resource_refuses_before_network_or_SQL(self):
        mutations = [('workgroup','maxCapacity',8),('workgroup','endpoint',{'address':'other','port':5439}),
            ('workgroup','namespaceName','other'),('workgroup','status','MODIFYING'),
            ('namespace','status','DELETING'),('namespace','namespaceId','other'),
            ('namespace','creationDate','2026-10-03T12:15:16.873Z'),
            ('namespace','dbName','other'),('workgroup','securityGroupIds',['sg-other']),
            ('workgroup','configParameters',[dict(parameterKey='require_ssl',parameterValue='false')]),
            ('workgroup','configParameters',[dict(parameterKey='require_ssl',parameterValue='true'),
               dict(parameterKey='max_query_execution_time',parameterValue='31')])]
        for object_name,key,value in mutations:
            with self.subTest(object_name=object_name,key=key), tempfile.TemporaryDirectory() as directory:
                responses=replies();responses[object_name][key]=value
                n=self.native(directory,responses)
                with patch.object(n,'network_guard',side_effect=AssertionError('network after refusal')):
                    with self.assertRaises(Refused):
                        n.security_available(300)
                self.assertFalse((Path(directory)/'serverless-security.json').exists())

    def test_expired_metadata_and_wrong_egress_do_not_publish_success(self):
        for late in (True,False):
            with self.subTest(late=late), tempfile.TemporaryDirectory() as directory:
                n=self.native(directory)
                n.now=lambda:300 if late else 100
                with patch.object(n,'network_guard',side_effect=Refused('wrong_egress')):
                    with self.assertRaises(Refused):
                        n.security_available(300)
                self.assertFalse((Path(directory)/'serverless-security.json').exists())

    def test_serverless_aws_allowlist_refuses_pause_resume_or_resource_creation(self):
        with tempfile.TemporaryDirectory() as directory:
            n=self.native(directory)
            for operation in (['redshift','resume-cluster'],['redshift','pause-cluster'],
                              ['redshift-serverless','create-workgroup'],
                              ['redshift-serverless','update-workgroup'],
                              ['redshift-serverless','list-workgroups']):
                with self.subTest(operation=operation), patch.object(n,'owned_process') as child:
                    with self.assertRaisesRegex(Refused,'operational_only'):
                        NativeOps.aws(n,operation,300)
                    child.assert_not_called()

    def test_read_allowlist_cannot_enumerate_or_substitute_another_resource(self):
        with tempfile.TemporaryDirectory() as directory:
            n=self.native(directory)
            for operation in (['redshift-serverless','get-workgroup','--workgroup-name','other'],
                              ['redshift-serverless','get-namespace','--namespace-name','other'],
                              ['ec2','describe-security-groups']):
                with self.subTest(operation=operation), patch.object(n,'owned_process') as child:
                    with self.assertRaisesRegex(Refused,'exact_serverless_resource_read_only'):
                        NativeOps.aws(n,operation,300)
                    child.assert_not_called()


if __name__ == '__main__':
    unittest.main()
