"""Closed relationship fixture partition; CTest remains the case executor.

This is operational orchestration, not a driver or SDK contract. It neither
opens private inputs nor grants admission. The full checkpoint inventory and
the ordered disjoint partition union must be pinned by the normal entry point.
"""
import copy
import re

from tools.development import redshift_powerbi_relationship as relationship
from tools.development.redshift_ctest_lifecycle import need

FIELDS = ('id', 'image', 'profile', 'gtest_filter')
PARTITIONS = ('native', 'powerbi_relationship')
CASES = tuple(relationship.public_plan()['native_cases'])


def validate_partition(manifest, full, head, selected, expected):
    """Prove an ordered partition belongs to one complete checkpoint union."""
    need(type(manifest) is dict and set(manifest) ==
         {'schema_version', 'source_head', 'partitions'} and
         manifest['schema_version'] == 1 and manifest['source_head'] == head,
         'checkpoint_partition_source')
    need(type(full) is dict and full.get('schema_version') == 1 and
         type(full.get('cases')) is list and bool(full['cases']),
         'checkpoint_full_inventory')
    need(selected in PARTITIONS and type(manifest['partitions']) is dict and
         set(manifest['partitions']) == set(PARTITIONS), 'checkpoint_partition_names')
    rows = full['cases']
    need(all(type(row) is dict and set(row) == set(FIELDS) and
             all(type(row[key]) is str and bool(row[key]) for key in FIELDS)
             for row in rows), 'checkpoint_case_schema')
    ids = [row['id'] for row in rows]
    identities = [tuple(row[key] for key in FIELDS[1:]) for row in rows]
    need(len(set(ids)) == len(ids) and len(set(identities)) == len(identities),
         'checkpoint_duplicate_case')
    union = []
    for name in PARTITIONS:
        chosen = manifest['partitions'][name]
        need(type(chosen) is list and bool(chosen) and
             all(type(value) is str and value in ids for value in chosen) and
             len(chosen) == len(set(chosen)), 'checkpoint_partition_members')
        need(chosen == [value for value in ids if value in chosen],
             'checkpoint_partition_order')
        union.extend(chosen)
    need(len(union) == len(ids) and len(set(union)) == len(ids) and
         set(union) == set(ids), 'checkpoint_complete_disjoint_union')
    relation_ids = manifest['partitions']['powerbi_relationship']
    relation_rows = [row for row in rows if row['id'] in relation_ids]
    need(all(row['gtest_filter'] in CASES for row in relation_rows) and
         all(row['gtest_filter'] not in CASES for row in rows
             if row['id'] not in relation_ids), 'checkpoint_relationship_members')
    for profile in {row['profile'] for row in relation_rows}:
        pair = [row for row in relation_rows if row['profile'] == profile]
        need(len(pair) == 2 and {row['gtest_filter'] for row in pair} == set(CASES) and
             len({row['image'] for row in pair}) == 1,
             'checkpoint_complete_relationship_profile')
    selected_rows = [row for row in rows
                     if row['id'] in manifest['partitions'][selected]]
    need(type(expected) is dict and expected.get('schema_version') == 1 and
         expected.get('cases') == selected_rows, 'checkpoint_selected_inventory')
    return copy.deepcopy(selected_rows)


def profile_variables(profile, observed):
    """Only an admitted own-driver DSN profile can run these real DM cases."""
    need(profile['private_input']['kind'] == 'OWN_DSN' and
         profile['uses_private_dsn'] is True and
         profile['connection_environment'] == ['ODBCPP_TEST_DSN'] and
         profile['variables'].get('ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP') == '1',
         'relationship_own_DSN_profile')
    need(set(observed) == {'ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP',
                          'ODBCPP_REDSHIFT_POWERBI_PARENT_OID',
                          'ODBCPP_REDSHIFT_POWERBI_FACT_OID'} and
         observed['ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP'] == '1',
         'relationship_observed_variables')
    for name in ('ODBCPP_REDSHIFT_POWERBI_PARENT_OID',
                 'ODBCPP_REDSHIFT_POWERBI_FACT_OID'):
        value = observed[name]
        need(type(value) is str and re.fullmatch('[1-9][0-9]{0,9}', value) is not None
             and int(value) <= 4294967295 and name in profile['variables'],
             'relationship_observed_OID')
    need(observed['ODBCPP_REDSHIFT_POWERBI_PARENT_OID'] !=
         observed['ODBCPP_REDSHIFT_POWERBI_FACT_OID'], 'relationship_distinct_OIDs')
    return dict(profile['variables'], **observed)


def plan(resource_kind='provisioned'):
    need(resource_kind in ('provisioned', 'serverless'), 'relationship_plan_resource_kind')
    if resource_kind == 'serverless':
        from tools.development.redshift_ctest_ops import serverless_fixture_reserve
        return dict(serverless_fixture_reserve('powerbi_relationship'),
                    relationship_fixture=relationship.public_plan(),
                    fixture_scope='powerbi_relationship')
    # Fixed fixture6/62SQL; conservative existing startup5/23SQL; cleanup:
    # PID capture4SQL + max8 guarded termination19SQL + two zero probes8SQL.
    # Wrapper SQL is already included. One login accompanies each helper.
    return {'total_helpers': 15, 'total_SQL': 116, 'total_logins': 15,
            'total_inclusive': 131, 'network_helpers_separate': 1,
            'relationship_fixture': relationship.public_plan(),
            'startup_reserve': {'helpers': 5, 'SQL': 23},
            'principal_cleanup_reserve': {'helpers': 4, 'SQL': 31},
            'maximum_cleanup_targets': 8, 'fixture_scope': 'powerbi_relationship',
            'ordinary_case_execution': 'CTest only'}


def prepare(ops, deadline):
    need(ops.relationship_partition is True, 'relationship_partition_disabled')
    state = {'prepared': False, 'substitutions': {},
             'fixture_scope': 'powerbi_relationship'}
    ops.fixture_state = state
    for kind in ('inspect', 'create_parent', 'create_fact', 'verify'):
        ops.powerbi_relationship_step(kind, deadline)
    state['prepared'] = True
    return state


def cleanup(ops, state, deadline):
    held = ops.powerbi_relationship_state
    if held is None:
        return {'complete': True, 'created': False, 'shared_schema': 'RETAINED'}
    need(ops.relationship_partition is True and ops.readers_known is True and
         ops.principals_quiescent is True, 'relationship_cleanup_not_quiescent')
    need(held['pending'] is None, 'relationship_unknown_intent_retained')
    for key in ('fact', 'parent'):
        need(held[key]['status'] in ('ABSENT', 'OWNED', 'DROPPED'),
             'relationship_unknown_object_retained')
        if held[key]['status'] == 'OWNED':
            ops.powerbi_relationship_step('drop_' + key, deadline)
            held = ops.powerbi_relationship_state
    need(held['pending'] is None and all(held[key]['status'] in ('ABSENT', 'DROPPED')
                                        for key in ('parent', 'fact')),
         'relationship_cleanup_incomplete')
    return {'complete': True, 'state': copy.deepcopy(held), 'shared_schema': 'RETAINED'}
