"""Finite Power BI resource lifecycle; no resource effects or credentials on import.

The caller must first validate the canonical checkpoint/source/CI/public pins
and fresh one-use app admission. Native app process/custody belongs to the
fixed Windows bridge; feature results belong to native GoogleTests and the
coordinator's actual app observations. This module neither schedules tests nor
interprets feature outcomes. It uses the canonical exact resource/SQL controls.
"""
from dataclasses import dataclass
import math

from tools.development.redshift_ctest_lifecycle import need
from tools.development import redshift_powerbi_partition as fixtures


def public_plan(resource_kind='provisioned'):
    need(resource_kind in ('provisioned', 'serverless'), 'app_owner_resource_kind')
    if resource_kind == 'serverless':
        from tools.development.redshift_powerbi_session_ops import plan
        result = plan(resource_kind)
        result['branches']['baseline_zero'] = {'helpers': 1, 'SQL': 5}
        result['total_helpers'] += 1
        result['total_SQL'] += 5
        result['total_logins'] += 1
        result['total_inclusive'] += 6
        result['serverless_principal_binding_sites'].append('baseline_zero')
        result['serverless_principal_binding_SQL'] += 1
        result.update(native_control_processes_maximum=8, maximum_app_launches=3,
                      global_guards_unchanged={'helpers': 40, 'inclusive_SQL': 450})
        return result
    # Existing minimal app22/202SQL/22logins, plus one baseline-zero helper
    # (one SQL +3 wrapper SQL +1 login). Network is separately charged.
    return {'total_helpers': 23, 'total_SQL': 206, 'total_logins': 23,
            'total_inclusive': 229, 'network_helpers_separate': 1,
            'maximum_cleanup_targets': 16, 'native_control_processes_maximum': 8,
            'maximum_app_launches': 3,
            'scope': 'one baseline, positive relationship import and reopen; one driver',
            'global_guards_unchanged': {'helpers': 40, 'inclusive_SQL': 450}}


@dataclass(frozen=True)
class Deadlines:
    owner: float
    child: float
    availability: float
    execution: float
    cleanup: float
    recovery_seconds: int = 900

    def validate(self, now):
        need(all(type(value) in (int, float) and math.isfinite(value) for value in
                 (self.owner, self.child, self.availability, self.execution, self.cleanup)),
             'finite_original_app_deadlines')
        need(now < self.availability <= self.execution < self.cleanup < self.child < self.owner
             and self.owner - now <= 7200 and self.cleanup - self.execution >= 900
             and self.child - self.cleanup >= 900 and self.owner - self.child >= 900
             and type(self.recovery_seconds) is int and 0 < self.recovery_seconds <= 900,
             'original_app_cleanup_reserves')


def collect(ops, bridge, deadlines, *, working_inputs=None):
    """Own one already-consumed, source/CI/one-use-admitted app partition.

Ops must be the closed PowerBiSessionOperations with NativeOps already created
but not prepared. Bridge accepts only the fixed native stage/ACK/retirement
verbs; its run verb is launched separately on the interactive Windows desktop.
No caller SQL, command, feature assertion or horizon extension is accepted.
"""
    deadlines.validate(ops.now())
    resource_kind = getattr(ops, 'resource_kind', 'provisioned')
    need(resource_kind in ('provisioned', 'serverless'), 'app_owner_resource_kind')
    need(ops.powerbi_session_partition is True and ops.native is not None,
         'closed_app_resource_composition')
    need(deadlines.execution <= ops.session_execution_end and
         deadlines.cleanup <= ops.session_cleanup_end, 'original_session_horizons')
    result = {'execution_complete': False, 'feature_results_parsed': False,
              'app_PASS_claimed': False, 'resource_available': False,
              'collection_status': 'PROVISIONAL_NATIVE_CHILD_COLLECTION',
              'acceptance_requires_actual_local_launcher_exit': True,
              'local_launcher_exit_verified': False,
              'native_readers_known': True, 'principal_cleanup': None,
              'fixture_cleanup': None, 'final_zero': None, 'paused_verified': False,
              'resource_kind': resource_kind, 'owned_activity_quiescent': False,
              'physical_compute_idle_verified': False,
              'private_inputs_closed': False, 'working_inputs_retired': False,
              'cleanup_complete': False, 'execution_errors': [], 'cleanup_errors': []}
    fixture_state = None
    resume_attempted = False
    stage_attempted = False
    terminal_seen = False
    baseline_seen = False
    cap_ready = False
    sampled = set()

    def error(group, phase, failure):
        # Never put a SQL diagnostic/connection value/exception message here.
        result[group].append({'phase': phase, 'error_type': type(failure).__name__})

    def cleanup_call(name, function):
        try:
            value = function()
            result[name] = value
            return value
        except BaseException as failure:
            error('cleanup_errors', name, failure)
            return None

    try:
        ops.native.prepare()
        ops.security_paused(deadlines.availability)
        resume_attempted = True
        ops.resume_available(deadlines.availability)
        result['resource_available'] = True
        fixture_state = fixtures.prepare(ops, deadlines.execution)
        ops.session_step('snapshot', deadlines.execution)
        original = ops.powerbi_session_state['original']['test_cap']
        need(type(original) is str and original.isascii() and original.isdecimal()
             and str(int(original)) == original and 1 <= int(original) <= 16,
             'baseline_requires_observed_finite_cap_1_to_16')
        # The finite baseline cap bounds cleanup even before demand is measured.
        # A larger/UNLIMITED original needs a separately reviewed bootstrap path.
        stage_attempted = True
        result['native_readers_known'] = False
        bridge.stage(ops, deadlines.execution, deadlines.cleanup)
        bridge.announce_ready()
        while ops.now() < deadlines.execution:
            event = bridge.poll(deadlines.execution)
            need(ops.now() < deadlines.execution, 'late_native_control_not_accepted')
            if event is None:
                continue
            kind = event['kind']
            if kind == 'baseline_closed':
                need(not baseline_seen and not cap_ready, 'baseline_closed_once')
                bridge.verify_baseline_closed(event)
                result['native_readers_known'] = True
                # This is the extra, explicitly budgeted helper. It verifies
                # zero sessions before any ALTER; it never terminates readers
                # to make a baseline appear clean.
                zero = ops.final_zero(deadlines.execution)
                need(zero.get('complete') is True, 'baseline_exact_principal_zero')
                bridge.record_baseline_zero(zero, ops.proofs['principal_zero'])
                baseline_seen = True
            elif kind == 'reviewed_measurement':
                need(baseline_seen and not cap_ready, 'cap_decision_after_closed_baseline_once')
                state = ops.decide_session_cap(event['measurement_pin'], deadlines.execution)
                if state['temporary_cap'] is not None:
                    state = ops.session_step('set_cap', deadlines.execution)
                need(state['phase'] in ('DECIDED', 'CAP_ACTIVE'), 'known_finite_app_cap')
                bridge.ack_cap(state, deadlines.execution)
                cap_ready = True
                result['native_readers_known'] = False
            elif kind in ('connected', 'navigator', 'refresh'):
                need(cap_ready and kind not in sampled, 'one_sample_after_cap_ready')
                state = ops.session_step('sample_' + kind, deadlines.execution)
                bridge.ack_sample(event, state, deadlines.execution)
                sampled.add(kind)
            elif kind == 'terminal':
                bridge.verify_terminal(event)
                result['native_readers_known'] = True
                terminal_seen = True
                need(ops.now() < deadlines.execution, 'late_native_terminal_not_accepted')
                need(cap_ready and sampled == {'connected', 'navigator', 'refresh'},
                     'complete_app_operational_observation_inventory')
                # Native process success is process collection only. Feature
                # results and the complete checkpoint are assessed elsewhere.
                result['execution_complete'] = True
                break
            else:
                raise RuntimeError('foreign_app_control_event')
        need(terminal_seen, 'original_app_execution_deadline_exhausted')
    except BaseException as failure:
        error('execution_errors', 'execution', failure)
    finally:
        try:
            getattr(ops, 'begin_cleanup', lambda: None)()
        except BaseException as failure:
            error('cleanup_errors', 'begin_cleanup', failure)
        if stage_attempted and not terminal_seen:
            try:
                # Retire an unconsumed stage or abort only the already-owned
                # scope, then require its actual terminal reader/job proof.
                result['native_readers_known'] = bridge.stop_and_reap(deadlines.cleanup) is True
            except BaseException as failure:
                result['native_readers_known'] = False
                error('cleanup_errors', 'native_stop_and_reap', failure)
        try:
            ops.readers_known = ops.reap(deadlines.cleanup) and result['native_readers_known']
            need(ops.readers_known, 'app_readers_unknown_retained')
        except BaseException as failure:
            ops.readers_known = False
            error('cleanup_errors', 'known_readers', failure)
        if resume_attempted and not result['resource_available']:
            # Resume may have succeeded before a lost response/readiness
            # failure. Verify independently; a returned method is not the
            # only way that the resource can have become available.
            available = cleanup_call('resource_available',
                                     lambda: ops.verify_running(deadlines.cleanup))
            result['resource_available'] = available is True
        if result['resource_available'] and ops.readers_known:
            principal = cleanup_call('principal_cleanup',
                                     lambda: ops.cleanup_principals(deadlines.cleanup))
            if principal is not None:
                if principal.get('complete') is not True or principal.get('cap_cleanup', {}).get('complete') is not True:
                    error('cleanup_errors', 'principal_cleanup', RuntimeError())
            # Partial prepare stores state on ops before the first mutation.
            # Never lose it merely because prepare() did not return.
            cleanup_call('fixture_cleanup', lambda: fixtures.cleanup(
                ops, fixture_state or ops.fixture_state, deadlines.cleanup))
            cleanup_call('final_zero', lambda: ops.final_zero(deadlines.cleanup))
        recovery_end = min(deadlines.owner, ops.now() + deadlines.recovery_seconds)
        if resource_kind == 'serverless':
            principal = result['principal_cleanup'] or {}
            fixture = result['fixture_cleanup'] or {}
            zero = result['final_zero'] or {}
            if (ops.readers_known and result['native_readers_known'] and
                    principal.get('complete') is True and
                    principal.get('cap_cleanup', {}).get('complete') is True and
                    fixture.get('complete') is True and zero.get('complete') is True):
                quiet = cleanup_call('owned_activity_quiescent', lambda: ops.verify_idle(recovery_end))
                if quiet is not True:
                    error('cleanup_errors', 'serverless_quiescence_verification', RuntimeError())
            else:
                error('cleanup_errors', 'serverless_owned_activity_unknown', RuntimeError())
            resource_closed = result['owned_activity_quiescent'] is True
        else:
            paused = cleanup_call('paused_verified', lambda: ops.pause_until(recovery_end))
            if paused is not True:
                error('cleanup_errors', 'pause_verification', RuntimeError())
            resource_closed = paused is True
        if ops.readers_known:
            closed = cleanup_call('private_inputs_closed', lambda: ops.close_inputs(deadlines.owner))
            if closed is not True:
                error('cleanup_errors', 'input_closure', RuntimeError())
            if working_inputs is not None and resource_closed and closed is True:
                cleanup_call('working_inputs_retired', lambda: working_inputs.retire(True))
        if working_inputs is not None:
            try:
                working_inputs.close()
            except BaseException as failure:
                error('cleanup_errors', 'working_input_handles', failure)
        else:
            result['working_inputs_retired'] = True  # No scoped input carrier was supplied.
        result['within_original_owner'] = ops.now() < deadlines.owner
        principal = result['principal_cleanup'] or {}
        fixture = result['fixture_cleanup'] or {}
        final_zero = result['final_zero'] or {}
        result['cleanup_complete'] = bool(
            not result['cleanup_errors'] and result['native_readers_known'] and ops.readers_known
            and resource_closed and result['private_inputs_closed'] is True
            and result['working_inputs_retired'] is True and result['within_original_owner']
            and (not result['resource_available'] or
                 (principal.get('complete') is True and fixture.get('complete') is True
                  and final_zero.get('complete') is True)))
        # A record can block. Its immutable content is a candidate requiring
        # this post-publication check, not an unconditional acceptance receipt.
        ops.record({'kind': 'app_resource_terminal', 'result': result,
                    'acceptance_requires_postpublication_check': True})
        result['terminal_publication_returned_in_time'] = ops.now() < deadlines.owner
        if not result['terminal_publication_returned_in_time']:
            result['within_original_owner'] = False
            result['cleanup_complete'] = False
            error('cleanup_errors', 'terminal_publication_after_owner_deadline', TimeoutError())
    return result
