"""Offline Serverless safety controls; no provider, SQL or runtime effects."""
import unittest
from pathlib import Path
from tools.development.redshift_ctest_lifecycle import Refused

from tools.development import redshift_session_sql as sql
from tools.development import redshift_timeout_factory as timeout
from tools.development import redshift_fixture_factory as fixtures
from tools.development import redshift_powerbi_sessions as sessions


class ServerlessSql(unittest.TestCase):
    def permission(self):
        return dict(database='odbcpp_pilot', owner='odbcpp_pilot_test', owner_oid='701',
                    schema_oid='811', schema_owner_oid='700', usage='t', create='f', absent='t')

    def test_name_resolution_is_separate_bounded_and_exact(self):
        query = sql.binding()
        self.assertIn('COUNT(*)=2', query)
        self.assertIn('COUNT(DISTINCT usesysid)=2', query)
        self.assertIn('AND NOT usesuper', query)
        self.assertIn('\\quit 4', query)
        self.assertNotIn('sys_', query)
        for query in (sql.rows(), sql.zero()):
            self.assertNotIn('pg_user_info', query)
            self.assertIn("TRIM(database_name)='odbcpp_pilot'", query)
            self.assertIn('session_id<>pg_backend_pid()', query)
            self.assertIn("status='active'", query)
            self.assertIn("TRY_CAST(NULLIF(TRIM(user_id),'') AS BIGINT)", query)
        self.assertIn("status IN ('planning','queued','running','returning')", sql.zero())
        self.assertNotIn('start_time>', sql.zero())
        held = sql.binding(admin_oid=700, test_oid=701)
        self.assertIn("usename='odbcpp_pilot_test' THEN usesysid END)=701", held)
        self.assertIn("usename='odbcpp_pilot_admin' THEN usesysid END)=700", held)
        with self.assertRaises(ValueError):
            sql.binding(admin_oid=700, test_oid=700)

    def test_live_identity_is_bound_once_and_no_mutation_before_discovery(self):
        factory = timeout.TimeoutFactory('odbcpp_timeout_gate_offline015',
            allow_current_identity=True, resource_kind='serverless')
        queries = factory.query_set(factory.TABLE, {})
        self.assertEqual(set(queries), {'permission_initial'})
        self.assertNotIn('CREATE TABLE', queries['permission_initial'][0])
        value = self.permission()
        factory.bind_current_principal(value)
        queries = factory.query_set(factory.TABLE, {'permission': value})
        self.assertIn('oid=811 AND nspowner=700', queries['grant'][0])
        self.assertIn('usesysid=701', queries['grant'][0])
        self.assertNotIn('107830', queries['grant'][0])
        with self.assertRaises(Refused):
            factory.bind_current_principal(value)

    def test_current_identity_missing_invalid_or_unrelated_fails_closed(self):
        for change in ({'database': 'other'}, {'owner': 'other'}, {'owner_oid': '0'},
                       {'schema_oid': '811 OR true'}, {'usage': 'f'}, {'absent': 'f'}):
            with self.subTest(change=change):
                factory = timeout.TimeoutFactory('odbcpp_timeout_gate_offline015',
                    allow_current_identity=True, resource_kind='serverless')
                with self.assertRaises(Refused):
                    factory.bind_current_principal(self.permission() | change)
                self.assertIsNone(factory.EXPECTED_PRINCIPAL)
        with self.assertRaises(Refused):
            timeout.TimeoutFactory('odbcpp_timeout_gate_offline015', allow_current_identity=True)

    def test_timeout_sys_rewrite_preserves_identity_and_drop_guards(self):
        record = dict(database='odbcpp_pilot', schema='odbcpp_fixture', schema_oid=811,
            schema_owner_oid=700, table='odbcpp_timeout_gate_offline015', oid=900,
            owner='odbcpp_pilot_test', owner_oid=701, kind='r',
            columns=[dict(name='value', type='integer', nullable=False, default=None)],
            rows=dict(count=1, min=1, max=1), create_ack=True, cleanup_delegated=True,
            receipts={key: dict(path=str(Path.cwd() / 'synthetic' / key), sha256='a' * 64) for key in
                ('creation','identity','shape_rows','authority','applicability','cleanup_delegation')})
        for kind in ('timeout_after', 'timeout_cleanup'):
            captured = [dict(pid=41, start='2026-10-24 12:00:00'),
                        dict(pid=42, start='2026-10-24 12:00:01')] if kind == 'timeout_after' else ()
            old = fixtures.timeout_stage(record, kind, captured)
            new = fixtures.timeout_stage(record, kind, captured, resource_kind='serverless')
            self.assertEqual(new['statements'], old['statements'] + 1)
            self.assertNotIn('stv_', new['query'])
            self.assertIn('sys_query_history', new['query'])
            self.assertIn('sys_session_history', new['query'])
            if kind == 'timeout_after':
                birth_query = new['query'].split('RSFIX2|TIMEOUT_GATE|BIRTHS\n', 1)[1].split(';', 1)[0]
                self.assertIn("WHERE status='active' AND session_id IN (41,42)", birth_query)
                self.assertNotIn('pg_backend_pid()', birth_query)
                self.assertNotIn("TRIM(database_name)='", birth_query)
            if kind == 'timeout_cleanup':
                for label in ('timeout_held', 'timeout_locks', 'timeout_shape', 'timeout_rows'):
                    self.assertIn(label, new['query'])
                self.assertIn('c.relowner=701', new['query'])
                self.assertIn('n.oid=811 AND n.nspowner=700', new['query'])

    def test_powerbi_sys_snapshot_retains_closed_frames_and_extra_statement(self):
        state = sessions.new_state(workload_id='offline015', input_profile_sha256='a' * 64,
            admitted_ms=100, execution_deadline_ms=1000, cleanup_deadline_ms=2000,
            resource_kind='serverless')
        stage = sessions.stage_for(state, 'snapshot')
        self.assertEqual(stage['statements'], 6)
        self.assertNotIn('stv_', stage['query'])
        self.assertIn('sys_session_history', stage['query'])
        self.assertIn(" OR TRY_CAST", stage['query'])
        self.assertIn('session_id=pg_backend_pid()', stage['query'])
        self.assertIn('RSPBIS1|USERS', stage['query'])
        self.assertIn('LIMIT 17', stage['query'])
        self.assertEqual(sessions.stage_for({key: value for key, value in state.items()
            if key != 'resource_kind'}, 'snapshot')['statements'], 5)


if __name__ == '__main__':
    unittest.main(verbosity=2)
