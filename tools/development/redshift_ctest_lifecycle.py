"""Own one finite local CTest collection; CTest owns all case execution/results.

ROOT supplies the reviewed real adapter. Its methods reuse pilot_lock,
native_auth_inputs and the existing exact-resource/TLS/network/process controls.
No provider, credential loading, admission or process starts occur on import.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat

STATE = Path('/Users/vahidsbr/.local/state/odbcpp/redshift-pilot')
BASE_ENV = {'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C'}
LOGGING = {'LogLevel': 'Debug', 'LogFormat': 'Json', 'LogSink': 'Stderr',
           'LogAsync': 'false', 'LogQueries': 'false'}


class Refused(RuntimeError):
    pass


def need(value, code):
    if not value:
        raise Refused(code)


@dataclass(frozen=True)
class Bounds:
    owner: int = 7200
    child: int = 6300
    aggregate: int = 4500
    execution: int = 3600
    availability: int = 900
    cleanup_reserve: int = 900
    recovery: int = 900
    case: int = 40

    def validate(self):
        for name, maximum in (('owner', 7200), ('child', 6300),
                              ('aggregate', 4500), ('execution', 3600),
                              ('availability', 900), ('recovery', 900),
                              ('case', 40)):
            value = getattr(self, name)
            need(type(value) is int and 0 < value <= maximum, 'finite_' + name)
        need(type(self.cleanup_reserve) is int and self.cleanup_reserve >= 900 and
             self.execution + self.cleanup_reserve <= self.aggregate and
             self.child + self.recovery <= self.owner and
             self.availability + self.aggregate + self.recovery <= self.child,
             'cleanup_clock_reserves')
        need(self.case == 40, 'existing_case_timeout40')


@dataclass(frozen=True)
class Config:
    run_directory: Path
    source_directory: Path
    source_head: str
    index: Path
    expected: Path
    cmake: Path
    ctest: Path
    public_pins: dict
    bounds: Bounds = Bounds()
    state_directory: Path | None = None


def record(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(value, stream, sort_keys=True)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())


def verify_public_pin(path, expected):
    """Only ROOT-selected public source/tool/binary/index files, never inputs."""
    path = Path(path)
    need(path.is_absolute() and '..' not in path.parts and
         path.name not in ('credentials', 'config', 'test.pgpass', 'admin.pgpass',
                           'odbc.ini', 'odbcinst.ini', 'connection.txt') and
         'provisioned-database-config' not in str(path) and
         re.fullmatch('[0-9a-f]{64}', expected or '') is not None, 'public_pin')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        before = os.fstat(fd)
        need(stat.S_ISREG(before.st_mode), 'public_regular_file')
        digest = hashlib.sha256()
        while True:
            chunk = os.read(fd, 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
        after = os.fstat(fd)
        need((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns,
              before.st_ctime_ns) == (after.st_dev, after.st_ino, after.st_size,
                                     after.st_mtime_ns, after.st_ctime_ns) and
             digest.hexdigest() == expected, 'public_binding_changed')
    finally:
        os.close(fd)


def verify_config(config):
    config.bounds.validate()
    need(config.run_directory.parent == (config.state_directory or STATE) and
         re.fullmatch('[A-Za-z0-9_.-]+', config.run_directory.name) is not None,
         'canonical_run_directory')
    need(config.source_directory.is_absolute() and
         re.fullmatch('[0-9a-f]{40}', config.source_head) is not None,
         'local_source_identity')
    need(type(config.public_pins) is dict and config.public_pins, 'public_bindings')
    for path in (config.index, config.expected, config.cmake, config.ctest):
        need(str(path) in config.public_pins, 'required_public_binding')
    for path, digest in config.public_pins.items():
        verify_public_pin(path, digest)
    index = json.loads(config.index.read_text())
    need(type(index.get('images')) is dict and index['images'], 'owning_images')
    for image in index['images'].values():
        verify_public_pin(image['executable'], image['sha256'])
    # Inventory mapping/completeness is supplied by ROOT and checked by CMake;
    # this module never enumerates, schedules or parses GoogleTest results.


def validate_fixture_plan(plan):
    """Dry, complete control-operation reserve; no inputs/cloud effects."""
    need(type(plan) is dict, 'fixture_plan')
    values = {}
    for name in ('total_helpers', 'total_SQL', 'total_logins', 'total_inclusive'):
        value = plan.get(name)
        need(type(value) is int and value >= 0, 'fixture_plan_' + name)
        values[name] = value
    need(values['total_inclusive'] == values['total_SQL'] + values['total_logins'],
         'fixture_plan_inclusive')
    need(values['total_helpers'] + plan.get('network_helpers_separate', 0) <= 40,
         'approved_control_helper_cap40')
    need(values['total_inclusive'] <= 450, 'approved_control_SQL_cap450')
    return plan


def run_once(config, ops, fixtures):
    """Run one CTest process, always closing resource/input ownership.

    Adapter API: now, lock(STATE), verify_source(path,head), open_inputs(run,end),
    security_paused(end), resume_available(end), stage_profiles(index,run,state,
    logging,end)->private-index Path, run_process(argv,env,deadline,output,umask),
    reap(deadline)->bool, cleanup_principals(deadline)->{'complete':bool},
    final_zero(deadline)->{'complete':bool},
    pause_until(deadline)->bool, close_inputs(deadline)->bool. run_process returns
    returncode/timed_out/reaped/group_absent, supervising the entire process tree.
    SQL and resource methods must enforce their existing identity/counter caps.
    FixtureError.state or ops.fixture_state retains partial setup ownership.
    close_inputs must retire native_auth_inputs, staged profile JSON/DSNs and
    generated ctest/CTestTestfile.cmake (including partial configure output) by
    exact held metadata after all CMake/CTest readers are reaped. Raw test logs
    remain private evidence, not credential-input cleanup targets. Acceptance of
    lifecycle-result.json also requires no publication-expired.json marker.
    """
    bounds = config.bounds
    started = ops.now()
    need(type(started) in (int, float) and math.isfinite(started), 'monotonic_clock')
    bounds.validate()
    owner_end = started + bounds.owner
    child_end = started + bounds.child
    # Reject an over-cap complete fixture plan before private snapshots/cloud.
    fixture_plan = validate_fixture_plan(fixtures.plan())
    aggregate_end = execution_end = None
    state = None
    errors = []
    fixture_cleanup = principal_cleanup = final_zero = None
    paused = inputs_closed = readers_known = owned_activity_quiescent = False
    resource_kind = getattr(ops, 'resource_kind', 'provisioned')
    need(resource_kind in ('provisioned', 'serverless'), 'resource_kind')
    if resource_kind == 'serverless':
        need(not getattr(ops, 'keep_cluster_running', False), 'serverless_no_cluster_continuation')
    ctest_result = None
    ctest_started = False
    phase = 'binding'
    result = None

    def remaining(deadline):
        need(ops.now() < deadline, 'original_absolute_deadline')

    def capture_error(where, error):
        # Exception messages can contain private input. Retain public type only.
        errors.append({'phase': where, 'error_type': type(error).__name__})

    with ops.lock(config.state_directory or STATE):
        verify_config(config)
        ops.verify_source(config.source_directory, config.source_head)
        # Exclusive directory creation is the one-use consumption. No resets.
        os.mkdir(config.run_directory, 0o700)
        record(config.run_directory / 'consumption.json',
               {'source_head': config.source_head, 'owner_deadline': owner_end,
                'child_deadline': child_end, 'CTest_invocations_allowed': 1,
                'case_timeout': bounds.case, 'scope': 'local-ctest-collect-all',
                'fixture_plan': fixture_plan})
        try:
            phase = 'inputs'
            ops.open_inputs(config.run_directory, child_end)
            phase = 'serverless_security' if resource_kind == 'serverless' else 'paused_security'
            availability_end = min(started + bounds.availability,
                                   child_end - bounds.aggregate - bounds.recovery)
            remaining(availability_end)
            ops.security_paused(availability_end)
            phase = 'serverless_availability' if resource_kind == 'serverless' else 'resume'
            ops.resume_available(availability_end)
            aggregate_started = ops.now()
            aggregate_end = min(aggregate_started + bounds.aggregate,
                                child_end - bounds.recovery)
            execution_end = min(aggregate_started + bounds.execution,
                                aggregate_end - bounds.cleanup_reserve)
            phase = 'fixture_prepare'
            state = fixtures.prepare(ops, execution_end)
            phase = 'profile_inputs'
            staged_index = Path(ops.stage_profiles(config.index, config.run_directory,
                                                  state, dict(LOGGING), execution_end))
            need(staged_index.is_absolute() and staged_index.is_relative_to(config.run_directory),
                 'owned_staged_index')
            if hasattr(ops, 'run_native_suite'):
                phase = 'native_windows_configure_ctest'
                remaining(execution_end)
                try:
                    native = ops.run_native_suite(staged_index, execution_end)
                finally:
                    ctest_started = ops.native_ctest_invoked
                need(native.configured is True and ctest_started is True,
                     'native_configure_or_CTest_not_completed')
                ctest_result = {'returncode': native.returncode, 'timed_out': native.timed_out,
                                'reaped': native.reaped, 'group_absent': native.group_absent}
                record(config.run_directory / 'ctest-exit.json', ctest_result)
            else:
                build = config.run_directory / 'ctest'
                output = config.run_directory / 'gtest'
                for path in (build, output):
                    os.mkdir(path, 0o700)
                configure = [str(config.cmake), '-S',
                             str(config.source_directory / 'tests/development/live_suite'),
                             '-B', str(build), '-DODBCPP_LIVE_INDEX=' + str(staged_index),
                             '-DODBCPP_LIVE_EXPECTED=' + str(config.expected),
                             '-DODBCPP_LIVE_OUTPUT=' + str(output)]
                phase = 'cmake_configure'
                remaining(execution_end)
                configured = ops.run_process(configure, env=dict(BASE_ENV), deadline=execution_end,
                                             output=config.run_directory / 'configure.log', umask=0o077)
                need(configured.reaped is True and configured.group_absent is True and
                     configured.timed_out is False and configured.returncode == 0,
                     'configure_not_completed')
                command = [str(config.ctest), '--test-dir', str(build), '-j1',
                           '--output-on-failure', '--output-junit',
                           str(config.run_directory / 'ctest-results.xml'),
                           '--output-log', str(config.run_directory / 'ctest-full.log')]
                phase = 'ctest'
                remaining(execution_end)
                record(config.run_directory / 'ctest-command.json',
                       {'argv': command, 'execution_deadline': execution_end,
                        'invocation': 1, 'stop_on_failure': False})
                ctest_started = True
                child = ops.run_process(command, env=dict(BASE_ENV), deadline=execution_end,
                                        output=config.run_directory / 'ctest-console.log', umask=0o077)
                need(type(child.returncode) is int and type(child.timed_out) is bool,
                     'CTest_return_unknown')
                ctest_result = {'returncode': child.returncode, 'timed_out': child.timed_out,
                                'reaped': child.reaped, 'group_absent': child.group_absent}
                record(config.run_directory / 'ctest-exit.json', ctest_result)
                # A normal nonzero return is collected verbatim; it is not a Python
                # feature-result gate. CTest already continued unrelated cases.
        except BaseException as error:
            state = getattr(error, 'state', None) or state or getattr(ops, 'fixture_state', None)
            capture_error(phase, error)
        finally:
            getattr(ops, 'begin_cleanup', lambda: None)()
            cleanup_end = aggregate_end if aggregate_end is not None else child_end - bounds.recovery
            try:
                readers_known = ops.reap(cleanup_end) is True
                need(readers_known, 'reader_reaps_unknown')
            except BaseException as error:
                capture_error('reap', error)
            ops.readers_known = readers_known
            try:
                principal_cleanup = ops.cleanup_principals(cleanup_end)
                need(principal_cleanup.get('complete') is True, 'principal_cleanup_incomplete')
            except BaseException as error:
                capture_error('principal_cleanup', error)
            # Exact mutation requires real principal quiescence. The injected
            # fixture hook refuses when readers/quiescence are not known.
            try:
                fixture_cleanup = fixtures.cleanup(ops, state, cleanup_end)
                need(fixture_cleanup.get('complete') is True, 'fixture_cleanup_incomplete')
            except BaseException as error:
                capture_error('fixture_cleanup', error)
            try:
                final_zero = ops.final_zero(cleanup_end)
                need(final_zero.get('complete') is True, 'final_zero_unverified')
            except BaseException as error:
                capture_error('final_zero', error)
            recovery_end = owner_end
            try:
                # Recovery retains its own bounded time after SQL cleanup; it
                # never extends SQL, CTest or successful aggregate acceptance.
                recovery_end = min(owner_end, ops.now() + bounds.recovery)
                if resource_kind == 'serverless':
                    need(readers_known and principal_cleanup is not None
                         and principal_cleanup.get('complete') is True
                         and fixture_cleanup is not None and fixture_cleanup.get('complete') is True
                         and final_zero is not None and final_zero.get('complete') is True,
                         'serverless_owned_activity_unknown')
                    owned_activity_quiescent = ops.verify_idle(recovery_end) is True
                    need(owned_activity_quiescent, 'serverless_owned_activity_unverified')
                elif getattr(ops, 'keep_cluster_running', False):
                    ops.verify_running(recovery_end)
                else:
                    paused = ops.pause_until(recovery_end) is True
                    need(paused, 'paused_unverified')
            except BaseException as error:
                capture_error('serverless_quiescence' if resource_kind == 'serverless' else 'pause', error)
            try:
                need(readers_known, 'private_input_readers_unknown_retained')
                inputs_closed = ops.close_inputs(recovery_end) is True
                need(inputs_closed and readers_known, 'private_inputs_retained')
            except BaseException as error:
                capture_error('input_close', error)
            within_original_aggregate = aggregate_end is not None and ops.now() < aggregate_end
            result = {'source_head': config.source_head, 'ctest_invoked': ctest_started,
                      'ctest_invocations': None if ctest_started is None else int(ctest_started), 'ctest': ctest_result,
                      'errors': errors, 'fixture_cleanup': fixture_cleanup,
                      'principal_cleanup': principal_cleanup, 'final_zero': final_zero,
                      'readers_known': readers_known,
                      'paused_verified': paused, 'cluster_left_running_by_human_request': getattr(ops, 'keep_cluster_running', False),
                      'resource_kind': resource_kind,
                      'owned_activity_quiescent': owned_activity_quiescent,
                      'physical_compute_idle_verified': False,
                      'private_inputs_closed': inputs_closed,
                      'owner_deadline': owner_end, 'child_deadline': child_end,
                      'aggregate_end': aggregate_end, 'execution_end': execution_end,
                      'within_original_aggregate': within_original_aggregate,
                      'lifecycle_cleanup_complete': not errors and readers_known and (paused or owned_activity_quiescent or getattr(ops, 'keep_cluster_running', False)) and
                          inputs_closed and within_original_aggregate,
                      'feature_results_parsed': False, 'feature_PASS_claimed': False}
            record(config.run_directory / 'lifecycle-result.json', result)
            if aggregate_end is not None and ops.now() >= aggregate_end:
                record(config.run_directory / 'publication-expired.json',
                       {'accepted': False, 'aggregate_end': aggregate_end,
                        'successful_lifecycle_acceptance': False})
                result['lifecycle_cleanup_complete'] = False
    return result


if __name__ == '__main__':
    raise SystemExit('ROOT adapter and reviewed finite one-use execution required')
