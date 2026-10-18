"""Offline census accounting and crash-residual preservation checks."""
import csv
import io
from types import SimpleNamespace
import unittest

import redshift_ctest_native_custody as census


def payload(rows):
    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(census.HEADERS)
    writer.writerows(rows)
    return output.getvalue()


def namespace(oid='1200', owner='101'):
    return ('rs5_cols_123_雪', 'rs5_cols_123_雪', 'n', oid, owner,
            oid, owner, '')


class Census(unittest.TestCase):
    def ops(self, outputs):
        values = iter(outputs)
        return SimpleNamespace(
            now=lambda: 0.0, readers_known=True, principals_quiescent=True,
            sql_unknown=False, sql_fenced=False, record=lambda value: None,
            sql=lambda stage, deadline: next(values))

    def test_two_helpers_exact_cost_and_no_mutation_SQL(self):
        plan = census.plan()
        self.assertEqual((plan['helpers'], plan['SQL'], plan['inclusive']), (2, 8, 10))
        sql = census.stage()['query']
        self.assertEqual(sql.count(';'), 1)
        self.assertNotIn('DROP ', sql)
        self.assertNotIn('svv_', sql.lower())
        self.assertEqual(census.stage()['statements'], 1)

    def test_unchanged_baseline_is_preserved_even_if_foreign_owned(self):
        raw = payload([namespace(owner='777')])
        ops = self.ops([raw, raw])
        state = census.before(ops, 50.0)
        result = census.after(ops, state, 50.0)
        self.assertTrue(result['complete'])
        self.assertEqual(result['residual'], [])

    def test_catalog_snapshot_avoids_boolean_and_oidvector_text_casts(self):
        query = census.stage()['query']
        self.assertNotIn('attnotnull::', query)
        self.assertNotIn('atthasdef::', query)
        self.assertNotIn('proargtypes::', query)
        self.assertIn("CASE WHEN a.attnotnull THEN 't' ELSE 'f' END", query)
        self.assertIn("CASE WHEN a.atthasdef THEN 't' ELSE 'f' END", query)
        self.assertIn("p.pronargs::varchar||'|'||p.prokind::varchar", query)
        self.assertIn('p.prooid::bigint', query)
        self.assertIn('p.proowner::bigint', query)
        before = ('rs5_routine_123_a_雪', 'owned_proc', 'p', '1201', '101',
                  '1200', '101', '1|p')
        replaced = (*before[:3], '1202', *before[4:])
        ops = self.ops([payload([before]), payload([replaced])])
        result = census.after(ops, census.before(ops, 50.0), 50.0)
        self.assertFalse(result['complete'])
        self.assertEqual(result['residual'], [replaced])
        self.assertEqual(result['baseline_removed_or_changed'], [before])

    def test_crash_residual_retains_actual_OID_owner_and_shape_without_DROP(self):
        table = ('rs5_cols_123_雪', 'z_table', 'r', '1201', '101', '1200', '101', '')
        column = ('rs5_cols_123_雪', 'z_table', 'a', '1201', '101', '1200', '101',
                  '1|id|23|-1|t|f')
        rows = [namespace(), table, column]
        ops = self.ops([payload([]), payload(rows)])
        result = census.after(ops, census.before(ops, 50.0), 50.0)
        self.assertFalse(result['complete'])
        self.assertEqual(set(result['residual']), set(rows))
        self.assertTrue(result['all_new_objects_ordinary_owned'])
        self.assertFalse(result['automatic_DROP'])

    def test_changed_or_removed_baseline_cannot_be_clean(self):
        ops = self.ops([payload([namespace()]), payload([namespace(owner='777')])])
        result = census.after(ops, census.before(ops, 50.0), 50.0)
        self.assertFalse(result['complete'])
        self.assertEqual(result['baseline_removed_or_changed'], [namespace()])
        self.assertFalse(result['all_new_objects_ordinary_owned'])

    def test_unknown_readers_or_missing_principal_zero_prevents_post_SQL(self):
        ops = self.ops([payload([])])
        state = census.before(ops, 50.0)
        ops.principals_quiescent = False
        with self.assertRaisesRegex(census.CensusError, 'principal_zero'):
            census.after(ops, state, 50.0)
        ops.principals_quiescent = True
        ops.readers_known = False
        with self.assertRaises(census.CensusError):
            census.after(ops, state, 50.0)

    def test_no_prefix_only_namespace_or_unbounded_output_acceptance(self):
        bad = list(namespace())
        bad[0] = bad[1] = 'rs5_cols_foreign'
        with self.assertRaisesRegex(census.CensusError, 'exact_source'):
            census.parse(payload([bad]))
        with self.assertRaisesRegex(census.CensusError, 'row_bound'):
            census.parse(payload([namespace(str(i + 1)) for i in range(513)]))


if __name__ == '__main__':
    unittest.main()
