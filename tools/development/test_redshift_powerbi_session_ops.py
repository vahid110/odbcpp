"""Offline real-adapter dispatch/counter/deadline controls with fake SQL I/O."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.development import redshift_powerbi_sessions as sessions
from tools.development import redshift_powerbi_session_ops as runtime
from tools.development.redshift_ctest_lifecycle import Refused, validate_fixture_plan
from tools.development.test_redshift_ctest_ops import adapter
from tools.development import test_redshift_powerbi_sessions as data


class SessionRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='odbcpp-session-dispatch-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.directory = self.root / 'powerbi007-own-offline'
        self.directory.mkdir()
        self.native_base = self.root / 'native'
        native_root = patch('tools.development.redshift_powerbi_bridge.BASE', self.native_base)
        native_root.start()
        self.addCleanup(native_root.stop)
        self.ops = object.__new__(runtime.PowerBiSessionOperations)
        self.ops.__dict__.update(adapter(self.directory).__dict__)
        self.ops.powerbi_session_state = data.new_state()
        self.ops.powerbi_session_partition = self.ops.relationship_partition = True
        self.ops.session_execution_end = 1100
        self.ops.session_cleanup_end = 2000
        self.ops.maximum_cleanup_targets = 16
        self.ops.session_sequence = 0
        self.ops.session_armed = False
        self.ops.events = 0
        self.ops.principals_quiescent = True
        self.clock = data.BASE_MS + 1
        self.ops.utc_ms = lambda: self.clock
        self.calls = []
        self.pid = 999
        self.bad_ack = False
        self.original_cap = '2'
        self.revalidation_cap = None
        original_process = self.ops.native.owned_process

        def process(argv, **kwargs):
            held = self.ops.powerbi_session_state
            kind = held['pending']['stage']['stage'].removeprefix('powerbi_sessions_')
            intent = self.directory / ('powerbi-sessions-' +
                str(self.ops.session_sequence) + '-intent.json')
            self.assertEqual(json.loads(intent.read_text()), json.loads(json.dumps(held)))
            self.assertFalse(self.ops.session_armed)
            payload = data.payload(held['pending']['before'], kind,
                                   cap=self.original_cap if kind == 'snapshot' else
                                       self.revalidation_cap if kind == 'baseline_revalidate' else None)
            if self.bad_ack:
                payload = payload.replace('RSPBIS1|ALTER_ACK\n', '')
            self.ops.native.raw = ('RSD1|odbcpp_pilot|odbcpp_pilot_admin|' +
                                   str(self.pid) + '\n' + payload).encode()
            self.calls.append(kind)
            self.clock += 1
            return original_process(argv, **kwargs)
        self.ops.native.owned_process = process

    def snapshot(self):
        return self.ops.session_step('snapshot', 500)

    def decide(self):
        value = data.measurement()
        for key in ('client_demand', 'server_census', 'review'):
            path = self.directory / (key + '.json')
            path.write_text('{"synthetic_offline_only":true}')
            value[key] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        path = self.directory / 'measurement.json'
        path.write_text(json.dumps(value))
        return self.ops.decide_session_cap(dict(path=str(path),
            sha256=hashlib.sha256(path.read_bytes()).hexdigest()), 500)

    def retained_descriptor(self, *, baseline_padding=0):
        def write(name, value, *, directory=None):
            path = (directory or self.directory) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(value))
            return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        self.ops.approval.update(source_head='e' * 40, run_name='powerbi007-own-offline',
                                 run_directory=str(self.directory))
        baseline = write('baseline-closed.json', dict(source_head='e' * 40,
            run_id='powerbi007-own-offline', phase=0, normal_close=True,
            workload_sha256='a' * 64, process=dict(readers_known=True,
                receipt=dict(treeEmpty=True, jobActiveProcesses=0, heldProcessHandlesStopped=True),
                held_observed_processes=[dict(stopped=True)],
                capture_evidence='x' * baseline_padding)), directory=self.native_base / self.directory.name)
        proofs = {name: write('zero-' + name + '.json', dict(synthetic_offline_zero=True))
                  for name in ('raw', 'intent', 'status')}
        self.ops.proofs['principal_zero'] = proofs
        zero = write('baseline-exact-zero.json', dict(source_head='e' * 40,
            run_id='powerbi007-own-offline', native_baseline=baseline,
            exact_principal_zero=True, SQL_proof=proofs))
        review = write('retention-review.json', dict(synthetic_offline_review=True))
        descriptor = data.retention(int(self.original_cap))
        descriptor.update(baseline_closed=baseline, exact_zero=zero, review=review)
        return write('retention.json', descriptor), descriptor

    def test_retained_original_path_rechecks_actual_cap_and_keeps_no_alter_cost_below_reserve(self):
        self.snapshot()
        ref, descriptor = self.retained_descriptor()
        state = self.ops.decide_session_cap(ref, 500)
        self.assertEqual(self.calls, ['snapshot', 'baseline_revalidate'])
        self.assertIsNone(state['measurement'])
        self.assertEqual(state['retention'], descriptor)
        self.assertIsNone(state['temporary_cap'])
        decision = json.loads((self.directory / 'powerbi-sessions-3-decision.json').read_text())
        self.assertEqual(decision['revalidation_SQL_proof'],
                         self.ops.proofs['powerbi_sessions_baseline_revalidate'])
        for event in sessions.EVENTS:
            self.ops.session_step('sample_' + event, 500)
        self.assertEqual(self.ops.restore_session_cap(1500)['phase'], 'CAP_VERIFIED_UNCHANGED')
        self.assertEqual(self.calls, ['snapshot', 'baseline_revalidate', 'sample_connected',
                                     'sample_navigator', 'sample_refresh', 'quiesce'])
        self.assertEqual((self.ops.helpers, self.ops.statements, self.ops.logins), (6, 48, 6))
        for _, call in self.ops.native.calls:
            self.assertNotIn('ALTER USER', call['input_bytes'].decode())

    def test_retained_nested_paths_reject_traversal_sibling_and_outside_before_evidence_IO(self):
        self.snapshot()
        ref, original = self.retained_descriptor()
        decision_path = Path(ref['path'])
        real_open = runtime.os.open
        for key in ('baseline_closed', 'exact_zero', 'review'):
            expected = Path(original[key]['path'])
            for wrong in (str(expected.parent / '..' / expected.name),
                          str(expected.parent.parent / 'powerbi007-own-sibling' / expected.name),
                          '/unrelated/retention-evidence.json'):
                with self.subTest(key=key, path=wrong):
                    value = copy.deepcopy(original)
                    value[key]['path'] = wrong
                    decision_path.write_text(json.dumps(value))
                    changed = dict(path=str(decision_path), sha256=hashlib.sha256(decision_path.read_bytes()).hexdigest())
                    with patch.object(runtime.os, 'open', wraps=real_open) as opened:
                        with self.assertRaisesRegex(Refused, 'fixed_evidence_path'):
                            self.ops.decide_session_cap(changed, 500)
                    self.assertTrue(all(Path(call.args[0]) == decision_path for call in opened.call_args_list))
                    self.assertEqual(self.calls, ['snapshot'])
                    self.assertEqual(self.ops.powerbi_session_state['phase'], 'OBSERVED')

    def test_retained_decision_and_run_component_refuse_before_any_IO(self):
        self.snapshot()
        ref, _ = self.retained_descriptor()
        for wrong in (str(self.directory / '..' / 'retention.json'),
                      str(self.directory.parent / 'powerbi007-own-sibling' / 'retention.json'),
                      '/unrelated/retention.json'):
            with self.subTest(path=wrong), patch.object(runtime, 'verify_public_pin') as verify, \
                    patch.object(runtime.os, 'open') as opened:
                with self.assertRaisesRegex(Refused, 'fixed_evidence_path'):
                    self.ops.decide_session_cap(dict(ref, path=wrong), 500)
                verify.assert_not_called()
                opened.assert_not_called()
        for run in ('../powerbi007-own-offline', 'powerbi007-own-../sibling',
                    'powerbi007-own-sibling', 'foreign'):
            with self.subTest(run=run), patch.object(runtime, 'verify_public_pin') as verify, \
                    patch.object(runtime.os, 'open') as opened:
                self.ops.approval['run_name'] = run
                with self.assertRaisesRegex(Refused, 'actual_admitted_run'):
                    self.ops.decide_session_cap(ref, 500)
                verify.assert_not_called()
                opened.assert_not_called()
        self.assertEqual(self.calls, ['snapshot'])

    def test_retained_closed_paths_cannot_be_symlinks(self):
        self.snapshot()
        ref, value = self.retained_descriptor()
        for target in (ref, value['baseline_closed'], value['exact_zero'], value['review']):
            path = Path(target['path'])
            held = path.with_name(path.name + '.original')
            path.rename(held)
            try:
                try:
                    path.symlink_to(held)
                except (OSError, NotImplementedError):
                    self.skipTest('symlink creation unavailable')
                with self.assertRaises((OSError, Refused)):
                    self.ops.decide_session_cap(ref, 500)
                self.assertEqual(self.calls, ['snapshot'])
            finally:
                if path.is_symlink():
                    path.unlink()
                held.rename(path)

    def test_retained_descriptor_requires_actual_current_baseline_SQL_proof_before_read(self):
        self.snapshot()
        ref, _ = self.retained_descriptor()
        self.ops.proofs['principal_zero']['status'] = dict(path='/foreign', sha256='b' * 64)
        with self.assertRaisesRegex(Refused, 'current_baseline_zero'):
            self.ops.decide_session_cap(ref, 500)
        self.assertEqual(self.calls, ['snapshot'])
        self.assertEqual(self.ops.powerbi_session_state['phase'], 'OBSERVED')

    def test_serverless_retained_path_actual_adapter_cost_keeps_existing_worst_case_reserve(self):
        self.ops.resource_kind = 'serverless'
        self.ops.powerbi_session_state['resource_kind'] = 'serverless'
        self.snapshot()
        ref, _ = self.retained_descriptor()
        self.ops.decide_session_cap(ref, 500)
        for event in sessions.EVENTS:
            self.ops.session_step('sample_' + event, 500)
        self.assertEqual(self.ops.restore_session_cap(1500)['phase'], 'CAP_VERIFIED_UNCHANGED')
        self.assertEqual((self.ops.helpers, self.ops.statements, self.ops.logins), (6, 54, 6))
        cost = sessions.public_plan('serverless')['maximum_retained_path']
        self.assertEqual(cost['inclusive'], 60)
        self.assertLess(cost['inclusive'], sessions.public_plan('serverless')['maximum_changed_path']['inclusive'])

    def test_actual_baseline_process_evidence_uses_existing_two_MiB_contract(self):
        self.snapshot()
        ref, value = self.retained_descriptor(baseline_padding=32768)
        self.assertGreater(Path(value['baseline_closed']['path']).stat().st_size, 16384)
        state = self.ops.decide_session_cap(ref, 500)
        self.assertIsNone(state['measurement'])
        self.assertEqual(self.calls, ['snapshot', 'baseline_revalidate'])

    def test_retained_revalidation_cannot_adopt_changed_cap_or_retry_pending_read(self):
        self.snapshot()
        ref, _ = self.retained_descriptor()
        self.revalidation_cap = '3'
        with self.assertRaisesRegex(sessions.Refused, 'held_identity_or_cap_changed'):
            self.ops.decide_session_cap(ref, 500)
        self.assertIsNotNone(self.ops.powerbi_session_state['pending'])
        with self.assertRaises(sessions.Refused):
            self.ops.decide_session_cap(ref, 500)
        self.assertEqual(self.calls, ['snapshot', 'baseline_revalidate'])

    def test_retained_decision_pins_reader_state_and_freshness_cannot_be_bypassed(self):
        self.snapshot()
        ref, value = self.retained_descriptor()
        self.ops.native.readers_known = False
        with self.assertRaisesRegex(Refused, 'known_baseline_readers'):
            self.ops.decide_session_cap(ref, 500)
        self.ops.native.readers_known = True
        Path(value['review']['path']).write_text('{"changed":true}')
        with self.assertRaises(Refused):
            self.ops.decide_session_cap(ref, 500)
        self.assertEqual(self.calls, ['snapshot'])

    def test_full_fixed_path_durable_intents_exact_restore_and_real_counters(self):
        self.snapshot()
        self.decide()
        self.ops.session_step('set_cap', 500)
        for event in sessions.EVENTS:
            self.ops.session_step('sample_' + event, 500)
        result = self.ops.restore_session_cap(1500)
        self.assertTrue(result['complete'])
        self.assertEqual(result['original']['test_cap'], '2')
        self.assertEqual(self.calls, list(sessions.STAGES))
        self.assertEqual((self.ops.helpers, self.ops.statements, self.ops.logins), (7, 70, 7))
        self.assertFalse(self.ops.sql_fenced)

    def test_unarmed_or_changed_stage_cannot_dispatch(self):
        stage = sessions.stage_for(self.ops.powerbi_session_state, 'snapshot')
        with self.assertRaisesRegex(Refused, 'not_armed'):
            self.ops.sql(stage, 500)
        self.ops.powerbi_session_state = sessions.begin(
            self.ops.powerbi_session_state, stage, now_ms=self.clock)
        self.ops.session_armed = True
        changed = dict(stage, query=stage['query'] + ' SELECT 1;')
        with self.assertRaisesRegex(Refused, 'SQL_changed'):
            self.ops.sql(changed, 500)
        self.assertEqual(self.calls, [])

    def test_SELF_is_bound_to_actual_RSD1_helper_PID(self):
        self.pid = 998
        with self.assertRaisesRegex(Refused, 'PID_mismatch'):
            self.snapshot()
        self.assertIsNotNone(self.ops.powerbi_session_state['pending'])
        with self.assertRaises(sessions.Refused):
            self.snapshot()
        self.assertEqual(self.calls, ['snapshot'])

    def test_missing_mutation_ACK_retains_unknown_and_fences_all_new_SQL(self):
        self.snapshot()
        self.decide()
        self.bad_ack = True
        with self.assertRaises(sessions.Refused):
            self.ops.session_step('set_cap', 500)
        self.assertEqual(self.ops.powerbi_session_state['phase'], 'ALTER_UNKNOWN')
        self.assertTrue(self.ops.sql_unknown and self.ops.sql_fenced)
        with self.assertRaisesRegex(Refused, 'requires_reconciliation'):
            self.ops.restore_session_cap(1500)

    def test_original_monotonic_and_UTC_horizons_both_apply(self):
        with self.assertRaisesRegex(Refused, 'monotonic_deadline'):
            self.ops.session_step('snapshot', 1101)
        self.clock = data.EXEC_END
        with self.assertRaises(sessions.Refused):
            self.snapshot()
        self.assertEqual(self.calls, [])

    def test_no_intent_publication_means_no_native_dispatch(self):
        with patch.object(self.ops, '_publish_session', side_effect=OSError):
            with self.assertRaises(OSError):
                self.snapshot()
        self.assertEqual(self.calls, [])
        self.assertIsNotNone(self.ops.powerbi_session_state['pending'])

    def test_restoration_requires_stopped_readers_and_actual_principal_zero(self):
        self.snapshot()
        self.decide()
        self.ops.session_step('set_cap', 500)
        self.ops.principals_quiescent = False
        with self.assertRaisesRegex(Refused, 'principal_zero'):
            self.ops.restore_session_cap(1500)
        self.assertEqual(self.calls, ['snapshot', 'set_cap'])

    def test_changed_measurement_or_evidence_is_not_a_cap_decision(self):
        self.snapshot()
        with patch.object(runtime, 'verify_public_pin', side_effect=Refused('changed')):
            with self.assertRaisesRegex(Refused, 'changed'):
                self.decide()
        self.assertEqual(self.ops.powerbi_session_state['phase'], 'OBSERVED')

    def test_separate_app_reserve_is_within_unchanged_global_caps(self):
        result = validate_fixture_plan(runtime.plan())
        self.assertEqual((result['total_helpers'], result['total_SQL'],
                          result['total_logins'], result['total_inclusive']), (22, 210, 22, 232))
        self.assertEqual(result['maximum_cleanup_targets'], 16)

    def test_extended_cleanup_is_scoped_and_uses_captured_birth_guards(self):
        rows = [(100 + i, 'odbcpp_pilot_test', 'odbcpp_pilot',
                 '2026-10-22 12:00:01') for i in range(16)]
        raw = ''.join('|'.join(map(str, row)) + '\n' for row in rows)
        self.ops._sql = Mock(side_effect=[(raw, 999),
            (''.join(str(row[0]) + '|t\n' for row in rows), 999), ('f|f\n', 999)])
        self.assertTrue(self.ops.cleanup_principals(500)['complete'])
        captured = self.ops._sql.call_args_list
        self.assertIn('LIMIT 17;', captured[0].args[0]['query'])
        query = captured[1].args[0]['query']
        self.assertEqual(captured[1].args[0]['statements'], 32)
        self.assertIn("starttime=TIMESTAMP '2026-10-22 12:00:01'", query)
        self.assertIn('process<>pg_backend_pid()', query)
        self.ops.powerbi_session_partition = False
        self.ops._sql.reset_mock()
        with self.assertRaisesRegex(Refused, 'requires_app_partition'):
            self.ops.cleanup_principals(500)
        self.ops._sql.assert_not_called()

    def test_normal_cleanup_mandatorily_restores_without_optional_caller_step(self):
        self.snapshot()
        self.decide()
        self.ops.session_step('set_cap',500)
        with patch.object(runtime.Operations,'cleanup_principals',
                          return_value={'complete':True}) as principal_cleanup:
            result=self.ops.cleanup_principals(1500)
            principal_cleanup.assert_called_once_with(1500)
            self.assertTrue(result['complete'])
            self.assertEqual(result['cap_cleanup']['phase'],'CAP_RESTORED')
            self.assertEqual(self.calls[-2:],['quiesce','restore'])
            calls=list(self.calls)
            # Reuse the completed cap proof; never replay ALTER or its intent.
            self.assertEqual(self.ops.cleanup_principals(1500)['cap_cleanup']['phase'],
                             'CAP_RESTORED')
            self.assertEqual(self.calls,calls)

    def test_exact_original_cap_is_verified_unchanged_by_normal_cleanup(self):
        self.original_cap='10'
        self.snapshot()
        self.decide()
        with patch.object(runtime.Operations,'cleanup_principals',return_value={'complete':True}):
            result=self.ops.cleanup_principals(1500)
        self.assertEqual(result['cap_cleanup']['phase'],'CAP_VERIFIED_UNCHANGED')
        self.assertEqual(self.calls,['snapshot','quiesce'])

    def test_untouched_setup_and_unknown_intent_have_truthful_cleanup_dispositions(self):
        with patch.object(runtime.Operations,'cleanup_principals',return_value={'complete':True}):
            result=self.ops.cleanup_principals(1500)
            self.assertEqual(result['cap_cleanup']['phase'],'OWNER_CAP_UNTOUCHED')
            self.assertFalse(result['cap_cleanup']['original_cap_freshly_verified'])
            self.assertEqual(self.calls,[])
            state=self.ops.powerbi_session_state
            self.ops.powerbi_session_state=sessions.begin(state,
                sessions.stage_for(state,'snapshot'),now_ms=self.clock)
            with self.assertRaisesRegex(Refused,'intent_blocks_cleanup'):
                self.ops.cleanup_principals(1500)

    def test_failed_principal_cleanup_never_restores_a_cap_while_readers_may_remain(self):
        self.snapshot()
        self.decide()
        self.ops.session_step('set_cap',500)
        with patch.object(runtime.Operations,'cleanup_principals',return_value={'complete':False}):
            with self.assertRaisesRegex(Refused,'principal_cleanup_incomplete'):
                self.ops.cleanup_principals(1500)
        self.assertEqual(self.calls,['snapshot','set_cap'])


if __name__ == '__main__':
    unittest.main()
