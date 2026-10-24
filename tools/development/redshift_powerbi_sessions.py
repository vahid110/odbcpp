"""Fixed qualification-only session/cap planning; no execution or import effects.

NOT_EXECUTABLE_UNTIL_ROOT_INTEGRATION. Canonical Operations must reconstruct
each stage, preserve native psql error-stop/RSD1 controls, persist begin() BEFORE
dispatch, charge every attempt and retain UNKNOWN on incomplete/late mutation.
Only native exit0 and exact bounded frames may reach complete(). No arbitrary
SQL, executor, credential reader, session terminator, watcher or timer is added.

Demand is supplied from a separately reviewed actual concurrent-client-demand
receipt WITH server census, not a physical peak at a restrictive cap, API trace
pairs, or a configured parallel-job maximum. Public pins are data: the caller
must actually verify their immutable bytes and admission/workload applicability.

Existing Operations cleanup supports at most8 targets. This optional app
partition can observe16 test sessions: DO NOT launch it until ROOT independently
integrates a partition-specific cleanup bound and its exact PID/birth controls.
The global40-helper/450-inclusive guards are unchanged. Caller still owns the
original deadlines, known readers, exact principal/query cleanup and resource
pause. A cap-restored state alone is NOT proof of resource cleanup or a feature
pass. Failed observation intents also remain pending for explicit reconciliation;
there is no retry/adoption escape hatch in this small reducer.

Safe explicit columns and syntax, checked against primary documentation:
https://docs.aws.amazon.com/redshift/latest/dg/pg_user_info.html
https://docs.aws.amazon.com/redshift/latest/dg/r_PG_DATABASE_INFO.html
https://docs.aws.amazon.com/redshift/latest/dg/r_STV_SESSIONS.html
https://docs.aws.amazon.com/redshift/latest/dg/r_ALTER_USER.html
"""
from copy import deepcopy
from datetime import datetime
from pathlib import PurePosixPath, PureWindowsPath
import re
from tools.development import redshift_session_sql as session_sql

DATABASE = 'odbcpp_pilot'
ADMIN = 'odbcpp_pilot_admin'
TEST = 'odbcpp_pilot_test'
EVENTS = ('connected', 'navigator', 'refresh')
MAX_TEST_SESSIONS = 16
MAX_OBSERVATION_AGE_MS = 300000
MAX_PAYLOAD_BYTES = 32768
STAGES = ('snapshot', 'set_cap', *(('sample_' + event) for event in EVENTS), 'quiesce', 'restore')
MUTATIONS = ('set_cap', 'restore')


class Refused(ValueError):
    pass


def need(value, reason):
    if not value:
        raise Refused(reason)


def _integer(value, low=1, high=2147483647):
    return type(value) is int and low <= value <= high


def _decimal(value, low=0, high=2147483647):
    need(type(value) is str and re.fullmatch(r'0|[1-9][0-9]{0,9}', value) is not None and
         low <= int(value) <= high, 'bounded_decimal')
    return int(value)


def _cap(value, *, database=False):
    if value == 'UNLIMITED' or (database and value == '-1'):
        return None
    return _decimal(value)


def _birth(value):
    need(type(value) is str and re.fullmatch(r'[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}(?:\.[0-9]{1,6})?', value) is not None, 'birth_format')
    try:
        return datetime.fromisoformat(value)
    except ValueError:
        raise Refused('birth_date') from None


def _digest(value):
    return type(value) is str and re.fullmatch('[0-9a-f]{64}', value) is not None


def _pin(value):
    need(type(value) is dict and set(value) == {'path', 'sha256'}, 'public_pin_schema')
    path = value['path']
    need(type(path) is str and 0 < len(path) <= 4096 and not any(c in path for c in '\x00\r\n') and
         (PurePosixPath(path).is_absolute() or PureWindowsPath(path).is_absolute()) and
         '..' not in PurePosixPath(path.replace('\\', '/')).parts and _digest(value['sha256']), 'public_pin')


def new_state(*, workload_id, input_profile_sha256, admitted_ms,
              execution_deadline_ms, cleanup_deadline_ms, resource_kind='provisioned'):
    """Original absolute UTC-ms horizons, supplied from existing finite admission."""
    need(type(workload_id) is str and re.fullmatch('[A-Za-z0-9][A-Za-z0-9_.-]{0,127}', workload_id) is not None and
         _digest(input_profile_sha256), 'workload_identity')
    need(all(_integer(v, high=2**63 - 1) for v in (admitted_ms, execution_deadline_ms, cleanup_deadline_ms)) and
         admitted_ms < execution_deadline_ms < cleanup_deadline_ms and
         cleanup_deadline_ms - execution_deadline_ms <= 900000, 'original_deadlines')
    need(resource_kind in ('provisioned', 'serverless'), 'session_resource_kind')
    result = {'phase': 'NEW', 'workload_id': workload_id, 'input_profile_sha256': input_profile_sha256,
            'admitted_ms': admitted_ms, 'execution_deadline_ms': execution_deadline_ms,
            'cleanup_deadline_ms': cleanup_deadline_ms, 'original': None, 'measurement': None,
            'temporary_cap': None, 'snapshot': None, 'observations': [], 'seen_sessions': [],
            'attempts': [], 'last_completed_ms': admitted_ms, 'pending': None}
    if resource_kind == 'serverless':
        result['resource_kind'] = resource_kind
    return result


def _validate(state):
    keys = {'phase', 'workload_id', 'input_profile_sha256', 'admitted_ms', 'execution_deadline_ms',
            'cleanup_deadline_ms', 'original', 'measurement', 'temporary_cap', 'snapshot',
            'observations', 'seen_sessions', 'attempts', 'last_completed_ms', 'pending'}
    need(type(state) is dict and set(state) in (keys, keys | {'resource_kind'}) and
         state.get('resource_kind', 'provisioned') in ('provisioned', 'serverless'), 'state_schema')
    new_state(**{key: state[key] for key in ('workload_id', 'input_profile_sha256', 'admitted_ms',
                                           'execution_deadline_ms', 'cleanup_deadline_ms')})
    need(state['phase'] in ('NEW', 'OBSERVED', 'DECIDED', 'ALTER_UNKNOWN', 'CAP_ACTIVE',
                            'QUIESCENT', 'RESTORE_UNKNOWN', 'CAP_RESTORED', 'CAP_VERIFIED_UNCHANGED'), 'phase')
    need(type(state['attempts']) is list and len(state['attempts']) <= 7 and
         all(kind in STAGES for kind in state['attempts']) and
         len(state['attempts']) == len(set(state['attempts'])), 'attempt_bound')
    need(type(state['observations']) is list and len(state['observations']) <= 3 and
         type(state['seen_sessions']) is list and len(state['seen_sessions']) <= 48, 'observation_bound')
    need(_integer(state['last_completed_ms'], high=2**63 - 1) and
         state['admitted_ms'] <= state['last_completed_ms'] < state['cleanup_deadline_ms'], 'completion_clock')
    original = state['original']
    if original is not None:
        need(type(original) is dict and set(original) == {'admin_oid', 'test_oid', 'database_oid', 'admin_cap', 'test_cap', 'database_cap'}, 'original_schema')
        need(all(_integer(original[k], high=4294967295) for k in ('admin_oid', 'test_oid', 'database_oid')) and
             original['admin_oid'] != original['test_oid'], 'original_oid')
        _cap(original['admin_cap'])
        _cap(original['test_cap'])
        _cap(original['database_cap'], database=True)
    need(state['temporary_cap'] is None or _integer(state['temporary_cap'], high=16), 'temporary_cap_bound')


def _now(state, now_ms, *, cleanup=False):
    end = state['cleanup_deadline_ms' if cleanup else 'execution_deadline_ms']
    need(_integer(now_ms, high=2**63 - 1) and state['last_completed_ms'] <= now_ms < end, 'original_deadline_expired_or_clock_backwards')
    return end


def _measurement(value, state, now_ms):
    keys = {'schema_version', 'observation_kind', 'bound_kind', 'parallel_mode', 'workload_id',
            'input_profile_sha256', 'observed_at_ms', 'measured_demand', 'finite_headroom',
            'requested_cap', 'headroom_justification', 'client_demand', 'server_census', 'review'}
    need(type(value) is dict and set(value) == keys and type(value['schema_version']) is int and
         value['schema_version'] == 1, 'measurement_schema')
    need(value['observation_kind'] == 'client_demand_with_server_census' and
         value['bound_kind'] == 'measured_simultaneous_request_upper_bound' and
         value['parallel_mode'] in ('default_parallel', 'explicit_parallel'), 'actual_demand_measurement_required')
    need(value['workload_id'] == state['workload_id'] and
         value['input_profile_sha256'] == state['input_profile_sha256'], 'measurement_workload_mismatch')
    need(_integer(value['observed_at_ms'], high=2**63 - 1) and
         0 <= now_ms - value['observed_at_ms'] <= MAX_OBSERVATION_AGE_MS, 'measurement_not_fresh')
    need(_integer(value['measured_demand'], high=16) and _integer(value['finite_headroom'], low=0, high=15) and
         _integer(value['requested_cap'], high=16) and
         value['requested_cap'] == value['measured_demand'] + value['finite_headroom'], 'explicit_finite_demand_headroom')
    reason = value['headroom_justification']
    need(type(reason) is str and 1 <= len(reason) <= 256 and all(32 <= ord(c) <= 126 for c in reason) and
         reason == reason.strip(), 'explicit_headroom_justification')
    for key in ('client_demand', 'server_census', 'review'):
        _pin(value[key])
    need(len({value[key]['path'] for key in ('client_demand', 'server_census', 'review')}) == 3, 'distinct_measurement_evidence')


def decide(state, measured_demand, *, now_ms):
    """No default demand/cap. No receipt is generated or read by this function."""
    _validate(state)
    _now(state, now_ms)
    need(state['phase'] == 'OBSERVED' and state['pending'] is None and state['original'] is not None, 'fresh_snapshot_required')
    need(0 <= now_ms - state['snapshot']['completed_ms'] <= MAX_OBSERVATION_AGE_MS, 'snapshot_not_fresh')
    _measurement(measured_demand, state, now_ms)
    requested = measured_demand['requested_cap']
    db_cap = _cap(state['original']['database_cap'], database=True)
    # Conservative extra slot for the serial admin observation/cleanup helper,
    # even though Redshift documents that superusers are exempt from these caps.
    need(db_cap is None or db_cap >= requested + 1, 'inadequate_database_headroom')
    result = deepcopy(state)
    result['measurement'] = deepcopy(measured_demand)
    original_cap = _cap(state['original']['test_cap'])
    # The effective server cap, not merely the observation count, must match
    # the finite cleanup envelope. Also bound UNLIMITED/larger originals and
    # retain their exact literal for restoration after the app is quiescent.
    result['temporary_cap'] = requested if original_cap != requested else None
    result['phase'] = 'DECIDED'
    result['last_completed_ms'] = now_ms
    return result


def _scope():
    return ("(TRIM(db_name)='" + DATABASE + "' OR TRIM(user_name) IN ('" + ADMIN + "','" + TEST +
            "')) AND process<>pg_backend_pid()")


def _current_cap(state):
    return str(state['temporary_cap']) if state['phase'] in ('CAP_ACTIVE', 'QUIESCENT', 'RESTORE_UNKNOWN') else state['original']['test_cap']


def stage_for(state, kind):
    """Fixed statement counts exclude the existing3 wrapper SQL and1 login.

    Each snapshot/sample has bounded17-row scoped census (16 plus overflow
    sentinel); SQL includes only the safe explicit user/database columns below.
    Caller must gate quiesce/restore on known stopped readers and exact principal/
    query cleanup. This planner never terminates a session or changes database,
    administrator, global or authentication settings.
    """
    _validate(state)
    need(state['pending'] is None and kind in STAGES and kind not in state['attempts'], 'pending_or_repeated_stage')
    if kind == 'snapshot':
        need(state['phase'] == 'NEW', 'snapshot_once')
    elif kind == 'set_cap':
        need(state['phase'] == 'DECIDED' and state['temporary_cap'] is not None, 'no_temporary_change_required')
    elif kind.startswith('sample_'):
        need(state['phase'] in ('DECIDED', 'CAP_ACTIVE') and
             (state['temporary_cap'] is None or state['phase'] == 'CAP_ACTIVE'), 'sample_before_admitted_cap')
    elif kind == 'quiesce':
        need(state['phase'] in ('DECIDED', 'CAP_ACTIVE') and
             (state['temporary_cap'] is None or state['phase'] == 'CAP_ACTIVE'), 'quiesce_phase')
    else:
        need(state['phase'] == 'QUIESCENT' and state['temporary_cap'] is not None, 'restore_requires_quiescence')
    serverless = state.get('resource_kind', 'provisioned') == 'serverless'
    principal_oids = ({key: state['original'][key] for key in ('admin_oid', 'test_oid')}
                      if state['original'] is not None else {})
    lines, count = ([session_sql.binding(**principal_oids)], 1) if serverless else ([], 0)

    def sql(query, label=None):
        nonlocal count
        if label:
            lines.append('\\echo RSPBIS1|' + label)
        lines.append(query + ';')
        count += 1

    def guard(query):
        sql('SELECT CASE WHEN ' + query + ' THEN 1 ELSE 0 END AS pbi_sessions_held')
        lines[-1] = lines[-1][:-1] + '\n\\gset\n\\if :pbi_sessions_held'

    def census(prefix):
        sql("SELECT usesysid,TRIM(usename),usesuper,TRIM(useconnlimit) FROM pg_user_info WHERE usename IN ('" +
            ADMIN + "','" + TEST + "') ORDER BY usename LIMIT 3", prefix + 'USERS')
        sql("SELECT datid,TRIM(datname),TRIM(datconnlimit) FROM pg_database_info WHERE datname='" + DATABASE + "' LIMIT 2", prefix + 'DATABASE')
        sql('SELECT COUNT(*) FROM ' + ('sys_session_history WHERE ' + session_sql.scope(broad=True)
            if serverless else 'stv_sessions WHERE ' + _scope()), prefix + 'COUNT')
        sql(session_sql.rows(broad=True, limit=17) if serverless else
            'SELECT process,TRIM(user_name),TRIM(db_name),starttime::VARCHAR FROM stv_sessions WHERE ' +
            _scope() + ' ORDER BY process LIMIT 17', prefix + 'SESSIONS')

    sql(session_sql.rows(self_only=True, limit=2) if serverless else
        "SELECT process,TRIM(user_name),TRIM(db_name),starttime::VARCHAR FROM stv_sessions WHERE process=pg_backend_pid() LIMIT 2", 'SELF')
    if kind not in MUTATIONS:
        census('')
    else:
        census('PRE_')
        original = state['original']
        expected_cap = original['test_cap'] if kind == 'set_cap' else str(state['temporary_cap'])
        target_cap = str(state['temporary_cap']) if kind == 'set_cap' else original['test_cap']
        held = ("current_database()='" + DATABASE + "' AND TRIM(current_user)='" + ADMIN +
                "' AND EXISTS(SELECT 1 FROM pg_user_info WHERE usesysid=" + str(original['admin_oid']) +
                " AND usename='" + ADMIN + "' AND usesuper AND TRIM(useconnlimit)='" + original['admin_cap'] +
                "') AND EXISTS(SELECT 1 FROM pg_user_info WHERE usesysid=" + str(original['test_oid']) +
                " AND usename='" + TEST + "' AND NOT usesuper AND TRIM(useconnlimit)='" + expected_cap +
                "') AND EXISTS(SELECT 1 FROM pg_database_info WHERE datid=" + str(original['database_oid']) +
                " AND datname='" + DATABASE + "' AND TRIM(datconnlimit)='" + original['database_cap'] + "')")
        if serverless:
            held += ' AND :rs_admin_oid=' + str(original['admin_oid']) + ' AND :rs_test_oid=' + str(original['test_oid'])
        guard(held)
        # Separate STV and PG-info guards retain the existing leader/compute
        # separation rather than joining system relations in a new query.
        guard('(SELECT COUNT(*)=0 FROM ' + ('sys_session_history WHERE ' + session_sql.scope(broad=True)
              if serverless else 'stv_sessions WHERE ' + _scope()) + ')')
        sql('ALTER USER "' + TEST + '" CONNECTION LIMIT ' + target_cap)
        lines.append('\\echo RSPBIS1|' + ('ALTER_ACK' if kind == 'set_cap' else 'RESTORE_ACK'))
        census('POST_')
        lines.extend(('\\else\n\\echo RSPBIS1|REFUSED\n\\endif',
                      '\\else\n\\echo RSPBIS1|REFUSED\n\\endif'))
    query = '\n'.join(lines) + '\n'
    need(len(query.encode('utf-8')) <= 16384 and count == (12 if kind in MUTATIONS else 5) + int(serverless), 'stage_bounds')
    return {'stage': 'powerbi_sessions_' + kind, 'keys': ('POWERBI_SESSIONS',),
            'role': 'admin', 'query': query, 'statements': count}


def begin(state, stage, *, now_ms):
    """Persist returned intent before ONE actual native dispatch, including reads."""
    need(type(stage) is dict and type(stage.get('stage')) is str, 'stage_schema')
    kind = stage['stage'].removeprefix('powerbi_sessions_')
    need(stage == stage_for(state, kind), 'stage_changed')
    deadline = _now(state, now_ms, cleanup=kind in ('quiesce', 'restore'))
    if kind == 'set_cap':
        _measurement(state['measurement'], state, now_ms)
        need(0 <= now_ms - state['snapshot']['completed_ms'] <= MAX_OBSERVATION_AGE_MS, 'snapshot_not_fresh')
    result = deepcopy(state)
    result['attempts'].append(kind)
    result['pending'] = {'before': deepcopy(state), 'stage': deepcopy(stage),
                         'dispatched_ms': now_ms, 'deadline_ms': deadline}
    if kind == 'set_cap':
        result['phase'] = 'ALTER_UNKNOWN'
    elif kind == 'restore':
        result['phase'] = 'RESTORE_UNKNOWN'
    return result


def parse_frames(payload):
    need(type(payload) is str and len(payload.encode('utf-8')) <= MAX_PAYLOAD_BYTES and '\x00' not in payload, 'frame_bounds')
    result, label = {}, None
    for line in payload.splitlines():
        if line.startswith('RSPBIS1|'):
            label = line[8:]
            need(re.fullmatch('[A-Z_]+', label) is not None and label not in result, 'duplicate_or_bad_frame')
            result[label] = []
        else:
            need(label is not None and 0 < len(line) <= 512 and len(result[label]) < 17, 'unframed_or_excess_rows')
            result[label].append(line)
    return result


def _observe(frames, prefix, self_row):
    users = frames[prefix + 'USERS']
    need(len(users) == 2, 'exact_two_users')
    parsed = []
    for raw, name, superuser in zip(users, (ADMIN, TEST), ('t', 'f')):
        cells = raw.split('|')
        need(len(cells) == 4 and cells[1:3] == [name, superuser], 'user_identity_or_superuser')
        parsed.append((_decimal(cells[0], low=1, high=4294967295), cells[3]))
        _cap(cells[3])
    need(parsed[0][0] != parsed[1][0], 'distinct_user_oid')
    database = frames[prefix + 'DATABASE']
    need(len(database) == 1, 'exact_database')
    db = database[0].split('|')
    need(len(db) == 3 and db[1] == DATABASE, 'database_identity')
    db_oid = _decimal(db[0], low=1, high=4294967295)
    _cap(db[2], database=True)
    count = frames[prefix + 'COUNT']
    need(len(count) == 1, 'session_count_frame')
    total = _decimal(count[0], high=16)
    sessions = []
    for raw in frames[prefix + 'SESSIONS']:
        cells = raw.split('|')
        need(len(cells) == 4, 'session_fields')
        pid = _decimal(cells[0], low=1)
        birth = _birth(cells[3])
        need(cells[1:3] == [TEST, DATABASE] and pid != self_row['pid'], 'unrelated_or_helper_session')
        sessions.append({'pid': pid, 'principal': TEST, 'database': DATABASE, 'birth': cells[3]})
    need(len(sessions) == total and len({row['pid'] for row in sessions}) == total, 'census_race_duplicate_or_overflow')
    return {'admin_oid': parsed[0][0], 'test_oid': parsed[1][0], 'database_oid': db_oid,
            'admin_cap': parsed[0][1], 'test_cap': parsed[1][1], 'database_cap': db[2]}, sessions


def complete(state, payload, *, native_exit, now_ms):
    """Caller supplies the real native policy exit and strips validated RSD1 only.

    Late, nonzero, absent ACK or changed readback leaves the input intent intact;
    the caller must persist that unresolved state, not reset/adopt/retry it.
    """
    _validate(state)
    pending = state['pending']
    need(type(pending) is dict and set(pending) == {'before', 'stage', 'dispatched_ms', 'deadline_ms'}, 'completion_without_intent')
    before, stage = pending['before'], pending['stage']
    need(state == begin(before, stage, now_ms=pending['dispatched_ms']), 'intent_changed')
    need(type(native_exit) is int and native_exit == 0, 'native_completion_not_success')
    need(_integer(now_ms, high=2**63 - 1) and pending['dispatched_ms'] <= now_ms < pending['deadline_ms'], 'late_or_invalid_ack')
    frames = parse_frames(payload)
    kind = stage['stage'].removeprefix('powerbi_sessions_')
    labels = {'SELF'}
    for prefix in (('PRE_', 'POST_') if kind in MUTATIONS else ('',)):
        labels.update(prefix + key for key in ('USERS', 'DATABASE', 'COUNT', 'SESSIONS'))
    if kind in MUTATIONS:
        label = 'ALTER_ACK' if kind == 'set_cap' else 'RESTORE_ACK'
        labels.add(label)
        need(frames.get(label) == [], 'actual_ack_required')
    need(set(frames) == labels, 'exact_stage_frames')
    need(len(frames['SELF']) == 1, 'self_frame')
    self_cells = frames['SELF'][0].split('|')
    need(len(self_cells) == 4 and self_cells[1:3] == [ADMIN, DATABASE], 'helper_identity')
    self_row = {'pid': _decimal(self_cells[0], low=1), 'birth': self_cells[3]}
    self_birth = _birth(self_row['birth'])
    result = deepcopy(before)
    result['attempts'] = deepcopy(state['attempts'])
    result['last_completed_ms'] = now_ms
    if kind == 'snapshot':
        original, sessions = _observe(frames, '', self_row)
        need(not sessions, 'preexisting_session_refused')
        need(_cap(original['database_cap'], database=True) is None or
             _cap(original['database_cap'], database=True) >= 2, 'inadequate_database_headroom')
        result['original'] = original
        result['snapshot'] = {'helper': self_row, 'completed_ms': now_ms}
        result['phase'] = 'OBSERVED'
    else:
        original = before['original']
        need(self_birth >= _birth(before['snapshot']['helper']['birth']), 'helper_birth_before_snapshot')
        if kind in MUTATIONS:
            pre, sessions = _observe(frames, 'PRE_', self_row)
            post, post_sessions = _observe(frames, 'POST_', self_row)
            expected_pre = deepcopy(original)
            expected_post = deepcopy(original)
            if kind == 'set_cap':
                expected_post['test_cap'] = str(before['temporary_cap'])
            else:
                expected_pre['test_cap'] = str(before['temporary_cap'])
            need(pre == expected_pre and post == expected_post and not sessions and not post_sessions,
                 'held_identity_cap_or_zero_changed')
            result['phase'] = 'CAP_ACTIVE' if kind == 'set_cap' else 'CAP_RESTORED'
        else:
            observed, sessions = _observe(frames, '', self_row)
            expected = {**original, 'test_cap': _current_cap(before)}
            need(observed == expected, 'held_identity_or_cap_changed')
            if kind == 'quiesce':
                need(not sessions, 'restore_requires_zero_sessions')
                result['phase'] = 'QUIESCENT' if before['temporary_cap'] is not None else 'CAP_VERIFIED_UNCHANGED'
            else:
                requested = before['measurement']['requested_cap']
                need(len(sessions) <= requested, 'partition_session_bound_exceeded')
                seen = {row['pid']: row for row in before['seen_sessions']}
                for row in sessions:
                    need(_birth(row['birth']) >= _birth(before['snapshot']['helper']['birth']), 'preexisting_session_birth')
                    need(row['pid'] not in seen or seen[row['pid']] == row, 'pid_birth_reuse')
                    seen[row['pid']] = row
                result['seen_sessions'] = [seen[pid] for pid in sorted(seen)]
                result['observations'].append({'event': kind[7:], 'completed_ms': now_ms, 'helper': self_row,
                                               'sessions': sessions, 'at_partition_bound': len(sessions) == requested,
                                               'not_a_client_demand_measurement': True})
    return result


def public_plan(resource_kind='provisioned'):
    need(resource_kind in ('provisioned', 'serverless'), 'session_plan_resource_kind')
    result = {'readiness': 'NOT_EXECUTABLE_UNTIL_ROOT_INTEGRATION', 'schema_version': 1,
            'safe_user_columns': ['usesysid', 'usename', 'usesuper', 'useconnlimit'],
            'safe_database_columns': ['datid', 'datname', 'datconnlimit'],
            'event_samples': list(EVENTS), 'max_test_sessions': 16, 'census_row_limit': 17,
            'stage_SQL': {'snapshot': 5, 'set_cap': 12, 'sample_each': 5, 'quiesce': 5, 'restore': 12},
            'maximum_changed_path': {'helpers': 7, 'SQL_before_wrappers': 49, 'wrapper_SQL': 21,
                                     'SQL': 70, 'logins': 7, 'inclusive': 77},
            'maximum_unchanged_path': {'helpers': 5, 'SQL_before_wrappers': 25, 'wrapper_SQL': 15,
                                       'SQL': 40, 'logins': 5, 'inclusive': 45},
            'existing_global_guards_unchanged': {'helpers_including_network': 40, 'SQL_plus_logins': 450},
            'existing_cleanup_max8_insufficient_for16': True,
            'required_integration': ['Closed Operations reconstruction and durable intent before each dispatch',
                                    'Native exit0, validated RSD1 and exact bounded frames; no retry/adoption of unknown mutation',
                                    'Reviewed actual concurrent-client demand AND server census with matching workload/input pins',
                                    'Original absolute horizons, known stopped readers and exact principal/query cleanup before restore',
                                    'Separately reviewed partition-specific16-target exact PID/birth cleanup before launch',
                                    'Complete combined fixture/network/sample/restoration reserve within unchanged40/450 guards'],
            'scope': 'Operational custody only; no native feature results, credentials, execution, termination or resource-cleanup claim'}
    if resource_kind == 'serverless':
        result['resource_kind'] = resource_kind
        for key in result['stage_SQL']:
            result['stage_SQL'][key] += 1
        for key, extra in (('maximum_changed_path', 7), ('maximum_unchanged_path', 5)):
            for field in ('SQL_before_wrappers', 'SQL', 'inclusive'):
                result[key][field] += extra
    return result
