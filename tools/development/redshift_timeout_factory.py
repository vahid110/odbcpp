"""Exact timeout fixture SQL scoped to admitted table/principal identities. Retains source092 guards and receipt semantics; no historical OID/table default."""
import re
from pathlib import Path
from tools.development.redshift_ctest_lifecycle import need
DATABASE = 'odbcpp_pilot'
ORDINARY = 'odbcpp_pilot_test'
SCHEMA = 'odbcpp_fixture'
RECEIPTS = ('creation','identity','shape_rows','authority','applicability','cleanup_delegation')
ADMIN_KEYS = {'zero','permission_initial','grant','permission_check','revoke','permission_final'}

class TimeoutFactory:
    ADMIN_KEYS = ADMIN_KEYS
    def __init__(self, table, expected_principal=None, *, allow_current_identity=False,
                 resource_kind='provisioned'):
        need(re.fullmatch(r'odbcpp_timeout_gate_[A-Za-z0-9_]+', table) is not None,
             'admitted_timeout_table')
        need(type(allow_current_identity) is bool and resource_kind in ('provisioned', 'serverless'),
             'timeout_identity_mode')
        self.resource_kind = resource_kind
        self.allow_current_identity = allow_current_identity
        self.TABLE = table
        self.EXPECTED_PRINCIPAL = None
        if allow_current_identity:
            need(resource_kind == 'serverless' and expected_principal is None,
                 'current_identity_serverless_only')
            return
        need(type(expected_principal) is dict and set(expected_principal) == {'database','owner','owner_oid','schema_oid','schema_owner_oid'}
             and expected_principal['database'] == DATABASE
             and expected_principal['owner'] == ORDINARY, 'admitted_timeout_principal')
        self.TABLE = table
        self.EXPECTED_PRINCIPAL = dict(expected_principal)
        for key in ('owner_oid','schema_oid','schema_owner_oid'):
            self.EXPECTED_PRINCIPAL[key] = str(self.positive(expected_principal[key]))

    def bind_current_principal(self, value):
        """Bind once from the admitted read-only permission row before any DDL.

        The fixed SQL resolves existing exact names. Empty/missing or duplicate
        rows are rejected by the caller's exact-one-row CSV parser. No role or
        schema is created to satisfy this discovery.
        """
        need(self.allow_current_identity and self.EXPECTED_PRINCIPAL is None,
             'current_identity_one_use')
        self.permission(value, absent='t')
        self.EXPECTED_PRINCIPAL = {key: value[key] for key in
            ('database', 'owner', 'owner_oid', 'schema_oid', 'schema_owner_oid')}
        return dict(self.EXPECTED_PRINCIPAL)

    def _bind_oids(self, queries):
        need(self.EXPECTED_PRINCIPAL is not None, 'timeout_current_identity_unbound')
        result = {}
        for key, (sql, headers, maximum, count) in queries.items():
            fields = {'107830': 'schema_oid', '101': 'owner_oid', '100': 'schema_owner_oid'}
            sql = re.sub(r'\b(?:107830|101|100)\b',
                         lambda match: self.EXPECTED_PRINCIPAL[fields[match[0]]], sql)
            result[key] = (sql, headers, maximum, count)
        return result

    def positive(self, value):
        need(type(value) in (str, int) and (not isinstance(value, bool)) and (re.fullmatch('[1-9][0-9]{0,9}', str(value)) is not None) and (int(value) <= 4294967295), 'positive_actual_OID')
        return int(value)

    def table_name(self, value):
        need(value == self.TABLE, 'exact_fresh_c_only')
        return value

    def preflight(self, value):
        need(type(value) is dict and set(value) == {'database', 'owner', 'owner_oid', 'schema_oid', 'schema_owner_oid', 'usage', 'create', 'absent'}, 'preflight_fields')
        need(all((value[k] == v for k, v in self.EXPECTED_PRINCIPAL.items())) and all((value[k] == 't' for k in ('usage', 'create', 'absent'))), 'ordinary_privilege_fresh_absence')
        for k in ('owner_oid', 'schema_oid', 'schema_owner_oid'):
            self.positive(value[k])
        return value

    def identity(self, value, pre, table):
        need(type(value) is dict and set(value) == {'database', 'owner', 'owner_oid', 'schema_oid', 'schema_owner_oid', 'table', 'oid', 'kind', 'shape'}, 'identity_fields')
        need(all((value[k] == pre[k] for k in ('database', 'owner', 'owner_oid', 'schema_oid', 'schema_owner_oid'))) and value['table'] == table and (value['kind'] == 'r') and (value['shape'] == 't'), 'actual_owner_schema_kind_shape')
        self.positive(value['oid'])
        return value

    def custody_record(self, table, ident, pins):
        self.table_name(table)
        need(ident['table'] == table and ident['database'] == DATABASE and (ident['owner'] == ORDINARY) and (ident['kind'] == 'r') and (ident['shape'] == 't'), 'custody_identity')
        need(set(pins) == set(RECEIPTS), 'six_receipts')
        for p in pins.values():
            need(type(p) is dict and set(p) in ({'path', 'sha256'}, {'path', 'sha256', 'bytes'}) and (type(p['path']) is str) and Path(p['path']).is_absolute() and ('..' not in Path(p['path']).parts) and (re.fullmatch('[0-9a-f]{64}', p['sha256']) is not None), 'receipt_pin')
            if 'bytes' in p:
                need(type(p['bytes']) is int and 0 <= p['bytes'] <= 16777216, 'receipt_size')
        return dict(database=DATABASE, schema=SCHEMA, schema_oid=self.positive(ident['schema_oid']), schema_owner_oid=self.positive(ident['schema_owner_oid']), table=table, oid=self.positive(ident['oid']), owner=ORDINARY, owner_oid=self.positive(ident['owner_oid']), kind='r', columns=[{'name': 'value', 'type': 'integer', 'nullable': False, 'default': None}], rows={'count': 1, 'min': 1, 'max': 1}, create_ack=True, cleanup_delegated=True, receipts=pins)

    def permission(self, value, initial=None, create=None, absent=None):
        need(type(value) is dict and set(value) == {'database', 'owner', 'owner_oid', 'schema_oid', 'schema_owner_oid', 'usage', 'create', 'absent'}, 'permission_fields')
        need(value['database'] == DATABASE and value['owner'] == ORDINARY and (value['usage'] == 't') and (value['create'] in ('t', 'f')) and (value['absent'] in ('t', 'f')), 'permission_identity_usage')
        for k in ('owner_oid', 'schema_oid', 'schema_owner_oid'):
            self.positive(value[k])
        if initial is not None:
            need(all((value[k] == initial[k] for k in ('database', 'owner', 'owner_oid', 'schema_oid', 'schema_owner_oid', 'usage'))), 'permission_identity_changed')
        if create is not None:
            need(value['create'] == create, 'permission_effective_state')
        if absent is not None:
            need(value['absent'] == absent, 'permission_relation_state')
        return value

    def query_set(self, table, facts):
        self.table_name(table)
        lit = "'" + table + "'"
        name = '"' + SCHEMA + '"."' + table + '"'
        ph = ('database', 'owner', 'owner_oid', 'schema_oid', 'schema_owner_oid', 'usage', 'create', 'absent')
        permission_sql = 'SELECT current_database() AS database,TRIM(u.usename) AS owner,u.usesysid AS owner_oid,n.oid AS schema_oid,n.nspowner AS schema_owner_oid,has_schema_privilege(\'odbcpp_pilot_test\',\'odbcpp_fixture\',\'USAGE\') AS usage,has_schema_privilege(\'odbcpp_pilot_test\',\'odbcpp_fixture\',\'CREATE\') AS "create",NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class c WHERE c.relnamespace=n.oid AND c.relname=' + lit + ") AS absent FROM pg_catalog.pg_namespace n CROSS JOIN pg_catalog.pg_user u WHERE n.nspname='odbcpp_fixture' AND u.usename='odbcpp_pilot_test' LIMIT 2;"
        inspect = 'SELECT current_database() AS database,TRIM(current_user) AS owner,u.usesysid AS owner_oid,n.oid AS schema_oid,n.nspowner AS schema_owner_oid,has_schema_privilege(current_user,\'odbcpp_fixture\',\'USAGE\') AS usage,has_schema_privilege(current_user,\'odbcpp_fixture\',\'CREATE\') AS "create",NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class c WHERE c.relnamespace=n.oid AND c.relname=' + lit + ") AS absent FROM pg_catalog.pg_namespace n CROSS JOIN pg_catalog.pg_user u WHERE n.nspname='odbcpp_fixture' AND u.usename=current_user LIMIT 2;"
        base = "current_database()='odbcpp_pilot' AND EXISTS(SELECT 1 FROM pg_catalog.pg_namespace WHERE nspname='odbcpp_fixture' AND oid=107830 AND nspowner=100) AND EXISTS(SELECT 1 FROM pg_catalog.pg_user WHERE usename='odbcpp_pilot_test' AND usesysid=101) AND has_schema_privilege('odbcpp_pilot_test','odbcpp_fixture','USAGE')"

        def guard(predicate, body, label='held'):
            return 'SELECT (' + predicate + ') AS ' + label + '\n\\gset\n\\if :' + label + '\n' + body + '\n\\else\n\\quit 4\n\\endif'
        q = {'permission_initial': (permission_sql, ph, 1, 4), 'permission_check': (permission_sql, ph, 1, 4), 'permission_final': (permission_sql, ph, 1, 4), 'inspect': (inspect, ph, 1, 4)}
        if self.EXPECTED_PRINCIPAL is None:
            need(self.allow_current_identity and not facts, 'unbound_identity_before_mutation_only')
            # A current OID is evidence only for the exact existing namespace
            # owned by the admitted database administrator, never a foreign
            # schema found under a familiar name.
            current_sql = permission_sql.replace(' LIMIT 2;',
                " AND n.nspowner=(SELECT usesysid FROM pg_catalog.pg_user WHERE usename='odbcpp_pilot_admin') LIMIT 2;")
            return {'permission_initial': (current_sql, ph, 1, 4)}
        if 'permission' not in facts:
            return self._bind_oids(q)
        initial = self.permission(facts['permission'], absent='t')
        need(all((initial[k] == v for k, v in self.EXPECTED_PRINCIPAL.items())), 'exact_initial_principal_schema')
        if initial['create'] == 'f':
            q['grant'] = (guard(base + " AND NOT has_schema_privilege('odbcpp_pilot_test','odbcpp_fixture','CREATE')", 'GRANT CREATE ON SCHEMA "odbcpp_fixture" TO "odbcpp_pilot_test";\nSELECT \'grant_ACK\' AS ack;'), ('ack',), 1, 6)
            q['revoke'] = (guard(base + " AND has_schema_privilege('odbcpp_pilot_test','odbcpp_fixture','CREATE')", 'REVOKE CREATE ON SCHEMA "odbcpp_fixture" FROM "odbcpp_pilot_test";\nSELECT \'revoke_ACK\' AS ack;'), ('ack',), 1, 6)
        if 'pre' not in facts:
            return self._bind_oids(q)
        self.preflight(facts['pre'])
        absent = 'NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class WHERE relnamespace=107830 AND relname=' + lit + ')'
        q['create'] = (guard(base + " AND current_user='odbcpp_pilot_test' AND has_schema_privilege(current_user,'odbcpp_fixture','CREATE') AND " + absent, 'CREATE TABLE ' + name + " (value INTEGER NOT NULL);\nSELECT 'create_ACK' AS ack;"), ('ack',), 1, 6)
        ident = 'SELECT current_database() AS database,TRIM(current_user) AS owner,c.relowner AS owner_oid,n.oid AS schema_oid,n.nspowner AS schema_owner_oid,c.relname AS "table",c.oid AS oid,c.relkind AS kind FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname=\'odbcpp_fixture\' AND c.relname=' + lit + ' LIMIT 2;'
        ih = ('database', 'owner', 'owner_oid', 'schema_oid', 'schema_owner_oid', 'table', 'oid', 'kind')
        q['identity'] = (ident, ih, 1, 4)
        q['identity_after'] = (ident, ih, 1, 4)
        q['shape'] = ("SELECT ordinal_position,column_name,LOWER(TRIM(data_type)) AS data_type,is_nullable,COALESCE(column_default,'') AS column_default FROM svv_columns WHERE table_catalog='odbcpp_pilot' AND table_schema='odbcpp_fixture' AND table_name=" + lit + ' ORDER BY ordinal_position LIMIT 2;', ('ordinal_position', 'column_name', 'data_type', 'is_nullable', 'column_default'), 2, 4)
        if 'identity' not in facts:
            return self._bind_oids(q)
        i = self.identity(facts['identity'], facts['pre'], table)
        held = base + " AND current_user='odbcpp_pilot_test' AND EXISTS(SELECT 1 FROM pg_catalog.pg_class WHERE relnamespace=107830 AND relname=" + lit + ' AND oid=' + str(self.positive(i['oid'])) + " AND relowner=101 AND relkind='r')"
        q['insert'] = (guard(held, guard('(SELECT COUNT(*) FROM ' + name + ')=0', 'INSERT INTO ' + name + " (value) VALUES (1);\nSELECT 'insert_ACK' AS ack;", 'rows_empty')), ('ack',), 1, 7)
        q['rows'] = (guard(held, 'SELECT COUNT(*) AS count,MIN(value) AS min,MAX(value) AS max FROM ' + name + ';'), ('count', 'min', 'max'), 1, 5)
        q['locks'] = ('SELECT COUNT(*) AS locks FROM svv_transactions WHERE relation=' + str(self.positive(i['oid'])) + " AND lockable_object_type='relation' AND txn_db='odbcpp_pilot';", ('locks',), 1, 4)
        return self._bind_oids(q)
