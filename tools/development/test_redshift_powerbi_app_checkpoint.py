"""Public plan/admission and real scoped-file custody controls; no provider calls."""
import copy
from contextlib import contextmanager, redirect_stdout
import datetime as dt
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('app_checkpoint_candidate',
    Path(__file__).with_name('redshift_powerbi_app_checkpoint.py'))
entry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(entry)


class Controls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='odbcpp-app-plan-controls-')
        self.root = Path(self.temp.name).resolve()
        self.state = self.root/'state'
        self.state.mkdir(mode=0o700)
        self.head = 'a'*40
        self.root_patch = patch.object(entry, 'ROOT', self.root)
        self.state_patch = patch.object(entry, 'STATE', self.state)
        self.root_patch.start()
        self.state_patch.start()
        self.addCleanup(self.temp.cleanup)
        self.addCleanup(self.root_patch.stop)
        self.addCleanup(self.state_patch.stop)
        modules = ('redshift_powerbi_app_checkpoint.py', 'redshift_powerbi_app_owner.py',
                   'redshift_powerbi_bridge.py',
                   'redshift_powerbi_session_ops.py', 'redshift_powerbi_sessions.py',
                   'redshift_powerbi_relationship.py', 'redshift_powerbi_partition.py',
                   'redshift_ctest_ops.py', 'redshift_resource.py')
        self.pins = []
        for name in modules:
            path = self.root/'tools/development'/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('# synthetic non-executed source fixture\n')
            self.pins.append(self.pin(path))
        self.checkpoint = {'source_head': self.head, 'partition': 'powerbi_relationship',
            'resource': {'arn': 'arn:aws:iam::385207570137:user/odbcpp-redshift-pilot'}}
        self.canonical_patch = patch.object(entry.canonical, 'read_plan', return_value=self.checkpoint)
        self.canonical = self.canonical_patch.start()
        self.addCleanup(self.canonical_patch.stop)
        self.plan = dict(schema_version=1, scope=entry.SCOPE, source_directory=str(self.root),
            source_head=self.head, run_name='powerbi007-own', state_directory=str(self.state),
            checkpoint_plan=self.write('checkpoint.json', {}), native_plan=self.write('native.json', {}),
            operator_policy=self.write('operator.txt', {'public_policy_fixture': True}), workload_id='relationship-q007',
            input_profile_sha256='b'*64, driver_role='own', comparison_target_cap=None,
            comparison_reference=None, case_ids=list(entry.CASES), bounds=copy.deepcopy(entry.BOUNDS),
            public_pins=self.pins)
        self.path = self.root/'app-plan.json'
        self.save_plan()

    @staticmethod
    def pin(path):
        return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}

    def write(self, name, value):
        path = self.root/name
        path.write_text(json.dumps(value))
        return self.pin(path)

    def save_plan(self):
        self.path.write_text(json.dumps(self.plan))

    def test_public_preflight_reuses_full_canonical_checkpoint_without_private_inputs(self):
        with patch.object(entry, 'private_json', side_effect=AssertionError('private input read')):
            actual, _ = entry.read_plan(self.path)
        self.assertEqual(self.plan, actual)
        self.canonical.assert_called_once_with(Path(self.plan['checkpoint_plan']['path']), require_ci=False)

    def test_missing_scenario_is_refused(self):
        self.plan['case_ids'].pop()
        self.save_plan()
        with self.assertRaisesRegex(RuntimeError, 'complete_fixed_app'):
            entry.read_plan(self.path)

    def test_wrong_source_or_partition_does_not_inherit_inventory(self):
        self.checkpoint['partition'] = 'native'
        with self.assertRaisesRegex(RuntimeError, 'same_source_complete_checkpoint'):
            entry.read_plan(self.path)

    def test_public_pin_mutation_is_refused(self):
        Path(self.pins[0]['path']).write_text('# changed\n')
        with self.assertRaisesRegex(RuntimeError, 'public_binding_changed'):
            entry.read_plan(self.path)

    def test_vendor_cannot_invent_same_cap_reference(self):
        self.plan.update(driver_role='aws2', comparison_target_cap=5)
        self.save_plan()
        with self.assertRaisesRegex(RuntimeError, 'vendor_requires_own_driver_cap_reference'):
            entry.read_plan(self.path)

    def test_vendor_reference_binds_input_source_cap_and_immutable_evidence(self):
        evidence = {key: self.write(key+'.json', {'synthetic': True}) for key in
                    ('cap_decision', 'native_terminal', 'review')}
        reference = dict(kind='own_driver_cap_observation', driver_role='own', source_head=self.head,
            workload_id=self.plan['workload_id'], input_profile_sha256='b'*64, cap=5, evidence=evidence)
        self.plan.update(driver_role='aws2', comparison_target_cap=5,
                         comparison_reference=self.write('comparison.json', reference))
        self.save_plan()
        entry.read_plan(self.path)
        reference['cap'] = 6
        self.plan['comparison_reference'] = self.write('comparison.json', reference)
        self.save_plan()
        with self.assertRaisesRegex(RuntimeError, 'same_source_input_user_cap'):
            entry.read_plan(self.path)

    def admission(self):
        now = dt.datetime.now(dt.timezone.utc)
        ms = int(now.timestamp()*1000)
        expires = (now+dt.timedelta(seconds=6000)).isoformat()
        inputs = self.state/'scoped-inputs'
        review = dict(disposition='ACCEPT', source_head=self.head, run_name=self.plan['run_name'],
                      plan_sha256=hashlib.sha256(self.path.read_bytes()).hexdigest())
        admission = dict(reviewed=True, independent_review='ACCEPT', review_receipt=self.write('review.json', review),
            source_head=self.head, scope=entry.SCOPE, run_name=self.plan['run_name'], plan_sha256=review['plan_sha256'],
            nonce='synthetic-oneuse-nonce-123', expires_utc=expires, bounds=copy.deepcopy(entry.BOUNDS),
            inventory_scope='complete_app_checkpoint_partition',
            powerbi_sessions=dict(workload_id=self.plan['workload_id'], input_profile_sha256='b'*64,
                admitted_ms=ms-1000, execution_deadline_ms=ms+1800000, cleanup_deadline_ms=ms+2700000),
            credential_source=str(inputs/'operator-session.ini'), database_config=str(inputs/'database-config.json'),
            operator_session_metadata=str(inputs/'operator-session-metadata.json'))
        metadata = {'identity': self.checkpoint['resource']['arn'], 'expires_utc': expires}
        return admission, metadata

    def test_authorize_requires_actual_ci_review_clean_source_and_current_scope(self):
        admission, metadata = self.admission()
        with patch.object(entry.subprocess, 'check_output', side_effect=[self.head+'\n', '']), \
                patch.object(entry, 'private_json', side_effect=[admission, metadata]):
            _, _, actual = entry.authorize(self.path, self.root/'private-admission.json')
        self.assertEqual(admission, actual)
        self.assertTrue(self.canonical.call_args.kwargs['require_ci'])

    def test_authorize_rejects_stale_scoped_input_path(self):
        admission, metadata = self.admission()
        admission['credential_source'] = '/tmp/historical-session.ini'
        with patch.object(entry.subprocess, 'check_output', side_effect=[self.head+'\n', '']), \
                patch.object(entry, 'private_json', side_effect=[admission, metadata]):
            with self.assertRaisesRegex(RuntimeError, 'only_current_scoped_working_inputs'):
                entry.authorize(self.path, self.root/'private-admission.json')

    def test_actual_scoped_files_are_retired_once_only_after_reader_proof(self):
        inputs = self.state/'scoped-inputs'
        inputs.mkdir(mode=0o700)
        for name in entry.ScopedInputs.NAMES:
            path = inputs/name
            path.write_text('synthetic-not-credentials')
            path.chmod(0o600)
        held = entry.ScopedInputs()
        self.addCleanup(held.close)
        with self.assertRaisesRegex(RuntimeError, 'scoped_input_readers_unknown'):
            held.retire(False)
        self.assertTrue(all((inputs/name).exists() for name in held.NAMES))
        self.assertTrue(held.retire(True))
        self.assertEqual([], list(inputs.iterdir()))

    def test_actual_substituted_scoped_file_is_not_deleted(self):
        inputs = self.state/'scoped-inputs'
        inputs.mkdir(mode=0o700)
        for name in entry.ScopedInputs.NAMES:
            path = inputs/name
            path.write_text('synthetic-not-credentials')
            path.chmod(0o600)
        held = entry.ScopedInputs()
        self.addCleanup(held.close)
        replacement = self.state/'replacement'
        replacement.write_text('preserve-substitution')
        replacement.chmod(0o600)
        replacement.replace(inputs/held.NAMES[1])
        with self.assertRaisesRegex(RuntimeError, 'scoped_input_changed_retained'):
            held.retire(True)
        self.assertEqual('preserve-substitution', (inputs/held.NAMES[1]).read_text())
        self.assertEqual(3, len(list(inputs.iterdir())))

    def test_bridge_is_a_required_public_source_pin(self):
        self.plan['public_pins'] = [ref for ref in self.pins
            if Path(ref['path']).name != 'redshift_powerbi_bridge.py']
        self.save_plan()
        with self.assertRaisesRegex(RuntimeError, 'required_app_source_pin'):
            entry.read_plan(self.path)

    def test_default_CLI_is_inert(self):
        with patch.object(entry, 'execute', side_effect=AssertionError('execution')), \
                patch.object(entry, 'read_plan', side_effect=AssertionError('input read')), \
                redirect_stdout(io.StringIO()) as output:
            self.assertEqual(0, entry.main([]))
        self.assertFalse(json.loads(output.getvalue())['GUI_launch_by_WSL'])

    def test_preflight_CLI_reads_only_public_plan(self):
        with patch.object(entry, 'private_json', side_effect=AssertionError('private read')), \
                patch.object(entry, 'execute', side_effect=AssertionError('resource action')), \
                redirect_stdout(io.StringIO()) as output:
            self.assertEqual(0, entry.main(['preflight', '--plan', str(self.path)]))
        self.assertEqual('PUBLIC_APP_PREFLIGHT_ONLY', json.loads(output.getvalue())['state'])

    def test_original_UTC_horizons_are_not_reset_by_late_composition(self):
        from datetime import datetime, timezone
        spec = importlib.util.spec_from_file_location('deadline_owner_fixture',
            Path(__file__).with_name('redshift_powerbi_app_owner.py'))
        module = importlib.util.module_from_spec(spec)
        import sys
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        wall = datetime(2026, 10, 22, 12, 0, tzinfo=timezone.utc)
        admitted = int(wall.timestamp()*1000)-120000
        admission = dict(expires_utc=(wall+dt.timedelta(seconds=6000)).isoformat(),
            powerbi_sessions=dict(admitted_ms=admitted, execution_deadline_ms=admitted+1800000,
                                  cleanup_deadline_ms=admitted+2700000))
        result = entry._deadlines(types.SimpleNamespace(Deadlines=module.Deadlines),
                                  admission, 100, wall)
        self.assertAlmostEqual(1780, result.execution)
        self.assertAlmostEqual(2680, result.cleanup)
        self.assertAlmostEqual(5380, result.owner)
        self.assertEqual(900, result.cleanup-result.execution)
        self.assertEqual(900, result.recovery_seconds)
        result.validate(100)

    def test_first_signal_starts_cleanup_repeated_signal_cannot_interrupt_it(self):
        import signal
        installed = {}
        def install(kind, handler):
            previous = installed.get(kind, 'original-'+str(kind))
            installed[kind] = handler
            return previous
        with patch.object(signal, 'signal', side_effect=install):
            with entry._interrupt_scope() as begin_cleanup:
                handler = installed[signal.SIGINT]
                with self.assertRaises(KeyboardInterrupt):
                    handler(signal.SIGINT, None)
                self.assertEqual(signal.SIG_IGN, installed[signal.SIGTERM])
                self.assertIsNone(handler(signal.SIGTERM, None))
                begin_cleanup()
            self.assertEqual('original-'+str(signal.SIGINT), installed[signal.SIGINT])
            self.assertEqual('original-'+str(signal.SIGTERM), installed[signal.SIGTERM])

    def test_scoped_close_attempts_every_handle_without_fd_reuse_retry(self):
        inputs = self.state/'scoped-inputs'
        inputs.mkdir(mode=0o700)
        for name in entry.ScopedInputs.NAMES:
            path = inputs/name
            path.write_text('synthetic-not-credentials')
            path.chmod(0o600)
        held = entry.ScopedInputs()
        actual_close = os.close
        calls = []
        def uncertain(fd):
            calls.append(fd)
            actual_close(fd)
            if len(calls) == 1:
                raise OSError('synthetic ambiguous return after close')
        with patch.object(entry.os, 'close', side_effect=uncertain):
            with self.assertRaisesRegex(RuntimeError, 'scoped_input_handle_close_unknown'):
                held.close()
            held.close()
        self.assertEqual(4, len(calls))
        self.assertEqual(4, len(set(calls)))
        self.assertEqual(3, len(list(inputs.iterdir())))

    def test_direct_default_CLI_bootstraps_own_tools_without_PYTHONPATH_or_checkout_cwd(self):
        # Isolate only import resolution/default-inert behavior. These stand-in
        # modules deliberately cannot provide a live runtime implementation.
        source = self.root/'source'/'tools'/'development'
        source.mkdir(parents=True)
        (source.parent/'__init__.py').write_text('')
        (source/'__init__.py').write_text('')
        (source/'live_checkpoint.py').write_text('# inert import fixture\n')
        (source/'redshift_ctest_lifecycle.py').write_text(
            'BASE_ENV = {}\nBounds = object\n'
            'def need(*args): raise AssertionError("unexpected effect")\n'
            'def verify_public_pin(*args): raise AssertionError("unexpected pin read")\n')
        (source/'redshift_resource.py').write_text(
            'NativeOps = object\n'
            'def private_json(*args): raise AssertionError("private input")\n'
            'def utc(*args): raise AssertionError("unexpected authority")\n')
        copied = source/'redshift_powerbi_app_checkpoint.py'
        copied.write_bytes(Path(entry.__file__).read_bytes())
        external = self.root/'outside-checkout'
        external.mkdir()
        child = subprocess.run([sys.executable, '-B', str(copied)], cwd=external,
            env={'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C',
                 'PYTHONDONTWRITEBYTECODE': '1'}, capture_output=True, text=True,
            timeout=5, check=False)
        self.assertEqual(0, child.returncode, child.stderr)
        self.assertEqual('INERT_REVIEWED_PLAN_AND_ONEUSE_ADMISSION_REQUIRED',
                         json.loads(child.stdout)['state'])

    @staticmethod
    def provisional_result():
        return dict(execution_complete=True, cleanup_complete=True,
            owner_deadline=1000, entry_finalization_within_original_owner=True,
            resource_owner_exit_status='PENDING_ACTUAL_RESOURCE_OWNER_PROCESS_EXIT',
            acceptance_requires_actual_resource_owner_exit=True,
            resource_owner_exit_verified=False, app_PASS_claimed=False)

    def test_successful_late_stdout_flush_returns_nonzero_despite_backward_UTC(self):
        clock = [999.0]
        result = self.provisional_result()
        published = []
        class BackwardUTC:
            @staticmethod
            def now(zone):
                return dt.datetime(2000, 1, 1, tzinfo=dt.timezone.utc)
        def slow_print(body, *, flush):
            self.assertTrue(flush)
            published.append(json.loads(body))
            clock[0] = 1001.0
        with patch.object(entry, 'execute', return_value=result), \
                patch.object(entry.time, 'clock_gettime', side_effect=lambda _: clock[0]), \
                patch.object(entry, 'dt', types.SimpleNamespace(datetime=BackwardUTC,
                                                               timezone=dt.timezone)), \
                patch('builtins.print', side_effect=slow_print):
            self.assertEqual(1, entry.main(['run', '--plan', str(self.path),
                                         '--admission', str(self.root/'synthetic.json')]))
        self.assertEqual(1, len(published))  # No attempted final receipt rewrite.
        self.assertEqual(0, published[0]['policy_exit_candidate_before_stdout_publication'])
        self.assertFalse(published[0]['resource_owner_exit_verified'])
        self.assertTrue(published[0]['cleanup_complete'])
        self.assertTrue(result['cleanup_complete'])

    def test_timely_CLI_exit_stays_explicitly_pending_actual_process_witness(self):
        result = self.provisional_result()
        with patch.object(entry, 'execute', return_value=result), \
                patch.object(entry.time, 'clock_gettime', return_value=999.0), \
                redirect_stdout(io.StringIO()) as output:
            self.assertEqual(0, entry.main(['run', '--plan', str(self.path),
                                         '--admission', str(self.root/'synthetic.json')]))
        printed = json.loads(output.getvalue())
        self.assertEqual('PENDING_ACTUAL_RESOURCE_OWNER_PROCESS_EXIT', printed['resource_owner_exit_status'])
        self.assertTrue(printed['acceptance_requires_actual_resource_owner_exit'])
        self.assertFalse(printed['resource_owner_exit_verified'])


class CompositionControls(unittest.TestCase):
    """Exercise actual execute+collect with synthetic effects only in tests."""
    pin = staticmethod(Controls.pin)
    write = Controls.write
    save_plan = Controls.save_plan
    admission = Controls.admission

    def setUp(self):
        Controls.setUp(self)
        import sys
        spec = importlib.util.spec_from_file_location('composition_core_controls',
            Path(__file__).with_name('test_redshift_powerbi_app_owner.py'))
        fixture = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = fixture
        spec.loader.exec_module(fixture)
        self.fixture = fixture
        self.pilot = self.root/'pilot'
        self.pilot.mkdir(mode=0o700)
        self.pilot_patch = patch.object(entry, 'PILOT_STATE', self.pilot)
        self.pilot_patch.start()
        self.addCleanup(self.pilot_patch.stop)
        inputs = self.state/'scoped-inputs'
        inputs.mkdir(mode=0o700)
        for name in entry.ScopedInputs.NAMES:
            p = inputs/name
            p.write_text('synthetic-not-credentials')
            p.chmod(0o600)
        self.admitted, self.metadata = self.admission()
        self.events = []
        self.instances = []
        self.held = []
        self.fail_bridge = False
        self.unknown_readers = False
        self.checkpoint.update(timeout_fixture={}, ca_sha256='c'*64, security_groups=[{'synthetic': True}],
            tools={name: self.write('tool-'+name, {'not_executed': True}) for name in
                   ('cmake', 'ctest', 'aws', 'psql', 'python')},
            authority_receipts={name: self.write(name+'.json', {}) for name in
                   ('authority', 'applicability', 'cleanup_delegation')})
        for name, value in (('profiles', {}), ('specs', []), ('index', {'synthetic': True}),
                            ('expected', {}), ('ci_receipt', {}),
                            ('checkpoint_expected', {}), ('checkpoint_partitions', {})):
            self.checkpoint[name] = self.write('component-'+name+'.json', value)
        outer = self
        class Ops(fixture.Ops):
            def __init__(self, **kwargs):
                super().__init__()
                self.t = time.clock_gettime(time.CLOCK_MONOTONIC)
                self.session_execution_end = self.t+1800
                self.session_cleanup_end = self.t+2700
                self.directory = Path(kwargs['approval']['run_directory'])
                self.kwargs = kwargs
                self.events = outer.events
                outer.instances.append(self)
                outer.assertTrue((self.directory/'consumption.json').exists())
        class Native:
            def __init__(self, approval, manifest, directory, child_end):
                outer.events.append('native_constructor')
                outer.assertTrue((Path(directory)/'consumption.json').exists())
                self.readers_known = True
                self.child_deadline = child_end
            def prepare(self):
                outer.events.append('prepare_inputs')
        class Bridge(fixture.Bridge):
            def __init__(self, ops, app, native, pin, admission):
                if outer.fail_bridge:
                    ops.native.readers_known = False
                    raise RuntimeError('synthetic constructor failure; unknown reader boundary')
                super().__init__(ops)
                if outer.unknown_readers:
                    self.events = [{'kind': 'foreign'}]
                    self.stopped_readers_known = False
        class Timeout:
            def __init__(self, **kwargs):
                pass
        actual_inputs = entry.ScopedInputs
        class Inputs(actual_inputs):
            def __init__(self):
                super().__init__()
                self.retire_attempts = []
                outer.held.append(self)
            def retire(self, known):
                self.retire_attempts.append(known)
                return super().retire(known)
        from tools.redshift.pilot_preflight import pilot_lock
        @contextmanager
        def lock(path):
            outer.assertEqual(outer.pilot, path)
            outer.events.append('canonical_pilot_lock')
            with pilot_lock(path) as fd:
                yield fd
        self.components = types.SimpleNamespace(Operations=Ops, NativeOps=Native, Bridge=Bridge,
            TimeoutFactory=Timeout, factory=object(), profile_inputs=object(),
            Deadlines=fixture.owner.Deadlines, collect=fixture.owner.collect,
            fixture_plan=fixture.owner.public_plan, lock=lock)
        self.runtime_patch = patch.object(entry, '_components', return_value=self.components)
        self.input_patch = patch.object(entry, 'ScopedInputs', Inputs)
        self.auth_patch = patch.object(entry, 'authorize', return_value=(self.plan, self.checkpoint, self.admitted))
        for p in (self.runtime_patch, self.input_patch, self.auth_patch):
            p.start()
            self.addCleanup(p.stop)

    def execute(self):
        def prepare(ops, end):
            self.assertIn('prepare_inputs', self.events)
            self.events.append('fixture_prepare')
            ops.fixture_state = {'prepared': True}
            return ops.fixture_state
        def cleanup(ops, state, end):
            self.assertTrue(ops.readers_known)
            self.assertFalse(ops.app_active)
            self.events.append('fixture_retire')
            return {'complete': True}
        with patch.object(self.fixture.owner.fixtures, 'prepare', prepare), \
                patch.object(self.fixture.owner.fixtures, 'cleanup', cleanup):
            return entry.execute(self.path, self.root/'synthetic-admission.json')

    def test_actual_composition_consumes_then_collects_and_closes_three_carriers(self):
        result = self.execute()
        self.assertTrue(result['execution_complete'])
        self.assertTrue(result['cleanup_complete'])
        self.assertEqual('PROVISIONAL_NATIVE_CHILD_COLLECTION', result['collection_status'])
        self.assertTrue(result['acceptance_requires_actual_local_launcher_exit'])
        self.assertFalse(result['local_launcher_exit_verified'])
        self.assertFalse(result['app_PASS_claimed'])
        self.assertLess(self.events.index('canonical_pilot_lock'), self.events.index('native_constructor'))
        self.assertLess(self.events.index('native_constructor'), self.events.index('prepare_inputs'))
        self.assertEqual([True], self.held[0].retire_attempts)
        self.assertIsNone(self.held[0].parent)
        self.assertFalse(self.held[0].files)
        self.assertEqual([], list((self.state/'scoped-inputs').iterdir()))
        consumption = json.loads((self.state/self.plan['run_name']/'consumption.json').read_text())
        self.assertEqual(self.admitted['nonce'], consumption['nonce'])
        self.assertFalse(consumption['GUI_launch_by_WSL'])
        self.assertEqual((self.components.factory, self.components.profile_inputs),
                         self.instances[0].kwargs['components'][1:])

    def test_constructor_failure_closes_handles_retains_inputs_and_consumes_run(self):
        self.fail_bridge = True
        with self.assertRaisesRegex(RuntimeError, 'synthetic constructor'):
            self.execute()
        self.assertNotIn('prepare_inputs', self.events)
        self.assertIsNone(self.held[0].parent)
        self.assertEqual([], self.held[0].retire_attempts)
        self.assertEqual(3, len(list((self.state/'scoped-inputs').iterdir())))
        with self.assertRaisesRegex(RuntimeError, 'oneuse_app_run_already_consumed'):
            self.execute()
        self.assertEqual(1, len(self.held))

    def test_unknown_native_readers_never_retire_carriers(self):
        self.unknown_readers = True
        result = self.execute()
        self.assertFalse(result['cleanup_complete'])
        self.assertEqual([], self.held[0].retire_attempts)
        self.assertIsNone(self.held[0].parent)
        self.assertEqual(3, len(list((self.state/'scoped-inputs').iterdir())))
        self.assertIn('pause_verified', self.events)

    def test_existing_resource_lock_blocks_before_consumption_or_native_effects(self):
        from tools.redshift.pilot_preflight import pilot_lock, Blocked
        with pilot_lock(self.pilot):
            with self.assertRaises(Blocked):
                self.execute()
        self.assertFalse((self.state/self.plan['run_name']).exists())
        self.assertNotIn('native_constructor', self.events)
        self.assertEqual([], self.held)

    def test_successful_late_state_fd_close_is_not_timely_even_if_UTC_moves_backwards(self):
        clock = [time.clock_gettime(time.CLOCK_MONOTONIC)]
        wall = dt.datetime.now(dt.timezone.utc)
        late = [False]
        class ReversingUTC:
            @staticmethod
            def now(zone):
                return wall-dt.timedelta(days=1) if late[0] else wall
        original_open, original_close = entry._open_state, os.close
        state_fd = []
        def opening(path):
            fd = original_open(path)
            if path == self.state:
                state_fd.append(fd)
            return fd
        def closing(fd):
            original_close(fd)
            if state_fd and fd == state_fd[0]:
                proof = json.loads((self.state/self.plan['run_name']/'consumption.json').read_text())
                clock[0] = proof['owner_deadline']+1
                late[0] = True
        with patch.object(entry, '_open_state', side_effect=opening), \
                patch.object(entry.os, 'close', side_effect=closing), \
                patch.object(entry.time, 'clock_gettime', side_effect=lambda _: clock[0]), \
                patch.object(entry, 'dt', types.SimpleNamespace(datetime=ReversingUTC,
                                                               timezone=dt.timezone)):
            result = self.execute()
        self.assertTrue(result['cleanup_complete'])  # Real cleanup still happened.
        self.assertTrue(result['execution_complete'])
        self.assertFalse(result['entry_finalization_within_original_owner'])
        self.assertFalse(result['resource_owner_exit_verified'])
        self.assertLess(result['owner_deadline'], clock[0])
        self.assertEqual([], list((self.state/'scoped-inputs').iterdir()))

    def test_successful_late_interrupt_restore_is_checked_after_context_exit(self):
        clock = [time.clock_gettime(time.CLOCK_MONOTONIC)]
        @contextmanager
        def late_restore():
            try:
                yield lambda: None
            finally:
                proof = json.loads((self.state/self.plan['run_name']/'consumption.json').read_text())
                clock[0] = proof['owner_deadline']+1
        with patch.object(entry, '_interrupt_scope', late_restore), \
                patch.object(entry.time, 'clock_gettime', side_effect=lambda _: clock[0]):
            result = self.execute()
        self.assertTrue(result['cleanup_complete'])
        self.assertFalse(result['entry_finalization_within_original_owner'])

    def test_successful_late_pilot_unlock_is_checked_after_shared_lock_exit(self):
        clock = [time.clock_gettime(time.CLOCK_MONOTONIC)]
        original_lock = self.components.lock
        @contextmanager
        def late_unlock(path):
            with original_lock(path) as fd:
                yield fd
            proof = json.loads((self.state/self.plan['run_name']/'consumption.json').read_text())
            clock[0] = proof['owner_deadline']+1
        self.components.lock = late_unlock
        with patch.object(entry.time, 'clock_gettime', side_effect=lambda _: clock[0]):
            result = self.execute()
        self.assertTrue(result['cleanup_complete'])
        self.assertFalse(result['entry_finalization_within_original_owner'])


if __name__ == '__main__':
    unittest.main()
