"""Bounded native desktop owner. Default/import execution is inert.

No UI actions, authentication, SQL, AWS, feature parsing or cap choice. Run only
locally on the interactive desktop after ROOT's separately reviewed admission.
WSL supplies DATA via --stage/--publish-ack; it never invokes --run.
"""
import argparse
import contextlib
import ctypes as C
from ctypes import wintypes as W
import datetime as dt
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from windows_powerbi_contract import *


def now():
    return dt.datetime.now(dt.timezone.utc)


def execution_in_time(authority, monotonic_end, code):
    need(time.monotonic() < monotonic_end and now() < utc(authority['execution_end_utc']), code)


def anchor_limits(ctx, authority, wall_started, monotonic_started):
    """Anchor both original absolute cutoffs once, before reading authority.

    A backward wall-clock adjustment during a successful cleanup, publication
    or handle close must never extend the remaining monotonic duration.
    """
    ctx.return_cleanup_end_utc = authority['cleanup_end_utc']
    ctx.return_cleanup_end_monotonic = monotonic_started + (
        utc(authority['cleanup_end_utc']) - wall_started).total_seconds()
    return monotonic_started + (utc(authority['execution_end_utc']) - wall_started).total_seconds()


def cleanup_in_time(ctx):
    return (time.monotonic() < ctx.return_cleanup_end_monotonic and
            now() < utc(ctx.return_cleanup_end_utc))


def sha(body):
    return hashlib.sha256(body).hexdigest()


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=True, indent=2)+'\n').encode('utf-8')


def decode(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            need(key not in result, 'duplicate_json_key')
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=unique)


class BootstrapHold:
    """Read/share-read lock for public plan/source before importing native API.

    Win32 calls occur only on explicit CLI action. Private data subsequently uses
    canonical HeldFile (owner/DACL, no-reparse, single link, full identity).
    """
    def __init__(self, path, expected):
        self.path = Path(path)
        self.handle = None
        need(self.path.is_absolute(), 'absolute_bootstrap_path')
        for item in (self.path, *self.path.parents):
            need(not item.lstat().st_file_attributes & 1024, 'bootstrap_reparse')
        k = C.WinDLL('kernel32', use_last_error=True)
        self.close_api = k.CloseHandle
        self.close_api.argtypes, self.close_api.restype = [W.HANDLE], W.BOOL
        create = k.CreateFileW
        create.argtypes = [W.LPCWSTR,W.DWORD,W.DWORD,C.c_void_p,W.DWORD,W.DWORD,W.HANDLE]
        create.restype = W.HANDLE
        self.handle = create(str(self.path), 0x80000000, 1, None, 3, 0x200000, None)
        need(self.handle not in (None, C.c_void_p(-1).value), 'bootstrap_share_read_hold')
        read = k.ReadFile
        read.argtypes, read.restype = [W.HANDLE,C.c_void_p,W.DWORD,C.POINTER(W.DWORD),C.c_void_p], W.BOOL
        chunks, total = [], 0
        try:
            while True:
                block, length = C.create_string_buffer(65536), W.DWORD()
                need(read(self.handle, block, len(block), C.byref(length), None), 'bootstrap_read')
                if not length.value:
                    break
                total += length.value
                need(total <= 128*1024*1024, 'bootstrap_bound')
                chunks.append(block.raw[:length.value])
            self.raw = b''.join(chunks)
            need(sha(self.raw) == expected, 'bootstrap_actual_pin')
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.handle not in (None, C.c_void_p(-1).value):
            self.close_api(self.handle)
            self.handle = None


class Context:
    def __init__(self, plan_path, plan_sha):
        self.holds = []
        try:
            digest(plan_sha)
            held = self.hold(BootstrapHold(plan_path, plan_sha))
            self.plan_path = Path(plan_path)
            self.plan, self.plan_sha = validate_plan(decode(held.raw)), plan_sha
            self.repo = Path(self.plan['checkout'])
            for item in self.plan['tools'].values():
                self.hold(BootstrapHold(item['path'], item['sha256']))
            for name, value in self.plan['modules'].items():
                self.hold(BootstrapHold(Path(__file__).parent/name, value))
            for name, value in self.plan['canonical'].items():
                self.hold(BootstrapHold(self.repo/'tools/development'/name, value))
            command = [str(GIT), '-C', str(self.repo)]
            need(subprocess.check_output([*command, 'rev-parse', 'HEAD'], text=True, timeout=30).strip() == self.plan['source_head']
                 and not subprocess.check_output([*command, 'status', '--porcelain'], text=True, timeout=30).strip(), 'clean_exact_checkout')
            sys.path.insert(0, str(self.repo))
            self.native = importlib.import_module('tools.development.windows_live_backend')
            self.job = importlib.import_module('tools.development.windows_native_job')
            factory = importlib.import_module('tools.development.redshift_fixture_factory')
            need(Path(self.native.__file__).resolve() == self.repo/'tools/development/windows_live_backend.py'
                 and Path(self.job.__file__).resolve() == self.repo/'tools/development/windows_native_job.py'
                 and Path(factory.__file__).resolve() == self.repo/'tools/development/redshift_fixture_factory.py', 'canonical_import_owner')
            self.limits = factory.CANONICAL_LIMITS
            need(len(self.limits) == 20, 'canonical_twenty_limits')
            self.native.SID = SID
            self.native.current_identity()
            self.tree = self.native.PrivateTree()
            self.run = BASE/self.plan['run_id']
        except BaseException:
            self.close()
            raise

    def hold(self, item):
        self.holds.append(item)
        return item

    def read(self, path, private=False, maximum=65536):
        held = self.native.HeldFile(path, private=private)
        try:
            return held.read(maximum=maximum)
        finally:
            held.close()

    def message(self, check_clock=False):
        self.hold_run()
        payload = self.hold(self.native.HeldFile(self.run/'test-input.json', private=True))
        body = decode(payload.read(maximum=65536))
        a, c = validate_message(body, self.plan, self.plan_sha, now(), check_clock)
        staged = decode(self.read(self.run/'staged.json', private=True))
        need(staged['input_identity'] == list(payload.identity) and staged['nonce'] == a['nonce']
             and staged['plan_sha256'] == self.plan_sha and staged['source_head'] == self.plan['source_head'], 'exact_staged_input')
        return payload, a, c

    def hold_run(self):
        if any(path == self.run for path,_ in self.tree.directories):
            return
        self.native.ancestry(self.run)
        handle = self.native.create_file(str(self.run),131072,3,None,3,35651584,None)
        need(handle != C.c_void_p(-1).value, 'private_run_parent_hold')
        try:
            self.native.restricted_security(handle)
            info = self.native.Info()
            self.native.check(self.native.file_info(handle,C.byref(info)))
            need(info.attrs & 16 and not info.attrs & 1024, 'plain_held_run_directory')
            self.tree.directories.append((self.run,handle))
        except BaseException:
            self.native.close_handle(handle)
            raise

    def record(self, name, value):
        need(Path(name).name == name, 'fixed_record_name')
        path = self.run/name
        # Polling readers must never see the final name while PrivateTree's
        # exclusive writer still holds it. Publish only after flush, private
        # ACL validation, byte readback and writer closure have completed.
        # Windows rename refuses an existing destination: preserve one-use
        # records and retain the staging file if publication fails.
        staging = self.run/(name+'.publishing')
        self.tree.record(staging, value)
        os.rename(staging, path)
        return sha(self.read(path, private=True, maximum=2*1024*1024))

    def close(self):
        if hasattr(self, 'tree'):
            self.tree.close_handles()
        for item in reversed(self.holds):
            item.close()
        self.holds.clear()


def stage(ctx, raw):
    need(len(raw) <= 65536, 'stage_input_bound')
    a, c = validate_message(decode(raw), ctx.plan, ctx.plan_sha, now(), True)
    ctx.tree.mkdir(ctx.run)  # Exclusive; parent provisioned by ROOT, no adoption.
    path = ctx.run/'test-input.json'
    ctx.record('stage-intent.json', dict(**base_request(ctx,a,'stage-intent',0),
        input_path=str(path), input_bytes=len(raw), launches=0))
    try:
        ctx.tree.write(path, raw, retire=False)
        held = ctx.hold(ctx.native.HeldFile(path, private=True))
        ctx.record('staged.json', dict(schema_version=1, run_id=a['run_id'], nonce=a['nonce'],
            source_head=ctx.plan['source_head'], plan_sha256=ctx.plan_sha,
            input_identity=list(held.identity), authority=dict(a), stage_only=True,
            persisted_passwords=0, cloud_calls=0, launches=0))
    except BaseException as error:
        retired = not path.exists()
        try:
            # Exclusive owned directory; no child/app ever launched. A short
            # failed write can retire only an exact prefix of our supplied bytes.
            for item in ctx.holds:
                if getattr(item,'path',None) == path:
                    item.close()
            if path.exists():
                held = ctx.native.HeldFile(path,private=True)
                try:
                    partial = held.read(maximum=65536)
                    need(raw.startswith(partial), 'partial_stage_bytes_differ')
                    held.retire()
                    retired = not path.exists()
                finally:
                    held.close()
        finally:
            ctx.record('stage-failure.json',dict(**base_request(ctx,a,'stage-failure',0),
                error_type=type(error).__name__, launches=0, readers_known=True,
                input_retired=retired, partial_pending=[str(p) for p in ctx.tree.pending]))
        raise
    return 0


def retire_staged(ctx):
    need(not (ctx.run/'consumed.json').exists(), 'consumed_input_requires_native_reader_cleanup')
    payload, a, _ = ctx.message()
    payload.retire()
    ctx.record('unconsumed-retirement.json', dict(run_id=a['run_id'], nonce=a['nonce'],
        plan_sha256=ctx.plan_sha, input_absent=not (ctx.run/'test-input.json').exists(), launches=0))
    return 0


def base_request(ctx, a, stage_name, phase):
    return record_base(a, ctx.plan, ctx.plan_sha, stage_name, phase)


def consumed(ctx, a):
    raw = ctx.read(ctx.run/'consumed.json', private=True)
    data = decode(raw)
    bound_record(data, a, ctx.plan, ctx.plan_sha, 'consumed')
    return data, sha(raw)


def publish_ack(ctx, raw):
    need(len(raw) <= 32768, 'ack_input_bound')
    payload, a, _ = ctx.message()
    payload.close()  # ACK becomes public before this helper exits; no input reader then remains.
    _, consumed_sha = consumed(ctx, a)
    ack = decode(raw)
    name = ack.get('stage') if type(ack) is dict else None
    need(name in (*SAMPLES, 'cap-ready', 'reviewed-measurement', 'abort'), 'closed_ack_name')
    baseline_sha = None
    if name == 'abort':
        request, request_sha = base_request(ctx, a, 'abort', 0), consumed_sha
    else:
        request_name = 'cap-ready' if name == 'reviewed-measurement' else name
        body = ctx.read(ctx.run/(request_name+'.native-request.json'), private=True)
        request, request_sha = decode(body), sha(body)
        if name in ('cap-ready', 'reviewed-measurement'):
            body = ctx.read(ctx.run/'baseline-closed.json', private=True, maximum=2*1024*1024)
            result, baseline_sha = decode(body), sha(body)
            need(safe_readers(result['process']) and result['normal_close'] is True, 'baseline_readers_and_normal_close_before_cap')
    validate_ack(ack, a, ctx.plan, ctx.plan_sha, request, request_sha, baseline_sha)
    if name == 'cap-ready':
        measured_raw = ctx.read(ctx.run/'reviewed-measurement.ack.json', private=True)
        measured = validate_ack(decode(measured_raw),a,ctx.plan,ctx.plan_sha,request,request_sha,baseline_sha)
        need(measured['measurement_pin'] == ack['measurement_review'], 'same_reviewed_measurement_for_cap')
    need(now() <= utc(a['cleanup_end_utc']), 'no_ack_after_original_cleanup_end')
    # No run mutex here. Create-new protects each fixed ACK against replay/races.
    ctx.record(name+'.ack.json', ack)
    return 0


def control(ctx, a, stage_name, phase):
    path = ctx.run/(stage_name+'.control.json')
    if not path.exists():
        return False
    value = decode(ctx.read(path, private=True, maximum=4096))
    closed(value, RECORD_KEYS, 'control_closed_data')
    bound_record(value, a, ctx.plan, ctx.plan_sha, stage_name)
    need(value['phase'] == phase, 'control_actual_phase')
    return True


def publish_control(ctx, raw):
    """Local operator DATA-only creation; caller owns its finite process bound.

    Not invoked by the WSL resource bridge; never performs UI input or signals.
    """
    need(len(raw) <= 4096, 'control_input_bound')
    payload,a,_ = ctx.message()
    payload.close()
    consumed(ctx,a)
    value=decode(raw)
    recipe_control=type(value) is dict and value.get('stage') == 'effective-recipe-applied'
    closed(value,(*RECORD_KEYS,'recipe_sha256','evidence') if recipe_control else RECORD_KEYS,'control_closed_data')
    name=value['stage']
    bound_record(value,a,ctx.plan,ctx.plan_sha,name)
    capture=re.fullmatch(r'phase-([012])-capture(0[1-9]|1[0-2])',name)
    need(name in (*SAMPLES,'reopen','finish','effective-recipe-applied') or capture is not None, 'fixed_local_control_name')
    if capture:
        need(value['phase'] == int(capture.group(1)), 'capture_phase_binding')
    if name in ('reopen','finish'):
        need(value['phase'] == 2, 'reopen_finish_phase2')
    if recipe_control:
        verify_recipe_attestation(ctx,a,value)
    need(now() <= utc(a['execution_end_utc']), 'local_control_original_execution_end')
    ctx.record(name+'.control.json',value)
    return 0


def check_abort(ctx, a):
    path = ctx.run/'abort.ack.json'
    if path.exists():
        _, csha = consumed(ctx, a)
        validate_ack(decode(ctx.read(path, private=True)), a, ctx.plan, ctx.plan_sha,
                     base_request(ctx, a, 'abort', 0), csha)
        raise RuntimeError('resource_owner_requested_owned_scope_abort')


def wait_ack(ctx, a, name, request_sha, request, end, baseline_sha=None):
    while time.monotonic() < end:
        check_abort(ctx, a)
        path = ctx.run/(name+'.ack.json')
        if path.exists():
            result = validate_ack(decode(ctx.read(path, private=True)), a, ctx.plan, ctx.plan_sha,
                                  request, request_sha, baseline_sha)
            execution_in_time(a, end, 'successful_ack_verification_after_original_cutoff')
            return result
        time.sleep(.1)
    raise RuntimeError('original_deadline_waiting_' + name)


def assert_desktop():
    # Current local operator's actual interactive Session1/WinSta0/Default.
    # Physical console ID is deliberately not required (RDP/console can differ).
    k = C.WinDLL('kernel32', use_last_error=True)
    u = C.WinDLL('user32', use_last_error=True)
    sid = W.DWORD()
    f = k.ProcessIdToSessionId
    f.argtypes, f.restype = [W.DWORD,C.POINTER(W.DWORD)], W.BOOL
    need(f(os.getpid(), C.byref(sid)) and sid.value == 1, 'interactive_session1')
    station, desktop = u.GetProcessWindowStation, u.GetThreadDesktop
    station.argtypes, station.restype = [], W.HANDLE
    desktop.argtypes, desktop.restype = [W.DWORD], W.HANDLE
    tid = k.GetCurrentThreadId
    tid.argtypes, tid.restype = [], W.DWORD
    info = u.GetUserObjectInformationW
    info.argtypes, info.restype = [W.HANDLE,C.c_int,C.c_void_p,W.DWORD,C.POINTER(W.DWORD)], W.BOOL
    for handle, expected in ((station(), 'WinSta0'), (desktop(tid()), 'Default')):
        buf, length = C.create_unicode_buffer(256), W.DWORD()
        need(info(handle, 2, buf, C.sizeof(buf), C.byref(length)) and buf.value.casefold() == expected.casefold(), 'actual_interactive_desktop')


def installed_source_applicability(ctx, installed_head):
    """Prove actual Git product-input identity; a receipt cannot grant it.

    This belongs to external application qualification, not the product SDK.
    Only operational Python/docs and the two fixed session-identity fixtures
    may differ. Build graphs, packaging, SDK, adapters and workflows cannot.
    """
    current = ctx.plan['source_head']
    need(all(type(value) is str and re.fullmatch('[0-9a-f]{40}', value)
             for value in (installed_head, current)), 'own_source_commit_identity')
    end = time.monotonic() + 30
    # Ignore inherited repository/index/config overrides; these are exact
    # local read-only Git operations, with no hooks, textconv or external diff.
    env = {key:value for key,value in os.environ.items() if not key.upper().startswith('GIT_')}
    env.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull)
    command = [str(GIT), '--no-replace-objects', '--no-optional-locks', '--no-pager',
               '-c', 'core.fsmonitor=false',
               '-c', 'core.untrackedCache=false', '-C', str(ctx.repo)]
    def git(*args):
        left = end - time.monotonic()
        need(left > 0, 'source_comparison_deadline')
        result = subprocess.run([*command, *args], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=left, check=False)
        need(result.returncode == 0 and len(result.stdout) <= 2*1024*1024
             and len(result.stderr) <= 2*1024*1024 and time.monotonic() < end,
             'source_comparison_git')
        return result.stdout
    def clean_current():
        need(git('rev-parse', '--verify', 'HEAD').decode('ascii').strip() == current
             and not git('status', '--porcelain', '--untracked-files=all',
                         '--ignore-submodules=none').strip(), 'clean_exact_product_comparison_checkout')
    clean_current()
    for head in {installed_head, current}:
        need(git('cat-file', '-t', head).strip() == b'commit', 'own_source_commit_object')
    if installed_head != current:
        git('merge-base', '--is-ancestor', installed_head, current)
    raw = git('diff-tree', '--no-commit-id', '--no-ext-diff', '--no-textconv',
              '--no-renames', '-r', '--name-only', '-z', installed_head, current, '--')
    names = raw.decode('utf-8', 'strict').split('\0') if raw else []
    if names:
        need(names[-1] == '', 'source_comparison_path_framing')
        names.pop()
    need(len(names) <= 4096, 'source_comparison_path_bound')
    fixture_paths = {'tests/integration/it_bind_col_real.cpp', 'tests/integration/it_metadata_real.cpp'}
    def operational(path):
        return (path.startswith('tools/development/') and path.endswith(('.py', '.md'))
                and '..' not in path.split('/') and '\\' not in path
                and not any(char in path for char in '\r\n\0'))
    need(all(path in fixture_paths or operational(path) for path in names),
         'installed_product_build_inputs_changed')
    trees = {head:git('rev-parse', '--verify', head+'^{tree}').decode('ascii').strip()
             for head in {installed_head, current}}
    need(all(re.fullmatch('[0-9a-f]{40}', value) for value in trees.values()), 'source_tree_identity')
    clean_current()
    return dict(installed_source_head=installed_head, application_source_head=current,
                installed_tree=trees[installed_head], application_tree=trees[current],
                changed_paths=names, unchanged_product_build_inputs=True,
                comparison='actual_local_git_commit_objects_and_fixed_path_allowlist',
                comparison_seconds=30, installed_source_relabelled=False)


def hold_runtime(ctx):
    plan, driver = ctx.plan, ctx.plan['driver']
    for item in (plan['application'], plan['ca'], driver['install_receipt'], *driver['payload']):
        held = ctx.hold(ctx.native.HeldFile(item['path']))
        need(sha(held.read()) == item['sha256'], 'actual_installed_asset_pin')
    receipt = decode(ctx.read(driver['install_receipt']['path']))
    # ROOT normalizes its separately verified actual installation into this
    # reviewed public receipt; not a self-generated installer proof.
    closed(receipt, ('schema_version', 'status', 'independent_review', 'kind', 'source_head',
                     'driver', 'payload', 'installer_sha256', 'evidence'), 'install_binding_schema')
    need(receipt['schema_version'] == 1 and receipt['status'] == 'VERIFIED_OWNED_INSTALL'
         and receipt['independent_review'] == 'ACCEPT' and receipt['kind'] == driver['kind']
         and receipt['driver'] == dict(path=driver['path'], sha256=driver['sha256'])
         and receipt['payload'] == driver['payload'], 'actual_install_binding')
    need(type(receipt['evidence']) is list and 1 <= len(receipt['evidence']) <= 8, 'install_evidence')
    for item in receipt['evidence']:
        pin(item)
    if driver['kind'] == 'own':
        need(receipt['installer_sha256'] is None, 'own_installed_source')
        inventory_pin = driver['inventory']
        held = ctx.hold(ctx.native.HeldFile(inventory_pin['path']))
        body = held.read()
        need(sha(body) == inventory_pin['sha256'], 'inventory_pin')
        inv = decode(body)
        need(inv['sourceRevision'] == receipt['source_head'] and inv['targetDatabase'] == 'REDSHIFT', 'own_inventory_source')
        applicability = installed_source_applicability(ctx, inv['sourceRevision'])
        expected = []
        for item in inv['files']:
            need(Path(item['file']).name == item['file'], 'flat_own_inventory')
            expected.append(dict(path=str(OWN_ROOT/item['file']), sha256=item['sha256'].lower()))
        need(sorted(expected, key=lambda x:x['path']) == sorted(driver['payload'], key=lambda x:x['path']), 'complete_own_inventory_payload')
    else:
        need(receipt['source_head'] is None and receipt['installer_sha256'] == AWS_MSI_SHA, 'exact_vendor_msi_receipt')
    import winreg
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, 'SOFTWARE\\ODBC\\ODBCINST.INI\\'+driver['description'],
                        0, winreg.KEY_READ|winreg.KEY_WOW64_64KEY) as key:
        value, kind = winreg.QueryValueEx(key, 'Driver')
        need(kind == winreg.REG_SZ and Path(value) == Path(driver['path']), 'actual_registered_image')
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, 'SOFTWARE\\ODBC\\ODBCINST.INI\\ODBC Drivers',
                        0, winreg.KEY_READ|winreg.KEY_WOW64_64KEY) as key:
        need(winreg.QueryValueEx(key, driver['description']) == ('Installed', winreg.REG_SZ), 'installed_mapping')
    if driver['kind'] == 'own':
        ctx.record('installed-source-applicability.json', applicability)


def fields(ctx, connection):
    driver = ctx.plan['driver']
    if driver['kind'] == 'own':
        result = dict(connection, DRIVER=driver['path'], SSL='1', TRANSPORTMODE='Sync',
            LoginTimeout='5', QueryTimeout='5', LogLevel='Error', LogFile=str(ctx.run/'driver.log'), LogQueries='false')
        result.update({k:str(v) for k,v in ctx.limits.items()})
        result['MAXRESULTS'] = '16'  # Reviewed app-only fanout override; native profiles untouched.
        return result
    return dict(DRIVER=driver['path'], UID=connection['UID'], HostName=connection['SERVER'],
        PortNumber='5439', Database=connection['DATABASE'], IAM='0', AutoCreate='0',
        SSLMode='verify-full', TrustStore=connection['SSLCAFILE'], Min_TLS='1.2',
        LoginTimeout='5', QueryTimeout='5', ConnectionRetryCount='0',
        DatabaseMetadataCurrentDbOnly='1', LogLevel='2', LogPath=str(ctx.run/'vendor-logs'))


def aws_connection_recipe(ctx):
    """Fixed PowerBI connection-string surface, no credentials or extra options."""
    need(ctx.plan['driver']['kind'] == 'aws2', 'aws_recipe_only')
    # Directory derives from the closed run namespace; braced ODBC value retains
    # spaces and separators without caller-selected connection-string syntax.
    log_path=str(ctx.run/'vendor-logs').replace('}','}}')
    return ('DSN=ODBCPP_AWS2_COMPARE007;LogLevel=2;LogPath={'+log_path+'};\n').encode('utf-8')


def stage_aws_recipe(ctx,a):
    raw=aws_connection_recipe(ctx)
    path=ctx.run/'app-connection.txt'
    ctx.tree.write(path,raw,retire=False)
    held=ctx.hold(ctx.native.HeldFile(path,private=True))
    need(held.read(maximum=4096) == raw, 'effective_recipe_actual_bytes')
    metadata=dict(**base_request(ctx,a,'effective-connection-recipe',0),
        recipe_file=dict(path=str(path),sha256=sha(raw)),input_identity=list(held.identity),
        intended_surface='PowerBI ODBC advanced connection string',
        credentials_present=False,global_logging_mutations=0,
        application_status='NOT_OBSERVED',log_coverage_claimed=False)
    ctx.record('app-connection-recipe.json',metadata)
    return held,metadata


def verify_recipe_attestation(ctx,a,value):
    closed(value,(*RECORD_KEYS,'recipe_sha256','evidence'),'effective_recipe_attestation_schema')
    bound_record(value,a,ctx.plan,ctx.plan_sha,'effective-recipe-applied')
    expected=aws_connection_recipe(ctx)
    need(value['recipe_sha256'] == sha(expected), 'exact_effective_recipe_attested')
    need(type(value['evidence']) is list and 1 <= len(value['evidence']) <= 8,'recipe_application_evidence_bound')
    for item in value['evidence']:
        pin(item)
    need(ctx.read(ctx.run/'app-connection.txt',private=True,maximum=4096) == expected,
         'held_effective_recipe_bytes')
    return value


def effective_vendor_diagnostics(ctx,a,metadata,artifacts):
    value=None
    path=ctx.run/'effective-recipe-applied.control.json'
    if path.exists():
        value=verify_recipe_attestation(ctx,a,decode(ctx.read(path,private=True,maximum=16384)))
    logs=[item for item in artifacts if Path(item['path']).parent == ctx.run/'vendor-logs' and item['bytes'] > 0]
    return dict(recipe_file=metadata['recipe_file'],
        recipe_application='OPERATOR_REPORTED_APPLIED' if value else 'NOT_OBSERVED',
        recipe_application_evidence=value['evidence'] if value else [],
        log_artifacts_status='OBSERVED_BOUNDED_NONEMPTY_FILES' if logs else 'NOT_OBSERVED',
        log_artifacts=logs,diagnostics_incomplete=not bool(value and logs),
        effective_log_level_verified_by_native=False,feature_or_logcoverage_PASS_claimed=False,
        global_logging_mutations=0,recipe_retired=False)


@contextlib.contextmanager
def app_environment(ctx):
    selected = lambda key: key.upper().startswith(('AWS_', 'ODBCPP_', 'GTEST_', 'SYSODBC')) or key.upper() in (
        'ODBCINI', 'ODBCSYSINI', 'ODBCINSTINI')
    previous = {k:v for k,v in os.environ.items() if selected(k)}
    for key in previous:
        del os.environ[key]
    try:
        if ctx.plan['driver']['kind'] == 'own':
            os.environ.update(ODBCPP_TRACE='1', ODBCPP_TRACE_FILE=str(ctx.run/'driver-trace.log'),
                ODBCPP_TRACE_FORMAT='json', ODBCPP_TRACE_QUERIES='0', ODBCPP_TRACE_SENSITIVE='0',
                ODBCPP_TRACE_MAX_SIZE='16777216', ODBCPP_TRACE_MAX_FILES='1')
        yield
    finally:
        for key in list(os.environ):
            if selected(key):
                del os.environ[key]
        os.environ.update(previous)  # Values restored in memory; never reported.


def diagnostic_files(ctx, final=False):
    result = []
    candidates = [ctx.run/'driver.log', ctx.run/'driver-trace.log', ctx.run/'driver-trace.log.1']
    vendor = ctx.run/'vendor-logs'
    if vendor.exists():
        files = list(vendor.iterdir())
        need(len(files) <= 64, 'vendor_log_file_count_bound')
        candidates.extend(files)
    for path in candidates:
        if path.exists():
            need(path.stat().st_size <= 16*1024*1024, 'diagnostic_polling_byte_limit')
            if final:
                held = ctx.native.HeldFile(path, private=True)
                try:
                    body = held.read(maximum=16*1024*1024)
                    result.append(dict(path=str(path), bytes=len(body), sha256=sha(body)))
                finally:
                    held.close()
    return result


def execute(ctx):
    from windows_powerbi_provider import ProviderUserDsn
    from windows_powerbi_scope import PowerBiScope
    assert_desktop()
    wall_started, monotonic_started = now(), time.monotonic()
    payload, a, connection = ctx.message(check_clock=True)
    end = anchor_limits(ctx, a, wall_started, monotonic_started)
    result = dict(schema_version=1, source_head=ctx.plan['source_head'], plan_sha256=ctx.plan_sha,
        run_id=a['run_id'], nonce=a['nonce'], original_execution_end_utc=a['execution_end_utc'],
        original_cleanup_end_utc=a['cleanup_end_utc'], phases=[], feature_results_parsed=False,
        inferred_connection_demand=False, feature_PASS_claimed=False, native_cleanup_complete=False,
        readers_known=True, execution_complete=False, errors=[])
    dsn, scope, consumed_input, samples = None, None, False, {}
    recipe,recipe_metadata=None,None
    if ctx.plan['driver']['kind'] == 'aws2':
        result['effective_vendor_logging']=dict(recipe_application='NOT_OBSERVED',
            log_artifacts_status='NOT_OBSERVED',diagnostics_incomplete=True,
            effective_log_level_verified_by_native=False,feature_or_logcoverage_PASS_claimed=False,
            global_logging_mutations=0,recipe_retired=False)
    terminal_code = 72
    try:
        ctx.record('consumed.json', dict(**base_request(ctx,a,'consumed',0),
            max_launches=3, original_execution_end_utc=a['execution_end_utc'], input_identity=list(payload.identity)))
        consumed_input = True
        hold_runtime(ctx)
        if ctx.plan['driver']['kind'] == 'aws2':
            ctx.tree.mkdir(ctx.run/'vendor-logs')
        else:
            for name in ('driver.log', 'driver-trace.log'):
                ctx.tree.write(ctx.run/name, b'', retire=False)
        dsn = ProviderUserDsn(ctx.plan['driver']['dsn'], ctx.run, ctx.tree,
            backend=ctx.native, native_job=ctx.job, driver=ctx.plan['driver'], canonical_limits=ctx.limits)
        dsn.prepare(fields(ctx, connection))
        if ctx.plan['driver']['kind'] == 'aws2':
            recipe,recipe_metadata=stage_aws_recipe(ctx,a)
        ctx.record('dsn-ready.json', dict(**base_request(ctx,a,'dsn-ready',0), dsn=dsn.NAME,
            TLS_required=True, password_persisted=False, cached_application_credentials_possible=True,
            effective_connection_recipe=recipe_metadata['recipe_file'] if recipe_metadata else None))
        with app_environment(ctx):
            for phase in (0,1,2):
                check_abort(ctx, a)
                need(time.monotonic() < end-180, 'original_launch_cleanup_reserve')
                dsn.verify()
                if phase == 0:
                    need(0 <= (now()-utc(a['cap_observed_utc'])).total_seconds() <= 300, 'baseline_cap_observation_still_fresh')
                elif phase == 1:
                    body = ctx.read(ctx.run/'baseline-closed.json', private=True, maximum=2*1024*1024)
                    baseline_sha = sha(body)
                    request = dict(**base_request(ctx,a,'cap-ready',0), baseline_closed_sha256=baseline_sha,
                        workload_sha256=ctx.plan['workload_sha256'], original_execution_end_utc=a['execution_end_utc'])
                    request_sha = ctx.record('cap-ready.native-request.json', request)
                    measured = wait_ack(ctx, a, 'reviewed-measurement', request_sha, request, end-180, baseline_sha)
                    cap_ack = wait_ack(ctx, a, 'cap-ready', request_sha, request, end-180, baseline_sha)
                    need(cap_ack['measurement_review'] == measured['measurement_pin'], 'same_reviewed_measurement_for_cap')
                else:
                    while not control(ctx,a,'reopen',2):
                        check_abort(ctx,a)
                        need(time.monotonic() < end-180, 'original_reopen_reserve')
                        time.sleep(.1)
                remaining = int(end-time.monotonic())-120
                need(0 < remaining <= 1800, 'original_launch_budget')
                # Constructor census can discover a foreign/live consumer. Do
                # not retire a newly staged DSN/input on an ambiguous census.
                result['readers_known'] = False
                scope = PowerBiScope(ctx.run/('app-'+str(phase)+'-console.log'), native=ctx.native,
                    job_path=ctx.repo/'tools/development/windows_native_job.py',
                    job_sha=ctx.plan['canonical']['windows_native_job.py'],
                    module_pins={p['path']:p['sha256'] for p in ctx.plan['driver']['payload']})
                scope.start(remaining)
                bootstrap_end = min(end-120,time.monotonic()+30)
                while not scope.receipt.get('assignedBeforeExecution') and scope.worker.is_alive() and time.monotonic()<bootstrap_end:
                    check_abort(ctx,a)
                    scope.worker.join(timeout=.05)
                need(scope.receipt.get('assignedBeforeExecution') is True, 'app_assigned_before_execution')
                ctx.record('phase-'+str(phase)+'-initial-capture.json', scope.capture())
                ctx.record('phase-'+str(phase)+'-started.json', dict(**base_request(ctx,a,'phase-started',phase),
                    root_pid=scope.receipt['childPid'], original_execution_end_utc=a['execution_end_utc']))
                seen = set()
                while scope.worker.is_alive():
                    check_abort(ctx,a)
                    need(time.monotonic()<end-90, 'original_execution_reserve')
                    diagnostic_files(ctx)
                    for number in range(1,13):
                        name = 'phase-'+str(phase)+'-capture'+str(number).zfill(2)
                        if name not in seen and control(ctx,a,name,phase):
                            ctx.record(name+'.json', dict(**base_request(ctx,a,name,phase), capture=scope.capture()))
                            seen.add(name)
                    for name in SAMPLES:
                        if name not in samples and control(ctx,a,name,phase):
                            capture_sha = ctx.record(name+'.capture.json', scope.capture())
                            request = dict(**base_request(ctx,a,name,phase), capture_sha256=capture_sha,
                                workload_sha256=ctx.plan['workload_sha256'], original_execution_end_utc=a['execution_end_utc'])
                            request_sha = ctx.record(name+'.native-request.json', request)
                            samples[name] = (request, request_sha)
                    scope.worker.join(timeout=.1)
                process = scope.finish()
                result['phases'].append(dict(phase=phase, process=process))
                scope = None
                result['readers_known'] = safe_readers(process)
                need(result['readers_known'], 'phase_reader_cleanup_unknown')
                execution_in_time(a, end, 'successful_phase_verification_after_original_cutoff')
                normal_close = (process['returned_exit'] == 0 and process.get('error_type') is None
                    and not process['receipt'].get('timedOut') and not process['receipt'].get('operator_abort'))
                need(normal_close, 'normal_app_close_required')
                for name,(request,request_sha) in samples.items():
                    wait_ack(ctx,a,name,request_sha,request,end-120)
                if phase == 0:
                    ctx.record('baseline-closed.json', dict(**base_request(ctx,a,'baseline-closed',0),
                        process=process, normal_close=True, original_cap=a['original_cap'],
                        demand_inferred=False, workload_sha256=ctx.plan['workload_sha256']))
            need(set(samples) == set(SAMPLES), 'three_event_samples_required')
            need(control(ctx,a,'finish',2), 'explicit_finish_after_planned_reopen')
            execution_in_time(a, end, 'successful_final_control_after_original_cutoff')
            result['execution_complete'] = True
            result['execution_verified_utc'] = now().isoformat()
    except BaseException as error:
        result['errors'].append(dict(error_type=type(error).__name__, location=ctx.native.error_location(error)))
    finally:
        if scope is not None:
            try:
                scope.abort()  # Only original owned job; no PID-selected kill.
                process = scope.finish()
                result['phases'].append(dict(phase='interrupted', process=process))
                result['readers_known'] = safe_readers(process)
            except BaseException as error:
                result['readers_known'] = False
                result['errors'].append(dict(cleanup_error_type=type(error).__name__))
        if consumed_input and result['readers_known']:
            try:
                if dsn is not None:
                    dsn.cleanup()  # UNKNOWN create/mutation remains a fence.
                result['diagnostics'] = diagnostic_files(ctx, final=True)
                if recipe_metadata is not None:
                    result['effective_vendor_logging']=effective_vendor_diagnostics(ctx,a,recipe_metadata,result['diagnostics'])
                if recipe is not None:
                    recipe.retire()
                    result['effective_vendor_logging']['recipe_retired']=not (ctx.run/'app-connection.txt').exists()
                payload.retire()
                result['native_cleanup_complete'] = not (ctx.run/'test-input.json').exists()
            except BaseException as error:
                result['errors'].append(dict(cleanup_error_type=type(error).__name__))
        result['within_original_cleanup_end'] = cleanup_in_time(ctx)
        terminal_code = 0 if (result['execution_complete'] and result['native_cleanup_complete']
                              and result['readers_known'] and result['within_original_cleanup_end']
                              and not result['errors']) else 72
        result.update(exit=None, candidate_exit=terminal_code, finished_utc=now().isoformat(),
            acceptance_status='PENDING_POSTPUBLICATION_VALIDATION_AND_ACTUAL_OWNER_EXIT')
        candidate_sha = ctx.record('terminal.json', result)
        candidate_publication_returned = now()
        candidate_in_time = cleanup_in_time(ctx)
        validation = dict(**base_request(ctx,a,'terminal-validation',2),
            terminal_candidate_sha256=candidate_sha,
            terminal_publication_returned_utc=candidate_publication_returned.isoformat(),
            terminal_publication_within_original_cleanup_end=candidate_in_time,
            candidate_exit=terminal_code, policy_exit_before_validation_publication=terminal_code if candidate_in_time else 72,
            acceptance_status='PENDING_VALIDATION_PUBLICATION_AND_ACTUAL_OWNER_EXIT')
        ctx.record('terminal-validation.json', validation)
        # A successful record/flush can itself return late. Never rewrite its
        # immutable candidate as accepted: actual owner-process exit is the seal.
        terminal_code = terminal_code if candidate_in_time and cleanup_in_time(ctx) else 72
        print(json.dumps(dict(state='NATIVE_APP_TERMINAL_CANDIDATE', **result)), flush=True)
        terminal_code = terminal_code if cleanup_in_time(ctx) else 72
    return terminal_code


def verify_owner_candidates(ctx, authority):
    """Bind immutable child receipts; process exit is checked by its parent job."""
    raw = ctx.read(ctx.run/'terminal.json',private=True,maximum=2*1024*1024)
    candidate = decode(raw)
    need(candidate['schema_version'] == 1 and candidate['run_id'] == authority['run_id']
         and candidate['nonce'] == authority['nonce'] and candidate['source_head'] == ctx.plan['source_head']
         and candidate['plan_sha256'] == ctx.plan_sha
         and candidate['original_execution_end_utc'] == authority['execution_end_utc']
         and candidate['original_cleanup_end_utc'] == authority['cleanup_end_utc']
         and candidate['acceptance_status'] == 'PENDING_POSTPUBLICATION_VALIDATION_AND_ACTUAL_OWNER_EXIT'
         and candidate['exit'] is None, 'exact_child_candidate')
    candidate_pin = dict(path=str(ctx.run/'terminal.json'),sha256=sha(raw))
    raw = ctx.read(ctx.run/'terminal-validation.json',private=True,maximum=65536)
    validation = decode(raw)
    bound_record(validation,authority,ctx.plan,ctx.plan_sha,'terminal-validation')
    need(validation['phase'] == 2 and validation['terminal_candidate_sha256'] == candidate_pin['sha256']
         and validation['candidate_exit'] == candidate['candidate_exit']
         and validation['acceptance_status'] == 'PENDING_VALIDATION_PUBLICATION_AND_ACTUAL_OWNER_EXIT', 'exact_child_validation')
    validation_pin = dict(path=str(ctx.run/'terminal-validation.json'),sha256=sha(raw))
    return candidate, validation, [candidate_pin,validation_pin]


def nested_job_platform():
    # Windows10+ supports the JOB_LIST creation attribute and nested jobs.
    # No BREAKAWAY/UI-restriction fallback. Actual three-level runtime unproved.
    need(sys.platform == 'win32' and sys.getwindowsversion().major >= 10, 'nested_job_windows10_or_later')


def launch_local(ctx):
    """Only fixed owner child, never a generic command launcher or GUI automation."""
    assert_desktop()
    nested_job_platform()
    wall_started, monotonic_started = now(), time.monotonic()
    payload,a,_ = ctx.message(check_clock=True)
    anchor_limits(ctx, a, wall_started, monotonic_started)
    # A parent file reader must not prevent the child retiring its input.
    payload.close()
    ctx.record('owner-process-intent.json',dict(**base_request(ctx,a,'owner-process-intent',0),
        source_file=str(Path(__file__).resolve()),max_application_launches=3,
        original_execution_end_utc=a['execution_end_utc'],original_cleanup_end_utc=a['cleanup_end_utc']))
    command=[str(PYTHON),'-B',str(Path(__file__).resolve()),'--plan',str(ctx.plan_path),
             '--plan-sha256',ctx.plan_sha,'--run']
    job_receipt,errors = {},[]
    policy_exit,process_exit,pins,candidate,validation = None,None,[],None,None
    try:
        remaining=int(min(ctx.return_cleanup_end_monotonic-time.monotonic(),
                          (utc(a['cleanup_end_utc'])-now()).total_seconds()))
        need(0<remaining<=2100,'original_local_owner_cleanup_horizon')
        with (ctx.run/'owner-console.log').open('xb') as output:
            policy_exit=ctx.job.run(command,output,job_receipt,timeout=remaining)
        process_exit=job_receipt.get('processExit')
        if job_receipt.get('treeEmpty') is True:
            candidate,validation,pins=verify_owner_candidates(ctx,a)
    except BaseException as error:
        errors.append(dict(error_type=type(error).__name__,location=ctx.native.error_location(error)))
    own_tree_safe=(job_receipt.get('treeEmpty') is True and job_receipt.get('jobActiveProcesses') == 0
                   and job_receipt.get('heldProcessHandlesStopped') is True)
    readers_known=(own_tree_safe and candidate is not None and candidate.get('readers_known') is True
                   and all(safe_readers(phase['process']) for phase in candidate.get('phases',[])))
    native_cleanup=(readers_known and candidate.get('native_cleanup_complete') is True)
    within=cleanup_in_time(ctx)
    execution_accepted=(policy_exit == 0 and process_exit == 0 and not job_receipt.get('timedOut')
        and own_tree_safe and candidate is not None and validation is not None
        and candidate.get('execution_complete') is True and candidate.get('candidate_exit') == 0
        and validation.get('policy_exit_before_validation_publication') == 0
        and validation.get('terminal_publication_within_original_cleanup_end') is True
        and readers_known and native_cleanup and within and not errors)
    receipt=dict(**base_request(ctx,a,'owner-process',0),
        owner_source_sha256=ctx.plan['modules']['windows_powerbi_owner.py'],
        actual_child_policy_exit=policy_exit,actual_child_process_exit=process_exit,owner_job=job_receipt,
        child_receipts=pins,readers_known=readers_known,native_cleanup_complete=native_cleanup,
        execution_accepted_before_owner_receipt_publication=execution_accepted,
        original_execution_end_utc=a['execution_end_utc'],original_cleanup_end_utc=a['cleanup_end_utc'],
        observed_child_terminal_utc=now().isoformat(),within_original_cleanup_end_before_publication=within,
        feature_PASS_claimed=False,errors=errors,
        receipt_status='ACTUAL_CHILD_EXIT_PROOF_PENDING_WRAPPER_PUBLICATION_CHECK')
    ctx.record('owner-process.json',receipt)
    # Neither a late publication nor owner exit0/timeoutrace can turn green.
    return 0 if execution_accepted and cleanup_in_time(ctx) else 72


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan')
    parser.add_argument('--plan-sha256')
    modes = parser.add_mutually_exclusive_group()
    for name in ('stage','run','retire-staged','publish-ack','launch-local'):
        modes.add_argument('--'+name, action='store_true')
    args = parser.parse_args(argv)
    if not any((args.stage,args.run,args.retire_staged,args.publish_ack,args.launch_local)):
        print('OFFLINE_ONLY: no app/provider/registry/SQL/cloud effects')
        return 0
    need(args.plan and args.plan_sha256, 'held_reviewed_plan_required')
    ctx = Context(args.plan,args.plan_sha256)
    mutex, mutex_owned, code = None, False, 72
    try:
        if args.publish_ack:
            return publish_ack(ctx,sys.stdin.buffer.read(32769))
        if args.launch_local:
            code=launch_local(ctx)
        else:
            code=run_mutex_mode(ctx,args)
    finally:
        ctx.close()
    if (args.run or args.launch_local) and not cleanup_in_time(ctx):
        code=72
    return code


def run_mutex_mode(ctx,args):
    mutex,mutex_owned=None,False
    try:
        # Fixed desktop/DSN owner mutex; data-only ACK publisher must not wait it.
        create = ctx.native.api(ctx.native.K,'CreateMutexW',[C.c_void_p,W.BOOL,W.LPCWSTR],W.HANDLE)
        wait = ctx.native.api(ctx.native.K,'WaitForSingleObject',[W.HANDLE,W.DWORD],W.DWORD)
        release = ctx.native.api(ctx.native.K,'ReleaseMutex',[W.HANDLE])
        mutex = create(None, False, 'Local\\ODBCPP-PowerBI-Qualification007')
        need(mutex, 'native_mutex_create')
        status = wait(mutex,0)
        # Abandoned prior owner is reconciliation debt, never authority to start.
        mutex_owned = status in (0, 128)
        need(status == 0, 'sole_native_app_owner_no_abandoned_predecessor')
        if args.stage:
            return stage(ctx,sys.stdin.buffer.read(65537))
        if args.retire_staged:
            return retire_staged(ctx)
        return execute(ctx)
    finally:
        if mutex:
            if mutex_owned:
                release(mutex)
            ctx.native.close_handle(mutex)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:
        # No exception messages, locals or private request values in diagnostics.
        frames, trace = [], error.__traceback__
        while trace is not None and len(frames)<32:
            frames.append(dict(file=trace.tb_frame.f_code.co_filename,line=trace.tb_lineno,function=trace.tb_frame.f_code.co_name))
            trace=trace.tb_next
        print(json.dumps(dict(state='NATIVE_APP_REFUSED',error_type=type(error).__name__,location=frames)),flush=True)
        raise SystemExit(72)
