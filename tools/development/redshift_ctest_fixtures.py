"""Closed Redshift fixture lifecycle for one collect-all CTest invocation.

The injected adapter owns SQL identity/TLS, counters, process supervision and
the original absolute clock. This module neither starts tests nor interprets
their results. Accepted SQL factories are injected, not imported with effects.
"""
import copy
import csv
import io


KEYS = (
    'SHARED_FIXTURE_SCHEMA', 'CONFIGURED_PILOT_ROWS_REUSE',
    'COMPOSITE_PARENT', 'COMPOSITE_CHILD', 'NO_KEY', 'QUOTED_KEY',
    'POSITIVE_ROUTINE', 'UNICODE_SCHEMA', 'UNICODE_TWO_ROWS',
    'EMPTY_FOUR_TYPES', 'ABSENT_SCHEMA', 'ABSENT_PK', 'ABSENT_ROUTINE',
)


class FixtureError(RuntimeError):
    def __init__(self, code, state):
        super().__init__(code)
        self.state = state


def plan():
    """Conservative full setup/cleanup reserve, including wrapper SQL/logins.

    Counts are SQL helper counts; the outer network process is separate. No
    bounds are increased here. The lifecycle must reject these totals under
    the historical 20-helper/183-inclusive admission before opening inputs.
    """
    return {
        'required_fixture_keys': list(KEYS) + ['TIMEOUT_GATE', 'DATABASE_CREATE',
                                               'SECOND_DATABASE_PUBLIC'],
        'fixture_helpers': 28, 'fixture_SQL': 310,
        'total_helpers': 36, 'total_SQL': 356, 'total_logins': 36,
        'total_inclusive': 392,
        'old_caps': {'helpers': 20, 'inclusive': 183},
        'branches': {
            'shared_all_missing_except_required_existing_schema':
                {'helpers': 7, 'SQL': 204},
            'fresh_timeout_temporary_schema_create': {'helpers': 13, 'SQL': 62},
            'timeout_exact_drop': {'helpers': 1, 'SQL': 13},
            'database_create_own_grant_and_restore': {'helpers': 5, 'SQL': 22},
            'second_database_public': {'helpers': 2, 'SQL': 9},
            'outer_startup': {'helpers': 5, 'SQL': 23},
            'outer_max8_PID_capture_terminate_and_final_zero':
                {'helpers': 3, 'SQL': 23},
        },
        'wrapper_SQL_per_helper': 3, 'login_per_helper': 1,
        'shared_batch_max': 10, 'ordinary_case_execution': 'CTest only',
        'existing_routine_collision': 'refuse without owned creation custody',
    }


def _need(value, code, state):
    if not value:
        raise FixtureError(code, state)


def _event(ops, state, kind, **value):
    event = {'kind': kind, **copy.deepcopy(value)}
    state['events'].append(event)
    ops.record(event)


def _sql_allowed(ops, state, deadline):
    _need(ops.now() < deadline, 'fixture_absolute_deadline', state)
    _need(getattr(ops, 'readers_known', False) is True and
          not getattr(ops, 'sql_unknown', False) and
          not getattr(ops, 'sql_fenced', False), 'fixture_SQL_fenced', state)


def _sql(ops, state, stage, deadline):
    _sql_allowed(ops, state, deadline)
    _event(ops, state, 'SQL_intent', stage=stage['stage'], role=stage['role'],
           statements=stage['statements'])
    raw = ops.sql(stage, deadline)
    _need(type(raw) is str and len(raw.encode('utf-8')) <= 65536 and
          '\x00' not in raw and ops.now() < deadline,
          'fixture_SQL_payload_or_deadline', state)
    state['helpers'] += 1
    state['SQL'] += stage['statements'] + 3
    _event(ops, state, 'SQL_completed', stage=stage['stage'], raw=raw)
    return raw


def _batch(ops, state, keys, kind, deadline):
    f = ops.factory
    stage = f.whole_batch_stage(state['plan'], keys, kind, state['records'])
    return f.whole_frames(_sql(ops, state, stage, deadline))


def _groups(keys):
    return [tuple(keys[i:i + 10]) for i in range(0, len(keys), 10)]


def _timeout_get(ops, state, key, deadline):
    t = state['timeout']
    query, headers, maxrows, inclusive = ops.timeout_factory.query_set(
        t['table'], t['facts'])[key]
    stage = {'query': query, 'headers': headers, 'maxrows': maxrows,
             'statements': inclusive - 3,
             'role': 'admin' if key in ops.timeout_factory.ADMIN_KEYS else 'test',
             'keys': ('TIMEOUT_GATE',), 'stage': 'timeout_' + key}
    raw = _sql(ops, state, stage, deadline)
    rows = list(csv.reader(io.StringIO(raw), strict=True))
    _need(len(rows) == 2 and tuple(rows[0]) == tuple(headers) and
          len(rows[1]) == len(headers) and maxrows >= 1,
          'timeout_exact_one_CSV_row_' + key, state)
    value = dict(zip(headers, rows[1]))
    t['receipts'][key] = value
    _event(ops, state, 'timeout_receipt', key=key, value=value)
    return value


def _prepare_timeout(ops, state, deadline):
    t = state['timeout']
    f = ops.timeout_factory
    initial = f.permission(_timeout_get(ops, state, 'permission_initial', deadline),
                           absent='t')
    _need(all(initial[k] == v for k, v in f.EXPECTED_PRINCIPAL.items()),
          'timeout_exact_initial_identity', state)
    t['facts']['permission'] = initial
    if initial['create'] == 'f':
        t['grant'] = 'UNKNOWN'
        _need(_timeout_get(ops, state, 'grant', deadline) == {'ack': 'grant_ACK'},
              'timeout_schema_GRANT_ACK', state)
        t['grant'] = 'OWNED'
        f.permission(_timeout_get(ops, state, 'permission_check', deadline),
                     initial, create='t', absent='t')
    t['facts']['pre'] = f.preflight(_timeout_get(ops, state, 'inspect', deadline))
    f.permission(t['facts']['pre'], initial, create='t', absent='t')
    t['object'] = 'CREATE_UNKNOWN'
    _need(_timeout_get(ops, state, 'create', deadline) == {'ack': 'create_ACK'},
          'timeout_CREATE_ACK', state)
    t['object'] = 'CREATE_ACK'
    bare = _timeout_get(ops, state, 'identity', deadline)
    t['facts']['identity'] = f.identity(dict(bare, shape='t'),
                                        t['facts']['pre'], t['table'])
    _need(_timeout_get(ops, state, 'shape', deadline) ==
          {'ordinal_position': '1', 'column_name': 'value', 'data_type': 'integer',
           'is_nullable': 'NO', 'column_default': ''}, 'timeout_exact_shape', state)
    _need(_timeout_get(ops, state, 'insert', deadline) == {'ack': 'insert_ACK'},
          'timeout_INSERT_ACK', state)
    _need(_timeout_get(ops, state, 'rows', deadline) ==
          {'count': '1', 'min': '1', 'max': '1'}, 'timeout_exact_rows', state)
    _need(_timeout_get(ops, state, 'locks', deadline) == {'locks': '0'},
          'timeout_no_locks', state)
    _restore_schema_grant(ops, state, deadline)
    f.permission(_timeout_get(ops, state, 'permission_final', deadline), initial,
                 create=initial['create'], absent='f')
    if t['grant'] == 'REVOKE_ACK':
        t['grant'] = 'RESTORED'
    _need(_timeout_get(ops, state, 'identity_after', deadline) == bare,
          'timeout_identity_after', state)
    # The adapter persists real proof receipts and supplies their actual pins.
    # No placeholder OID, acknowledgment or six-receipt handoff is synthesized.
    t['record'] = f.custody_record(t['table'], t['facts']['identity'],
                                  ops.timeout_receipt_pins(state))
    t['object'] = 'OWNED'
    state['timeout_record'] = copy.deepcopy(t['record'])
    state['substitutions']['timeout'] = copy.deepcopy(t['record'])


def _restore_schema_grant(ops, state, deadline):
    t = state['timeout']
    if t['grant'] == 'OWNED':
        t['grant'] = 'REVOKE_UNKNOWN'
        _need(_timeout_get(ops, state, 'revoke', deadline) == {'ack': 'revoke_ACK'},
              'timeout_schema_REVOKE_ACK', state)
        t['grant'] = 'REVOKE_ACK'
    # Effective false is observed by permission_final, not inferred from ACK.


def _db_sql(ops, state, kind, deadline):
    queries = {
        'probe': ("SELECT has_database_privilege('odbcpp_pilot_test',"
                  "'odbcpp_pilot','CREATE');", 1),
        'grant': ('GRANT CREATE ON DATABASE odbcpp_pilot TO odbcpp_pilot_test;\n'
                  "SELECT 'DBCREATE_GRANT_ACK_V1';", 2),
        'revoke': ('REVOKE CREATE ON DATABASE odbcpp_pilot FROM odbcpp_pilot_test;\n'
                   "SELECT 'DBCREATE_REVOKE_ACK_V1';", 2),
    }
    query, count = queries[kind]
    return _sql(ops, state, {'query': query, 'statements': count, 'role': 'admin',
                            'keys': ('DATABASE_CREATE',), 'stage': 'db_' + kind},
                deadline).strip()


def prepare(ops, deadline):
    """Prepare every shared fixture once; preserve partial custody on failure."""
    state = {'records': {k: ops.factory.new_fixture_record(k) for k in KEYS},
             'plan': ops.factory.closed_fixture_plan(), 'events': [],
             'helpers': 0, 'SQL': 0, 'substitutions': {}, 'prepared': False,
             'database_create': {'initial': None, 'state': 'UNTOUCHED'},
             'timeout': {'table': ops.timeout_factory.TABLE, 'facts': {},
                         'receipts': {}, 'grant': 'UNTOUCHED',
                         'object': 'UNTOUCHED', 'record': None}}
    ops.fixture_state = state
    try:
        _event(ops, state, 'fixture_state_started', keys=list(KEYS))
        for keys in _groups(KEYS):
            frames = _batch(ops, state, keys, 'inspect', deadline)
            for key in keys:
                ops.factory.whole_capture(state['plan'], (key,), frames,
                                           state['records'])
                _event(ops, state, 'fixture_custody', key=key,
                       record=state['records'][key])
        missing = [k for k in KEYS if state['records'][k]['state'] == 'MISSING']
        for keys in _groups(missing):
            stage = ops.factory.whole_batch_stage(state['plan'], keys, 'create',
                                                   state['records'])
            for key in keys:
                state['records'][key]['state'] = 'CREATE_UNKNOWN'
            frames = ops.factory.whole_frames(_sql(ops, state, stage, deadline))
            for key in keys:
                ops.factory.whole_capture(state['plan'], (key,), frames,
                                           state['records'], creating=True)
                _event(ops, state, 'fixture_custody', key=key,
                       record=state['records'][key])
        for keys in _groups(KEYS):
            frames = _batch(ops, state, keys, 'ordinary', deadline)
            ops.factory.whole_validate_ready(state['plan'], keys, frames,
                                              state['records'])
        _prepare_timeout(ops, state, deadline)
        db = state['database_create']
        db['initial'] = _db_sql(ops, state, 'probe', deadline)
        _need(db['initial'] in ('t', 'f'), 'database_CREATE_baseline', state)
        if db['initial'] == 'f':
            db['state'] = 'GRANT_UNKNOWN'
            _need(_db_sql(ops, state, 'grant', deadline) ==
                  'DBCREATE_GRANT_ACK_V1', 'database_CREATE_GRANT_ACK', state)
            db['state'] = 'OWNED'
            _need(_db_sql(ops, state, 'probe', deadline) == 't',
                  'database_CREATE_effective_true', state)
        else:
            db['state'] = 'PREEXISTING_TRUE'
        other = ops.factory.two_db_select(_sql(
            ops, state, ops.factory.two_db_stage('databases'), deadline))
        ops.factory.two_db_public(_sql(
            ops, state, ops.factory.two_db_stage('schemas', other), deadline), other)
        state['substitutions']['second_database'] = other
        state['prepared'] = True
        _event(ops, state, 'fixtures_prepared', substitutions=state['substitutions'])
        return state
    except Exception as error:
        if isinstance(error, FixtureError):
            raise
        raise FixtureError('fixture_prepare_' + type(error).__name__, state) from error



def resume_prepared(ops, state, deadline):
    """Revalidate ROOT-supplied exact prepared custody without creating anything.

    The caller owns the immutable predecessor receipts and has freshly verified
    principal zero under the new lifecycle. Existing custody/grant ownership is
    retained so ordinary cleanup closes the predecessor's exact created objects.
    """
    ops.fixture_state = state
    try:
        _need(type(state) is dict and state.get('prepared') is True and
              set(state.get('records', {})) == set(KEYS), 'resume_prepared_state', state)
        state['prepared'] = False
        _sql_allowed(ops, state, deadline)
        for keys in _groups(KEYS):
            before = {key: state['records'][key].get('readiness_sha256') for key in keys}
            frames = _batch(ops, state, keys, 'ordinary', deadline)
            ops.factory.whole_validate_ready(state['plan'], keys, frames, state['records'])
            _need(all(before[key] is None or before[key] ==
                      state['records'][key].get('readiness_sha256') for key in keys),
                  'resume_shared_fixture_changed', state)
        t = state['timeout']
        record = ops.factory.validate_timeout_preowned(t['record'])
        _need(t['object'] == 'OWNED' and t['grant'] in ('UNTOUCHED', 'RESTORED') and
              state['timeout_record'] == record and
              state['substitutions']['timeout'] == record, 'resume_timeout_custody', state)
        bare = _timeout_get(ops, state, 'identity_after', deadline)
        actual = ops.timeout_factory.identity(dict(bare, shape='t'),
                                              t['facts']['pre'], t['table'])
        _need(actual == t['facts']['identity'] and all(
            str(actual[key]) == str(record[key]) for key in
            ('table', 'oid', 'owner_oid', 'schema_oid', 'schema_owner_oid')),
            'resume_timeout_identity', state)
        _need(_timeout_get(ops, state, 'shape', deadline) ==
              {'ordinal_position': '1', 'column_name': 'value', 'data_type': 'integer',
               'is_nullable': 'NO', 'column_default': ''}, 'resume_timeout_shape', state)
        _need(_timeout_get(ops, state, 'rows', deadline) ==
              {'count': '1', 'min': '1', 'max': '1'}, 'resume_timeout_rows', state)
        _need(_timeout_get(ops, state, 'locks', deadline) == {'locks': '0'},
              'resume_timeout_locks', state)
        initial = t['facts']['permission']
        ops.timeout_factory.permission(_timeout_get(ops, state, 'permission_final', deadline),
                                       initial, create=initial['create'], absent='f')
        db = state['database_create']
        _need((db['initial'], db['state']) in (('f', 'OWNED'), ('t', 'PREEXISTING_TRUE')),
              'resume_database_CREATE_custody', state)
        _need(_db_sql(ops, state, 'probe', deadline) == 't',
              'resume_database_CREATE_effective_true', state)
        other = ops.factory.two_db_select(_sql(
            ops, state, ops.factory.two_db_stage('databases'), deadline))
        _need(other == state['substitutions']['second_database'],
              'resume_second_database_changed', state)
        ops.factory.two_db_public(_sql(
            ops, state, ops.factory.two_db_stage('schemas', other), deadline), other)
        state['prepared'] = True
        _event(ops, state, 'fixtures_resumed', substitutions=state['substitutions'])
        return state
    except Exception as error:
        if isinstance(error, FixtureError):
            raise
        raise FixtureError('fixture_resume_' + type(error).__name__, state) from error


def cleanup(ops, state, deadline):
    """Drop only positively owned exact objects after actual principal zero.

    A known validation failure does not prevent closure of another known-owned
    item. Each dispatch still independently refuses unknown readers/SQL state.
    Unknown mutation completion is never replayed as an owned object.
    """
    receipt = {'complete': False, 'errors': [], 'owned_dropped': [],
               'reused_preserved': [], 'permission_restored': False}
    if state is None:
        receipt['complete'] = True
        return receipt

    def attempt(function):
        try:
            _need(getattr(ops, 'principals_quiescent', False) is True,
                  'fixture_cleanup_requires_actual_principal_zero', state)
            function()
        except Exception as error:
            receipt['errors'].append(type(error).__name__)

    records = state['records']
    known = [k for k in KEYS if records[k]['state'] in
             ('OWNED', 'READ_ONLY_REUSED', 'VERIFIED_ABSENT')]
    for keys in reversed(_groups(known)):
        def close_batch(keys=keys):
            owned = [k for k in keys if records[k]['state'] == 'OWNED']
            reused = [k for k in keys if records[k]['state'] != 'OWNED']
            stage = ops.factory.whole_batch_stage(state['plan'], keys, 'cleanup', records)
            _sql_allowed(ops, state, deadline)
            for key in owned:
                records[key]['state'] = 'DROP_UNKNOWN'
            frames = ops.factory.whole_frames(_sql(ops, state, stage, deadline))
            for key in reversed(owned):
                _need((key, 'DROP_ACK') in frames and
                      frames.get((key, 'IDENTITY')) == [] and
                      frames.get((key, 'OID_ABSENT')) == ['0'] and
                      (key, 'REFUSED') not in frames, 'exact_fixture_drop', state)
                records[key]['state'] = 'DROPPED'
                receipt['owned_dropped'].append(key)
            if reused:
                before = {k: records[k].get('readiness_sha256') for k in reused}
                ops.factory.whole_validate_ready(state['plan'], reused, frames, records)
                _need(all(before[k] is None or
                          before[k] == records[k].get('readiness_sha256')
                          for k in reused), 'reused_fixture_changed', state)
                receipt['reused_preserved'].extend(reused)
        attempt(close_batch)

    t = state['timeout']
    def close_timeout():
        if t['object'] == 'OWNED':
            stage = ops.factory.timeout_stage(t['record'], 'timeout_cleanup')
            _sql_allowed(ops, state, deadline)
            t['object'] = 'DROP_UNKNOWN'
            frames = ops.factory.whole_frames(_sql(ops, state, stage, deadline))
            ops.factory.validate_timeout_frames(t['record'], 'timeout_cleanup', frames)
            t['object'] = 'DROPPED'
    attempt(close_timeout)

    def restore_schema():
        _restore_schema_grant(ops, state, deadline)
        if t['grant'] == 'REVOKE_ACK':
            ops.timeout_factory.permission(
                _timeout_get(ops, state, 'permission_final', deadline),
                t['facts']['permission'], create='f')
            t['grant'] = 'RESTORED'
    attempt(restore_schema)

    db = state['database_create']
    def restore_database():
        if db['state'] == 'OWNED':
            db['state'] = 'REVOKE_UNKNOWN'
            _need(_db_sql(ops, state, 'revoke', deadline) ==
                  'DBCREATE_REVOKE_ACK_V1', 'database_CREATE_REVOKE_ACK', state)
            db['state'] = 'REVOKE_ACK'
            _need(_db_sql(ops, state, 'probe', deadline) == 'f',
                  'database_CREATE_effective_false', state)
            db['state'] = 'RESTORED'
    attempt(restore_database)
    receipt['permission_restored'] = (
        db['state'] in ('UNTOUCHED', 'PREEXISTING_TRUE', 'RESTORED') and
        t['grant'] in ('UNTOUCHED', 'RESTORED'))
    receipt['unresolved'] = [k for k in KEYS if records[k]['state'] not in
                            ('UNTOUCHED', 'MISSING', 'DROPPED', 'READ_ONLY_REUSED',
                             'VERIFIED_ABSENT')]
    receipt['complete'] = (not receipt['errors'] and not receipt['unresolved'] and
                           receipt['permission_restored'] and
                           t['object'] in ('UNTOUCHED', 'DROPPED'))
    _event(ops, state, 'fixture_cleanup', receipt=receipt)
    return receipt
