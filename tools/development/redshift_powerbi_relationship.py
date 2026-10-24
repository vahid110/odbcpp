"""Optional qualification fixture: fixed SQL planning and custody, never execution.

NOT_EXECUTABLE_UNTIL_ROOT_INTEGRATION: canonical Operations must reconstruct
stage_for() itself, enforce its existing deadlines/counters, and durably publish
begin() before dispatch. This module owns no credentials, connections or resources.
Feature assertions belong in it_redshift_powerbi_relationship.cpp, not here.
"""
from copy import deepcopy
import re

SCHEMA = 'odbcpp_fixture'
DATABASE = 'odbcpp_pilot'
OWNER = 'odbcpp_pilot_admin'
PRINCIPAL = 'odbcpp_pilot_test'
PARENT = 'pbi_parent_single_20261022_q007'
FACT = 'pbi_fact_single_20261022_q007'
NAMES = {'parent': PARENT, 'fact': FACT}
PARENT_ROWS = ('1|North', '2|South')
FACT_ROWS = ('101|1|10', '102|1|20', '103|2|7', '104|2|13', '105|2|5')
SHAPES = {
    'parent': ('parent_id|integer|NO|NULL', 'parent_label|character varying|NO|16'),
    'fact': ('fact_id|integer|NO|NULL', 'parent_id|integer|NO|NULL', 'amount|integer|NO|NULL'),
}
KINDS = ('inspect', 'create_parent', 'create_fact', 'verify', 'drop_fact', 'drop_parent')


class Refused(ValueError):
    pass


def need(value, reason):
    if not value:
        raise Refused(reason)


def _positive(value):
    return type(value) is int and 0 < value <= 4294967295


def _q(key):
    return '"' + SCHEMA + '"."' + NAMES[key] + '"'


def new_state():
    return {'anchors': None, 'parent': {'status': 'NEW', 'oid': None},
            'fact': {'status': 'NEW', 'oid': None}, 'pending': None}


def _validate(state):
    need(type(state) is dict and set(state) == {'anchors', 'parent', 'fact', 'pending'}, 'state_schema')
    anchors = state['anchors']
    need(anchors is None or (type(anchors) is list and len(anchors) == 4 and
                             all(_positive(v) for v in anchors)), 'anchor_schema')
    for key in NAMES:
        record = state[key]
        need(type(record) is dict and set(record) == {'status', 'oid'}, 'record_schema')
        need(record['status'] in ('NEW', 'ABSENT', 'CREATE_UNKNOWN', 'OWNED', 'DROP_UNKNOWN', 'DROPPED'), 'record_status')
        need((record['oid'] is None and record['status'] in ('NEW', 'ABSENT', 'CREATE_UNKNOWN')) or
             (_positive(record['oid']) and record['status'] in ('OWNED', 'DROP_UNKNOWN', 'DROPPED')), 'record_oid')


def _identity(key):
    return ("SELECT c.oid,c.relowner,n.oid,n.nspowner,c.relkind FROM pg_catalog.pg_class c "
            "JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='" + SCHEMA +
            "' AND c.relname='" + NAMES[key] + "' LIMIT 2")


def _shape(key):
    return ("SELECT column_name,LOWER(TRIM(data_type)),is_nullable,"
            "COALESCE(character_maximum_length::VARCHAR,'NULL') FROM svv_columns "
            "WHERE table_catalog=current_database() AND table_schema='" + SCHEMA +
            "' AND table_name='" + NAMES[key] + "' ORDER BY ordinal_position LIMIT 4")


def _rows(key):
    return ('SELECT parent_id,parent_label FROM ' + _q(key) + ' ORDER BY parent_id LIMIT 3'
            if key == 'parent' else
            'SELECT fact_id,parent_id,amount FROM ' + _q(key) + ' ORDER BY fact_id LIMIT 6')


def _held(state, key):
    admin, _test, schema, schema_owner = state['anchors']
    return ("EXISTS(SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n "
            "ON n.oid=c.relnamespace WHERE c.oid=" + str(state[key]['oid']) +
            ' AND c.relowner=' + str(admin) + " AND c.relkind='r' AND n.oid=" + str(schema) +
            ' AND n.nspowner=' + str(schema_owner) + " AND n.nspname='" + SCHEMA +
            "' AND c.relname='" + NAMES[key] + "')")


def _shape_guard(key):
    columns = (('parent_id', 'integer', None), ('parent_label', 'character varying', 16)) if key == 'parent' else (
        ('fact_id', 'integer', None), ('parent_id', 'integer', None), ('amount', 'integer', None))
    matches = []
    for ordinal, (name, kind, size) in enumerate(columns, 1):
        length = 'character_maximum_length IS NULL' if size is None else 'character_maximum_length=' + str(size)
        matches.append("(ordinal_position=" + str(ordinal) + " AND column_name='" + name +
                       "' AND LOWER(TRIM(data_type))='" + kind + "' AND is_nullable='NO' AND " +
                       length + ' AND column_default IS NULL)')
    return ('(SELECT COUNT(*)=' + str(len(columns)) + ' AND SUM(CASE WHEN ' + ' OR '.join(matches) +
            " THEN 1 ELSE 0 END)=" + str(len(columns)) + " FROM svv_columns WHERE table_catalog=current_database() "
            "AND table_schema='" + SCHEMA + "' AND table_name='" + NAMES[key] + "')")


def _rows_guard(key):
    predicates = ("(parent_id=1 AND parent_label='North')", "(parent_id=2 AND parent_label='South')") if key == 'parent' else tuple(
        '(fact_id=' + str(i) + ' AND parent_id=' + str(p) + ' AND amount=' + str(a) + ')'
        for i, p, a in ((101, 1, 10), (102, 1, 20), (103, 2, 7), (104, 2, 13), (105, 2, 5)))
    unique = 'parent_id' if key == 'parent' else 'fact_id'
    return ('(SELECT COUNT(*)=' + str(len(predicates)) + ' AND COUNT(DISTINCT ' + unique + ')=' +
            str(len(predicates)) + ' AND SUM(CASE WHEN ' + ' OR '.join(predicates) +
            ' THEN 1 ELSE 0 END)=' + str(len(predicates)) + ' FROM ' + _q(key) + ')')


def stage_for(state, kind):
    """Closed admin stage, no arbitrary SQL/identifier/role/principal parameters.

    `statements` counts SQL, including conditional branches (conservative upper
    bound); canonical Operations must additionally charge its three wrapper SQL
    statements and one helper/login. Original absolute deadline is caller-owned.
    """
    _validate(state)
    need(state['pending'] is None and kind in KINDS, 'pending_or_stage')
    need(not any(state[k]['status'] in ('CREATE_UNKNOWN', 'DROP_UNKNOWN') for k in NAMES), 'unknown_custody')
    if kind == 'inspect':
        need(state == new_state(), 'inspect_once')
    else:
        need(state['anchors'] is not None, 'inspection_required')
    if kind.startswith('create_'):
        key = kind[7:]
        need(state[key]['status'] == 'ABSENT', 'create_requires_verified_absence')
        need(key != 'fact' or state['parent']['status'] == 'OWNED', 'parent_required')
    if kind == 'verify':
        need(all(state[k]['status'] == 'OWNED' for k in NAMES), 'verify_requires_owned_pair')
    if kind.startswith('drop_'):
        key = kind[5:]
        need(state[key]['status'] == 'OWNED', 'drop_requires_owned')
        need(key != 'parent' or state['fact']['status'] in ('ABSENT', 'DROPPED'), 'reverse_dependency_order')
    lines = []
    count = 0
    guard_count = 0

    def sql(query, label=None):
        nonlocal count
        if label is not None:
            lines.append('\\echo RSPBI1|' + label)
        lines.append(query + ';')
        count += 1

    def guard(expression):
        nonlocal guard_count
        sql('SELECT CASE WHEN ' + expression + ' THEN 1 ELSE 0 END AS pbi_held')
        lines[-1] = lines[-1][:-1] + '\n\\gset\n\\if :pbi_held'
        guard_count += 1

    session = ("SELECT current_database(),TRIM(current_user),(SELECT usesysid FROM pg_user WHERE usename='" + OWNER +
               "'),(SELECT usesysid FROM pg_user WHERE usename='" + PRINCIPAL +
               "'),n.oid,n.nspowner FROM pg_catalog.pg_namespace n WHERE n.nspname='" + SCHEMA + "' LIMIT 2")
    sql(session, 'SESSION')
    if kind == 'inspect':
        for key in NAMES:
            sql(_identity(key), key.upper() + '_IDENTITY')
    else:
        admin, test, schema, schema_owner = state['anchors']
        anchor_guard = ("current_database()='" + DATABASE + "' AND TRIM(current_user)='" + OWNER +
                        "' AND EXISTS(SELECT 1 FROM pg_user WHERE usename='" + OWNER + "' AND usesysid=" + str(admin) +
                        " AND usesuper) AND EXISTS(SELECT 1 FROM pg_user WHERE usename='" + PRINCIPAL +
                        "' AND usesysid=" + str(test) + ") AND EXISTS(SELECT 1 FROM pg_catalog.pg_namespace WHERE nspname='" +
                        SCHEMA + "' AND oid=" + str(schema) + ' AND nspowner=' + str(schema_owner) + ')')
        guard(anchor_guard)
        if kind.startswith('create_'):
            key = kind[7:]
            guard('NOT EXISTS(' + _identity(key).removesuffix(' LIMIT 2').replace('SELECT c.oid,c.relowner,n.oid,n.nspowner,c.relkind', 'SELECT 1') + ')')
            if key == 'fact':
                # Redshift cannot combine leader catalog, SVV shape and user
                # rows in one query. Keep every predicate in its own gate.
                guard(_held(state, 'parent'))
                guard(_shape_guard('parent'))
                guard(_rows_guard('parent'))
            columns = ('parent_id INTEGER NOT NULL,parent_label VARCHAR(16) NOT NULL,CONSTRAINT pbi_parent_single_q007_pk PRIMARY KEY(parent_id)' if key == 'parent' else
                       'fact_id INTEGER NOT NULL,parent_id INTEGER NOT NULL,amount INTEGER NOT NULL,CONSTRAINT pbi_fact_single_q007_pk PRIMARY KEY(fact_id),CONSTRAINT pbi_fact_single_q007_fk FOREIGN KEY(parent_id) REFERENCES ' + _q('parent') + '(parent_id)')
            sql('CREATE TABLE ' + _q(key) + '(' + columns + ')')
            rows = "(1,'North'),(2,'South')" if key == 'parent' else '(101,1,10),(102,1,20),(103,2,7),(104,2,13),(105,2,5)'
            sql('INSERT INTO ' + _q(key) + ' VALUES ' + rows)
            sql('GRANT SELECT ON TABLE ' + _q(key) + ' TO "' + PRINCIPAL + '"')
            lines.append('\\echo RSPBI1|CREATE_ACK')
            sql(_identity(key), 'IDENTITY')
            sql(_shape(key), 'SHAPE')
            sql(_rows(key), 'ROWS')
        elif kind == 'verify':
            for key in NAMES:
                sql(_identity(key), key.upper() + '_IDENTITY')
                sql(_shape(key), key.upper() + '_SHAPE')
                sql(_rows(key), key.upper() + '_ROWS')
        else:
            key = kind[5:]
            guard(_held(state, key))
            guard(_shape_guard(key))
            guard(_rows_guard(key))
            guard('(SELECT COUNT(*)=0 FROM svv_transactions WHERE relation=' + str(state[key]['oid']) +
                  " AND lockable_object_type='relation' AND txn_db='" + DATABASE + "')")
            sql('DROP TABLE ' + _q(key) + ' RESTRICT')
            lines.append('\\echo RSPBI1|DROP_ACK')
            sql(_identity(key), 'IDENTITY')
            sql('SELECT COUNT(*) FROM pg_catalog.pg_class WHERE oid=' + str(state[key]['oid']), 'OID_ABSENT')
            sql("SELECT oid,nspowner FROM pg_catalog.pg_namespace WHERE nspname='" + SCHEMA + "' LIMIT 2", 'SCHEMA')
        # All gates enclose the stage effects/ACK; each false branch emits one
        # refusal. Close every nested gate in reverse order, including anchor.
        for _ in range(guard_count):
            lines.append('\\else\n\\echo RSPBI1|REFUSED\n\\endif')
    query = '\n'.join(lines) + '\n'
    need(len(query.encode('utf-8')) <= 16384 and count <= 12, 'stage_bounds')
    return {'stage': 'powerbi_relationship_' + kind, 'keys': ('POWERBI_SINGLE_KEY',),
            'role': 'admin', 'query': query, 'statements': count}


def begin(state, stage):
    """Return intent to persist BEFORE one dispatch. Never retry unknown SQL."""
    need(type(stage) is dict, 'stage_schema')
    kind = stage.get('stage', '').removeprefix('powerbi_relationship_')
    need(stage == stage_for(state, kind), 'stage_changed')
    result = deepcopy(state)
    result['pending'] = {'before': deepcopy(state), 'stage': deepcopy(stage)}
    if kind.startswith('create_'):
        result[kind[7:]]['status'] = 'CREATE_UNKNOWN'
    elif kind.startswith('drop_'):
        result[kind[5:]]['status'] = 'DROP_UNKNOWN'
    return result


def parse_frames(payload):
    need(type(payload) is str and len(payload.encode('utf-8')) <= 16384 and '\x00' not in payload, 'frame_bounds')
    frames, label = {}, None
    for line in payload.splitlines():
        if line.startswith('RSPBI1|'):
            label = line[7:]
            need(re.fullmatch('[A-Z_]+', label) and label not in frames, 'duplicate_or_invalid_frame')
            frames[label] = []
        else:
            need(label is not None and line and len(frames[label]) < 8, 'unframed_or_excess_rows')
            frames[label].append(line)
    return frames


def complete(state, payload):
    """Accept only exact successful native psql frames. Failure leaves intent unknown.

    Caller strips its separately validated RSD1 wrapper frame before calling.
    No unknown-create adoption, cleanup retry, OID discovery or feature parsing.
    """
    _validate(state)
    pending = state['pending']
    need(type(pending) is dict and set(pending) == {'before', 'stage'}, 'completion_without_intent')
    before, stage = pending['before'], pending['stage']
    need(state == begin(before, stage), 'intent_changed')
    kind = stage['stage'].removeprefix('powerbi_relationship_')
    frames = parse_frames(payload)
    need('REFUSED' not in frames and len(frames.get('SESSION', [])) == 1, 'stage_refused_or_session')
    session = frames['SESSION'][0].split('|')
    need(len(session) == 6 and session[:2] == [DATABASE, OWNER] and
         all(re.fullmatch('[1-9][0-9]{0,9}', v) and _positive(int(v)) for v in session[2:]), 'session_identity')
    anchors = [int(v) for v in session[2:]]
    need(anchors[0] != anchors[1], 'separate_existing_principals')
    need(before['anchors'] is None or anchors == before['anchors'], 'anchor_changed')
    result = deepcopy(before)
    result['anchors'] = anchors

    def identity(key, label):
        values = frames.get(label)
        need(type(values) is list and len(values) == 1, 'identity_missing')
        fields = values[0].split('|')
        need(len(fields) == 5 and fields[-1] == 'r' and all(re.fullmatch('[1-9][0-9]{0,9}', x) and _positive(int(x)) for x in fields[:4]), 'identity_fields')
        oid, owner, schema, schema_owner = map(int, fields[:4])
        need([owner, schema, schema_owner] == [anchors[0], anchors[2], anchors[3]], 'identity_custody')
        need(before[key]['oid'] is None or before[key]['oid'] == oid, 'oid_changed')
        return oid

    if kind == 'inspect':
        need(set(frames) == {'SESSION', 'PARENT_IDENTITY', 'FACT_IDENTITY'} and
             frames['PARENT_IDENTITY'] == [] and frames['FACT_IDENTITY'] == [], 'preexisting_collision')
        for key in NAMES:
            result[key]['status'] = 'ABSENT'
    elif kind.startswith('create_'):
        key = kind[7:]
        need(set(frames) == {'SESSION', 'CREATE_ACK', 'IDENTITY', 'SHAPE', 'ROWS'} and frames['CREATE_ACK'] == [], 'actual_create_ack_required')
        oid = identity(key, 'IDENTITY')
        need(oid != before['parent' if key == 'fact' else 'fact']['oid'], 'distinct_object_oid')
        need(frames['SHAPE'] == list(SHAPES[key]) and frames['ROWS'] == list(PARENT_ROWS if key == 'parent' else FACT_ROWS), 'created_shape_rows')
        result[key] = {'status': 'OWNED', 'oid': oid}
    elif kind == 'verify':
        need(set(frames) == {'SESSION', 'PARENT_IDENTITY', 'PARENT_SHAPE', 'PARENT_ROWS', 'FACT_IDENTITY', 'FACT_SHAPE', 'FACT_ROWS'}, 'verify_frames')
        for key in NAMES:
            identity(key, key.upper() + '_IDENTITY')
            need(frames[key.upper() + '_SHAPE'] == list(SHAPES[key]) and frames[key.upper() + '_ROWS'] == list(PARENT_ROWS if key == 'parent' else FACT_ROWS), 'current_shape_rows')
    else:
        key = kind[5:]
        need(set(frames) == {'SESSION', 'DROP_ACK', 'IDENTITY', 'OID_ABSENT', 'SCHEMA'} and
             frames['DROP_ACK'] == [] and frames['IDENTITY'] == [] and frames['OID_ABSENT'] == ['0'] and
             frames['SCHEMA'] == [str(anchors[2]) + '|' + str(anchors[3])], 'drop_ack_absence_schema')
        result[key]['status'] = 'DROPPED'
    return result


def public_plan():
    return {'schema_version': 1, 'readiness': 'NOT_EXECUTABLE_UNTIL_ROOT_INTEGRATION',
            'database': DATABASE, 'schema': SCHEMA, 'owner': OWNER, 'principal': PRINCIPAL,
            'tables': deepcopy(NAMES), 'stages': list(KINDS), 'helpers': 6,
            'statement_upper_bound_including_operations_wrappers': 70,
            'login_upper_bound': 6, 'inclusive_statements_plus_logins': 76,
            'stage_statement_bounds': {'inspect': 3, 'create_parent': 9, 'create_fact': 12,
                                       'verify': 8, 'drop_fact': 10, 'drop_parent': 10},
            'existing_absolute_deadline_required': True, 'statement_timeout_ms_max': 15000,
            'shared_schema': 'RETAIN', 'drop': 'RESTRICT; fact before parent; no unknown adoption',
            'marker': {'ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP': '1'},
            'connection_environment': ['ODBCPP_TEST_DSN'],
            'required_public_variables': ['ODBCPP_REDSHIFT_POWERBI_PARENT_OID',
                                          'ODBCPP_REDSHIFT_POWERBI_FACT_OID',
                                          'ODBCPP_REDSHIFT_PACKAGE_INSTALLED_DRIVER'],
            'native_cases': ['PowerBiRelationshipNative.ColumnsAndSingleKeyMetadata',
                             'PowerBiRelationshipNative.RowsFilterAndReset'],
            'powerbi_expected': {'all': {'count': 5, 'sum': 55}, 'North': {'count': 2, 'sum': 30},
                                 'South': {'count': 3, 'sum': 25}},
            'no_automatic_authority_or_runtime_extension': True}
