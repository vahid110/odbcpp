"""Fixed WSL DATA bridge to the local Windows app owner; never launches GUI.

Native custody establishes NTFS ownership/ACLs. WSL reads only bounded public
control/receipt data with exact original source/nonce/hash bindings, and invokes
the fixed native stage/ACK/retirement verbs. It never treats POSIX modes on
NTFS as private-input custody or derives app/feature/connection-demand results.
"""
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path, PureWindowsPath
import stat
import time

from tools.development.redshift_ctest_lifecycle import BASE_ENV, need, record, verify_public_pin

BASE = Path('/mnt/c/Users/vahid/odbcpp-qualification/PowerBI007')
SAMPLES = ('connected', 'navigator', 'refresh')


class NotReady(RuntimeError):
    """A create-new record writer has not finished publishing its bytes."""


def mounted(value):
    path = PureWindowsPath(value)
    need(path.is_absolute() and path.drive.upper() == 'C:' and '..' not in path.parts,
         'fixed_native_C_drive_path')
    return Path('/mnt/c').joinpath(*path.parts[1:])


def digest(body):
    return hashlib.sha256(body).hexdigest()


def decode(body):
    def unique(pairs):
        value = {}
        for key, item in pairs:
            need(key not in value, 'duplicate_native_record_key')
            value[key] = item
        return value
    return json.loads(body, object_pairs_hook=unique)


def read_public(path, maximum=2*1024*1024):
    """No secrets: stable regular receipt bytes, not an NTFS ACL assertion."""
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        before = os.fstat(descriptor)
        need(stat.S_ISREG(before.st_mode) and before.st_size <= maximum,
             'bounded_native_public_record')
        if before.st_size == 0:
            raise NotReady('native_record_empty_while_publishing')
        body = os.read(descriptor, maximum + 1)
        after = os.fstat(descriptor)
        identity = lambda info: (info.st_dev, info.st_ino, info.st_size,
                                 info.st_mtime_ns, info.st_ctime_ns)
        if len(body) != before.st_size or identity(before) != identity(after):
            raise NotReady('native_record_writer_in_progress')
        need(identity(after) == identity(os.stat(path, follow_symlinks=False)), 'native_record_replaced')
        return decode(body), {'path': str(path), 'sha256': digest(body)}
    finally:
        os.close(descriptor)


class Bridge:
    def __init__(self, ops, app_plan, native_plan, native_plan_pin, admission):
        self.ops, self.app, self.plan, self.admission = ops, app_plan, native_plan, admission
        self.plan_pin = native_plan_pin
        local_plan = Path(native_plan_pin['path'])
        need(local_plan.is_absolute() and local_plan.is_relative_to('/mnt/c') and
             '..' not in local_plan.parts, 'native_public_plan_on_C_drive')
        self.plan_windows = PureWindowsPath('C:/', *local_plan.relative_to('/mnt/c').parts)
        verify_public_pin(str(local_plan), native_plan_pin['sha256'])
        need(native_plan['source_head'] == app_plan['source_head'] and
             native_plan['run_id'] == app_plan['run_name'] and
             native_plan['driver']['kind'] == app_plan['driver_role'] and
             native_plan['workload_sha256'] == app_plan['input_profile_sha256'],
             'same_native_app_source_input_workload')
        self.run = BASE/native_plan['run_id']
        checkout = PureWindowsPath(native_plan['checkout'])
        need(checkout.parent.parent == PureWindowsPath('C:/Users/vahid/.codex/worktrees') and
             checkout.name == 'odbcpp' and '..' not in checkout.parts,
             'fixed_attached_native_checkout')
        self.entry = checkout/'tools/development/windows_powerbi_owner.py'
        for group in ('modules', 'canonical'):
            for name, sha256 in native_plan[group].items():
                need(Path(name).name == name and name.endswith('.py'), 'native_module_basename')
                verify_public_pin(str(mounted(checkout/'tools/development'/name)), sha256)
        self.python = PureWindowsPath(native_plan['tools']['python']['path'])
        need(str(self.python).replace('\\', '/') ==
             'C:/Users/vahid/AppData/Local/Programs/Python/Python312/python.exe',
             'fixed_native_python')
        verify_public_pin(str(mounted(self.python)), native_plan['tools']['python']['sha256'])
        self.authority = None
        self.calls = 0
        self.unknown = False
        self.seen = set()
        self.published = set()
        self.baseline = self.zero = self.measurement = None

    def _check_end(self, end):
        need(self.ops.now() < end, 'original_native_bridge_cutoff')

    def _record(self, name, value):
        path = self.ops.directory/name
        record(path, value)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        return {'path': str(path), 'sha256': digest(path.read_bytes())}

    def _command(self, mode, end, value=None):
        need(mode in ('stage', 'publish-ack', 'retire-staged') and not self.unknown,
             'fixed_native_data_control')
        self._check_end(end)
        # Seven WSL calls plus the separate ROOT measurement publisher reserve8.
        self.calls += 1
        need(self.calls <= 7, 'native_bridge_process_reserve')
        seconds = min(60, math.floor(end-self.ops.now())-2)
        need(seconds > 0, 'native_bridge_whole_process_reserve')
        argv = [str(mounted(self.python)), '-B', str(self.entry), '--plan',
                str(self.plan_windows),
                '--plan-sha256', self.plan_pin['sha256'], '--'+mode]
        body = None if value is None else (json.dumps(value, ensure_ascii=True)+'\n').encode()
        try:
            result = self.ops.native.owned_process(argv, env=BASE_ENV, seconds=seconds,
                output=self.ops.directory/('native-control-'+str(self.calls)+'.log'), input_bytes=body)
            need(type(result.returncode) is int and not result.timed_out and result.returncode == 0,
                 'native_control_failed_or_unknown')
            self._check_end(end)
        except BaseException:
            # Never retry a potentially published control or pretend interop
            # timeout proves the Windows reader/process is already absent.
            self.unknown = True
            self.ops.native.readers_known = False
            raise

    def _bound(self, value, stage=None):
        need(type(value) is dict and value.get('schema_version') == 1 and
             value.get('source_head') == self.plan['source_head'] and
             value.get('run_id') == self.plan['run_id'] and
             value.get('nonce') == self.admission['nonce'] and
             value.get('plan_sha256') == self.plan_pin['sha256'], 'native_original_run_binding')
        if stage is not None:
            need(value.get('stage') == stage, 'native_fixed_record_stage')

    def _read(self, name):
        value, pin = read_public(self.run/name)
        self._bound(value)
        return value, pin

    def _ack_base(self, request, request_pin, name):
        self._bound(request)
        return dict(schema_version=1, run_id=self.plan['run_id'], nonce=self.admission['nonce'],
            source_head=self.plan['source_head'], plan_sha256=self.plan_pin['sha256'], stage=name,
            phase=request['phase'], status='ACK', request_sha256=request_pin['sha256'])

    def _publish(self, ack, end):
        name = ack['stage']
        need(name not in self.published, 'oneuse_native_ack')
        self.published.add(name)  # Arm once before dispatch, including unknown outcomes.
        self._command('publish-ack', end, ack)

    def stage(self, ops, execution_end, cleanup_end):
        need(ops is self.ops and self.authority is None, 'oneuse_native_stage')
        self._check_end(execution_end)
        state = ops.powerbi_session_state
        cap = int(state['original']['test_cap'])
        observation = self._record('native-baseline-cap-observation.json', {
            'kind': 'actual_cap_snapshot', 'source_head': self.plan['source_head'],
            'run_id': self.plan['run_id'], 'snapshot': state['snapshot'],
            'original': state['original'], 'SQL_proof': ops.proofs['powerbi_sessions_snapshot']})
        now = dt.datetime.now(dt.timezone.utc)
        at = lambda end: (now+dt.timedelta(seconds=end-ops.now())).isoformat()
        need(execution_end-ops.now() >= 240 and cleanup_end-execution_end >= 900,
             'native_launch_and_resource_cleanup_reserve')
        a = dict(schema_version=1, state='ROOT_ADMITTED', independent_review='ACCEPT',
            scope=self.plan['scope'], source_head=self.plan['source_head'], run_id=self.plan['run_id'],
            nonce=self.admission['nonce'], plan_sha256=self.plan_pin['sha256'],
            execute_before_utc=at(min(execution_end-240, ops.now()+300)),
            execution_end_utc=at(execution_end), cleanup_end_utc=at(execution_end+300),
            original_cap=cap, cap_observed_utc=dt.datetime.fromtimestamp(
                state['snapshot']['completed_ms']/1000, dt.timezone.utc).isoformat(),
            cap_observation=observation)
        c = ops.native.config
        connection = dict(SERVER=c['host'], PORT='5439', DATABASE=c['database'], UID=c['test_user'],
                          SSLCAFILE=self.plan['ca']['path'])
        self.authority = a
        self._command('stage', execution_end, {'authority': a, 'connection': connection})

    def announce_ready(self):
        print(json.dumps({'state': 'RESOURCE_READY_FOR_LOCAL_POWERBI_LAUNCH',
            'run_id': self.plan['run_id'], 'original_execution_end_utc': self.authority['execution_end_utc'],
            'GUI_launch_by_WSL': False, 'feature_PASS_claimed': False}), flush=True)

    def poll(self, end):
        self._check_end(end)
        for name, kind in [('baseline-closed.json', 'baseline_closed'),
                           ('reviewed-measurement.ack.json', 'reviewed_measurement'),
                           *((f'sample-{s}.native-request.json', s) for s in SAMPLES),
                           ('owner-process.json', 'terminal')]:
            if kind in self.seen or not (self.run/name).exists():
                continue
            try:
                value, pin = self._read(name)
            except (NotReady, json.JSONDecodeError, UnicodeDecodeError):
                # A create-new writer may still be finishing its bounded file.
                # It is never accepted until complete and stable; no replay.
                continue
            if kind == 'terminal' and not all((self.run/n).exists() for n in
                    ('terminal.json', 'terminal-validation.json')):
                continue
            event = {'kind': kind, 'record': value, 'pin': pin}
            if kind == 'reviewed_measurement':
                self._bound(value, 'reviewed-measurement')
                need(value['status'] == 'ACK', 'native_measurement_handoff')
                measurement = value['measurement_pin']
                verify_public_pin(measurement['path'], measurement['sha256'])
                # Consume only the bounded bytes whose digest was reviewed.
                # The path may have been replaced after verify_public_pin.
                body, consumed_pin = read_public(measurement['path'])
                need(consumed_pin['sha256'] == measurement['sha256'],
                     'reviewed_measurement_bytes_changed')
                if self.app['comparison_target_cap'] is not None:
                    need(body['requested_cap'] == self.app['comparison_target_cap'],
                         'same_actual_comparison_cap')
                event['measurement_pin'] = measurement
                self.measurement = measurement
            self.seen.add(kind)
            return event
        time.sleep(min(.2, max(0, end-self.ops.now())))
        return None

    def verify_baseline_closed(self, event):
        from tools.development.windows_powerbi_contract import safe_readers
        value = event['record']
        self._bound(value, 'baseline-closed')
        need(value['phase'] == 0 and value['normal_close'] is True and safe_readers(value['process'])
             and value['workload_sha256'] == self.plan['workload_sha256'], 'actual_baseline_reader_proof')
        self.baseline = event['pin']

    def record_baseline_zero(self, zero, proofs):
        need(self.baseline is not None and zero.get('complete') is True, 'actual_baseline_zero')
        self.zero = self._record('baseline-exact-zero.json', dict(
            source_head=self.plan['source_head'], run_id=self.plan['run_id'],
            native_baseline=self.baseline, exact_principal_zero=True, SQL_proof=proofs))

    def ack_cap(self, state, end):
        need(self.baseline is not None and self.zero is not None and self.measurement is not None,
             'cap_ack_requires_actual_baseline_zero_and_reviewed_demand')
        request, request_pin = self._read('cap-ready.native-request.json')
        self._bound(request, 'cap-ready')
        need(request['baseline_closed_sha256'] == self.baseline['sha256'], 'same_closed_baseline')
        cap = state['temporary_cap'] if state['temporary_cap'] is not None else int(state['original']['test_cap'])
        need(1 <= cap <= 16 and (self.app['comparison_target_cap'] is None or
             cap == self.app['comparison_target_cap']), 'same_finite_comparison_cap')
        proof = self._record('app-cap-decision.json', {'state': state, 'measurement': self.measurement,
            'baseline_zero': self.zero, 'not_feature_result': True})
        ack = self._ack_base(request, request_pin, 'cap-ready')
        ack.update(evidence=[self.baseline, self.zero, self.measurement, proof], target_cap=cap,
            baseline_closed_sha256=self.baseline['sha256'], zero_sessions=self.zero,
            measurement_review=self.measurement, cap_decision=proof)
        self._publish(ack, end)

    def ack_sample(self, event, state, end):
        name = 'sample-'+event['kind']
        self._bound(event['record'], name)
        need(event['record']['phase'] in (1, 2), 'positive_app_sample_phase')
        proof = self._record(name+'-resource-proof.json', {'state': state,
            'SQL_proof': self.ops.proofs['powerbi_sessions_'+name.replace('-', '_')],
            'native_request': event['pin'], 'not_feature_result': True})
        ack = self._ack_base(event['record'], event['pin'], name)
        ack['evidence'] = [event['pin'], proof]
        self._publish(ack, end)

    def _terminal(self):
        process, process_pin = self._read('owner-process.json')
        candidate, candidate_pin = self._read('terminal.json')
        validation, validation_pin = self._read('terminal-validation.json')
        self._bound(process, 'owner-process')
        self._bound(validation, 'terminal-validation')
        # Native pins use C-drive spellings; compare both identity and bytes.
        held = {str(mounted(ref['path'])): ref['sha256'] for ref in process['child_receipts']}
        need(held == {candidate_pin['path']: candidate_pin['sha256'],
                      validation_pin['path']: validation_pin['sha256']}, 'actual_owner_child_receipt_binding')
        need(validation['terminal_candidate_sha256'] == candidate_pin['sha256'], 'native_terminal_validation_binding')
        job = process['owner_job']
        readers = (process['readers_known'] is True and candidate['readers_known'] is True and
            job.get('treeEmpty') is True and job.get('jobActiveProcesses') == 0 and
            job.get('heldProcessHandlesStopped') is True)
        return process, candidate, validation, readers

    def verify_terminal(self, event):
        process, candidate, validation, readers = self._terminal()
        need(process['receipt_status'] == 'ACTUAL_CHILD_EXIT_PROOF_PENDING_WRAPPER_PUBLICATION_CHECK'
             and candidate['acceptance_status'] == 'PENDING_POSTPUBLICATION_VALIDATION_AND_ACTUAL_OWNER_EXIT'
             and validation['acceptance_status'] == 'PENDING_VALIDATION_PUBLICATION_AND_ACTUAL_OWNER_EXIT'
             and candidate['exit'] is None, 'native_collection_receipts_remain_provisional')
        need(readers and process['native_cleanup_complete'] is True and
             candidate['native_cleanup_complete'] is True, 'actual_native_terminal_reader_cleanup')
        need(process['actual_child_policy_exit'] == process['actual_child_process_exit'] == 0 and
             candidate['candidate_exit'] == validation['candidate_exit'] ==
             validation['policy_exit_before_validation_publication'] == 0 and
             process['execution_accepted_before_owner_receipt_publication'] is True and
             validation['terminal_publication_within_original_cleanup_end'] is True,
             'actual_native_owner_process_collection')
        # This receipt can become visible before launch_local finishes its own
        # publication/close checks. ROOT must independently observe that exact
        # local launcher return/job proof before final checkpoint acceptance.
        return dict(collection_status='PROVISIONAL_NATIVE_CHILD_COLLECTION',
            acceptance_requires_actual_local_launcher_exit=True,
            local_launcher_exit_verified=False, feature_PASS_claimed=False)

    def stop_and_reap(self, end):
        self._check_end(end)
        if (self.run/'owner-process.json').exists():
            process, candidate, _, readers = self._terminal()
            return bool(readers and process['native_cleanup_complete'] is True and
                        candidate['native_cleanup_complete'] is True and not self.unknown)
        if not (self.run/'consumed.json').exists():
            if (self.run/'staged.json').exists():
                self._command('retire-staged', end)
            return not (self.run/'test-input.json').exists() and not self.unknown
        if not self.unknown and 'abort' not in self.published:
            consumed, consumed_pin = self._read('consumed.json')
            request = dict(consumed, stage='abort', phase=0)
            ack = self._ack_base(request, consumed_pin, 'abort')
            ack['evidence'] = [consumed_pin]
            self._publish(ack, end)
        while self.ops.now() < end:
            if (self.run/'owner-process.json').exists():
                process, candidate, _, readers = self._terminal()
                return bool(readers and process['native_cleanup_complete'] is True and
                            candidate['native_cleanup_complete'] is True and not self.unknown)
            time.sleep(min(.2, max(0, end-self.ops.now())))
        return False
