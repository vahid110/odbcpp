"""Optional app-partition resource controls; no app automation or admission.

Use only from the reviewed Power BI resource owner after fresh finite admission.
The normal native CTest owner retains its eight-target cleanup/default fixtures.
The app owner still owns exact Windows job/reap proof, credential custody, the
full checkpoint union, pause and final cleanup. This class cannot certify them.
"""
import copy
import hashlib
import json
import os
import stat
import time

from tools.development import redshift_powerbi_sessions as sessions
from tools.development import redshift_powerbi_partition as relationship
from tools.development.redshift_ctest_lifecycle import need, record, verify_public_pin
from tools.development.redshift_ctest_ops import Operations


def plan(resource_kind='provisioned'):
    # Derive relationship and cap costs; retain startup5/23SQL and
    # capture+max16 guarded termination+two zero probes4/47SQL.
    need(resource_kind in ('provisioned', 'serverless'), 'app_plan_resource_kind')
    if resource_kind == 'serverless':
        from tools.development.redshift_ctest_ops import serverless_fixture_reserve
        return dict(serverless_fixture_reserve('app'),
                    relationship=relationship.plan(resource_kind),
                    session_controls=sessions.public_plan(resource_kind),
                    no_resource_or_admission_effects=True)
    relationship_plan = relationship.plan()
    session_controls = sessions.public_plan()
    cap = session_controls['maximum_changed_path']
    helpers = relationship_plan['total_helpers'] + cap['helpers']
    statements = (relationship_plan['total_SQL'] + cap['SQL'] +
                  47 - relationship_plan['principal_cleanup_reserve']['SQL'])
    result = {'total_helpers': helpers, 'total_SQL': statements, 'total_logins': helpers,
            'total_inclusive': statements + helpers, 'network_helpers_separate': 1,
            'maximum_cleanup_targets': 16, 'relationship': relationship_plan,
            'session_controls': session_controls,
            'principal_cleanup': {'helpers': 4, 'SQL': 47},
            'scope': 'single minimal app partition; no full native fixtures or repeated vendor cycle',
            'no_resource_or_admission_effects': True}
    return result


class PowerBiSessionOperations(Operations):
    def __init__(self, **kwargs):
        need('relationship_partition' not in kwargs, 'fixed_app_fixture_scope')
        super().__init__(relationship_partition=True, **kwargs)
        need(self.approval.get('inventory_scope') == 'complete_app_checkpoint_partition',
             'fresh_app_partition_admission_required')
        authority = self.approval['powerbi_sessions']
        self.powerbi_session_state = sessions.new_state(**authority,
            **({'resource_kind': 'serverless'} if self.resource_kind == 'serverless' else {}))
        now_ms, monotonic = self.utc_ms(), self.now()
        need(authority['admitted_ms'] <= now_ms < authority['execution_deadline_ms'] and
             authority['cleanup_deadline_ms'] - authority['admitted_ms'] <= 7200000,
             'original_app_admission_horizon')
        self.session_execution_end = monotonic + (authority['execution_deadline_ms'] - now_ms) / 1000
        self.session_cleanup_end = monotonic + (authority['cleanup_deadline_ms'] - now_ms) / 1000
        self.powerbi_session_partition = True
        self.maximum_cleanup_targets = 16
        self.session_sequence = 0
        self.session_armed = False

    @staticmethod
    def utc_ms():
        return time.time_ns() // 1000000

    def _publish_session(self, suffix, value):
        need(suffix in ('intent', 'complete', 'decision'), 'session_record_suffix')
        record(self.directory / ('powerbi-sessions-' + str(self.session_sequence) +
                                 '-' + suffix + '.json'), value)
        descriptor = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def _closed_stage(self, stage):
        if type(stage) is dict and type(stage.get('stage')) is str and stage['stage'].startswith('powerbi_sessions_'):
            need(self.powerbi_session_partition is True and self.session_armed is True,
                 'session_dispatch_not_armed')
            held = self.powerbi_session_state
            need(type(held.get('pending')) is dict, 'session_pending_intent_required')
            pending = held['pending']
            expected = sessions.stage_for(pending['before'], stage['stage'].removeprefix('powerbi_sessions_'))
            need(stage == expected and held == sessions.begin(
                pending['before'], expected, now_ms=pending['dispatched_ms']),
                'closed_session_SQL_changed')
            self.session_armed = False
            return
        return super()._closed_stage(stage)

    def _session_deadline(self, kind, deadline):
        original = self.session_cleanup_end if kind in ('quiesce', 'restore') else self.session_execution_end
        need(self.now() < deadline <= original, 'original_session_monotonic_deadline')

    def session_step(self, kind, deadline):
        need(kind in sessions.STAGES, 'fixed_session_stage')
        self._session_deadline(kind, deadline)
        if kind in ('quiesce', 'restore'):
            need(self.readers_known and self.principals_quiescent and
                 self.native is not None and self.native.readers_known,
                 'session_restore_requires_stopped_readers_and_principal_zero')
        before = self.powerbi_session_state
        stage = sessions.stage_for(before, kind)
        held = sessions.begin(before, stage, now_ms=self.utc_ms())
        self.session_sequence += 1
        self.powerbi_session_state = held
        self._publish_session('intent', held)
        self.session_armed = True
        try:
            # _sql returns only after native exit0/reap, RSD1 identity and its
            # original cutoff; independently bind the session SELF to RSD1 PID.
            self._closed_stage(stage)
            raw, actual_pid = self._sql(stage, deadline)
            frames = sessions.parse_frames(raw)
            self_frame = frames.get('SELF', [])
            need(len(self_frame) == 1 and self_frame[0].split('|', 1)[0] == str(actual_pid),
                 'session_SELF_RSD1_PID_mismatch')
            self._session_deadline(kind, deadline)
            completed = sessions.complete(held, raw, native_exit=0, now_ms=self.utc_ms())
            self._publish_session('complete', completed)
            self._session_deadline(kind, deadline)
            self.powerbi_session_state = completed
            return copy.deepcopy(completed)
        except BaseException:
            if kind in sessions.MUTATIONS:
                # A lost ACK, late return or failed durable completion may
                # hide a successful ALTER. No new SQL until independently
                # reconciled under a fresh reviewed recovery admission.
                self.sql_unknown = self.sql_fenced = True
            raise
        finally:
            self.session_armed = False

    def decide_session_cap(self, measurement_pin, deadline):
        self._session_deadline('snapshot', deadline)
        verify_public_pin(measurement_pin['path'], measurement_pin['sha256'])
        descriptor = os.open(measurement_pin['path'], os.O_RDONLY | os.O_NOFOLLOW)
        try:
            info = os.fstat(descriptor)
            need(stat.S_ISREG(info.st_mode) and info.st_size <= 16384, 'bounded_session_measurement')
            body = os.read(descriptor, 16385)
            need(len(body) == info.st_size and hashlib.sha256(body).hexdigest() ==
                 measurement_pin['sha256'], 'session_measurement_changed')
        finally:
            os.close(descriptor)
        measurement = json.loads(body)
        # These pins refer to actual reviewed client-demand/server observations.
        # This verifies their bytes, never invents or parses a measured demand.
        decided = sessions.decide(self.powerbi_session_state, measurement, now_ms=self.utc_ms())
        for key in ('client_demand', 'server_census', 'review'):
            ref = measurement[key]
            verify_public_pin(ref['path'], ref['sha256'])
        self._session_deadline('snapshot', deadline)
        self.session_sequence += 1
        self._publish_session('decision', {'state': decided, 'measurement': measurement_pin})
        self._session_deadline('snapshot', deadline)
        self.powerbi_session_state = decided
        return copy.deepcopy(decided)

    def restore_session_cap(self, deadline):
        need(self.powerbi_session_state['pending'] is None,
             'unknown_session_mutation_requires_reconciliation')
        state = self.powerbi_session_state
        if state['phase'] in ('DECIDED', 'CAP_ACTIVE'):
            state = self.session_step('quiesce', deadline)
        if state['phase'] == 'QUIESCENT' and state['temporary_cap'] is not None:
            state = self.session_step('restore', deadline)
        need(state['phase'] in ('CAP_RESTORED', 'CAP_VERIFIED_UNCHANGED'),
             'original_cap_not_verified')
        return {'complete': True, 'original': copy.deepcopy(state['original']),
                'phase': state['phase'], 'not_resource_cleanup_proof': True}

    def cleanup_principals(self, deadline):
        result = super().cleanup_principals(deadline)
        need(result.get('complete') is True, 'app_principal_cleanup_incomplete')
        state = self.powerbi_session_state
        need(state['pending'] is None, 'unknown_session_intent_blocks_cleanup')
        if 'set_cap' in state['attempts'] or state['phase'] in (
                'CAP_VERIFIED_UNCHANGED', 'CAP_RESTORED') or (
                state['phase'] == 'DECIDED' and state['temporary_cap'] is None):
            cap = self.restore_session_cap(deadline)
        else:
            # Setup may stop before any ALTER is dispatched. There is no
            # owner cap mutation to undo and no fresh cap-readback claim.
            cap = {'complete': True, 'phase': 'OWNER_CAP_UNTOUCHED',
                   'original_cap_freshly_verified': False}
        return dict(result, cap_cleanup=cap)
