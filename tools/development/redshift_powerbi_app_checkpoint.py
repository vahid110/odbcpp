"""Public app-plan and private admission binding; no resource effects on import.

This composes the normal complete-checkpoint plan rather than inventing an app
exception to source, CI, inventory, fixture, identity, TLS or cleanup authority.
The POSIX entry owns resources/data only; ROOT launches the native Windows
owner locally. Calling read_plan() never reads credentials, resumes a resource
or starts an app. Import and the default CLI are inert.
"""
import argparse
from contextlib import contextmanager
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time

# Match the canonical entry: direct script invocation outside the checkout
# must resolve its own checked-in tools, without depending on PYTHONPATH/cwd.
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.development import live_checkpoint as canonical
from tools.development.redshift_ctest_lifecycle import BASE_ENV, Bounds, need, verify_public_pin
from tools.development.redshift_resource import NativeOps, private_json, utc

STATE = Path('/home/vahid/.local/state/odbcpp/powerbi-app007')
PILOT_STATE = Path('/home/vahid/.local/state/odbcpp/redshift-pilot')
SCOPE = 'redshift-powerbi-relationship-checkpoint'
CASES = ('PBI007-DEMAND-BASELINE', 'PBI007-DEFAULT-PARALLEL-IMPORT',
         'PBI007-SINGLE-KEY-RELATIONSHIP', 'PBI007-FILTER-PROPAGATION-RESET',
         'PBI007-REFRESH-SAVE-CLOSE', 'PBI007-REOPEN-REFRESH-SAVE-CLOSE')
BOUNDS = dict(owner=5400, child=4500, aggregate=2700, execution=1800,
              availability=900, cleanup_reserve=900, recovery=900, case=40)


def pin_file(ref):
    need(type(ref) is dict and set(ref) == {'path', 'sha256'}, 'app_public_pin_schema')
    verify_public_pin(ref['path'], ref['sha256'])
    return Path(ref['path'])


def read_plan(path, *, require_ci=False):
    path = Path(path)
    need(path.is_absolute() and not path.is_symlink() and path.stat().st_size <= 65536,
         'bounded_public_app_plan')
    plan = json.loads(path.read_text(encoding='utf-8-sig'))
    keys = {'schema_version', 'scope', 'source_directory', 'source_head', 'run_name',
            'state_directory', 'checkpoint_plan', 'native_plan', 'operator_policy',
            'workload_id', 'input_profile_sha256', 'driver_role', 'comparison_target_cap', 'comparison_reference',
            'case_ids', 'bounds', 'public_pins'}
    need(type(plan) is dict and set(plan) == keys and plan['schema_version'] == 1
         and plan['scope'] == SCOPE, 'closed_app_public_plan')
    need(Path(plan['source_directory']).resolve() == ROOT and
         re.fullmatch('[0-9a-f]{40}', plan['source_head']) is not None,
         'actual_app_source_checkout')
    need(Path(plan['state_directory']) == STATE and
         re.fullmatch('powerbi007-[A-Za-z0-9_-]{1,64}', plan['run_name']) is not None,
         'fixed_owned_app_namespace')
    need(plan['case_ids'] == list(CASES) and plan['bounds'] == BOUNDS,
         'complete_fixed_app_scenarios_and_bounds')
    Bounds(**plan['bounds']).validate()
    need(type(plan['workload_id']) is str and re.fullmatch(
         '[A-Za-z0-9][A-Za-z0-9_.-]{0,127}', plan['workload_id']) is not None and
         re.fullmatch('[0-9a-f]{64}', plan['input_profile_sha256']) is not None,
         'app_workload_and_equal_input_identity')
    need(plan['driver_role'] in ('own', 'aws2'), 'closed_comparator_driver_roles')
    cap = plan['comparison_target_cap']
    need(cap is None or (type(cap) is int and 1 <= cap <= 16), 'finite_common_comparison_cap')
    need(plan['driver_role'] != 'aws2' or cap is not None, 'vendor_requires_same_comparison_cap')
    reference = plan['comparison_reference']
    if plan['driver_role'] == 'aws2':
        need(reference is not None, 'vendor_requires_own_driver_cap_reference')
    if reference is not None:
        observed = json.loads(pin_file(reference).read_text(encoding='utf-8-sig'))
        need(observed.get('kind') == 'own_driver_cap_observation' and
             observed.get('driver_role') == 'own' and observed.get('source_head') == plan['source_head']
             and observed.get('workload_id') == plan['workload_id'] and
             observed.get('input_profile_sha256') == plan['input_profile_sha256'] and
             observed.get('cap') == cap, 'same_source_input_user_cap_comparison_reference')
        need(type(observed.get('evidence')) is dict and set(observed['evidence']) ==
             {'cap_decision', 'native_terminal', 'review'}, 'comparison_reference_evidence')
        for ref in observed['evidence'].values():
            pin_file(ref)
    checkpoint = canonical.read_plan(pin_file(plan['checkpoint_plan']), require_ci=require_ci)
    need(checkpoint['source_head'] == plan['source_head'] and
         checkpoint.get('partition') == 'powerbi_relationship',
         'same_source_complete_checkpoint_relationship_partition')
    # The canonical reader verifies the disjoint complete native inventory,
    # profile/input recipes, exact tools/CI and standing fixture authority.
    pin_file(plan['native_plan'])
    pin_file(plan['operator_policy'])
    need(type(plan['public_pins']) is list and bool(plan['public_pins']), 'app_source_closure_pins')
    paths = []
    for ref in plan['public_pins']:
        paths.append(str(pin_file(ref)))
    need(len(paths) == len(set(paths)), 'duplicate_app_source_pin')
    for module in ('redshift_powerbi_app_checkpoint.py', 'redshift_powerbi_app_owner.py',
                   'redshift_powerbi_bridge.py',
                   'redshift_powerbi_session_ops.py', 'redshift_powerbi_sessions.py',
                   'redshift_powerbi_relationship.py', 'redshift_powerbi_partition.py',
                   'redshift_ctest_ops.py', 'redshift_resource.py'):
        need(str(ROOT/'tools/development'/module) in paths, 'required_app_source_pin')
    return plan, checkpoint


def authorize(plan_path, admission_path):
    """Fresh authority is explicit; no credentials, SQL or provider acquisition."""
    plan, checkpoint = read_plan(plan_path, require_ci=True)
    actual = subprocess.check_output(['/usr/bin/git', '-C', str(ROOT), 'rev-parse', 'HEAD'],
                                     env=BASE_ENV, text=True, stdin=subprocess.DEVNULL,
                                     timeout=5).strip()
    dirty = subprocess.check_output(['/usr/bin/git', '-C', str(ROOT), 'status', '--porcelain'],
                                    env=BASE_ENV, text=True, stdin=subprocess.DEVNULL,
                                    timeout=5).strip()
    need(actual == plan['source_head'] and not dirty, 'clean_exact_app_source')
    admission = private_json(admission_path)
    keys = {'reviewed', 'independent_review', 'review_receipt', 'source_head', 'scope',
            'run_name', 'plan_sha256', 'nonce', 'expires_utc', 'bounds',
            'inventory_scope', 'powerbi_sessions', 'credential_source',
            'database_config', 'operator_session_metadata'}
    need(type(admission) is dict and set(admission) == keys and admission['reviewed'] is True
         and admission['independent_review'] == 'ACCEPT' and admission['source_head'] == actual
         and admission['scope'] == SCOPE and admission['run_name'] == plan['run_name']
         and admission['plan_sha256'] == hashlib.sha256(Path(plan_path).read_bytes()).hexdigest()
         and admission['bounds'] == BOUNDS
         and admission['inventory_scope'] == 'complete_app_checkpoint_partition'
         and re.fullmatch('[A-Za-z0-9_-]{16,128}', admission['nonce']) is not None,
         'fresh_reviewed_oneuse_app_admission')
    review = json.loads(pin_file(admission['review_receipt']).read_text(encoding='utf-8-sig'))
    need(review.get('disposition') == 'ACCEPT' and review.get('source_head') == actual
         and review.get('run_name') == plan['run_name'] and
         review.get('plan_sha256') == admission['plan_sha256'], 'actual_app_admission_review')
    inputs = STATE/'scoped-inputs'
    need(admission['credential_source'] == str(inputs/'operator-session.ini') and
         admission['database_config'] == str(inputs/'database-config.json') and
         admission['operator_session_metadata'] == str(inputs/'operator-session-metadata.json'),
         'only_current_scoped_working_inputs')
    metadata = private_json(admission['operator_session_metadata'])
    now = dt.datetime.now(dt.timezone.utc)
    need(metadata['identity'] == checkpoint['resource']['arn'] and
         now + dt.timedelta(seconds=BOUNDS['owner'] + 2) < utc(admission['expires_utc'])
         <= utc(metadata['expires_utc']), 'original_operator_and_finite_horizon')
    session = admission['powerbi_sessions']
    from tools.development.redshift_powerbi_sessions import new_state
    new_state(**session)
    now_ms = int(now.timestamp() * 1000)
    need(session['workload_id'] == plan['workload_id'] and
         session['input_profile_sha256'] == plan['input_profile_sha256'] and
         session['admitted_ms'] <= now_ms < session['execution_deadline_ms'] and
         session['cleanup_deadline_ms'] - session['admitted_ms'] <= BOUNDS['owner'] * 1000 and
         session['cleanup_deadline_ms'] < int(utc(admission['expires_utc']).timestamp() * 1000),
         'original_app_session_authority')
    return plan, checkpoint, admission


class ScopedInputs:
    """Hold exact current working carriers; no secret reads or copies here."""
    NAMES = ('database-config.json', 'operator-session.ini', 'operator-session-metadata.json')

    def __init__(self):
        self.parent = os.open(STATE/'scoped-inputs', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        self.files = {}
        try:
            parent = os.fstat(self.parent)
            need(parent.st_uid == os.geteuid() and stat.S_IMODE(parent.st_mode) == 0o700,
                 'private_scoped_input_parent')
            for name in self.NAMES:
                fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=self.parent)
                self.files[name] = (fd, None)
                info = os.fstat(fd)
                need(stat.S_ISREG(info.st_mode) and info.st_uid == os.geteuid() and
                     info.st_nlink == 1 and stat.S_IMODE(info.st_mode) == 0o600,
                     'private_scoped_input_custody')
                self.files[name] = (fd, NativeOps.stamp(info))
        except BaseException:
            self.close()
            raise

    def retire(self, readers_known):
        need(readers_known is True, 'scoped_input_readers_unknown_retained')
        for name, (fd, identity) in self.files.items():
            need(NativeOps.stamp(os.fstat(fd)) == identity and
                 NativeOps.stamp(os.stat(name, dir_fd=self.parent, follow_symlinks=False)) == identity,
                 'scoped_input_changed_retained')
        for name in self.files:
            os.unlink(name, dir_fd=self.parent)
        os.fsync(self.parent)
        return True

    def close(self):
        errors = []
        for name, (fd, _) in list(self.files.items()):
            try:
                os.close(fd)
            except OSError:
                errors.append(name)
            finally:
                # Never close this numeric fd again after an ambiguous close:
                # Linux may already have released/reused it.
                self.files.pop(name)
        if self.parent is not None:
            parent, self.parent = self.parent, None
            try:
                os.close(parent)
            except OSError:
                errors.append('parent')
        need(not errors, 'scoped_input_handle_close_unknown')


def _components():
    """Fixed checked-in composition; tests patch imports, not a public executor."""
    from types import SimpleNamespace
    from tools.development import redshift_fixture_factory as factory
    from tools.development import redshift_profile_inputs as profile_inputs
    from tools.development.redshift_timeout_factory import TimeoutFactory
    from tools.development.redshift_powerbi_session_ops import PowerBiSessionOperations
    from tools.development.redshift_powerbi_app_owner import collect, Deadlines, public_plan
    from tools.development.redshift_powerbi_bridge import Bridge
    from tools.redshift.pilot_preflight import pilot_lock
    return SimpleNamespace(factory=factory, profile_inputs=profile_inputs,
        TimeoutFactory=TimeoutFactory, Operations=PowerBiSessionOperations,
        NativeOps=NativeOps, Bridge=Bridge, collect=collect, Deadlines=Deadlines,
        fixture_plan=public_plan, lock=pilot_lock)


def _open_state(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        need(stat.S_ISDIR(info.st_mode) and info.st_uid == os.geteuid() and
             stat.S_IMODE(info.st_mode) == 0o700 and
             (info.st_dev, info.st_ino) ==
             (os.stat(path, follow_symlinks=False).st_dev,
              os.stat(path, follow_symlinks=False).st_ino), 'existing_private_app_state')
        return fd
    except BaseException:
        os.close(fd)
        raise


@contextmanager
def _interrupt_scope():
    import signal
    cleaning = False

    def begin_cleanup():
        nonlocal cleaning
        if not cleaning:
            cleaning = True
            for kind in (signal.SIGINT, signal.SIGTERM):
                signal.signal(kind, signal.SIG_IGN)

    def interrupted(kind, frame):
        if not cleaning:
            begin_cleanup()
            raise KeyboardInterrupt()

    previous = {}
    try:
        for kind in (signal.SIGINT, signal.SIGTERM):
            previous[kind] = signal.signal(kind, interrupted)
        yield begin_cleanup
    finally:
        for kind, handler in previous.items():
            signal.signal(kind, handler)


def _deadlines(components, admission, monotonic, wall):
    """Map original UTC admission once; no horizon reset after resume/setup."""
    session = admission['powerbi_sessions']
    seconds = lambda milliseconds: monotonic + milliseconds / 1000 - wall.timestamp()
    admitted = seconds(session['admitted_ms'])
    owner = min(admitted + BOUNDS['owner'],
                monotonic + (utc(admission['expires_utc']) - wall).total_seconds())
    child = min(admitted + BOUNDS['child'], owner - BOUNDS['recovery'])
    cleanup = min(admitted + BOUNDS['aggregate'], seconds(session['cleanup_deadline_ms']),
                  child - BOUNDS['recovery'])
    execution = min(admitted + BOUNDS['execution'], seconds(session['execution_deadline_ms']),
                    cleanup - BOUNDS['cleanup_reserve'])
    return components.Deadlines(owner=owner, child=child,
        availability=min(admitted + BOUNDS['availability'], execution),
        execution=execution, cleanup=cleanup, recovery_seconds=BOUNDS['recovery'])


def _manifest(plan, checkpoint, plan_path):
    tools = {name: str(pin_file(ref).resolve(strict=True))
             for name, ref in checkpoint['tools'].items()}
    pins = {}
    refs = [*checkpoint['tools'].values(), *checkpoint['authority_receipts'].values(),
            *plan['public_pins'], plan['checkpoint_plan'], plan['native_plan'],
            plan['operator_policy']]
    refs += [checkpoint[name] for name in ('index', 'expected', 'profiles', 'specs',
                                          'ci_receipt', 'checkpoint_expected',
                                          'checkpoint_partitions')]
    for ref in refs:
        path = str(pin_file(ref))
        need(path not in pins or pins[path] == ref['sha256'], 'conflicting_app_public_pin')
        pins[path] = ref['sha256']
    pins[str(plan_path)] = hashlib.sha256(plan_path.read_bytes()).hexdigest()
    return tools, dict(tools, pins=pins, ca_sha256=checkpoint['ca_sha256'],
                      security_groups=checkpoint['security_groups'])


def execute(plan_path, admission_path):
    """One admitted WSL resource owner; no native app launch verb exists here."""
    need(os.name == 'posix', 'local_WSL_resource_owner_required')
    plan_path, admission_path = Path(plan_path), Path(admission_path)
    components = _components()
    # Match the existing native/resource owner's canonical WSL pilot lock.
    # Never create its private parent or substitute an app-only global lock.
    pilot_fd = _open_state(PILOT_STATE)
    os.close(pilot_fd)
    with components.lock(PILOT_STATE):
        plan, checkpoint, admission = authorize(plan_path, admission_path)
        # Bind UTC to monotonic once. Public composition consumes this same
        # original horizon and cannot earn time by finishing or resuming late.
        monotonic = time.clock_gettime(time.CLOCK_MONOTONIC)
        wall = dt.datetime.now(dt.timezone.utc)
        deadlines = _deadlines(components, admission, monotonic, wall)
        deadlines.validate(time.clock_gettime(time.CLOCK_MONOTONIC))
        from tools.development.redshift_ctest_lifecycle import record, validate_fixture_plan
        fixture_plan = validate_fixture_plan(components.fixture_plan())
        tools, manifest = _manifest(plan, checkpoint, plan_path)
        profiles = json.loads(pin_file(checkpoint['profiles']).read_text(encoding='utf-8-sig'))
        specs = json.loads(pin_file(checkpoint['specs']).read_text(encoding='utf-8-sig'))
        blueprint = json.loads(pin_file(checkpoint['index']).read_text(encoding='utf-8-sig'))
        native_plan = json.loads(pin_file(plan['native_plan']).read_text(encoding='utf-8-sig'))
        state_fd = _open_state(STATE)
        run = STATE / plan['run_name']
        try:
            # EXCL run creation is the one-use claim, before native snapshots,
            # providers, resource resume, SQL or native control publication.
            try:
                os.mkdir(plan['run_name'], 0o700, dir_fd=state_fd)
            except FileExistsError:
                # Keep the canonical lock's OSError wrapper from obscuring
                # the concrete consumed-run refusal. Never reset/retry it.
                need(False, 'oneuse_app_run_already_consumed')
            record(run/'consumption.json', dict(source_head=plan['source_head'],
                run_name=plan['run_name'], nonce=admission['nonce'], scope=SCOPE,
                plan_sha256=admission['plan_sha256'], original_bounds=BOUNDS,
                owner_deadline=deadlines.owner, child_deadline=deadlines.child,
                execution_deadline=deadlines.execution, cleanup_deadline=deadlines.cleanup,
                fixture_plan=fixture_plan, GUI_launch_by_WSL=False))
            os.fsync(state_fd)
            held = None
            result = None
            with _interrupt_scope() as begin_cleanup:
                try:
                    held = ScopedInputs()
                    approval = dict(admission, resource=checkpoint['resource'],
                        run_directory=str(run), keep_cluster_running=False)
                    timeout = components.TimeoutFactory(**checkpoint['timeout_fixture'])
                    timeout.NativeOps = components.NativeOps
                    ops = components.Operations(native_source=None, native_sha256=None,
                        factory_source=None, factory_sha256=None, launcher_source=None,
                        launcher_sha256=None, approval=approval, preparation_manifest=manifest,
                        profile_manifest=manifest, profiles=profiles, specs=specs,
                        blueprint=blueprint, authority_receipts=checkpoint['authority_receipts'],
                        components=(timeout, components.factory, components.profile_inputs),
                        executables=(tools['cmake'], tools['ctest']))
                    ops.native = components.NativeOps(approval, manifest, run, deadlines.child)
                    ops.begin_cleanup = begin_cleanup
                    bridge = components.Bridge(ops, plan, native_plan, plan['native_plan'], admission)
                    deadlines.validate(ops.now())
                    result = components.collect(ops, bridge, deadlines, working_inputs=held)
                finally:
                    begin_cleanup()
                    if held is not None:
                        # Before collect, no native prepare has occurred. On
                        # constructor/binding failure close handles only; never
                        # retire carriers or guess that external readers stopped.
                        try:
                            held.close()
                        except BaseException as failure:
                            if result is None:
                                raise
                            result['cleanup_complete'] = False
                            result['cleanup_errors'].append(dict(phase='entry_scoped_handle_close',
                                error_type=type(failure).__name__))
        finally:
            os.close(state_fd)
    # Both interrupt handlers and the state fd/pilot lock have now closed.
    # A successful but late close/release is not timely process completion.
    # Preserve physical cleanup facts; acceptance is a separate, provisional
    # policy check requiring the actual resource-owner process exit witness.
    result.update(owner_deadline=deadlines.owner,
        entry_finalization_within_original_owner=(
            time.clock_gettime(time.CLOCK_MONOTONIC) < deadlines.owner),
        resource_owner_exit_status='PENDING_ACTUAL_RESOURCE_OWNER_PROCESS_EXIT',
        acceptance_requires_actual_resource_owner_exit=True,
        resource_owner_exit_verified=False)
    return result


def _policy_exit(result):
    # Never rederive this ceiling from UTC or renew it after publication.
    timely = time.clock_gettime(time.CLOCK_MONOTONIC) < result['owner_deadline']
    return 0 if (result['execution_complete'] and result['cleanup_complete'] and
                 result['entry_finalization_within_original_owner'] and timely) else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', nargs='?', choices=('preflight', 'run'))
    parser.add_argument('--plan', type=Path)
    parser.add_argument('--admission', type=Path)
    args = parser.parse_args(argv)
    if args.mode is None:
        print(json.dumps(dict(state='INERT_REVIEWED_PLAN_AND_ONEUSE_ADMISSION_REQUIRED',
            resource_operations=0, GUI_launch_by_WSL=False)))
        return 0
    if not args.plan:
        parser.error('--plan is required')
    if args.mode == 'preflight':
        plan, _ = read_plan(args.plan)
        print(json.dumps(dict(state='PUBLIC_APP_PREFLIGHT_ONLY', source_head=plan['source_head'],
            run_name=plan['run_name'], cases=list(CASES), resource_operations=0,
            private_inputs_read=False, GUI_launch_by_WSL=False, app_PASS_claimed=False)))
        return 0
    if not args.admission:
        parser.error('run requires the protected one-use --admission')
    result = execute(args.plan, args.admission)
    result['policy_exit_candidate_before_stdout_publication'] = _policy_exit(result)
    print(json.dumps(result), flush=True)
    # Exit0 is provisional child collection + cleanup only. ROOT still needs
    # actual local launcher exit/job evidence and app/native feature assessment.
    # Print/flush can return successfully after the owner deadline. The one
    # published object stays explicitly pending; actual exit is external proof.
    return _policy_exit(result)


if __name__ == '__main__':
    raise SystemExit(main())
