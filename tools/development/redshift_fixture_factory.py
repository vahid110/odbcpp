"""Closed fixture/profile SQL and input formats extracted from accepted source074. No execution or private Python loading. Historical scheduling, publication, consumed admissions and absolute source paths excluded."""
import re
import json
import hashlib
from pathlib import Path
LIMIT_KEYS = frozenset(('MAXRESPONSEBYTES', 'MAXRESPONSEMESSAGES', 'MAXSTARTUPRESPONSEBYTES', 'MAXSTARTUPRESPONSEMESSAGES', 'MAXROWS', 'MAXCELLS', 'MAXCOLUMNS', 'MAXRESULTS', 'MAXMETADATAENTRIES', 'MAXCOLUMNNAMEBYTES', 'MAXMETADATANAMEBYTES', 'MAXDIAGNOSTICBYTES', 'MAXSQLBYTES', 'MAXPARAMETERS', 'MAXPARAMETERBYTES', 'MAXPARAMETERTOTALBYTES', 'MAXCONNECTIONFIELDBYTES', 'MAXREQUESTWIREBYTES', 'MAXSTARTUPWIREBYTES', 'MAXAUTHWIREBYTES'))

CANONICAL_LIMITS = {'MAXRESPONSEBYTES': 67108864, 'MAXRESPONSEMESSAGES': 100000, 'MAXSTARTUPRESPONSEBYTES': 1048576, 'MAXSTARTUPRESPONSEMESSAGES': 10000, 'MAXROWS': 1000000, 'MAXCELLS': 4000000, 'MAXCOLUMNS': 4096, 'MAXRESULTS': 1, 'MAXMETADATAENTRIES': 65536, 'MAXCOLUMNNAMEBYTES': 1024, 'MAXMETADATANAMEBYTES': 1048576, 'MAXDIAGNOSTICBYTES': 16384, 'MAXSQLBYTES': 1048576, 'MAXPARAMETERS': 65535, 'MAXPARAMETERBYTES': 16777216, 'MAXPARAMETERTOTALBYTES': 67108864, 'MAXCONNECTIONFIELDBYTES': 65536, 'MAXREQUESTWIREBYTES': 134217728, 'MAXSTARTUPWIREBYTES': 1048576, 'MAXAUTHWIREBYTES': 1048576}

class FactoryRefused(ValueError):
    pass

def need(value, reason):
    if not value:
        raise FactoryRefused(reason) from None

def field(value):
    need(type(value) is str and 0 < len(value.encode('utf-8')) <= 16384 and (not any((ch in value for ch in '\x00\r\n'))), 'invalid_field')
    return '{' + value.replace('}', '}}') + '}'

def build_connection(config, limits, emitted_resource_omissions=()):
    need(type(config) is dict and set(config) == {'admin_password', 'admin_user', 'ca', 'database', 'host', 'port', 'test_password', 'test_user'}, 'invalid_config')
    need(config['database'] == 'odbcpp_pilot' and config['test_user'] == 'odbcpp_pilot_test' and (config['admin_user'] == 'odbcpp_pilot_admin') and (config['port'] == 5439), 'scope_mismatch')
    need(type(config['host']) is str and re.fullmatch('[A-Za-z0-9][A-Za-z0-9.-]{0,252}', config['host'], re.ASCII) and (type(config['ca']) is str) and Path(config['ca']).is_absolute(), 'invalid_endpoint')
    need(type(limits) is dict and set(limits) == LIMIT_KEYS and all((type(v) is int and 1 <= v <= 2 ** 63 - 1 for v in limits.values())), 'limit_inventory')
    need(type(emitted_resource_omissions) is tuple and emitted_resource_omissions in ((), ('MAXROWS', 'MAXCELLS', 'MAXRESPONSEBYTES', 'MAXRESPONSEMESSAGES')), 'emitted_resource_omissions')
    entries = [('SERVER', config['host']), ('PORT', '5439'), ('DATABASE', config['database']), ('UID', config['test_user']), ('PWD', config['test_password']), ('SSL', '1'), ('SSLCAFILE', config['ca']), ('TRANSPORTMODE', 'Sync')]
    connection = ''.join((k + '=' + field(v) + ';' for k, v in entries))
    connection += ''.join((k + '=' + str(limits[k]) + ';' for k in sorted(LIMIT_KEYS) if k not in emitted_resource_omissions))
    need(len(connection.encode('utf-8')) <= 16384, 'connection_bound')
    return connection

def limits_for(results):
    need(type(results) is int and results in (1, 3), 'result_profile')
    return {**CANONICAL_LIMITS, 'MAXRESULTS': results}

FIXTURE_SCHEMA = 'odbcpp_fixture'

FIXTURE_TABLE = 'odbcpp_catalog_types_20261015_c01'

FIXTURE_QNAME = '"odbcpp_fixture"."odbcpp_catalog_types_20261015_c01"'

FIXTURE_CONTRACT = {'schema': 'odbcpp_fixture', 'table': 'odbcpp_catalog_types_20261015_c01', 'owner': 'odbcpp_pilot_admin', 'columns': [['c_varbyte', 'VARBYTE(16)'], ['c_super', 'SUPER'], ['c_geometry', 'GEOMETRY'], ['c_geography', 'GEOGRAPHY']], 'rows': 0, 'drop': 'RESTRICT'}

def validate_targets(targets):
    need(type(targets) is dict and set(targets) == {'varbyte', 'super', 'spatial'}, 'fixture_inventory')
    need(all((type(v) is dict and v == {'schema': FIXTURE_SCHEMA, 'table': FIXTURE_TABLE} for v in targets.values())), 'fixture_exact_identifier')
    return targets

def dsn_documents(config, limits=None, driver=None, driver_manager=None):
    limits = limits_for(1) if limits is None else limits
    build_connection(config, limits)
    fields = {'DRIVER': 'ODBCPP Current44', 'SERVER': config['host'], 'PORT': '5439', 'DATABASE': config['database'], 'UID': config['test_user'], 'PWD': config['test_password'], 'SSL': '1', 'SSLCAFILE': config['ca'], 'TRANSPORTMODE': 'Sync', 'LoginTimeout': '5', 'QueryTimeout': '5'}
    fields.update({k: str(v) for k, v in limits.items()})
    for value in fields.values():
        need(type(value) is str and value == value.strip() and (0 < len(value) <= 16384) and (not any((c in value for c in '\r\n\x00'))), 'dsn_field')
    text = '[RedshiftProd]\n' + ''.join((k + '=' + v + '\n' for k, v in fields.items()))
    need(len(text.encode('utf-8')) <= 32768, 'dsn_bytes')
    driver = '/private/tmp/odbcpp-current44-checkpoint-native001/build/libodbcpp.1.0.0.dylib' if driver is None else driver
    need(type(driver) is str and Path(driver).is_absolute() and ('..' not in driver.split('/')) and (not any((x in driver for x in '\r\n\x00'))), 'dsn_driver')
    registration = '[ODBCPP Current44]\nDriver=' + driver + '\n'
    if driver_manager == 'iODBC':
        # iODBC converts SQLWCHAR text by the driver's declared width.
        registration += 'DriverUnicodeType=UCS4\n'
    return (text, registration)

def _pin(value):
    need(type(value) is dict and set(value) in ({'path', 'sha256'}, {'path', 'sha256', 'bytes'}) and (type(value['path']) is str) and Path(value['path']).is_absolute() and ('..' not in value['path'].split('/')) and (type(value['sha256']) is str) and (re.fullmatch('[0-9a-f]{64}', value['sha256']) is not None), 'evidence_pin')
    if 'bytes' in value:
        need(type(value['bytes']) is int and 0 <= value['bytes'] <= 16 * 1024 * 1024, 'evidence_size')

def validate_profile(profile):
    base = {'profile_name', 'limits', 'connection_environment', 'uses_private_dsn', 'variables', 'review', 'private_input'}
    need(type(profile) is dict and set(profile) - {'driver_manager'} in (base, base | {'emitted_resource_omissions'}), 'profile_schema')
    _pin(profile['review'])
    need(type(profile['profile_name']) is str and 0 < len(profile['profile_name']) <= 256, 'profile_name')
    limits = profile['limits']
    need(type(limits) is dict and set(limits) == LIMIT_KEYS and all((type(v) is int and 1 <= v <= 2 ** 63 - 1 for v in limits.values())), 'finite_source_profile')
    names = profile['connection_environment']
    need(type(names) is list and len(names) == len(set(names)) and all((type(n) is str and re.fullmatch('ODBCPP_[A-Z0-9_]+', n) is not None for n in names)), 'connection_environment')
    need(type(profile['uses_private_dsn']) is bool and type(profile['variables']) is dict and (len(profile['variables']) <= 32), 'profile_environment')
    for k, v in profile['variables'].items():
        need(type(k) is str and re.fullmatch('ODBCPP_[A-Z0-9_]+', k) is not None and (not any((x in k for x in ('PASSWORD', 'SECRET', 'TOKEN', 'CREDENTIAL')))) and (type(v) is str) and (0 < len(v) <= 4096) and (not any((c in v for c in '\x00\r\n'))) and (k not in names), 'profile_public_variable')
    private = profile['private_input']
    need(type(private) is dict and set(private) == {'kind', 'driver', 'password_rejection'}, 'private_input_schema')
    kind = private['kind']
    need(kind in ('CORE_ENV', 'OWN_DSN', 'OWN_DM_FILE', 'VENDOR_DM_FILE') and type(private['password_rejection']) is bool, 'private_input_kind')
    need(profile['uses_private_dsn'] == (kind == 'OWN_DSN') and (not private['password_rejection'] or kind in ('CORE_ENV', 'OWN_DSN')), 'private_input_consistency')
    omissions = profile.get('emitted_resource_omissions', [])
    materialized = 'emitted_resource_omissions' in profile
    if materialized:
        need(kind == 'CORE_ENV' and type(omissions) is list and (omissions == ['MAXROWS', 'MAXCELLS', 'MAXRESPONSEBYTES', 'MAXRESPONSEMESSAGES']), 'emitted_resource_omissions')
        need(all((limits[k] == v for k, v in {'MAXROWS': 1000000, 'MAXCELLS': 4000000, 'MAXRESPONSEBYTES': 67108864, 'MAXRESPONSEMESSAGES': 100000}.items())), 'materialized_resolved_SDK_defaults')
    driver = private['driver']
    manager = profile.get('driver_manager')
    if manager is not None:
        # The owned driver loaded through an installed Driver Manager: by
        # DRIVER= path for CORE_ENV, by the private DSN for OWN_DSN.
        need(kind in ('CORE_ENV', 'OWN_DSN') and manager in ('iODBC', 'unixODBC') and (not private['password_rejection']), 'driver_manager_profile')
        need(type(driver) is dict and set(driver) == {'path', 'sha256', 'wchar_bytes', 'kind'} and driver['kind'] == 'OWN' and (driver['wchar_bytes'] == (4 if manager == 'iODBC' else 2)), 'driver_manager_driver')
        _pin({'path': driver['path'], 'sha256': driver['sha256']})
    elif kind == 'CORE_ENV':
        need(driver is None, 'CORE_has_no_DM_driver')
    else:
        need(type(driver) is dict and set(driver) == {'path', 'sha256', 'wchar_bytes', 'kind'}, 'profile_driver_schema')
        _pin({'path': driver['path'], 'sha256': driver['sha256']})
        need(type(driver['wchar_bytes']) is int and driver['wchar_bytes'] in (2, 4) and (driver['kind'] == ('VENDOR' if kind == 'VENDOR_DM_FILE' else 'OWN')), 'profile_driver_identity')
        if kind == 'VENDOR_DM_FILE':
            need(driver['wchar_bytes'] == 4, 'unsupported_vendor_width')
    reserved = {'ODBCPP_REDSHIFT_PACKAGE_CONNECTION_FILE', 'ODBCPP_REDSHIFT_PACKAGE_INSTALLED_DRIVER', 'ODBCPP_REDSHIFT_PACKAGE_EXPECTED_HOST', 'ODBCPP_REDSHIFT_PACKAGE_EXPECTED_CA_FILE', 'ODBCPP_REDSHIFT_PACKAGE_EXPECTED_DATABASE', 'ODBCPP_REDSHIFT_PACKAGE_EXPECTED_PRINCIPAL', 'ODBCPP_REDSHIFT_AUTH_INVALID_CONNECTION', 'ODBCPP_REDSHIFT_AUTH_EXPECTED_USER'}
    need(not reserved.intersection(profile['variables']) and (not reserved.intersection(names)), 'private_values_not_public_variables')
    need(kind not in ('CORE_ENV', 'OWN_DSN') or bool(names), 'connection_environment')
    return profile

def iodbc_registration_documents(profile):
    validate_profile(profile)
    private = profile['private_input']
    driver = private['driver']
    need(private['kind'] in ('OWN_DM_FILE', 'VENDOR_DM_FILE') and driver['wchar_bytes'] == 4, 'iodbc_registration_profile')
    path = driver['path']
    need(path.isascii() and path.startswith('/') and (not any((c in path for c in '\x00\r\n[];={}'))) and (str(Path(path).resolve(strict=True)) == path) and Path(path).is_file(), 'iodbc_registration_path')
    return ('', '[' + path + ']\nDriver=' + path + '\nDriverUnicodeType=UCS4\n')

def profile_connection_fields(config, profile, *, password_rejection=False):
    validate_profile(profile)
    build_connection(config, profile['limits'])
    need(type(password_rejection) is bool and (not password_rejection or profile['private_input']['password_rejection']), 'unapproved_password_variant')
    password = config['test_password']
    if password_rejection:
        need(type(password) is str and bool(password), 'password_variant_input')
        password = ('y' if password.startswith('x') else 'x') + password[1:]
        need(password != config['test_password'], 'password_variant_not_distinct')
    values = {'SERVER': config['host'], 'PORT': '5439', 'DATABASE': config['database'], 'UID': config['test_user'], 'PWD': password}
    kind = profile['private_input']['kind']
    if kind in ('OWN_DM_FILE', 'VENDOR_DM_FILE'):
        values = {'DRIVER': profile['private_input']['driver']['path'], **values}
        values.update({'SSLMODE': 'verify-full', 'TRUSTSTORE': config['ca']} if kind == 'VENDOR_DM_FILE' else {'SSL': '1', 'SSLCAFILE': config['ca']})
        if kind == 'VENDOR_DM_FILE' and profile['profile_name'] == 'REDSHIFT/official-vendor/iODBC4-diagnostic':
            values.update({'LOGLEVEL': '0', 'CONNECTIONRETRYCOUNT': '0', 'READONLY': '0', 'USEDECLAREFETCH': '0'})
    else:
        values.update({'SSL': '1', 'SSLCAFILE': config['ca'], 'TRANSPORTMODE': 'Sync'})
        values.update({k: str(v) for k, v in profile['limits'].items() if k not in profile.get('emitted_resource_omissions', [])})
    return values

def build_profile_connection(config, profile, *, password_rejection=False):
    values = profile_connection_fields(config, profile, password_rejection=password_rejection)
    if profile['private_input']['kind'] in ('CORE_ENV', 'OWN_DSN'):
        adapted = dict(config)
        adapted['test_password'] = values['PWD']
        connection = build_connection(adapted, profile['limits'], tuple(profile.get('emitted_resource_omissions', [])))
        if profile.get('driver_manager') is not None:
            connection = 'DRIVER=' + field(profile['private_input']['driver']['path']) + ';' + connection
        return connection
    text = ''.join((k + '=' + field(v) + ';' for k, v in values.items()))
    maximum = 4096 if profile['private_input']['kind'] in ('OWN_DM_FILE', 'VENDOR_DM_FILE') else 16384
    need(0 < len(text.encode('utf-8')) <= maximum and (not any((c in text for c in '\x00\r\n'))), 'private_connection_size')
    return text

WHOLE_FIXTURE_IDS = ('SHARED_FIXTURE_SCHEMA', 'CONFIGURED_FRESH_ROWS', 'CONFIGURED_PILOT_ROWS_REUSE', 'COMPOSITE_PARENT', 'COMPOSITE_CHILD', 'NO_KEY', 'QUOTED_KEY', 'POSITIVE_ROUTINE', 'UNICODE_SCHEMA', 'UNICODE_TWO_ROWS', 'EMPTY_FOUR_TYPES')

WHOLE_CHECK_IDS = ('ABSENT_ROUTINE', 'ABSENT_PK', 'ABSENT_SCHEMA')

def required_fixtures(plan, target, case):
    need(plan['sha256'] == WHOLE_FIXTURE_SHA, 'whole_fixture_plan')
    rows = [r for r in plan['cases'] if r['target'] == target and r['selector'] == case]
    need(len(rows) <= 1, 'whole_fixture_case_duplicate')
    if not rows:
        return ('ABSENT_SCHEMA',) if target == 'it_metadata_real' and case == 'MetadataIntegrationTest.MissingSchemaUsesOdbcSqlstate' else ()
    result = []

    def add(key):
        if key == 'CONFIGURED_FRESH_ROWS_OR_READ_ONLY_PILOT_ROWS':
            key = 'CONFIGURED_PILOT_ROWS_REUSE'
        if key.startswith('SELF_CREATED_') or key == 'EXISTING_TWO_DB_PUBLIC_SCHEMA_METADATA_ONLY':
            return
        if key in WHOLE_CHECK_IDS:
            if key != 'ABSENT_SCHEMA':
                add('SHARED_FIXTURE_SCHEMA')
            if key not in result:
                result.append(key)
            return
        need(key in plan['objects'], 'whole_fixture_unrecognized')
        for dep in plan['objects'][key].get('depends_on', []):
            add(dep)
        if key == 'CONFIGURED_PILOT_ROWS_REUSE':
            add('SHARED_FIXTURE_SCHEMA')
        if key not in result:
            result.append(key)
    for key in rows[0]['external_fixture_ids']:
        add(key)
    return tuple(result)

def fixture_stage(plan, key, stage):
    need(plan['sha256'] == WHOLE_FIXTURE_SHA and key in WHOLE_FIXTURE_IDS and (stage in ('inspect', 'create', 'insert', 'grant', 'ordinary', 'drop')), 'whole_fixture_stage')
    row = plan['objects'][key]
    queries = []
    if stage == 'inspect':
        value = row.get('initial_identity_SQL', row.get('inspect_SQL', row.get('all_overloads_inspect_SQL')))
        queries = list(value) if type(value) is list else [value]
    elif stage == 'create':
        queries = [row.get('create_if_absent_SQL', row.get('create_if_no_overload_SQL', row.get('create_only_absent_exact_owned_SQL')))]
    elif stage == 'insert':
        queries = row.get('INSERT_only_if_created_here_SQL', [])
    elif stage == 'grant':
        queries = [row.get('conditional_minimum_existing_role_grant_SQL', row.get('conditional_grant_SQL'))]
    elif stage == 'ordinary':
        queries = row.get('ordinary_read_inspections_SQL', row.get('ordinary_visibility_SQL', []))
    elif stage == 'drop':
        queries = [row.get('cleanup_only_held_newly_created_SQL', row.get('cleanup_only_if_created_here_exclusive_and_empty_SQL'))]
    need(bool(queries) and all((type(q) is str and 0 < len(q.encode('utf-8')) <= 16384 and ('\x00' not in q) and ('\\' not in q) for q in queries)), 'whole_fixture_stage_undefined')
    return {'id': key, 'stage': stage, 'queries': tuple(queries), 'query': '\n'.join(queries), 'statements': len(queries), 'role': 'test' if stage == 'ordinary' else 'admin', 'plan_sha256': WHOLE_FIXTURE_SHA}

def new_fixture_record(key):
    need(key in WHOLE_FIXTURE_IDS + WHOLE_CHECK_IDS, 'fixture_record_scope')
    return {'id': key, 'state': 'UNTOUCHED', 'custody': None, 'last_failure': None}

def record_fixture_stage(record, stage, custody):
    need(type(record) is dict and record.get('id') in WHOLE_FIXTURE_IDS and (record.get('state') in ('UNTOUCHED', 'MISSING', 'CREATE_UNKNOWN')) and (stage in ('CREATE_ACK', 'REUSED')), 'fixture_record_transition')
    need(type(custody) is dict and set(custody) == {'oid', 'owner_oid', 'shape_sha256'} and all((type(custody[k]) is int and 0 < custody[k] <= 4294967295 for k in ('oid', 'owner_oid'))) and (type(custody['shape_sha256']) is str) and (re.fullmatch('[a-f0-9]{64}', custody['shape_sha256']) is not None), 'fixture_record_custody')
    record['custody'] = dict(custody)
    record['state'] = 'OWNED' if stage == 'CREATE_ACK' else 'READ_ONLY_REUSED'

def fixture_cleanup_permitted(record):
    need(type(record) is dict and record.get('id') in WHOLE_FIXTURE_IDS and (record.get('state') == 'OWNED') and (type(record.get('custody')) is dict) and (record['id'] != 'SHARED_FIXTURE_SCHEMA'), 'fixture_cleanup_not_owned')
    return True

WHOLE_COLUMNS = {'CONFIGURED_PILOT_ROWS_REUSE': (('id', 'integer', 'YES', None), ('value', 'character varying', 'YES', 32)), 'COMPOSITE_PARENT': (('key_a', 'integer', 'NO', None), ('key_b', 'integer', 'NO', None)), 'COMPOSITE_CHILD': (('ref_a', 'integer', 'NO', None), ('ref_b', 'integer', 'NO', None)), 'NO_KEY': (('payload', 'integer', 'NO', None),), 'QUOTED_KEY': (('key.a', 'integer', 'NO', None), ('key"b', 'integer', 'NO', None)), 'UNICODE_TWO_ROWS': (('i"é表', 'integer', 'YES', None), ('v%_é表', 'character varying', 'YES', 32)), 'EMPTY_FOUR_TYPES': (('varbyte_value', 'varbyte', 'YES', None), ('super_value', 'super', 'YES', None), ('geometry_value', 'geometry', 'YES', None), ('geography_value', 'geography', 'YES', None))}

def _whole_literal(value):
    need(type(value) is str and (not any((c in value for c in '\x00\r\n|'))), 'fixture_literal')
    return "'" + value.replace("'", "''") + "'"

def _whole_ident(value):
    need(type(value) is str and 0 < len(value.encode('utf-8')) <= 127 and (not any((c in value for c in '\x00\r\n|'))), 'fixture_identifier')
    return '"' + value.replace('"', '""') + '"'

def whole_object(plan, key):
    need(plan['sha256'] == WHOLE_FIXTURE_SHA and key in WHOLE_FIXTURE_IDS and (key != 'CONFIGURED_FRESH_ROWS'), 'fixture_selected_object')
    return plan['objects'][key]

def whole_name(plan, key):
    row = whole_object(plan, key)
    if key == 'POSITIVE_ROUTINE':
        return '"odbcpp_fixture"."sp_m2_catalog_modes_20261003_c01"(INTEGER,INTEGER)'
    return _whole_ident(row['schema']) + '.' + _whole_ident(row['name']) if 'schema' in row else _whole_ident(row['name'])

def whole_identity_sql(plan, key):
    row = whole_object(plan, key)
    if key == 'CONFIGURED_PILOT_ROWS_REUSE':
        return row['inspect_SQL'][0]
    if key == 'POSITIVE_ROUTINE':
        return row['all_overloads_inspect_SQL'].replace("p.proname IN ('sp_m2_catalog_modes_20261003_c01','odbcppabsentmetadataid20261010')", "p.proname='sp_m2_catalog_modes_20261003_c01'")
    return row.get('initial_identity_SQL', row.get('inspect_SQL'))

def whole_shape_sql(plan, key):
    row = whole_object(plan, key)
    need(key in WHOLE_COLUMNS, 'fixture_relation_shape')
    if key == 'CONFIGURED_PILOT_ROWS_REUSE':
        return row['inspect_SQL'][1]
    return row['shape_inspection_SQL']

def whole_shape_guard(plan, key):
    row = whole_object(plan, key)
    terms = []
    for i, (name, kind, nullable, width) in enumerate(WHOLE_COLUMNS[key], 1):
        kinds = "('varbyte','varbinary','binary varying')" if kind == 'varbyte' else '(' + _whole_literal(kind) + ')'
        terms.append('(ordinal_position=' + str(i) + ' AND column_name=' + _whole_literal(name) + ' AND LOWER(TRIM(data_type)) IN ' + kinds + ' AND is_nullable=' + _whole_literal(nullable) + ' AND column_default IS NULL' + (' AND character_maximum_length=' + str(width) if width is not None else '') + ')')
    return 'SELECT CASE WHEN COUNT(*)=' + str(len(terms)) + ' AND SUM(CASE WHEN ' + ' OR '.join(terms) + ' THEN 1 ELSE 0 END)=' + str(len(terms)) + ' THEN 1 ELSE 0 END AS fx_shape FROM svv_columns WHERE table_catalog=current_database() AND table_schema=' + _whole_literal(row['schema']) + ' AND table_name=' + _whole_literal(row['name'])

def whole_frames(payload):
    need(type(payload) is str and len(payload.encode('utf-8')) <= 65536 and ('\x00' not in payload), 'fixture_frame_bound')
    frames = {}
    current = None
    for line in payload.splitlines():
        if line.startswith('RSFIX2|'):
            parts = line.split('|')
            need(len(parts) == 3 and parts[1] in WHOLE_FIXTURE_IDS + WHOLE_CHECK_IDS + ('SESSION', 'TIMEOUT_GATE') and (parts[2] in ('ADMIN_OID', 'IDENTITY', 'SHAPE', 'CREATE', 'CREATE_ACK', 'GRANT_PRE', 'GRANT_ACK', 'INSERT_0', 'PRIVILEGE', 'ROWS_0', 'ROWS_1', 'KEYS', 'PARAMETERS', 'DROP', 'DROP_ACK', 'OID_ABSENT', 'REFUSED', 'LOCKS', 'SESSIONS', 'QUERIES', 'BIRTHS', 'DROP_SHAPE', 'DROP_ROWS', 'DROP_LOCKS', 'SCHEMA_PRESENT')), 'fixture_frame_header')
            if parts[1] == 'TIMEOUT_GATE':
                need(parts[2] in ('IDENTITY', 'SHAPE', 'ROWS_0', 'LOCKS', 'SESSIONS', 'QUERIES', 'BIRTHS', 'DROP', 'DROP_SHAPE', 'DROP_ROWS', 'DROP_LOCKS', 'DROP_ACK', 'OID_ABSENT', 'SCHEMA_PRESENT', 'REFUSED'), 'timeout_frame_label')
            else:
                need(parts[2] in ('ADMIN_OID', 'IDENTITY', 'SHAPE', 'CREATE', 'CREATE_ACK', 'GRANT_PRE', 'GRANT_ACK', 'INSERT_0', 'PRIVILEGE', 'ROWS_0', 'ROWS_1', 'KEYS', 'PARAMETERS', 'DROP', 'DROP_ACK', 'OID_ABSENT', 'REFUSED'), 'fixture_frame_header')
            current = (parts[1], parts[2])
            need(current not in frames, 'fixture_frame_duplicate')
            frames[current] = []
        else:
            need(current is not None and len(frames[current]) < 64, 'fixture_frame_unframed_or_rows')
            frames[current].append(line)
    return frames

def whole_validate_identity(plan, key, lines):
    need(type(lines) is list and len(lines) <= 1, 'fixture_collision_or_overload')
    if not lines:
        return None
    fields = lines[0].split('|')
    row = whole_object(plan, key)
    if key in ('SHARED_FIXTURE_SCHEMA', 'UNICODE_SCHEMA'):
        need(len(fields) == 3 and fields[1] == row['name'], 'fixture_schema_identity')
        oid, owner = (fields[0], fields[2])
    else:
        need(len(fields) == 5, 'fixture_object_identity')
        schema = row.get('schema', 'odbcpp_fixture')
        name = row.get('name', 'sp_m2_catalog_modes_20261003_c01')
        need(fields[:2] == [schema, name] and fields[4] == ('23 23' if key == 'POSITIVE_ROUTINE' else 'r'), 'fixture_object_identity')
        oid, owner = fields[2:4]
    need(all((re.fullmatch('[1-9][0-9]{0,9}', v) and int(v) <= 4294967295 for v in (oid, owner))), 'fixture_identity_numbers')
    return {'oid': int(oid), 'owner_oid': int(owner), 'identity': lines[0]}

def whole_validate_shape(key, lines):
    need(key in WHOLE_COLUMNS and len(lines) == len(WHOLE_COLUMNS[key]), 'fixture_shape_count')
    for i, (line, expected) in enumerate(zip(lines, WHOLE_COLUMNS[key]), 1):
        cells = line.split('|')
        name, kind, nullable, width = expected
        need(len(cells) == 10 and cells[2] == name and (cells[3] == str(i)) and (cells[5] == nullable) and (cells[6] == ''), 'fixture_shape_fields')
        actual = cells[4].strip().lower()
        need(actual == kind or (kind == 'varbyte' and actual in ('varbinary', 'binary varying')), 'fixture_shape_type')
        if width is not None:
            need(cells[7] == str(width), 'fixture_shape_width')
    import hashlib, json
    return hashlib.sha256(json.dumps(lines, ensure_ascii=False, separators=(',', ':')).encode()).hexdigest()

def whole_batch_stage(plan, keys, stage, records):
    need(type(keys) in (tuple, list) and len(keys) == len(set(keys)) and (0 < len(keys) <= 10) and all((k in WHOLE_FIXTURE_IDS + WHOLE_CHECK_IDS and k != 'CONFIGURED_FRESH_ROWS' for k in keys)), 'fixture_batch_scope')
    need(stage in ('inspect', 'create', 'grant', 'ordinary', 'drop', 'cleanup'), 'fixture_batch_stage')
    chunks = []
    statements = 0

    def query(key, label, sql):
        nonlocal statements
        chunks.extend(('\\echo RSFIX2|' + key + '|' + label, sql.rstrip(';') + ';'))
        statements += 1
    query('SESSION', 'ADMIN_OID', "SELECT usesysid FROM pg_catalog.pg_user WHERE usename='odbcpp_pilot_admin'")
    if stage == 'cleanup':
        reused = tuple((k for k in keys if records[k]['state'] in ('READ_ONLY_REUSED', 'VERIFIED_ABSENT')))
        owned = tuple((k for k in keys if records[k]['state'] == 'OWNED'))
        parts = []
        count = 0
        for subset, kind in ((reused, 'ordinary'), (owned, 'drop')):
            if subset:
                part = whole_batch_stage(plan, subset, kind, records)
                lines = part['query'].splitlines()
                if parts:
                    lines = lines[2:]
                    count -= 1
                parts.extend(lines)
                count += part['statements']
        if not parts:
            parts = chunks
            count = statements
        return {'query': '\n'.join(parts) + '\n', 'statements': count, 'role': 'admin', 'keys': tuple(keys), 'stage': stage}
    ordered = list(reversed(keys)) if stage == 'drop' else keys
    for key in ordered:
        if key in WHOLE_CHECK_IDS:
            if stage in ('inspect', 'ordinary'):
                query(key, 'IDENTITY', whole_absence_query(key))
            continue
        row = whole_object(plan, key)
        record = records[key]
        name = whole_name(plan, key)
        if stage == 'inspect':
            query(key, 'IDENTITY', whole_identity_sql(plan, key))
            if key in WHOLE_COLUMNS:
                query(key, 'SHAPE', whole_shape_sql(plan, key))
        elif stage == 'create':
            if record['state'] != 'MISSING':
                continue
            need(key != 'SHARED_FIXTURE_SCHEMA', 'shared_schema_missing')
            block, count = whole_created_block(plan, key)
            chunks.append(block)
            statements += count
        elif stage == 'grant':
            if record['state'] != 'OWNED':
                continue
            fixture_cleanup_permitted(record)
            if key == 'CONFIGURED_PILOT_ROWS_REUSE':
                inserts = ["INSERT INTO odbcpp_fixture.pilot_rows(id,value) VALUES (1,'one'),(2,'two');"]
            else:
                inserts = row.get('INSERT_only_if_created_here_SQL', [])
            for index, sql in enumerate(inserts):
                query(key, 'INSERT_' + str(index), sql)
            if key == 'POSITIVE_ROUTINE':
                need(False, 'routine_privilege_probe_unsealed')
            elif key == 'UNICODE_SCHEMA':
                priv = "has_schema_privilege('odbcpp_pilot_test'," + _whole_literal(row['name']) + ",'USAGE')"
                sql = row['conditional_grant_SQL']
            else:
                priv = "has_table_privilege('odbcpp_pilot_test'," + _whole_literal(name) + ",'SELECT')"
                sql = 'GRANT SELECT ON TABLE ' + name + ' TO odbcpp_pilot_test;'
            query(key, 'GRANT_PRE', 'SELECT CASE WHEN ' + priv + " THEN 'false' ELSE 'true' END AS fx_grant")
            chunks[-1] = chunks[-1].rstrip(';') + '\n\\gset\n\\if :fx_grant\n' + sql + '\n\\endif\n\\echo RSFIX2|' + key + '|GRANT_ACK'
            statements += 1
        elif stage == 'ordinary':
            query(key, 'IDENTITY', whole_identity_sql(plan, key))
            if key in WHOLE_COLUMNS:
                query(key, 'SHAPE', whole_shape_sql(plan, key))
                query(key, 'PRIVILEGE', "SELECT has_schema_privilege('odbcpp_pilot_test'," + _whole_literal(row['schema']) + ",'USAGE'),has_table_privilege('odbcpp_pilot_test'," + _whole_literal(name) + ",'SELECT')")
                if key == 'CONFIGURED_PILOT_ROWS_REUSE':
                    reads = ['SELECT COUNT(*) FROM ' + name + ';', 'SELECT id,value FROM ' + name + ' ORDER BY id LIMIT 3;']
                elif key == 'UNICODE_TWO_ROWS':
                    first = _whole_ident('i"é表')
                    value = _whole_ident('v%_é表')
                    reads = [row['ordinary_read_inspections_SQL'][0], 'SELECT ' + first + ',CASE WHEN ' + value + " IS NULL THEN 'RSNULL' ELSE 'RSVALUE'||" + value + ' END FROM ' + name + ' ORDER BY ' + first + ' LIMIT 3;']
                else:
                    reads = row['ordinary_read_inspections_SQL']
                for index, sql in enumerate(reads):
                    query(key, 'ROWS_' + str(index), sql)
                if key in ('COMPOSITE_PARENT', 'NO_KEY', 'QUOTED_KEY', 'COMPOSITE_CHILD'):
                    kind = 'FOREIGN' if key == 'COMPOSITE_CHILD' else 'PRIMARY'
                    chunks.append('\\pset tuples_only off\n\\pset format csv')
                    query(key, 'KEYS', 'SHOW CONSTRAINTS ' + kind + ' KEYS FROM TABLE "odbcpp_pilot".' + name + ';')
                    chunks.append('\\pset tuples_only on\n\\pset format unaligned')
            elif key == 'POSITIVE_ROUTINE':
                chunks.append('\\pset tuples_only off\n\\pset format csv')
                query(key, 'PARAMETERS', row['ordinary_visibility_SQL'][2])
                chunks.append('\\pset tuples_only on\n\\pset format unaligned')
            else:
                query(key, 'PRIVILEGE', "SELECT has_schema_privilege('odbcpp_pilot_test'," + _whole_literal(row['name']) + ",'USAGE')")
        else:
            if record['state'] != 'OWNED':
                continue
            fixture_cleanup_permitted(record)
            held = record['custody']
            need(type(held['oid']) is int and type(held['owner_oid']) is int, 'fixture_drop_custody')
            if key in WHOLE_COLUMNS:
                q = 'SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS fx_held FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE c.oid=' + str(held['oid']) + ' AND c.relowner=' + str(held['owner_oid']) + " AND c.relkind='r' AND n.nspname=" + _whole_literal(row['schema']) + ' AND c.relname=' + _whole_literal(row['name'])
            elif key == 'POSITIVE_ROUTINE':
                q = "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS fx_held FROM pg_proc_info p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace WHERE p.prokind='p' AND p.proargmodes[1]='i' AND p.proargmodes[2]='b' AND p.proargmodes[3] IS NULL AND p.proargnames[1]='p_input' AND p.proargnames[2]='p_result' AND p.proargnames[3] IS NULL AND p.prooid=" + str(held['oid']) + ' AND p.proowner=' + str(held['owner_oid']) + " AND n.nspname='odbcpp_fixture' AND p.proname='sp_m2_catalog_modes_20261003_c01' AND p.proargtypes='23 23'"
            else:
                q = 'SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS fx_held FROM pg_catalog.pg_namespace WHERE oid=' + str(held['oid']) + ' AND nspowner=' + str(held['owner_oid']) + ' AND nspname=' + _whole_literal(row['name'])
            chunks.extend(('\\echo RSFIX2|' + key + '|DROP', q + '\n\\gset\n\\if :fx_held'))
            statements += 1
            if key in WHOLE_COLUMNS:
                chunks.extend((whole_shape_guard(plan, key) + '\n\\gset\n\\if :fx_shape',))
                statements += 1
            sql = 'DROP TABLE ' + name + ' RESTRICT;' if key in WHOLE_COLUMNS else fixture_stage(plan, key, 'drop')['query']
            chunks.append(sql)
            statements += 1
            chunks.append('\\echo RSFIX2|' + key + '|DROP_ACK')
            query(key, 'IDENTITY', whole_identity_sql(plan, key))
            catalog = 'pg_catalog.pg_class' if key in WHOLE_COLUMNS else 'pg_catalog.pg_proc' if key == 'POSITIVE_ROUTINE' else 'pg_catalog.pg_namespace'
            query(key, 'OID_ABSENT', 'SELECT COUNT(*) FROM ' + catalog + ' WHERE oid=' + str(held['oid']))
            if key in WHOLE_COLUMNS:
                chunks.append('\\else\n\\echo RSFIX2|' + key + '|REFUSED\n\\endif')
            chunks.append('\\else\n\\echo RSFIX2|' + key + '|REFUSED\n\\endif')
    return {'query': '\n'.join(chunks) + '\n', 'statements': statements, 'role': 'test' if stage == 'ordinary' else 'admin', 'keys': tuple(keys), 'stage': stage}

def whole_input_encoding():
    return 'utf-8'

def whole_capture(plan, keys, frames, records, *, creating=False):
    import hashlib, json
    admin = frames.get(('SESSION', 'ADMIN_OID'))
    need(type(admin) is list and len(admin) == 1 and (re.fullmatch('[1-9][0-9]{0,9}', admin[0]) is not None), 'fixture_admin_oid')
    for key in keys:
        record = records[key]
        if creating and record['state'] not in ('MISSING', 'CREATE_UNKNOWN'):
            continue
        try:
            if key in WHOLE_CHECK_IDS:
                need(not creating and frames.get((key, 'IDENTITY')) == ['0'], 'fixture_required_absence')
                record['state'] = 'VERIFIED_ABSENT'
                record['ready'] = True
                continue
            identity = whole_validate_identity(plan, key, frames.get((key, 'IDENTITY'), []))
            if identity is None:
                need(not creating and key != 'SHARED_FIXTURE_SCHEMA', 'fixture_postidentity_missing')
                record['state'] = 'MISSING'
                continue
            if creating:
                need((key, 'CREATE_ACK') in frames and identity['owner_oid'] == int(admin[0]), 'fixture_create_ack_or_owner')
            elif key == 'POSITIVE_ROUTINE':
                need(False, 'fixture_routine_collision')
            shape = whole_validate_shape(key, frames.get((key, 'SHAPE'), [])) if key in WHOLE_COLUMNS else hashlib.sha256(identity['identity'].encode()).hexdigest()
            record_fixture_stage(record, 'CREATE_ACK' if creating else 'REUSED', {'oid': identity['oid'], 'owner_oid': identity['owner_oid'], 'shape_sha256': shape})
            record['identity'] = identity['identity']
            record['ready'] = False
        except Exception:
            if creating:
                record['state'] = 'UNKNOWN'
            raise

def whole_created_block(plan, key):
    row = whole_object(plan, key)
    name = whole_name(plan, key)
    parts = []
    count = 0

    def sql(label, q):
        nonlocal count
        parts.extend(('\\echo RSFIX2|' + key + '|' + label, q.rstrip(';') + ';'))
        count += 1
    create = 'CREATE TABLE "odbcpp_fixture"."pilot_rows" (id INTEGER, value VARCHAR(32));' if key == 'CONFIGURED_PILOT_ROWS_REUSE' else fixture_stage(plan, key, 'create')['query']
    sql('CREATE', create)
    parts.append('\\echo RSFIX2|' + key + '|IDENTITY')
    parts.append(whole_identity_sql(plan, key).rstrip(';') + '\n\\gset fx_')
    count += 1
    if key in WHOLE_COLUMNS:
        parts.append('\\echo :fx_nspname|:fx_relname|:fx_oid|:fx_relowner|:fx_relkind')
        sql('SHAPE', whole_shape_sql(plan, key))
        owner = "SELECT CASE WHEN :fx_relowner=(SELECT usesysid FROM pg_catalog.pg_user WHERE usename='odbcpp_pilot_admin') AND EXISTS(SELECT 1 FROM pg_catalog.pg_class WHERE oid=:fx_oid AND relowner=:fx_relowner AND relkind='r') THEN 1 ELSE 0 END AS fx_owned"
    elif key == 'POSITIVE_ROUTINE':
        parts.append('\\echo :fx_nspname|:fx_proname|:fx_oid|:fx_proowner|:fx_proargtypes')
        owner = "SELECT CASE WHEN :fx_proowner=(SELECT usesysid FROM pg_catalog.pg_user WHERE usename='odbcpp_pilot_admin') AND :'fx_proargtypes'='23 23' AND EXISTS(SELECT 1 FROM pg_proc_info WHERE prooid=:fx_oid AND proowner=:fx_proowner AND prokind='p' AND proargmodes[1]='i' AND proargmodes[2]='b' AND proargmodes[3] IS NULL AND proargnames[1]='p_input' AND proargnames[2]='p_result' AND proargnames[3] IS NULL) THEN 1 ELSE 0 END AS fx_owned"
    else:
        parts.append('\\echo :fx_oid|:fx_nspname|:fx_nspowner')
        owner = "SELECT CASE WHEN :fx_nspowner=(SELECT usesysid FROM pg_catalog.pg_user WHERE usename='odbcpp_pilot_admin') AND EXISTS(SELECT 1 FROM pg_catalog.pg_namespace WHERE oid=:fx_oid AND nspowner=:fx_nspowner) THEN 1 ELSE 0 END AS fx_owned"
    parts.append(owner + '\n\\gset\n\\if :fx_owned')
    count += 1
    if key in WHOLE_COLUMNS:
        parts.append(whole_shape_guard(plan, key) + '\n\\gset\n\\if :fx_shape')
        count += 1
    parts.append('\\echo RSFIX2|' + key + '|CREATE_ACK')
    inserts = ["INSERT INTO odbcpp_fixture.pilot_rows(id,value) VALUES (1,'one'),(2,'two');"] if key == 'CONFIGURED_PILOT_ROWS_REUSE' else row.get('INSERT_only_if_created_here_SQL', [])
    for index, q in enumerate(inserts):
        sql('INSERT_' + str(index), q)
    if key == 'POSITIVE_ROUTINE':
        sql('GRANT_PRE', row['conditional_minimum_existing_role_grant_SQL'])
        parts.append('\\echo RSFIX2|' + key + '|GRANT_ACK')
    else:
        privilege = "has_schema_privilege('odbcpp_pilot_test'," + _whole_literal(row['name']) + ",'USAGE')" if key == 'UNICODE_SCHEMA' else "has_table_privilege('odbcpp_pilot_test'," + _whole_literal(name) + ",'SELECT')"
        grant = row['conditional_grant_SQL'] if key == 'UNICODE_SCHEMA' else 'GRANT SELECT ON TABLE ' + name + ' TO odbcpp_pilot_test;'
        parts.extend(('\\echo RSFIX2|' + key + '|GRANT_PRE', 'SELECT CASE WHEN ' + privilege + " THEN 'false' ELSE 'true' END AS fx_grant\n\\gset\n\\if :fx_grant\n" + grant + '\n\\endif\n\\echo RSFIX2|' + key + '|GRANT_ACK'))
        count += 2
    if key in WHOLE_COLUMNS:
        parts.append('\\else\n\\echo RSFIX2|' + key + '|REFUSED\n\\endif')
    parts.append('\\else\n\\echo RSFIX2|' + key + '|REFUSED\n\\endif')
    return ('\n'.join(parts), count)

def whole_csv(lines, required, optional=()):
    import csv, io
    need(type(lines) is list and 0 < len(lines) <= 64, 'fixture_csv_bound')
    rows = list(csv.reader(io.StringIO('\n'.join(lines) + '\n'), strict=True))
    header = rows[0]
    need(len(header) == len(set(header)) and set(required) <= set(header) <= set(required) | set(optional), 'fixture_csv_header')
    need(all((len(r) == len(header) for r in rows[1:])), 'fixture_csv_width')
    return [dict(zip(header, r)) for r in rows[1:]]

def whole_validate_ready(plan, keys, frames, records):
    import hashlib, json
    for key in keys:
        record = records[key]
        if key in WHOLE_CHECK_IDS:
            need(record['state'] == 'VERIFIED_ABSENT' and frames.get((key, 'IDENTITY')) == ['0'], 'fixture_required_absence')
            continue
        need(record['state'] in ('OWNED', 'READ_ONLY_REUSED'), 'fixture_ready_custody')
        identity = whole_validate_identity(plan, key, frames.get((key, 'IDENTITY'), []))
        need(identity is not None and identity['identity'] == record['identity'], 'fixture_identity_changed')
        if key in WHOLE_COLUMNS:
            need(whole_validate_shape(key, frames.get((key, 'SHAPE'), [])) == record['custody']['shape_sha256'], 'fixture_shape_changed')
            need(frames.get((key, 'PRIVILEGE')) == ['t|t'], 'fixture_ordinary_privilege')
            count = '2' if key in ('CONFIGURED_PILOT_ROWS_REUSE', 'UNICODE_TWO_ROWS') else '0'
            need(frames.get((key, 'ROWS_0')) == [count], 'fixture_exact_row_count')
            expected = ['1|one', '2|two'] if key == 'CONFIGURED_PILOT_ROWS_REUSE' else ['1|RSVALUEGrüße 😀', '2|RSNULL'] if key == 'UNICODE_TWO_ROWS' else []
            need(frames.get((key, 'ROWS_1')) == expected, 'fixture_exact_rows')
            if key in ('COMPOSITE_PARENT', 'NO_KEY', 'QUOTED_KEY'):
                rows = whole_csv(frames.get((key, 'KEYS')), ('database_name', 'schema_name', 'table_name', 'column_name', 'key_seq', 'pk_name'))
                columns = [] if key == 'NO_KEY' else ['key_b', 'key_a'] if key == 'COMPOSITE_PARENT' else ['key"b', 'key.a']
                need(len(rows) == len(columns) and {r['key_seq'] for r in rows} == {str(i) for i in range(1, len(columns) + 1)}, 'fixture_PK_count')
                rows.sort(key=lambda r: int(r['key_seq']))
                for index, (r, column) in enumerate(zip(rows, columns), 1):
                    obj = plan['objects'][key]
                    need([r['database_name'], r['schema_name'], r['table_name'], r['column_name'], r['key_seq']] == ['odbcpp_pilot', obj['schema'], obj['name'], column, str(index)] and bool(r['pk_name']), 'fixture_PK_order')
                if rows:
                    need(len({r['pk_name'] for r in rows}) == 1, 'fixture_PK_name')
                if key == 'QUOTED_KEY':
                    need(all((r['pk_name'] == plan['objects'][key]['expected']['constraint'] for r in rows)), 'fixture_quoted_PK_name')
            elif key == 'COMPOSITE_CHILD':
                fields = ('pk_database_name', 'pk_schema_name', 'pk_table_name', 'pk_column_name', 'fk_database_name', 'fk_schema_name', 'fk_table_name', 'fk_column_name', 'key_seq', 'update_rule', 'delete_rule', 'fk_name', 'pk_name', 'deferrability')
                rows = whole_csv(frames.get((key, 'KEYS')), fields)
                need(len(rows) == 2 and {r['key_seq'] for r in rows} == {'1', '2'}, 'fixture_FK_count')
                rows.sort(key=lambda r: int(r['key_seq']))
                for index, (r, pair) in enumerate(zip(rows, (('ref_b', 'key_b'), ('ref_a', 'key_a'))), 1):
                    need([r['pk_database_name'], r['pk_schema_name'], r['pk_table_name'], r['pk_column_name'], r['fk_database_name'], r['fk_schema_name'], r['fk_table_name'], r['fk_column_name'], r['key_seq']] == ['odbcpp_pilot', 'odbcpp_fixture', 'm2_catalog_parent_20261003_c01', pair[1], 'odbcpp_pilot', 'odbcpp_fixture', plan['objects'][key]['name'], pair[0], str(index)], 'fixture_FK_order')
                need(len({r['fk_name'] for r in rows}) == len({r['pk_name'] for r in rows}) == 1, 'fixture_FK_names')
        elif key == 'POSITIVE_ROUTINE':
            cols = ('database_name', 'schema_name', 'procedure_name', 'parameter_name', 'ordinal_position', 'parameter_type', 'data_type', 'character_maximum_length', 'numeric_precision', 'numeric_scale')
            rows = whole_csv(frames.get((key, 'PARAMETERS')), cols, ('remarks',))
            need(len(rows) == 2 and {r['ordinal_position'] for r in rows} == {'1', '2'}, 'fixture_routine_parameters')
            rows.sort(key=lambda r: int(r['ordinal_position']))
            for index, (r, name, mode) in enumerate(zip(rows, ('p_input', 'p_result'), ('IN', 'INOUT')), 1):
                need([r['database_name'], r['schema_name'], r['procedure_name'], r['parameter_name'], r['ordinal_position'], r['parameter_type'], r['data_type']] == ['odbcpp_pilot', 'odbcpp_fixture', 'sp_m2_catalog_modes_20261003_c01', name, str(index), mode, 'integer'], 'fixture_routine_signature')
        else:
            need(frames.get((key, 'PRIVILEGE')) == ['t'], 'fixture_schema_usage')
        record['ready'] = True
        fingerprint = {label: sorted(value) if label in ('KEYS', 'PARAMETERS') else value for (ident, label), value in frames.items() if ident == key}
        record['readiness_sha256'] = hashlib.sha256(json.dumps(fingerprint, sort_keys=True, ensure_ascii=False).encode()).hexdigest()

def whole_absence_query(key):
    need(key in WHOLE_CHECK_IDS, 'fixture_absence_scope')
    if key == 'ABSENT_SCHEMA':
        return "SELECT COUNT(*) FROM pg_catalog.pg_namespace WHERE nspname='odbcpp_missing_schema_diag_6b31';"
    if key == 'ABSENT_PK':
        return "SELECT COUNT(*) FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='odbcpp_fixture' AND c.relname='m2_pk_absent_20261004_c01';"
    return "SELECT COUNT(*) FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace WHERE n.nspname='odbcpp_fixture' AND p.proname='odbcppabsentmetadataid20261010';"

TWO_DB_CASE = 'RedshiftMetadataScopeShowRealTest.TwoVisibleDatabasesKeepCommonSchemaIdentityThroughWideFanoutAndSibling'

TWO_DB_VARIABLE = 'ODBCPP_REDSHIFT_METADATA_SCOPE_SECOND_DATABASE'

TWO_DB_SCHEMA = 'ODBCPP_REDSHIFT_METADATA_SCOPE_COMMON_SCHEMA'

TWO_DB_MARKER = 'ROOT_RESOLVE_EXISTING_TWO_DB_PUBLIC_V1'

def two_db_name(value):
    need(type(value) is str and 0 < len(value.encode('utf-8', 'strict')) <= 128 and (not any((ord(c) < 32 or ord(c) == 127 for c in value))), 'two_db_identifier')
    return '"' + value.replace('"', '""') + '"'

def two_db_stage(kind, other=None):
    if kind == 'databases':
        need(other is None, 'two_db_probe_scope')
        query = '\\pset format csv\n\\pset tuples_only off\n\\echo RSDB2|DATABASES\nSHOW DATABASES;\n\\echo RSDB2|END'
        count = 1
    else:
        need(kind == 'schemas' and other != 'odbcpp_pilot', 'two_db_probe_scope')
        quoted = two_db_name(other)
        query = '\\pset format csv\n\\pset tuples_only off\n\\echo RSDB2|CURRENT\nSHOW SCHEMAS FROM DATABASE "odbcpp_pilot";\n\\echo RSDB2|OTHER\nSHOW SCHEMAS FROM DATABASE ' + quoted + ';\n\\echo RSDB2|END'
        count = 2
    return {'stage': 'two_db_' + kind, 'keys': (), 'other': other, 'role': 'test', 'query': query, 'statements': count}

def two_db_csv(payload, labels, header):
    import csv, io
    need(type(payload) is str and len(payload.encode('utf-8')) <= 65536, 'two_db_output_bound')
    lines = payload.splitlines()
    need(lines and lines[-1] == 'RSDB2|END', 'two_db_frame')
    frames = {}
    current = None
    for line in lines[:-1]:
        if line.startswith('RSDB2|'):
            current = line[6:]
            need(current in labels and current not in frames, 'two_db_frame')
            frames[current] = []
        else:
            need(current is not None, 'two_db_frame')
            frames[current].append(line)
    need(tuple(frames) == tuple(labels), 'two_db_frame')
    result = {}
    for label, values in frames.items():
        rows = list(csv.reader(io.StringIO('\n'.join(values) + '\n'), strict=True))
        need(1 <= len(rows) <= 10001, 'two_db_row_bound')
        cols = rows[0]
        known_database_header = ('database_name', 'database_owner', 'database_type', 'database_acl', 'parameters', 'database_isolation_level')
        extended = tuple(labels) == ('DATABASES',) and tuple(header) == known_database_header and (len(cols) == 7) and (set(cols) == set(header) | {'allow_connections'})
        need(len(cols) == len(header) and set(cols) == set(header) or extended, 'two_db_header')
        need(all((len(r) == len(cols) for r in rows[1:])), 'two_db_width')
        result[label] = [dict(zip(cols, r)) for r in rows[1:]]
    return result

def two_db_select(payload):
    rows = two_db_csv(payload, ('DATABASES',), ('database_name', 'database_owner', 'database_type', 'database_acl', 'parameters', 'database_isolation_level'))['DATABASES']
    names = [r['database_name'] for r in rows]
    for name in names:
        two_db_name(name)
    need(len(names) == len(set(names)), 'two_db_exact_existing_pair')
    if rows and 'allow_connections' in rows[0]:
        need(all((r['allow_connections'] in ('t', 'f') and r['database_type'] in ('local', 'auto mounted catalog') for r in rows)), 'two_db_type_or_allow')
        local = [r for r in rows if r['database_type'] == 'local']
        other = [r for r in rows if r['database_type'] != 'local']
        need(len(local) == 2 and {r['database_name'] for r in local} == {'odbcpp_pilot', 'dev'} and all((r['allow_connections'] == 't' for r in local)), 'two_db_exact_existing_local_pair')
        need(len(other) <= 1 and all((r['database_name'] == 'awsdatacatalog' and r['database_type'] == 'auto mounted catalog' and (r['allow_connections'] == 'f') for r in other)), 'two_db_unrecognized_nonlocal')
        return next((r['database_name'] for r in local if r['database_name'] != 'odbcpp_pilot'))
    need(len(names) == 2 and 'odbcpp_pilot' in names, 'two_db_exact_existing_pair')
    return next((n for n in names if n != 'odbcpp_pilot'))

def two_db_public(payload, other):
    two_db_name(other)
    frames = two_db_csv(payload, ('CURRENT', 'OTHER'), ('database_name', 'schema_name', 'schema_owner', 'schema_type', 'schema_acl', 'source_database', 'schema_option'))
    for label, database in (('CURRENT', 'odbcpp_pilot'), ('OTHER', other)):
        rows = frames[label]
        names = [r['schema_name'] for r in rows]
        for name in names:
            two_db_name(name)
        need(all((r['database_name'] == database for r in rows)) and len(names) == len(set(names)) and ('public' in names), 'two_db_existing_public')
    return True

TIMEOUT_CASES = ('it_bind_col_real::BindColIntegrationTest.QueryTimeoutUsesTransportDeadline', 'it_metadata_real::MetadataIntegrationTest.PreparedMetadataRefreshesAfterTimeoutReconnect')

TIMEOUT_VARIABLES = ('ODBCPP_REDSHIFT_TIMEOUT_SCHEMA', 'ODBCPP_REDSHIFT_TIMEOUT_TABLE', 'ODBCPP_REDSHIFT_TIMEOUT_OID')

def validate_timeout_preowned(value):
    fields = {'database', 'schema', 'schema_oid', 'schema_owner_oid', 'table', 'oid', 'owner', 'owner_oid', 'kind', 'columns', 'rows', 'create_ack', 'cleanup_delegated', 'receipts'}
    need(type(value) is dict and set(value) == fields, 'timeout_actual_record_required')
    need(value['database'] == 'odbcpp_pilot' and value['schema'] == 'odbcpp_fixture' and (value['owner'] == 'odbcpp_pilot_test') and (value['kind'] == 'r'), 'timeout_principal_scope')
    need(type(value['table']) is str and re.fullmatch('odbcpp_timeout_gate_[a-z0-9_]{1,43}', value['table']) is not None, 'timeout_table_scope')
    for k in ('oid', 'owner_oid', 'schema_oid', 'schema_owner_oid'):
        need(type(value[k]) is int and 0 < value[k] <= 4294967295, 'timeout_actual_oid')
    need(type(value['columns']) is list and value['columns'] == [{'name': 'value', 'type': 'integer', 'nullable': False, 'default': None}] and (value['columns'][0]['nullable'] is False) and (type(value['rows']) is dict) and (value['rows'] == {'count': 1, 'min': 1, 'max': 1}) and all((type(v) is int for v in value['rows'].values())), 'timeout_shape_rows')
    need(value['create_ack'] is True and value['cleanup_delegated'] is True, 'timeout_ack_cleanup_delegation')
    need(type(value['receipts']) is dict and set(value['receipts']) == {'creation', 'identity', 'shape_rows', 'authority', 'applicability', 'cleanup_delegation'}, 'timeout_receipt_inventory')
    for pin in value['receipts'].values():
        _pin(pin)
    import json
    return json.loads(json.dumps(value))

def validate_timeout_profile(record, spec, profile):
    if spec['key'] not in TIMEOUT_CASES:
        return
    r = validate_timeout_preowned(record)
    need(profile['private_input']['kind'] == 'OWN_DSN', 'timeout_private_dsn')
    need(profile['variables'].get('ODBCPP_TEST_DSN') == 'DSN=RedshiftProd', 'timeout_dsn_name')
    for key, value in zip(TIMEOUT_VARIABLES, (r['schema'], r['table'], str(r['oid']))):
        need(profile['variables'].get(key) == value, 'timeout_profile_actual_identity')

def timeout_native_sessions(record, spec, result):
    r = validate_timeout_preowned(record)
    need(spec['key'] in TIMEOUT_CASES, 'timeout_literal_case')
    need(result.get('key') == spec['key'] and result.get('case_name') == spec['case'] and (result.get('status') in ('PASS', 'FAIL', 'SKIP')) and (result.get('timed_out') is False) and (type(result.get('returncode')) is int) and (result['returncode'] == int(result['status'] == 'FAIL')), 'timeout_native_completion')
    p = result.get('properties')
    need(type(p) is dict, 'timeout_native_properties')
    for key, value in zip(('timeout_fixture_schema', 'timeout_fixture_table', 'timeout_fixture_oid'), (r['schema'], r['table'], str(r['oid']))):
        need(p.get(key) == value, 'timeout_native_held_relation')
    for key in ('timeout_fixture_unknown', 'timeout_waiter_completion_unknown'):
        need(key not in p, 'timeout_native_unknown')
    for key in ('timeout_blocker_lock_ack', 'timeout_blocker_rollback_ack', 'timeout_blocker_cleanup_known', 'timeout_waiter_expected_transport_timeout'):
        need(p.get(key) == 'true', 'timeout_native_ack')
    sessions = []
    prefixes = ('timeout_waiter_before', 'timeout_blocker') + (('timeout_waiter_after',) if spec['key'] == TIMEOUT_CASES[1] else ())
    for prefix in prefixes:
        need(p.get(prefix + '_owner') == r['owner'] and p.get(prefix + '_database') == r['database'], 'timeout_native_session_identity')
        pid = p.get(prefix + '_pid')
        start = p.get(prefix + '_start')
        need(type(pid) is str and re.fullmatch('[1-9][0-9]{0,9}', pid) is not None and (int(pid) <= 2147483647), 'timeout_native_pid')
        need(type(start) is str and 0 < len(start.encode('utf-8')) <= 159 and (not any((ch in start for ch in '\x00\r\n|'))), 'timeout_native_birth')
        sessions.append({'pid': int(pid), 'start': start})
    need(sessions[0]['pid'] != sessions[1]['pid'] and len({(v['pid'], v['start']) for v in sessions}) == len(sessions), 'timeout_native_distinct_sessions')
    for prefix in prefixes[:2]:
        ms = p.get(prefix + '_statement_timeout_ms')
        need(type(ms) is str and re.fullmatch('[0-9]{1,20}', ms) is not None and (int(ms) <= 18446744073709551615) and (int(ms) == 0 or int(ms) >= 3000), 'timeout_native_server_timeout')
    return sessions

def timeout_stage(record, kind, sessions=()):
    r = validate_timeout_preowned(record)
    need(kind in ('timeout_before', 'timeout_after', 'timeout_cleanup'), 'timeout_stage_kind')
    need(type(sessions) in (tuple, list) and (not sessions if kind != 'timeout_after' else 2 <= len(sessions) <= 3), 'timeout_session_scope')
    name = _whole_ident(r['schema']) + '.' + _whole_ident(r['table'])
    oid = str(r['oid'])
    owner = str(r['owner_oid'])
    identity = "SELECT n.nspname,c.relname,c.oid,c.relowner,c.relkind,n.oid,n.nspowner FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='odbcpp_fixture' AND c.relname=" + _whole_literal(r['table']) + ' LIMIT 2'
    shape = "SELECT column_name,LOWER(TRIM(data_type)),is_nullable,CASE WHEN column_default IS NULL THEN 'NULL' ELSE column_default END FROM svv_columns WHERE table_catalog=current_database() AND table_schema='odbcpp_fixture' AND table_name=" + _whole_literal(r['table']) + ' ORDER BY ordinal_position LIMIT 2'
    locks = 'SELECT COUNT(*) FROM svv_transactions WHERE relation=' + oid + " AND lockable_object_type='relation' AND txn_db='odbcpp_pilot'"
    chunks = []
    count = 0

    def q(label, sql):
        nonlocal count
        chunks.extend(('\\echo RSFIX2|TIMEOUT_GATE|' + label, sql.rstrip(';') + ';'))
        count += 1
    if kind == 'timeout_before':
        q('IDENTITY', identity)
        q('SHAPE', shape)
        q('ROWS_0', 'SELECT COUNT(*),MIN(value),MAX(value) FROM ' + name)
        q('LOCKS', locks)
    elif kind == 'timeout_after':
        q('SESSIONS', "SELECT EXISTS(SELECT 1 FROM stv_sessions WHERE user_name IN ('odbcpp_pilot_admin','odbcpp_pilot_test') AND db_name='odbcpp_pilot' AND process <> pg_backend_pid())")
        q('QUERIES', "SELECT EXISTS(SELECT 1 FROM stv_recents WHERE user_name IN ('odbcpp_pilot_admin','odbcpp_pilot_test') AND db_name='odbcpp_pilot' AND status <> 'Done' AND pid <> pg_backend_pid())")
        q('LOCKS', locks)
        ids = []
        for v in sessions:
            need(type(v) is dict and set(v) == {'pid', 'start'} and (type(v['pid']) is int) and (0 < v['pid'] <= 2147483647) and (type(v['start']) is str) and (0 < len(v['start'].encode('utf-8')) <= 159) and (not any((ch in v['start'] for ch in '\x00\r\n|'))), 'timeout_session_birth_scope')
            ids.append(str(v['pid']))
        q('BIRTHS', 'SELECT process,starttime,user_name,db_name FROM stv_sessions WHERE process IN (' + ','.join(ids) + ') LIMIT 4')
        q('IDENTITY', identity)
    else:
        guard = 'SELECT CASE WHEN EXISTS(SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE c.oid=' + oid + ' AND c.relowner=' + owner + " AND c.relkind='r' AND n.nspname='odbcpp_fixture' AND n.oid=" + str(r['schema_oid']) + ' AND n.nspowner=' + str(r['schema_owner_oid']) + ' AND c.relname=' + _whole_literal(r['table']) + ') THEN 1 ELSE 0 END AS timeout_held'
        q('DROP', guard)
        chunks[-1] = chunks[-1].rstrip(';') + '\n\\gset\n\\if :timeout_held'
        lock_guard = 'SELECT CASE WHEN (' + locks + ')=0 THEN 1 ELSE 0 END AS timeout_locks'
        q('DROP_LOCKS', lock_guard)
        chunks[-1] = chunks[-1].rstrip(';') + '\n\\gset\n\\if :timeout_locks'
        shape_guard = "SELECT CASE WHEN COUNT(*)=1 AND SUM(CASE WHEN ordinal_position=1 AND column_name='value' AND LOWER(TRIM(data_type))='integer' AND is_nullable='NO' AND column_default IS NULL THEN 1 ELSE 0 END)=1 THEN 1 ELSE 0 END AS timeout_shape FROM svv_columns WHERE table_catalog=current_database() AND table_schema='odbcpp_fixture' AND table_name=" + _whole_literal(r['table'])
        q('DROP_SHAPE', shape_guard)
        chunks[-1] = chunks[-1].rstrip(';') + '\n\\gset\n\\if :timeout_shape'
        q('DROP_ROWS', 'SELECT CASE WHEN COUNT(*)=1 AND MIN(value)=1 AND MAX(value)=1 THEN 1 ELSE 0 END AS timeout_rows FROM ' + name)
        chunks[-1] = chunks[-1].rstrip(';') + '\n\\gset\n\\if :timeout_rows'
        chunks.extend(('DROP TABLE ' + name + ' RESTRICT;', '\\echo RSFIX2|TIMEOUT_GATE|DROP_ACK'))
        count += 1
        q('IDENTITY', identity)
        q('OID_ABSENT', 'SELECT COUNT(*) FROM pg_catalog.pg_class WHERE oid=' + oid)
        q('SCHEMA_PRESENT', "SELECT oid,nspowner FROM pg_catalog.pg_namespace WHERE nspname='odbcpp_fixture' LIMIT 2")
        q('SESSIONS', "SELECT EXISTS(SELECT 1 FROM stv_sessions WHERE user_name IN ('odbcpp_pilot_admin','odbcpp_pilot_test') AND db_name='odbcpp_pilot' AND process <> pg_backend_pid())")
        q('QUERIES', "SELECT EXISTS(SELECT 1 FROM stv_recents WHERE user_name IN ('odbcpp_pilot_admin','odbcpp_pilot_test') AND db_name='odbcpp_pilot' AND status <> 'Done' AND pid <> pg_backend_pid())")
        chunks.append('\\else\n\\echo RSFIX2|TIMEOUT_GATE|REFUSED\n\\endif\n\\else\n\\echo RSFIX2|TIMEOUT_GATE|REFUSED\n\\endif\n\\else\n\\echo RSFIX2|TIMEOUT_GATE|REFUSED\n\\endif\n\\else\n\\echo RSFIX2|TIMEOUT_GATE|REFUSED\n\\endif')
    return {'query': '\n'.join(chunks) + '\n', 'statements': count, 'role': 'test' if kind == 'timeout_before' else 'admin', 'keys': ('TIMEOUT_GATE',), 'stage': kind}

def validate_timeout_frames(record, kind, frames):
    r = validate_timeout_preowned(record)
    prefix = 'TIMEOUT_GATE'
    expected = {'timeout_before': {'IDENTITY', 'SHAPE', 'ROWS_0', 'LOCKS'}, 'timeout_after': {'IDENTITY', 'SESSIONS', 'QUERIES', 'LOCKS', 'BIRTHS'}, 'timeout_cleanup': {'DROP', 'DROP_LOCKS', 'DROP_SHAPE', 'DROP_ROWS', 'DROP_ACK', 'IDENTITY', 'OID_ABSENT', 'SCHEMA_PRESENT', 'SESSIONS', 'QUERIES'}}
    need(kind in expected and set(frames) == {(prefix, k) for k in expected[kind]}, 'timeout_exact_stage_frames')
    identity = [r['schema'] + '|' + r['table'] + '|' + str(r['oid']) + '|' + str(r['owner_oid']) + '|r|' + str(r['schema_oid']) + '|' + str(r['schema_owner_oid'])]
    if kind == 'timeout_cleanup':
        need((prefix, 'REFUSED') not in frames and frames.get((prefix, 'DROP_LOCKS')) == [] and (frames.get((prefix, 'DROP_ACK')) == []) and (frames.get((prefix, 'IDENTITY')) == []) and (frames.get((prefix, 'OID_ABSENT')) == ['0']) and (frames.get((prefix, 'SCHEMA_PRESENT')) == [str(r['schema_oid']) + '|' + str(r['schema_owner_oid'])]), 'timeout_drop_ack_absence')
    else:
        need(frames.get((prefix, 'IDENTITY')) == identity, 'timeout_current_identity')
    if kind == 'timeout_before':
        need(frames.get((prefix, 'SHAPE')) == ['value|integer|NO|NULL'] and frames.get((prefix, 'ROWS_0')) == ['1|1|1'] and (frames.get((prefix, 'LOCKS')) == ['0']), 'timeout_current_shape_row_locks')
    else:
        need(frames.get((prefix, 'SESSIONS')) == ['f'] and frames.get((prefix, 'QUERIES')) == ['f'], 'timeout_scoped_zero')
        if kind == 'timeout_after':
            need(frames.get((prefix, 'LOCKS')) == ['0'] and frames.get((prefix, 'BIRTHS')) == [], 'timeout_active_or_reused_pid')
    return True

def validate_timeout_profiles(record, specs, profiles):
    selected = [s for s in specs if s['key'] in TIMEOUT_CASES]
    if not selected:
        need(record is None, 'timeout_record_without_case')
        return
    validate_timeout_preowned(record)
    base_id = 'p_ab30a30dd09f448bfd81'
    base = profiles.get(base_id)
    need(type(base) is dict, 'timeout_shared_profile_required')
    ids = [str(s['profile']) for s in selected]
    need(len(ids) == len(set(ids)) and base_id not in ids, 'timeout_two_dedicated_clones')
    need(not any((str(s['profile']) in ids for s in specs if s['key'] not in TIMEOUT_CASES)), 'timeout_clone_shared_with_other_case')
    for spec in selected:
        profile = profiles[str(spec['profile'])]
        validate_timeout_profile(record, spec, profile)
        for key in set(base).union(profile) - {'variables', 'review'}:
            need(profile.get(key) == base.get(key), 'timeout_clone_changed_base')
        base_vars = dict(base['variables'])
        clone_vars = dict(profile['variables'])
        need(not any((k in base_vars for k in TIMEOUT_VARIABLES)), 'timeout_shared_profile_mutated')
        for key in TIMEOUT_VARIABLES:
            clone_vars.pop(key, None)
        need(clone_vars == base_vars, 'timeout_clone_extra_public_input')

WHOLE_FIXTURE_SHA = '255b89664b780637450f4572bf96c5e61e7c8614b10b084058527b7e91485582'

def closed_fixture_plan():
    path = Path(__file__).with_name('redshift_fixture_data.json')
    raw = path.read_bytes()
    need(hashlib.sha256(raw).hexdigest() == WHOLE_FIXTURE_SHA, 'canonical_fixture_data_changed')
    data = json.loads(raw)
    need(tuple(data['objects']) == WHOLE_FIXTURE_IDS and len(data['cases']) == 155,
         'canonical_fixture_inventory')
    return dict(data, sha256=WHOLE_FIXTURE_SHA)
