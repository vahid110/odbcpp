"""Synthetic composition checks for reuse of the bounded IAM runner."""
from contextlib import ExitStack
from datetime import timedelta
from pathlib import Path
import time
import unittest
from unittest.mock import patch

from tools.redshift import pilot_finite_amendment as ledger
from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_finite_live as launch
from tools.redshift import pilot_iam_credentials as iam
from tools.redshift import pilot_live as live
from tools.redshift import pilot_recovery as r
from tools.redshift import pilot_window_live as windows
from tools.redshift import test_pilot_finite_amendment as accounting_fixture
from tools.redshift import test_pilot_continuation_live as runner_fixture
from tools.redshift.pilot_preflight import Blocked
from tools.redshift.pilot_runtime import bounded_process


class FiniteLiveTests(unittest.TestCase):
    def setUp(self):
        self.accounting=accounting_fixture.FiniteAmendmentTests();self.accounting.setUp()
        self.addCleanup(self.accounting.doCleanups)
        self.runner=runner_fixture.ContinuationCompositionTests();self.runner.setUp()
        self.addCleanup(self.runner.doCleanups)
        self.f=self.accounting.f; self.c=self.runner
        self.c.f=self.f;self.c.directory=self.f.path.parent;self.c.sequence=9
        self.c.accounting=self.accounting.c
        self.c.write('bootstrap-anchor.json',self.f.anchor.record(self.f.now))
        self.c.write('database-config.json',self.c.config.config)
        review=self.accounting.review();review['source_code_digest']=launch.code_digest()
        with self.accounting.session() as session:
            self.f.passed(session.initialize(r.digest(self.accounting.source),review))
        self.state=self.f.path.read_text()
        self.c.request.update(sequence=9,observed_at=self.f.stamp(self.f.now),
            hard_deadline=self.accounting.c.current.hard_deadline,
            state_digest=r.digest(self.state),code_digest=launch.code_digest())
        self.c.request['admission'].update(account_id=self.f.anchor.account_id,
            earliest_billable_start=self.f.anchor.earliest_billable_start,
            observed_at=self.f.stamp(self.f.now))
        self.c.write_request()

    def execute(self,controls=None):
        real=ledger.finite_amendment_session
        def session(path,anchor):return real(path,anchor,clock=lambda:self.f.now)
        def exchange(*args):
            self.c.events.append('exchange')
            return iam.DatabaseCredentials(iam.DB_USER,'synthetic-token',
                self.f.now+timedelta(seconds=900))
        with ExitStack() as stack:
            stack.enter_context(patch.object(windows,'utcnow',side_effect=lambda:self.f.now))
            stack.enter_context(patch.object(ledger,'finite_amendment_session',side_effect=session))
            stack.enter_context(patch.object(windows,'fresh_controls',side_effect=controls))
            stack.enter_context(patch.object(live,'Window',self.c.Window))
            stack.enter_context(patch.object(iam,'acquire',side_effect=exchange))
            stack.enter_context(patch.object(live,'aws',side_effect=AssertionError('cloud forbidden')))
            return launch.execute(self.f.path.parent)

    def test_one_identity_and_iam_inventory_with_cleanup_and_no_replay(self):
        result=self.execute()
        self.assertEqual(result['status'],'passed');self.assertTrue(result['cleanup_verified'])
        self.assertEqual(self.c.events,['reserved','cleanup','exchange',live.IDENTITY,
            windows.PROFILES['iam'],'cleanup'])
        with self.assertRaises(Blocked):self.execute()

    def test_source_changed_during_controls_prevents_any_sql(self):
        def controls(*args):self.f.path.write_bytes(self.f.path.read_bytes()+b' ')
        with self.assertRaisesRegex(Blocked,'continuation_source_changed'):self.execute(controls)
        self.assertEqual(self.c.events,[])

    def test_wrong_code_inventory_and_implicit_migration_rejected(self):
        before=self.f.path.read_bytes()
        for key,value in [('code_digest','0'*64),('profile','scalar'),('sequence',8)]:
            previous=self.c.request[key];self.c.request[key]=value;self.c.write_request()
            with self.assertRaises(Blocked):self.execute()
            self.assertEqual(self.c.events,[]);self.assertEqual(self.f.path.read_bytes(),before)
            self.c.request[key]=previous
        self.c.write_request();self.f.path.write_text(self.accounting.source)
        with self.assertRaises(Blocked):self.execute()
        self.assertEqual(self.f.path.read_text(),self.accounting.source)

    def test_precleanup_failure_prevents_credentials_and_retains_liability(self):
        self.c.failure='cleanup';result=self.execute()
        self.assertEqual(result['reason'],'remote_cleanup_unverified')
        self.assertNotIn('exchange',self.c.events)
        with self.assertRaises(Blocked):self.execute()

    def test_different_stored_code_review_blocks_before_cloud_or_sql(self):
        state=self.f.read();overlay=state[b.OVERLAY]
        overlay['review']['source_code_digest']='0'*64
        overlay['review_digest']=r.digest(b._encode(overlay['review']))
        self.f.write(state)
        self.c.request['state_digest']=r.digest(self.f.path.read_text());self.c.write_request()
        with self.assertRaisesRegex(Blocked,'amendment_code_review_mismatch'):self.execute()
        self.assertEqual(self.c.events,[])

    def test_cleanup_recovery_logs_cannot_block_a_new_window_process(self):
        historical=self.f.path.parent/'bootstrap-9001-sql.log'
        historical.write_text('preserved recovery evidence');historical.chmod(0o600)
        window=live.Window(self.f.path.parent,self.c.config.config,time.monotonic()+10)
        window.seq=9000;window.log_prefix='window-009'
        executable=Path('/usr/bin/true')
        if not executable.exists():executable=Path('/bin/true')
        result=bounded_process([str(executable)],env=live.clean_env(),seconds=5,
            output=window.output('sql'))
        self.assertEqual(result.returncode,0)
        self.assertEqual(historical.read_text(),'preserved recovery evidence')
