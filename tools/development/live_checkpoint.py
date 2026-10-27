#!/usr/bin/env python3
"""One normal live checkpoint entry: cloud-free preflight, or explicitly admitted CTest lifecycle."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import subprocess

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.development import redshift_ctest_lifecycle as lifecycle
from tools.development.live_preflight import preflight


def pinned(ref):
    if os.name == 'nt':
        from tools.development.windows_live_backend import HeldFile
        path = Path(ref['path'])
        lifecycle.need(path.is_absolute() and '..' not in path.parts
                       and path.name.lower() not in ('credentials','config','connection.txt','test.pgpass','admin.pgpass')
                       and 'provisioned-database-config' not in str(path)
                       and re.fullmatch('[0-9a-f]{64}',ref['sha256']) is not None, 'public_native_pin')
        held = HeldFile(path)
        try:
            lifecycle.need(hashlib.sha256(held.read()).hexdigest() == ref['sha256'], 'public_native_pin_changed')
        finally:
            held.close()
    else:
        lifecycle.verify_public_pin(ref['path'], ref['sha256'])
    return Path(ref['path'])


def prepare_plan(recipe_path, output):
    """Snapshot public file/tool/image bindings; never read credentials or admit a run."""
    recipe = json.loads(recipe_path.read_text(encoding='utf-8-sig'))
    def bind(path):
        path = Path(path).resolve(strict=True)
        return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
    plan = dict(recipe, schema_version=1, source_directory=str(ROOT),
                source_head=subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip())
    for key in ('index','expected','profiles','specs','ci_receipt','local_validation',
                'checkpoint_expected','checkpoint_partitions'):
        if recipe.get(key):plan[key] = bind(recipe[key])
    plan['tools'] = {key:bind(path) for key,path in recipe['tools'].items()}
    plan['authority_receipts'] = {key:bind(path) for key,path in recipe['authority_receipts'].items()}
    if 'native_windows' in recipe:
        from pathlib import PureWindowsPath
        from tools.development.redshift_windows import mounted
        def native_path(path):return str(PureWindowsPath(path))
        native = dict(recipe['native_windows'])
        for key in ('source_directory','working_directory','model','ca','python'):
            native[key] = native_path(native[key])
        native['tools'] = {key:native_path(path) for key,path in native['tools'].items()}
        native['runtime_directories'] = [native_path(path) for path in native['runtime_directories']]
        native['driver'] = {'path': native_path(native['driver']['path'])}
        native['driver']['sha256'] = bind(mounted(native['driver']['path']))['sha256']
        index = json.loads(pinned(plan['index']).read_text(encoding='utf-8-sig'))
        model = {'source_head': plan['source_head'], 'index': index,
                 'expected': json.loads(pinned(plan['expected']).read_text(encoding='utf-8-sig')),
                 'specs': json.loads(pinned(plan['specs']).read_text(encoding='utf-8-sig')),
                 'profiles': json.loads(pinned(plan['profiles']).read_text(encoding='utf-8-sig'))}
        # Native input index is public; only paths are normalized. Private profile
        # values are supplied after admission through held stdin/file custody.
        for image in index['images'].values():
            image['executable'] = native_path(image['executable'])
        with mounted(native['model']).open('x') as stream:
            json.dump(model,stream,indent=2);stream.write('\n')
        resource_index = json.loads(json.dumps(index))
        for image in resource_index['images'].values():
            image['executable'] = str(mounted(image['executable']))
        resource_index_path = output.with_name(output.stem+'-resource-index.json')
        with resource_index_path.open('x') as stream:
            json.dump(resource_index,stream,indent=2);stream.write('\n')
        plan['index'] = bind(resource_index_path)
        native.update(source_head=plan['source_head'],case_count=len(index['cases']))
        paths = {native['model'],native['ca'],native['python'],native['driver']['path'],
                 *native['tools'].values()}
        paths.update(native_path(PureWindowsPath(native['source_directory'])/path) for path in (
            'tools/development/windows_live_backend.py','tools/development/windows_native_job.py',
            'tools/development/redshift_fixture_factory.py','cmake/LiveSuite.cmake',
            'tests/development/live_suite/CMakeLists.txt'))
        paths.update(image['executable'] for image in index['images'].values())
        for directory in native['runtime_directories'] + [str(PureWindowsPath(native['tools']['ninja']).parent)]:
            paths.update(native_path(str(PureWindowsPath(directory)/p.name))
                         for p in mounted(directory).glob('*.dll'))
        native['public_pins'] = {path:bind(mounted(path))['sha256'] for path in sorted(paths)}
        plan['native_windows'] = native
    with output.open('x') as stream:
        json.dump(plan,stream,indent=2);stream.write('\n')
    return plan


LOCAL_VALIDATION_CONTROL_FILES = {
    'tools/development/live_checkpoint.py',
    'tools/development/test_live_checkpoint.py',
    'tools/development/CHECKPOINT_INPUTS.md',
    'tools/development/redshift_powerbi_app_checkpoint.py',
    'tools/development/test_redshift_powerbi_app_checkpoint.py',
    'tools/development/redshift_powerbi_sessions.py',
    'tools/development/test_redshift_powerbi_sessions.py',
    'tools/development/redshift_powerbi_session_ops.py',
    'tools/development/test_redshift_powerbi_session_ops.py',
    'tools/development/windows_powerbi_owner.py',
    'tools/development/test_windows_powerbi_publication.py',
}


def same_validation_product(producer, consumer, *, windows_sequence=False):
    """Keep compiled-producer reuse distinct from prior Windows sequencing proof."""
    lifecycle.need(all(isinstance(head, str) and re.fullmatch('[0-9a-f]{40}', head)
                       for head in (producer, consumer)), 'local_validation_source_heads')
    changed = subprocess.check_output(
        ['git', '-C', str(ROOT), 'diff', '--name-only', producer, consumer], text=True).splitlines()
    allowed = LOCAL_VALIDATION_CONTROL_FILES
    if windows_sequence:
        # WindowsOperations overrides open_inputs/stage_profiles and never uses
        # this POSIX DSN stager. Its correction still needs fresh WSL validation;
        # it does not invalidate the earlier completed Windows sequencing step.
        allowed = allowed | {'tools/development/redshift_profile_inputs.py',
                             'tools/development/test_redshift_profile_inputs.py'}
    lifecycle.need(set(changed) <= allowed,
                   'local_validation_same_product_source')


def validation_file(ref):
    # Public Windows artifacts may be read by the local WSL resource owner.
    from pathlib import PureWindowsPath
    path = ref['path']
    value = PureWindowsPath(path)
    if value.drive:
        lifecycle.need(value.is_absolute() and value.drive.upper() == 'C:' and
                       '..' not in value.parts, 'local_validation_absolute_C_drive')
        if os.name != 'nt':
            path = str(Path('/mnt/c').joinpath(*value.parts[1:]))
    elif os.name == 'nt' and path.startswith('/mnt/c/'):
        path = 'C:/' + path[len('/mnt/c/'):]
    return pinned(dict(ref, path=path))


def validate_local_validation(plan):
    """Trust the coordinator's reviewed result, at the existing CI-receipt boundary.

    The coordinator checks actual full XML/inventory/config/image association;
    this entry checks the pinned assessment, scope and unchanged product source.
    It never manufactures CI success or relaxes operational admission.
    """
    def load(ref):
        return json.loads(validation_file(ref).read_text(encoding='utf-8-sig'))
    lifecycle.need(bool(plan.get('local_validation')), 'local_validation_receipt_required')
    receipt = load(plan['local_validation'])
    profile = plan.get('validation_profile')
    lifecycle.need(receipt.get('schema_version') == 1 and
                   receipt.get('kind') == 'human-directed-local-validation-v1' and
                   receipt.get('reviewed') is True and receipt.get('status') == 'completed' and
                   receipt.get('human_direction') == 'windows-first-then-wsl' and
                   receipt.get('complete_applicable_offline') is True and
                   receipt.get('ci_acceptance') is False and
                   receipt.get('ci_status') == 'pending_not_accepted' and
                   receipt.get('source_head') == plan['source_head'] and
                   receipt.get('profile') == profile and
                   profile in (('windows-redshift',) if 'native_windows' in plan else
                               ('unixodbc-redshift', 'linux-iodbc-redshift')) and
                   all(type(receipt.get(key)) is int and receipt[key] == 0
                       for key in ('build_exit', 'ctest_exit')),
                   'reviewed_completed_human_local_validation')
    lifecycle.need(all(receipt.get(key) for key in ('producer_head', 'human_direction_ref', 'evidence')),
                   'local_validation_receipt_fields')
    same_validation_product(receipt['producer_head'], plan['source_head'])
    direction = load(receipt['human_direction_ref'])
    lifecycle.need(isinstance(direction.get('human_direction'), str) and
                   bool(direction['human_direction'].strip()), 'retained_human_direction_required')
    # These are public evidence/assessment bindings, not an alternative XML or
    # configuration engine. The coordinator preserves actual failures and skips.
    validation_file(receipt['evidence'])
    lifecycle.need(receipt.get('runtime_index_sha256') == plan['index']['sha256'] and
                   receipt.get('profiles_sha256') == plan['profiles']['sha256'],
                   'reviewed_local_runtime_inventory_and_profiles')
    if 'native_windows' not in plan:
        lifecycle.need(bool(receipt.get('windows_success')), 'windows_qualification_required_before_WSL')
        windows = load(receipt['windows_success'])
        same_validation_product(windows['source_head'], plan['source_head'], windows_sequence=True)
        lifecycle.need(windows.get('reviewed') is True and windows.get('status') == 'completed' and
                       windows.get('profile') == 'windows-redshift' and
                       windows.get('complete_applicable_windows_live') is True and
                       windows.get('cleanup_complete') is True and
                       type(windows.get('actual_exit')) is int and windows['actual_exit'] == 0,
                       'complete_successful_windows_qualification_before_WSL')
        validation_file(windows['evidence'])


def checkpoint_validation_mode(plan, ci, required):
    mode = plan.get('validation_mode', 'ci')
    lifecycle.need(mode in ('ci', 'human_directed_local'), 'checkpoint_validation_mode')
    if required and mode == 'ci':
        lifecycle.need(ci.get('source_head') == plan['source_head'] and
                       ci.get('applicable_ci_green') is True, 'exact_source_green_applicable_CI')
    if required:
        lifecycle.need(type(plan.get('security_groups')) is list and bool(plan['security_groups']),
                       'approved_existing_security_group_snapshot')
    return mode


def read_plan(path, *, require_ci=False):
    plan = json.loads(path.read_text(encoding='utf-8-sig'))
    lifecycle.need(plan['schema_version'] == 1, 'plan_schema')
    lifecycle.need(Path(plan['source_directory']).resolve() == ROOT
                   and re.fullmatch('[0-9a-f]{40}', plan['source_head']) is not None,
                   'this_exact_source_checkout')
    lifecycle.need(re.fullmatch('[A-Za-z0-9_.-]+', plan['run_name']) is not None,
                   'one_owned_run_name')
    for key in ('index', 'expected', 'profiles', 'specs'):
        pinned(plan[key])
    for ref in plan['tools'].values():
        pinned(ref)
    lifecycle.need(set(plan['tools']) == {'cmake','ctest','aws','psql','python'}, 'fixed_tool_inventory')
    ci = json.loads(pinned(plan['ci_receipt']).read_text(encoding='utf-8-sig')) if plan.get('ci_receipt') else {}
    validation_mode = checkpoint_validation_mode(plan, ci, require_ci)
    lifecycle.need(set(plan['authority_receipts']) == {'authority','applicability','cleanup_delegation'},
                   'existing_fixture_authority_receipts')
    for ref in plan['authority_receipts'].values():
        pinned(ref)
    resource = plan['resource']
    from tools.development.redshift_resource import validate_resource
    resource_kind = validate_resource(resource)
    if resource_kind == 'serverless':
        timeout = plan.get('timeout_fixture')
        lifecycle.need(type(timeout) is dict and set(timeout) == {
            'table', 'expected_principal', 'allow_current_identity', 'resource_kind'}
            and timeout['expected_principal'] is None
            and timeout['allow_current_identity'] is True
            and timeout['resource_kind'] == 'serverless',
            'serverless_current_principal_discovery_required')
    lifecycle.need(re.fullmatch('[0-9a-f]{64}', plan['ca_sha256']) is not None, 'CA_pin')
    # Completeness compares ordered tuples through the existing registrar. No
    # profile environment or protected database/credential file is opened here.
    index = json.loads(pinned(plan['index']).read_text(encoding='utf-8-sig'))
    expected = json.loads(pinned(plan['expected']).read_text(encoding='utf-8-sig'))
    lifecycle.need(index['schema_version'] == expected['schema_version'] == 1, 'inventory_schema')
    fields = ('id','image','profile','gtest_filter')
    lifecycle.need([{k: row[k] for k in fields} for row in index['cases']] == expected['cases']
                   and bool(expected['cases']), 'complete_ordered_inventory')
    partition_fields = {'partition', 'checkpoint_expected', 'checkpoint_partitions'}
    if partition_fields.intersection(plan):
        lifecycle.need(partition_fields <= set(plan), 'checkpoint_partition_bindings')
        from tools.development.redshift_powerbi_partition import validate_partition
        validate_partition(
            json.loads(pinned(plan['checkpoint_partitions']).read_text(encoding='utf-8-sig')),
            json.loads(pinned(plan['checkpoint_expected']).read_text(encoding='utf-8-sig')),
            plan['source_head'], plan['partition'], expected)
    if 'native_windows' in plan:
        validate_native_plan(plan, index, expected)
    if require_ci and validation_mode == 'human_directed_local':
        validate_local_validation(plan)
    return plan


def validate_native_plan(plan, index, expected):
    native = plan['native_windows']
    # This is a local Windows backend; WSL's owning images and Windows images
    # share the exact ordered case/profile inventory, not another smaller list.
    from pathlib import PureWindowsPath
    def local(path):
        value = PureWindowsPath(path)
        lifecycle.need(value.is_absolute() and value.drive.upper() == 'C:'
                       and '..' not in value.parts, 'native_C_drive_input')
        return Path(path) if os.name == 'nt' else Path('/mnt/c').joinpath(*value.parts[1:])
    lifecycle.need(native['source_head'] == plan['source_head']
                   and native['case_count'] == len(index['cases'])
                   and plan['run_name'].startswith('windows-live-'), 'native_full_scope')
    pins = native['public_pins']
    required = {native['model'], native['driver']['path'], native['ca'], native['python'],
                *native['tools'].values()}
    source = PureWindowsPath(native['source_directory'])
    required.update(str(source/path) for path in (
        'tools/development/windows_live_backend.py','tools/development/windows_native_job.py',
        'tools/development/redshift_fixture_factory.py','cmake/LiveSuite.cmake',
        'tests/development/live_suite/CMakeLists.txt'))
    lifecycle.need(required <= set(pins), 'complete_native_code_tool_image_pins')
    for path, digest in pins.items():
        pinned({'path':str(local(path)),'sha256':digest})
    lifecycle.need(pins[native['ca']] == plan['ca_sha256']
                   and pins[native['driver']['path']] == native['driver']['sha256'],
                   'native_TLS_driver_binding')
    model = json.loads(local(native['model']).read_text(encoding='utf-8-sig'))
    lifecycle.need(model['source_head'] == plan['source_head'] and model['expected'] == expected
                   and model['index']['cases'] == index['cases']
                   and model['specs'] == json.loads(pinned(plan['specs']).read_text(encoding='utf-8-sig'))
                   and model['profiles'] == json.loads(pinned(plan['profiles']).read_text(encoding='utf-8-sig')),
                   'native_model_same_source_inputs_profiles_inventory')
    lifecycle.need(set(model['index']['images']) == set(index['images']), 'native_owning_image_inventory')
    for key, image in model['index']['images'].items():
        lifecycle.need(image['sha256'] == index['images'][key]['sha256']
                       and local(image['executable']) == Path(index['images'][key]['executable'])
                       and pins.get(image['executable']) == image['sha256'],
                       'same_actual_native_owning_images')


def profile_driver_pins(profiles, public_pins):
    """Validate and bind every declared profile image before private acquisition."""
    from tools.development import redshift_fixture_factory as factory
    lifecycle.need(type(profiles) is dict, 'profile_inventory')
    result = dict(public_pins)
    drivers = {}
    for profile in profiles.values():
        factory.validate_profile(profile)
        driver = profile['private_input']['driver']
        if driver is not None:
            path, digest = driver['path'], driver['sha256']
            lifecycle.need(result.get(path, digest) == digest,
                           'profile_driver_pin_collision')
            result[path] = digest
            drivers[path] = digest
    for path, digest in drivers.items():
        pinned(dict(path=path, sha256=digest))
    return result


def validate_execution_bounds(plan, bounds):
    # This is the native delegate's existing necessary envelope, checked before
    # opening resource inputs. It does not extend any original deadline or
    # guarantee that fixture preparation leaves the required time at dispatch.
    if 'native_windows' in plan:
        lifecycle.need(bounds.execution >= 1560, 'native_complete_suite_execution_envelope')


def execute(plan, plan_path, admission_path):
    # On Windows, --wsl-source invokes this same fixed entry locally in WSL.
    # WSL owns resource credentials/SQL; the native backend owns its held test
    # inputs, HKCU DSN, Windows jobs and CTest. No POSIX custody on NTFS secrets.
    lifecycle.need(os.name == 'posix', 'use_local_WSL_resource_owner_on_Windows')
    from tools.development import redshift_fixture_factory as factory
    from tools.development import redshift_profile_inputs as profile_inputs
    from tools.development.redshift_timeout_factory import TimeoutFactory
    from tools.development.redshift_resource import NativeOps, private_json, utc
    from tools.development.redshift_ctest_ops import Operations
    from tools.development import redshift_ctest_fixtures as fixtures
    actual_head = subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git','-C',str(ROOT),'status','--porcelain'], text=True).strip()
    lifecycle.need(actual_head == plan['source_head'] and not dirty, 'clean_exact_admitted_source')
    # Finish public validation, including profile image hashes and collisions,
    # before even reading the private admission; no driver is loaded here.
    plan = read_plan(plan_path, require_ci=True)
    state = Path(plan['state_directory'])
    lifecycle.need(state.is_absolute() and state.is_dir() and not state.is_symlink(), 'owned_private_state')
    tools = {k: str(pinned(v).resolve(strict=True)) for k, v in plan['tools'].items()}
    public_pins = {v['path']: v['sha256'] for v in plan['tools'].values()}
    for key in ('index','expected','profiles','specs','ci_receipt','local_validation'):
        if not plan.get(key):
            continue
        path = str(validation_file(plan[key])) if key == 'local_validation' else plan[key]['path']
        public_pins[path] = plan[key]['sha256']
    if 'partition' in plan:
        for key in ('checkpoint_expected', 'checkpoint_partitions'):
            public_pins[plan[key]['path']] = plan[key]['sha256']
    for path in list((ROOT/'tools/development').glob('redshift_*.py')) + [Path(__file__), Path(factory.__file__)]:
        path = path.resolve()
        public_pins[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    profiles = json.loads(pinned(plan['profiles']).read_text(encoding='utf-8-sig'))
    public_pins = profile_driver_pins(profiles, public_pins)
    admission = private_json(admission_path)
    lifecycle.need(admission.get('reviewed') is True
                   and admission['plan_sha256'] == hashlib.sha256(plan_path.read_bytes()).hexdigest()
                   and admission['source_head'] == plan['source_head']
                   and admission['inventory_scope'] == (
                       'complete_checkpoint_partition' if 'partition' in plan
                       else 'complete_applicable')
                   and admission['run_name'] == plan['run_name']
                   and re.fullmatch('[A-Za-z0-9_-]{16,128}', admission['nonce']) is not None,
                   'existing_fresh_operational_admission')
    if 'partition' in plan:
        lifecycle.need(admission.get('partition') == plan['partition'] and
                       admission.get('checkpoint_sha256') ==
                       plan['checkpoint_expected']['sha256'],
                       'fresh_complete_checkpoint_partition_admission')
    bounds = lifecycle.Bounds(**admission['bounds']);bounds.validate()
    validate_execution_bounds(plan, bounds)
    lifecycle.need(utc(admission['expires_utc']) > dt.datetime.now(dt.timezone.utc)
                   + dt.timedelta(seconds=bounds.owner), 'finite_authority_horizon')
    # Existing exclusive pilot_lock and exclusive run directory consume the
    # one-use run. A nonce is never automatically renewed or a run reset.
    approval = dict(admission, resource=plan['resource'],
                    credential_source=admission['credential_source'],
                    database_config=admission['database_config'],
                    run_directory=str(state/plan['run_name']), keep_cluster_running=False)
    manifest = dict(tools, pins=public_pins, ca_sha256=plan['ca_sha256'],
                    security_groups=plan['security_groups'])
    timeout_factory = TimeoutFactory(**plan['timeout_fixture'])
    timeout_factory.NativeOps = NativeOps
    specs = json.loads(pinned(plan['specs']).read_text(encoding='utf-8-sig'))
    blueprint = json.loads(pinned(plan['index']).read_text(encoding='utf-8-sig'))
    ops_type = Operations
    extra = {}
    if plan.get('partition') == 'powerbi_relationship':
        extra['relationship_partition'] = True
    if 'native_windows' in plan:
        from tools.development.redshift_windows import WindowsOperations
        ops_type = WindowsOperations
        extra['native_plan'] = plan['native_windows']
    ops = ops_type(native_source=None, native_sha256=None, factory_source=None,
                     factory_sha256=None, launcher_source=None, launcher_sha256=None,
                     approval=approval, preparation_manifest=manifest,
                     profile_manifest=manifest, profiles=profiles, specs=specs,
                     blueprint=blueprint, authority_receipts=plan['authority_receipts'],
                     components=(timeout_factory, factory, profile_inputs),
                     executables=(tools['cmake'], tools['ctest']), **extra)
    config = lifecycle.Config(state/plan['run_name'], ROOT, plan['source_head'],
        pinned(plan['index']), pinned(plan['expected']), Path(tools['cmake']),
        Path(tools['ctest']), public_pins, bounds, state)
    class CompleteFixtures:
        plan = staticmethod(ops.combined_fixture_plan)
        prepare = staticmethod(fixtures.prepare)
        cleanup = staticmethod(fixtures.cleanup)
    if plan.get('partition') == 'powerbi_relationship':
        from tools.development import redshift_powerbi_partition as partition
        CompleteFixtures.plan = staticmethod(lambda: partition.plan(
            plan['resource'].get('kind', 'provisioned')))
        CompleteFixtures.prepare = staticmethod(partition.prepare)
        CompleteFixtures.cleanup = staticmethod(partition.cleanup)
    # Ignore a second interrupt only while mandatory cleanup is running.
    import signal
    def begin_cleanup():
        for kind in (signal.SIGINT, signal.SIGTERM):
            signal.signal(kind, signal.SIG_IGN)
    def interrupted(kind, frame):
        begin_cleanup()
        raise KeyboardInterrupt()
    ops.begin_cleanup = begin_cleanup
    previous = {kind: signal.signal(kind, interrupted) for kind in (signal.SIGINT, signal.SIGTERM)}
    try:
        # Public hashing/composition must not consume the already granted
        # credential/operational cleanup horizon before resource acquisition.
        lifecycle.need(utc(admission['expires_utc']) > dt.datetime.now(dt.timezone.utc)
                       + dt.timedelta(seconds=bounds.owner+2), 'remaining_operational_horizon')
        return lifecycle.run_once(config, ops, CompleteFixtures)
    finally:
        for kind, handler in previous.items():
            signal.signal(kind, handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('prepare','preflight','run'))
    parser.add_argument('--plan', type=Path)
    parser.add_argument('--recipe', type=Path, help='Public paths/resource recipe for prepare mode')
    parser.add_argument('--output', type=Path, help='Fresh preflight-only output directory')
    parser.add_argument('--admission', type=Path, help='Existing protected one-use operational admission')
    parser.add_argument('--wsl-source', help='Windows: matching checked-in WSL source checkout; resource ownership stays local WSL')
    args = parser.parse_args()
    if args.wsl_source:
        lifecycle.need(os.name == 'nt' and args.wsl_source.startswith('/')
                       and '..' not in Path(args.wsl_source).parts, 'fixed_local_WSL_backend')
        # Specific local backend entry, never SSH, a task, shell payload or a
        # generic remote executor. Paths only; no credentials in command args.
        command = ['wsl.exe','--exec','python3','-B',
                   args.wsl_source.rstrip('/')+'/tools/development/live_checkpoint.py',
                   args.mode]
        if args.plan:command += ['--plan',args.plan.as_posix()]
        if args.recipe:command += ['--recipe',args.recipe.as_posix()]
        if args.output:command += ['--output',args.output.as_posix()]
        if args.admission:command += ['--admission',args.admission.as_posix()]
        return subprocess.run(command).returncode
    os.umask(0o077)
    if args.mode == 'prepare':
        if not args.recipe or not args.output:
            parser.error('prepare requires --recipe and a new --output plan file')
        plan = prepare_plan(args.recipe, args.output)
        print(json.dumps({'plan_written':str(args.output),'source_head':plan['source_head'],
                          'resource_operations':0,'credential_inputs_read':False}))
        return 0
    if not args.plan:
        parser.error('--plan is required for preflight/run')
    plan = read_plan(args.plan)
    if args.mode == 'preflight':
        parser.error('--output is required for preflight') if not args.output else None
        result = preflight(pinned(plan['index']), pinned(plan['expected']), args.output,
                           str(pinned(plan['tools']['cmake'])))
        result['resource_run_supported'] = os.name == 'posix'
        result['native_windows_backend'] = 'native_windows' in plan
        result['CI_and_network_snapshot_present'] = bool(plan.get('ci_receipt') and plan.get('security_groups'))
        result['validation_mode'] = plan.get('validation_mode', 'ci')
        result['local_validation_present'] = bool(plan.get('local_validation'))
        print(json.dumps(result));return result['configure_exit']
    if not args.admission:
        parser.error('run requires the existing protected --admission')
    result = execute(plan, args.plan, args.admission)
    result['validation_mode'] = plan.get('validation_mode', 'ci')
    result['local_validation_is_CI_acceptance'] = False
    # CTest failure does not interrupt cleanup or become a successful checkpoint.
    print(json.dumps(result))
    return 0 if result['lifecycle_cleanup_complete'] and result['ctest'] is not None and result['ctest']['returncode'] == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
