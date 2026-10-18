"""ROOT-injected operational adapter for one local CTest collection.

Construction loads pinned public Python sources only. Private acquisition and
resource effects start solely when the reviewed lifecycle calls open_inputs.
The accepted fixture092 NativeOps implementation remains immutable.
"""
import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import time
from types import SimpleNamespace

from tools.development import redshift_ctest_inputs as inputs
from tools.development import redshift_ctest_fixtures as fixtures
from tools.development import redshift_ctest_native_custody as native_custody
from tools.development.redshift_ctest_lifecycle import BASE_ENV, Refused, need, record, verify_public_pin
from tools.redshift.pilot_preflight import pilot_lock
from tools.redshift.pilot_runtime import _stop_group

DATABASE = 'odbcpp_pilot'
ADMIN = 'odbcpp_pilot_admin'
ORDINARY = 'odbcpp_pilot_test'
OWNED = "user_name IN ('odbcpp_pilot_admin','odbcpp_pilot_test') AND db_name='odbcpp_pilot'"
PIDS = ('SELECT process,TRIM(user_name),TRIM(db_name),starttime::varchar FROM stv_sessions WHERE ' +
        OWNED + ' AND process<>pg_backend_pid() LIMIT 9;')
ZERO = "SELECT EXISTS(SELECT 1 FROM stv_sessions WHERE " + OWNED + \
       " AND process<>pg_backend_pid()) AS sessions,EXISTS(SELECT 1 FROM stv_recents WHERE " + OWNED + \
       " AND status<>'Done' AND pid<>pg_backend_pid()) AS queries;"
DB_QUERIES = {
    'db_probe': ("SELECT has_database_privilege('odbcpp_pilot_test','odbcpp_pilot','CREATE');", 1),
    'db_grant': ('GRANT CREATE ON DATABASE odbcpp_pilot TO odbcpp_pilot_test;\n'
                 "SELECT 'DBCREATE_GRANT_ACK_V1';", 2),
    'db_revoke': ('REVOKE CREATE ON DATABASE odbcpp_pilot FROM odbcpp_pilot_test;\n'
                  "SELECT 'DBCREATE_REVOKE_ACK_V1';", 2),
}


def load_public(path, digest, name):
    verify_public_pin(path, digest)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def pin(path):
    path = Path(path)
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'bytes': path.stat().st_size}


def combined_fixture_plan():
    """Pure complete reserve, callable before Operations or private inputs."""
    plan = fixtures.plan()
    census = native_custody.plan()
    for key, extra in (('total_helpers', 'helpers'), ('total_SQL', 'SQL'),
                       ('total_logins', 'logins'), ('total_inclusive', 'inclusive')):
        plan[key] += census[extra]
    plan['total_SQL'] += 8
    plan['total_inclusive'] += 8
    plan['max8_termination_scope_guards_SQL'] = 8
    plan['native_census'] = census
    plan['network_helpers_separate'] = 1
    return plan


def termination_stage(targets):
    """Guard each captured session tuple and exclude SELF before termination.

    STV_SESSIONS.starttime is the actual connection birth timestamp. Every
    guard is a separate statement from pg_terminate_backend, preserving the
    existing Redshift leader/compute query separation.
    """
    need(type(targets) is tuple and 1 <= len(targets) <= 8, 'termination_targets')
    queries = []
    for pid, principal, database, birth in targets:
        need(type(pid) is int and 1 <= pid <= 2147483647 and
             principal in (ADMIN, ORDINARY) and database == DATABASE and
             re.fullmatch(r'[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}(?:\.[0-9]{1,6})?', birth)
             is not None, 'captured_session_tuple')
        predicate = ("process=" + str(pid) + " AND user_name='" + principal +
                     "' AND db_name='odbcpp_pilot' AND starttime=TIMESTAMP '" + birth +
                     "' AND process<>pg_backend_pid()")
        queries.extend(['SELECT EXISTS(SELECT 1 FROM stv_sessions WHERE ' + predicate +
                        ') AS still_owned\n\\gset\n\\if :still_owned',
                        'SELECT ' + str(pid) + ',pg_terminate_backend(' + str(pid) + ');',
                        '\\else', "SELECT " + str(pid) + ",'SKIPPED';", '\\endif'])
    need(len({row[0] for row in targets}) == len(targets), 'duplicate_captured_PID')
    return {'query': '\n'.join(queries), 'statements': 2 * len(targets),
            'role': 'admin', 'stage': 'principal_terminate'}


class Operations:
    def __init__(self, *, native_source, native_sha256, approval, preparation_manifest,
                 profile_manifest, factory_source, factory_sha256, launcher_source,
                 launcher_sha256, profiles, specs, blueprint, authority_receipts):
        self.timeout_factory = load_public(native_source, native_sha256, '_ctest_native092')
        self.factory = load_public(factory_source, factory_sha256, '_ctest_factory074')
        self.launcher = load_public(launcher_source, launcher_sha256, '_ctest_inputs089')
        self.approval = copy.deepcopy(approval)
        self.preparation_manifest = copy.deepcopy(preparation_manifest)
        self.profile_manifest = copy.deepcopy(profile_manifest)
        self.profiles = copy.deepcopy(profiles)
        self.specs, self.blueprint = copy.deepcopy(specs), copy.deepcopy(blueprint)
        self.authority_receipts = copy.deepcopy(authority_receipts)
        self.directory = Path(approval['run_directory'])
        self.native = None
        self.profile_inputs = None
        self.input_residue = None
        self.env_custody = []
        self.env_directory = None
        self.generated = []
        self.processes = []
        self.helpers = self.statements = self.logins = 0
        self.sql_unknown = self.sql_fenced = False
        self.readers_known = True
        self.principals_quiescent = False
        self.fixture_state = None
        self.events = 0
        self.proofs = {}
        self.selected_other_database = None
        self.native_custody_state = None
        self.native_custody_result = None
        self.keep_cluster_running = approval.get('keep_cluster_running') is True
        self.available_cluster_verified = False
        self.security_status = None
        self.available_cluster_identity = None
        self.executables = {str(Path('/opt/homebrew/bin/' + tool).resolve(strict=True))
                            for tool in ('cmake', 'ctest')}

    def combined_fixture_plan(self):
        return combined_fixture_plan()

    def now(self):
        return time.clock_gettime(time.CLOCK_MONOTONIC)

    def lock(self, directory):
        return pilot_lock(directory)

    def verify_source(self, directory, head):
        result = subprocess.run(['/usr/bin/git', '-C', str(directory), 'rev-parse', 'HEAD'],
                                env=BASE_ENV, stdin=subprocess.DEVNULL, capture_output=True,
                                timeout=5, check=False)
        need(result.returncode == 0 and result.stdout.decode('ascii').strip() == head,
             'local_source_head_changed')

    def open_inputs(self, directory, child_end):
        need(Path(directory) == self.directory and self.native is None, 'oneuse_native_inputs')
        self.native = self.timeout_factory.NativeOps(self.approval, self.preparation_manifest,
                                                    directory, child_end)
        try:
            self.native.prepare()
            self.profile_inputs = self.launcher.prepare_profile_inputs(
                self.directory / 'profiles', self.native.config,
                inputs.LoggingFactory(self.factory), self.profiles,
                self.profile_manifest, deadline=child_end)
        except BaseException as error:
            self.profile_inputs = getattr(error, 'state', None) or self.profile_inputs
            self.input_residue = getattr(error, 'residue', None)
            raise

    def security_paused(self, end):
        need(self.now() < end, 'security_deadline')
        continuation = self.approval.get('continue_available_cluster_from')
        if continuation is None:
            self.native.security_paused()
            self.security_status = 'paused'
        else:
            need(type(continuation) is dict and set(continuation) == {'path', 'sha256'},
                 'available_continuation_pin')
            verify_public_pin(continuation['path'], continuation['sha256'])
            terminal = json.loads(Path(continuation['path']).read_text())
            need(terminal.get('cleanup_complete') is True and
                 terminal.get('readers_known') is True and
                 terminal.get('private_inputs_closed') is True and
                 terminal.get('SQL_unknown') is False, 'available_recovery_not_closed')
            self.native.identity(end)
            cluster = self._available_cluster(end)
            ids = [row['VpcSecurityGroupId'] for row in cluster['VpcSecurityGroups']]
            actual = self.native.aws(['ec2', 'describe-security-groups', '--group-ids', *ids],
                                     end)['SecurityGroups']
            need(actual == self.preparation_manifest['security_groups'], 'SG_changed')
            self.native.network_guard(actual, end)
            params = cluster['ClusterParameterGroups']
            need(len(params) == 1, 'TLS_groups')
            settings = self.native.aws(['redshift', 'describe-cluster-parameters',
                '--parameter-group-name', params[0]['ParameterGroupName']], end)
            need(not settings.get('Marker') and any(
                row.get('ParameterName') == 'require_ssl' and row.get('ParameterValue') == 'true'
                for row in settings['Parameters']), 'TLS_required')
            need(self.now() < end, 'security_deadline')
            self.security_status = cluster['ClusterStatus']
            self.available_cluster_identity = {
                key: copy.deepcopy(cluster[key]) for key in
                ('ClusterIdentifier', 'ClusterCreateTime', 'Endpoint',
                 'VpcSecurityGroups', 'ClusterParameterGroups')}
            self.available_cluster_verified = True
            record(self.directory / 'available-cluster-security.json',
                   {'cluster_status': self.security_status, 'recovery_terminal': continuation,
                    'identity_TLS_network_verified': True, 'resume_skipped': True})
        need(self.now() < end, 'security_deadline')

    def _available_cluster(self, end):
        need(self.now() < end, 'security_deadline')
        cluster = self.native.cluster(end)  # Native owns exact resource/endpoint identity.
        while cluster['ClusterStatus'] == 'resuming' and self.now() < end - 10:
            time.sleep(5)
            cluster = self.native.cluster(end)
        need(cluster['ClusterStatus'] == 'available' and
             cluster.get('ClusterAvailabilityStatus') == 'Available', 'not_available')
        return cluster

    def resume_available(self, end):
        if self.approval.get('continue_available_cluster_from') is not None:
            need(self.available_cluster_verified, 'available_security_not_verified')
            cluster = self._available_cluster(end)
            need(all(cluster[key] == value for key, value in
                     self.available_cluster_identity.items()), 'available_cluster_changed')
        else:
            self.native.resume_available(end)
        self.final_zero(end)

    def record(self, event):
        self.events += 1
        record(self.directory / ('fixture-event-' + str(self.events) + '.json'), event)

    def _closed_stage(self, stage):
        state = self.fixture_state
        need(type(stage) is dict and state is not None, 'fixture_stage_without_state')
        name = stage['stage']
        if name == 'native_persistent_census':
            expected = native_custody.stage()
        elif name in ('inspect', 'create', 'ordinary', 'cleanup'):
            records = copy.deepcopy(state['records'])
            for key in stage['keys']:
                if name == 'create' and records[key]['state'] == 'CREATE_UNKNOWN':
                    records[key]['state'] = 'MISSING'
                if name == 'cleanup' and records[key]['state'] == 'DROP_UNKNOWN':
                    records[key]['state'] = 'OWNED'
            expected = self.factory.whole_batch_stage(state['plan'], stage['keys'], name, records)
        elif name == 'timeout_cleanup':
            expected = self.factory.timeout_stage(state['timeout']['record'], name)
        elif name.startswith('timeout_'):
            key = name[len('timeout_'):]
            query, headers, maximum, inclusive = self.timeout_factory.query_set(
                state['timeout']['table'], state['timeout']['facts'])[key]
            expected = {'query': query, 'headers': headers, 'maxrows': maximum,
                        'statements': inclusive - 3,
                        'role': 'admin' if key in self.timeout_factory.ADMIN_KEYS else 'test',
                        'keys': ('TIMEOUT_GATE',), 'stage': name}
        elif name in DB_QUERIES:
            query, count = DB_QUERIES[name]
            expected = {'query': query, 'statements': count, 'role': 'admin',
                        'keys': ('DATABASE_CREATE',), 'stage': name}
        elif name == 'two_db_databases':
            expected = self.factory.two_db_stage('databases')
        elif name == 'two_db_schemas':
            need(stage.get('other') == self.selected_other_database, 'actual_second_database')
            expected = self.factory.two_db_stage('schemas', self.selected_other_database)
        else:
            raise Refused('nonallowlisted_fixture_stage')
        need(stage == expected, 'closed_fixture_SQL_changed')

    def sql(self, stage, deadline):
        self._closed_stage(stage)
        raw, _ = self._sql(stage, deadline)
        if stage['stage'] == 'two_db_databases':
            self.selected_other_database = self.factory.two_db_select(raw)
        return raw

    def _sql(self, stage, deadline):
        need(self.native is not None and self.readers_known and
             not self.sql_unknown and not self.sql_fenced, 'SQL_fenced')
        need(stage['role'] in ('admin', 'test') and type(stage['statements']) is int and
             0 < stage['statements'] <= 180, 'SQL_role_cost')
        role = ADMIN if stage['role'] == 'admin' else ORDINARY
        self.helpers += 1
        self.statements += stage['statements'] + 3
        self.logins += 1
        need(self.helpers + getattr(self.native, 'network_helpers', 0) <= 40 and
             self.statements + self.logins <= 450, 'approved_SQL_caps40_450')
        if role == ORDINARY:
            self.native.ordinary_held()
        config = self.native.config
        env = {**BASE_ENV, 'PGHOST': config['host'], 'PGPORT': '5439',
               'PGDATABASE': DATABASE, 'PGUSER': role, 'PGSSLMODE': 'verify-full',
               'PGSSLROOTCERT': config['ca'], 'PGCONNECT_TIMEOUT': '10',
               'PGGSSENCMODE': 'disable', 'PGSSLCERTMODE': 'disable',
               'PGPASSFILE': self.native.snapshot.passfile_path if role == ADMIN
                   else str(self.native.test_passfile)}
        guard = "current_database()='odbcpp_pilot' AND TRIM(current_user)='" + role + "'"
        if role == ADMIN:
            guard += ' AND EXISTS(SELECT 1 FROM pg_catalog.pg_user WHERE usename=current_user AND usesuper)'
        script = ('SET statement_timeout=15000;\nSELECT ' + guard + ' AS held\n\\gset\n\\if :held\n'
                  "SELECT 'RSD1|'||current_database()||'|'||TRIM(current_user)||'|'||pg_backend_pid();\n")
        if 'headers' in stage:
            script += '\\pset format csv\n\\pset tuples_only off\n\\pset footer off\n'
        script += stage['query'] + '\n\\else\n\\quit 4\n\\endif\n'
        need(len(script.encode()) <= 131072, 'SQL_script_bound')
        armed = self.now()
        seconds = min(120, math.floor(deadline - armed - 2))
        need(math.isfinite(seconds) and seconds > 0, 'whole_SQL_helper_reserve')
        cutoff = armed + seconds + 2
        stem = self.directory / ('sql-' + str(self.helpers))
        log = Path(str(stem) + '.log')
        intent = Path(str(stem) + '-intent.json')
        status = Path(str(stem) + '-status.json')
        record(intent, {'stage': stage['stage'], 'SQL': stage['statements'] + 3,
                        'login': 1, 'whole_cutoff': cutoff})
        returned = False
        try:
            result = self.native.owned_process(
                [self.preparation_manifest['psql'], '-X', '-w', '-qAt', '-v',
                 'ON_ERROR_STOP=1', '-v', 'VERBOSITY=sqlstate'], env=env, seconds=seconds,
                output=log, input_bytes=script.encode('utf-8'))
            returned = type(result.returncode) is int and type(result.timed_out) is bool
            record(status, {'returned_reaped': returned, 'returncode': result.returncode,
                            'timed_out': result.timed_out})
            need(returned and result.returncode == 0 and not result.timed_out and
                 self.now() < cutoff, 'SQL_completion_unknown')
            need(0 < log.stat().st_size <= 65536, 'SQL_output_bound')
            frame, payload = log.read_bytes().split(b'\n', 1)
            fields = frame.decode('ascii').split('|')
            need(len(fields) == 4 and fields[:3] == ['RSD1', DATABASE, role] and
                 re.fullmatch('[1-9][0-9]{0,9}', fields[3]) is not None and
                 int(fields[3]) <= 2147483647, 'SQL_actual_identity')
            need(not payload or payload.endswith(b'\n'), 'SQL_truncated')
            raw = payload.decode('utf-8', 'strict')
            need('\x00' not in raw, 'SQL_NUL')
            self.proofs[stage['stage']] = {'raw': pin(log), 'intent': pin(intent),
                                          'status': pin(status)}
            need(self.now() < cutoff, 'SQL_parse_publication_cutoff')
            return raw, int(fields[3])
        except BaseException:
            self.sql_unknown = self.sql_fenced = True
            self.readers_known = self.readers_known and returned and self.native.readers_known
            raise

    def timeout_receipt_pins(self, state):
        need(set(self.authority_receipts) == {'authority', 'applicability', 'cleanup_delegation'},
             'actual_timeout_authority_receipts')
        result = copy.deepcopy(self.authority_receipts)
        for ref in result.values():
            verify_public_pin(ref['path'], ref['sha256'])
        for name, stages in {'creation': ('timeout_create', 'timeout_insert'),
                             'identity': ('timeout_identity', 'timeout_identity_after'),
                             'shape_rows': ('timeout_shape', 'timeout_rows', 'timeout_locks')}.items():
            proof = {key: self.proofs[key] for key in stages}
            path = self.directory / ('timeout-' + name + '.json')
            record(path, {'identity': state['timeout']['facts']['identity'],
                          'actual_SQL_proofs': proof, 'observed': state['timeout']['receipts']})
            result[name] = pin(path)
        return result

    def stage_profiles(self, index, run, state, logging, end):
        need(logging == inputs.LOG_OPTIONS and state['prepared'] is True, 'prepared_profiles_only')
        self.launcher.validate_profile_inputs(self.profile_inputs, deadline=end)
        profiles = copy.deepcopy(self.profiles)
        actual = state['timeout_record']
        for profile in profiles.values():
            if 'ODBCPP_REDSHIFT_TIMEOUT_OID' in profile['variables']:
                profile['variables'].update(ODBCPP_REDSHIFT_TIMEOUT_SCHEMA=actual['schema'],
                    ODBCPP_REDSHIFT_TIMEOUT_TABLE=actual['table'],
                    ODBCPP_REDSHIFT_TIMEOUT_OID=str(actual['oid']))
        connections = {key: inputs.LoggingFactory(self.factory).build_profile_connection(
            self.native.config, profile) for key, profile in profiles.items()}
        environments = {key: self.launcher.profile_private_environment(self.profile_inputs, key)
                        for key in profiles}
        two = {spec['key']: {self.factory.TWO_DB_VARIABLE: state['substitutions']['second_database']}
               for spec in self.specs if spec['case'] == self.factory.TWO_DB_CASE}
        self.env_directory = self.directory / 'case-environments'
        staged, self.env_custody = inputs.stage_case_environments(
            self.env_directory, self.blueprint, self.specs, profiles, connections,
            environments, two_database_values=two)
        path = self.directory / 'staged-index.json'
        record(path, staged)
        return path

    def run_process(self, argv, *, env, deadline, output, umask):
        need(type(argv) is list and Path(argv[0]).is_absolute() and
             argv[0] in self.executables and env == BASE_ENV and umask == 0o077 and
             not any(v in argv for v in ('-V', '-VV', '--verbose', '--show-only', '--stop-on-failure')),
             'fixed_process_environment')
        if Path(argv[0]).name == 'ctest':
            need(self.native_custody_state is None, 'oneuse_CTest_census')
            native_custody.before(self, deadline)
        seconds = deadline - self.now() - 2
        need(0 < seconds <= 3600, 'process_absolute_reserve')
        process = None
        timed_out = False
        fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            with os.fdopen(fd, 'wb') as stream:
                process = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                    stdout=stream, stderr=subprocess.STDOUT, start_new_session=True,
                    close_fds=True, umask=umask)
                self.processes.append(process)
                try:
                    process.wait(timeout=seconds)
                except subprocess.TimeoutExpired:
                    timed_out = True
                finally:
                    _stop_group(process)
            absent = self._group_absent(process.pid)
            need(self.now() < deadline, 'process_reap_absolute_cutoff')
            return SimpleNamespace(returncode=process.returncode, timed_out=timed_out,
                                   reaped=process.poll() is not None, group_absent=absent)
        finally:
            if process is not None and process.poll() is None:
                self.readers_known = False
            # CMake may have produced a credential copy even on configure failure.
            generated = self.directory / 'ctest' / 'CTestTestfile.cmake'
            if generated.exists() and not self.generated:
                self.generated.append(inputs.generated_environment_custody(generated.parent))

    @staticmethod
    def _group_absent(pid):
        for _ in range(2):
            try:
                os.killpg(pid, 0)
                return False
            except ProcessLookupError:
                pass
        return True

    def reap(self, deadline):
        self.readers_known = (self.readers_known and self.now() < deadline and
            (self.native is None or self.native.readers_known) and
            all(p.poll() is not None and self._group_absent(p.pid) for p in self.processes))
        return self.readers_known

    def cleanup_principals(self, deadline):
        stage = {'query': PIDS, 'statements': 1, 'role': 'admin', 'stage': 'principal_pids'}
        raw, own = self._sql(stage, deadline)
        values = raw.strip().splitlines()
        need(len(values) <= 8, 'exact_principal_PID_bound')
        targets = []
        for value in values:
            columns = value.split('|')
            need(len(columns) == 4 and re.fullmatch('[1-9][0-9]{0,9}', columns[0]) is not None,
                 'exact_principal_session_capture')
            targets.append((int(columns[0]), *columns[1:]))
        targets = tuple(targets)
        pids = tuple(row[0] for row in targets)
        need(len(set(pids)) == len(pids) and all(p <= 2147483647 and p != own for p in pids),
             'exact_principal_PID_identity')
        if pids:
            raw, own = self._sql(termination_stage(targets), deadline)
            try:
                acks = raw.strip().splitlines()
                self.record({'kind': 'exact_session_cleanup', 'captured': targets, 'guarded_ACK': acks})
                need(len(acks) == len(pids) and all(
                     line in (str(pid) + '|t', str(pid) + '|SKIPPED') and
                     (pid != own or line == str(pid) + '|SKIPPED')
                     for pid, line in zip(pids, acks)),
                     'exact_principal_termination_ACK')
            except BaseException:
                self.sql_unknown = self.sql_fenced = True
                raise
        result = self.final_zero(deadline)
        self.principals_quiescent = result['complete']
        if self.native_custody_state is not None:
            self.native_custody_result = native_custody.after(self, self.native_custody_state, deadline)
            return {'complete': result['complete'] and self.native_custody_result['complete'],
                    'principal_zero': True, 'native_custody': self.native_custody_result}
        return result

    def final_zero(self, deadline):
        raw, _ = self._sql({'query': ZERO, 'statements': 1, 'role': 'admin',
                            'stage': 'principal_zero'}, deadline)
        need(raw.strip() == 'f|f', 'exact_principal_session_query_zero')
        return {'complete': True}

    def verify_running(self, deadline):
        self.native.identity(deadline)
        self._available_cluster(deadline)
        return True

    def pause_until(self, deadline):
        return self.native is not None and self.native.pause_until(deadline)

    def close_inputs(self, deadline):
        need(self.readers_known and self.reap(deadline), 'private_readers_unknown_retained')
        generated = self.directory / 'ctest' / 'CTestTestfile.cmake'
        if generated.exists() and not self.generated:
            self.generated.append(inputs.generated_environment_custody(generated.parent))
        errors = []
        def attempt(function):
            try:
                function()
            except BaseException as error:
                errors.append(type(error).__name__)
        for custody in list(self.generated):
            def close_generated(custody=custody):
                inputs.close_generated_environment(custody)
                self.generated.remove(custody)
            attempt(close_generated)
        if self.env_directory is not None and self.env_directory.exists():
            attempt(lambda: inputs.close_case_environments(self.env_custody, self.env_directory))
        if self.profile_inputs is not None:
            attempt(lambda: self.launcher.close_profile_inputs(
                self.profile_inputs, readers_known=True, deadline=deadline))
        if self.native is not None:
            def close_ordinary():
                if self.native.test_passfile is not None:
                    self.native.ordinary_held()
                    os.unlink(self.native.test_passfile)
                    self.native.test_passfile = None
            attempt(close_ordinary)
            if self.native.snapshot is not None:
                attempt(lambda: need(self.native.snapshot.close(), 'snapshot_retained'))
        need(not errors and self.input_residue is None,
             'private_input_cleanup_incomplete_retained')
        return True


if __name__ == '__main__':
    raise SystemExit('ROOT composition and independent review required; no standalone execution')
