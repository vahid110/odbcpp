"""Composed counter proof with mocked SQL I/O and a conservative fixture envelope.

No SQL executes. The established complete-fixture maximum is injected while
real Operations methods, psql wrapper charging, census, lifecycle and recovery
run together. Separate fixture-factory tests derive that envelope from stages.
"""
from contextlib import contextmanager
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.development import redshift_ctest_lifecycle as lifecycle
from tools.development import redshift_ctest_ops as module
from tools.development import redshift_ctest_native_custody as census
from tools.development import redshift_powerbi_partition as relationship
from tools.development import redshift_powerbi_session_ops as app
from tools.development import redshift_powerbi_app_owner as app_owner
from tools.development import redshift_fixture_factory as factory
from tools.development import redshift_session_sql as session_sql
from tools.development.test_redshift_ctest_ops import adapter
from tools.development.redshift_timeout_factory import TimeoutFactory


class CountingOps(module.Operations):
    def __init__(self,directory,maximum=8):
        self.__dict__.update(adapter(directory).__dict__)
        self.resource_kind='serverless';self.keep_cluster_running=False
        self.events=0;self.principals_quiescent=False;self.maximum_cleanup_targets=maximum
        self.powerbi_session_partition=maximum==16
        self.timeout_factory=TimeoutFactory('odbcpp_timeout_gate_accounting',
            expected_principal=None,allow_current_identity=True,resource_kind='serverless')
        self.native.resume_available=lambda end:None
        self.native.verify_idle=lambda end:True
        self.charged=[];self.binding_stages=[];self.fail_stage=None

    @contextmanager
    def lock(self,*args):
        yield

    def verify_source(self,*args):
        pass

    def open_inputs(self,*args):
        pass

    def security_paused(self,*args):
        pass

    def reap(self,*args):
        return True

    def close_inputs(self,*args):
        return True

    def _sql(self,stage,end):
        name=stage['stage'];self.charged.append((name,stage['statements']))
        if 'AS rs_admin_oid' in stage['query']:
            self.binding_stages.append(name)
        payload='1\n'
        if name=='current_identity':
            payload='database,owner,owner_oid,schema_oid,schema_owner_oid\nodbcpp_pilot,odbcpp_pilot_test,701,702,700\n'
        elif name=='principal_zero':
            payload='f\nf\n'
        elif name=='principal_pids':
            payload=''.join(f'{200+i}|odbcpp_pilot_test|odbcpp_pilot|2026-10-24 12:00:00\n'
                            for i in range(self.maximum_cleanup_targets))
        elif name=='principal_terminate':
            payload=''.join(f'{200+i}|t\n' for i in range(self.maximum_cleanup_targets))
        elif name=='native_persistent_census':
            payload=','.join(census.HEADERS)+'\n'
        role=module.ADMIN if stage['role']=='admin' else module.ORDINARY
        self.native.raw=(f'RSD1|odbcpp_pilot|{role}|123\n'+payload).encode()
        if name==self.fail_stage:
            self.native.returncode=1
        return super()._sql(stage,end)

    def sql(self,stage,end):
        # Fixture envelope is injected, not a caller-extensible production path.
        if stage['stage']=='native_persistent_census':
            return self._sql(stage,end)[0]
        return super().sql(stage,end)

    def stage_profiles(self,index,run,state,logging,end):
        p=run/'index.json';p.write_text('{}');return p

    def run_process(self,command,**kwargs):
        if command[0]=='/mock/ctest':
            census.before(self,kwargs['deadline'])
        return SimpleNamespace(returncode=0,timed_out=False,reaped=True,group_absent=True)


class Envelope:
    def __init__(self,scope='native'):
        self.scope=scope

    def plan(self):
        return (module.combined_fixture_plan('serverless') if self.scope=='native'
                else relationship.plan('serverless'))

    def prepare(self,ops,end):
        ops.fixture_state={'owned':True}
        if self.scope!='native':
            for name,body in (('inspect',3),('create_parent',9),('create_fact',10),('verify',8)):
                ops._sql({'stage':'relationship_envelope_'+name,'query':'SELECT 1;\n'*body,
                          'statements':body,'role':'admin'},end)
            return ops.fixture_state
        # 27 helpers / 297 SQL, plus timeout cleanup14 SQL = the existing
        # complete fixture28 / 310 SQL reserve plus one Serverless binding.
        for number in range(27):
            ops._sql({'stage':'fixture_envelope_'+str(number),'query':'SELECT 1;\n'*8,
                      'statements':8,'role':'admin'},end)
        return ops.fixture_state

    def cleanup(self,ops,state,end):
        if state is not None:
            if self.scope!='native':
                for name in ('drop_fact','drop_parent'):
                    ops._sql({'stage':'relationship_envelope_'+name,'query':'SELECT 1;\n'*7,
                              'statements':7,'role':'admin'},end)
            else:
                record=dict(database='odbcpp_pilot',schema='odbcpp_fixture',schema_oid=702,
                    schema_owner_oid=700,table='odbcpp_timeout_gate_accounting',oid=900,
                    owner='odbcpp_pilot_test',owner_oid=701,kind='r',
                    columns=[dict(name='value',type='integer',nullable=False,default=None)],
                    rows=dict(count=1,min=1,max=1),create_ack=True,cleanup_delegated=True,
                    receipts={key:dict(path=str(Path.cwd()/'synthetic'/key),sha256='a'*64)
                        for key in ('creation','identity','shape_rows','authority','applicability','cleanup_delegation')})
                ops._sql(factory.timeout_stage(record,'timeout_cleanup',resource_kind='serverless'),end)
        return {'complete':True}


class Accounting(unittest.TestCase):
    def test_mode_reserves_fit_unchanged_caps_and_include_every_branch(self):
        plans=[(module.combined_fixture_plan('serverless'),(37,372,37,409)),
               (relationship.plan('serverless'),(15,123,15,138)),
               (app.plan('serverless'),(20,208,20,228)),
               (app_owner.public_plan('serverless'),(21,214,21,235))]
        for plan,expected in plans:
            with self.subTest(scope=plan['scope']):
                lifecycle.validate_fixture_plan(plan)
                self.assertEqual(tuple(plan[k] for k in
                    ('total_helpers','total_SQL','total_logins','total_inclusive')),expected)
                self.assertEqual(sum(b['helpers'] for b in plan['branches'].values()),expected[0])
                self.assertEqual(sum(b['SQL'] for b in plan['branches'].values()),expected[1])
                self.assertLessEqual(plan['total_helpers']+1,40)
                self.assertLessEqual(plan['total_inclusive'],450)
        self.assertEqual(len(plans[0][0]['serverless_principal_binding_sites']),7)

    def run_counted(self,directory,ops,scope='native'):
        run=Path(directory)/'run'
        config=lifecycle.Config(run,Path('/mock/source'),'a'*40,Path('/mock/index'),
            Path('/mock/expected'),Path('/mock/cmake'),Path('/mock/ctest'),{})
        ops.directory=run
        with patch.object(lifecycle,'verify_config'):
            return lifecycle.run_once(config,ops,Envelope(scope))

    def test_max8_complete_lifecycle_actual_wrapped_counters_include_final_idle(self):
        with tempfile.TemporaryDirectory() as directory:
            ops=CountingOps(directory)
            result=self.run_counted(directory,ops)
            self.assertTrue(result['lifecycle_cleanup_complete'],result['errors'])
            self.assertEqual((ops.helpers,ops.statements,ops.logins),(37,372,37))
            self.assertEqual(sum(name=='principal_zero' for name,_ in ops.charged),4)
            self.assertIn(('principal_terminate',17),ops.charged)
            self.assertEqual(len(ops.binding_stages),7)
            self.assertIn('timeout_cleanup',ops.binding_stages)
            self.assertFalse(result['paused_verified'])
            self.assertTrue(result['owned_activity_quiescent'])

    def test_late_maximal_cleanup_failure_never_retries_or_claims_idle(self):
        with tempfile.TemporaryDirectory() as directory:
            ops=CountingOps(directory);ops.fail_stage='timeout_cleanup'
            result=self.run_counted(directory,ops)
            self.assertFalse(result['lifecycle_cleanup_complete'])
            self.assertFalse(result['owned_activity_quiescent'])
            self.assertTrue(ops.sql_fenced)
            self.assertEqual(sum(name=='timeout_cleanup' for name,_ in ops.charged),1)
            self.assertLessEqual(ops.helpers+1,40)
            self.assertLessEqual(ops.statements+ops.logins,450)

    def test_relationship_composed_counters_include_both_native_censuses(self):
        with tempfile.TemporaryDirectory() as directory:
            ops=CountingOps(directory)
            result=self.run_counted(directory,ops,'powerbi_relationship')
            self.assertTrue(result['lifecycle_cleanup_complete'],result['errors'])
            self.assertEqual((ops.helpers,ops.statements,ops.logins),(15,123,15))
            self.assertEqual(sum(name=='native_persistent_census' for name,_ in ops.charged),2)
            self.assertEqual(len(ops.binding_stages),6)

    def test_app_max16_changed_cap_envelopes_real_outer_counters_and_baseline_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            ops=CountingOps(directory,maximum=16)
            ops.resume_available(300)
            envelope=Envelope('powerbi_relationship')
            state=envelope.prepare(ops,300)
            ops.final_zero(300)  # Closed baseline before any cap change.
            for name,body in (('snapshot',6),('set_cap',13),('sample_connected',6),
                              ('sample_navigator',6),('sample_refresh',6),
                              ('quiesce',6),('restore',13)):
                ops._sql({'stage':'app_envelope_'+name,
                    'query':session_sql.binding(admin_oid=700,test_oid=701)+'SELECT 1;\n'*(body-1),
                    'statements':body,'role':'admin'},300)
            self.assertTrue(ops.cleanup_principals(300)['complete'])
            envelope.cleanup(ops,state,300)
            ops.final_zero(300)
            self.assertTrue(ops.verify_idle(300))
            self.assertEqual((ops.helpers,ops.statements,ops.logins),(21,214,21))
            self.assertEqual(len(ops.binding_stages),14)
            self.assertIn(('principal_terminate',33),ops.charged)


if __name__=='__main__':
    unittest.main()
