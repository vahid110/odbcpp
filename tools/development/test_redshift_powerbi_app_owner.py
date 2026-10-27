"""Offline failure-path controls. No provider, native app, credentials or SQL."""
import importlib.util
import copy
from pathlib import Path
import types
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('app_owner_candidate',
    Path(__file__).with_name('redshift_powerbi_app_owner.py'))
owner = importlib.util.module_from_spec(spec)
import sys
sys.modules[spec.name] = owner
spec.loader.exec_module(owner)


class Ops:
    powerbi_session_partition = True
    session_execution_end = 1800
    session_cleanup_end = 2700

    def __init__(self):
        self.events = []
        self.t = 0
        self.readers_known = True
        self.app_active = False
        self.mutated = False
        self.fixture_state = None
        self.proofs = {'principal_zero': {'raw': 'synthetic'}}
        self.powerbi_session_state = {'original': {'test_cap': '2'}, 'phase': 'NEW'}
        self.native = types.SimpleNamespace(prepare=lambda: self.events.append('prepare_inputs'))

    def now(self):
        self.t += .01
        return self.t

    def record(self, value):
        self.events.append(copy.deepcopy(value))

    def security_paused(self, end):
        self.events.append('security_paused')

    def resume_available(self, end):
        self.events.append('resume')

    def verify_running(self, end):
        self.events.append('verify_running')
        return True

    def session_step(self, kind, end):
        self.events.append(kind)
        if kind == 'set_cap':
            if self.app_active:
                raise AssertionError('ALTER before native baseline closure')
            self.mutated = True
            self.powerbi_session_state['phase'] = 'CAP_ACTIVE'
        return self.powerbi_session_state

    def decide_session_cap(self, pin, end):
        self.events.append('reviewed_demand')
        self.powerbi_session_state.update(phase='DECIDED', temporary_cap=5)
        return self.powerbi_session_state

    def final_zero(self, end):
        if self.app_active:
            raise AssertionError('zero assertion while native app active')
        self.events.append('zero')
        return {'complete': True}

    def reap(self, end):
        self.events.append('reap_helpers')
        return True

    def cleanup_principals(self, end):
        if self.app_active or not self.readers_known:
            raise AssertionError('cleanup while native readers unknown')
        self.events.append('principal_zero_and_restore')
        self.mutated = False
        return {'complete': True, 'cap_cleanup': {'complete': True}}

    def pause_until(self, end):
        self.events.append('pause_verified')
        return True

    def close_inputs(self, end):
        self.events.append('close_inputs')
        return True


class Bridge:
    def __init__(self, ops, events=None):
        self.ops = ops
        self.events = events or [
            {'kind': 'baseline_closed'},
            {'kind': 'reviewed_measurement', 'measurement_pin': {'synthetic': True}},
            {'kind': 'connected'}, {'kind': 'navigator'}, {'kind': 'refresh'},
            {'kind': 'terminal'}]
        self.stopped_readers_known = True

    def stage(self, *args):
        self.ops.events.append('stage_native')
        self.ops.app_active = True

    def announce_ready(self):
        pass

    def poll(self, end):
        if not self.events:
            raise TimeoutError()
        event = self.events.pop(0)
        if event.get('late'):
            self.ops.t = end + 1
        return event

    def verify_baseline_closed(self, event):
        self.ops.events.append('baseline_native_closed')
        self.ops.app_active = False

    def record_baseline_zero(self, *args):
        self.ops.events.append('baseline_zero_evidence')

    def ack_cap(self, *args):
        self.ops.events.append('cap_ready')
        self.ops.app_active = True

    def ack_sample(self, event, *args):
        self.ops.events.append('ack_' + event['kind'])

    def verify_terminal(self, event):
        self.ops.events.append('native_terminal')
        self.ops.app_active = False

    def stop_and_reap(self, end):
        self.ops.events.append('native_stop_and_reap')
        if self.stopped_readers_known:
            self.ops.app_active = False
        return self.stopped_readers_known


class Controls(unittest.TestCase):
    def test_serverless_complete_collection_closes_owned_activity_without_pause(self):
        self.setUp()
        self.ops.resource_kind='serverless'
        def idle(end):
            self.ops.events.append('verify_owned_idle')
            self.ops.final_zero(end)
            return True
        self.ops.verify_idle=idle
        result=self.run_owner()
        self.assertTrue(result['execution_complete'])
        self.assertTrue(result['cleanup_complete'])
        self.assertTrue(result['owned_activity_quiescent'])
        self.assertFalse(result['paused_verified'])
        self.assertFalse(result['physical_compute_idle_verified'])
        self.assertNotIn('pause_verified',self.ops.events)
        self.assertEqual(self.ops.events.count('verify_owned_idle'),1)
        self.assertLess(self.ops.events.index('fixture_retire'),
                        self.ops.events.index('verify_owned_idle'))

    def test_serverless_unknown_reader_principal_cap_or_idle_blocks_cleanup_acceptance(self):
        for failure in ('reader','principal','cap','idle'):
            with self.subTest(failure=failure):
                self.setUp();self.ops.resource_kind='serverless'
                idle=unittest.mock.Mock(return_value=failure!='idle')
                self.ops.verify_idle=idle
                if failure=='reader':
                    self.bridge.events=[{'kind':'foreign'}]
                    self.bridge.stopped_readers_known=False
                elif failure in ('principal','cap'):
                    self.ops.cleanup_principals=lambda end:{'complete':failure!='principal',
                        'cap_cleanup':{'complete':failure!='cap'}}
                result=self.run_owner()
                self.assertFalse(result['cleanup_complete'])
                self.assertFalse(result['owned_activity_quiescent'])
                self.assertFalse(result['paused_verified'])
                self.assertNotIn('pause_verified',self.ops.events)
                if failure!='idle':
                    idle.assert_not_called()

    def test_serverless_working_inputs_retire_only_after_verified_mode_closure(self):
        for idle_known in (True,False):
            with self.subTest(idle_known=idle_known):
                self.setUp();self.ops.resource_kind='serverless'
                self.ops.verify_idle=lambda end:idle_known
                inputs=unittest.mock.Mock()
                inputs.retire.return_value=True
                with patch.object(owner.fixtures,'prepare',return_value={'prepared':True}), \
                     patch.object(owner.fixtures,'cleanup',return_value={'complete':True}):
                    result=owner.collect(self.ops,self.bridge,self.deadlines,working_inputs=inputs)
                self.assertEqual(result['cleanup_complete'],idle_known)
                self.assertFalse(result['paused_verified'])
                inputs.close.assert_called_once()
                if idle_known:
                    inputs.retire.assert_called_once_with(True)
                else:
                    inputs.retire.assert_not_called()

    def setUp(self):
        self.ops = Ops()
        self.bridge = Bridge(self.ops)
        self.deadlines = owner.Deadlines(5400, 4500, 900, 1800, 2700)

    def run_owner(self):
        def prepare(ops, end):
            ops.events.append('fixture_prepare')
            ops.fixture_state = {'prepared': True}
            return ops.fixture_state

        def cleanup(ops, state, end):
            if ops.app_active or not ops.readers_known:
                raise AssertionError('fixture drop while native readers active')
            self.assertIs(state, ops.fixture_state)
            ops.events.append('fixture_retire')
            return {'complete': True}

        with patch.object(owner.fixtures, 'prepare', prepare), patch.object(owner.fixtures, 'cleanup', cleanup):
            return owner.collect(self.ops, self.bridge, self.deadlines)

    def test_complete_flow_raises_cap_only_after_closed_baseline_and_actual_zero(self):
        result = self.run_owner()
        self.assertTrue(result['execution_complete'])
        self.assertTrue(result['cleanup_complete'])
        events = self.ops.events
        self.assertLess(events.index('baseline_native_closed'), events.index('zero'))
        self.assertLess(events.index('zero'), events.index('reviewed_demand'))
        self.assertLess(events.index('set_cap'), events.index('cap_ready'))
        self.assertLess(events.index('native_terminal'), events.index('principal_zero_and_restore'))
        self.assertLess(events.index('principal_zero_and_restore'), events.index('fixture_retire'))
        self.assertFalse(result['app_PASS_claimed'])

    def test_duplicate_sample_fails_execution_but_still_reaps_restores_and_pauses(self):
        self.bridge.events.insert(3, {'kind': 'connected'})
        result = self.run_owner()
        self.assertFalse(result['execution_complete'])
        self.assertTrue(result['execution_errors'])
        self.assertTrue(result['cleanup_complete'])
        self.assertIn('native_stop_and_reap', self.ops.events)
        self.assertFalse(self.ops.mutated)

    def test_unknown_native_readers_retain_inputs_and_fixture_but_attempt_pause(self):
        self.bridge.events = [{'kind': 'foreign'}]
        self.bridge.stopped_readers_known = False
        result = self.run_owner()
        self.assertFalse(result['cleanup_complete'])
        self.assertFalse(result['native_readers_known'])
        self.assertIn('pause_verified', self.ops.events)
        self.assertNotIn('principal_zero_and_restore', self.ops.events)
        self.assertNotIn('fixture_retire', self.ops.events)
        self.assertNotIn('close_inputs', self.ops.events)

    def test_unlimited_original_cap_refuses_native_launch_and_retires_fixtures(self):
        self.ops.powerbi_session_state['original']['test_cap'] = 'UNLIMITED'
        result = self.run_owner()
        self.assertFalse(result['execution_complete'])
        self.assertNotIn('stage_native', self.ops.events)
        self.assertTrue(result['cleanup_complete'])

    def test_measurement_before_native_close_is_rejected(self):
        self.bridge.events = [{'kind': 'reviewed_measurement', 'measurement_pin': {}}]
        result = self.run_owner()
        self.assertFalse(result['execution_complete'])
        self.assertNotIn('reviewed_demand', self.ops.events)
        self.assertTrue(result['cleanup_complete'])

    def test_early_terminal_is_not_complete_operational_inventory(self):
        self.bridge.events = [{'kind': 'terminal'}]
        result = self.run_owner()
        self.assertFalse(result['execution_complete'])
        self.assertTrue(result['cleanup_complete'])

    def test_expired_or_extended_session_horizon_refuses_before_input_open(self):
        self.ops.session_execution_end = 1000
        with self.assertRaisesRegex(RuntimeError, 'original_session_horizons'):
            self.run_owner()
        self.assertNotIn('prepare_inputs', self.ops.events)

    def test_missing_cleanup_reserve_refuses_before_resource_acquisition(self):
        self.deadlines = owner.Deadlines(5400, 4500, 900, 1800, 2500)
        with self.assertRaisesRegex(RuntimeError, 'original_app_cleanup_reserves'):
            self.run_owner()
        self.assertNotIn('prepare_inputs', self.ops.events)

    def test_partial_fixture_prepare_custody_is_not_lost(self):
        held = {'prepared': False, 'parent': 'OWNED'}

        def partial(ops, end):
            ops.fixture_state = held
            raise RuntimeError('prepare interrupted after own create')

        with patch.object(owner.fixtures, 'prepare', partial), patch.object(owner.fixtures, 'cleanup',
                side_effect=lambda ops, state, end: {'complete': state is held}) as cleanup:
            result = owner.collect(self.ops, self.bridge, self.deadlines)
        self.assertFalse(result['execution_complete'])
        self.assertTrue(result['cleanup_complete'])
        self.assertIs(cleanup.call_args.args[1], held)

    def test_late_terminal_cannot_become_execution_success(self):
        self.bridge.events[-1]['late'] = True
        result = self.run_owner()
        self.assertFalse(result['execution_complete'])
        self.assertIn('native_stop_and_reap', self.ops.events)
        self.assertTrue(result['cleanup_complete'])

    def test_lost_resume_completion_rechecks_resource_before_principal_cleanup(self):
        def lost_resume(end):
            self.ops.events.append('resume_effect_then_response_lost')
            raise RuntimeError()
        self.ops.resume_available = lost_resume
        result = self.run_owner()
        self.assertFalse(result['execution_complete'])
        self.assertTrue(result['resource_available'])
        self.assertTrue(result['cleanup_complete'])
        self.assertLess(self.ops.events.index('verify_running'),
                        self.ops.events.index('principal_zero_and_restore'))

    def test_missing_cap_restoration_proof_cannot_close_cleanup(self):
        self.ops.cleanup_principals = lambda end: {'complete': True}
        result = self.run_owner()
        self.assertFalse(result['cleanup_complete'])
        self.assertIn('pause_verified', self.ops.events)

    def test_working_handle_close_error_does_not_skip_terminal_record(self):
        class Inputs:
            def retire(self, known):
                return True
            def close(self):
                raise OSError()
        with patch.object(owner.fixtures, 'prepare', return_value={'prepared': True}), \
                patch.object(owner.fixtures, 'cleanup', return_value={'complete': True}):
            result = owner.collect(self.ops, self.bridge, self.deadlines, working_inputs=Inputs())
        self.assertTrue(result['execution_complete'])
        self.assertFalse(result['cleanup_complete'])
        self.assertEqual('app_resource_terminal', self.ops.events[-1]['kind'])

    def test_successful_terminal_verification_after_deadline_is_not_accepted(self):
        original = self.bridge.verify_terminal
        def late_verifier(event):
            original(event)
            self.ops.t = self.deadlines.execution + 1
        self.bridge.verify_terminal = late_verifier
        result = self.run_owner()
        self.assertFalse(result['execution_complete'])
        self.assertTrue(result['native_readers_known'])
        self.assertTrue(result['cleanup_complete'])
        self.assertNotIn('native_stop_and_reap', self.ops.events)

    def test_successful_terminal_publication_after_owner_deadline_is_not_accepted(self):
        original = self.ops.record
        def late_publication(value):
            original(value)
            if value.get('kind') == 'app_resource_terminal':
                self.ops.t = self.deadlines.owner + 1
        self.ops.record = late_publication
        result = self.run_owner()
        self.assertFalse(result['cleanup_complete'])
        self.assertFalse(result['within_original_owner'])
        self.assertFalse(result['terminal_publication_returned_in_time'])
        self.assertTrue(self.ops.events[-1]['acceptance_requires_postpublication_check'])


if __name__ == '__main__':
    unittest.main()
