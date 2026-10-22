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
        need(tuple(args[:2]) in allowed, 'operational_only')
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
        rows = self.aws(['redshift', 'describe-clusters', '--cluster-identifier', self.resource['cluster']], end)['Clusters']
        need(len(rows) == 1, 'cluster_count')
        c = rows[0]
        need(c['ClusterIdentifier'] == self.resource['cluster'] and c['NodeType'] == self.resource['node_type'] and (c['NumberOfNodes'] == 1) and (c['Encrypted'] is True) and (c['Endpoint']['Address'] == self.config['host']) and (c['Endpoint']['Port'] == 5439) and (utc(c['ClusterCreateTime']) == utc(self.resource['created'])), 'cluster_identity')
        return c

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
