"""Synthetic private temporary state only; no AWS or canonical state access."""
import copy
from dataclasses import replace
from datetime import datetime, timedelta, timezone
from decimal import Decimal
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_preflight as r0


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name).resolve()
        os.chmod(self.directory, 0o700)
        self.path = self.directory / 'setup.json'
        self.now = datetime(2026, 10, 2, 12, tzinfo=timezone.utc)
        self.anchor = b.BootstrapAnchor('initial-redshift-pilot', 'initial-20261002', '123456789012',
            '11111111-1111-1111-1111-111111111111', '22222222-2222-2222-2222-222222222222',
            self.stamp(self.now-timedelta(hours=1)), self.stamp(self.now+timedelta(minutes=5)), True)
        self.legacy = dict(account_id=self.anchor.account_id, region='eu-north-1', approved_total_usd=15,
            compute_envelope_usd=10, other_charges_and_tax_reserve_usd=5,
            approval_scope='existing operator allowance', actual_spend_usd_verified=None,
            remaining_allowance_usd_verified=None, reserved_compute_usd=0,
            usage_windows=[], live_testing_enabled=False,
            history=[{'old': 'preserve', 'unknown': None}], extra={'nested': [1, 'secret-canary', None]})
        self.write(self.legacy)

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def stamp(value):
        return value.isoformat().replace('+00:00', 'Z')

    def write(self, state):
        self.path.write_text(b._encode(state))
        os.chmod(self.path, 0o600)

    def read(self):
        return json.loads(self.path.read_text(), parse_float=Decimal)

    def session(self, anchor=None):
        return b.bootstrap_session(self.path, anchor or self.anchor, clock=lambda: self.now)

    def evidence(self):
        return dict(verified=True, observed_at=self.stamp(self.now), anchor=self.anchor.record(self.now),
            base_rpus=4, max_rpus=4, usd_per_rpu_hour='.374', cleanup_bound_seconds=60,
            earliest_start_verified=True, no_other_billable_activity=True, other_tax_bound_verified=True)

    def reserve(self, session, evidence=None, **kwargs):
        return session.reserve(self.evidence() if evidence is None else evidence,
            **(dict(metering_seconds=60, cleanup_seconds=60, other_tax_usd='5') | kwargs))

    def clean(self, session):
        return session.transition('cleaned_pending_billing', dict(verified=True,
            observed_at=self.stamp(self.now), anchor=self.anchor.record(self.now),
            no_active_queries=True, no_active_sessions=True, transactions_closed=True))

    def passed(self, result):
        self.assertEqual(result, {'status': 'pass', 'reasons': [], 'live_enabled': False})

    def blocked(self, result, code=None):
        self.assertEqual(result['status'], 'block', result)
        self.assertFalse(result['live_enabled'])
        self.assertNotIn('secret-canary', str(result))
        if code:
            self.assertEqual(result['reasons'], [code])

    def test_happy_null_preservation_and_cleanup_never_refunds(self):
        with self.session() as session:
            self.passed(session.initialize())
            self.passed(self.reserve(session))
            reserved = copy.deepcopy(self.read()[b.OVERLAY]['window'])
            self.assertEqual(reserved['compute_upper_bound_usd'], '1.68')
            self.assertEqual(reserved['total_upper_bound_usd'], '6.68')
            self.passed(session.transition('active'))
            self.passed(session.transition('uncertain'))
            self.passed(self.clean(session))
            self.blocked(self.reserve(session), 'bootstrap_window_already_used')
        state = self.read()
        self.assertEqual({k: v for k,v in state.items() if k != b.OVERLAY}, self.legacy)
        window = state[b.OVERLAY]['window']
        self.assertEqual(window['phase'], 'cleaned_pending_billing')
        for key in ('compute_upper_bound_usd','total_upper_bound_usd'):
            self.assertEqual(window[key], reserved[key])
        self.assertIsNone(state['actual_spend_usd_verified'])
        self.assertIsNone(state['remaining_allowance_usd_verified'])

    def test_unknown_billing_requires_full_other_tax_envelope(self):
        with self.session() as session:
            self.passed(session.initialize())
            for amount in ('0.01', '4.99', '5.01'):
                self.blocked(self.reserve(session, other_tax_usd=amount), 'invalid_headroom')
            self.assertIsNone(self.read()[b.OVERLAY]['window'])

    def test_at_most_one_window_across_restart_every_phase(self):
        for phase in ('reserved','active','uncertain','cleaned_pending_billing'):
            with self.subTest(phase=phase):
                self.write(self.legacy)
                with self.session() as session:
                    self.passed(session.initialize()); self.passed(self.reserve(session))
                    if phase == 'cleaned_pending_billing': self.passed(self.clean(session))
                    elif phase != 'reserved': self.passed(session.transition(phase))
                before = self.path.read_bytes()
                with self.session() as session:
                    self.blocked(self.reserve(session), 'bootstrap_window_already_used')
                self.assertEqual(before, self.path.read_bytes())

    def test_no_auto_initializer_duplicate_initialize(self):
        with self.session() as session:
            before = self.path.read_bytes()
            self.blocked(self.reserve(session), 'overlay_missing_or_invalid')
            self.assertEqual(before, self.path.read_bytes())
            self.passed(session.initialize()); before = self.path.read_bytes()
            self.blocked(session.initialize(), 'already_initialized')
            self.assertEqual(before, self.path.read_bytes())

    def test_exact_compute_boundary_and_one_cent_over(self):
        # Decimal seconds chosen so ceil cents is exactly 10 vs 10.01.
        for seconds, expected in ((24064, 'pass'), (24065, 'block')):
            self.write(self.legacy)
            self.anchor = replace(self.anchor, earliest_billable_start=self.stamp(self.now-timedelta(seconds=seconds-420)))
            with self.session() as session:
                self.passed(session.initialize())
                result = self.reserve(session)
                self.assertEqual(result['status'], expected)
                if expected == 'pass':
                    w = self.read()[b.OVERLAY]['window']
                    self.assertEqual(w['compute_upper_bound_usd'], '10.00')
                    self.assertEqual(w['total_upper_bound_usd'], '15.00')
                else:
                    self.blocked(result, 'budget_exhausted')
                    self.assertIsNone(self.read()[b.OVERLAY]['window'])

    def test_missing_start_foreign_anchor_caps_account_period(self):
        variants = [replace(self.anchor, earliest_billable_start=None), replace(self.anchor, total_limit_usd='16'),
            replace(self.anchor, compute_limit_usd='11'), replace(self.anchor, account_id='999999999999'),
            replace(self.anchor, period_id='newperiod'), replace(self.anchor, operator_assigned_existing_period=False),
            replace(self.anchor, namespace_id='secret-canary'), replace(self.anchor, hard_deadline='secret-canary')]
        for anchor in variants:
            with self.subTest(anchor=anchor):
                before=self.path.read_bytes()
                with self.session(anchor) as session: self.blocked(session.initialize())
                self.assertEqual(before, self.path.read_bytes())

    def test_existing_usage_or_known_spend_not_reset(self):
        for key, value in [('reserved_compute_usd', '.01'), ('usage_windows', [{}]),
            ('actual_spend_usd_verified', '0'), ('remaining_allowance_usd_verified', '15'),
            ('live_testing_enabled', True), ('approved_total_usd',16), ('region','foreign')]:
            state = copy.deepcopy(self.legacy); state[key] = value; self.write(state)
            before=self.path.read_bytes()
            with self.session() as session: self.blocked(session.initialize())
            self.assertEqual(before,self.path.read_bytes())

    def test_stale_future_or_unverified_evidence_blocks(self):
        with self.session() as session:
            self.passed(session.initialize())
            for key,value in [('observed_at',self.stamp(self.now-timedelta(seconds=301))),
                ('observed_at',self.stamp(self.now+timedelta(seconds=1))), ('verified',False),
                ('base_rpus',True),('max_rpus',8),('usd_per_rpu_hour','secret-canary'),
                ('earliest_start_verified',False),('no_other_billable_activity',False),
                ('other_tax_bound_verified',False),('cleanup_bound_seconds',61)]:
                e=self.evidence(); e[key]=value
                before=self.path.read_bytes(); self.blocked(self.reserve(session,e))
                self.assertEqual(before,self.path.read_bytes())

    def test_invalid_headroom_numeric_and_deadline(self):
        with self.session() as session:
            self.passed(session.initialize())
            for kwargs in [dict(metering_seconds=59),dict(cleanup_seconds=0),dict(other_tax_usd=0),
                dict(other_tax_usd='5.01'),dict(metering_seconds=True),dict(cleanup_seconds=Decimal('NaN')),
                dict(other_tax_usd=Decimal('1e999999')),dict(cleanup_seconds='secret-canary')]:
                self.blocked(self.reserve(session, **kwargs))
            self.now += timedelta(minutes=5)
            self.blocked(self.reserve(session), 'deadline_expired')

    def test_anchor_and_legacy_changes_fail_closed(self):
        with self.session() as session: self.passed(session.initialize())
        for anchor in [replace(self.anchor, workgroup_id='33333333-3333-3333-3333-333333333333'),
                       replace(self.anchor, hard_deadline=self.stamp(self.now+timedelta(minutes=6)))]:
            with self.session(anchor) as session: self.blocked(self.reserve(session), 'overlay_mismatch')
        state=self.read(); state['history'].append({'changed':True}); self.write(state)
        with self.session() as session: self.blocked(self.reserve(session), 'overlay_mismatch')

    def test_verified_cleanup_required_and_transition_not_arbitrary(self):
        with self.session() as session:
            self.passed(session.initialize()); self.passed(self.reserve(session))
            self.blocked(session.transition('reconciled'))
            self.blocked(session.transition('cleaned_pending_billing', {'secret-canary':'secret-canary'}))
            self.passed(session.transition('uncertain'))
            self.blocked(session.transition('active'))
            self.passed(self.clean(session))
            self.blocked(session.transition('active'))

    def test_interrupted_replace_leaves_intent_restart_block(self):
        with self.session() as session:
            self.passed(session.initialize())
            before=self.path.read_bytes()
            with patch.object(b.os,'replace',side_effect=OSError('secret-canary')):
                self.blocked(self.reserve(session), 'bootstrap_unavailable_or_invalid')
            self.assertEqual(before,self.path.read_bytes())
            self.assertTrue((self.directory/b.INTENT).exists())
        with self.session() as session: self.blocked(self.reserve(session),'interrupted_persistence')

    def test_interruption_after_replace_keeps_durable_reservation_and_blocks(self):
        with self.session() as session:
            self.passed(session.initialize())
            real_unlink=os.unlink
            def fail_intent(name,*args,**kwargs):
                if name==b.INTENT: raise OSError('secret-canary')
                return real_unlink(name,*args,**kwargs)
            with patch.object(b.os,'unlink',side_effect=fail_intent): self.blocked(self.reserve(session))
            self.assertEqual(self.read()[b.OVERLAY]['window']['phase'],'reserved')
        with self.session() as session: self.blocked(self.reserve(session),'interrupted_persistence')

    def test_secure_state_file_and_other_ledger_intent(self):
        os.chmod(self.path,0o644)
        with self.session() as session: self.blocked(session.initialize(),'unsafe_private_file')
        os.chmod(self.path,0o600)
        target=self.directory/'other'; target.write_bytes(self.path.read_bytes()); os.chmod(target,0o600)
        self.path.unlink(); self.path.symlink_to(target)
        with self.session() as session: self.blocked(session.initialize())
        self.path.unlink(); self.write(self.legacy)
        (self.directory/'.pilot-ledger.intent').write_text('pending')
        with self.session() as session: self.blocked(session.initialize(),'interrupted_persistence')

    def test_lock_contention_and_borrowed_session_lifetime(self):
        with self.session() as session:
            with self.assertRaises(r0.Blocked) as blocked:
                with self.session(): pass
            self.assertEqual(blocked.exception.code,'lock_contended')
            self.passed(session.initialize())
        self.blocked(session.initialize(),'session_closed')
        with self.assertRaises(OSError): os.fstat(session.fd)

    def test_legacy_decimal_values_semantically_preserved(self):
        self.legacy['extra']['exact']=Decimal('123.12345678901234567890123456789')
        self.write(self.legacy)
        with self.session() as session: self.passed(session.initialize())
        self.assertEqual(self.read()['extra'], self.legacy['extra'])

    def test_expired_deadline_blocks_active_but_allows_uncertain_and_cleanup(self):
        with self.session() as session:
            self.passed(session.initialize()); self.passed(self.reserve(session))
            self.now += timedelta(minutes=5)
            self.blocked(session.transition('active'), 'deadline_expired')
            self.passed(session.transition('uncertain'))
            self.passed(self.clean(session))
            self.blocked(self.reserve(session), 'bootstrap_window_already_used')

    def test_hardlink_state_and_directory_replacement_fail_closed(self):
        os.link(self.path, self.directory/'linked')
        with self.session() as session: self.blocked(session.initialize(),'unsafe_private_file')
        (self.directory/'linked').unlink()
        moved = self.directory.with_name(self.directory.name+'-moved')
        with self.session() as session:
            self.passed(session.initialize())
            self.directory.rename(moved)
            self.directory.mkdir(mode=0o700)
            try:
                self.blocked(self.reserve(session),'directory_identity_changed')
                self.assertFalse(self.path.exists())
            finally:
                self.directory.rmdir(); moved.rename(self.directory)

    def test_malformed_overlay_secret_errors_not_serialized(self):
        with self.session() as session: self.passed(session.initialize())
        state=self.read(); state[b.OVERLAY]['window']={'secret-canary':'secret-canary'}; self.write(state)
        with self.session() as session: self.blocked(self.reserve(session),'invalid_window')


if __name__ == '__main__':
    unittest.main()
