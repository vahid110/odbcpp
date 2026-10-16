#!/usr/bin/env python3
"""Finite cold Windows owning qualification; orchestration, never product assertions.

MSVC tracking logs bind actual compiler argv and read/link dependencies. Windows
peak working set is measured for the direct child only, not the concurrent tree.
No cache implementation or service/native/registry/package execution is present.
"""
import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import platform
import re
import shutil
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET

TARGETS = ('test_backend_contract', 'test_diagnostics_apis',
           'test_numeric_scale_conversion', 'test_redshift_data_conversion',
           'test_redshift_catalog_execution', 'test_sql_escape',
           'test_pg_protocol_parser')
CHECKS = ('test_architecture_boundaries', 'test_sdk_contract_boundary',
          'test_sdk_contract_boundary_failures', 'test_internal_source_partitions',
          'test_internal_include_closure', 'test_crypto_manifest')
FIRST_PRODUCT = '112890df9c44edd47b8aa3cf1049eb4ab046a29b'
NUMERIC_CASES = (
    'BackendContractTest.NumericInfoNullOutputReturnsRequiredScalarBytes',
    'BackendContractTest.NumericInfoNullOutputLengthPublicationIsUnalignedAndBounded',
    'BackendContractTest.NumericInfoLengthQueriesPreserveOrdinaryScalarPublication',
    'BackendContractTest.NumericInfoNullOutputKeepsValidationAndStringQueryControls',
)
# Closed reviewed contract manifests; this is not a count-only scope expansion.
SDK_HEADERS_23 = (
    'odbcpp/database/backend_capabilities.h', 'odbcpp/database/backend_provider.h',
    'odbcpp/database/backend_result.h', 'odbcpp/database/catalog_queries.h',
    'odbcpp/database/catalog_execution.h', 'odbcpp/database/catalog_request.h',
    'odbcpp/database/error_policy.h', 'odbcpp/database/i_database_connection.h',
    'odbcpp/database/native_type_info.h', 'odbcpp/database/query_parameter.h',
    'odbcpp/database/query_result.h', 'odbcpp/database/session_health.h',
    'odbcpp/database/session_reset.h', 'odbcpp/database/sql_dialect.h',
    'odbcpp/database/sql_translation.h', 'odbcpp/database/statement_description.h',
    'odbcpp/database/statement_kind.h', 'odbcpp/database/transaction.h',
    'odbcpp/database/transaction_session.h', 'odbcpp/database/type_definition.h',
    'odbcpp/transport/i_transport.h', 'odbcpp/util/deadline.h', 'odbcpp/util/result.h',
)
SDK_HEADERS_24 = SDK_HEADERS_23[:11] + ('odbcpp/database/result_validation.h',) + SDK_HEADERS_23[11:]
SDK_MANIFEST_SHA256 = {
    23: '20c54a37f72e79d566b03eba312e6469e92217528065bc3b169b7d276e7f3bb4',
    24: 'ede15fe5cb19bebf56a6cb02eb5080bd1505b83db94f087cba0fc46df85d342f',
}
BUILD = Path('build-windows-product')
EVIDENCE = Path('windows-product-evidence')
PROFILE = ('-A', 'x64', '-DBUILD_EXAMPLES=OFF', '-DBUILD_TESTING=ON',
           '-DTARGET_DATABASE=POSTGRESQL', '-DODBCPP_CRYPTO_PROVIDER=OPENSSL',
           '-DODBCPP_CRYPTO_LINKAGE=BUNDLED_SHARED',
           '-DODBCPP_CRYPTO_PROFILE_LIVE_TESTS=OFF', '-DODBCPP_CRYPTO_TLS_PROOF=OFF',
           '-DODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=2', '-DODBCPP_EXPECT_DM_SQLWCHAR_SIZE=2',
           '-DODBCPP_CRYPTO_ROOT=C:/Program Files/OpenSSL')
WORKFLOW_FILES = ('AGENTS.md', 'PARALLEL_WORKFLOW.md', 'DEVELOPMENT_WORKFLOW.md',
                  '.agents/skills/odbcpp-development-batch/SKILL.md',
                  '.github/workflows/ci.yml', '.github/workflows/windows-product-qualification.yml',
                  'tools/ci/windows_product_qualification.py',
                  'tools/ci/test_windows_product_qualification.py')
PHASES = ('preflight', 'configure', 'build', 'inventory', 'test')
AUTHORITY_SHA256 = {
    'human_request': '2e47224e077958248d5ff93cd415723f334b5c1a106f27d48362cb034cc98354',
    'workflow_lease': 'f1a8c1f89d05b7497c0ade93ea53dbab963eabf13603c1c62279bc38a48cb32e',
    'first_product_lease': 'b9127de0b411df08dfae5f1ca4a0984ba0c04efdd58e328264fec0b6e3914262',
    'owned_child_identity_lease': '80432afe8935bac045c9c43556feb28d5a8561cf0cb27a4e3e60d3d684e42bc8',
    'configure_owned_auxiliary_lease': '8c43877fd9cbd2b26b03b99f615e0ccb1affa28a16be6ef72a64ad124273d1fb',
    'owned_build_auxiliary_lease': '5e6694b42a24757e512f341c8ff6ee77f27a4000093bd8d5b28865b99202f8ca',
}
TEXT_SUFFIXES = {'.json', '.txt', '.xml', '.log'}
MAX_BYTES = 20 * 1024 * 1024
LOG_BYTES = 2 * 1024 * 1024


def utc():
    return datetime.now(timezone.utc).isoformat()


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def write(name, value):
    EVIDENCE.mkdir(exist_ok=True)
    (EVIDENCE / name).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def output(*cmd, cwd=None):
    return subprocess.check_output(cmd, cwd=cwd, text=True, encoding='utf-8',
                                   errors='replace', timeout=60).strip()


def immutable(value):
    if not re.fullmatch('[0-9a-f]{40}', value):
        raise ValueError('immutable full lowercase commit ID required')
    return value


def selection(raw):
    names = raw.split(',')
    if not 1 <= len(names) <= 4 or len(set(names)) != len(names) or any(n not in TARGETS for n in names):
        raise ValueError('one to four distinct literal owning targets required')
    return names


def selected():
    return selection(os.environ['WINDOWS_PRODUCT_TARGETS'])


def selected_regex():
    return '^(' + '|'.join(selected() + list(CHECKS)) + ')$'


def request_guard(env):
    for key in ('WINDOWS_WORKFLOW_HEAD', 'WINDOWS_PRODUCT_HEAD', 'WINDOWS_PRODUCT_BASE'):
        immutable(env[key])
    if env['WINDOWS_WORKFLOW_HEAD'] == env['WINDOWS_PRODUCT_HEAD']:
        raise ValueError('separate immutable workflow W and product P required')
    names = selection(env['WINDOWS_PRODUCT_TARGETS'])
    if env.get('WINDOWS_CONFLICTING_MODE', '').lower() != 'false':
        raise ValueError('Windows mode must be exclusive')
    if json.loads(env.get('WINDOWS_CACHE_REQUEST', '{}')) != {}:
        raise ValueError('initial Windows qualification is cold; cache requests rejected')
    if env['WINDOWS_PRODUCT_HEAD'] == FIRST_PRODUCT and 'test_backend_contract' not in names:
        raise ValueError('first numeric product requires its complete backend owning suite')
    if env.get('GITHUB_REPOSITORY') != 'vahid110/odbcpp' or env.get('GITHUB_REPOSITORY_VISIBILITY') != 'public':
        raise ValueError('only standard free public vahid110/odbcpp runner authorized')
    if env.get('RUNNER_ENVIRONMENT') != 'github-hosted' or env.get('RUNNER_OS') != 'Windows' or env.get('RUNNER_ARCH') != 'X64':
        raise ValueError('requires standard hosted Windows x64 runner')
    submitted = datetime.fromisoformat(env['WINDOWS_SUBMITTED_AT'].replace('Z', '+00:00'))
    if submitted.utcoffset() is None or submitted.utcoffset().total_seconds() != 0 or submitted > datetime.now(timezone.utc):
        raise ValueError('submission must be a preceding UTC timestamp')
    return submitted


def source(expected, root=None):
    root = Path.cwd() if root is None else Path(root)
    git = ['git', '-C', str(root)]
    if output(*git, 'rev-parse', 'HEAD') != immutable(expected):
        raise ValueError('actual source HEAD differs from immutable reviewed source')
    if output(*git, 'status', '--porcelain', '--untracked-files=no'):
        raise ValueError('tracked source mutated')
    # Only our owned outputs may be untracked; no injected compiler source inputs.
    untracked = output(*git, 'ls-files', '--others').splitlines()
    if any(not n.startswith((str(BUILD) + '/', str(EVIDENCE) + '/')) for n in untracked):
        raise ValueError('unexpected untracked source input')
    files = output(*git, 'ls-files').splitlines()
    return {'head': expected, 'tree': output(*git, 'rev-parse', 'HEAD^{tree}'),
            'tracked_sha256': digest({n: sha(root / n) for n in files})}


def sources():
    return {'product': source(os.environ['WINDOWS_PRODUCT_HEAD']),
            'workflow': source(os.environ['WINDOWS_WORKFLOW_HEAD'], os.environ['WINDOWS_WORKFLOW_ROOT'])}


def source_guard():
    before = json.loads((EVIDENCE / 'identity.json').read_text())['sources']
    after = sources()
    write('sources-after.json', after)
    if before != after:
        raise ValueError('W/P source identities changed during qualification')


def direct_peak(handle):
    if os.name != 'nt':
        return None
    class Counters(ctypes.Structure):
        _fields_ = [('cb', ctypes.c_ulong), ('PageFaultCount', ctypes.c_ulong)] + [
            (n, ctypes.c_size_t) for n in ('PeakWorkingSetSize', 'WorkingSetSize',
                'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
                'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage')]
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    api = ctypes.WinDLL('psapi', use_last_error=True).GetProcessMemoryInfo
    api.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
    api.restype = ctypes.c_int
    return counters.PeakWorkingSetSize if api(int(handle), ctypes.byref(counters), counters.cb) else None


class WindowsSuspendedProcess:
    """Create a redirected command suspended; assignment precedes ResumeThread.

    https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags
    No command code runs until resume(), so its children inherit assigned jobs.
    """
    def __init__(self, command):
        import msvcrt
        self.command = command
        self._handle = self._thread = None
        self.stdout = None
        self.launch_order = []
        self.api = ctypes.WinDLL('kernel32', use_last_error=True)
        class Startup(ctypes.Structure):
            _fields_ = [('cb', ctypes.c_ulong)] + [(n, ctypes.c_wchar_p) for n in
                ('lpReserved', 'lpDesktop', 'lpTitle')] + [(n, ctypes.c_ulong) for n in
                ('dwX', 'dwY', 'dwXSize', 'dwYSize', 'dwXCountChars', 'dwYCountChars',
                 'dwFillAttribute', 'dwFlags')] + [('wShowWindow', ctypes.c_ushort),
                ('cbReserved2', ctypes.c_ushort), ('lpReserved2', ctypes.c_void_p)] + [
                (n, ctypes.c_void_p) for n in ('hStdInput', 'hStdOutput', 'hStdError')]
        class ProcessInfo(ctypes.Structure):
            _fields_ = [('hProcess', ctypes.c_void_p), ('hThread', ctypes.c_void_p),
                        ('dwProcessId', ctypes.c_ulong), ('dwThreadId', ctypes.c_ulong)]
        self.api.CreateProcessW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p,
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_ulong,
            ctypes.c_void_p, ctypes.c_wchar_p, ctypes.POINTER(Startup), ctypes.POINTER(ProcessInfo)]
        self.api.CreateProcessW.restype = ctypes.c_int
        self.api.ResumeThread.argtypes = [ctypes.c_void_p]
        self.api.ResumeThread.restype = ctypes.c_ulong
        self.api.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.api.WaitForSingleObject.restype = ctypes.c_ulong
        self.api.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        self.api.GetExitCodeProcess.restype = ctypes.c_int
        self.api.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        self.api.TerminateProcess.restype = ctypes.c_int
        self.api.CloseHandle.argtypes = [ctypes.c_void_p]
        self.api.CloseHandle.restype = ctypes.c_int
        executable = shutil.which(command[0])
        if not executable:
            raise FileNotFoundError(command[0])
        read_fd, write_fd = os.pipe()
        null_fd = None
        try:
            null_fd = os.open(os.devnull, os.O_RDONLY)
            stdout_handle = msvcrt.get_osfhandle(write_fd)
            stdin_handle = msvcrt.get_osfhandle(null_fd)
            os.set_handle_inheritable(stdout_handle, True)
            os.set_handle_inheritable(stdin_handle, True)
            startup, info = Startup(), ProcessInfo()
            startup.cb = ctypes.sizeof(startup)
            startup.dwFlags = 0x100  # STARTF_USESTDHANDLES
            startup.hStdInput = stdin_handle
            startup.hStdOutput = startup.hStdError = stdout_handle
            argv = ctypes.create_unicode_buffer(subprocess.list2cmdline(command))
            if not self.api.CreateProcessW(executable, argv, None, None, True,
                    0x4, None, None, ctypes.byref(startup), ctypes.byref(info)):  # CREATE_SUSPENDED
                raise ctypes.WinError(ctypes.get_last_error())
            self._handle, self._thread, self.pid = info.hProcess, info.hThread, info.dwProcessId
            self.launch_order.append('created_suspended')
            self.stdout = os.fdopen(read_fd, 'rb')
            read_fd = None
        except BaseException:
            if self._handle:
                self.kill()
                self.wait(timeout=30)
                self.close()
            raise
        finally:
            if read_fd is not None:
                os.close(read_fd)
            os.close(write_fd)
            if null_fd is not None:
                os.close(null_fd)

    def resume(self):
        count = self.api.ResumeThread(self._thread)
        if count == 0xffffffff:
            raise ctypes.WinError(ctypes.get_last_error())
        if count != 1:
            raise RuntimeError('expected exactly one primary-thread suspension')
        self.launch_order.append('resumed')
        self.api.CloseHandle(self._thread)
        self._thread = None

    def poll(self):
        result = self.api.WaitForSingleObject(self._handle, 0)
        if result == 258:  # WAIT_TIMEOUT
            return None
        if result != 0:
            raise ctypes.WinError(ctypes.get_last_error())
        code = ctypes.c_ulong()
        if not self.api.GetExitCodeProcess(self._handle, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        return code.value

    def wait(self, timeout):
        result = self.api.WaitForSingleObject(self._handle, max(1, int(timeout * 1000)))
        if result == 258:
            raise subprocess.TimeoutExpired(self.command, timeout)
        if result != 0:
            raise ctypes.WinError(ctypes.get_last_error())
        return self.poll()

    def kill(self):
        if self.poll() is None and not self.api.TerminateProcess(self._handle, 124):
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        for name in ('_thread', '_handle'):
            handle = getattr(self, name)
            if handle:
                self.api.CloseHandle(handle)
                setattr(self, name, None)


def assign_and_resume(job, process):
    job.attach(process)
    process.launch_order.append('assigned_to_job')
    process.resume()


def relative_key(path, root):
    return path.relative_to(root).as_posix()


def configure_command():
    return ['cmake', '-S', '.', '-B', str(BUILD), *PROFILE]


def configure_auxiliary_context(installation, build):
    """Read fresh generated tool/profile proof; no environment or basename opt-in."""
    if installation.get('isComplete') is not True or installation.get('isLaunchable') is not True:
        raise ValueError('selected Visual Studio installation is incomplete')
    root = Path(installation['installationPath']).resolve(strict=True)
    if root != Path(installation['resolvedInstallationPath']).resolve(strict=True):
        raise ValueError('Visual Studio resolved installation differs')
    build = Path(build)
    cache_file = build / 'CMakeCache.txt'
    cache = {}
    for line in cache_file.read_text(encoding='utf-8-sig').splitlines():
        match = re.fullmatch(r'([^/#][^:=]*):[^=]+=(.*)', line)
        if match:
            key, value = match.groups()
            if key in cache:
                raise ValueError('duplicate cache key')
            cache[key] = value
    expected = dict(flag[2:].split('=', 1) for flag in PROFILE if flag.startswith('-D'))
    expected['CMAKE_GENERATOR_PLATFORM'] = 'x64'
    if any(cache.get(key) != value for key, value in expected.items()):
        raise ValueError('fresh configured profile differs')
    if not re.fullmatch(r'Visual Studio \d+ \d{4}', cache.get('CMAKE_GENERATOR', '')) or \
            'Release' not in cache.get('CMAKE_CONFIGURATION_TYPES', '').split(';'):
        raise ValueError('configured Visual Studio multiconfig Release proof missing')
    configs = list((build / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
    if len(configs) != 1:
        raise ValueError('one generated compiler configuration required')
    config = configs[0].read_text(encoding='utf-8-sig')
    ids = re.findall(r'set\(CMAKE_CXX_COMPILER_ID "([^"]+)"\)', config)
    paths = re.findall(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)', config)
    if ids != ['MSVC'] or len(paths) != 1:
        raise ValueError('generated compiler is not one MSVC tool')
    compiler = Path(paths[0]).resolve(strict=True)
    relative = compiler.relative_to(root).parts
    if len(relative) != 8 or tuple(p.casefold() for p in relative[:3]) != ('vc', 'tools', 'msvc') or \
            not re.fullmatch(r'\d+\.\d+\.\d+', relative[3]) or \
            tuple(p.casefold() for p in relative[4:]) != ('bin', 'hostx64', 'x64', 'cl.exe'):
        raise ValueError('compiler differs from selected installation x64 MSVC tool root')
    auxiliary = (compiler.parent / 'vctip.exe').resolve(strict=True)
    if auxiliary.parent != compiler.parent or auxiliary.name.casefold() != 'vctip.exe' or \
            not compiler.is_file() or not auxiliary.is_file():
        raise ValueError('exact configured compiler sibling auxiliary missing')
    return {'installation_root': root.as_posix(), 'compiler_path': compiler.as_posix(),
            'compiler_sha256': sha(compiler), 'auxiliary_path': auxiliary.as_posix(),
            'auxiliary_sha256': sha(auxiliary), 'cache_sha256': sha(cache_file),
            'compiler_configuration_sha256': sha(configs[0]), 'profile': list(PROFILE)}


def _compiler_auxiliary_disposition(record, context, phase, command, fresh_graph, policy):
    """Shared exact held-identity/reap predicate for two private owning phases."""
    refused = {'accepted': False, 'disposition': 'unexpected_owned_child_refused'}
    try:
        if not context or record['phase'] != phase or record['argv'] != command or \
                type(record['exit']) is not int or record['exit'] != 0 or \
                type(record['direct_pid']) is not int or record['direct_pid'] <= 0 or record['timed_out'] is not False or \
                record['direct_terminal'] is not True or record['pipe_drained'] is not True or \
                record['fresh_graph_before_launch'] is not fresh_graph or record['unexpected_children'] is not True or \
                record['launch_order'] != ['created_suspended', 'assigned_to_job', 'resumed']:
            return refused
        if context['profile'] != list(PROFILE) or any(not re.fullmatch('[0-9a-f]{64}', context[key]) for key in
                ('compiler_sha256', 'auxiliary_sha256', 'cache_sha256', 'compiler_configuration_sha256')):
            return refused
        compiler, auxiliary = PureWindowsPath(context['compiler_path']), PureWindowsPath(context['auxiliary_path'])
        root = PureWindowsPath(context['installation_root'])
        if not compiler.is_absolute() or not auxiliary.is_absolute() or not root.is_absolute():
            return refused
        relative = compiler.relative_to(root).parts
        if len(relative) != 8 or tuple(p.casefold() for p in relative[:3]) != ('vc', 'tools', 'msvc') or \
                not re.fullmatch(r'\d+\.\d+\.\d+', relative[3]) or \
                tuple(p.casefold() for p in relative[4:]) != ('bin', 'hostx64', 'x64', 'cl.exe') or \
                auxiliary != compiler.parent / 'vctip.exe':
            return refused
        def filetime(value):
            stamp = datetime.fromisoformat(value)
            if stamp.utcoffset() is None or stamp.utcoffset().total_seconds() != 0:
                raise ValueError('command timestamps must be UTC')
            delta = stamp - datetime(1601, 1, 1, tzinfo=timezone.utc)
            return (delta.days * 86400 + delta.seconds) * 10000000 + delta.microseconds * 10
        start, exited = filetime(record['started_at']), filetime(record['direct_exited_at'])
        if exited < start:
            return refused
        cleanup = record['cleanup']
        if cleanup['tree_owned_by_job'] is not True or cleanup['kill_on_job_close'] is not True or \
                cleanup['excluded_direct_pid'] != record['direct_pid'] or cleanup['identity_errors'] or \
                cleanup['termination_requested'] is not True or cleanup['termination_succeeded'] is not True or \
                type(cleanup['active_after_cleanup']) is not int or cleanup['active_after_cleanup'] != 0 or \
                type(cleanup['active_after_grace']) is not int or type(cleanup['cleanup_seconds']) is not int or \
                cleanup['cleanup_seconds'] != 5 or type(cleanup['cleanup_elapsed_seconds']) not in (int, float) or \
                not 0 <= cleanup['cleanup_elapsed_seconds'] <= 5:
            return refused
        children = cleanup['owned_descendants']
        snapshots = [s for s in cleanup['identity_stages'] if s['stage'] == 'after_grace']
        if not children or not snapshots:
            return refused
        pids = [child['pid'] for child in children]
        members = snapshots[-1]['member_pids']
        if len(pids) != len(set(pids)) or cleanup['active_after_grace'] != len(pids) or \
                not isinstance(members, list) or sorted(members) != sorted(pids):
            return refused
        for child in children:
            if type(child['pid']) is not int or child['pid'] <= 0 or child['pid'] == record['direct_pid'] or \
                    PureWindowsPath(child['image_path']) != auxiliary or child['image_sha256'] != context['auxiliary_sha256'] or \
                    type(child['creation_filetime']) is not int or not start <= child['creation_filetime'] <= exited or \
                    child['creation_unit'] != '100ns since1601 UTC' or any(child[key] is not True for key in
                        ('job_member', 'held_handle', 'terminal_after_cleanup', 'held_handle_closed')):
                return refused
        return {'accepted': True, 'disposition': 'expected_owned_compiler_auxiliary_cleaned',
                'member_pids': pids, 'policy': policy}
    except (KeyError, TypeError, ValueError, OverflowError):
        return refused


def configure_auxiliary_disposition(record, context):
    """Preserve the original configure-only policy and its raw child fact."""
    return _compiler_auxiliary_disposition(record, context, 'configure', configure_command(), True,
        'configure-only exact compiler sibling; verified owned termination and reap')


def build_command():
    return ['cmake', '--build', str(BUILD), '--config', 'Release', '--parallel', '2', '--target',
            *selected(), 'odbcpp_sdk_header_isolation', 'odbcpp_driver', '--',
            '/verbosity:normal', '/nodeReuse:false', '/p:TrackFileAccess=true']


def build_auxiliary_context():
    """Revalidate private cold-configure provenance and live tools, before/after build."""
    request_guard(os.environ)
    paths = {name: EVIDENCE / name for name in
             ('identity.json', 'configure-command.json', 'configure.json', 'dependencies.json')}
    values = {name: json.loads(path.read_text()) for name, path in paths.items()}
    identity, configured = values['identity.json'], values['configure-command.json']
    runner = {key: os.environ[key] for key in ('GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_SHA')}
    current_sources = sources()
    if identity['sources'] != current_sources or identity['base'] != os.environ['WINDOWS_PRODUCT_BASE'] or \
            identity['profile'] != list(PROFILE) or identity['targets'] != selected() or \
            identity['cache'] != {'mode': 'cold', 'restored': False, 'saved': False} or \
            any(identity['runner'].get(key) != value for key, value in runner.items()) or \
            runner['GITHUB_SHA'] != os.environ['WINDOWS_WORKFLOW_HEAD'] or \
            Path(identity['product_path']).resolve(strict=True) != Path.cwd() or \
            Path(identity['workflow_path']).resolve(strict=True) != Path(os.environ['WINDOWS_WORKFLOW_ROOT']).resolve(strict=True):
        raise ValueError('build differs from admitted cold W/P/profile/targets/run')
    for name, key in [('workflow', 'WINDOWS_WORKFLOW_HEAD'), ('product', 'WINDOWS_PRODUCT_HEAD')]:
        source_identity = current_sources[name]
        if source_identity['head'] != immutable(os.environ[key]) or \
                not re.fullmatch('[0-9a-f]{40}', source_identity['tree']) or \
                not re.fullmatch('[0-9a-f]{64}', source_identity['tracked_sha256']):
            raise ValueError('build source identity missing or changed')
    if values['configure.json']['status'] != 'passed' or configured['phase'] != 'configure' or \
            configured['argv'] != configure_command() or configured['fresh_graph_before_launch'] is not True or \
            type(configured['exit']) is not int or configured['exit'] != 0 or configured['timed_out'] is not False or \
            configured['direct_terminal'] is not True or configured['pipe_drained'] is not True or \
            configured['launch_order'] != ['created_suspended', 'assigned_to_job', 'resumed'] or \
            configured['configure_auxiliary_context_error'] is not None:
        raise ValueError('fresh successful configure proof required before owning build')
    finished = datetime.fromisoformat(values['configure.json']['finished_at'])
    exited = datetime.fromisoformat(configured['direct_exited_at'])
    if any(stamp.utcoffset() is None or stamp.utcoffset().total_seconds() != 0 for stamp in (finished, exited)) or finished < exited:
        raise ValueError('configure completion timestamp differs')
    installations = values['dependencies.json']['visual_studio']
    if len(installations) != 1:
        raise ValueError('same single selected Visual Studio installation required')
    tool = configure_auxiliary_context(installations[0], BUILD)
    if tool != configured['configure_auxiliary_context']:
        raise ValueError('configured cache/compiler/auxiliary identity changed')
    cleanup = configured['cleanup']
    if configured['unexpected_children'] is True:
        if not configure_auxiliary_disposition(configured, tool)['accepted']:
            raise ValueError('configure auxiliary cleanup proof invalid')
    elif configured['unexpected_children'] is not False or \
            type(cleanup['active_after_cleanup']) is not int or cleanup['active_after_cleanup'] != 0 or \
            cleanup['identity_errors'] or cleanup['tree_owned_by_job'] is not True or \
            cleanup['kill_on_job_close'] is not True or any(
                child.get('terminal_after_cleanup') is not True or child.get('held_handle_closed') is not True
                for child in cleanup['owned_descendants']):
        raise ValueError('configure owned terminal/close/zero proof missing')
    return {'tool': tool, 'sources': current_sources, 'base': identity['base'], 'runner': runner,
            'argv': build_command(), 'build_path': str(BUILD.resolve(strict=True)),
            'configure_finished_at': values['configure.json']['finished_at'],
            'identity_sha256': sha(paths['identity.json']),
            'configure_record_sha256': sha(paths['configure-command.json']),
            'configure_phase_sha256': sha(paths['configure.json']),
            'dependencies_sha256': sha(paths['dependencies.json'])}


def build_auxiliary_disposition(record, context):
    """Explicit build-only policy; no arbitrary caller, basename or environment exemption."""
    refused = {'accepted': False, 'disposition': 'unexpected_owned_child_refused'}
    try:
        if not context or record['phase'] != 'build' or record['argv'] != build_command() or \
                context['argv'] != record['argv'] or context['build_path'] != str(BUILD.resolve()) or \
                context != record['build_auxiliary_context'] or context != record['build_auxiliary_context_after'] or \
                record['build_auxiliary_context_error'] is not None or \
                context['base'] != immutable(os.environ['WINDOWS_PRODUCT_BASE']):
            return refused
        if any(not re.fullmatch('[0-9a-f]{64}', context[key]) for key in
               ('identity_sha256', 'configure_record_sha256', 'configure_phase_sha256', 'dependencies_sha256')):
            return refused
        for name, key in [('workflow', 'WINDOWS_WORKFLOW_HEAD'), ('product', 'WINDOWS_PRODUCT_HEAD')]:
            value = context['sources'][name]
            if value['head'] != immutable(os.environ[key]) or not re.fullmatch('[0-9a-f]{40}', value['tree']) or \
                    not re.fullmatch('[0-9a-f]{64}', value['tracked_sha256']):
                return refused
        if context['runner'] != {key: os.environ[key] for key in ('GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_SHA')} or \
                context['runner']['GITHUB_SHA'] != os.environ['WINDOWS_WORKFLOW_HEAD']:
            return refused
        finished, started = (datetime.fromisoformat(value) for value in
                             (context['configure_finished_at'], record['started_at']))
        if any(stamp.utcoffset() is None or stamp.utcoffset().total_seconds() != 0 for stamp in (finished, started)) or finished > started:
            return refused
        return _compiler_auxiliary_disposition(record, context['tool'], 'build', build_command(), False,
            'build-only exact compiler sibling after same fresh configure; verified owned termination and reap')
    except (KeyError, TypeError, ValueError, OverflowError):
        return refused


class HeldOwnedProcess:
    """A job-verified process handle retained through cleanup, never a PID target."""
    def __init__(self, api, handle, evidence):
        self.api, self.handle, self.evidence = api, handle, evidence

    def terminal(self):
        result = self.api.WaitForSingleObject(self.handle, 0)
        if result not in (0, 258):
            raise ctypes.WinError(ctypes.get_last_error())
        return result == 0

    def close(self):
        if self.handle:
            handle, self.handle = self.handle, None
            if not self.api.CloseHandle(handle):
                raise ctypes.WinError(ctypes.get_last_error())


def owned_exit_cleanup(job, direct_pid, grace_seconds=5, cleanup_seconds=5):
    """Capture bounded owned identities; retain original remaining-child refusal."""
    held, errors = [], []
    stages = []
    def capture(stage):
        try:
            found, issues = job.capture_owned(direct_pid, known_pids=tuple(h.evidence['pid'] for h in held))
            for item in found:
                item.evidence['observed_stage'] = stage
            held.extend(found)
            errors.extend(dict(item, stage=stage) for item in issues)
            stages.append({'stage': stage, 'at': utc(), 'new_held_identities': len(found),
                           'member_pids': getattr(job, 'last_capture_pids', None)})
        except Exception as exc:
            errors.append({'stage': stage, 'error': str(exc)})
    def active(stage):
        try:
            count = job.active()
            if not isinstance(count, int) or count < 0:
                raise ValueError('invalid owned active-process count')
            return count
        except Exception as exc:
            errors.append({'stage': stage, 'error': str(exc)})
            return None
    initial = active('direct_exit')
    capture('direct_exit')
    grace_start = time.monotonic()
    cleanup_start = None
    records = []
    termination_requested = termination_succeeded = False
    try:
        deadline = grace_start + grace_seconds
        remaining = active('grace')
        while remaining and time.monotonic() < deadline:
            time.sleep(0.05)
            remaining = active('grace')
        remaining = active('after_grace')
        unexpected_children = remaining != 0  # Preserved original guard, including unknown counts.
        grace_elapsed = time.monotonic() - grace_start
        if unexpected_children:
            capture('after_grace')
        cleanup_start = time.monotonic()
        deadline = cleanup_start + cleanup_seconds  # Shared by the job count and every held handle.
        if unexpected_children:
            termination_requested = True
            try:
                job.terminate()  # Job only; never kill a recorded PID or a nonmember.
                termination_succeeded = True
            except Exception as exc:
                errors.append({'stage': 'terminate_owned_job', 'error': str(exc)})
        records = [dict(item.evidence, terminal_after_cleanup=False) for item in held]
        while True:
            count = active('cleanup')
            for item, record in zip(held, records):
                if record['terminal_after_cleanup'] is False:
                    try:
                        record['terminal_after_cleanup'] = item.terminal()
                    except Exception as exc:
                        record['terminal_after_cleanup'] = None
                        errors.append({'stage': 'held_process_reap', 'pid': record['pid'], 'error': str(exc)})
            pending = count != 0 or any(r['terminal_after_cleanup'] is False for r in records)
            left = deadline - time.monotonic()
            if not pending or left <= 0:
                break
            time.sleep(min(0.05, left))
        final = active('after_cleanup')
    finally:
        for index, item in enumerate(held):
            try:
                item.close()
                if index < len(records):
                    records[index]['held_handle_closed'] = True
            except Exception as exc:
                if index < len(records):
                    records[index]['held_handle_closed'] = False
                errors.append({'stage': 'close_held_handle', 'pid': item.evidence['pid'], 'error': str(exc)})
    cleanup = {'active_at_direct_exit': initial, 'active_after_grace': remaining,
               'active_after_direct_exit': remaining, 'active_after_cleanup': final,
               'tree_owned_by_job': True, 'kill_on_job_close': True,
               'excluded_direct_pid': direct_pid, 'owned_descendants': records,
               'identity_errors': errors, 'identity_stages': stages,
               'grace_seconds': grace_seconds, 'grace_elapsed_seconds': grace_elapsed,
               'cleanup_seconds': cleanup_seconds,
               'termination_requested': termination_requested, 'termination_succeeded': termination_succeeded,
               'cleanup_elapsed_seconds': time.monotonic() - cleanup_start if cleanup_start is not None else 0,
               'identity_limit': 'only current owned job members, at most 64 per snapshot; handles held through reap'}
    invalid_cleanup = final != 0 or bool(errors) or any(r.get('terminal_after_cleanup') is not True or r.get('held_handle_closed') is not True for r in records)
    return cleanup, unexpected_children, invalid_cleanup


class WindowsProcessJob:
    """Own one command tree; kill-on-close prevents escaped child continuations."""
    def __init__(self):
        self.api = ctypes.WinDLL('kernel32', use_last_error=True)
        self.api.CreateJobObjectW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
        self.api.CreateJobObjectW.restype = ctypes.c_void_p
        self.api.CloseHandle.argtypes = [ctypes.c_void_p]
        self.api.CloseHandle.restype = ctypes.c_int
        self.api.SetInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_ulong]
        self.api.SetInformationJobObject.restype = ctypes.c_int
        self.api.AssignProcessToJobObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.api.AssignProcessToJobObject.restype = ctypes.c_int
        self.api.QueryInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p]
        self.api.QueryInformationJobObject.restype = ctypes.c_int
        self.api.TerminateJobObject.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        self.api.TerminateJobObject.restype = ctypes.c_int
        class BasicLimits(ctypes.Structure):
            _fields_ = [('PerProcessUserTimeLimit', ctypes.c_longlong), ('PerJobUserTimeLimit', ctypes.c_longlong),
                        ('LimitFlags', ctypes.c_ulong), ('MinimumWorkingSetSize', ctypes.c_size_t),
                        ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', ctypes.c_ulong),
                        ('Affinity', ctypes.c_size_t), ('PriorityClass', ctypes.c_ulong), ('SchedulingClass', ctypes.c_ulong)]
        class IoCounters(ctypes.Structure):
            _fields_ = [(n, ctypes.c_ulonglong) for n in ('ReadOperationCount', 'WriteOperationCount',
                        'OtherOperationCount', 'ReadTransferCount', 'WriteTransferCount', 'OtherTransferCount')]
        class ExtendedLimits(ctypes.Structure):
            _fields_ = [('BasicLimitInformation', BasicLimits), ('IoInfo', IoCounters)] + [
                (n, ctypes.c_size_t) for n in ('ProcessMemoryLimit', 'JobMemoryLimit', 'PeakProcessMemoryUsed', 'PeakJobMemoryUsed')]
        self.handle = self.api.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = ExtendedLimits()
        limits.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.api.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            error = ctypes.get_last_error()
            self.close()
            raise ctypes.WinError(error)

    def attach(self, process):
        if not self.api.AssignProcessToJobObject(self.handle, int(process._handle)):
            raise ctypes.WinError(ctypes.get_last_error())

    def active(self):
        class Accounting(ctypes.Structure):
            _fields_ = [(n, ctypes.c_longlong) for n in ('TotalUserTime', 'TotalKernelTime',
                        'ThisPeriodTotalUserTime', 'ThisPeriodTotalKernelTime')] + [
                (n, ctypes.c_ulong) for n in ('TotalPageFaultCount', 'TotalProcesses', 'ActiveProcesses', 'TotalTerminatedProcesses')]
        accounting = Accounting()
        if not self.api.QueryInformationJobObject(self.handle, 1, ctypes.byref(accounting), ctypes.sizeof(accounting), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return accounting.ActiveProcesses

    def capture_owned(self, direct_pid, known_pids=()):
        """Only this job's PID list; image/creation/membership through held handles.

        QueryInformationJobObject(JobObjectBasicProcessIdList) is bounded to64.
        No VM process enumeration, PID-directed termination or guessed tool name.
        """
        limit = 64
        self.last_capture_pids = None
        class PidList(ctypes.Structure):
            _fields_ = [('assigned', ctypes.c_ulong), ('listed', ctypes.c_ulong),
                        ('pids', ctypes.c_size_t * limit)]
        info = PidList()
        if not self.api.QueryInformationJobObject(self.handle, 3, ctypes.byref(info), ctypes.sizeof(info), None):
            return [], [{'error': 'owned PID list query failed', 'winerror': ctypes.get_last_error(), 'limit': limit}]
        if info.assigned > limit or info.listed > limit or info.listed != info.assigned:
            return [], [{'error': 'owned PID list incomplete/over bound', 'assigned': info.assigned, 'listed': info.listed, 'limit': limit}]
        self.last_capture_pids = [pid for pid in info.pids[:info.listed] if pid != direct_pid]
        self.api.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        self.api.OpenProcess.restype = ctypes.c_void_p
        self.api.IsProcessInJob.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
        self.api.IsProcessInJob.restype = ctypes.c_int
        self.api.GetProcessId.argtypes = [ctypes.c_void_p]
        self.api.GetProcessId.restype = ctypes.c_ulong
        self.api.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)]
        self.api.QueryFullProcessImageNameW.restype = ctypes.c_int
        self.api.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.api.WaitForSingleObject.restype = ctypes.c_ulong
        class FileTime(ctypes.Structure):
            _fields_ = [('low', ctypes.c_ulong), ('high', ctypes.c_ulong)]
        self.api.GetProcessTimes.argtypes = [ctypes.c_void_p, *([ctypes.POINTER(FileTime)] * 4)]
        self.api.GetProcessTimes.restype = ctypes.c_int
        held, errors = [], []
        for pid in info.pids[:info.listed]:
            if pid == direct_pid or pid in known_pids:
                continue  # The direct/known process handle remains held, preventing PID reuse.
            handle = self.api.OpenProcess(0x100000 | 0x1000, False, pid)  # SYNCHRONIZE / limited query only
            if not handle:
                errors.append({'pid': pid, 'error': 'owned process could not be opened', 'winerror': ctypes.get_last_error()})
                continue
            keep = False
            try:
                belongs = ctypes.c_int()
                if not self.api.IsProcessInJob(handle, self.handle, ctypes.byref(belongs)):
                    raise ctypes.WinError(ctypes.get_last_error())
                if not belongs.value or self.api.GetProcessId(handle) != pid:
                    raise ValueError('process no longer belongs to owned job or PID/handle mismatch')
                image = ctypes.create_unicode_buffer(4096)
                length = ctypes.c_ulong(len(image))
                if not self.api.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(length)):
                    raise ctypes.WinError(ctypes.get_last_error())
                creation, exit_time, kernel, user = FileTime(), FileTime(), FileTime(), FileTime()
                if not self.api.GetProcessTimes(handle, ctypes.byref(creation), ctypes.byref(exit_time), ctypes.byref(kernel), ctypes.byref(user)):
                    raise ctypes.WinError(ctypes.get_last_error())
                stamp = (creation.high << 32) | creation.low
                if not image.value or stamp <= 0:
                    raise ValueError('missing owned image/creation identity')
                held.append(HeldOwnedProcess(self.api, handle, {'pid': pid, 'image_path': image.value, 'image_sha256': sha(image.value),
                            'creation_filetime': stamp, 'creation_unit': '100ns since1601 UTC',
                            'job_member': True, 'held_handle': True, 'captured_at': utc()}))
                keep = True
            except Exception as exc:
                errors.append({'pid': pid, 'error': str(exc)})
            finally:
                if not keep:
                    if not self.api.CloseHandle(handle):
                        errors.append({'pid': pid, 'error': 'failed to close unverified process handle', 'winerror': ctypes.get_last_error()})
        return held, errors

    def terminate(self):
        if not self.api.TerminateJobObject(self.handle, 124):
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        if self.handle:
            self.api.CloseHandle(self.handle)
            self.handle = None


def measure(name, command, timeout, *, configure_installation=None, owning_build=False):
    EVIDENCE.mkdir(exist_ok=True)
    start, tick = utc(), time.monotonic()
    fresh_graph = name == 'configure' and command == configure_command() and \
        configure_installation is not None and not BUILD.exists()
    build_context, build_context_after, build_context_error = None, None, None
    if owning_build:
        try:
            if name != 'build' or command != build_command():
                raise ValueError('private owning build requires exact phase/argv')
            build_context = build_auxiliary_context()
        except Exception as exc:
            write(name + '-command.json', {'argv': command, 'started_at': start, 'finished_at': utc(),
                  'status': 'failed', 'build_auxiliary_context_error': str(exc)})
            raise
    job = WindowsProcessJob() if os.name == 'nt' else None
    try:
        if job:
            process = WindowsSuspendedProcess(command)
            assign_and_resume(job, process)
        else:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except BaseException as exc:
        cleanup = {}
        try:
            if 'process' in locals():
                process.kill()
                process.wait(timeout=30)
                cleanup['direct_pid'] = process.pid
                cleanup['direct_terminal'] = process.poll() is not None
                process.stdout.close()
                if job:
                    cleanup['launch_order'] = list(process.launch_order)
                    process.close()
            if job:
                cleanup['job_active_after_cleanup'] = job.active()
        finally:
            if job:
                job.close()
            write(name + '-command.json', {'argv': command, 'started_at': start, 'finished_at': utc(),
                  'launch_error': str(exc), 'status': 'failed', 'cleanup': cleanup})
        raise
    stored, tail = bytearray(), bytearray()
    total = [0]
    def drain():
        for block in iter(lambda: process.stdout.read(8192), b''):
            total[0] += len(block)
            tail.extend(block)
            if len(tail) > 65536:
                del tail[:-65536]
            if len(stored) < LOG_BYTES:
                stored.extend(block[:LOG_BYTES - len(stored)])
    reader = threading.Thread(target=drain, daemon=True)
    reader.start()
    timed_out = False
    cleanup = None
    try:
        code = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        if job:
            job.terminate()
            cleanup = {'job_tree_terminated_on_timeout': True}
        else:
            process.kill()
        code = process.wait(timeout=30)
    direct_exited_at = utc()
    context, context_error = None, None
    if fresh_graph and code == 0 and not timed_out:
        try:
            context = configure_auxiliary_context(configure_installation, BUILD)
        except Exception as exc:
            context_error = str(exc)
    if owning_build and code == 0 and not timed_out:
        try:
            build_context_after = build_auxiliary_context()
            if build_context_after != build_context:
                raise ValueError('build configure/source/tool provenance changed during command')
        except Exception as exc:
            build_context_error = str(exc)
    peak = direct_peak(process._handle) if os.name == 'nt' else None
    unexpected_children = invalid_cleanup = False
    if job:
        try:
            owned, unexpected_children, invalid_cleanup = owned_exit_cleanup(job, process.pid)
            cleanup = dict(owned, prior=cleanup)
        finally:
            job.close()
    reader.join(timeout=5)
    if reader.is_alive():
        # An inherited pipe still open after parent exit is a failed cleanup signal.
        cleanup = {'pipe_still_open': True, 'prior': cleanup}
    if not reader.is_alive():
        process.stdout.close()
    log = stored.decode('utf-8', errors='replace')
    if total[0] > LOG_BYTES:
        log += '\n[bounded log: middle omitted; final 64 KiB follows]\n' + tail.decode('utf-8', errors='replace')
    (EVIDENCE / (name + '.log')).write_text(log, encoding='utf-8')
    terminal = process.poll() is not None
    launch_order = list(process.launch_order) if job else ['popen']
    if job:
        process.close()
    record = {'argv': command, 'started_at': start, 'finished_at': utc(),
              'phase': name, 'direct_exited_at': direct_exited_at, 'fresh_graph_before_launch': fresh_graph,
              'pipe_drained': not reader.is_alive(), 'unexpected_children': unexpected_children,
              'configure_auxiliary_context': context, 'configure_auxiliary_context_error': context_error,
              'build_auxiliary_context': build_context, 'build_auxiliary_context_after': build_context_after,
              'build_auxiliary_context_error': build_context_error,
              'elapsed_seconds': time.monotonic() - tick, 'exit': code,
              'timed_out': timed_out, 'direct_pid': process.pid,
              'direct_terminal': terminal, 'cleanup': cleanup, 'launch_order': launch_order,
              'direct_process_peak_working_set_bytes': peak,
              'memory_limit': 'direct child peak only, not summed process-tree peak',
              'disk_free_bytes_after': shutil.disk_usage('.').free,
              'log_bytes_emitted': total[0], 'log_truncated': total[0] > LOG_BYTES}
    disposition = (build_auxiliary_disposition(record, build_context) if owning_build else
                   configure_auxiliary_disposition(record, context))
    record['auxiliary_disposition'] = disposition
    write(name + '-command.json', record)
    if code or timed_out or reader.is_alive() or build_context_error or (unexpected_children and not disposition['accepted']) or invalid_cleanup:
        raise RuntimeError(f'{name} command failed; original exit/log/cleanup retained')
    return record


def init():
    submitted = request_guard(os.environ)
    if os.name != 'nt' or platform.machine().upper() not in ('AMD64', 'X86_64'):
        raise ValueError('Windows x64 Python process required')
    if os.environ['GITHUB_SHA'] != os.environ['WINDOWS_WORKFLOW_HEAD']:
        raise ValueError('dispatch ref must equal reviewed immutable W')
    if BUILD.exists() or EVIDENCE.exists():
        raise ValueError('independently fresh graph/evidence required; no cache restore')
    identity = sources()
    workflow = Path(os.environ['WINDOWS_WORKFLOW_ROOT'])
    write('identity.json', {'sources': identity, 'authority_sha256': AUTHORITY_SHA256, 'workflow_file_sha256': {n: sha(workflow / n) for n in WORKFLOW_FILES},
          'product_path': str(Path.cwd()), 'workflow_path': str(workflow.resolve()),
          'base': os.environ['WINDOWS_PRODUCT_BASE'], 'profile': list(PROFILE),
          'targets': selected(), 'checks': list(CHECKS), 'submitted_at': submitted.isoformat(),
          'evidence_started_at': utc(), 'submission_to_evidence_seconds': (datetime.now(timezone.utc) - submitted).total_seconds(),
          'runner': {k: os.environ.get(k) for k in ('RUNNER_OS', 'RUNNER_ARCH', 'RUNNER_ENVIRONMENT',
                    'RUNNER_NAME', 'ImageOS', 'ImageVersion', 'GITHUB_SHA', 'GITHUB_REF',
                    'GITHUB_WORKFLOW_REF', 'GITHUB_WORKFLOW_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT')},
          'cache': {'mode': 'cold', 'restored': False, 'saved': False},
          'disk_free_bytes_before': shutil.disk_usage('.').free,
          'machine_resources': json.loads(output('pwsh', '-NoProfile', '-Command',
              '$c=Get-CimInstance Win32_ComputerSystem; $p=Get-CimInstance Win32_Processor; '
              '[pscustomobject]@{total_physical_memory_bytes=$c.TotalPhysicalMemory; '
              'logical_processors=$c.NumberOfLogicalProcessors; processors=@($p.Name)} | ConvertTo-Json -Compress')),
          'limits': 'Windows owning primary only; full/native/live/registry/MSI/final acceptance pending'})


def preflight():
    base = immutable(os.environ['WINDOWS_PRODUCT_BASE'])
    output('git', 'merge-base', '--is-ancestor', base, 'HEAD')
    files = output('git', 'diff', '--name-only', base, 'HEAD', '--', '*.cpp', '*.cc', '*.cxx', '*.h', '*.hpp').splitlines()
    if files:
        measure('changed-gtest', [sys.executable, '-B', 'tools/development/check_changed_gtest_control_flow.py',
                                 '--base', base, '--head', os.environ['WINDOWS_PRODUCT_HEAD'], *files], 120)
    write('changed-gtest.json', {'base': base, 'head': os.environ['WINDOWS_PRODUCT_HEAD'],
                               'paths': files, 'status': 'passed' if files else 'no changed C++'})


def configure():
    if BUILD.exists():
        raise ValueError('cold build path must be absent')
    vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installations = json.loads(output(str(vswhere), '-latest', '-products', '*', '-requires',
                                      'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-format', 'json'))
    if len(installations) != 1:
        raise ValueError('actual installed x64 MSVC Visual Studio required')
    openssl_root = Path('C:/Program Files/OpenSSL')
    openssl = openssl_root / 'bin/openssl.exe'
    dependencies = {'visual_studio': installations, 'vswhere_sha256': sha(vswhere),
                    'cmake_version': output('cmake', '--version'), 'ctest_version': output('ctest', '--version'),
                    'cmake_path': shutil.which('cmake'), 'cmake_sha256': sha(shutil.which('cmake')),
                    'openssl_version': output(str(openssl), 'version', '-a'),
                    'openssl_files': {str(p.relative_to(openssl_root)): sha(p) for p in sorted(openssl_root.rglob('*'))
                                      if p.is_file() and p.suffix.lower() in ('.h', '.lib', '.dll', '.exe')}}
    write('dependencies.json', dependencies)
    measure('configure', configure_command(), 300, configure_installation=installations[0])
    cache = (BUILD / 'CMakeCache.txt').read_text()
    for needle in ('CMAKE_GENERATOR_PLATFORM:INTERNAL=x64', 'TARGET_DATABASE:STRING=POSTGRESQL',
                   'ODBCPP_CRYPTO_LINKAGE:STRING=BUNDLED_SHARED', 'ODBCPP_CRYPTO_PROFILE_LIVE_TESTS:BOOL=OFF',
                   'ODBCPP_CRYPTO_TLS_PROOF:BOOL=OFF'):
        if needle not in cache:
            raise ValueError('configured profile differs: ' + needle)
    shutil.copyfile(BUILD / 'CMakeCache.txt', EVIDENCE / 'CMakeCache.txt')
    shutil.copyfile(BUILD / 'odbcpp-crypto-manifest.json', EVIDENCE / 'crypto-manifest.json')
    compiler_files = list((BUILD / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
    if len(compiler_files) != 1:
        raise ValueError('actual MSVC compiler configuration missing')
    compiler = compiler_files[0].read_text()
    if 'set(CMAKE_CXX_COMPILER_ID "MSVC")' not in compiler:
        raise ValueError('actual compiler is not MSVC')
    (EVIDENCE / 'CMakeCXXCompiler.txt').write_text(compiler, encoding='utf-8')
    path = re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)', compiler)
    if not path or 'hostx64/x64' not in path[1].replace('\\', '/').lower():
        raise ValueError('actual hostx64/x64 compiler path required')
    write('compiler.json', {'path': path[1], 'sha256': sha(path[1]), 'configuration_sha256': sha(compiler_files[0])})
    fetched = {}
    for name in ('googletest', 'spdlog'):
        root = BUILD / '_deps' / (name + '-src')
        files = {relative_key(p, root): sha(p) for p in sorted(root.rglob('*')) if p.is_file()}
        if not files:
            raise ValueError('missing actual fetched dependency: ' + name)
        fetched[name] = {'files': len(files), 'sha256': digest(files)}
    write('fetched-dependencies.json', fetched)


def build():
    measure('build', build_command(), 1200, owning_build=True)


def gtest_names(text):
    names, suite = [], ''
    for line in text.splitlines():
        bare = line.split('#', 1)[0].rstrip()
        if bare.startswith('  ') and suite:
            names.append(suite + bare.strip())
        elif bare.endswith('.') and not bare.startswith(' '):
            suite = bare
    if not names or len(set(names)) != len(names) or any('DISABLED_' in n for n in names):
        raise ValueError('empty, duplicate or disabled owning GoogleTest inventory')
    return sorted(names)


def xml_result(path, expected):
    root = ET.parse(path).getroot()
    cases = list(root.iter('testcase'))
    names = [c.attrib['classname'] + '.' + c.attrib['name'] for c in cases]
    if sorted(names) != sorted(expected) or len(set(names)) != len(names):
        raise ValueError('GoogleTest inventory/XML mismatch')
    if any(list(root.iter(tag)) for tag in ('failure', 'error', 'skipped')) or any(
            c.get('status') == 'notrun' or c.get('result') in ('skipped', 'suppressed') for c in cases):
        raise ValueError('failed, skipped or disabled owning GoogleTest cases')
    for attribute in ('failures', 'errors', 'disabled', 'skipped'):
        if int(root.get(attribute, 0)):
            raise ValueError('declared unsuccessful GoogleTest count')
    if int(root.get('tests', len(cases))) != len(cases):
        raise ValueError('XML declared count mismatch')
    return {'count': len(cases), 'names': sorted(names), 'xml_sha256': sha(path)}


def sdk_headers(manifest):
    normalized = manifest.replace('\r\n', '\n')
    headers = tuple(normalized.splitlines())
    if headers not in (SDK_HEADERS_23, SDK_HEADERS_24) or normalized != '\n'.join(headers) + '\n':
        raise ValueError('SDK manifest must be exactly the reviewed ordered 23 or 24 headers')
    manifest_sha = hashlib.sha256(normalized.encode('utf-8')).hexdigest()
    if manifest_sha != SDK_MANIFEST_SHA256[len(headers)]:
        raise ValueError('canonical SDK manifest identity differs')
    return headers, manifest_sha


def validate_sdk_inventory(manifest, owner_text, original, staged, objects):
    headers, manifest_sha = sdk_headers(manifest)
    owners = {}
    for row in owner_text.splitlines():
        parts = row.split('|')
        if len(parts) != 2 or any(not p or p != p.strip() for p in parts) or parts[0] in owners:
            raise ValueError('SDK header owner row missing, noncanonical or duplicate')
        owners[parts[0]] = parts[1]
    expected_owners = {h: 'sdk/include/' + h for h in headers}
    public_owners = {h: owner for h, owner in owners.items() if owner.startswith('sdk/include/') or h in headers}
    if public_owners != expected_owners:
        raise ValueError('SDK public header owners differ from exact canonical membership')
    if set(original) != set(headers) or staged != original or any(
            not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value) for value in original.values()):
        raise ValueError('SDK staged/source header hash membership differs')
    # Exact MSVC Release object spelling observed for these generated first-include units.
    expected_objects = {'sdk/odbcpp_sdk_header_isolation.dir/Release/' + h.replace('/', '_') + '.obj'
                        for h in headers}
    if set(objects) != expected_objects or any(
            type(v.get('bytes')) is not int or v['bytes'] <= 0 or
            not isinstance(v.get('sha256'), str) or not re.fullmatch('[0-9a-f]{64}', v['sha256'])
            for v in objects.values()):
        raise ValueError('one fresh bound MSVC object per exact SDK header required')
    return {'manifest_sha256': manifest_sha, 'owners': expected_owners,
            'headers': staged, 'objects': objects}


def inventory():
    listing = json.loads(output('ctest', '--test-dir', str(BUILD), '-C', 'Release', '-R', selected_regex(), '--show-only=json-v1'))
    if sorted(t['name'] for t in listing['tests']) != sorted(selected() + list(CHECKS)):
        raise ValueError('exact owning and applicable architecture CTest inventory required')
    write('ctest-inventory.json', listing)
    binaries = {}
    for name in selected():
        binary = BUILD / 'tests/Release' / (name + '.exe')
        text = output(str(binary.resolve()), '--gtest_list_tests')
        (EVIDENCE / (name + '-list.txt')).write_text(text + '\n', encoding='utf-8')
        names = gtest_names(text)
        if name == 'test_backend_contract' and os.environ['WINDOWS_PRODUCT_HEAD'] == FIRST_PRODUCT and not set(NUMERIC_CASES) <= set(names):
            raise ValueError('four meaningful numeric product cases missing')
        binaries[name] = {'sha256': sha(binary), 'path': str(binary.resolve()), 'names': names}
    write('executables.json', binaries)
    objects = {relative_key(p, BUILD): {'bytes': p.stat().st_size, 'sha256': sha(p)}
               for p in sorted(BUILD.rglob('*.obj')) if 'Release' in p.parts}
    manifest = Path('sdk/contract_headers.txt').read_text(encoding='utf-8')
    headers, _ = sdk_headers(manifest)
    staged = {h: sha(BUILD / 'sdk/contract-include' / h) for h in headers}
    original = {h: sha(Path('sdk/include') / h) for h in headers}
    sdk_objects = {p: v for p, v in objects.items() if 'odbcpp_sdk_header_isolation.dir' in p}
    write('sdk-header-isolation.json', validate_sdk_inventory(
        manifest, Path('sdk/header_owners.txt').read_text(encoding='utf-8'), original, staged, sdk_objects))
    write('objects.json', objects)
    links = {relative_key(p, BUILD): {'bytes': p.stat().st_size, 'sha256': sha(p)}
             for p in sorted(BUILD.rglob('*')) if p.is_file() and 'Release' in p.parts and p.suffix.lower() in ('.lib', '.dll', '.exe', '.map')}
    if not any(p.endswith('odbcpp_core.lib') for p in links) or 'Release/odbcpp.dll' not in links:
        raise ValueError('fresh core lib and driver DLL required')
    write('links.json', links)
    logs = {}
    lines, line_ids = [], {}
    raw_logs = {}
    commands, reads, link_commands = [], [], []
    for p in sorted(BUILD.rglob('*')):
        if not p.is_file() or 'Release' not in p.parts or p.suffix.lower() not in ('.tlog', '.rsp'):
            continue
        key = relative_key(p, BUILD)
        raw = p.read_bytes()
        text = raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8', errors='replace')
        raw_logs[key] = text
        ids = []
        for line in text.splitlines():
            if line not in line_ids:
                line_ids[line] = len(lines)
                lines.append(line)
            ids.append(line_ids[line])
        logs[key] = {'sha256': sha(p), 'line_ids': ids}
        if p.name.lower().startswith('cl.command'):
            commands.append(key)
        if p.name.lower().startswith('cl.read'):
            reads.append(key)
        if p.name.lower().startswith('link.command'):
            link_commands.append(key)
    if not commands or not reads or not link_commands:
        raise ValueError('actual MSVC compiler command/read dependency tlogs required')
    write('msvc-tracking.json', {'unique_lines': lines, 'files': logs,
          'encoding': 'ordered per-file line IDs refer to unique_lines; original raw files independently hashed'})
    dependencies = {}
    for key in reads:
        for line in raw_logs[key].splitlines():
            path = Path(line.strip())
            if path.is_file():
                dependencies[str(path)] = sha(path)
    if not dependencies:
        raise ValueError('actual MSVC read dependencies missing')
    write('msvc-read-dependency-hashes.json', dependencies)
    projects = {relative_key(p, BUILD): {'sha256': sha(p), 'text': p.read_text(encoding='utf-8-sig', errors='replace')}
                for p in sorted(BUILD.rglob('*.vcxproj')) if p.stem in selected() + ['odbcpp_core', 'odbcpp_driver', 'odbcpp_sdk_header_isolation']}
    write('msvc-project-flags.json', projects)


def test():
    binaries = json.loads((EVIDENCE / 'executables.json').read_text())
    original = os.environ.get('GTEST_OUTPUT')
    os.environ['GTEST_OUTPUT'] = 'xml:' + EVIDENCE.resolve().as_posix() + '/gtest/'
    try:
        measure('test', ['ctest', '--test-dir', str(BUILD), '-C', 'Release', '-R', selected_regex(),
                        '--parallel', '2', '--timeout', '90', '--no-tests=error', '--output-on-failure',
                        '--output-junit', 'windows-product-ctest.xml'], 600)
    finally:
        if original is None:
            os.environ.pop('GTEST_OUTPUT', None)
        else:
            os.environ['GTEST_OUTPUT'] = original
        junit = BUILD / 'windows-product-ctest.xml'
        if junit.exists():
            shutil.copyfile(junit, EVIDENCE / 'ctest.xml')
    results = {}
    for name, entry in binaries.items():
        if sha(entry['path']) != entry['sha256']:
            raise ValueError('owning executable changed during tests')
        results[name] = xml_result(EVIDENCE / 'gtest' / (name + '.xml'), entry['names'])
    root = ET.parse(EVIDENCE / 'ctest.xml').getroot()
    if sorted(c.attrib['name'] for c in root.iter('testcase')) != sorted(selected() + list(CHECKS)) or any(
            list(root.iter(tag)) for tag in ('failure', 'error', 'skipped')):
        raise ValueError('exact successful selected CTest results required')
    write('results.json', results)


def completion():
    phases = {}
    for phase in PHASES:
        path = EVIDENCE / (phase + '.json')
        phases[phase] = json.loads(path.read_text()) if path.exists() else {'status': 'missing'}
    complete = all(v.get('status') == 'passed' for v in phases.values()) and (EVIDENCE / 'results.json').exists()
    return complete, phases


def finish():
    source_guard()
    complete, phases = completion()
    write('finish.json', {'finished_at': utc(), 'complete': complete, 'phases': phases,
          'disk_free_bytes_after': shutil.disk_usage('.').free,
          'cache': {'restored': False, 'saved': False},
          'cleanup': 'phase command records retain direct terminal, owned job active-process counts and timeout tree-kill evidence',
          'memory_limit': 'direct process peak, not aggregate concurrent peak',
          'timing_limit': 'runner timestamps exclude exact provider queue and step stamps; coordinator retains run/jobs timestamps'})
    if not complete:
        raise RuntimeError('incomplete or failed Windows qualification; original failure/missing phases preserved')


def package():
    EVIDENCE.mkdir(exist_ok=True)
    files = {}
    for p in EVIDENCE.rglob('*'):
        if p.is_symlink():
            raise ValueError('evidence symlink rejected')
        if p.is_file():
            if p.suffix not in TEXT_SUFFIXES:
                raise ValueError('only text evidence permitted')
            if p.name != 'evidence-index.json':
                files[str(p.relative_to(EVIDENCE))] = {'bytes': p.stat().st_size, 'sha256': sha(p)}
    encoded = json.dumps(files, indent=2, sort_keys=True) + '\n'
    if sum(f['bytes'] for f in files.values()) + len(encoded.encode()) > MAX_BYTES:
        raise ValueError('source-only artifact exceeds 20 MiB including its index')
    (EVIDENCE / 'evidence-index.json').write_text(encoded, encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('init', *PHASES, 'finish', 'package'))
    args = parser.parse_args()
    if os.name == 'nt':
        os.environ['PATH'] = 'C:/Program Files/OpenSSL/bin' + os.pathsep + os.environ['PATH']
        os.environ['MSBUILDDISABLENODEREUSE'] = '1'
    try:
        if args.phase in PHASES:
            source_guard()
        globals()[args.phase]()
        if args.phase in PHASES:
            source_guard()
            write(args.phase + '.json', {'status': 'passed', 'finished_at': utc()})
    except Exception as exc:
        write(args.phase + '.json', {'status': 'failed', 'at': utc(), 'type': type(exc).__name__, 'error': str(exc)[:4096]})
        raise


if __name__ == '__main__':
    main()
