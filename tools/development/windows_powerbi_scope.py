"""One-use Power BI process custody; no UI input, DSN, SQL or provider calls.

The existing native job implementation retains creation/limits/wait/termination.
Read-only wrappers retain its new job handle for exact membership observations.
The caller supplies the remaining original execution budget, never a reset.
"""
import ctypes as C
from ctypes import wintypes as W
import hashlib
import importlib.util
import os
from pathlib import Path
import threading
import time

from windows_powerbi_contract import APP, APP_SHA



def need(value, code):
    if not value:
        raise RuntimeError(code)


class Entry(C.Structure):
    _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('pid', W.DWORD),
        ('heap', C.c_size_t), ('module', W.DWORD), ('threads', W.DWORD),
        ('parent', W.DWORD), ('priority', W.LONG), ('flags', W.DWORD), ('name', W.WCHAR * 260)]


class FileTime(C.Structure):
    _fields_ = [('low', W.DWORD), ('high', W.DWORD)]


class ModuleEntry(C.Structure):
    _fields_ = [('size',W.DWORD),('module_id',W.DWORD),('pid',W.DWORD),
        ('global_usage',W.DWORD),('process_usage',W.DWORD),('base',C.c_void_p),
        ('bytes',W.DWORD),('module',W.HMODULE),('name',W.WCHAR*256),('path',W.WCHAR*260)]


class PowerBiScope:
    def __init__(self, console, *, native, job_path, job_sha, module_pins):
        self.native = native
        self.module_pins = {str(Path(p)).casefold(): s for p, s in module_pins.items()}
        path = Path(job_path)
        need(hashlib.sha256(path.read_bytes()).hexdigest() == job_sha, 'native_job_source_pin')
        need(hashlib.sha256(APP.read_bytes()).hexdigest() == APP_SHA, 'fixed_preinstalled_app_pin')
        spec = importlib.util.spec_from_file_location('_oneuse_pbi_job', path)
        self.job = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.job)
        self.lock = threading.RLock()
        self.handle = None
        self.tracked = {}
        self.retired_handles = []
        self.console = Path(console)
        self.receipt = dict(application=str(APP), application_sha256=APP_SHA,
            custody_module_sha256=job_sha, observer_only=True)
        self.error = None
        self.exit = None
        self.worker = None
        self.finished = None
        self.last_capture = None
        self.original_create, self.original_close = self.job.create_job, self.job.close
        def create(*args):
            need(args == (None, None), 'unchanged_unnamed_job_creation')
            handle = self.original_create(*args)
            with self.lock:
                self.handle = handle
            return handle
        def close(handle):
            with self.lock:
                if handle == self.handle:
                    try:
                        return self.original_close(handle)
                    finally:
                        self.handle = None
            return self.original_close(handle)
        self.job.create_job, self.job.close = create, close
        k = self.job.K
        self.snapshot_api = self.job.api('CreateToolhelp32Snapshot', [W.DWORD,W.DWORD], W.HANDLE)
        self.first = self.job.api('Process32FirstW', [W.HANDLE,C.POINTER(Entry)])
        self.next = self.job.api('Process32NextW', [W.HANDLE,C.POINTER(Entry)])
        self.module_first = self.job.api('Module32FirstW',[W.HANDLE,C.POINTER(ModuleEntry)])
        self.module_next = self.job.api('Module32NextW',[W.HANDLE,C.POINTER(ModuleEntry)])
        self.times = self.job.api('GetProcessTimes', [W.HANDLE,*([C.POINTER(FileTime)]*4)])
        self.session = self.job.api('ProcessIdToSessionId', [W.DWORD,C.POINTER(W.DWORD)])
        own_session = W.DWORD()
        self.job.check(self.session(os.getpid(), C.byref(own_session)))
        need(own_session.value == 1, 'actual_interactive_session1_required')
        self.baseline = self.processes()
        need(not any(self.relevant(row['name']) for row in self.baseline.values()), 'existing_PBI_or_data_worker_refused')

    @staticmethod
    def relevant(name):
        name = name.lower()
        return name in ('pbidesktop.exe', 'msmdsrv.exe') or name.startswith('microsoft.mashup.')

    def processes(self):
        handle = self.snapshot_api(2, 0)
        need(handle != C.c_void_p(-1).value, 'process_snapshot')
        rows = {}
        try:
            entry = Entry()
            entry.size = C.sizeof(entry)
            ok = self.first(handle, C.byref(entry))
            while ok:
                need(len(rows) < 16384, 'process_inventory_bound')
                rows[entry.pid] = dict(pid=entry.pid, parent=entry.parent, name=entry.name)
                ok = self.next(handle, C.byref(entry))
            need(C.get_last_error() == 18, 'complete_process_inventory')
        finally:
            self.original_close(handle)
        return rows

    def _members(self):
        capacity = 64
        while True:
            data = C.create_string_buffer(8 + capacity*C.sizeof(C.c_size_t))
            ok = self.job.query_job(self.handle, 3, data, C.sizeof(data), None)
            if ok:
                assigned, returned = (W.DWORD*2).from_buffer(data)
                if assigned <= returned:
                    return set((C.c_size_t*returned).from_buffer(data, 8))
            else:
                need(C.get_last_error() == 234, 'exact_job_member_query')
            capacity *= 2
            need(capacity <= 8192, 'exact_job_member_capacity')

    def _observe(self, row):
        pid = row['pid']
        if pid in self.tracked and self.job.wait(self.tracked[pid]['handle'],0)==0:
            # The census PID can already denote a different process. Retain the
            # old birth handle for terminal evidence and open the current PID.
            self.retired_handles.append((pid,self.tracked.pop(pid)))
        if pid not in self.tracked:
            handle = self.job.open_process(0x101000, False, pid)
            if not handle:
                need(C.get_last_error() == 87, 'owned_process_handle')
                return dict(row, exited_before_open=True)
            try:
                born, exited, kernel, user = FileTime(), FileTime(), FileTime(), FileTime()
                self.job.check(self.times(handle, C.byref(born), C.byref(exited), C.byref(kernel), C.byref(user)))
                size = W.DWORD(32768)
                image = C.create_unicode_buffer(size.value)
                self.job.check(self.job.query_image(handle, 0, image, C.byref(size)))
                self.tracked[pid] = dict(handle=handle, image=image.value,
                    created_ticks=(born.high << 32) | born.low)
            except BaseException:
                self.original_close(handle)
                raise
        held = self.tracked[pid]
        if self.job.wait(held['handle'], 0) == 0:
            return dict(row, exited=True, image=held['image'], created_ticks=held['created_ticks'])
        assigned = W.BOOL()
        if self.handle:
            self.job.check(self.job.in_job(held['handle'], self.handle, C.byref(assigned)))
            held['was_in_exact_owned_job']=bool(assigned.value)
        return dict(row, image=held['image'], created_ticks=held['created_ticks'], in_exact_owned_job=bool(assigned.value))

    def _module_target_live(self, row):
        # The held handle identifies the original birth, not a reusable PID.
        status = self.job.wait(self.tracked[row['pid']]['handle'], 0)
        need(status in (0, 258), 'module_target_wait_failed')
        if status == 0:
            return False
        need(self.handle is not None, 'module_target_job_closed')
        assigned = W.BOOL()
        self.job.check(self.job.in_job(self.tracked[row['pid']]['handle'],
                                      self.handle, C.byref(assigned)))
        need(assigned.value, 'module_target_left_exact_owned_job')
        return True

    def modules(self, row):
        """Read loaded-image metadata with bounded loader-readiness retries.

        Windows documents module snapshots failing while loader state is not
        initialized or changes. Only BAD_LENGTH/PARTIAL_COPY are retried; other
        errors fail with their actual code. No incomplete inventory is accepted.
        """
        end = min(self.execution_deadline, time.monotonic() + 5.0)
        observation = dict(attempts=0, transient_errors=[], maximum_wait_seconds=5.0)
        row['module_snapshot_observation'] = observation
        for attempt in range(101):
            if not self._module_target_live(row):
                return dict(process_exited_before_module_snapshot=True)
            need(time.monotonic() < end, 'module_snapshot_readiness_deadline')
            handle = self.snapshot_api(0x18, row['pid'])
            error = C.get_last_error()  # Capture before any other Win32 call.
            observation['attempts'] = attempt + 1
            if handle != C.c_void_p(-1).value:
                break
            observation['transient_errors'].append(error)
            if not self._module_target_live(row):
                return dict(process_exited_before_module_snapshot=True)
            need(error in (24, 299), 'module_snapshot_win32_error_' + str(error))
            remaining = end - time.monotonic()
            need(remaining > 0, 'module_snapshot_readiness_deadline')
            time.sleep(min(0.05, remaining))
        else:
            raise RuntimeError('module_snapshot_readiness_attempt_bound')
        selected = []
        try:
            need(time.monotonic() < end, 'module_snapshot_readiness_deadline')
            if not self._module_target_live(row):
                return dict(process_exited_before_module_snapshot=True)
            entry = ModuleEntry()
            entry.size = C.sizeof(entry)
            ok = self.module_first(handle, C.byref(entry))
            count = 0
            while ok:
                count += 1
                need(count <= 8192, 'module_inventory_bound')
                name = entry.name.lower()
                if (name.startswith(('odbcpp', 'libcrypto', 'libssl')) or name == 'odbc32.dll'
                        or name in {Path(p).name.casefold() for p in self.module_pins}):
                    path = Path(entry.path)
                    held = self.native.HeldFile(path)
                    try:
                        actual = hashlib.sha256(held.read()).hexdigest()
                    finally:
                        held.close()
                    key = str(path).casefold()
                    expected = self.module_pins.get(key)
                    if name != 'odbc32.dll':
                        need(expected == actual, 'observed_driver_runtime_path_or_image_differs')
                    selected.append(dict(name=entry.name, path=str(path), sha256=actual,
                        actual_loaded_module=True, matches_reviewed_runtime=expected == actual))
                ok = self.module_next(handle, C.byref(entry))
            error = C.get_last_error()
            need(error == 18, 'complete_data_consumer_module_inventory_win32_' + str(error))
            if not self._module_target_live(row):
                return dict(process_exited_during_module_snapshot=True)
        finally:
            self.original_close(handle)
        need(time.monotonic() < end, 'module_snapshot_readiness_deadline')
        return dict(inspected_module_count=count, connection_modules=selected)

    @staticmethod
    def trusted_console_parent(image):
        path = Path(image)
        return (path.name.lower() in ('msmdsrv.exe', 'microsoft.mashup.container.netfx45.exe')
                and path == APP.parent/path.name)

    def _console_witness(self, row):
        """Validate lineage with held births; never acquire termination rights."""
        if (row.get('name', '').lower() != 'conhost.exe'
                or Path(row.get('image', '')) != Path('C:/Windows/System32/conhost.exe')):
            return None
        candidates = [*self.retired_handles, *self.tracked.items()]
        for parent_pid, parent in candidates:
            if parent_pid != row.get('parent') or not self.trusted_console_parent(parent['image']):
                continue
            born, exited, kernel, user = FileTime(), FileTime(), FileTime(), FileTime()
            self.job.check(self.times(parent['handle'], C.byref(born), C.byref(exited),
                                      C.byref(kernel), C.byref(user)))
            birth = (born.high << 32) | born.low
            end = (exited.high << 32) | exited.low
            need(birth == parent['created_ticks'], 'held_parent_birth_changed')
            if row['created_ticks'] < birth:
                continue
            stopped = self.job.wait(parent['handle'], 0) == 0
            if stopped:
                # A reused parent PID cannot authorize a console born after the
                # original held process died. Exit time is mandatory, not inferred.
                if not (parent.get('was_in_exact_owned_job') is True
                        and end > 0 and row['created_ticks'] <= end):
                    continue
                evidence = 'HELD_EXACT_JOB_PARENT_LIFETIME'
            else:
                if not self.handle:
                    continue
                assigned = W.BOOL()
                self.job.check(self.job.in_job(parent['handle'], self.handle, C.byref(assigned)))
                if not assigned.value:
                    continue
                parent['was_in_exact_owned_job'] = True
                evidence = 'LIVE_HELD_PARENT_IN_EXACT_JOB'
            return dict(parent_pid=parent_pid, parent_image=parent['image'],
                        parent_created_ticks=birth, parent_exit_ticks=end,
                        evidence=evidence)
        return None

    def capture(self):
        with self.lock:
            result = dict(capture_status='IN_PROGRESS', processes=[], exact_job_members=[])
            self.last_capture = result
            try:
                need(self.handle and self.receipt.get('assignedBeforeExecution') is True,
                     'owned_app_not_started_or_closed')
                members = self._members()
                rows = self.processes()
                root = self.receipt['childPid']
                result.update(root_pid=root, exact_job_members=sorted(members))
                need(root in members and root in rows, 'active_root_in_exact_job_required')
                selected = set(members) | {root}
                # Include broker-created consumers before descendant closure so
                # their new console children are observed as well.
                selected.update(pid for pid, row in rows.items() if self.relevant(row['name']))
                changed = True
                while changed:
                    old = len(selected)
                    selected.update(pid for pid, row in rows.items() if row['parent'] in selected)
                    changed = len(selected) != old
                observed = result['processes']
                for pid in sorted(selected):
                    observed.append(self._observe(rows.get(pid, dict(pid=pid, parent=0,
                        name='<job-member-exited-before-snapshot>'))))
                for row in observed:
                    if row.get('exited') or row.get('exited_before_open') or row.get('in_exact_owned_job'):
                        continue
                    witness = self._console_witness(row)
                    need(witness is not None, 'unowned_relevant_app_consumer')
                    row['console_outside_job_requires_terminal_reap'] = True
                    row['console_parent_witness'] = witness
                for row in observed:
                    if self.relevant(row['name']) and row.get('in_exact_owned_job'):
                        row['loaded_image_evidence'] = self.modules(row)
                result.update(capture_status='PASS', all_observed_data_consumers_in_exact_job=True,
                    scope='Observed exact-job consumers; closed console lineage retained by process handle')
                return result
            except BaseException as error:
                result.update(capture_status='FAILED', error_type=type(error).__name__,
                              error_reason=str(error))
                raise

    def start(self, timeout):
        need(self.worker is None and type(timeout) is int and 0 < timeout <= 1800, 'single_start_remaining_app_budget')
        def run():
            try:
                with self.console.open('xb') as output:
                    self.exit = self.job.run([str(APP)], output, self.receipt, timeout=timeout)
            except BaseException as error:
                self.error = type(error).__name__
        self.execution_deadline = time.monotonic() + timeout
        self.worker = threading.Thread(target=run, daemon=True)
        self.worker.start()

    def abort(self):
        with self.lock:
            if self.handle:
                self.job.check(self.job.terminate_job(self.handle, 124))
                self.receipt['operator_abort'] = True

    def finish(self):
        if self.finished is not None:
            return self.finished
        need(self.worker is not None, 'started_app_required')
        self.worker.join(timeout=35)
        known = not self.worker.is_alive() and self.receipt.get('treeEmpty') is True
        end = time.monotonic()+30
        processes = []
        final_census=[]
        try:
            # A broker may create a data consumer after the last requested
            # capture. Never infer its absence from the owned job being empty.
            rows=self.processes()
            # Capture both live relevant parents and late console children. A
            # numeric PID alone selects a console for inspection, never acceptance.
            parent_pids = {pid for pid, _ in [*self.retired_handles, *self.tracked.items()]}
            parent_pids.update(pid for pid, row in rows.items() if self.relevant(row['name']))
            selected = [row for row in rows.values() if self.relevant(row['name'])
                        or (row['name'].lower() == 'conhost.exe' and row['parent'] in parent_pids)]
            for row in selected:
                seen = self._observe(row)
                final_census.append(seen)
                if not seen.get('exited') and not seen.get('exited_before_open'):
                    witness = self._console_witness(seen)
                    if witness is None:
                        known = False
                    else:
                        seen['console_outside_job_requires_terminal_reap'] = True
                        seen['console_parent_witness'] = witness
        except BaseException as error:
            known=False
            self.receipt['final_process_census_error']=type(error).__name__
        try:
            # Even an ambiguous census must attempt bounded natural-stop waits
            # on every held process; it never gains foreign termination rights.
            for pid,held in [*self.retired_handles,*self.tracked.items()]:
                row = dict(pid=pid, image=held['image'], created_ticks=held['created_ticks'], stopped=False)
                processes.append(row)
                try:
                    row['stopped'] = self.job.wait(held['handle'], max(0,int((end-time.monotonic())*1000))) == 0
                except BaseException as error:
                    row['wait_error_type'] = type(error).__name__
                known = known and row['stopped']
        finally:
            for _,held in [*self.retired_handles,*self.tracked.items()]:
                self.original_close(held['handle'])
            self.tracked.clear()
            self.retired_handles.clear()
        self.finished = dict(receipt=self.receipt, returned_exit=self.exit, error_type=self.error,
            readers_known=known, held_observed_processes=processes,
            final_relevant_process_census=final_census,
            last_capture=getattr(self, 'last_capture', None),
            no_provider_or_feature_pass_claim=True)
        return self.finished
