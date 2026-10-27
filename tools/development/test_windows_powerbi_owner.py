"""Pure safety controls: fake process/provider/files, no Windows resource calls."""
import copy
import datetime as dt
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import windows_powerbi_contract as contract
import windows_powerbi_owner as owner
import windows_powerbi_scope as observer
import windows_powerbi_provider as provider


def example_plan(kind='own'):
    root = contract.OWN_ROOT if kind == 'own' else contract.AWS_ROOT/'Drivers'
    image = root/('odbcpp.dll' if kind == 'own' else 'reviewed-vendor.dll')
    return dict(schema_version=1, scope='redshift-powerbi007', run_id='powerbi007-'+kind+'-001',
        source_head='a'*40, checkout='C:/Users/vahid/.codex/worktrees/example/odbcpp', workload_sha256='b'*64,
        application=dict(path=str(contract.APP),sha256=contract.APP_SHA), ca=dict(path=str(contract.CA),sha256=contract.CA_SHA),
        driver=dict(kind=kind,description='ODBCPP Redshift' if kind=='own' else 'Amazon Redshift ODBC Driver (x64)',
            path=str(image),sha256='c'*64,install_receipt=dict(path='C:/proof/install.json',sha256='d'*64),
            inventory=dict(path=str(contract.OWN_ROOT/'inventory.json'),sha256='e'*64) if kind=='own' else None,
            payload=[dict(path=str(image),sha256='c'*64)],
            dsn='ODBCPP_PBI_RS_20261021_003' if kind=='own' else 'ODBCPP_AWS2_COMPARE007'),
        modules={name:'f'*64 for name in contract.MODULES},canonical={name:'f'*64 for name in contract.CANONICAL},
        tools={name:dict(path=str(path),sha256='f'*64) for name,path in (('git',contract.GIT),('powershell',contract.PS),('python',contract.PYTHON))},
        max_execution_seconds=1800,max_cleanup_seconds=300,max_launches=3,max_capture_per_phase=12,
        trace=dict(kind='own-driver' if kind=='own' else 'vendor-log-only',maximum_bytes=16*1024*1024,polling_seconds=1),
        independent_review='ACCEPT')


def message(plan=None):
    plan = plan or example_plan()
    now = owner.now()
    return dict(authority=dict(schema_version=1,state='ROOT_ADMITTED',independent_review='ACCEPT',scope=plan['scope'],
        source_head=plan['source_head'],run_id=plan['run_id'],nonce='OFFLINE_NOT_ADMITTED_001',plan_sha256='1'*64,
        execute_before_utc=(now+dt.timedelta(seconds=30)).isoformat(),
        execution_end_utc=(now+dt.timedelta(seconds=600)).isoformat(),cleanup_end_utc=(now+dt.timedelta(seconds=900)).isoformat(),
        original_cap=2,cap_observed_utc=now.isoformat(),cap_observation=dict(path='C:/proof/cap.json',sha256='2'*64)),
        connection=dict(SERVER='pilot.abc.eu-north-1.redshift.amazonaws.com',PORT='5439',DATABASE='odbcpp_pilot',
                        UID='odbcpp_pilot_test',SSLCAFILE=str(contract.CA)))


def safe_process():
    return dict(readers_known=True,receipt=dict(treeEmpty=True,jobActiveProcesses=0,heldProcessHandlesStopped=True),
        held_observed_processes=[dict(stopped=True)],returned_exit=0,error_type=None)


class Contracts(unittest.TestCase):
    def setUp(self):
        self.plan=example_plan();self.message=message(self.plan)

    def valid(self):
        return contract.validate_message(self.message,self.plan,'1'*64,owner.now())

    def test_closed_plan_allows_both_fixed_driver_kinds(self):
        for kind in ('own','aws2'):
            self.assertEqual(contract.validate_plan(example_plan(kind))['driver']['kind'],kind)

    def test_public_plan_has_no_build_time_utc_horizon(self):
        self.plan['execution_end_utc']=owner.now().isoformat()
        with self.assertRaisesRegex(RuntimeError,'native_plan_schema'):
            contract.validate_plan(self.plan)

    def test_arbitrary_app_and_command_are_refused(self):
        self.plan['application']['path']='C:/arbitrary.exe'
        with self.assertRaises(RuntimeError):
            contract.validate_plan(self.plan)
        self.message['authority']['command']=['anything']
        with self.assertRaises(RuntimeError):
            self.valid()

    def test_arbitrary_package_path_is_refused(self):
        self.plan['driver']['path']='C:/foreign.dll'
        with self.assertRaisesRegex(RuntimeError,'fixed_installed_driver_namespace'):
            contract.validate_plan(self.plan)

    def test_vendor_does_not_gain_own_caps_or_weak_tls_policy(self):
        plan=example_plan('aws2')
        ctx=SimpleNamespace(plan=plan,run=contract.BASE/plan['run_id'],limits={'MAXRESULTS':1})
        fields=owner.fields(ctx,self.message['connection'])
        self.assertEqual(fields['SSLMode'],'verify-full')
        self.assertEqual(fields['ConnectionRetryCount'],'0')
        self.assertNotIn('MAXRESULTS',fields)
        self.assertNotIn('PWD',fields)

    def test_all_twenty_own_limits_preserved_except_reviewed_app_fanout(self):
        limits={f'LIMIT{i}':i for i in range(19)}|{'MAXRESULTS':1}
        fields=owner.fields(SimpleNamespace(plan=self.plan,run=Path('C:/offline'),limits=limits),self.message['connection'])
        self.assertEqual(fields['MAXRESULTS'],'16')
        self.assertTrue(all(fields[k]==str(v) for k,v in limits.items() if k!='MAXRESULTS'))
        self.assertEqual(fields['LogQueries'],'false')

    def test_password_admin_or_unlimited_cap_refused(self):
        for field,value in [('PWD','invented'),('UID','odbcpp_pilot_admin')]:
            old=copy.deepcopy(self.message)
            self.message['connection'][field]=value
            with self.assertRaises(RuntimeError):
                self.valid()
            self.message=old
        for value in ('UNLIMITED',17,0,True):
            self.message['authority']['original_cap']=value
            with self.assertRaisesRegex(RuntimeError,'finite_baseline'):
                self.valid()

    def test_fresh_observation_and_original_deadlines_required(self):
        self.message['authority']['cap_observed_utc']=(owner.now()-dt.timedelta(seconds=301)).isoformat()
        with self.assertRaisesRegex(RuntimeError,'fresh_actual_baseline_cap'):
            self.valid()
        self.message=message()
        self.message['authority']['execution_end_utc']=(owner.now()+dt.timedelta(seconds=1801)).isoformat()
        self.message['authority']['cleanup_end_utc']=(owner.now()+dt.timedelta(seconds=1900)).isoformat()
        with self.assertRaisesRegex(RuntimeError,'original_execution_horizon'):
            self.valid()

    def test_source_and_plan_binding_cannot_change(self):
        self.message['authority']['plan_sha256']='3'*64
        with self.assertRaisesRegex(RuntimeError,'admission_binding'):
            self.valid()

    def test_exact_existing_serverless_endpoint_preserves_all_other_identity_guards(self):
        self.message['connection']['SERVER']=contract.SERVERLESS_HOST
        self.assertEqual(self.valid()[1]['SERVER'],contract.SERVERLESS_HOST)
        for name,value in (('PORT','5438'),('DATABASE','dev'),('UID','odbcpp_pilot_admin'),
                           ('SSLCAFILE','C:/other.pem')):
            with self.subTest(name=name):
                previous=self.message['connection'][name]
                self.message['connection'][name]=value
                with self.assertRaisesRegex(RuntimeError,'exact_test_identity_tls'):
                    self.valid()
                self.message['connection'][name]=previous

    def test_serverless_neighbor_account_region_suffix_and_alias_are_refused(self):
        for host in (contract.SERVERLESS_HOST.replace('pilot.','other.'),
                     contract.SERVERLESS_HOST.replace('385207570137','385207570138'),
                     contract.SERVERLESS_HOST.replace('eu-north-1','eu-west-1'),
                     contract.SERVERLESS_HOST+'.evil.invalid',
                     contract.SERVERLESS_HOST+'.',contract.SERVERLESS_HOST.upper(),
                     'other.385207570137.eu-north-1.redshift-serverless.amazonaws.com'):
            with self.subTest(host=host):
                self.message['connection']['SERVER']=host
                with self.assertRaisesRegex(RuntimeError,'exact_test_identity_tls'):
                    self.valid()

    def test_duplicate_json_keys_refused(self):
        with self.assertRaisesRegex(RuntimeError,'duplicate_json_key'):
            owner.decode(b'{"nonce":"first","nonce":"second"}')

    def test_safe_reader_gate_requires_job_accounting_and_all_handles(self):
        result=safe_process()
        self.assertTrue(contract.safe_readers(result))
        for key,value in [('treeEmpty',False),('jobActiveProcesses',1),('heldProcessHandlesStopped',False)]:
            bad=copy.deepcopy(result);bad['receipt'][key]=value
            self.assertFalse(contract.safe_readers(bad))
        result['held_observed_processes'][0]['stopped']=False
        self.assertFalse(contract.safe_readers(result))

    def test_cap_ack_requires_exact_baseline_and_reviewed_evidence(self):
        a=self.message['authority'];req=contract.record_base(a,self.plan,'1'*64,'cap-ready',0)
        ack=dict(req,status='ACK',request_sha256='4'*64,evidence=[dict(path='C:/proof/a.json',sha256='5'*64)],
            target_cap=6,baseline_closed_sha256='6'*64,
            zero_sessions=dict(path='C:/proof/zero.json',sha256='7'*64),
            measurement_review=dict(path='C:/proof/measurement.json',sha256='8'*64),
            cap_decision=dict(path='C:/proof/decision.json',sha256='9'*64))
        self.assertEqual(contract.validate_ack(ack,a,self.plan,'1'*64,req,'4'*64,'6'*64)['target_cap'],6)
        ack['baseline_closed_sha256']='0'*64
        with self.assertRaisesRegex(RuntimeError,'closed_cap_ready'):
            contract.validate_ack(ack,a,self.plan,'1'*64,req,'4'*64,'6'*64)
        ack.pop('measurement_review')
        with self.assertRaisesRegex(RuntimeError,'ack_schema'):
            contract.validate_ack(ack,a,self.plan,'1'*64,req,'4'*64,'6'*64)

    def test_sample_ack_cannot_change_phase_nonce_or_request(self):
        a=self.message['authority'];req=contract.record_base(a,self.plan,'1'*64,'sample-connected',0)
        ack=dict(req,status='ACK',request_sha256='4'*64,evidence=[dict(path='C:/proof/a.json',sha256='5'*64)])
        for key,value in [('phase',1),('nonce','OTHER_OFFLINE_NONCE'),('request_sha256','0'*64)]:
            bad=dict(ack,**{key:value})
            with self.assertRaises(RuntimeError):
                contract.validate_ack(bad,a,self.plan,'1'*64,req,'4'*64)

    def test_environment_scrub_and_exact_in_memory_restore(self):
        env={'AWS_PROFILE':'synthetic','ODBCPP_TRACE_SENSITIVE':'1','ODBCINI':'foreign','KEEP':'value'}
        before=dict(env)
        with patch.dict(owner.os.environ,env,clear=True):
            with owner.app_environment(SimpleNamespace(plan=self.plan,run=Path('C:/offline'))):
                self.assertNotIn('AWS_PROFILE',owner.os.environ)
                self.assertNotIn('ODBCINI',owner.os.environ)
                self.assertEqual(owner.os.environ['ODBCPP_TRACE_SENSITIVE'],'0')
                self.assertEqual(owner.os.environ['ODBCPP_TRACE_QUERIES'],'0')
            self.assertEqual(dict(owner.os.environ),before)


class ProviderControls(unittest.TestCase):
    def setUp(self):
        self.records=[]
        self.native=SimpleNamespace(need=contract.need)
        self.dsn=provider.ProviderUserDsn('ODBCPP_PBI_RS_20261021_003',Path('C:/offline'),
            SimpleNamespace(record=lambda path,value:self.records.append((path,value))),backend=self.native,
            native_job=SimpleNamespace(),driver=example_plan()['driver'],canonical_limits={'MAXRESULTS':1})
        self.values=dict(DRIVER=str(contract.OWN_ROOT/'odbcpp.dll'),SERVER='synthetic.invalid',PORT='5439',
            DATABASE='odbcpp_pilot',UID='odbcpp_pilot_test',SSL='1',SSLCAFILE=str(contract.CA),
            TRANSPORTMODE='Sync',LogQueries='false',MAXRESULTS='16')

    def test_missing_driver_or_password_or_alias_refused_before_provider(self):
        self.dsn._call=lambda *args:self.fail('Provider must not be called')
        for patch_value in ({'PWD':'invented'},{'USER':'other'},{'server':'duplicate'},{'DRIVER':'C:/other.dll'}):
            with self.assertRaises(RuntimeError):
                self.dsn.prepare(dict(self.values,**patch_value))

    def test_collision_is_not_adopted(self):
        calls=[]
        self.dsn._call=lambda action,*args:(calls.append(action) or dict(exists=True))
        with self.assertRaisesRegex(RuntimeError,'collision'):
            self.dsn.prepare(self.values)
        self.dsn.cleanup()
        self.assertEqual(calls,['get'])

    def test_missing_create_ack_never_authorizes_cleanup(self):
        calls=[]
        def call(action,*args):
            calls.append(action)
            if action=='get':
                return dict(exists=False)
            raise OSError('inert injected lost ACK')
        self.dsn._call=call
        with self.assertRaises(OSError):
            self.dsn.prepare(self.values)
        with self.assertRaisesRegex(RuntimeError,'not_acknowledged'):
            self.dsn.cleanup()
        self.assertEqual(calls,['get','create'])

    def test_exact_ack_then_changed_value_blocks_delete(self):
        def call(action,fields=None):
            if action=='get' and not self.dsn.created:
                return dict(exists=False)
            return dict(exists=True,name=self.dsn.NAME,driver='ODBCPP Redshift',platform='64-bit',
                        fields=dict(self.dsn.expected,SERVER='foreign.invalid') if self.dsn.created else dict(fields))
        self.dsn._call=call
        self.dsn.prepare(self.values)
        self.assertNotIn('UID',self.dsn.expected)
        with self.assertRaisesRegex(RuntimeError,'changed'):
            self.dsn.cleanup()


class MemoryContext:
    def __init__(self):
        self.plan=example_plan();self.plan_sha='1'*64;self.run=Path('C:/OFFLINE_NOT_CREATED')
        self.repo=Path(self.plan['checkout']);self.a=message()['authority'];self.c=message()['connection']
        self.payload=SimpleNamespace(identity=(1,2),retired=False)
        self.payload.retire=lambda:setattr(self.payload,'retired',True)
        self.payload.close=lambda:None
        self.native=SimpleNamespace(error_location=lambda error:[])
        self.job=SimpleNamespace();self.tree=SimpleNamespace(write=lambda *args,**kwargs:None)
        self.limits={f'LIMIT{i}':i for i in range(19)}|{'MAXRESULTS':1};self.files={}
        self.holds=[]

    def hold(self,item):
        self.holds.append(item)
        return item

    def message(self,check_clock=False):
        return self.payload,self.a,self.c

    def record(self,name,value):
        if name in self.files:
            raise FileExistsError('inert create-new replay refusal')
        self.files[name]=copy.deepcopy(value)
        return hashlib.sha256(owner.json_bytes(value)).hexdigest()

    def read(self,path,**kwargs):
        return owner.json_bytes(self.files[Path(path).name])


class PublisherControls(unittest.TestCase):
    def setUp(self):
        self.ctx=MemoryContext();ctx=self.ctx
        ctx.record('consumed.json',contract.record_base(ctx.a,ctx.plan,ctx.plan_sha,'consumed',0))
        ctx.record('baseline-closed.json',dict(process=safe_process(),normal_close=True))
        self.request=dict(contract.record_base(ctx.a,ctx.plan,ctx.plan_sha,'cap-ready',0),
                          baseline_closed_sha256=hashlib.sha256(ctx.read('baseline-closed.json')).hexdigest())
        self.request_sha=ctx.record('cap-ready.native-request.json',self.request)
        self.evidence=dict(path='/home/vahid/.local/state/odbcpp/qualification007/proof.json',sha256='8'*64)

    def ack(self,stage):
        ctx=self.ctx
        value=dict(contract.record_base(ctx.a,ctx.plan,ctx.plan_sha,stage,0),status='ACK',
                   request_sha256=self.request_sha,evidence=[self.evidence])
        if stage=='reviewed-measurement':
            value['measurement_pin']=self.evidence
        if stage=='cap-ready':
            value.update(target_cap=6,baseline_closed_sha256=self.request['baseline_closed_sha256'],
                zero_sessions=self.evidence,measurement_review=self.evidence,cap_decision=self.evidence)
        return value

    def test_fixed_measurement_and_cap_publish_and_replay_refusal(self):
        owner.publish_ack(self.ctx,owner.json_bytes(self.ack('reviewed-measurement')))
        owner.publish_ack(self.ctx,owner.json_bytes(self.ack('cap-ready')))
        self.assertIn('cap-ready.ack.json',self.ctx.files)
        with self.assertRaises(FileExistsError):
            owner.publish_ack(self.ctx,owner.json_bytes(self.ack('cap-ready')))

    def test_cap_before_measurement_or_unknown_baseline_cannot_publish(self):
        with self.assertRaises(KeyError):
            owner.publish_ack(self.ctx,owner.json_bytes(self.ack('cap-ready')))
        self.ctx.files['baseline-closed.json']['process']['readers_known']=False
        with self.assertRaisesRegex(RuntimeError,'baseline_readers'):
            owner.publish_ack(self.ctx,owner.json_bytes(self.ack('reviewed-measurement')))
        self.assertNotIn('reviewed-measurement.ack.json',self.ctx.files)

    def test_changed_measurement_pin_blocks_cap_ack(self):
        owner.publish_ack(self.ctx,owner.json_bytes(self.ack('reviewed-measurement')))
        ack=self.ack('cap-ready');ack['measurement_review']=dict(self.evidence,sha256='9'*64)
        with self.assertRaisesRegex(RuntimeError,'same_reviewed_measurement'):
            owner.publish_ack(self.ctx,owner.json_bytes(ack))

    def test_abort_data_binds_consumed_no_arbitrary_pid_or_signal(self):
        ctx=self.ctx
        value=dict(contract.record_base(ctx.a,ctx.plan,ctx.plan_sha,'abort',0),status='ACK',
            request_sha256=hashlib.sha256(ctx.read('consumed.json')).hexdigest(),evidence=[self.evidence])
        owner.publish_ack(ctx,owner.json_bytes(value))
        with patch.object(Path,'exists',lambda path:Path(path).name in ctx.files):
            with self.assertRaisesRegex(RuntimeError,'owned_scope_abort'):
                owner.check_abort(ctx,ctx.a)
        value['pid']=123
        with self.assertRaisesRegex(RuntimeError,'ack_schema'):
            owner.publish_ack(ctx,owner.json_bytes(value))

    def test_wrong_nonce_cannot_publish_any_control(self):
        ack=self.ack('reviewed-measurement');ack['nonce']='DIFFERENT_OFFLINE_NONCE'
        with self.assertRaisesRegex(RuntimeError,'control_binding'):
            owner.publish_ack(self.ctx,owner.json_bytes(ack))
        self.assertNotIn('reviewed-measurement.ack.json',self.ctx.files)

    def test_local_controls_are_closed_and_one_use(self):
        ctx=self.ctx
        value=contract.record_base(ctx.a,ctx.plan,ctx.plan_sha,'phase-1-capture01',1)
        owner.publish_control(ctx,owner.json_bytes(value))
        with self.assertRaises(FileExistsError):
            owner.publish_control(ctx,owner.json_bytes(value))
        for name in ('execute','stop-pid','phase-1-capture13'):
            bad=contract.record_base(ctx.a,ctx.plan,ctx.plan_sha,name,1)
            with self.assertRaisesRegex(RuntimeError,'fixed_local_control'):
                owner.publish_control(ctx,owner.json_bytes(bad))

    def test_reopen_cannot_authorize_another_baseline_launch(self):
        ctx=self.ctx
        value=contract.record_base(ctx.a,ctx.plan,ctx.plan_sha,'reopen',0)
        with self.assertRaisesRegex(RuntimeError,'phase2'):
            owner.publish_control(ctx,owner.json_bytes(value))


class PartialStageControls(unittest.TestCase):
    def failed_stage(self,foreign=False):
        ctx=MemoryContext();raw=owner.json_bytes(message(ctx.plan))
        state=dict(present=False,body=b'',retired=False)
        class Held:
            def __init__(self,*args,**kwargs):pass
            def read(self,**kwargs):return state['body']
            def close(self):pass
            def retire(self):state.update(present=False,retired=True)
        def write(path,body,**kwargs):
            self.assertIn('stage-intent.json',ctx.files)
            ctx.tree.pending.append(path)
            state.update(present=True,body=b'foreign' if foreign else body[:17])
            raise OSError('inert partial write')
        ctx.native.HeldFile=Held
        ctx.tree=SimpleNamespace(mkdir=lambda path:None,write=write,pending=[])
        with patch.object(Path,'exists',lambda path:Path(path).name=='test-input.json' and state['present']):
            with self.assertRaises((OSError,RuntimeError)):
                owner.stage(ctx,raw)
        return ctx,state

    def test_known_partial_input_retires_after_failed_stage_no_reader(self):
        ctx,state=self.failed_stage()
        self.assertTrue(state['retired'])
        self.assertTrue(ctx.files['stage-failure.json']['readers_known'])
        self.assertTrue(ctx.files['stage-failure.json']['input_retired'])
        self.assertEqual(ctx.files['stage-failure.json']['launches'],0)

    def test_changed_partial_bytes_never_adopted_or_deleted(self):
        ctx,state=self.failed_stage(foreign=True)
        self.assertFalse(state['retired'])
        self.assertTrue(state['present'])
        self.assertFalse(ctx.files['stage-failure.json']['input_retired'])


class PhaseControls(unittest.TestCase):
    def run_owner(self,*,unknown_phase=None,abort_after_baseline=False,constructor_failure=False,
                  advance_finish=None,advance_control=None,advance_record=None,clock=None):
        ctx=MemoryContext();scopes=[];actions=[]
        if advance_record:
            original_record=ctx.record
            def slow_record(name,value):
                result=original_record(name,value)
                if name==advance_record:
                    clock['seconds']=901
                return result
            ctx.record=slow_record
        class Dsn:
            NAME='ODBCPP_PBI_RS_20261021_003'
            def __init__(self,*args,**kwargs):pass
            def prepare(self,value):actions.append('dsn-create')
            def verify(self):actions.append('dsn-verify')
            def cleanup(self):actions.append('dsn-retire')
        class Scope:
            def __init__(self,*args,**kwargs):
                if constructor_failure:
                    raise RuntimeError('inert foreign preexisting reader')
                self.number=len(scopes);scopes.append(self);self.alive=0
                self.receipt=dict(assignedBeforeExecution=True,childPid=100+self.number)
                self.worker=SimpleNamespace(is_alive=self.is_alive,join=lambda timeout:None)
            def is_alive(self):
                self.alive+=1
                return self.alive==1
            def start(self,remaining):actions.append(('launch',self.number,remaining))
            def capture(self):return dict(actual_calls=0,synthetic=True)
            def finish(self):
                value=safe_process()
                if advance_finish==self.number:
                    clock['seconds']=601
                if self.number==unknown_phase:
                    value['readers_known']=False
                return value
            def abort(self):actions.append(('abort',self.number))
        def control(ctx,a,name,phase):
            if advance_control==name:
                clock['seconds']=601
            return name in ('reopen','finish') or name==contract.SAMPLES[phase]
        def ack(ctx,a,name,request_sha,request,end,baseline_sha=None):
            actions.append(('ack',name))
            if name=='cap-ready':
                self.assertEqual(len(scopes),1)
                self.assertTrue(contract.safe_readers(ctx.files['baseline-closed.json']['process']))
                if abort_after_baseline:
                    raise RuntimeError('inert resource interruption')
            return dict(status='ACK',measurement_review=dict(path='C:/proof/measurement.json',sha256='8'*64),
                        measurement_pin=dict(path='C:/proof/measurement.json',sha256='8'*64))
        with patch.object(owner,'assert_desktop'),patch.object(owner,'hold_runtime'),patch.object(owner,'check_abort'), \
             patch.object(owner,'diagnostic_files',return_value=[]),patch.object(owner,'control',control), \
             patch.object(owner,'wait_ack',ack),patch.object(provider,'ProviderUserDsn',Dsn), \
             patch.object(observer,'PowerBiScope',Scope),patch('builtins.print'):
            code=owner.execute(ctx)
        return ctx,code,scopes,actions

    def test_three_launch_flow_cap_gate_and_no_horizon_reset(self):
        ctx,code,scopes,actions=self.run_owner()
        self.assertEqual(code,0)
        self.assertEqual(len(scopes),3)
        budgets=[a[2] for a in actions if type(a) is tuple and a[0]=='launch']
        self.assertTrue(all(0<b<=480 for b in budgets))
        self.assertEqual(budgets,sorted(budgets,reverse=True))
        self.assertTrue(ctx.payload.retired)
        self.assertFalse(ctx.files['terminal.json']['feature_PASS_claimed'])
        self.assertFalse(ctx.files['terminal.json']['inferred_connection_demand'])
        self.assertEqual(ctx.files['consumed.json']['max_launches'],3)

    def test_unknown_baseline_readers_block_cap_later_launch_and_retirement(self):
        ctx,code,scopes,actions=self.run_owner(unknown_phase=0)
        self.assertEqual(code,72);self.assertEqual(len(scopes),1)
        self.assertNotIn(('ack','cap-ready'),actions)
        self.assertNotIn('dsn-retire',actions)
        self.assertFalse(ctx.payload.retired)
        self.assertFalse(ctx.files['terminal.json']['native_cleanup_complete'])

    def test_resource_interruption_while_closed_fences_next_launch_but_retires_inputs(self):
        ctx,code,scopes,actions=self.run_owner(abort_after_baseline=True)
        self.assertEqual(code,72);self.assertEqual(len(scopes),1)
        self.assertIn('dsn-retire',actions)
        self.assertTrue(ctx.payload.retired)
        self.assertTrue(ctx.files['terminal.json']['native_cleanup_complete'])
        self.assertFalse(ctx.files['terminal.json']['execution_complete'])

    def test_constructor_foreign_reader_refusal_keeps_inputs_fenced(self):
        ctx,code,scopes,actions=self.run_owner(constructor_failure=True)
        self.assertEqual(code,72)
        self.assertEqual(scopes,[])
        self.assertNotIn('dsn-retire',actions)
        self.assertFalse(ctx.payload.retired)
        self.assertFalse(ctx.files['terminal.json']['readers_known'])


if __name__=='__main__':
    unittest.main(verbosity=2)
