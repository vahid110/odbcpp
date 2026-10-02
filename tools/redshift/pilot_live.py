"""One-shot initial pilot setup. No retries, arbitrary SQL or recurring admission.

Requires the protected operator anchor and one canonical bootstrap reservation.
Unknown billing remains unknown; cleanup retains the entire reservation. Normal
subsequent test windows require independently reconciled billing and the general
qualified runner. All native output and configuration remain private.
"""
from datetime import datetime, timezone
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

from tools.redshift.pilot_preflight import Blocked, CANONICAL_DIRECTORY, validate_private_file, _unique_object
from tools.redshift.pilot_bootstrap import BootstrapAnchor, bootstrap_session
from tools.redshift.pilot_runtime import bounded_process, test_summary, verify_executable

AWS = '/opt/homebrew/bin/aws'
PSQL = '/opt/homebrew/bin/psql'
ACCOUNT = '385207570137'
REGION = 'eu-north-1'
NAME = 'odbcpp-redshift-pilot'
IAM_DB_USER = 'IAMR:odbcpp-redshift-test-runtime'
IDENTITY = ('RedshiftRealTest.VersionQuery',)
BASELINE = ('RedshiftRealTest.ConnectionTest', 'RedshiftRealTest.MultipleRowQuery',
            'RedshiftRealTest.PreparedScalarAndNull', 'RedshiftRealTest.ConfiguredFixtureMetadata',
            'RedshiftRealTest.ErrorHandling')


def private_json(path):
    validate_private_file(str(path))
    if path.stat().st_size > 1024 * 1024:
        raise Blocked('private_record_too_large')
    try:
        return json.loads(path.read_text(), object_pairs_hook=_unique_object,
                          parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
    except (ValueError, OSError):
        raise Blocked('private_record_unavailable') from None


def clean_env():
    # Fixed local profile discovery, no ambient service tokens or client options.
    return {'HOME': str(Path.home()), 'PATH': '/usr/bin:/bin',
            'LANG': 'C', 'LC_ALL': 'C'}


def aws(args):
    env = clean_env(); env['AWS_PAGER'] = ''
    try:
        result = subprocess.run([AWS, '--profile', NAME, '--region', REGION, '--no-cli-pager'] + args,
                                env=env, capture_output=True, timeout=25)
        if result.returncode:
            raise Blocked('aws_readback_failed')
        return json.loads(result.stdout)
    except (OSError, ValueError, subprocess.SubprocessError):
        raise Blocked('aws_readback_failed') from None


def validate_controls(identity, workgroup, namespace, usage, network, budget, anchor, config, admission, *, amended=False):
    if (anchor.account_id != ACCOUNT or anchor.region != REGION
            or identity.get('Account') != ACCOUNT or identity.get('Arn') != f'arn:aws:iam::{ACCOUNT}:user/{NAME}'):
        raise Blocked('aws_identity_mismatch')
    w, n = workgroup['workgroup'], namespace['namespace']
    params = {p['parameterKey']: p['parameterValue'] for p in w['configParameters']}
    earliest = datetime.fromisoformat(anchor.earliest_billable_start.replace('Z','+00:00'))
    if any(earliest > datetime.fromisoformat(x['creationDate'].replace('Z', '+00:00')) for x in (w,n)):
        raise Blocked('earliest_start_mismatch')
    if (w['workgroupId'] != anchor.workgroup_id or n['namespaceId'] != anchor.namespace_id
            or w['namespaceName'] != NAME or w['status'] != 'AVAILABLE'
            or w['baseCapacity'] != 4 or w['maxCapacity'] != 4
            or params.get('require_ssl') != 'true' or params.get('max_query_execution_time') != '30'
            or w['endpoint']['port'] != 5439 or w['endpoint']['address'] != config['host']):
        raise Blocked('workgroup_controls_mismatch')
    limits = usage['usageLimits']
    if (len(limits) != 1 or limits[0]['resourceArn'] != w['workgroupArn']
            or limits[0]['amount'] != (267 if amended else 20) or limits[0]['period'] != 'monthly'
            or limits[0]['usageType'] != 'serverless-compute' or limits[0]['breachAction'] != 'deactivate'):
        raise Blocked('usage_controls_mismatch')
    groups = network['SecurityGroups']
    if len(groups) != 1 or w['securityGroupIds'] != [groups[0]['GroupId']]:
        raise Blocked('network_controls_mismatch')
    rules = groups[0]['IpPermissions']
    if (len(rules) != 1 or rules[0]['IpProtocol'] != 'tcp' or rules[0]['FromPort'] != 5439
            or rules[0]['ToPort'] != 5439 or len(rules[0]['IpRanges']) != 1
            or rules[0]['IpRanges'][0]['CidrIp'] != admission['allowed_ipv4_cidr']
            or groups[0]['OwnerId'] != ACCOUNT or groups[0]['VpcId'] != admission['vpc_id']
            or groups[0]['GroupId'] != admission['security_group_id']
            or rules[0].get('Ipv6Ranges') or rules[0].get('UserIdGroupPairs') or rules[0].get('PrefixListIds')):
        raise Blocked('network_controls_mismatch')
    b = budget['Budget']
    if (Decimal(b['BudgetLimit']['Amount']) != (150 if amended else 15) or b['BudgetLimit']['Unit'] != 'USD'
            or not b['TimePeriod']['Start'].startswith('2026-10-02')
            or not b['TimePeriod']['End'].startswith('2026-10-12')):
        raise Blocked('budget_controls_mismatch')


def validate_admission(a, anchor, now, *, amended=False):
    required = ('earliest_start_verified', 'no_other_billable_activity', 'other_tax_bound_verified')
    keys = {'account_id','region','workgroup_id','namespace_id','earliest_billable_start',
            'observed_at','usd_per_rpu_hour','other_tax_usd','price_source','initial_inventory_basis',
            'other_tax_basis','vpc_id','security_group_id','allowed_ipv4_cidr',*required}
    if not isinstance(a,dict) or set(a) != keys:
        raise Blocked('admission_provenance_mismatch')
    if (a.get('account_id') != anchor.account_id or a.get('region') != anchor.region
            or a.get('workgroup_id') != anchor.workgroup_id or a.get('namespace_id') != anchor.namespace_id
            or a.get('earliest_billable_start') != anchor.earliest_billable_start
            or any(a.get(k) is not True for k in required)
            or a.get('usd_per_rpu_hour') != '.374' or a.get('other_tax_usd') != ('50' if amended else '5')
            or a.get('price_source') != 'https://pricing.us-east-1.amazonaws.com/offers/v1.0/aws/AmazonRedshift/current/eu-north-1/index.json'
            or not all(isinstance(a.get(k), str) and a[k] for k in ('initial_inventory_basis','other_tax_basis','vpc_id','security_group_id'))
            or not re.fullmatch(r'(?:[0-9]{1,3}\.){3}[0-9]{1,3}/32', a.get('allowed_ipv4_cidr',''))):
        raise Blocked('admission_provenance_mismatch')
    try:
        observed = datetime.fromisoformat(a['observed_at'].replace('Z','+00:00'))
        age = (now-observed).total_seconds()
    except (KeyError, ValueError, TypeError):
        raise Blocked('admission_provenance_mismatch') from None
    if not 0 <= age <= 300:
        raise Blocked('admission_provenance_stale')


def validate_config(c):
    expected = {'port': 5439, 'database': 'odbcpp_pilot', 'admin_user': 'odbcpp_pilot_admin',
                'test_user': 'odbcpp_pilot_test', 'schema': 'odbcpp_fixture', 'table': 'pilot_rows',
                'ca_file': '/etc/ssl/cert.pem'}
    if (not isinstance(c,dict) or set(c) != set(expected)|{'host','admin_password','test_password'}
            or any(c.get(k) != v for k, v in expected.items())):
        raise Blocked('database_config_mismatch')
    if not re.fullmatch(r'odbcpp-redshift-pilot\.385207570137\.eu-north-1\.redshift-serverless\.amazonaws\.com', c.get('host', '')):
        raise Blocked('database_config_mismatch')
    admin = c.get('admin_password')
    if not isinstance(admin, str) or not 20 <= len(admin) <= 128 or any(ord(x)<33 or ord(x)>126 for x in admin):
        raise Blocked('database_config_mismatch')
    # This password is also used as a SQL literal and connection-string value.
    if not isinstance(c.get('test_password'), str) or not re.fullmatch(r'[A-Za-z0-9!]{20,128}', c['test_password']):
        raise Blocked('database_config_mismatch')


class Window:
    def __init__(self, directory, config, deadline, *, cleanup_principal=None):
        if cleanup_principal not in (None, IAM_DB_USER):
            raise Blocked('iam_db_principal_mismatch')
        self.cleanup_principal = cleanup_principal
        self.directory, self.config, self.deadline = directory, config, deadline
        self.seq = 0

    def seconds(self, limit):
        available = int(self.deadline - time.monotonic())
        if available < 1:
            raise Blocked('execution_deadline')
        return min(limit, available)

    def output(self, name):
        self.seq += 1
        return self.directory/f'bootstrap-{self.seq:02d}-{name}.log'

    def sql(self, text, *, admin=True, limit=25, statement_timeout_ms=15000):
        if statement_timeout_ms not in (15000,30000):
            raise Blocked('invalid_sql_timeout')
        c = self.config; env = clean_env()
        env.update(PGHOST=c['host'], PGPORT=str(c['port']), PGDATABASE=c['database'],
                   PGUSER=c['admin_user'] if admin else c['test_user'],
                   PGPASSWORD=c['admin_password'] if admin else c['test_password'],
                   PGSSLMODE='verify-full', PGSSLROOTCERT=c['ca_file'], PGCONNECT_TIMEOUT='10',
                   PGAPPNAME='odbcpp-pilot-setup')
        output = self.output('sql')
        result = bounded_process([PSQL, '-X', '-qAt', '-v', 'ON_ERROR_STOP=1'], env=env,
            seconds=self.seconds(limit), output=output,
            input_bytes=(f'SET statement_timeout = {statement_timeout_ms};\n'+text+'\n').encode())
        if result.timed_out or result.returncode:
            raise Blocked('sql_step_failed')
        return output.read_text().strip()

    def driver(self, manifest, cases, *, credentials=None, execution_end=None):
        c = self.config; executable = Path(manifest['path'])
        verify_executable(executable, manifest['sha256'])
        env = clean_env()
        env['ODBCPP_REDSHIFT_TEST_CONNECTION'] = (f"SERVER={c['host']};PORT=5439;DATABASE={c['database']};"
            f"UID={c['test_user']};PWD={c['test_password']};SSL=1;SSLCAFILE={c['ca_file']};")
        env['ODBCPP_REDSHIFT_TEST_SCHEMA'] = c['schema']; env['ODBCPP_REDSHIFT_TEST_TABLE'] = c['table']
        if credentials is not None:
            from dataclasses import replace
            from tools.redshift import pilot_iam_credentials as iam
            allowed = (IDENTITY, ('RedshiftRealTest.IAMPrincipalScalar',
                                 'RedshiftRealTest.IAMInvalidPassword'))
            if self.cleanup_principal != IAM_DB_USER or cases not in allowed:
                raise Blocked('iam_inventory_mismatch')
            env['ODBCPP_REDSHIFT_TEST_CONNECTION'] = iam.connection_string(
                c, credentials, datetime.now(timezone.utc), execution_end)
            invalid = replace(credentials, password='ODBCPP_IAM_NEGATIVE_INVALID_PASSWORD')
            env['ODBCPP_REDSHIFT_IAM_INVALID_CONNECTION'] = iam.connection_string(
                c, invalid, datetime.now(timezone.utc), execution_end)

        output = self.output('driver'); report = output.with_suffix('.xml')
        fd = os.open(report, os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW, 0o600); os.close(fd)
        result = bounded_process([str(executable), '--gtest_filter='+':'.join(cases),
            '--gtest_repeat=1', '--gtest_output=xml:'+str(report)], env=env,
            seconds=self.seconds(90), output=output)
        summary = test_summary(report, cases)
        self.last_driver_summary = summary
        if result.timed_out or result.returncode or summary['failed']:
            raise Blocked('driver_baseline_failed')
        return summary

    def prepare_fixture(self):
        password = self.config['test_password']
        # Preserve an existing pilot user after partial setup. Only the owned
        # fixture table is recreated; no unrelated schemas/tables are touched.
        self.sql("SELECT COUNT(*) AS pilot_user_exists FROM pg_user "
            "WHERE usename='odbcpp_pilot_test'\n\\gset\n"
            "\\if :pilot_user_exists\n\\else\n"
            f"CREATE USER odbcpp_pilot_test PASSWORD '{password}' NOCREATEDB NOCREATEUSER CONNECTION LIMIT 2;\n"
            "\\endif\n"
            "ALTER USER odbcpp_pilot_test NOCREATEDB;\n"
            "ALTER USER odbcpp_pilot_test NOCREATEUSER;\n"
            "ALTER USER odbcpp_pilot_test CONNECTION LIMIT 2;\n"
            "ALTER USER odbcpp_pilot_test SESSION TIMEOUT 60;\n"
            "ALTER USER odbcpp_pilot_test SET statement_timeout TO 15000;\n"
            "ALTER USER odbcpp_pilot_admin SESSION TIMEOUT 60;\n"
            "ALTER USER odbcpp_pilot_admin SET statement_timeout TO 15000;\n"
            "CREATE SCHEMA IF NOT EXISTS odbcpp_fixture;\n"
            "REVOKE CREATE ON SCHEMA public FROM PUBLIC;\n"
            "DROP TABLE IF EXISTS odbcpp_fixture.pilot_rows;\n"
            "CREATE TABLE odbcpp_fixture.pilot_rows (id INTEGER, value VARCHAR(32));\n"
            "INSERT INTO odbcpp_fixture.pilot_rows VALUES (1,'one'),(2,'two');\n"
            "GRANT USAGE ON SCHEMA odbcpp_fixture TO odbcpp_pilot_test;\n"
            "GRANT SELECT ON odbcpp_fixture.pilot_rows TO odbcpp_pilot_test;",
            limit=90,statement_timeout_ms=30000)

    def cleanup(self):
        # Serverless uses SYS views. Resolve user/PID values on the leader,
        # and execute each terminate separately instead of over a system scan.
        bind = ("SELECT pg_backend_pid() AS own_pid, "
            "COALESCE(MAX(CASE WHEN usename='odbcpp_pilot_admin' THEN usesysid END),-1) AS admin_id, "
            "COALESCE(MAX(CASE WHEN usename='odbcpp_pilot_test' THEN usesysid END),-1) AS test_id "
            "FROM pg_user\n\\gset\n")
        ids = ':admin_id,:test_id'
        if self.cleanup_principal is not None:
            bind = bind.replace(" AS test_id ", " AS test_id, "
                "COALESCE(MAX(CASE WHEN usename='" + IAM_DB_USER +
                "' THEN usesysid END),-1) AS iam_id ")
            ids += ',:iam_id'
        self.sql(bind + "SELECT 'SELECT pg_terminate_backend(' || session_id || ');' "
            "FROM sys_session_history WHERE database_name='odbcpp_pilot' AND status='active' "
            f"AND CAST(user_id AS INTEGER) IN ({ids}) AND session_id<>:own_pid\n\\gexec")
        # Separate EXISTS checks can stop at the first active row; COUNT scans
        # historical views to completion. Both explicit false results are needed.
        sessions = self.sql(bind + "SELECT EXISTS(SELECT 1 FROM sys_session_history "
            "WHERE database_name='odbcpp_pilot' AND status='active' "
            f"AND CAST(user_id AS INTEGER) IN ({ids}) AND session_id<>:own_pid);",
            limit=20,statement_timeout_ms=15000)
        if sessions != 'f':
            raise Blocked('remote_cleanup_unverified')
        queries = self.sql(bind + "SELECT EXISTS(SELECT 1 FROM sys_query_history "
            "WHERE database_name='odbcpp_pilot' "
            "AND status IN ('planning','queued','running','returning') "
            f"AND user_id IN ({ids}) AND session_id<>:own_pid);",
            limit=20,statement_timeout_ms=15000)
        if queries != 'f':
            raise Blocked('remote_cleanup_unverified')


def execute(directory=CANONICAL_DIRECTORY, *, qualification=False):
    directory = Path(directory)
    original_anchor = BootstrapAnchor(**private_json(directory/'bootstrap-anchor.json'))
    anchor = (BootstrapAnchor(**private_json(directory/'qualification-anchor.json'))
              if qualification else original_anchor)
    config = private_json(directory/'database-config.json'); validate_config(config)
    admission = private_json(directory/('qualification-admission.json' if qualification else 'bootstrap-admission.json'))
    validate_admission(admission,anchor,datetime.now(timezone.utc))
    manifest = private_json(directory/'executable-admission.json')
    if set(manifest) != {'path','sha256','identity_case','baseline_cases'} or manifest.get('identity_case') != IDENTITY[0] or tuple(manifest.get('baseline_cases', [])) != BASELINE:
        raise Blocked('test_inventory_mismatch')
    verify_executable(Path(manifest['path']), manifest['sha256'])
    report = {'status': 'blocked', 'reason': None, 'cleanup_verified': False,
              'actual_spend_usd': None, 'remaining_allowance_usd': None, 'results': [],
              'execution_failure': None,
              'started_at': datetime.now(timezone.utc).isoformat(), 'finished_at': None}
    from tools.redshift.pilot_cumulative import currentperiod_session
    session = (currentperiod_session(directory/'setup.json',original_anchor) if qualification
               else bootstrap_session(directory/'setup.json',anchor))
    with session as ledger:
        identity = aws(['sts','get-caller-identity'])
        workgroup = aws(['redshift-serverless','get-workgroup','--workgroup-name',NAME])
        namespace = aws(['redshift-serverless','get-namespace','--namespace-name',NAME])
        usage = aws(['redshift-serverless','list-usage-limits','--resource-arn',workgroup['workgroup']['workgroupArn']])
        groups = workgroup['workgroup']['securityGroupIds']
        network = aws(['ec2','describe-security-groups','--group-ids']+groups)
        budget = aws(['budgets','describe-budget','--account-id',ACCOUNT,'--budget-name',NAME])
        validate_controls(identity,workgroup,namespace,usage,network,budget,anchor,config,admission)
        inventory_w = aws(['redshift-serverless','list-workgroups'])['workgroups']
        inventory_n = aws(['redshift-serverless','list-namespaces'])['namespaces']
        if ([x['workgroupId'] for x in inventory_w] != [anchor.workgroup_id]
                or [x['namespaceId'] for x in inventory_n] != [anchor.namespace_id]):
            raise Blocked('additional_redshift_resources')
        now = datetime.now(timezone.utc)
        anchor.record(now)
        validate_admission(admission,anchor,now)
        if qualification:
            state=ledger._load(); overlay=state['bootstrap_overlay']
            if overlay['schema_version']==2 and overlay['attempts']:
                raise Blocked('qualification_already_attempted')
            historical = overlay if overlay['schema_version']==1 else overlay['original_bootstrap']
            prior = historical['window']
            proof = private_json(directory/'bounded-safety-cleanup-result.json')
            if prior['phase']!='cleaned_pending_billing' or proof.get('status')!='cleanup_verified':
                raise Blocked('original_cleanup_required')
            cleanup_evidence=dict(verified=True,observed_at=prior['cleanup_at'],
                anchor=original_anchor.record(now),no_active_queries=True,
                no_active_sessions=True,transactions_closed=True)
            if overlay['schema_version']==1:
                migrated=ledger.migrate(cleanup_evidence)
                if migrated['status']!='pass':raise Blocked(migrated['reasons'][0])
            evidence=dict(verified=True,observed_at=admission['observed_at'],anchor=anchor.record(now),
                base_rpus=4,max_rpus=4,usd_per_rpu_hour=admission['usd_per_rpu_hour'],
                no_additional_resources=True,no_other_billable_activity=admission['no_other_billable_activity'],
                controls_verified=True,network_verified=True,other_tax_usd=admission['other_tax_usd'],
                additional_metering_seconds=60,additional_cleanup_seconds=60,cleanup_evidence=cleanup_evidence)
            reserved=ledger.reserve(evidence,anchor)
            if reserved['status']!='pass':raise Blocked(reserved['reasons'][0])
        else:
            initialized = ledger.initialize()
            if initialized['status'] != 'pass':
                raise Blocked(initialized['reasons'][0])
            # Operator's anchor carries independently reviewed initial resource absence,
            # creation timestamp and regional price provenance. This is one setup only.
            evidence = dict(verified=True, observed_at=admission['observed_at'],
                anchor=anchor.record(now), base_rpus=4,max_rpus=4,usd_per_rpu_hour=admission['usd_per_rpu_hour'],
                cleanup_bound_seconds=60,**{k:admission[k] for k in
                    ('earliest_start_verified','no_other_billable_activity','other_tax_bound_verified')})
            reserved = ledger.reserve(evidence,metering_seconds=60,cleanup_seconds=60,other_tax_usd=admission['other_tax_usd'])
            if reserved['status'] != 'pass':
                raise Blocked(reserved['reasons'][0])
            active = ledger.transition('active')
            if active['status'] != 'pass':
                raise Blocked(active['reasons'][0])
        remaining = (datetime.fromisoformat(anchor.hard_deadline.replace('Z','+00:00'))-datetime.now(timezone.utc)).total_seconds()
        if remaining < 1:
            ledger.transition('uncertain')
            raise Blocked('execution_deadline')
        window = Window(directory,config,time.monotonic()+min(180,remaining))
        if qualification:window.seq=100
        try:
            if qualification:
                window.cleanup()
                observed=datetime.now(timezone.utc)
                active=ledger.transition('active',dict(verified=True,observed_at=observed.isoformat().replace('+00:00','Z'),
                    anchor=anchor.record(observed),no_active_queries=True,no_active_sessions=True,transactions_closed=True))
                if active['status']!='pass':raise Blocked(active['reasons'][0])
            if window.sql("SELECT CASE WHEN POSITION('redshift' IN LOWER(version()))>0 THEN 1 ELSE 0 END;") != '1':
                raise Blocked('server_identity_mismatch')
            window.prepare_fixture()
            if window.sql("SHOW statement_timeout;",admin=False) != '15000':
                raise Blocked('server_timeout_unverified')
            report['results'].append(window.driver(manifest,IDENTITY))
            report['results'].append(window.driver(manifest,BASELINE))
            report['status'] = 'passed'
        except Blocked as error:
            report['reason'] = error.code
            report['execution_failure'] = error.code
            ledger.transition('uncertain')
        except BaseException:
            report['reason'] = 'execution_interrupted'
            report['execution_failure'] = 'execution_interrupted'
            ledger.transition('uncertain')
        finally:
            # Cleanup has its separately reserved headroom, not a renewed query window.
            window.deadline = time.monotonic()+60
            try:
                window.cleanup()
                observed = datetime.now(timezone.utc).isoformat().replace('+00:00','Z')
                outcome = ledger.transition('cleaned_pending_billing',dict(verified=True,
                    observed_at=observed,anchor=anchor.record(datetime.now(timezone.utc)),
                    no_active_queries=True,no_active_sessions=True,transactions_closed=True))
                if outcome['status'] != 'pass':
                    raise Blocked('cleanup_persistence_failed')
                report['cleanup_verified'] = True
            except BaseException:
                report['status']='blocked';report['reason']='remote_cleanup_unverified'
                ledger.transition('uncertain')
        path = directory/('qualification-result.json' if qualification else 'bootstrap-result.json')
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
        with os.fdopen(fd,'w') as f:
            json.dump(report,f,indent=2);f.flush();os.fsync(f.fileno())
        return report


if __name__ == '__main__':
    try:
        result = execute()
        print(json.dumps(result))
        if result['status'] != 'passed':
            raise SystemExit(1)
    except Blocked as error:
        print(json.dumps({'status':'blocked','reason':error.code}))
        raise SystemExit(1)
