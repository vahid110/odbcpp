"""Pure offline process-custody controls; no Windows API or resource calls."""
import hashlib
import json
from pathlib import Path
import threading
from types import SimpleNamespace
import unittest
import windows_powerbi_scope as module
import sys
from unittest.mock import patch


class ObserverControls(unittest.TestCase):
    def make_scope(self, parent_name='Microsoft.Mashup.Container.NetFX45.exe'):
        scope=object.__new__(module.PowerBiScope)
        scope.lock=threading.RLock();scope.handle=999
        scope.tracked={};scope.retired_handles=[];scope.finished=None;scope.last_capture=None
        scope.receipt=dict(assignedBeforeExecution=True,childPid=10,treeEmpty=True)
        scope.worker=SimpleNamespace(join=lambda timeout:None,is_alive=lambda:False)
        scope.exit=0;scope.error=None
        scope.closed=[];scope.waited=[]
        scope.original_close=lambda handle:scope.closed.append(handle)
        scope.data={
            1000:dict(image=str(module.APP),birth=100,exit=0,alive=True,exact=True),
            2000:dict(image=str(module.APP.parent/parent_name),birth=200,exit=0,alive=True,exact=True),
            3000:dict(image='C:/Windows/System32/conhost.exe',birth=300,exit=0,alive=True,exact=False),
        }
        scope.pid_handles={10:1000,20:2000,30:3000}
        scope.rows={10:dict(pid=10,parent=1,name='PBIDesktop.exe'),
                    20:dict(pid=20,parent=10,name=parent_name),
                    30:dict(pid=30,parent=20,name='conhost.exe')}
        scope.processes=lambda:dict(scope.rows)
        scope._members=lambda:{10,20}
        scope.modules=lambda row:dict(offline_metadata=True)
        def check(value):
            if not value:
                raise RuntimeError('injected_native_check_failure')
        def wait(handle,timeout):
            scope.waited.append((handle,timeout))
            item=scope.data[handle]
            if timeout>0 and item.get('natural_stop'):
                item['alive']=False;item['exit']=500
            return 258 if item['alive'] else 0
        def in_job(handle,job,out):
            self.assertEqual(job,999)
            out._obj.value=scope.data[handle]['exact']
            return True
        def query_image(handle,flags,buffer,size):
            buffer.value=scope.data[handle]['image']
            return True
        scope.job=SimpleNamespace(wait=wait,check=check,
            open_process=lambda rights,inherit,pid:scope.pid_handles[pid],
            in_job=in_job,query_image=query_image)
        def times(handle,born,exited,*args):
            for target,value in ((born,scope.data[handle]['birth']),(exited,scope.data[handle]['exit'])):
                target._obj.low=value&0xffffffff;target._obj.high=value>>32
            return True
        scope.times=times
        return scope

    def tracked(self,scope,pid,handle):
        item=scope.data[handle]
        scope.tracked[pid]=dict(handle=handle,image=item['image'],created_ticks=item['birth'],
                                was_in_exact_owned_job=item['exact'])

    def final_scope(self):
        scope=self.make_scope()
        self.tracked(scope,20,2000)
        scope.data[2000].update(alive=False,exit=400)
        scope.data[3000]['natural_stop']=True
        scope.handle=None
        scope.rows={30:scope.rows[30]}
        return scope

    def test_valid_mashup_console_capture(self):
        scope=self.make_scope()
        result=scope.capture()
        console=next(row for row in result['processes'] if row['pid']==30)
        self.assertEqual(result['capture_status'],'PASS')
        self.assertEqual(console['console_parent_witness']['parent_pid'],20)
        self.assertEqual(console['console_parent_witness']['evidence'],'LIVE_HELD_PARENT_IN_EXACT_JOB')
        self.assertTrue(console['console_outside_job_requires_terminal_reap'])
        self.assertIn(30,scope.tracked)

    def test_existing_msmd_console_capture(self):
        scope=self.make_scope('msmdsrv.exe')
        self.assertEqual(scope.capture()['capture_status'],'PASS')

    def test_late_mashup_console_finish_waits_exact_handle(self):
        scope=self.final_scope()
        result=scope.finish()
        self.assertTrue(result['readers_known'])
        self.assertEqual(result['final_relevant_process_census'][0]['pid'],30)
        self.assertEqual(result['final_relevant_process_census'][0]['console_parent_witness']['evidence'],
                         'HELD_EXACT_JOB_PARENT_LIFETIME')
        self.assertIn(3000,scope.closed)
        self.assertTrue(any(handle==3000 and timeout>0 for handle,timeout in scope.waited))

    def test_late_console_still_running_is_unknown_and_sticky(self):
        scope=self.final_scope();scope.data[3000]['natural_stop']=False
        result=scope.finish()
        self.assertFalse(result['readers_known'])
        scope.data[3000]['alive']=False
        self.assertIs(scope.finish(),result)
        self.assertFalse(scope.finish()['readers_known'])

    def test_console_after_parent_exit_rejected_despite_reused_parent_pid(self):
        scope=self.final_scope();scope.data[2000]['exit']=250
        result=scope.finish()
        self.assertFalse(result['readers_known'])
        self.assertNotIn('console_parent_witness',result['final_relevant_process_census'][0])
        self.assertIn(3000,scope.closed)

    def test_console_before_parent_birth_rejected(self):
        scope=self.make_scope();scope.data[3000]['birth']=199
        with self.assertRaisesRegex(RuntimeError,'unowned_relevant_app_consumer'):
            scope.capture()

    def test_untrusted_parent_image_rejected(self):
        scope=self.make_scope();scope.data[2000]['image']='C:/foreign/Microsoft.Mashup.Container.NetFX45.exe'
        with self.assertRaisesRegex(RuntimeError,'unowned_relevant_app_consumer'):
            scope.capture()

    def test_unclosed_mashup_parent_name_rejected(self):
        scope=self.make_scope('Microsoft.Mashup.Unapproved.exe')
        with self.assertRaisesRegex(RuntimeError,'unowned_relevant_app_consumer'):
            scope.capture()

    def test_foreign_console_image_rejected(self):
        scope=self.make_scope();scope.data[3000]['image']='C:/foreign/conhost.exe'
        with self.assertRaisesRegex(RuntimeError,'unowned_relevant_app_consumer'):
            scope.capture()

    def test_unknown_console_parent_rejected(self):
        scope=self.make_scope();scope.rows[30]['parent']=404
        scope._members=lambda:{10,20,30}
        with self.assertRaisesRegex(RuntimeError,'unowned_relevant_app_consumer'):
            scope.capture()

    def test_foreign_data_worker_rejected(self):
        scope=self.make_scope();scope.data[2000]['exact']=False
        scope._members=lambda:{10}
        with self.assertRaisesRegex(RuntimeError,'unowned_relevant_app_consumer'):
            scope.capture()

    def test_late_foreign_data_worker_keeps_cleanup_unknown(self):
        scope=self.final_scope()
        scope.data[4000]=dict(image=str(module.APP.parent/'Microsoft.Mashup.Container.NetFX45.exe'),
                              birth=450,exit=0,alive=True,exact=False,natural_stop=True)
        scope.pid_handles[40]=4000
        scope.rows[40]=dict(pid=40,parent=777,name='Microsoft.Mashup.Container.NetFX45.exe')
        self.assertFalse(scope.finish()['readers_known'])

    def test_current_parent_pid_reuse_never_uses_stopped_birth(self):
        scope=self.make_scope()
        self.tracked(scope,20,2000)
        scope.data[2000].update(alive=False,exit=250)
        scope.data[2100]=dict(image='C:/foreign/reused.exe',birth=275,exit=0,alive=True,exact=False)
        scope.pid_handles[20]=2100;scope.rows[20]['name']='reused.exe'
        scope._members=lambda:{10}
        with self.assertRaisesRegex(RuntimeError,'unowned_relevant_app_consumer'):
            scope.capture()
        row=next(row for row in scope.last_capture['processes'] if row['pid']==20)
        self.assertEqual(row['created_ticks'],275)
        self.assertEqual(scope.retired_handles[0][1]['handle'],2000)

    def test_missing_parent_exit_time_remains_unknown(self):
        scope=self.final_scope();scope.data[2000]['exit']=0
        self.assertFalse(scope.finish()['readers_known'])

    def test_parent_without_exact_job_history_remains_unknown(self):
        scope=self.final_scope();scope.tracked[20]['was_in_exact_owned_job']=False
        self.assertFalse(scope.finish()['readers_known'])

    def test_failed_capture_full_rows_reason_retained_in_finish(self):
        scope=self.make_scope();scope.data[3000]['image']='C:/foreign/conhost.exe'
        with self.assertRaisesRegex(RuntimeError,'unowned_relevant_app_consumer'):
            scope.capture()
        capture=scope.last_capture
        self.assertEqual(capture['capture_status'],'FAILED')
        self.assertEqual(capture['error_reason'],'unowned_relevant_app_consumer')
        self.assertEqual({row['pid'] for row in capture['processes']},{10,20,30})
        for item in scope.data.values():
            item.update(alive=False,exit=500)
        scope.rows={};scope.handle=None
        result=scope.finish()
        self.assertIs(result['last_capture'],capture)
        self.assertTrue(result['readers_known'])

    def test_reused_console_pid_preserves_old_handle(self):
        scope=self.final_scope()
        scope.data[2900]=dict(image='C:/Windows/System32/conhost.exe',birth=150,exit=190,alive=False,exact=False)
        scope.tracked[30]=dict(handle=2900,image=scope.data[2900]['image'],created_ticks=150)
        result=scope.finish()
        self.assertTrue(result['readers_known'])
        console=next(row for row in result['held_observed_processes'] if row['pid']==30 and row['created_ticks']==300)
        self.assertTrue(console['stopped'])
        self.assertCountEqual(scope.closed,[2900,2000,3000])


    def test_census_error_still_waits_held_console_and_stays_unknown(self):
        scope=self.final_scope()
        original=scope.times
        def failed_times(handle,*args):
            if handle==2000:
                raise OSError('injected parent lifetime failure')
            return original(handle,*args)
        scope.times=failed_times
        result=scope.finish()
        self.assertFalse(result['readers_known'])
        self.assertEqual(result['receipt']['final_process_census_error'],'OSError')
        self.assertTrue(any(handle==3000 and timeout>0 for handle,timeout in scope.waited))
        self.assertIn(3000,scope.closed)


class ModuleReadinessControls(unittest.TestCase):
    def make_scope(self, errors):
        scope = object.__new__(module.PowerBiScope)
        scope.handle = 99
        scope.tracked = {10: dict(handle=100)}
        scope.execution_deadline = 60.0
        scope.clock = 10.0
        scope.calls = 0
        scope.closed = []
        scope.alive = True
        scope.exact = True
        scope.last_error = 0
        scope.enumeration_error = 18
        scope.original_close = scope.closed.append
        scope.observed = dict(pid=10)
        errors = iter(errors)
        def check(value):
            if not value:
                raise RuntimeError('native_check')
        def in_job(handle, job, assigned):
            self.assertEqual((handle, job), (100, 99))
            assigned._obj.value = scope.exact
            return True
        scope.job = SimpleNamespace(wait=lambda handle, timeout: 258 if scope.alive else 0,
                                    check=check, in_job=in_job)
        def snapshot(flags, pid):
            self.assertEqual((flags, pid), (0x18, 10))
            scope.calls += 1
            scope.last_error = next(errors, 0)
            return module.C.c_void_p(-1).value if scope.last_error else 77
        scope.snapshot_api = snapshot
        def first(handle, entry):
            scope.last_error = scope.enumeration_error
            return False
        scope.module_first = first
        def sleep(seconds):
            scope.clock += seconds
        scope.patches = [patch.object(module.C, 'get_last_error', lambda: scope.last_error),
                         patch.object(module.time, 'monotonic', lambda: scope.clock),
                         patch.object(module.time, 'sleep', sleep)]
        for item in scope.patches:
            item.start()
            self.addCleanup(item.stop)
        return scope

    def test_loader_partial_copy_and_bad_length_retry_to_complete_inventory(self):
        scope = self.make_scope([299, 24, 0])
        result = scope.modules(scope.observed)
        self.assertEqual(result, dict(inspected_module_count=0, connection_modules=[]))
        self.assertEqual(scope.observed['module_snapshot_observation']['transient_errors'], [299,24])
        self.assertEqual(scope.calls, 3)
        self.assertEqual(scope.closed, [77])

    def test_access_denied_is_not_retried_or_accepted(self):
        scope = self.make_scope([5, 0])
        with self.assertRaisesRegex(RuntimeError, 'module_snapshot_win32_error_5'):
            scope.modules(scope.observed)
        self.assertEqual(scope.calls, 1)
        self.assertEqual(scope.observed['module_snapshot_observation']['transient_errors'], [5])

    def test_retry_stops_at_original_execution_deadline(self):
        scope = self.make_scope([299]*200)
        scope.execution_deadline = 10.12
        with self.assertRaisesRegex(RuntimeError, 'module_snapshot_readiness_deadline'):
            scope.modules(scope.observed)
        self.assertLessEqual(scope.calls, 3)
        self.assertAlmostEqual(scope.clock, 10.12)

    def test_retry_has_five_second_total_bound(self):
        scope = self.make_scope([24]*200)
        with self.assertRaisesRegex(RuntimeError, 'module_snapshot_readiness_(deadline|attempt_bound)'):
            scope.modules(scope.observed)
        self.assertLessEqual(scope.calls, 101)
        self.assertLessEqual(scope.clock, 15.0)

    def test_stopped_held_process_does_not_snapshot_reused_pid(self):
        scope = self.make_scope([0])
        scope.alive = False
        self.assertEqual(scope.modules(scope.observed), dict(process_exited_before_module_snapshot=True))
        self.assertEqual(scope.calls, 0)

    def test_process_exit_during_failed_snapshot_is_not_unknown_inventory(self):
        scope = self.make_scope([299])
        original = scope.snapshot_api
        def snapshot(*args):
            result = original(*args)
            scope.alive = False
            return result
        scope.snapshot_api = snapshot
        self.assertEqual(scope.modules(scope.observed), dict(process_exited_before_module_snapshot=True))
        self.assertEqual(scope.observed['module_snapshot_observation']['transient_errors'], [299])

    def test_lost_exact_job_membership_is_rejected_before_snapshot(self):
        scope = self.make_scope([0])
        scope.exact = False
        with self.assertRaisesRegex(RuntimeError, 'module_target_left_exact_owned_job'):
            scope.modules(scope.observed)
        self.assertEqual(scope.calls, 0)

    def test_incomplete_module_enumeration_is_never_accepted_and_handle_closes(self):
        scope = self.make_scope([0])
        scope.enumeration_error = 5
        with self.assertRaisesRegex(RuntimeError, 'complete_data_consumer_module_inventory_win32_5'):
            scope.modules(scope.observed)
        self.assertEqual(scope.closed, [77])

    def test_native_wait_failure_is_not_liveness(self):
        scope = self.make_scope([0])
        scope.job.wait = lambda *args: 0xffffffff
        with self.assertRaisesRegex(RuntimeError, 'module_target_wait_failed'):
            scope.modules(scope.observed)
        self.assertEqual(scope.calls, 0)

    def test_successful_snapshot_crossing_deadline_is_closed_and_rejected(self):
        scope = self.make_scope([0])
        original = scope.snapshot_api
        def snapshot(*args):
            result = original(*args)
            scope.clock = 60.1
            return result
        scope.snapshot_api = snapshot
        with self.assertRaisesRegex(RuntimeError, 'module_snapshot_readiness_deadline'):
            scope.modules(scope.observed)
        self.assertEqual(scope.closed, [77])

    def test_successful_enumeration_crossing_deadline_is_closed_and_rejected(self):
        scope = self.make_scope([0])
        original = scope.module_first
        def first(*args):
            result = original(*args)
            scope.clock = 60.1
            return result
        scope.module_first = first
        with self.assertRaisesRegex(RuntimeError, 'module_snapshot_readiness_deadline'):
            scope.modules(scope.observed)
        self.assertEqual(scope.closed, [77])

    def test_snapshot_success_after_original_deadline_is_not_accepted(self):
        scope = self.make_scope([0])
        scope.execution_deadline = 10.0
        with self.assertRaisesRegex(RuntimeError, 'module_snapshot_readiness_deadline'):
            scope.modules(scope.observed)
        self.assertEqual(scope.calls, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
