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
        self.directory = Path(self.temporary.name)
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
        original_process = self.ops.native.owned_process

        def process(argv, **kwargs):
            held = self.ops.powerbi_session_state
            kind = held['pending']['stage']['stage'].removeprefix('powerbi_sessions_')
            intent = self.directory / ('powerbi-sessions-' +
                str(self.ops.session_sequence) + '-intent.json')
            self.assertEqual(json.loads(intent.read_text()), json.loads(json.dumps(held)))
            self.assertFalse(self.ops.session_armed)
            payload = data.payload(held['pending']['before'], kind,
                                   cap=self.original_cap if kind == 'snapshot' else None)
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
                          result['total_logins'], result['total_inclusive']), (22, 202, 22, 224))
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
