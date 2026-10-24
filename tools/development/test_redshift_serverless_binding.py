"""Composed normal-entry/partial-setup controls, all external effects injected."""
from contextlib import ExitStack
import copy
import datetime as dt
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from tools.development import live_checkpoint as entry
from tools.development import redshift_ctest_ops as operational
from tools.development import redshift_ctest_fixtures as fixtures
from tools.development import redshift_powerbi_partition as partition
from tools.development import redshift_ctest_native_custody as census
from tools.development import redshift_powerbi_app_checkpoint as app_entry
from tools.development import redshift_powerbi_sessions as sessions
from tools.development.redshift_timeout_factory import TimeoutFactory
from tools.development.test_redshift_ctest_ops import adapter
from tools.development.test_redshift_powerbi_relationship import observed


IDENTITY = dict(database='odbcpp_pilot', owner='odbcpp_pilot_test', owner_oid='11',
                schema_oid='20', schema_owner_oid='10')


def identity_csv():
    return ','.join(IDENTITY) + '\n' + ','.join(IDENTITY.values()) + '\n'


class BindingComposition(unittest.TestCase):
    def make_ops(self, directory, *, app=False):
        ops = adapter(directory)
        if app:
            selected = app_entry._components().Operations
            instance = selected.__new__(selected)
            instance.__dict__.update(ops.__dict__)
            ops = instance
            ops.powerbi_session_partition = True
            ops.maximum_cleanup_targets = 16
            ops.powerbi_session_state = sessions.new_state(workload_id='offline015',
                input_profile_sha256='a' * 64, admitted_ms=100,
                execution_deadline_ms=1000, cleanup_deadline_ms=2000, resource_kind='serverless')
        ops.resource_kind = 'serverless'
        ops.native.resume_available = mock.Mock()
        ops.timeout_factory = TimeoutFactory('odbcpp_timeout_gate_synthetic015',
            allow_current_identity=True, resource_kind='serverless')
        ops.record = mock.Mock()
        ops.proofs = {}
        ops.fixture_state = None
        ops.native_custody_state = None
        ops.relationship_partition = True
        ops.powerbi_relationship_state = None
        ops.powerbi_relationship_sequence = 0
        ops.powerbi_relationship_armed = ops.powerbi_relationship_verified = False
        self.calls = []

        def dispatch(stage, deadline):
            self.calls.append(stage['stage'])
            if stage['stage'] == 'current_identity':
                return identity_csv(), 999
            if stage['stage'] == 'principal_zero':
                return 'f\n', 999
            if stage['stage'] == 'principal_pids':
                return '', 999
            if stage['stage'].startswith('powerbi_relationship_'):
                kind = stage['stage'].removeprefix('powerbi_relationship_')
                return observed(kind, ops.powerbi_relationship_state['pending']['before']), 999
            if stage['stage'] == 'native_persistent_census':
                return ','.join(census.HEADERS) + '\n', 999
            raise AssertionError(stage['stage'])
        ops._sql = dispatch
        return ops

    def test_common_resume_binds_before_relationship_mutation_and_native_census(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = self.make_ops(directory)
            ops.resume_available(300)
            self.assertEqual(ops.timeout_factory.EXPECTED_PRINCIPAL, IDENTITY)
            self.assertEqual(self.calls[:2], ['current_identity', 'principal_zero'])
            state = partition.prepare(ops, 300)
            self.assertTrue(state['prepared'])
            held = census.before(ops, 300)
            self.assertEqual(held['owner_oid'], 11)
            self.assertTrue(ops.cleanup_principals(300)['complete'])
            self.assertTrue(partition.cleanup(ops, state, 300)['complete'])
            self.assertEqual(self.calls.count('current_identity'), 1)

    def test_unbound_mutation_is_refused_before_any_dispatch(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = self.make_ops(directory)
            with self.assertRaisesRegex(operational.Refused, 'principals_not_bound'):
                partition.prepare(ops, 300)
            self.assertEqual(self.calls, [])

    def test_app_entry_operations_use_common_binding_without_timeout_fixture_creation(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = self.make_ops(directory, app=True)
            ops.resume_available(300)
            self.assertEqual(ops.timeout_factory.EXPECTED_PRINCIPAL, IDENTITY)
            state = partition.prepare(ops, 300)
            result = ops.cleanup_principals(300)
            self.assertTrue(result['complete'])
            self.assertEqual(result['cap_cleanup']['phase'], 'OWNER_CAP_UNTOUCHED')
            self.assertTrue(partition.cleanup(ops, state, 300)['complete'])
            self.assertEqual(self.calls.count('current_identity'), 1)
            self.assertFalse(any(name.startswith('timeout_') for name in self.calls))

    def test_shared_owned_creation_failure_can_prove_quiescence_and_exact_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            ops = self.make_ops(directory)
            ops.relationship_partition = False
            ops.resume_available(300)
            first = fixtures.KEYS[0]
            def capture(plan, keys, frames, records, creating=False):
                for key in keys:
                    records[key]['state'] = 'OWNED' if creating else ('MISSING' if key == first else 'VERIFIED_ABSENT')
            def stage(plan, keys, kind, records):
                return dict(stage=kind, keys=tuple(keys), role='admin', statements=1, query=kind)
            def ready(plan, keys, frames, records):
                if records[first]['state'] == 'OWNED' and 'cleanup' not in self.calls:
                    raise ValueError('synthetic post-create readiness failure')
            ops.factory = SimpleNamespace(new_fixture_record=lambda key: {'state': 'UNTOUCHED'},
                closed_fixture_plan=lambda: {}, whole_batch_stage=stage, whole_capture=capture,
                whole_frames=lambda raw: {(first, 'DROP_ACK'): [], (first, 'IDENTITY'): [],
                                         (first, 'OID_ABSENT'): ['0']}, whole_validate_ready=ready)
            original = ops._sql
            def dispatch(value, deadline):
                if value['stage'] in ('inspect', 'create', 'ordinary', 'cleanup'):
                    self.calls.append(value['stage'])
                    return '', 999
                return original(value, deadline)
            ops._sql = dispatch
            with self.assertRaises(fixtures.FixtureError) as raised:
                fixtures.prepare(ops, 300)
            state = raised.exception.state
            self.assertEqual(state['records'][first]['state'], 'OWNED')
            for flag in ('readers_known', 'sql_unknown'):
                invalid = copy.deepcopy(state)
                prior = list(self.calls)
                setattr(ops, flag, flag == 'sql_unknown')
                refused = fixtures.cleanup(ops, invalid, 300)
                self.assertFalse(refused['complete'])
                self.assertEqual(invalid['records'][first]['state'], 'OWNED')
                self.assertEqual(self.calls, prior)
                setattr(ops, flag, flag == 'readers_known')
            self.assertTrue(ops.cleanup_principals(300)['complete'])
            receipt = fixtures.cleanup(ops, state, 300)
            self.assertTrue(receipt['complete'], receipt)
            self.assertEqual(receipt['owned_dropped'], [first])
            self.assertLess(self.calls.index('current_identity'), self.calls.index('create'))
            self.assertEqual(ops.timeout_factory.EXPECTED_PRINCIPAL, IDENTITY)

    def test_normal_entry_composes_same_binding_for_native_and_relationship(self):
        for selected in ('native', 'powerbi_relationship'):
            with self.subTest(selected=selected), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                path = root / 'plan.json'
                path.write_text('{}')
                def pin(value, name):
                    file = root / (name + '.json')
                    file.write_text(json.dumps(value))
                    return dict(path=str(file), sha256=hashlib.sha256(file.read_bytes()).hexdigest())
                executable = Path(sys.executable).resolve(strict=True)
                tool = dict(path=str(executable), sha256=hashlib.sha256(executable.read_bytes()).hexdigest())
                plan = dict(source_head='a' * 40, run_name='synthetic015', partition=selected,
                    state_directory=directory, resource={'kind': 'serverless'},
                    tools={key: tool for key in ('cmake', 'ctest', 'psql', 'aws', 'python')},
                    timeout_fixture=dict(table='odbcpp_timeout_gate_synthetic015', expected_principal=None,
                        allow_current_identity=True, resource_kind='serverless'),
                    ca_sha256='a' * 64, security_groups=[], authority_receipts={})
                for name, value in (('profiles', {}), ('specs', []), ('index', {}), ('expected', {}),
                                    ('ci_receipt', {}), ('checkpoint_expected', {}), ('checkpoint_partitions', {})):
                    plan[name] = pin(value, name)
                admission = dict(reviewed=True, plan_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                    source_head=plan['source_head'], inventory_scope='complete_checkpoint_partition',
                    run_name=plan['run_name'], nonce='offline_nonce_015', partition=selected,
                    checkpoint_sha256=plan['checkpoint_expected']['sha256'], bounds={},
                    expires_utc=(dt.datetime.now(dt.timezone.utc) + dt.timedelta(days=1)).isoformat(),
                    credential_source={}, database_config={})
                def run(config, ops, complete):
                    self.assertEqual(ops.resource_kind, 'serverless')
                    self.assertEqual(complete.prepare, partition.prepare if selected == 'powerbi_relationship' else fixtures.prepare)
                    self.assertEqual(complete.cleanup, partition.cleanup if selected == 'powerbi_relationship' else fixtures.cleanup)
                    ops.native = SimpleNamespace(resume_available=lambda end: None)
                    ops.directory.mkdir(mode=0o700)
                    ops.now = lambda: 10
                    ops.sql = lambda stage, end: identity_csv()
                    ops.final_zero = lambda end: {'complete': True}
                    ops.resume_available(100)
                    self.assertEqual(ops.timeout_factory.EXPECTED_PRINCIPAL, IDENTITY)
                    return {'offline_composition': True}
                with mock.patch.object(entry.subprocess, 'check_output', side_effect=['a' * 40 + '\n', '']), \
                     mock.patch('tools.development.redshift_resource.private_json', return_value=admission), \
                     mock.patch.object(entry, 'read_plan', return_value=plan), \
                     mock.patch.object(entry.lifecycle, 'run_once', side_effect=run):
                    self.assertEqual(entry.execute(plan, path, root / 'never-read-admission'), {'offline_composition': True})


if __name__ == '__main__':
    unittest.main(verbosity=2)
