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
from tools.development import redshift_powerbi_relationship as fixture
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
            payload='f\n'
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

    @staticmethod
    def relationship_stage(kind):
        state = fixture.new_state()
        if kind != 'inspect':
            state['anchors'] = [700, 701, 702, 700]
            state['parent'] = {'status': 'ABSENT', 'oid': None}
            state['fact'] = {'status': 'ABSENT', 'oid': None}
            if kind != 'create_parent':
                state['parent'] = {'status': 'OWNED', 'oid': 900}
            if kind in ('verify', 'drop_fact', 'drop_parent'):
                state['fact'] = {'status': 'OWNED', 'oid': 901}
            if kind == 'drop_parent':
                state['fact']['status'] = 'DROPPED'
        return fixture.stage_for(state, kind)

    def prepare(self,ops,end):
        ops.fixture_state={'owned':True}
        if self.scope!='native':
            for kind in ('inspect', 'create_parent', 'create_fact', 'verify'):
                ops._sql(self.relationship_stage(kind), end)
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
                for kind in ('drop_fact', 'drop_parent'):
                    ops._sql(self.relationship_stage(kind), end)
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
               (relationship.plan('serverless'),(15,131,15,146)),
               (app.plan('serverless'),(20,216,20,236)),
               (app_owner.public_plan('serverless'),(21,222,21,243))]
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

    def test_fixture_cost_flows_into_every_mode_and_owner_reserve(self):
        planners = (relationship.plan, app.plan, app_owner.public_plan)
        expected = {'provisioned': ((15,124,15,139), (22,210,22,232), (23,214,23,237)),
                    'serverless': ((15,131,15,146), (20,216,20,236), (21,222,21,243))}
        actual = fixture.public_plan()
        emitted = sum(Envelope.relationship_stage(kind)['statements'] for kind in fixture.KINDS)
        self.assertEqual(emitted + 3 * actual['helpers'],
                         actual['statement_upper_bound_including_operations_wrappers'])
        for mode, targets in expected.items():
            for planner, target in zip(planners, targets):
                with self.subTest(mode=mode, planner=planner.__module__):
                    baseline = planner(mode)
                    lifecycle.validate_fixture_plan(baseline)
                    self.assertEqual(tuple(baseline[key] for key in
                        ('total_helpers','total_SQL','total_logins','total_inclusive')), target)
                    changed = dict(actual, statement_upper_bound_including_operations_wrappers=
                                   actual['statement_upper_bound_including_operations_wrappers'] + 1)
                    with patch.object(fixture, 'public_plan', return_value=changed):
                        inherited = planner(mode)
                    self.assertEqual(inherited['total_SQL'], baseline['total_SQL'] + 1)
                    self.assertEqual(inherited['total_inclusive'], baseline['total_inclusive'] + 1)
                    self.assertEqual(inherited['total_helpers'], baseline['total_helpers'])

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
            self.assertEqual((ops.helpers,ops.statements,ops.logins),(15,131,15))
            self.assertIn(('powerbi_relationship_create_fact',12),ops.charged)
            self.assertIn(('powerbi_relationship_drop_fact',10),ops.charged)
            self.assertIn(('powerbi_relationship_drop_parent',10),ops.charged)
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
            self.assertEqual((ops.helpers,ops.statements,ops.logins),(21,222,21))
            self.assertEqual(len(ops.binding_stages),14)
            self.assertIn(('principal_terminate',33),ops.charged)


if __name__=='__main__':
    unittest.main()
