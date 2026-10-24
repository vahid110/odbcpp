"""Specific admitted Redshift resource lifecycle; fixed repository code, POSIX custody.

No credential reads/provider calls on import. AWS methods are extracted from
accepted NativeOps092, with explicit current resource/tool inputs and cap40.
"""
import datetime as dt
import ipaddress
import json
import hashlib
import math
import os
from pathlib import Path
import re
import stat
import time
from tools.development.redshift_ctest_lifecycle import need, record, BASE_ENV, Refused
from tools.redshift import native_auth_inputs
from tools.redshift.pilot_runtime import bounded_process


def validate_resource(resource):
    """The two existing pilot resources, never a general provisioning surface."""
    need(type(resource) is dict, 'resource_record')
    kind = resource.get('kind', 'provisioned')
    common = {'account', 'arn', 'region', 'profile'}
    if kind == 'provisioned':
        need(set(resource) in (common | {'cluster', 'created', 'node_type'},
                               common | {'kind', 'cluster', 'created', 'node_type'})
             and resource['cluster'] == 'odbcpp-redshift-provisioned-pilot',
             'unchanged_existing_resource_identity')
    elif kind == 'serverless':
        need(set(resource) == common | {'kind', 'workgroup', 'workgroup_id',
             'namespace', 'namespace_id', 'namespace_created', 'base_capacity',
             'max_capacity', 'endpoint', 'port', 'security_group_ids',
             'max_query_execution_time'}, 'serverless_resource_fields')
        need(resource['workgroup'] == resource['namespace'] == 'odbcpp-redshift-pilot'
             and resource['workgroup_id'] == 'd41cff82-7eb8-410d-be60-1d467c335756'
             and resource['namespace_id'] == 'd94a619c-bff3-42bb-8958-2405c99862db'
             and utc(resource['namespace_created']) == utc('2026-10-02T12:15:16.873Z')
             and resource['endpoint'] == 'odbcpp-redshift-pilot.385207570137.eu-north-1.redshift-serverless.amazonaws.com'
             and type(resource['port']) is int and resource['port'] == 5439
             and resource['security_group_ids'] == ['sg-06227a706295e3f05'],
             'existing_serverless_resource_identity')
        need(all(type(resource[key]) is int for key in
                 ('base_capacity', 'max_capacity', 'max_query_execution_time'))
             and resource['base_capacity'] == resource['max_capacity'] == 4
             and resource['max_query_execution_time'] == 30,
             'unchanged_serverless_caps')
    else:
        raise Refused('resource_kind')
    need(resource['account'] == '385207570137'
         and resource['arn'] == 'arn:aws:iam::385207570137:user/odbcpp-redshift-pilot'
         and resource['profile'] == 'odbcpp-redshift-pilot',
         'unchanged_existing_resource_identity')
    need(resource['region'] == 'eu-north-1', 'approved_region_eu_north_1')
    return kind


def utc(value):
    parsed = dt.datetime.fromisoformat(value.replace('Z', '+00:00'))
    need(parsed.tzinfo is not None, 'timezone_required')
    return parsed.astimezone(dt.timezone.utc)


def publication_pin(path):
    path = Path(path)
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'bytes': path.stat().st_size}


def private_json(path, limit=16384):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        before = os.fstat(fd)
        need(stat.S_ISREG(before.st_mode) and before.st_uid == os.geteuid()
             and before.st_nlink == 1 and stat.S_IMODE(before.st_mode) == 0o600
             and 0 < before.st_size <= limit, 'private_record')
        raw = os.read(fd, limit + 1)
        need(len(raw) == before.st_size and NativeOps.stamp(os.fstat(fd)) == NativeOps.stamp(before)
             and NativeOps.stamp(os.stat(path, follow_symlinks=False)) == NativeOps.stamp(before),
             'private_record_changed')
        return json.loads(raw)
    finally:
        os.close(fd)


class NativeOps:
    def __init__(self, approval, manifest, directory, child_deadline):
        self.a, self.m = approval, manifest
        self.directory, self.child_deadline = Path(directory), child_deadline
        self.resource = approval['resource']
        self.resource_kind = self.resource.get('kind', 'provisioned')
        need(self.resource_kind in ('provisioned', 'serverless'), 'resource_kind')
        if self.resource_kind == 'serverless':
            validate_resource(self.resource)
        self.snapshot = self.config = self.test_passfile = self.test_stamp = None
        self.sql_unknown = self.sql_fenced = False
        self.readers_known = True
        self.helpers = self.statements = self.aws_calls = 0
        self.process = bounded_process

    def now(self):
        return time.clock_gettime(time.CLOCK_MONOTONIC)

    def left(self, end, cap):
        seconds = min(cap, math.floor(end - self.now()))
        need(seconds >= 1, 'deadline')
        return seconds

    def owned_process(self, *args, **kwargs):
        try:
            return self.process(*args, **kwargs)
        except BaseException:
            self.readers_known = False
            raise

    @staticmethod
    def stamp(st):
        return (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns,
                st.st_mode, st.st_uid, st.st_nlink)

    def ordinary_held(self):
        need(self.test_passfile is not None and self.stamp(os.stat(
            self.test_passfile, follow_symlinks=False)) == self.test_stamp,
            'ordinary_private_input_changed')

    def prepare(self):
        self.config = private_json(self.a['database_config'])
        c = self.config
        need(set(c) == {'admin_password','admin_user','ca','database','host','port',
                        'test_password','test_user'} and c['database'] == 'odbcpp_pilot'
             and c['admin_user'] == 'odbcpp_pilot_admin'
             and c['test_user'] == 'odbcpp_pilot_test' and c['port'] == 5439,
             'config_identity')
        if self.resource_kind == 'serverless':
            need(c['host'] == self.resource['endpoint'], 'serverless_config_endpoint')
        import hashlib
        need(hashlib.sha256(Path(c['ca']).read_bytes()).hexdigest() == self.m['ca_sha256'],
             'CA_changed')
        self.snapshot = native_auth_inputs.prepare_snapshot(
            credentials_path=self.a['credential_source'], profile=self.resource['profile'],
            directory=str(self.directory/'inputs'),
            admin={k: c[k] for k in ('host','port','database','admin_user','admin_password')})
        self.aws_env = {'AWS_CONFIG_FILE': self.snapshot.config_path,
                        'AWS_SHARED_CREDENTIALS_FILE': self.snapshot.credentials_path}
        def escape(value):
            value = str(value)
            need(not any(x in value for x in ('\n','\r','\x00')), 'input_line_invalid')
            return value.replace('\\', '\\\\').replace(':', '\\:')
        line = ':'.join(escape(c[k]) for k in ('host','port','database','test_user','test_password'))+'\n'
        self.test_passfile = self.directory/'test.pgpass'
        fd = os.open(self.test_passfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'w') as stream:
            stream.write(line); stream.flush(); os.fsync(stream.fileno())
        st = os.stat(self.test_passfile, follow_symlinks=False)
        need(stat.S_ISREG(st.st_mode) and st.st_uid == os.geteuid()
             and st.st_nlink == 1 and stat.S_IMODE(st.st_mode) == 0o600,
             'ordinary_private_file')
        self.test_stamp = self.stamp(st)

    def aws(self, args, end):
        allowed = {('sts', 'get-caller-identity'), ('redshift', 'describe-clusters'), ('redshift', 'describe-cluster-parameters'), ('ec2', 'describe-security-groups'), ('redshift', 'resume-cluster'), ('redshift', 'pause-cluster')}
        if self.resource_kind == 'serverless':
            allowed = {('sts', 'get-caller-identity'), ('ec2', 'describe-security-groups'),
                       ('redshift-serverless', 'get-workgroup'),
                       ('redshift-serverless', 'get-namespace')}
        need(tuple(args[:2]) in allowed, 'operational_only')
        if self.resource_kind == 'serverless':
            commands = [
                ['sts', 'get-caller-identity'],
                ['redshift-serverless', 'get-workgroup', '--workgroup-name', self.resource['workgroup']],
                ['redshift-serverless', 'get-namespace', '--namespace-name', self.resource['namespace']],
                ['ec2', 'describe-security-groups', '--group-ids', *self.resource['security_group_ids']]]
            need(args in commands, 'exact_serverless_resource_read_only')
        self.aws_calls += 1
        need(self.aws_calls <= 64, 'AWS_count')
        env = {'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C', 'AWS_PAGER': '', 'AWS_EC2_METADATA_DISABLED': 'true', **self.aws_env}
        path = self.directory / ('aws-' + str(self.aws_calls) + '.log')
        r = self.owned_process([self.m['aws'], '--profile', self.resource['profile'], '--region', self.resource['region'], '--no-cli-pager', '--cli-connect-timeout', '5', '--cli-read-timeout', '5', *args, '--output', 'json'], env=env, seconds=self.left(end, 5), output=path)
        need(r.returncode == 0 and r.timed_out is False, 'AWS_return')
        self.left(end, 5)
        raw = path.read_bytes()
        need(len(raw) <= 4 * 1024 * 1024, 'AWS_bytes')
        return json.loads(raw)

    def cluster(self, end):
        need(self.resource_kind == 'provisioned', 'provisioned_only')
        rows = self.aws(['redshift', 'describe-clusters', '--cluster-identifier', self.resource['cluster']], end)['Clusters']
        need(len(rows) == 1, 'cluster_count')
        c = rows[0]
        need(c['ClusterIdentifier'] == self.resource['cluster'] and c['NodeType'] == self.resource['node_type'] and (c['NumberOfNodes'] == 1) and (c['Encrypted'] is True) and (c['Endpoint']['Address'] == self.config['host']) and (c['Endpoint']['Port'] == 5439) and (utc(c['ClusterCreateTime']) == utc(self.resource['created'])), 'cluster_identity')
        return c

    def serverless(self, end):
        """Read exact existing identities; AVAILABLE is not SQL/compute-idle proof."""
        need(self.resource_kind == 'serverless', 'serverless_only')
        r = self.resource
        w = self.aws(['redshift-serverless', 'get-workgroup',
                      '--workgroup-name', r['workgroup']], end)['workgroup']
        n = self.aws(['redshift-serverless', 'get-namespace',
                      '--namespace-name', r['namespace']], end)['namespace']
        prefix = 'arn:aws:redshift-serverless:' + r['region'] + ':' + r['account'] + ':'
        need(w['workgroupName'] == r['workgroup'] and w['workgroupId'] == r['workgroup_id']
             and w['workgroupArn'] == prefix + 'workgroup/' + r['workgroup_id']
             and w['namespaceName'] == r['namespace']
             and n['namespaceName'] == r['namespace'] and n['namespaceId'] == r['namespace_id']
             and n['namespaceArn'] == prefix + 'namespace/' + r['namespace_id']
             and utc(n['creationDate']) == utc(r['namespace_created'])
             and n['dbName'] == self.config['database'] == 'odbcpp_pilot'
             and n['adminUsername'] == self.config['admin_user'] == 'odbcpp_pilot_admin',
             'serverless_identity')
        need(w['status'] == n['status'] == 'AVAILABLE', 'serverless_not_available')
        need(type(w['baseCapacity']) is int and type(w['maxCapacity']) is int
             and w['baseCapacity'] == r['base_capacity']
             and w['maxCapacity'] == r['max_capacity'], 'serverless_caps_changed')
        need(w['endpoint']['address'] == r['endpoint'] == self.config['host']
             and type(w['endpoint']['port']) is int
             and w['endpoint']['port'] == r['port'] == self.config['port']
             and w['publiclyAccessible'] is True
             and sorted(w['securityGroupIds']) == r['security_group_ids'],
             'serverless_endpoint_network_identity')
        rows = w['configParameters']
        need(type(rows) is list and len({row['parameterKey'] for row in rows}) == len(rows),
             'serverless_configuration_unique')
        parameters = {row['parameterKey']: row['parameterValue'] for row in rows}
        need(parameters.get('require_ssl') == 'true', 'TLS_required')
        need(parameters.get('max_query_execution_time') == str(r['max_query_execution_time']),
             'serverless_query_cap_changed')
        self.left(end, 5)
        return w

    def security_available(self, end):
        self.identity(end)
        w = self.serverless(end)
        actual = self.aws(['ec2', 'describe-security-groups', '--group-ids',
                           *self.resource['security_group_ids']], end)['SecurityGroups']
        need(actual == self.m['security_groups'], 'SG_changed')
        need(sorted(row['GroupId'] for row in actual) == sorted(w['securityGroupIds']),
             'serverless_security_group_identity')
        self.network_guard(actual, end)
        self.left(end, 5)
        record(self.directory / 'serverless-security.json', {
            'workgroup': self.resource['workgroup'], 'namespace': self.resource['namespace'],
            'identity_TLS_network_verified': True, 'resource_status': 'AVAILABLE',
            'resource_mutations': 0, 'SQL_readiness_not_inferred': True,
            'physical_compute_idle_not_inferred': True})

    def verify_idle(self, end):
        """Resource-only readback; caller must separately prove owned SQL zero."""
        need(self.resource_kind == 'serverless', 'serverless_only')
        self.identity(end)
        self.serverless(end)
        actual = self.aws(['ec2', 'describe-security-groups', '--group-ids',
                           *self.resource['security_group_ids']], end)['SecurityGroups']
        need(actual == self.m['security_groups'], 'SG_changed')
        self.left(end, 5)
        record(self.directory / 'serverless-final-resource.json', {
            'resource_identity_caps_TLS_verified': True, 'resource_status': 'AVAILABLE',
            'paused_verified': False, 'physical_compute_idle_verified': False,
            'SQL_quiescence_verified_by_resource_metadata': False,
            'resource_mutations': 0})
        return True

    def identity(self, end):
        x = self.aws(['sts', 'get-caller-identity'], end)
        need(x['Account'] == self.resource['account'] and x['Arn'] == self.resource['arn'], 'principal')

    def network_guard(self, groups, end):
        address = None
        matched = False
        failure = None
        result = None
        started = self.now()
        cutoff = started + 5
        self.network_helpers = getattr(self, 'network_helpers', 0) + 1
        try:
            need(Path(self.m['python']).is_absolute() and re.fullmatch('[0-9a-f]{64}', self.m['pins'].get(self.m['python'], '')) is not None, 'network_pinned_python')
            need(end - started >= 5 and self.network_helpers == 1 and (self.helpers + self.network_helpers <= 40), 'network_five_second_reserve')
            code = "import sys,ssl,urllib.request\nclass NoRedirect(urllib.request.HTTPRedirectHandler):\n def redirect_request(self,*args,**kwargs): raise RuntimeError('egress_redirect')\nopener=urllib.request.build_opener(NoRedirect(),urllib.request.HTTPSHandler(context=ssl.create_default_context()))\nwith opener.open('https://checkip.amazonaws.com',timeout=3) as r: b=r.read(65)\nif not 0<len(b)<=64: raise RuntimeError('egress_bound')\nsys.stdout.buffer.write(b)\n"
            result = self.owned_process([self.m['python'], '-I', '-c', code], env={'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C'}, seconds=4, output=self.directory / 'network-checkip.log')
            need(type(result.returncode) is int and result.returncode == 0 and (result.timed_out is False) and (self.now() < cutoff), 'network_probe_failed_or_late')
            raw = (self.directory / 'network-checkip.log').read_bytes()
            need(0 < len(raw) <= 64, 'network_public_response_bound')
            address = ipaddress.IPv4Address(raw.decode('ascii').strip())
            need(address.is_global, 'network_public_IPv4_only')
            exact_rules = 0
            for group in groups:
                for rule in group['IpPermissions']:
                    protocol = rule.get('IpProtocol')
                    permits = protocol == '-1' or (protocol in ('tcp', '6') and rule.get('FromPort', 65536) <= 5439 <= rule.get('ToPort', -1))
                    if not permits:
                        continue
                    ranges = rule.get('IpRanges', [])
                    need(protocol in ('tcp', '6') and rule.get('FromPort') == 5439 and (rule.get('ToPort') == 5439) and (len(ranges) == 1) and (not rule.get('Ipv6Ranges')) and (not rule.get('UserIdGroupPairs')) and (not rule.get('PrefixListIds')) and (ranges[0].get('CidrIp') == str(address) + '/32'), 'current_egress_exact_single_5439_rule')
                    exact_rules += 1
            need(exact_rules == 1 and self.now() < cutoff, 'current_egress_exact_one_rule')
            matched = True
        except Exception as error:
            failure = str(error) if isinstance(error, Refused) else type(error).__name__
            raise Refused('network_guard_refused_' + failure) from None
        finally:
            record(self.directory / 'network-controls.json', {'state': 'MATCHED_BEFORE_RESUME' if matched else 'REFUSED_BEFORE_RESUME', 'endpoint': 'https://checkip.amazonaws.com', 'public_ipv4': None if address is None else str(address), 'required_cidr': None if address is None else str(address) + '/32', 'database_port': 5439, 'exact_single_ipv4_rule': matched, 'process_seconds': 4, 'whole_helper_cutoff': cutoff, 'original_availability_deadline': end, 'failure': failure, 'network_helper_count': self.network_helpers, 'process_returncode': None if result is None else result.returncode, 'process_timed_out': None if result is None else result.timed_out, 'known_returned_reaped': result is not None and type(result.returncode) is int and (type(result.timed_out) is bool), 'raw_public_reply': publication_pin(self.directory / 'network-checkip.log') if (self.directory / 'network-checkip.log').exists() else None, 'no_security_mutation': True, 'no_resume_from_probe': True, 'acceptance_requires': 'Guard returns before original whole-helper cutoff; receipt existence alone is not acceptance'})
        need(self.now() < cutoff, 'network_whole_helper_deadline')

    def security_paused(self):
        need(self.resource_kind == 'provisioned', 'serverless_has_no_paused_state')
        end = self.child_deadline - 900
        self.identity(end)
        c = self.cluster(end)
        need(c['ClusterStatus'] == 'paused', 'not_paused')
        groups = c['VpcSecurityGroups']
        ids = [r['VpcSecurityGroupId'] for r in groups]
        actual = self.aws(['ec2', 'describe-security-groups', '--group-ids', *ids], end)['SecurityGroups']
        need(actual == self.m['security_groups'], 'SG_changed')
        self.network_guard(actual, end)
        params = c['ClusterParameterGroups']
        need(len(params) == 1, 'TLS_groups')
        p = self.aws(['redshift', 'describe-cluster-parameters', '--parameter-group-name', params[0]['ParameterGroupName']], end)
        need(not p.get('Marker') and any((r.get('ParameterName') == 'require_ssl' and r.get('ParameterValue') == 'true' for r in p['Parameters'])), 'TLS_required')

    def resume_available(self, end):
        if self.resource_kind == 'serverless':
            self.serverless(end)
            record(self.directory / 'serverless-availability.json', {
                'resource_status': 'AVAILABLE', 'resource_mutations': 0,
                'SQL_readiness_not_inferred': True})
            return
        record(self.directory / 'resume-intent.json', {'cluster': self.resource['cluster']})
        self.aws(['redshift', 'resume-cluster', '--cluster-identifier', self.resource['cluster']], end)
        first_available = None
        while True:
            c = self.cluster(end)
            if c['ClusterStatus'] == 'available' and c.get('ClusterAvailabilityStatus') == 'Available':
                if first_available is None:
                    first_available = self.now()
                elif self.now() - first_available >= 10:
                    record(self.directory / 'stable-availability.json', {'first_available_monotonic': first_available, 'second_available_monotonic': self.now(), 'minimum_separation': 10, 'within_original_availability_deadline': end, 'cluster': self.resource['cluster'], 'source': 'two_exact_describe_readbacks', 'SQL_readiness_not_inferred': True})
                    return
            else:
                need(c['ClusterStatus'] in ('available', 'resuming'), 'resume_state')
                first_available = None
            time.sleep(self.left(end, 10))

    def pause_until(self, end):
        need(self.resource_kind == 'provisioned', 'serverless_has_no_paused_state')
        self.identity(end)
        paused = False
        while True:
            c = self.cluster(end)
            status = c['ClusterStatus']
            if status == 'paused':
                return True
            if status == 'available' and (not paused):
                record(self.directory / 'pause-intent.json', {'cluster': self.resource['cluster']})
                paused = True
                self.aws(['redshift', 'pause-cluster', '--cluster-identifier', self.resource['cluster']], end)
            else:
                need(status in ('available', 'resuming', 'pausing'), 'pause_state')
            time.sleep(self.left(end, 10))
