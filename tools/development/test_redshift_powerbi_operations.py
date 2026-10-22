"""Offline POSIX resource-owner dispatch controls; no provider or feature execution."""
import copy
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.development import redshift_ctest_ops as operations
from tools.development import redshift_powerbi_relationship as relationship
from tools.development.test_redshift_powerbi_relationship import observed


class RelationshipOperationsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='odbcpp-relationship-controls-')
        self.addCleanup(self.temporary.cleanup)
        self.ops = object.__new__(operations.Operations)
        self.ops.directory = Path(self.temporary.name)
        self.ops.relationship_partition = True
        self.ops.powerbi_relationship_state = None
        self.ops.powerbi_relationship_sequence = 0
        self.ops.powerbi_relationship_armed = False
        self.ops.powerbi_relationship_verified = False
        self.ops.fixture_state = None
        self.ops.readers_known = self.ops.principals_quiescent = True
        self.ops.native = SimpleNamespace(readers_known=True)
        self.clock = 10
        self.ops.now = lambda: self.clock
        self.calls = []
        self.response = None

        def fake_native(stage, deadline):
            intent = self.ops.powerbi_relationship_state
            path = self.ops.directory / ('powerbi-relationship-' +
                str(self.ops.powerbi_relationship_sequence) + '-intent.json')
            self.assertTrue(path.is_file(), 'Durable intent must precede dispatch')
            self.assertEqual(json.loads(path.read_text()), json.loads(json.dumps(intent)))
            self.assertFalse(self.ops.powerbi_relationship_armed, 'Dispatch must already be consumed')
            self.calls.append(copy.deepcopy(stage))
            if self.response:
                return self.response(stage, deadline)
            kind = stage['stage'].removeprefix('powerbi_relationship_')
            return observed(kind, intent['pending']['before']), 1234

        self.ops._sql = fake_native

    def step(self, kind):
        return self.ops.powerbi_relationship_step(kind, 100)

    def owned(self):
        for kind in ('inspect', 'create_parent', 'create_fact', 'verify'):
            self.step(kind)

    def test_actual_wrapper_publishes_intent_before_dispatch_and_exact_completion(self):
        self.owned()
        self.assertEqual(self.ops.powerbi_relationship_variables(), {
            'ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP': '1',
            'ODBCPP_REDSHIFT_POWERBI_PARENT_OID': '30',
            'ODBCPP_REDSHIFT_POWERBI_FACT_OID': '31'})
        self.step('drop_fact')
        self.step('drop_parent')
        self.assertEqual(len(self.calls), 6)
        self.assertEqual(len(list(self.ops.directory.glob('*-intent.json'))), 6)
        self.assertEqual(len(list(self.ops.directory.glob('*-complete.json'))), 6)
        self.assertTrue(all(self.ops.powerbi_relationship_state[k]['status'] == 'DROPPED'
                            for k in ('parent', 'fact')))

    def test_partition_default_off_does_not_dispatch_or_write(self):
        self.ops.relationship_partition = False
        with self.assertRaisesRegex(operations.Refused, 'partition_disabled'):
            self.step('inspect')
        self.assertEqual(self.calls, [])
        self.assertEqual(list(self.ops.directory.iterdir()), [])

    def test_unarmed_sql_or_changed_sql_cannot_reach_native(self):
        state = relationship.new_state()
        stage = relationship.stage_for(state, 'inspect')
        with self.assertRaises(operations.Refused):
            self.ops.sql(stage, 100)
        self.ops.powerbi_relationship_state = relationship.begin(state, stage)
        self.ops.powerbi_relationship_armed = True
        changed = dict(stage, query=stage['query'] + 'SELECT 999;')
        with self.assertRaisesRegex(operations.Refused, 'SQL_changed'):
            self.ops.sql(changed, 100)
        self.assertEqual(self.calls, [])

    def test_native_failure_consumes_dispatch_and_preserves_unknown_without_retry(self):
        self.step('inspect')
        def fail(stage, deadline):
            raise RuntimeError('synthetic native failure')
        self.response = fail
        with self.assertRaises(RuntimeError):
            self.step('create_parent')
        self.assertEqual(self.ops.powerbi_relationship_state['parent']['status'], 'CREATE_UNKNOWN')
        self.assertFalse(self.ops.powerbi_relationship_armed)
        with self.assertRaises(relationship.Refused):
            self.step('create_parent')
        self.assertEqual(len(self.calls), 2)
        with self.assertRaises(operations.Refused):
            self.ops.sql(self.calls[-1], 100)

    def test_incomplete_ack_cannot_become_owned(self):
        self.step('inspect')
        self.response = lambda stage, deadline: (
            observed('create_parent', self.ops.powerbi_relationship_state['pending']['before'])
                .replace('RSPBI1|CREATE_ACK\n', ''), 1234)
        with self.assertRaisesRegex(relationship.Refused, 'ack_required'):
            self.step('create_parent')
        self.assertEqual(self.ops.powerbi_relationship_state['parent']['status'], 'CREATE_UNKNOWN')
        self.assertFalse((self.ops.directory / 'powerbi-relationship-2-complete.json').exists())

    def test_success_after_original_deadline_stays_unknown(self):
        self.step('inspect')
        def late(stage, deadline):
            raw = observed('create_parent', self.ops.powerbi_relationship_state['pending']['before'])
            self.clock = 100
            return raw, 1234
        self.response = late
        with self.assertRaisesRegex(operations.Refused, 'completion_deadline'):
            self.step('create_parent')
        self.assertEqual(self.ops.powerbi_relationship_state['parent']['status'], 'CREATE_UNKNOWN')
        self.assertFalse(self.ops.powerbi_relationship_verified)

    def test_failed_intent_publication_prevents_native_dispatch(self):
        with patch.object(operations, 'record', side_effect=OSError('synthetic disk failure')):
            with self.assertRaises(OSError):
                self.step('inspect')
        self.assertEqual(self.calls, [])
        self.assertIsNotNone(self.ops.powerbi_relationship_state['pending'])
        self.assertFalse(self.ops.powerbi_relationship_armed)

    def test_drop_requires_actual_quiescence_and_both_reader_statuses(self):
        self.owned()
        for owner, attribute in ((self.ops, 'principals_quiescent'),
                                 (self.ops, 'readers_known'), (self.ops.native, 'readers_known')):
            with self.subTest(attribute=attribute):
                setattr(owner, attribute, False)
                with self.assertRaisesRegex(operations.Refused, 'not_quiescent'):
                    self.step('drop_fact')
                setattr(owner, attribute, True)
        self.assertEqual(len(self.calls), 4)

    def test_fixture_identity_values_are_unavailable_before_current_verification(self):
        for kind in ('inspect', 'create_parent', 'create_fact'):
            self.step(kind)
            with self.assertRaises(operations.Refused):
                self.ops.powerbi_relationship_variables()
        self.step('verify')
        self.assertEqual(self.ops.powerbi_relationship_variables()['ODBCPP_REDSHIFT_POWERBI_PARENT_OID'], '30')


if __name__ == '__main__':
    unittest.main()
