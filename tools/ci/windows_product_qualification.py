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
from pathlib import Path
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
           'test_redshift_catalog_execution', 'test_sql_escape')
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
            stages.append({'stage': stage, 'at': utc(), 'new_held_identities': len(found)})
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
            try:
                job.terminate()  # Job only; never kill a recorded PID or a nonmember.
            except Exception as exc:
                errors.append({'stage': 'terminate_owned_job', 'error': str(exc)})
            deadline = cleanup_start + cleanup_seconds
            count = active('cleanup')
            while count and time.monotonic() < deadline:
                time.sleep(0.05)
                count = active('cleanup')
        final = active('after_cleanup')
        for item in held:
            record = dict(item.evidence)
            try:
                record['terminal_after_cleanup'] = item.terminal()
            except Exception as exc:
                record['terminal_after_cleanup'] = None
                errors.append({'stage': 'held_process_reap', 'pid': record['pid'], 'error': str(exc)})
            records.append(record)
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
        class PidList(ctypes.Structure):
            _fields_ = [('assigned', ctypes.c_ulong), ('listed', ctypes.c_ulong),
                        ('pids', ctypes.c_size_t * limit)]
        info = PidList()
        if not self.api.QueryInformationJobObject(self.handle, 3, ctypes.byref(info), ctypes.sizeof(info), None):
            return [], [{'error': 'owned PID list query failed', 'winerror': ctypes.get_last_error(), 'limit': limit}]
        if info.assigned > limit or info.listed > limit or info.listed != info.assigned:
            return [], [{'error': 'owned PID list incomplete/over bound', 'assigned': info.assigned, 'listed': info.listed, 'limit': limit}]
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
                held.append(HeldOwnedProcess(self.api, handle, {'pid': pid, 'image_path': image.value,
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


def measure(name, command, timeout):
    EVIDENCE.mkdir(exist_ok=True)
    start, tick = utc(), time.monotonic()
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
              'elapsed_seconds': time.monotonic() - tick, 'exit': code,
              'timed_out': timed_out, 'direct_pid': process.pid,
              'direct_terminal': terminal, 'cleanup': cleanup, 'launch_order': launch_order,
              'direct_process_peak_working_set_bytes': peak,
              'memory_limit': 'direct child peak only, not summed process-tree peak',
              'disk_free_bytes_after': shutil.disk_usage('.').free,
              'log_bytes_emitted': total[0], 'log_truncated': total[0] > LOG_BYTES}
    write(name + '-command.json', record)
    if code or timed_out or reader.is_alive() or unexpected_children or invalid_cleanup:
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
    measure('configure', ['cmake', '-S', '.', '-B', str(BUILD), *PROFILE], 300)
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
    measure('build', ['cmake', '--build', str(BUILD), '--config', 'Release', '--parallel', '2', '--target',
                     *selected(), 'odbcpp_sdk_header_isolation', 'odbcpp_driver', '--',
                     '/verbosity:normal', '/nodeReuse:false', '/p:TrackFileAccess=true'], 1200)


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
    headers = Path('sdk/contract_headers.txt').read_text().splitlines()
    if len(headers) != 23:
        raise ValueError('SDK23 manifest changed; requires reviewed scope update')
    staged = {h: sha(BUILD / 'sdk/contract-include' / h) for h in headers}
    if any(staged[h] != sha(Path('sdk/include') / h) for h in headers):
        raise ValueError('SDK staged headers differ from product source')
    sdk_objects = {p: v for p, v in objects.items() if 'odbcpp_sdk_header_isolation.dir' in p}
    if len(sdk_objects) != len(headers):
        raise ValueError('missing fresh SDK first-only-include objects')
    write('sdk-header-isolation.json', {'headers': staged, 'objects': sdk_objects})
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
