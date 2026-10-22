"""N1/N2 + fixed local owner-child controls. All APIs/process/files are fake."""
import copy
import datetime as dt
import hashlib
import io
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import windows_powerbi_contract as contract
import windows_powerbi_owner as owner
import test_windows_powerbi_owner as baseline_tests
from test_windows_powerbi_owner import MemoryContext,example_plan,message,safe_process


class DeadlineSuccessControls(unittest.TestCase):
    def run_advanced(self,**kwargs):
        clock=dict(seconds=0)
        origin=dt.datetime(2026,10,22,12,tzinfo=dt.timezone.utc)
        with patch.object(owner,'now',lambda:origin+dt.timedelta(seconds=clock['seconds'])), \
             patch.object(owner.time,'monotonic',lambda:1000+clock['seconds']):
            return baseline_tests.PhaseControls().run_owner(clock=clock,**kwargs)

    def test_successful_last_process_verifier_returning_after_execution_is_failure_but_readers_safe(self):
        ctx,code,scopes,_=self.run_advanced(advance_finish=2)
        self.assertEqual(len(scopes),3)
        self.assertEqual(code,72)
        result=ctx.files['terminal.json']
        self.assertFalse(result['execution_complete'])
        self.assertTrue(result['readers_known'])
        self.assertTrue(result['native_cleanup_complete'])
        self.assertIsNone(result['exit'])

    def test_successful_finish_control_returning_late_cannot_complete_execution(self):
        ctx,code,_,_=self.run_advanced(advance_control='finish')
        self.assertEqual(code,72)
        self.assertFalse(ctx.files['terminal.json']['execution_complete'])
        self.assertTrue(ctx.files['terminal.json']['native_cleanup_complete'])

    def test_successful_candidate_publication_returning_late_has_pending_receipt_and_policy_failure(self):
        ctx,code,_,_=self.run_advanced(advance_record='terminal.json')
        self.assertEqual(code,72)
        candidate=ctx.files['terminal.json']
        self.assertTrue(candidate['execution_complete'])
        self.assertTrue(candidate['native_cleanup_complete'])
        self.assertEqual(candidate['candidate_exit'],0)
        self.assertIsNone(candidate['exit'])
        self.assertEqual(candidate['acceptance_status'],'PENDING_POSTPUBLICATION_VALIDATION_AND_ACTUAL_OWNER_EXIT')
        seal=ctx.files['terminal-validation.json']
        self.assertFalse(seal['terminal_publication_within_original_cleanup_end'])
        self.assertEqual(seal['policy_exit_before_validation_publication'],72)

    def test_successful_validation_publication_returning_late_requires_actual_nonzero_exit(self):
        ctx,code,_,_=self.run_advanced(advance_record='terminal-validation.json')
        self.assertEqual(code,72)
        self.assertEqual(ctx.files['terminal-validation.json']['policy_exit_before_validation_publication'],0)
        self.assertEqual(ctx.files['terminal-validation.json']['acceptance_status'],'PENDING_VALIDATION_PUBLICATION_AND_ACTUAL_OWNER_EXIT')
        self.assertTrue(ctx.files['terminal.json']['readers_known'])

    def test_successful_ack_read_finishing_after_its_bound_is_rejected(self):
        ctx=MemoryContext();a=ctx.a
        req=contract.record_base(a,ctx.plan,ctx.plan_sha,'sample-connected',0)
        ack=dict(req,status='ACK',request_sha256='4'*64,evidence=[dict(path='C:/proof.json',sha256='5'*64)])
        clock=dict(value=1)
        def read(*args,**kwargs):
            clock['value']=11
            return owner.json_bytes(ack)
        with patch.object(Path,'exists',lambda path:True),patch.object(owner,'check_abort'), \
             patch.object(owner.time,'monotonic',lambda:clock['value']):
            ctx.read=read
            with self.assertRaisesRegex(RuntimeError,'successful_ack_verification'):
                owner.wait_ack(ctx,a,'sample-connected','4'*64,req,10)

    def test_handle_close_success_crossing_cleanup_deadline_cannot_return_zero(self):
        origin=owner.now();clock=dict(seconds=0)
        context=SimpleNamespace(return_cleanup_end_utc=(origin+dt.timedelta(seconds=1)).isoformat(),
                                return_cleanup_end_monotonic=owner.time.monotonic()+1,
                                close=lambda:clock.update(seconds=2))
        with patch.object(owner,'Context',return_value=context),patch.object(owner,'run_mutex_mode',return_value=0), \
             patch.object(owner,'now',lambda:origin+dt.timedelta(seconds=clock['seconds'])):
            self.assertEqual(owner.main(['--run','--plan','C:/fake.json','--plan-sha256','a'*64]),72)

    def test_backwards_wall_clock_during_handle_close_cannot_extend_cleanup(self):
        origin=owner.now();clock=dict(seconds=0)
        context=SimpleNamespace(return_cleanup_end_utc=(origin+dt.timedelta(seconds=900)).isoformat(),
            return_cleanup_end_monotonic=1001, close=lambda:clock.update(seconds=2))
        with patch.object(owner,'Context',return_value=context),patch.object(owner,'run_mutex_mode',return_value=0), \
             patch.object(owner.time,'monotonic',lambda:1000+clock['seconds']), \
             patch.object(owner,'now',lambda:origin-dt.timedelta(seconds=clock['seconds'])):
            self.assertEqual(owner.main(['--run','--plan','C:/fake.json','--plan-sha256','a'*64]),72)

    def test_original_monotonic_cleanup_ceiling_survives_backwards_wall_clock(self):
        origin=owner.now();clock=dict(seconds=0)
        authority=dict(execution_end_utc=(origin+dt.timedelta(seconds=600)).isoformat(),
                       cleanup_end_utc=(origin+dt.timedelta(seconds=900)).isoformat())
        ctx=SimpleNamespace()
        end=owner.anchor_limits(ctx,authority,origin,1000)
        self.assertEqual(1600,end)
        with patch.object(owner.time,'monotonic',lambda:1000+clock['seconds']), \
             patch.object(owner,'now',lambda:origin-dt.timedelta(seconds=clock['seconds'])):
            self.assertTrue(owner.cleanup_in_time(ctx))
            clock['seconds']=901
            self.assertFalse(owner.cleanup_in_time(ctx))


class LocalOwnerProcessControls(unittest.TestCase):
    def context(self,*,child_exit=0,policy_exit=None,tree_safe=True,timed_out=False,missing_seal=False,
                publication_late=False):
        ctx=MemoryContext();ctx.plan_path=Path('C:/synthetic-public-plan.json')
        ctx.close=lambda:None
        ctx.payload.closed=False
        ctx.payload.close=lambda:setattr(ctx.payload,'closed',True)
        origin=owner.now();clock=dict(seconds=0)
        ctx.a['cleanup_end_utc']=(origin+dt.timedelta(seconds=900)).isoformat()
        calls=[]
        def job(command,output,receipt,timeout):
            calls.append((command,timeout))
            self.assertTrue(ctx.payload.closed)
            self.assertEqual(command,[str(contract.PYTHON),'-B',str(Path(owner.__file__).resolve()),
                '--plan',str(ctx.plan_path),'--plan-sha256',ctx.plan_sha,'--run'])
            self.assertLessEqual(timeout,2100)
            self.assertGreater(timeout,0)
            receipt.update(processExit=child_exit,treeEmpty=tree_safe,jobActiveProcesses=0 if tree_safe else 1,
                           heldProcessHandlesStopped=tree_safe,timedOut=timed_out)
            if not missing_seal:
                candidate=dict(schema_version=1,run_id=ctx.a['run_id'],nonce=ctx.a['nonce'],
                    source_head=ctx.plan['source_head'],plan_sha256=ctx.plan_sha,
                    original_execution_end_utc=ctx.a['execution_end_utc'],original_cleanup_end_utc=ctx.a['cleanup_end_utc'],
                    acceptance_status='PENDING_POSTPUBLICATION_VALIDATION_AND_ACTUAL_OWNER_EXIT',exit=None,
                    candidate_exit=0 if child_exit==0 else 72,execution_complete=child_exit==0,
                    readers_known=True,native_cleanup_complete=True,phases=[dict(phase=0,process=safe_process())])
                pin=ctx.record('terminal.json',candidate)
                ctx.record('terminal-validation.json',dict(**contract.record_base(ctx.a,ctx.plan,ctx.plan_sha,'terminal-validation',2),
                    terminal_candidate_sha256=pin,candidate_exit=candidate['candidate_exit'],
                    acceptance_status='PENDING_VALIDATION_PUBLICATION_AND_ACTUAL_OWNER_EXIT',
                    policy_exit_before_validation_publication=candidate['candidate_exit'],
                    terminal_publication_within_original_cleanup_end=True))
            return child_exit if policy_exit is None else policy_exit
        ctx.job.run=job
        if publication_late:
            original=ctx.record
            def late_record(name,value):
                result=original(name,value)
                if name=='owner-process.json':
                    clock['seconds']=901
                return result
            ctx.record=late_record
        return ctx,calls,origin,clock

    def run_launcher(self,**kwargs):
        ctx,calls,origin,clock=self.context(**kwargs)
        with patch.object(owner,'assert_desktop'),patch.object(owner,'nested_job_platform'), \
             patch.object(Path,'open',lambda *args,**kwargs:io.BytesIO()), \
             patch.object(owner,'now',lambda:origin+dt.timedelta(seconds=clock['seconds'])):
            code=owner.launch_local(ctx)
        return ctx,code,calls

    def test_fixed_local_owner_success_has_actual_process_and_policy_exit_proof(self):
        ctx,code,calls=self.run_launcher()
        self.assertEqual(code,0);self.assertEqual(len(calls),1)
        result=ctx.files['owner-process.json']
        self.assertEqual(result['actual_child_policy_exit'],0)
        self.assertEqual(result['actual_child_process_exit'],0)
        self.assertTrue(result['readers_known'])
        self.assertEqual(len(result['child_receipts']),2)
        self.assertTrue(result['owner_job']['heldProcessHandlesStopped'])

    def test_actual_owner_failure_keeps_physical_reader_cleanup_independent(self):
        ctx,code,_=self.run_launcher(child_exit=72)
        self.assertEqual(code,72)
        result=ctx.files['owner-process.json']
        self.assertFalse(result['execution_accepted_before_owner_receipt_publication'])
        self.assertTrue(result['readers_known'])
        self.assertTrue(result['native_cleanup_complete'])

    def test_job_policy_timeout_cannot_be_hidden_by_real_process_exit_zero(self):
        ctx,code,_=self.run_launcher(child_exit=0,policy_exit=124,timed_out=True)
        self.assertEqual(code,72)
        result=ctx.files['owner-process.json']
        self.assertEqual(result['actual_child_policy_exit'],124)
        self.assertEqual(result['actual_child_process_exit'],0)
        self.assertFalse(result['execution_accepted_before_owner_receipt_publication'])

    def test_unknown_job_tree_never_proves_external_app_readers_stopped(self):
        ctx,code,_=self.run_launcher(tree_safe=False)
        self.assertEqual(code,72)
        self.assertFalse(ctx.files['owner-process.json']['readers_known'])
        self.assertFalse(ctx.files['owner-process.json']['native_cleanup_complete'])

    def test_missing_terminal_seal_does_not_adopt_process_exit_zero(self):
        ctx,code,_=self.run_launcher(missing_seal=True)
        self.assertEqual(code,72)
        self.assertFalse(ctx.files['owner-process.json']['readers_known'])

    def test_successful_owner_receipt_publication_returning_late_fails_wrapper(self):
        ctx,code,_=self.run_launcher(publication_late=True)
        self.assertEqual(code,72)
        self.assertEqual(ctx.files['owner-process.json']['actual_child_process_exit'],0)
        self.assertTrue(ctx.files['owner-process.json']['readers_known'])

    def test_owner_publication_with_backwards_clock_still_fails_original_monotonic_ceiling(self):
        ctx,calls,origin,clock=self.context(publication_late=True)
        with patch.object(owner,'assert_desktop'),patch.object(owner,'nested_job_platform'), \
             patch.object(Path,'open',lambda *args,**kwargs:io.BytesIO()), \
             patch.object(owner.time,'monotonic',lambda:1000+clock['seconds']), \
             patch.object(owner,'now',lambda:origin-dt.timedelta(seconds=clock['seconds'])):
            self.assertEqual(owner.launch_local(ctx),72)
        self.assertTrue(ctx.files['owner-process.json']['readers_known'])
        self.assertEqual(0,ctx.files['owner-process.json']['actual_child_process_exit'])

    def test_nested_job_platform_has_no_unsafe_old_platform_fallback(self):
        with patch.object(owner.sys,'platform','win32'), \
             patch.object(owner.sys,'getwindowsversion',lambda:SimpleNamespace(major=6),create=True):
            with self.assertRaisesRegex(RuntimeError,'nested_job_windows10'):
                owner.nested_job_platform()


class NativeHeldFileInterfaceControls(unittest.TestCase):
    @unittest.skipUnless(sys.platform == 'win32', 'actual canonical HeldFile needs Windows')
    def test_context_message_uses_actual_canonical_held_file_identity(self):
        # Actual canonical file read/identity, using a public synthetic file.
        # This tests the shared API, not private ACL custody or app execution.
        from tools.development import windows_live_backend as native
        ctx=MemoryContext()
        with tempfile.TemporaryDirectory(prefix='odbcpp-public-held-interface-') as directory:
            ctx.run=Path(directory)
            path=ctx.run/'test-input.json'
            path.write_bytes(owner.json_bytes(dict(authority=ctx.a,connection=ctx.c)))
            ctx.native=SimpleNamespace(HeldFile=lambda value,private: native.HeldFile(value,private=False))
            ctx.hold_run=lambda:None
            actual=native.HeldFile(path,private=False)
            try:
                identity=actual.identity
            finally:
                actual.close()
            ctx.files['staged.json']=dict(input_identity=list(identity),nonce=ctx.a['nonce'],
                plan_sha256=ctx.plan_sha,source_head=ctx.plan['source_head'])
            try:
                payload,a,c=owner.Context.message(ctx)
                self.assertEqual(identity,payload.identity)
                self.assertEqual(ctx.a,a)
                self.assertEqual(ctx.c,c)
            finally:
                for held in ctx.holds:
                    held.close()


class AwsEffectiveRecipeControls(unittest.TestCase):
    def context(self):
        ctx=MemoryContext();ctx.plan=example_plan('aws2');ctx.run=contract.BASE/ctx.plan['run_id']
        ctx.a=message(ctx.plan)['authority']
        ctx.files['app-connection.txt']=owner.aws_connection_recipe(ctx)
        read_json=ctx.read
        def read(path,**kwargs):
            value=ctx.files.get(Path(path).name)
            return value if type(value) is bytes else read_json(path,**kwargs)
        ctx.read=read
        return ctx

    def attestation(self,ctx):
        return dict(contract.record_base(ctx.a,ctx.plan,ctx.plan_sha,'effective-recipe-applied',0),
            recipe_sha256=hashlib.sha256(owner.aws_connection_recipe(ctx)).hexdigest(),
            evidence=[dict(path='C:/UI-proof.json',sha256='a'*64)])

    def test_recipe_is_exact_effective_string_without_persisted_identity_or_password(self):
        ctx=self.context()
        expected=('DSN=ODBCPP_AWS2_COMPARE007;LogLevel=2;LogPath={'+str(ctx.run/'vendor-logs')+'};\n').encode()
        self.assertEqual(owner.aws_connection_recipe(ctx),expected)
        self.assertNotIn(b'PWD',expected);self.assertNotIn(b'UID',expected)
        self.assertNotIn(b'ErrorLogLevel',expected)

    def test_per_dsn_settings_do_not_prove_effective_application_or_logging(self):
        ctx=self.context();meta=dict(recipe_file=dict(path=str(ctx.run/'app-connection.txt'),sha256='b'*64))
        with patch.object(Path,'exists',lambda path:False):
            result=owner.effective_vendor_diagnostics(ctx,ctx.a,meta,[])
        self.assertEqual(result['recipe_application'],'NOT_OBSERVED')
        self.assertEqual(result['log_artifacts_status'],'NOT_OBSERVED')
        self.assertTrue(result['diagnostics_incomplete'])

    def test_operator_application_without_actual_nonempty_logs_stays_incomplete(self):
        ctx=self.context();ctx.files['effective-recipe-applied.control.json']=self.attestation(ctx)
        with patch.object(Path,'exists',lambda path:Path(path).name in ctx.files):
            result=owner.effective_vendor_diagnostics(ctx,ctx.a,dict(recipe_file={}),
                [dict(path=str(ctx.run/'vendor-logs'/'empty.log'),bytes=0,sha256='c'*64)])
        self.assertEqual(result['recipe_application'],'OPERATOR_REPORTED_APPLIED')
        self.assertEqual(result['log_artifacts_status'],'NOT_OBSERVED')
        self.assertTrue(result['diagnostics_incomplete'])

    def test_actual_artifact_metadata_plus_operator_report_is_not_native_feature_or_loglevel_proof(self):
        ctx=self.context();ctx.files['effective-recipe-applied.control.json']=self.attestation(ctx)
        with patch.object(Path,'exists',lambda path:Path(path).name in ctx.files):
            result=owner.effective_vendor_diagnostics(ctx,ctx.a,dict(recipe_file={}),
                [dict(path=str(ctx.run/'vendor-logs'/'vendor.log'),bytes=27,sha256='c'*64)])
        self.assertEqual(result['log_artifacts_status'],'OBSERVED_BOUNDED_NONEMPTY_FILES')
        self.assertFalse(result['diagnostics_incomplete'])
        self.assertFalse(result['effective_log_level_verified_by_native'])
        self.assertFalse(result['feature_or_logcoverage_PASS_claimed'])

    def test_recipe_attestation_rejects_changed_recipe_or_missing_evidence(self):
        ctx=self.context();value=self.attestation(ctx)
        ctx.files['app-connection.txt']=b'DSN=other;'
        with self.assertRaisesRegex(RuntimeError,'held_effective_recipe_bytes'):
            owner.verify_recipe_attestation(ctx,ctx.a,value)
        value['evidence']=[]
        with self.assertRaisesRegex(RuntimeError,'evidence_bound'):
            owner.verify_recipe_attestation(ctx,ctx.a,value)


if __name__=='__main__':
    unittest.main(verbosity=2)
