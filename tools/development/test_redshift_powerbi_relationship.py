"""Inert custody/SQL-planner controls; no driver feature or cloud assertions."""
from copy import deepcopy
import unittest

import redshift_powerbi_relationship as fixture

SESSION = 'odbcpp_pilot|odbcpp_pilot_admin|10|11|20|10'


def frames(**values):
    return ''.join('RSPBI1|' + key + '\n' + ''.join(line + '\n' for line in lines)
                   for key, lines in values.items())


def observed(kind, state):
    result = {'SESSION': [SESSION]}
    if kind == 'inspect':
        result.update(PARENT_IDENTITY=[], FACT_IDENTITY=[])
    elif kind.startswith('create_'):
        key = kind[7:]
        result.update(CREATE_ACK=[], IDENTITY=[('30' if key == 'parent' else '31') + '|10|20|10|r'],
                      SHAPE=list(fixture.SHAPES[key]), ROWS=list(fixture.PARENT_ROWS if key == 'parent' else fixture.FACT_ROWS))
    elif kind == 'verify':
        for key in fixture.NAMES:
            result[key.upper() + '_IDENTITY'] = [str(state[key]['oid']) + '|10|20|10|r']
            result[key.upper() + '_SHAPE'] = list(fixture.SHAPES[key])
            result[key.upper() + '_ROWS'] = list(fixture.PARENT_ROWS if key == 'parent' else fixture.FACT_ROWS)
    else:
        result.update(DROP_ACK=[], IDENTITY=[], OID_ABSENT=['0'], SCHEMA=['20|10'])
    return frames(**result)


def advance(state, kind):
    stage = fixture.stage_for(state, kind)
    return fixture.complete(fixture.begin(state, stage), observed(kind, state))


def owned():
    state = advance(fixture.new_state(), 'inspect')
    state = advance(state, 'create_parent')
    return advance(state, 'create_fact')


class RelationshipCustodyTests(unittest.TestCase):
    def test_complete_lifecycle_keeps_schema_and_reverse_order(self):
        state = owned()
        state = advance(state, 'verify')
        state = advance(state, 'drop_fact')
        state = advance(state, 'drop_parent')
        self.assertEqual(['DROPPED', 'DROPPED'], [state[k]['status'] for k in fixture.NAMES])
        self.assertEqual([10, 11, 20, 10], state['anchors'])

    def test_collision_even_exact_shape_is_never_adopted(self):
        initial = fixture.new_state()
        intent = fixture.begin(initial, fixture.stage_for(initial, 'inspect'))
        payload = observed('inspect', initial).replace('RSPBI1|PARENT_IDENTITY\n', 'RSPBI1|PARENT_IDENTITY\n30|10|20|10|r\n')
        with self.assertRaisesRegex(fixture.Refused, 'collision'):
            fixture.complete(intent, payload)

    def test_no_create_without_inspection_or_parent(self):
        with self.assertRaises(fixture.Refused):
            fixture.stage_for(fixture.new_state(), 'create_parent')
        state = advance(fixture.new_state(), 'inspect')
        with self.assertRaisesRegex(fixture.Refused, 'parent_required'):
            fixture.stage_for(state, 'create_fact')

    def test_unknown_create_blocks_every_new_dispatch(self):
        state = advance(fixture.new_state(), 'inspect')
        intent = fixture.begin(state, fixture.stage_for(state, 'create_parent'))
        self.assertEqual('CREATE_UNKNOWN', intent['parent']['status'])
        for kind in fixture.KINDS:
            with self.subTest(kind=kind), self.assertRaises(fixture.Refused):
                fixture.stage_for(intent, kind)
        payload = observed('create_parent', state).replace('RSPBI1|CREATE_ACK\n', '')
        with self.assertRaisesRegex(fixture.Refused, 'ack_required'):
            fixture.complete(intent, payload)
        self.assertEqual('CREATE_UNKNOWN', intent['parent']['status'])

    def test_wrong_owner_oid_schema_shape_rows_and_principal_refuse(self):
        state = advance(fixture.new_state(), 'inspect')
        intent = fixture.begin(state, fixture.stage_for(state, 'create_parent'))
        payload = observed('create_parent', state)
        for before, after in [('30|10|20|10|r', '30|12|20|10|r'),
                              ('30|10|20|10|r', '30|10|21|10|r'),
                              ('parent_id|integer|NO|NULL', 'parent_id|bigint|NO|NULL'),
                              ('1|North', '1|Foreign'), ('|10|11|20|10', '|10|12|20|10')]:
            with self.subTest(after=after), self.assertRaises(fixture.Refused):
                fixture.complete(intent, payload.replace(before, after))

    def test_current_oid_change_is_not_rebound(self):
        state = owned()
        intent = fixture.begin(state, fixture.stage_for(state, 'verify'))
        with self.assertRaisesRegex(fixture.Refused, 'oid_changed'):
            fixture.complete(intent, observed('verify', state).replace('30|10|20|10|r', '32|10|20|10|r'))

    def test_reverse_dependency_and_unknown_drop_refuse(self):
        state = owned()
        with self.assertRaisesRegex(fixture.Refused, 'reverse_dependency'):
            fixture.stage_for(state, 'drop_parent')
        intent = fixture.begin(state, fixture.stage_for(state, 'drop_fact'))
        self.assertEqual('DROP_UNKNOWN', intent['fact']['status'])
        for replacement in [observed('drop_fact', state).replace('RSPBI1|DROP_ACK\n', ''),
                            observed('drop_fact', state).replace('RSPBI1|OID_ABSENT\n0', 'RSPBI1|OID_ABSENT\n1'),
                            observed('drop_fact', state).replace('20|10\n', '21|10\n')]:
            with self.assertRaises(fixture.Refused):
                fixture.complete(intent, replacement)
        with self.assertRaises(fixture.Refused):
            fixture.stage_for(intent, 'drop_parent')

    def test_duplicate_unframed_refused_and_oversize_payloads(self):
        for payload in ['foreign\n', 'RSPBI1|SESSION\nRSPBI1|SESSION\n',
                        'RSPBI1|ROWS\n' + '1\n' * 9, 'x' * 16385, '\x00']:
            with self.subTest(payload=payload[:40]), self.assertRaises(fixture.Refused):
                fixture.parse_frames(payload)
        state = advance(fixture.new_state(), 'inspect')
        intent = fixture.begin(state, fixture.stage_for(state, 'create_parent'))
        with self.assertRaises(fixture.Refused):
            fixture.complete(intent, observed('create_parent', state) + 'RSPBI1|REFUSED\n')

    def test_modified_stage_and_intent_are_rejected(self):
        state = advance(fixture.new_state(), 'inspect')
        stage = fixture.stage_for(state, 'create_parent')
        for field, value in [('query', 'DROP SCHEMA odbcpp_fixture CASCADE;'),
                             ('role', 'test'), ('statements', 0), ('keys', ('foreign',))]:
            changed = {**stage, field: value}
            with self.subTest(field=field), self.assertRaises(fixture.Refused):
                fixture.begin(state, changed)
        intent = fixture.begin(state, stage)
        intent['pending']['stage']['query'] += 'SELECT 1;'
        with self.assertRaises(fixture.Refused):
            fixture.complete(intent, observed('create_parent', state))

    def test_exact_sql_scope_grants_guards_and_costs(self):
        state = fixture.new_state()
        statements = 0
        for kind in fixture.KINDS:
            stage = fixture.stage_for(state, kind)
            self.assertEqual('admin', stage['role'])
            self.assertLessEqual(len(stage['query'].encode('utf-8')), 16384)
            self.assertNotIn('CASCADE', stage['query'])
            self.assertNotIn('CREATE SCHEMA', stage['query'])
            self.assertNotIn('DROP SCHEMA', stage['query'])
            self.assertNotIn('CREATE USER', stage['query'])
            if kind.startswith('create_'):
                self.assertIn('NOT EXISTS(', stage['query'])
                self.assertIn(' AND usesysid=11)', stage['query'])
                self.assertEqual(1, stage['query'].count('GRANT SELECT ON TABLE'))
                self.assertIn(' TO "odbcpp_pilot_test";', stage['query'])
                self.assertLess(stage['query'].index('INSERT INTO'), stage['query'].index('RSPBI1|CREATE_ACK'))
            if kind.startswith('drop_'):
                self.assertIn(' RESTRICT;', stage['query'])
                self.assertIn('svv_transactions', stage['query'])
                self.assertIn('COUNT(DISTINCT ', stage['query'])
                self.assertIn('column_default IS NULL', stage['query'])
                self.assertIn('n.nspowner=10', stage['query'])
            statements += stage['statements'] + 3
            self.assertEqual(stage['query'].count('\\if :pbi_held'), stage['query'].count('\\endif'))
            state = advance(state, kind)
        self.assertEqual(62, statements)
        self.assertLessEqual(statements, fixture.public_plan()['statement_upper_bound_including_operations_wrappers'])
        self.assertEqual(68, statements + fixture.public_plan()['login_upper_bound'])

    def test_no_timeout_reset_or_resource_executor(self):
        plan = fixture.public_plan()
        self.assertEqual('NOT_EXECUTABLE_UNTIL_ROOT_INTEGRATION', plan['readiness'])
        self.assertTrue(plan['existing_absolute_deadline_required'])
        self.assertTrue(plan['no_automatic_authority_or_runtime_extension'])
        self.assertEqual(6, plan['helpers'])
        self.assertEqual(2, len(plan['native_cases']))
        copy = deepcopy(plan)
        copy['tables']['parent'] = 'foreign'
        self.assertEqual(fixture.PARENT, fixture.public_plan()['tables']['parent'])


if __name__ == '__main__':
    unittest.main()
