"""Offline DATA bridge controls; no app, private credentials, SQL or provider."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

q = Path(__file__).parent
spec = importlib.util.spec_from_file_location('bridge_candidate', q/'redshift_powerbi_bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)
contract_spec = importlib.util.spec_from_file_location('tools.development.windows_powerbi_contract',
    q/'windows_powerbi_contract.py')
contract = importlib.util.module_from_spec(contract_spec)
contract_spec.loader.exec_module(contract)
sys.modules[contract_spec.name] = contract


class Ops:
    def __init__(self):
        self.t = 10
        self.calls = []
        self.native = types.SimpleNamespace(readers_known=True, owned_process=self.process)
        self.proofs = {'powerbi_sessions_sample_connected': {'synthetic': True}}
    def now(self):
        return self.t
    def process(self, argv, **kwargs):
        self.calls.append((argv, kwargs))
        return types.SimpleNamespace(returncode=0, timed_out=False)


class Controls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='odbcpp-public-bridge-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ops = Ops()
        self.ops.directory = self.root
        self.b = bridge.Bridge.__new__(bridge.Bridge)
        self.b.ops = self.ops
        self.b.app = {'comparison_target_cap': None}
        self.b.plan = {'source_head': 'a'*40, 'run_id': 'powerbi007-own-001', 'workload_sha256': 'b'*64}
        self.b.plan_pin = {'path': '/mnt/c/public-plan.json', 'sha256': 'c'*64}
        self.b.plan_windows = bridge.PureWindowsPath('C:/public-plan.json')
        self.b.admission = {'nonce': 'synthetic-nonce-only-001'}
        self.b.run = self.root
        self.b.entry = bridge.PureWindowsPath('C:/Users/vahid/.codex/worktrees/q/odbcpp/tools/development/windows_powerbi_owner.py')
        self.b.python = bridge.PureWindowsPath('C:/Users/vahid/AppData/Local/Programs/Python/Python312/python.exe')
        self.b.calls, self.b.unknown = 0, False
        self.b.seen, self.b.published = set(), set()
        self.b.authority = self.b.baseline = self.b.zero = self.b.measurement = None

    def value(self, stage, phase=0, **extra):
        return dict(schema_version=1, run_id=self.b.plan['run_id'], nonce=self.b.admission['nonce'],
            source_head=self.b.plan['source_head'], plan_sha256=self.b.plan_pin['sha256'],
            stage=stage, phase=phase, **extra)

    def write(self, name, value):
        path = self.root/name
        path.write_text(json.dumps(value))
        return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}

    def test_partial_native_publication_is_not_accepted_or_treated_as_terminal(self):
        (self.root/'baseline-closed.json').write_bytes(b'')
        with patch.object(bridge.time, 'sleep'):
            self.assertIsNone(self.b.poll(100))
        self.assertEqual(set(), self.b.seen)

    def test_foreign_nonce_is_rejected_before_any_ack_or_sql(self):
        self.write('baseline-closed.json', self.value('baseline-closed') | {'nonce': 'foreign'})
        with self.assertRaisesRegex(RuntimeError, 'native_original_run_binding'):
            self.b.poll(100)
        self.assertEqual([], self.ops.calls)

    def test_measurement_handoff_is_once_and_retains_exact_reviewed_pin(self):
        pin = self.write('measurement.json', {'requested_cap': 5})
        self.write('reviewed-measurement.ack.json', self.value('reviewed-measurement', status='ACK', measurement_pin=pin))
        event = self.b.poll(100)
        self.assertEqual(pin, event['measurement_pin'])
        with patch.object(bridge.time, 'sleep'):
            self.assertIsNone(self.b.poll(100))

    def test_measurement_replacement_after_pin_verification_is_rejected(self):
        self.b.app['comparison_target_cap'] = 5
        pin = self.write('measurement.json', {'requested_cap': 6})
        self.write('reviewed-measurement.ack.json', self.value(
            'reviewed-measurement', status='ACK', measurement_pin=pin))
        verify = bridge.verify_public_pin

        def replace_after_verification(path, expected):
            verify(path, expected)
            self.write('measurement.json', {'requested_cap': 5})

        with patch.object(bridge, 'verify_public_pin', side_effect=replace_after_verification):
            with self.assertRaisesRegex(RuntimeError, 'reviewed_measurement_bytes_changed'):
                self.b.poll(100)
        self.assertIsNone(self.b.measurement)
        self.assertNotIn('reviewed_measurement', self.b.seen)
        self.assertEqual([], self.ops.calls)

    def test_oversized_measurement_is_rejected_before_acceptance(self):
        pin = self.write('measurement.json', {'padding': 'x' * (2 * 1024 * 1024)})
        self.write('reviewed-measurement.ack.json', self.value(
            'reviewed-measurement', status='ACK', measurement_pin=pin))
        with self.assertRaisesRegex(RuntimeError, 'bounded_native_public_record'):
            self.b.poll(100)
        self.assertIsNone(self.b.measurement)
        self.assertNotIn('reviewed_measurement', self.b.seen)

    def test_vendor_cannot_change_common_cap_from_referenced_own_run(self):
        self.b.app['comparison_target_cap'] = 5
        pin = self.write('measurement.json', {'requested_cap': 6})
        self.write('reviewed-measurement.ack.json', self.value('reviewed-measurement', status='ACK', measurement_pin=pin))
        with self.assertRaisesRegex(RuntimeError, 'same_actual_comparison_cap'):
            self.b.poll(100)

    def test_timeout_fences_acks_and_does_not_claim_windows_readers_are_absent(self):
        self.ops.native.owned_process = lambda *a, **k: types.SimpleNamespace(returncode=0, timed_out=True)
        with self.assertRaisesRegex(RuntimeError, 'native_control_failed_or_unknown'):
            self.b._command('publish-ack', 100, {})
        self.assertTrue(self.b.unknown)
        self.assertFalse(self.ops.native.readers_known)
        with self.assertRaisesRegex(RuntimeError, 'fixed_native_data_control'):
            self.b._command('publish-ack', 100, {})

    def test_bridge_cannot_launch_gui_or_replay_a_published_stage(self):
        with self.assertRaisesRegex(RuntimeError, 'fixed_native_data_control'):
            self.b._command('run', 100)
        self.b._publish({'stage': 'cap-ready'}, 100)
        with self.assertRaisesRegex(RuntimeError, 'oneuse_native_ack'):
            self.b._publish({'stage': 'cap-ready'}, 100)
        self.assertEqual(1, len(self.ops.calls))

    def test_seven_wsldata_calls_preserve_separate_eighth_measurement_publisher(self):
        self.b.calls = 7
        with self.assertRaisesRegex(RuntimeError, 'native_bridge_process_reserve'):
            self.b._command('stage', 100)
        self.assertEqual([], self.ops.calls)

    def test_baseline_requires_actual_empty_job_and_stopped_handles(self):
        process = dict(readers_known=True, receipt=dict(treeEmpty=True, jobActiveProcesses=0,
            heldProcessHandlesStopped=True), held_observed_processes=[])
        event = dict(record=self.value('baseline-closed', normal_close=True, process=process,
                     workload_sha256='b'*64), pin={'path': 'synthetic', 'sha256': 'd'*64})
        self.b.verify_baseline_closed(event)
        process['receipt']['heldProcessHandlesStopped'] = False
        with self.assertRaisesRegex(RuntimeError, 'actual_baseline_reader_proof'):
            self.b.verify_baseline_closed(event)

    def test_sample_ack_preserves_actual_phase_request_and_sql_proof(self):
        request = self.value('sample-connected', phase=1)
        pin = self.write('sample-connected.native-request.json', request)
        event = dict(kind='connected', record=request, pin=pin)
        self.b.ack_sample(event, {'phase': 'CAP_ACTIVE'}, 100)
        ack = json.loads(self.ops.calls[0][1]['input_bytes'])
        self.assertEqual('sample-connected', ack['stage'])
        self.assertEqual(1, ack['phase'])
        self.assertEqual(pin['sha256'], ack['request_sha256'])
        self.assertEqual(2, len(ack['evidence']))

    def test_unknown_interop_reader_is_not_overruled_by_unrelated_app_cleanup(self):
        self.b.unknown = True
        self.write('owner-process.json', {})
        self.b._terminal = lambda: ({'native_cleanup_complete': True},
            {'native_cleanup_complete': True}, {}, True)
        self.assertFalse(self.b.stop_and_reap(100))

    def terminal_files(self, child_exit=0):
        candidate = self.value('terminal', phase=2, readers_known=True,
            native_cleanup_complete=True, candidate_exit=child_exit, exit=None,
            acceptance_status='PENDING_POSTPUBLICATION_VALIDATION_AND_ACTUAL_OWNER_EXIT')
        candidate_pin = self.write('terminal.json', candidate)
        validation = self.value('terminal-validation', phase=2,
            terminal_candidate_sha256=candidate_pin['sha256'], candidate_exit=child_exit,
            policy_exit_before_validation_publication=child_exit,
            terminal_publication_within_original_cleanup_end=True,
            acceptance_status='PENDING_VALIDATION_PUBLICATION_AND_ACTUAL_OWNER_EXIT')
        validation_pin = self.write('terminal-validation.json', validation)
        process = self.value('owner-process', readers_known=True, native_cleanup_complete=True,
            actual_child_policy_exit=child_exit, actual_child_process_exit=child_exit,
            child_receipts=[candidate_pin, validation_pin], owner_job=dict(treeEmpty=True,
                jobActiveProcesses=0, heldProcessHandlesStopped=True),
            execution_accepted_before_owner_receipt_publication=child_exit == 0,
            receipt_status='ACTUAL_CHILD_EXIT_PROOF_PENDING_WRAPPER_PUBLICATION_CHECK')
        self.write('owner-process.json', process)
        return process

    def test_actual_bound_child_receipts_collect_only_provisional_process_evidence(self):
        self.terminal_files()
        # Use real bounded files and actual hashes; only translate native path
        # spellings for the temporary POSIX fixture instead of opening C drive.
        with patch.object(bridge, 'mounted', lambda value: Path(value)):
            proof = self.b.verify_terminal({'kind': 'terminal'})
        self.assertEqual('PROVISIONAL_NATIVE_CHILD_COLLECTION', proof['collection_status'])
        self.assertTrue(proof['acceptance_requires_actual_local_launcher_exit'])
        self.assertFalse(proof['local_launcher_exit_verified'])
        self.assertFalse(proof['feature_PASS_claimed'])

    def test_child_failure_keeps_physical_cleanup_without_accepting_collection(self):
        self.terminal_files(child_exit=72)
        with patch.object(bridge, 'mounted', lambda value: Path(value)):
            self.assertTrue(self.b.stop_and_reap(100))
            with self.assertRaisesRegex(RuntimeError, 'actual_native_owner_process_collection'):
                self.b.verify_terminal({'kind': 'terminal'})

    def test_real_terminal_hash_mismatch_is_rejected_even_when_child_exit_is_zero(self):
        self.terminal_files()
        changed = json.loads((self.root/'terminal.json').read_text())
        self.write('terminal.json', changed | {'substituted': True})
        with patch.object(bridge, 'mounted', lambda value: Path(value)):
            with self.assertRaisesRegex(RuntimeError, 'actual_owner_child_receipt_binding'):
                self.b.verify_terminal({'kind': 'terminal'})

    def test_public_record_pin_rejects_symlink_instead_of_reading_target(self):
        target = self.root/'target'
        target.write_text('{}')
        path = self.root/'link'
        path.symlink_to(target)
        with self.assertRaises(OSError):
            bridge.read_public(path)


if __name__ == '__main__':
    unittest.main()
