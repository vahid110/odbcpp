"""Offline fixture custody/count tests. No database or provider calls."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import redshift_ctest_fixtures as fixtures
import redshift_ctest_lifecycle as lifecycle


FACTORY = Path('/private/tmp/odbcpp-current44-combined-live001/source074/'
               'factory-proposed003.py')


def load_factory():
    spec = importlib.util.spec_from_file_location('accepted_fixture_sql', FACTORY)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class Fixtures(unittest.TestCase):
    def test_complete_plan_refuses_old_caps_before_operational_method(self):
        class NoEffects:
            def now(self):
                return 0.0

            def __getattr__(self, name):
                raise AssertionError('operational method called: ' + name)

        config = SimpleNamespace(bounds=lifecycle.Bounds())
        with self.assertRaisesRegex(lifecycle.Refused, 'helper_cap20'):
            lifecycle.run_once(config, NoEffects(), fixtures)

    def test_actual_factory_costs_and_full_branch_total(self):
        f = load_factory()
        plan = f.closed_fixture_plan()
        records = {k: f.new_fixture_record(k) for k in fixtures.KEYS}
        groups = fixtures._groups(fixtures.KEYS)
        inspect = sum(f.whole_batch_stage(plan, g, 'inspect', records)
                      ['statements'] + 3 for g in groups)
        for k in fixtures.KEYS:
            records[k]['state'] = ('VERIFIED_ABSENT' if k in f.WHOLE_CHECK_IDS
                                   else 'READ_ONLY_REUSED')
        ordinary = sum(f.whole_batch_stage(plan, g, 'ordinary', records)
                       ['statements'] + 3 for g in groups)
        missing = [k for k in fixtures.KEYS if k not in f.WHOLE_CHECK_IDS and
                   k != 'SHARED_FIXTURE_SCHEMA']
        for k in missing:
            records[k]['state'] = 'MISSING'
        create = f.whole_batch_stage(plan, missing, 'create', records)['statements'] + 3
        for k in missing:
            records[k].update(state='OWNED', custody={
                'oid': 123, 'owner_oid': 100, 'shape_sha256': '0' * 64})
        cleanup = sum(f.whole_batch_stage(plan, g, 'cleanup', records)
                      ['statements'] + 3 for g in groups)
        self.assertEqual((inspect, ordinary, create, cleanup), (28, 56, 64, 56))
        two_db = sum(f.two_db_stage(k, None if k == 'databases' else 'dev')
                     ['statements'] + 3 for k in ('databases', 'schemas'))
        self.assertEqual(two_db, 9)
        public = fixtures.plan()
        self.assertEqual(sum(v['helpers'] for v in public['branches'].values()), 36)
        self.assertEqual(sum(v['SQL'] for v in public['branches'].values()), 356)
        self.assertEqual(public['total_inclusive'], 392)

    def test_prepare_publishes_partial_custody_before_failure(self):
        f = load_factory()
        ops = SimpleNamespace(factory=f, timeout_factory=SimpleNamespace(TABLE='fresh'),
                              readers_known=True, sql_unknown=False, sql_fenced=False,
                              now=lambda: 0.0, record=lambda event: None)
        def fail(stage, deadline):
            self.assertIsNotNone(ops.fixture_state)
            # Actual CREATE_UNKNOWN custody would likewise survive this exception.
            ops.fixture_state['records']['NO_KEY']['state'] = 'CREATE_UNKNOWN'
            raise TimeoutError()
        ops.sql = fail
        with self.assertRaises(fixtures.FixtureError) as caught:
            fixtures.prepare(ops, 100.0)
        self.assertIs(caught.exception.state, ops.fixture_state)
        self.assertEqual(ops.fixture_state['records']['NO_KEY']['state'], 'CREATE_UNKNOWN')

    def test_unknown_readers_prevent_any_cleanup_SQL(self):
        f = load_factory()
        state = {'plan': f.closed_fixture_plan(), 'events': [], 'helpers': 0, 'SQL': 0,
                 'records': {k: f.new_fixture_record(k) for k in fixtures.KEYS},
                 'timeout': {'grant': 'UNTOUCHED', 'object': 'UNTOUCHED'},
                 'database_create': {'state': 'UNTOUCHED'}}
        state['records']['NO_KEY']['state'] = 'OWNED'
        state['records']['NO_KEY']['custody'] = {
            'oid': 123, 'owner_oid': 100, 'shape_sha256': '0' * 64}
        def forbidden(*args):
            raise AssertionError('SQL dispatched with unknown readers')
        ops = SimpleNamespace(factory=f, principals_quiescent=True, readers_known=False,
                              now=lambda: 0.0, sql=forbidden, record=lambda event: None)
        receipt = fixtures.cleanup(ops, state, 100.0)
        self.assertFalse(receipt['complete'])
        self.assertEqual(state['records']['NO_KEY']['state'], 'OWNED')

    def test_preexisting_database_grant_is_preserved(self):
        f = load_factory()
        state = {'plan': f.closed_fixture_plan(), 'events': [], 'helpers': 0, 'SQL': 0,
                 'records': {k: f.new_fixture_record(k) for k in fixtures.KEYS},
                 'timeout': {'grant': 'UNTOUCHED', 'object': 'UNTOUCHED'},
                 'database_create': {'state': 'PREEXISTING_TRUE', 'initial': 't'}}
        ops = SimpleNamespace(factory=f, principals_quiescent=True,
                              record=lambda event: None)
        with patch.object(fixtures, '_db_sql', side_effect=AssertionError('REVOKE')):
            receipt = fixtures.cleanup(ops, state, 100.0)
        self.assertTrue(receipt['complete'])
        self.assertEqual(state['database_create']['state'], 'PREEXISTING_TRUE')

    def test_timeout_csv_rejects_extra_row_and_preserves_owned_grant(self):
        state = {'events': [], 'helpers': 0, 'SQL': 0, 'timeout': {
            'table': 'fresh', 'facts': {}, 'receipts': {}, 'grant': 'OWNED'}}
        t = SimpleNamespace(ADMIN_KEYS={'revoke'}, query_set=lambda *args: {
            'revoke': ('SELECT 1;', ('ack',), 1, 6)})
        ops = SimpleNamespace(timeout_factory=t, readers_known=True, sql_unknown=False,
                              sql_fenced=False, now=lambda: 0.0,
                              sql=lambda *args: 'ack\nrevoke_ACK\nextra\n',
                              record=lambda event: None)
        with self.assertRaises(fixtures.FixtureError):
            fixtures._restore_schema_grant(ops, state, 100.0)
        self.assertEqual(state['timeout']['grant'], 'REVOKE_UNKNOWN')


if __name__ == '__main__':
    unittest.main()
