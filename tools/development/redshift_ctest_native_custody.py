"""Two bounded catalog censuses around CTest; never delete native objects.

Only persistent names authored by the existing column/routine/read-only tests
are covered. Actual process/session cleanup is a prerequisite for the second
census. Feature assertions and test outcomes remain entirely in GoogleTest.
"""
import copy
import csv
import io
import re


READONLY_TABLE = 'odbcpp_readonly_2026101500000001'
READONLY_SCHEMA = READONLY_TABLE
HEADERS = ('schema_name', 'object_name', 'kind', 'oid', 'owner_oid',
           'schema_oid', 'schema_owner_oid', 'detail')
MAX_ROWS = 512
MAX_NEW_SCHEMAS = 13  # Existing column/routine namespaces plus the read-only fixture.
OWNER = 101
SCHEMA_RE = re.compile(r'(?:rs5_cols_[0-9]{1,20}_雪|'
                       r'rs5_routine_[0-9]{1,20}_(?:a_雪|literal_%))\Z')


class CensusError(RuntimeError):
    def __init__(self, code, state):
        super().__init__(code)
        self.state = state


def plan():
    return {'helpers': 2, 'SQL': 8, 'logins': 2, 'inclusive': 10,
            'body_SQL_per_helper': 1, 'max_rows': MAX_ROWS,
            'max_new_schemas': MAX_NEW_SCHEMAS, 'automatic_DROP': False}


def stage():
    # All branches stay in the PG-catalog domain. No user-table COUNT, SVV
    # lock relation or compute-only operation is mixed into this statement.
    # Serialize booleans with CASE, not a boolean-to-varchar cast. Routine
    # custody compares its OID/owner/name and numeric argument count; GoogleTest
    # owns argument-type assertions, so no oidvector-to-varchar cast is needed.
    domain = ("(n.nspname ~ '^rs5_cols_[0-9]{1,20}_雪$' OR "
              "n.nspname ~ '^rs5_routine_[0-9]{1,20}_(a_雪|literal_%)$' OR "
              "n.nspname='" + READONLY_SCHEMA + "')")
    relation = ('(' + domain + " OR (n.nspname='public' AND c.relname='" +
                READONLY_TABLE + "'))")
    query = (
        'SELECT * FROM (\n'
        "SELECT n.nspname::varchar AS schema_name,n.nspname::varchar AS object_name,"
        "'n'::varchar AS kind,n.oid::bigint AS oid,n.nspowner::bigint AS owner_oid,"
        "n.oid::bigint AS schema_oid,n.nspowner::bigint AS schema_owner_oid,"
        "''::varchar AS detail FROM pg_catalog.pg_namespace n WHERE " + domain +
        '\nUNION ALL\n'
        'SELECT n.nspname::varchar,c.relname::varchar,c.relkind::varchar,'
        'c.oid::bigint,c.relowner::bigint,n.oid::bigint,n.nspowner::bigint,'
        "''::varchar FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n "
        'ON n.oid=c.relnamespace WHERE ' + relation +
        '\nUNION ALL\n'
        "SELECT n.nspname::varchar,c.relname::varchar,'a'::varchar,c.oid::bigint,"
        'c.relowner::bigint,n.oid::bigint,n.nspowner::bigint,'
        "(a.attnum::varchar||'|'||a.attname::varchar||'|'||a.atttypid::varchar||"
        "'|'||a.atttypmod::varchar||'|'||CASE WHEN a.attnotnull THEN 't' ELSE 'f' END||'|'||"
        "CASE WHEN a.atthasdef THEN 't' ELSE 'f' END)::varchar FROM pg_catalog.pg_attribute a "
        'JOIN pg_catalog.pg_class c ON c.oid=a.attrelid '
        'JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace '
        'WHERE a.attnum>0 AND NOT a.attisdropped AND ' + relation +
        '\nUNION ALL\n'
        "SELECT n.nspname::varchar,p.proname::varchar,'p'::varchar,p.prooid::bigint,"
        'p.proowner::bigint,n.oid::bigint,n.nspowner::bigint,'
        "(p.pronargs::varchar||'|'||p.prokind::varchar)::varchar "
        'FROM pg_proc_info p JOIN pg_catalog.pg_namespace n '
        'ON n.oid=p.pronamespace WHERE ' + domain +
        ') AS native_census ORDER BY schema_name,object_name,kind,oid,detail '
        'LIMIT 513;\n')
    return {'query': query, 'statements': 1, 'role': 'admin', 'keys': (),
            'stage': 'native_persistent_census', 'headers': HEADERS,
            'maxrows': MAX_ROWS}


def _need(value, code, state=None):
    if not value:
        raise CensusError(code, state)


def _positive(value):
    return (type(value) is str and re.fullmatch(r'[1-9][0-9]{0,9}', value)
            is not None and int(value) <= 4294967295)


def parse(raw):
    _need(type(raw) is str and len(raw.encode('utf-8')) <= 65536 and
          '\x00' not in raw, 'census_output_bound')
    rows = list(csv.reader(io.StringIO(raw), strict=True))
    _need(rows and tuple(rows[0]) == HEADERS and len(rows) - 1 <= MAX_ROWS,
          'census_header_or_row_bound')
    result = []
    for row in rows[1:]:
        _need(len(row) == len(HEADERS), 'census_row_width')
        item = dict(zip(HEADERS, row))
        schema, name = item['schema_name'], item['object_name']
        _need(SCHEMA_RE.fullmatch(schema) is not None or schema == READONLY_SCHEMA or
              (schema == 'public' and name == READONLY_TABLE),
              'census_exact_source_namespace')
        _need(item['kind'] in ('n', 'r', 'v', 'p', 'a') and
              all(_positive(item[k]) for k in
                  ('oid', 'owner_oid', 'schema_oid', 'schema_owner_oid')) and
              0 < len(name.encode('utf-8')) <= 127 and
              len(item['detail'].encode('utf-8')) <= 512,
              'census_identity_or_shape_bound')
        result.append(tuple(row))
    _need(len(result) == len(set(result)), 'census_duplicate_tuple')
    return sorted(result)


def _observe(ops, deadline, state):
    _need(ops.now() < deadline and getattr(ops, 'readers_known', False) is True and
          not getattr(ops, 'sql_unknown', False) and
          not getattr(ops, 'sql_fenced', False), 'census_SQL_fenced', state)
    raw = ops.sql(stage(), deadline)
    _need(ops.now() < deadline, 'census_original_deadline', state)
    rows = parse(raw)
    ops.record({'kind': 'native_persistent_census', 'rows': copy.deepcopy(rows),
                'raw': raw})
    return rows


def before(ops, deadline):
    state = {'baseline': None, 'after': None, 'complete': False}
    if getattr(ops, 'resource_kind', 'provisioned') == 'serverless':
        expected = ops.timeout_factory.EXPECTED_PRINCIPAL
        _need(type(expected) is dict and expected.get('database') == 'odbcpp_pilot' and
              expected.get('owner') == 'odbcpp_pilot_test' and
              _positive(expected.get('owner_oid')), 'census_current_owner_unbound', state)
        state['owner_oid'] = int(expected['owner_oid'])
    ops.native_custody_state = state
    state['baseline'] = _observe(ops, deadline, state)
    return state


def after(ops, state, deadline):
    _need(type(state) is dict and type(state.get('baseline')) is list,
          'census_actual_baseline_required', state)
    _need(getattr(ops, 'principals_quiescent', False) is True and
          getattr(ops, 'readers_known', False) is True,
          'census_requires_reaps_and_principal_zero', state)
    state['after'] = _observe(ops, deadline, state)
    baseline = set(map(tuple, state['baseline']))
    current = set(map(tuple, state['after']))
    residual = sorted(current - baseline)
    removed = sorted(baseline - current)
    schemas = {r[0] for r in residual if r[0] != 'public'}
    # A changed owner or unrecognized child is debt, never deletion authority.
    owner = state.get('owner_oid', OWNER)
    if getattr(ops, 'resource_kind', 'provisioned') == 'serverless':
        expected = ops.timeout_factory.EXPECTED_PRINCIPAL
        _need(type(expected) is dict and owner == int(expected['owner_oid']),
              'census_current_owner_changed', state)
    owned = all(int(r[4]) == owner and
                (r[0] == 'public' or int(r[6]) == owner) for r in residual)
    receipt = {'complete': not residual and not removed,
               'residual': residual, 'baseline_removed_or_changed': removed,
               'within_source_schema_count': len(schemas) <= MAX_NEW_SCHEMAS,
               'all_new_objects_ordinary_owned': owned,
               'automatic_DROP': False}
    state['complete'] = receipt['complete']
    ops.record({'kind': 'native_persistent_custody_result', 'receipt': receipt})
    return receipt
