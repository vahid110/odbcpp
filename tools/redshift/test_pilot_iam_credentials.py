"""Synthetic IAM contract tests; no cloud, SQL or real credentials."""
import copy
from datetime import datetime, timezone, timedelta
import unittest

from tools.redshift import pilot_iam_credentials as iam
from tools.redshift import pilot_live as live
from tools.redshift.pilot_preflight import Blocked


class IAMContractTests(unittest.TestCase):
    def setUp(self):
        self.now = datetime(2026, 10, 2, 19, tzinfo=timezone.utc)
        self.end = self.now + timedelta(seconds=180)
        self.expiry = (self.now + timedelta(seconds=900)).isoformat()
        self.identity = dict(Account=live.ACCOUNT,
            Arn=f'arn:aws:sts::{live.ACCOUNT}:assumed-role/{iam.ROLE_NAME}/{iam.SESSION_NAME}')
        self.role = dict(AssumedRoleUser=dict(Arn=self.identity['Arn']),
            Credentials=dict(AccessKeyId='synthetic-access-key',
                SecretAccessKey='synthetic-secret-key-canary',
                SessionToken='synthetic-session-token-canary', Expiration=self.expiry))
        self.db = dict(dbUser=iam.DB_USER, dbPassword='synthetic-db-password-canary',
                       expiration=self.expiry)
        self.config = dict(host='odbcpp-redshift-pilot.385207570137.eu-north-1.redshift-serverless.amazonaws.com',
            port=5439, database='odbcpp_pilot', admin_user='odbcpp_pilot_admin',
            test_user='odbcpp_pilot_test', schema='odbcpp_fixture', table='pilot_rows',
            ca_file='/etc/ssl/cert.pem', admin_password='admin-synthetic-password',
            test_password='SyntheticTestPassword123!')

    def test_exact_role_and_database_requests(self):
        iam.verify_role_identity(self.identity)
        self.assertEqual(iam.credential_request(), ['redshift-serverless','get-credentials',
            '--workgroup-name',live.NAME,'--db-name','odbcpp_pilot','--duration-seconds','900'])
        self.assertEqual(iam.ROLE_ARN,
            'arn:aws:iam::385207570137:role/odbcpp-redshift-test/odbcpp-redshift-test-runtime')

    def test_ephemeral_role_env_excludes_ambient_and_profile_fallback(self):
        c = iam.role_credentials(self.role,self.now,self.end)
        env = c.environment()
        self.assertEqual(env['AWS_ACCESS_KEY_ID'],self.role['Credentials']['AccessKeyId'])
        self.assertEqual(env['AWS_MAX_ATTEMPTS'],'1')
        self.assertEqual(env['AWS_CONFIG_FILE'],'/dev/null')
        self.assertEqual(env['AWS_SHARED_CREDENTIALS_FILE'],'/dev/null')
        self.assertNotIn('AWS_PROFILE',env)
        for value in self.role['Credentials'].values():
            self.assertNotIn(value,repr(c))

    def test_wrong_role_account_and_session_are_rejected(self):
        for field,value in [('Account','000000000000'),('Arn',self.identity['Arn']+'wrong')]:
            identity=dict(self.identity);identity[field]=value
            with self.assertRaises(Blocked):iam.verify_role_identity(identity)
        for arn in [self.identity['Arn'].replace(live.ACCOUNT,'000000000000'),
                    self.identity['Arn'].replace(iam.ROLE_NAME,'other'),
                    self.identity['Arn'].replace(iam.SESSION_NAME,'other')]:
            response=copy.deepcopy(self.role);response['AssumedRoleUser']['Arn']=arn
            with self.assertRaises(Blocked):iam.role_credentials(response,self.now,self.end)

    def test_missing_or_invalid_secrets_and_safe_exceptions(self):
        for field in ('AccessKeyId','SecretAccessKey','SessionToken'):
            for value in (None,'','x\nsecret','x'*17000):
                response=copy.deepcopy(self.role);response['Credentials'][field]=value
                with self.assertRaises(Blocked) as error:
                    iam.role_credentials(response,self.now,self.end)
                self.assertNotIn('secret',str(error.exception))
        for value in (None,'','x\x00canary','x'*257):
            response=dict(self.db);response['dbPassword']=value
            with self.assertRaises(Blocked):iam.database_credentials(response,self.now,self.end)

    def test_principal_and_shape_are_exact(self):
        for user in ('odbcpp_pilot_admin','IAMR:other','',None):
            response=dict(self.db);response['dbUser']=user
            with self.assertRaises(Blocked):iam.database_credentials(response,self.now,self.end)
        response=dict(self.db);response['unexpected']='value'
        with self.assertRaises(Blocked):iam.database_credentials(response,self.now,self.end)

    def test_expiry_refusal_and_cleanup_margin(self):
        for expiry in (None,'bad','2026-10-02T19:15:00',
            (self.now-timedelta(seconds=1)).isoformat(),
            (self.end+timedelta(seconds=59)).isoformat(),
            (self.now+timedelta(seconds=961)).isoformat()):
            response=dict(self.db);response['expiration']=expiry
            with self.assertRaises(Blocked):iam.database_credentials(response,self.now,self.end)
        response=dict(self.db);response['expiration']=(self.end+timedelta(seconds=60)).isoformat()
        iam.database_credentials(response,self.now,self.end)
        c=iam.database_credentials(self.db,self.now,self.end)
        later=self.now+timedelta(seconds=700)
        with self.assertRaises(Blocked):iam.connection_string(self.config,c,later,later+timedelta(seconds=180))
        response=dict(self.db);response['nextRefreshTime']='bad'
        with self.assertRaises(Blocked):iam.database_credentials(response,self.now,self.end)

    def test_verified_endpoint_tls_and_credential_delimiters(self):
        response=dict(self.db);response['dbPassword']='synthetic;password}=!canary'
        c=iam.database_credentials(response,self.now,self.end)
        original=copy.deepcopy(self.config)
        text=iam.connection_string(self.config,c,self.now,self.end)
        self.assertIn('PWD={synthetic;password}}=!canary};',text)
        self.assertIn('UID={'+iam.DB_USER+'};',text)
        self.assertIn('SSL=1;SSLCAFILE=/etc/ssl/cert.pem;',text)
        self.assertEqual(self.config,original)
        self.assertNotIn(response['dbPassword'],repr(c))
        for field,value in [('host','other'),('port',5432),('database','other'),
                            ('ca_file',''),('test_user','other')]:
            config=dict(self.config);config[field]=value
            with self.assertRaises(Blocked):iam.connection_string(config,c,self.now,self.end)
        config=dict(self.config);config['SSL']=0
        with self.assertRaises(Blocked):iam.connection_string(config,c,self.now,self.end)

    def test_invalid_execution_bound(self):
        for end in (self.now,self.now+timedelta(seconds=181),self.end.replace(tzinfo=None)):
            with self.assertRaises(Blocked):iam.database_credentials(self.db,self.now,end)


if __name__ == '__main__':
    unittest.main()
