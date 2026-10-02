"""Offline integration of explicit cumulative admission and corrected launcher."""
from datetime import timedelta
import json
import unittest
from unittest.mock import patch
from tools.redshift import pilot_live as live
from tools.redshift import test_pilot_live as harness
from tools.redshift.pilot_preflight import Blocked

class QualificationTests(unittest.TestCase):
    def setUp(self):
        self.f=harness.LiveCompositionTests();self.f.setUp();self.addCleanup(self.f.doCleanups)
        self.f.execute()
        self.original=json.loads((self.f.directory/'setup.json').read_text())
        anchor=dict(self.f.anchor)
        anchor['hard_deadline']=(self.f.now+timedelta(minutes=4)).isoformat().replace('+00:00','Z')
        self.f.write('qualification-anchor.json',anchor)
        self.f.write('qualification-admission.json',self.f.admission)
        self.f.write('bounded-safety-cleanup-result.json',dict(status='cleanup_verified'))
        self.fail_driver=False;self.fail_pre_cleanup=False;self.steps=[];outer=self
        class Window:
            def __init__(self,directory,config,deadline):
                self.directory=directory
                outer.assertEqual(self.phase(),'reserved')
            def phase(self):
                return json.loads((self.directory/'setup.json').read_text())['bootstrap_overlay']['attempts'][-1]['phase']
            def cleanup(self):
                phase=self.phase();outer.steps.append(('cleanup',phase))
                if outer.fail_pre_cleanup and phase=='reserved':raise Blocked('remote_cleanup_unverified')
            def prepare_fixture(self):outer.assertEqual(self.phase(),'active');outer.steps.append(('fixture',self.phase()))
            def sql(self,text,**kwargs):return '1' if 'POSITION' in text else '15000'
            def driver(self,manifest,cases):
                outer.assertEqual(self.phase(),'active')
                if outer.fail_driver:raise Blocked('driver_baseline_failed')
                return dict(cases=list(cases),passed=len(cases),failed=0)
        self.Window=Window

    def execute(self):
        with patch.object(live,'aws',side_effect=self.f.aws),patch.object(live,'Window',self.Window):
            return live.execute(self.f.directory,qualification=True)

    def test_new_window_preserves_history_and_requires_cleanup_before_activation(self):
        r=self.execute();self.assertEqual(r['status'],'passed');self.assertTrue(r['cleanup_verified'])
        s=json.loads((self.f.directory/'setup.json').read_text());o=s['bootstrap_overlay']
        self.assertEqual(o['original_bootstrap'],self.original['bootstrap_overlay'])
        self.assertIsNone(s['actual_spend_usd_verified'])
        self.assertIsNone(s['remaining_allowance_usd_verified'])
        self.assertGreaterEqual(float(o['total_upper_bound_usd']),float(o['original_bootstrap']['window']['total_upper_bound_usd']))
        self.assertEqual(self.steps,[('cleanup','reserved'),('fixture','active'),('cleanup','active')])
        with self.assertRaisesRegex(Blocked,'qualification_already_attempted'):self.execute()

    def test_failed_baseline_keeps_attempt_and_never_auto_retries(self):
        self.fail_driver=True;r=self.execute()
        self.assertEqual(r['execution_failure'],'driver_baseline_failed')
        self.assertTrue(r['cleanup_verified'])
        with self.assertRaisesRegex(Blocked,'qualification_already_attempted'):self.execute()
        s=json.loads((self.f.directory/'setup.json').read_text())
        self.assertEqual(len(s['bootstrap_overlay']['attempts']),1)
        self.assertEqual(s['bootstrap_overlay']['attempts'][0]['phase'],'cleaned_pending_billing')

    def test_pre_activation_cleanup_failure_never_runs_fixture_or_driver(self):
        self.fail_pre_cleanup=True;r=self.execute()
        self.assertEqual(r['status'],'blocked');self.assertFalse(r['results'])
        self.assertEqual(self.steps,[('cleanup','reserved'),('cleanup','uncertain')])
        with self.assertRaisesRegex(Blocked,'qualification_already_attempted'):self.execute()

    def test_changed_controls_block_before_migration(self):
        self.f.workgroup['workgroup']['maxCapacity']=8
        with self.assertRaisesRegex(Blocked,'workgroup_controls_mismatch'):self.execute()
        self.assertEqual(json.loads((self.f.directory/'setup.json').read_text()),self.original)
        self.assertFalse(self.steps)

if __name__=='__main__':unittest.main()
