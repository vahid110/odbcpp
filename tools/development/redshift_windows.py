"""WSL resource owner to local native Windows CTest backend; no SSH or remote executor."""
import copy
import datetime as dt
import json
import os
from pathlib import Path, PureWindowsPath
import stat
import subprocess
from types import SimpleNamespace

from tools.development import redshift_ctest_ops as opsmod
from tools.development.redshift_ctest_lifecycle import need, record


def mounted(path):
    value = PureWindowsPath(path)
    need(value.is_absolute() and value.drive.upper() == 'C:' and '..' not in value.parts,
         'local_native_C_drive_path')
    return Path('/mnt/c').joinpath(*value.parts[1:])


class WindowsOperations(opsmod.Operations):
    def __init__(self, *, native_plan, **kwargs):
        super().__init__(**kwargs)
        self.native_plan = native_plan
        self.native_ctest_invoked = False
        self.windows_readers_known = True
        self.payload_custody = None

    def open_inputs(self, directory, child_end):
        need(Path(directory) == self.directory and self.native is None, 'oneuse_resource_inputs')
        self.native = self.timeout_factory.NativeOps(
            self.approval, self.preparation_manifest, directory, child_end)
        self.native.prepare()

    def stage_profiles(self, index_path, run, state, logging, end):
        need(state['prepared'] is True and logging == opsmod.inputs.LOG_OPTIONS,
             'prepared_logged_native_scope')
        variables = []
        actual = state.get('timeout_record') if self.relationship_partition else state['timeout_record']
        need(len(self.blueprint['cases']) == len(self.specs), 'complete_native_case_inputs')
        for case, spec in zip(self.blueprint['cases'], self.specs):
            need(case['id'] == spec['ctest_id'] and case['gtest_filter'] == spec['case'],
                 'native_case_order')
            values = copy.deepcopy(self.profiles[spec['profile']]['variables'])
            if self.relationship_partition:
                values = self.relationship_profile_variables(self.profiles[spec['profile']])
            if 'ODBCPP_REDSHIFT_TIMEOUT_OID' in values:
                need(actual is not None, 'timeout_fixture_required')
                values.update(ODBCPP_REDSHIFT_TIMEOUT_SCHEMA=actual['schema'],
                              ODBCPP_REDSHIFT_TIMEOUT_TABLE=actual['table'],
                              ODBCPP_REDSHIFT_TIMEOUT_OID=str(actual['oid']))
            if spec['env'] is not None:
                values[spec['env']] = spec['marker']
            if spec['case'] == 'RedshiftMetadataWorkflowRealTest.MetadataToPreparedQueryShowConfigured':
                values.update(ODBCPP_REDSHIFT_TEST_SCHEMA='odbcpp_fixture',
                              ODBCPP_REDSHIFT_TEST_TABLE='pilot_rows')
            if spec['case'] == self.factory.TWO_DB_CASE:
                values[self.factory.TWO_DB_VARIABLE] = state['substitutions']['second_database']
            variables.append({'id': case['id'], 'variables': values})
        config = {k: self.native.config[k] for k in ('host','port','database','test_user','test_password')}
        config['ca'] = self.native_plan['ca']
        payload = {'schema_version': 1, 'source_head': self.approval['source_head'],
                   'run_id': self.directory.name, 'config': config, 'case_variables': variables}
        body = (json.dumps(payload)+'\n').encode()
        need(len(body) <= 16*1024*1024, 'native_payload_bound')
        path = self.directory/'native-test-input.json'
        fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        st = os.fstat(fd)
        self.payload_custody = (path, fd, b'', (st.st_dev, st.st_ino, st.st_size))
        try:
            with os.fdopen(os.dup(fd), 'wb') as stream:
                need(stream.write(body) == len(body), 'native_payload_short_write')
                stream.flush();os.fsync(stream.fileno())
        finally:
            st = os.fstat(fd);actual_bytes = os.pread(fd, st.st_size+1, 0)
            self.payload_custody = (path, fd, actual_bytes, (st.st_dev, st.st_ino, st.st_size))
        need(actual_bytes == body, 'native_payload_held_readback')
        return path

    def delegate_process(self, body, end):
        native = self.native_plan
        argv = [str(mounted(native['python'])), '-B',
                str(PureWindowsPath(native['source_directory'])/'tools/development/windows_live_backend.py'),
                '--receive']
        need(end-self.now() >= 1560, 'native_complete_suite_and_cleanup_reserve')
        seconds = min(1550, int(end-self.now()-2))
        process = None;timed_out = False
        fd = os.open(self.directory/'native-windows-console.log',
                     os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            with os.fdopen(fd, 'wb') as stream:
                self.windows_readers_known = False;self.native_ctest_invoked = None
                try:
                    process = subprocess.Popen(argv, env=dict(opsmod.BASE_ENV),
                        stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
                        start_new_session=True, close_fds=True, umask=0o077)
                except BaseException:
                    self.windows_readers_known = True;self.native_ctest_invoked = False
                    raise
                self.processes.append(process)
                try:
                    process.communicate(body, timeout=seconds)
                except subprocess.TimeoutExpired:
                    timed_out = True
                finally:
                    opsmod._stop_group(process)
                    if process.stdin:process.stdin.close()
            need(self._group_absent(process.pid) and process.poll() is not None
                 and self.now() < end, 'native_interop_process_reaped')
            return SimpleNamespace(returncode=process.returncode, timed_out=timed_out,
                                   reaped=True, group_absent=True)
        finally:
            if process is not None and process.poll() is None:
                self.readers_known = False

    def run_native_suite(self, staged, end):
        path, fd, body, identity = self.payload_custody
        need(Path(staged) == path and os.pread(fd, len(body)+1, 0) == body, 'native_payload_custody')
        opsmod.native_custody.before(self, end)
        receipt_path = mounted(self.native_plan['working_directory'])/self.directory.name/'native-receipt.json'
        need(not receipt_path.exists(), 'native_oneuse_receipt_absent')
        authority = {'schema_version': 1, 'state': 'ROOT_ADMITTED', 'independent_review': 'ACCEPT',
            'source_head': self.approval['source_head'], 'scope': 'complete-applicable-live-native-windows',
            'run_id': self.directory.name, 'execute_before_utc':
                (dt.datetime.now(dt.timezone.utc)+dt.timedelta(minutes=5)).isoformat(),
            'execution_timeout_seconds': 1500, 'native': self.native_plan,
            'public_pins': self.native_plan['public_pins']}
        message = (json.dumps({'authority': authority, 'payload': json.loads(body)})+'\n').encode()
        result = self.delegate_process(message, end)
        need(receipt_path.is_file() and not receipt_path.is_symlink()
             and receipt_path.stat().st_nlink == 1, 'native_terminal_receipt_required')
        receipt = json.loads(receipt_path.read_text())
        record(self.directory/'native-windows-receipt.json', receipt)
        need(receipt['source_head'] == self.approval['source_head']
             and receipt['applicable_count'] == len(self.blueprint['cases'])
             and receipt['feature_results_parsed'] is False
             and receipt['resource_operations'] == receipt['SQL_operations'] == 0,
             'native_receipt_scope')
        need(type(receipt['ctest_invocations']) is int and receipt['ctest_invocations'] in (0,1),
             'native_actual_invocation_count')
        self.native_ctest_invoked = receipt['ctest_invocations'] == 1
        jobs = receipt['jobs']
        safe = all(j.get('treeEmpty') is True and j.get('heldProcessHandlesStopped') is True
                   and j.get('jobActiveProcesses') == 0 for j in jobs)
        need(jobs or not self.native_ctest_invoked, 'native_missing_job_receipts')
        need(result.reaped and result.group_absent and result.returncode == receipt['exit'],
             'native_exit_receipt_binding')
        self.windows_readers_known = safe and receipt['readers_known'] is True
        self.readers_known = self.readers_known and self.windows_readers_known
        need(self.windows_readers_known and receipt['native_cleanup_complete'] is True
             and receipt['private_inputs_closed'] is True and receipt['dsn_closed'] is True
             and receipt['lease_released'] is True, 'native_custody_not_closed')
        tested = [j for j in jobs if j['phase'] == 'ctest']
        configured = [j for j in jobs if j['phase'] == 'configure']
        need(len(tested) == len(configured) == 1 and self.native_ctest_invoked
             and receipt['exit'] == receipt['native_policy_exit'], 'one_native_configure_and_CTest')
        return SimpleNamespace(configured=configured[0]['processExit'] == 0,
            returncode=receipt['native_policy_exit'],
            timed_out=tested[0].get('timedOut', False) or result.timed_out,
            reaped=safe, group_absent=safe)

    def reap(self, end):
        safe = super().reap(end)
        self.readers_known = self.readers_known and self.windows_readers_known
        return safe and self.windows_readers_known

    def close_inputs(self, end):
        safe = super().close_inputs(end)
        if self.payload_custody is not None and self.readers_known and self.windows_readers_known:
            path, fd, body, identity = self.payload_custody;st = path.lstat()
            need(not path.is_symlink() and (st.st_dev,st.st_ino,st.st_size) == identity
                 and st.st_nlink == 1 and stat.S_IMODE(st.st_mode) == 0o600
                 and os.pread(fd,len(body)+1,0) == body, 'exact_native_payload_retirement')
            path.unlink();os.close(fd);self.payload_custody = None
        return safe and self.payload_custody is None
