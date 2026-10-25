"""Offline partition completeness and exact fixture lifecycle controls."""
import copy
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.development import redshift_powerbi_partition as partition
from tools.development.redshift_ctest_lifecycle import Refused, validate_fixture_plan


class PartitionTests(unittest.TestCase):
    def setUp(self):
        self.head = 'a' * 40
        self.rows = [dict(id='ordinary', image='base', profile='core',
                          gtest_filter='Existing.CompleteCase')]
        for profile in ('unixodbc', 'iodbc'):
            self.rows.extend(dict(id=profile + str(number), image='relationship',
                                  profile=profile, gtest_filter=case)
                             for number, case in enumerate(partition.CASES))
        self.full = dict(schema_version=1, cases=self.rows)
        self.manifest = dict(schema_version=1, source_head=self.head, partitions={
            'native': ['ordinary'],
            'powerbi_relationship': [row['id'] for row in self.rows[1:]]})
        self.selected = dict(schema_version=1, cases=self.rows[1:])

    def check(self):
        return partition.validate_partition(self.manifest, self.full, self.head,
                                            'powerbi_relationship', self.selected)

    def test_complete_union_preserves_both_cases_for_each_profile(self):
        self.assertEqual(self.check(), self.selected['cases'])
        partition.validate_partition(self.manifest, self.full, self.head, 'native',
                                     dict(schema_version=1, cases=self.rows[:1]))

    def test_omission_overlap_reordering_and_wrong_source_are_refused(self):
        for failure in ('omitted', 'overlap', 'reorder', 'source', 'extra', 'partial_profile'):
            with self.subTest(failure=failure):
                self.setUp()
                if failure == 'omitted':
                    self.manifest['partitions']['native'] = []
                elif failure == 'overlap':
                    self.manifest['partitions']['native'].append(self.rows[1]['id'])
                elif failure == 'reorder':
                    self.manifest['partitions']['powerbi_relationship'].reverse()
                elif failure == 'source':
                    self.manifest['source_head'] = 'b' * 40
                elif failure == 'extra':
                    self.manifest['partitions']['extra'] = ['ordinary']
                else:
                    self.rows.pop()
                    self.manifest['partitions']['powerbi_relationship'].pop()
                with self.assertRaises(Refused):
                    self.check()

    def test_changed_case_or_selected_inventory_is_refused(self):
        for failure in ('filter', 'duplicate', 'subset', 'selected_order'):
            with self.subTest(failure=failure):
                self.setUp()
                self.selected = copy.deepcopy(self.selected)
                if failure == 'filter':
                    self.rows[1]['gtest_filter'] = 'Unrelated.Case'
                elif failure == 'duplicate':
                    self.rows[2].update(self.rows[1])
                elif failure == 'subset':
                    self.selected['cases'].pop()
                else:
                    self.selected['cases'].reverse()
                with self.assertRaises(Refused):
                    self.check()

    def test_complete_operational_reserve_stays_within_unchanged_caps(self):
        plan = validate_fixture_plan(partition.plan())
        self.assertEqual((plan['total_helpers'], plan['total_SQL'],
                          plan['total_logins'], plan['total_inclusive']), (15, 124, 15, 139))
        self.assertEqual(plan['maximum_cleanup_targets'], 8)

    def test_only_own_DSN_profile_accepts_actual_distinct_OIDs(self):
        observed = dict(ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP='1',
                        ODBCPP_REDSHIFT_POWERBI_PARENT_OID='6001',
                        ODBCPP_REDSHIFT_POWERBI_FACT_OID='6002')
        profile = dict(private_input={'kind': 'OWN_DSN'}, uses_private_dsn=True,
                       connection_environment=['ODBCPP_TEST_DSN'],
                       variables={key: '1' for key in observed})
        self.assertEqual(partition.profile_variables(profile, observed), observed)
        for failure in ('driver_kind', 'connection', 'marker', 'same_oid', 'bad_oid'):
            with self.subTest(failure=failure):
                bad = copy.deepcopy(profile)
                values = dict(observed)
                if failure == 'driver_kind':
                    bad['private_input']['kind'] = 'CORE_ENV'
                elif failure == 'connection':
                    bad['connection_environment'] = ['ODBCPP_OTHER']
                elif failure == 'marker':
                    bad['variables'].pop('ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP')
                elif failure == 'same_oid':
                    values['ODBCPP_REDSHIFT_POWERBI_FACT_OID'] = '6001'
                else:
                    values['ODBCPP_REDSHIFT_POWERBI_PARENT_OID'] = '4294967296'
                with self.assertRaises(Refused):
                    partition.profile_variables(bad, values)


class FixtureLifecycleTests(unittest.TestCase):
    def ops(self):
        ops = SimpleNamespace(relationship_partition=True, fixture_state=None,
                              powerbi_relationship_state=None, readers_known=True,
                              principals_quiescent=True)
        calls = []

        def step(kind, deadline):
            calls.append((kind, deadline))
            if kind == 'inspect':
                ops.powerbi_relationship_state = dict(pending=None,
                    parent=dict(status='ABSENT'), fact=dict(status='ABSENT'))
            elif kind.startswith('create_'):
                ops.powerbi_relationship_state[kind[7:]]['status'] = 'OWNED'
            elif kind.startswith('drop_'):
                ops.powerbi_relationship_state[kind[5:]]['status'] = 'DROPPED'
        ops.powerbi_relationship_step = step
        return ops, calls

    def test_fixed_prepare_reverse_cleanup_and_original_deadline(self):
        ops, calls = self.ops()
        state = partition.prepare(ops, 123)
        self.assertTrue(state['prepared'])
        self.assertTrue(partition.cleanup(ops, state, 456)['complete'])
        self.assertEqual(calls, [(kind, 123) for kind in
            ('inspect', 'create_parent', 'create_fact', 'verify')] +
            [('drop_fact', 456), ('drop_parent', 456)])

    def test_partial_owned_parent_is_cleaned_without_adopting_unknown_fact(self):
        ops, calls = self.ops()
        ops.powerbi_relationship_step('inspect', 123)
        ops.powerbi_relationship_step('create_parent', 123)
        self.assertTrue(partition.cleanup(ops, {}, 456)['complete'])
        self.assertEqual(calls[-1], ('drop_parent', 456))
        ops, calls = self.ops()
        partition.prepare(ops, 123)
        ops.powerbi_relationship_state['pending'] = {'unknown_create': True}
        with self.assertRaisesRegex(Refused, 'unknown_intent'):
            partition.cleanup(ops, {}, 456)
        self.assertEqual(len(calls), 4)

    def test_cleanup_requires_actual_quiescence_and_leaves_shared_schema(self):
        ops, calls = self.ops()
        partition.prepare(ops, 123)
        ops.principals_quiescent = False
        with self.assertRaisesRegex(Refused, 'not_quiescent'):
            partition.cleanup(ops, {}, 456)
        self.assertEqual(len(calls), 4)
        ops.principals_quiescent = True
        self.assertEqual(partition.cleanup(ops, {}, 456)['shared_schema'], 'RETAINED')


if __name__ == '__main__':
    unittest.main()
