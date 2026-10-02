"""R1a synthetic state only; fault injection never touches the canonical pilot."""
import copy
from datetime import timedelta
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import unittest
from unittest.mock import patch

from tools.redshift import pilot_ledger as module
from tools.redshift import pilot_preflight as r0

# Reuse the existing R0 synthetic observations without changing its tests.
spec = importlib.util.spec_from_file_location('r0_test_fixture', Path(__file__).with_name('test_pilot_preflight.py'))
fixture_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture_module)


class LedgerTests(unittest.TestCase):
    def setUp(self):
        self.base = fixture_module.PreflightTests()
        self.base.setUp()
        self.addCleanup(self.base.doCleanups)
        self.now = self.base.now
        self.directory = self.base.directory
        self.path = self.directory/'setup.json'
        identity = self.base.ledger['identity']
        self.binding = module.PilotBinding('pilot-one', 'initial-pilot', **identity)
        self.state = {'schema_version': 1, 'binding': self.binding.record(), 'revision': 0,
                      'ledger': copy.deepcopy(self.base.ledger), 'windows': {}}
        self.write(self.state)

    def write(self, state):
        self.path.write_text(json.dumps(state))
        self.path.chmod(0o600)

    def read(self):
        return json.loads(self.path.read_text())

    def session(self, binding=None):
        return module.ledger_session(self.path, binding or self.binding, clock=lambda: self.now)

    def reserve(self, rid='one'):
        with self.session() as session:
            return session.reserve(rid, self.base.obs, self.base.window)

    def assertBlocked(self, result, code=None):
        self.assertEqual(result['status'], 'block')
        self.assertIs(result['live_enabled'], False)
        if code is not None:
            self.assertEqual(result['reasons'], [code])
        return result

    def cleanup(self, rid='one'):
        return {'binding': self.binding.record(), 'reservation_id': rid, 'verified': True,
                'observed_at': self.now.isoformat().replace('+00:00', 'Z'),
                'no_active_queries': True, 'no_active_sessions': True, 'transactions_closed': True}

    def billing(self, rid='one'):
        stamp = self.now.isoformat().replace('+00:00', 'Z')
        return {'binding': self.binding.record(), 'reservation_id': rid, 'verified': True,
                'observed_at': stamp, 'charges_complete_through': stamp, 'reporting_delay_seconds': 0,
                'actual_total_usd': '0.10', 'actual_compute_usd': '0.09',
                'cumulative_total_usd': '0.10', 'cumulative_compute_usd': '0.09'}

    def cleaned(self):
        self.assertEqual(self.reserve()['status'], 'pass')
        with self.session() as session:
            self.assertEqual(session.mark_active('one')['status'], 'pass')
            self.assertEqual(session.verify_cleanup('one', self.cleanup())['status'], 'pass')

    def test_reserve_persists_before_return_and_blocks_reentry(self):
        result = self.reserve()
        self.assertEqual(result['status'], 'pass')
        self.assertIs(result['live_enabled'], False)
        state = self.read()
        self.assertEqual(state['windows']['one']['phase'], 'reserved')
        self.assertEqual(state['ledger']['reservations'][0]['compute_usd'], '0.20')
        self.assertTrue(state['ledger']['unresolved_cleanup'])
        self.assertBlocked(self.reserve('two'), 'prior_window_pending')
        self.assertBlocked(self.reserve('one'), 'duplicate_reservation')
        self.assertEqual(self.read(), state)

    def test_cleanup_retains_bounds_then_matching_complete_billing_reconciles(self):
        self.cleaned()
        state = self.read()
        self.assertEqual(state['ledger']['reservations'][0]['status'], 'outstanding')
        self.assertEqual(state['ledger']['reconciled_total_usd'], '0')
        self.assertBlocked(self.reserve('two'), 'prior_window_pending')
        with self.session() as session:
            self.assertEqual(session.reconcile('one', self.billing())['status'], 'pass')
        state = self.read()
        self.assertEqual(state['windows']['one']['phase'], 'reconciled')
        self.assertEqual(state['ledger']['remaining_total_usd'], '14.90')
        self.base.obs['billing'].update(total_spend_usd='0.10', compute_spend_usd='0.09')
        self.base.obs['billing']['observed_at'] = state['ledger']['observed_at']
        self.assertEqual(self.reserve('two')['status'], 'pass')
        self.assertEqual(len(self.read()['ledger']['reservations']), 2)

    def test_no_initializer_missing_or_old_schema_remains_unchanged(self):
        self.path.unlink()
        self.assertBlocked(self.reserve())
        self.assertFalse(self.path.exists())
        self.write({'reported': 'unknown', 'remaining': None})
        before = self.path.read_bytes()
        self.assertBlocked(self.reserve(), 'invalid_state')
        self.assertEqual(self.path.read_bytes(), before)

    def test_foreign_period_identity_caps_and_balances(self):
        variants = []
        for section, field, value in [('binding', 'period_id', 'foreign'), ('binding', 'account_id', '999999999999'),
            ('ledger', 'total_limit_usd', '150'), ('ledger', 'remaining_total_usd', '14'),
            ('ledger', 'reconciled_total_usd', None), ('ledger', 'period_id', 'foreign')]:
            state = copy.deepcopy(self.state); state[section][field] = value; variants.append(state)
        for state in variants:
            self.write(state)
            before = self.path.read_bytes()
            self.assertBlocked(self.reserve())
            self.assertEqual(self.path.read_bytes(), before)
        self.write(self.state)
        foreign = module.PilotBinding('other-pilot', 'initial-pilot', **self.base.ledger['identity'])
        with self.session(foreign) as session:
            self.assertBlocked(session.reserve('one', self.base.obs, self.base.window), 'binding_mismatch')

    def test_r0_stale_unknown_controls_and_exhausted_budget(self):
        for section, field, value in [('controls', 'verified', False), ('billing', 'delayed_total_upper_bound_usd', None),
                                       ('identity', 'observed_at', '2026-10-02T11:00:00Z')]:
            before = self.path.read_bytes()
            original = self.base.obs[section][field]; self.base.obs[section][field] = value
            self.assertBlocked(self.reserve())
            self.assertEqual(self.path.read_bytes(), before)
            self.base.obs[section][field] = original
        state = copy.deepcopy(self.state)
        state['ledger'].update(reconciled_total_usd='15', reconciled_compute_usd='10',
                                remaining_total_usd='0', remaining_compute_usd='0')
        self.write(state)
        self.base.obs['billing'].update(total_spend_usd='15', compute_spend_usd='10')
        self.assertBlocked(self.reserve(), 'budget_exhausted')

    def test_uncertain_and_failed_cleanup_never_refund(self):
        self.reserve()
        with self.session() as session:
            self.assertEqual(session.mark_uncertain('one')['status'], 'pass')
            evidence = self.cleanup(); evidence['no_active_queries'] = False
            self.assertBlocked(session.verify_cleanup('one', evidence), 'cleanup_unverified')
            self.assertBlocked(session.reconcile('one', self.billing()), 'cleanup_required')
        self.assertEqual(self.read()['windows']['one']['phase'], 'uncertain')
        self.assertEqual(self.read()['ledger']['reservations'][0]['status'], 'outstanding')
        self.assertBlocked(self.reserve('two'), 'prior_window_pending')

    def test_unknown_partial_foreign_or_over_bound_billing_retains_reservation(self):
        self.cleaned()
        for field, value in [('verified', False), ('actual_total_usd', None), ('reservation_id', 'other'),
            ('reporting_delay_seconds', 60), ('actual_total_usd', '0.22'),
            ('observed_at', '2026-10-02T12:00:01Z'), ('cumulative_compute_usd', '0.20')]:
            evidence = self.billing(); evidence[field] = value
            before = self.path.read_bytes()
            with self.session() as session:
                self.assertBlocked(session.reconcile('one', evidence))
            self.assertEqual(self.path.read_bytes(), before)
        evidence = self.billing(); evidence['binding']['period_id'] = 'foreign'
        with self.session() as session:
            self.assertBlocked(session.reconcile('one', evidence), 'evidence_mismatch')

    def test_submicrosecond_reporting_delay_rounded_conservatively(self):
        self.cleaned()
        evidence = self.billing(); evidence['reporting_delay_seconds'] = '0.000000000001'
        with self.session() as session:
            self.assertBlocked(session.reconcile('one', evidence), 'billing_incomplete')

    def test_billing_timestamp_cannot_go_backwards(self):
        self.cleaned()
        state = self.read(); state['ledger']['observed_at'] = '2026-10-02T12:00:01Z'
        self.write(state); self.now += timedelta(seconds=2)
        evidence = self.billing(); evidence['observed_at'] = '2026-10-02T12:00:00Z'
        evidence['charges_complete_through'] = evidence['observed_at']
        with self.session() as session:
            self.assertBlocked(session.reconcile('one', evidence), 'nonmonotonic_billing_evidence')

    def test_cumulative_over_budget_is_durable_even_when_window_in_bounds(self):
        self.cleaned()
        evidence = self.billing()
        evidence.update(cumulative_total_usd='16', cumulative_compute_usd='11')
        with self.session() as session:
            self.assertBlocked(session.reconcile('one', evidence), 'budget_overrun')
        state = self.read()
        self.assertEqual(state['ledger']['reconciled_total_usd'], '16')
        self.assertEqual(state['ledger']['reconciled_compute_usd'], '11')
        self.assertEqual(state['ledger']['remaining_total_usd'], '0')
        self.assertEqual(state['ledger']['remaining_compute_usd'], '0')
        self.assertEqual(state['windows']['one']['phase'], 'overrun')
        self.assertFalse(state['windows']['one']['overrun']['bound_exceeded'])
        self.assertTrue(state['windows']['one']['overrun']['budget_exceeded'])
        self.assertEqual(state['ledger']['reservations'][0]['status'], 'outstanding')
        self.assertBlocked(self.reserve('two'), 'prior_window_pending')
        self.assertEqual(self.read(), state)

    def test_actual_window_overrun_records_spend_and_restart_cannot_refund(self):
        self.cleaned()
        evidence = self.billing()
        evidence.update(actual_total_usd='0.30', actual_compute_usd='0.25',
                        cumulative_total_usd='0.30', cumulative_compute_usd='0.25')
        with self.session() as session:
            self.assertBlocked(session.reconcile('one', evidence), 'reservation_overrun')
        state = self.read()
        self.assertEqual(state['ledger']['reconciled_total_usd'], '0.30')
        self.assertEqual(state['ledger']['reconciled_compute_usd'], '0.25')
        self.assertEqual(state['windows']['one']['overrun']['actual_total_usd'], '0.30')
        self.assertEqual(state['windows']['one']['overrun']['actual_compute_usd'], '0.25')
        self.assertTrue(state['windows']['one']['overrun']['bound_exceeded'])
        self.assertFalse(state['windows']['one']['overrun']['budget_exceeded'])
        self.assertEqual(state['ledger']['reservations'][0]['total_usd'], '0.21')
        self.assertEqual(state['ledger']['reservations'][0]['compute_usd'], '0.20')
        self.now += timedelta(seconds=301)
        self.assertBlocked(self.reserve('two'), 'prior_window_pending')
        with self.session() as session:
            self.assertBlocked(session.refresh_accounting(self.refresh_evidence('0.30', '0.25')), 'prior_window_pending')
            self.assertBlocked(session.reconcile('one', self.billing()), 'cleanup_required')
        self.assertEqual(self.read(), state)

    def test_overrun_evidence_malformed_or_inconsistent_is_rejected(self):
        self.cleaned()
        evidence = self.billing()
        evidence.update(actual_total_usd='0.30', actual_compute_usd='0.25',
                        cumulative_total_usd='0.30', cumulative_compute_usd='0.25')
        with self.session() as session: session.reconcile('one', evidence)
        state = self.read()
        for field, value in [('actual_total_usd', None), ('bound_exceeded', False),
                             ('budget_exceeded', True), ('actual_compute_usd', 'CANARY')]:
            malformed = copy.deepcopy(state); malformed['windows']['one']['overrun'][field] = value
            self.write(malformed)
            result = self.assertBlocked(self.reserve('two'), 'invalid_overrun')
            self.assertNotIn('CANARY', json.dumps(result))

    def test_spend_must_not_decrease(self):
        self.cleaned()
        state = self.read()
        state['ledger'].update(reconciled_total_usd='1', reconciled_compute_usd='0.50',
                                remaining_total_usd='14', remaining_compute_usd='9.50')
        self.write(state)
        with self.session() as session:
            self.assertBlocked(session.reconcile('one', self.billing()), 'nonmonotonic_or_inconsistent_spend')
        self.assertEqual(self.read()['ledger']['reconciled_total_usd'], '1')

    def test_duplicate_history_and_wrong_phase_rejected(self):
        self.reserve()
        state = self.read(); state['ledger']['reservations'].append(copy.deepcopy(state['ledger']['reservations'][0]))
        self.write(state)
        self.assertBlocked(self.reserve('two'), 'invalid_reservations')
        self.write(self.state)
        with self.session() as session:
            self.assertBlocked(session.mark_active('missing'), 'invalid_transition')
        self.assertEqual(self.read(), self.state)

    def test_crash_after_reservation_restart_stays_blocked(self):
        self.reserve()
        with self.session() as session:
            session.mark_active('one')
        self.assertBlocked(self.reserve('two'), 'prior_window_pending')
        self.now += timedelta(hours=1)
        self.assertBlocked(self.reserve('two'), 'prior_window_pending')

    def test_write_failure_before_replace_preserves_old_state_and_blocks_recovery(self):
        before = self.path.read_bytes()
        with patch.object(module.os, 'replace', side_effect=OSError('SECRET-CANARY')):
            output = self.assertBlocked(self.reserve())
        self.assertNotIn('CANARY', json.dumps(output))
        self.assertEqual(self.path.read_bytes(), before)
        self.assertTrue((self.directory/module.INTENT_NAME).exists())
        self.assertBlocked(self.reserve(), 'interrupted_persistence')

    def test_failure_after_replace_new_state_is_complete_but_marker_blocks(self):
        original = module.os.fsync
        def fail_directory_after_replace(fd):
            if self.path.exists() and self.read()['revision'] == 1 and os.fstat(fd).st_mode & 0o170000 == 0o040000:
                raise OSError('CANARY')
            return original(fd)
        with patch.object(module.os, 'fsync', side_effect=fail_directory_after_replace):
            self.assertBlocked(self.reserve())
        self.assertEqual(self.read()['windows']['one']['phase'], 'reserved')
        self.assertBlocked(self.reserve('two'), 'interrupted_persistence')

    def test_orphan_temp_blocks_without_deleting(self):
        orphan = self.directory/(module.TEMP_PREFIX+'orphan')
        orphan.write_text('CANARY'); orphan.chmod(0o600)
        self.assertBlocked(self.reserve(), 'interrupted_persistence')
        self.assertEqual(orphan.read_text(), 'CANARY')

    def test_unsafe_files_directories_symlink_fifo_and_hardlink(self):
        self.path.chmod(0o644)
        self.assertBlocked(self.reserve(), 'unsafe_private_file')
        self.path.chmod(0o600)
        self.directory.chmod(0o755)
        with self.assertRaises(r0.Blocked):
            with self.session(): pass
        self.directory.chmod(0o700)
        linked = self.directory/'directory-link'
        linked.symlink_to(self.directory, target_is_directory=True)
        with self.assertRaises(r0.Blocked):
            with module.ledger_session(linked/'setup.json', self.binding, clock=lambda: self.now): pass
        self.path.unlink(); self.path.symlink_to(self.base.config)
        self.assertBlocked(self.reserve())
        self.path.unlink(); os.mkfifo(self.path, 0o600)
        self.assertBlocked(self.reserve(), 'unsafe_private_file')
        self.path.unlink(); os.link(self.base.config, self.path)
        self.assertBlocked(self.reserve(), 'unsafe_private_file')

    def test_subprocess_contention_reuses_stable_r0_lock(self):
        code = """from pathlib import Path
import sys
from tools.redshift.pilot_preflight import pilot_lock,Blocked
try:
 with pilot_lock(Path(sys.argv[1])): print('acquired')
except Blocked as e: print(e.code)
"""
        with self.session():
            lock = self.directory/r0.LOCK_NAME; inode = lock.stat().st_ino
            result = subprocess.run([sys.executable, '-c', code, str(self.directory)],
                                    capture_output=True, text=True, timeout=5, check=True)
            self.assertEqual(result.stdout.strip(), 'lock_contended')
        self.assertEqual(lock.stat().st_ino, inode)

    def test_secret_payload_duplicates_and_closed_session_sanitized(self):
        self.state['password-CANARY'] = 'SECRET-CANARY'; self.write(self.state)
        self.assertNotIn('CANARY', json.dumps(self.assertBlocked(self.reserve(), 'invalid_state')))
        self.path.write_text('{"schema_version":1,"schema_version":1,"PWD":"CANARY"}')
        self.assertNotIn('CANARY', json.dumps(self.assertBlocked(self.reserve(), 'duplicate_record_member')))
        self.write({key: value for key, value in self.state.items() if key != 'password-CANARY'})
        with self.session() as session: pass
        self.assertBlocked(session.reserve('one', self.base.obs, self.base.window), 'session_closed')

    def refresh_evidence(self, total='0.10', compute='0.09'):
        stamp = self.now.isoformat().replace('+00:00', 'Z')
        return {'binding': self.binding.record(), 'verified': True, 'observed_at': stamp,
                'charges_complete_through': stamp, 'reporting_delay_seconds': 0,
                'cumulative_total_usd': total, 'cumulative_compute_usd': compute}

    def reconcile_first(self):
        self.cleaned()
        with self.session() as session:
            self.assertEqual(session.reconcile('one', self.billing())['status'], 'pass')

    def fresh_observations(self, total='0.10', compute='0.09'):
        stamp = self.now.isoformat().replace('+00:00', 'Z')
        for section in self.base.obs.values():
            if isinstance(section, dict): section['observed_at'] = stamp
        self.base.obs['billing'].update(total_spend_usd=total, compute_spend_usd=compute)

    def test_later_window_fresh_accounting_with_unchanged_totals(self):
        self.reconcile_first()
        self.now += timedelta(seconds=301)
        self.fresh_observations()
        self.assertBlocked(self.reserve('two'), 'stale_accounting')
        with self.session() as session:
            self.assertEqual(session.refresh_accounting(self.refresh_evidence())['status'], 'pass')
            self.assertEqual(session.reserve('two', self.base.obs, self.base.window)['status'], 'pass')
        self.assertEqual(self.read()['ledger']['reconciled_total_usd'], '0.10')
        self.assertEqual(self.read()['revision'], 6)

    def test_refresh_persists_increased_unrelated_spend(self):
        self.reconcile_first()
        self.now += timedelta(seconds=301)
        self.fresh_observations('2.10', '1.09')
        with self.session() as session:
            self.assertEqual(session.refresh_accounting(self.refresh_evidence('2.10', '1.09'))['status'], 'pass')
            self.assertEqual(session.reserve('two', self.base.obs, self.base.window)['status'], 'pass')
        state = self.read()
        self.assertEqual(state['ledger']['remaining_total_usd'], '12.90')
        self.assertEqual(state['ledger']['remaining_compute_usd'], '8.91')
        self.assertEqual(state['windows']['one']['phase'], 'reconciled')

    def test_refresh_exhaustion_and_overage_are_persisted_before_reservation_denial(self):
        for total, compute in [('15', '10'), ('16', '11')]:
            with self.subTest(total=total):
                self.write(self.state)
                self.now = self.base.now + timedelta(seconds=301)
                evidence = self.refresh_evidence(total, compute)
                self.fresh_observations(total, compute)
                with self.session() as session:
                    self.assertEqual(session.refresh_accounting(evidence)['status'], 'pass')
                    self.assertBlocked(session.reserve('one', self.base.obs, self.base.window), 'budget_exhausted')
                state = self.read()
                self.assertEqual(state['ledger']['reconciled_total_usd'], total)
                self.assertEqual(state['ledger']['reconciled_compute_usd'], compute)
                self.assertEqual(state['ledger']['remaining_total_usd'], '0')
                self.assertEqual(state['ledger']['remaining_compute_usd'], '0')
                self.assertEqual(state['windows'], {})

    def test_refresh_rejects_rollback_timestamp_regression_unknown_or_foreign_evidence(self):
        self.reconcile_first()
        self.now += timedelta(seconds=301)
        with self.session() as session:
            self.assertEqual(session.refresh_accounting(self.refresh_evidence())['status'], 'pass')
        self.now += timedelta(seconds=1)
        before = self.path.read_bytes()
        variants = [self.refresh_evidence('0.09', '0.09'), self.refresh_evidence('0.10', '0.08'),
                    self.refresh_evidence(None, '0.09')]
        regression = self.refresh_evidence(); regression['observed_at'] = '2026-10-02T12:05:00Z'
        regression['charges_complete_through'] = regression['observed_at']; variants.append(regression)
        foreign = self.refresh_evidence(); foreign['binding']['period_id'] = 'foreign'; variants.append(foreign)
        incomplete = self.refresh_evidence(); incomplete['charges_complete_through'] = '2026-10-02T11:59:59Z'
        variants.append(incomplete)
        future = self.refresh_evidence(); future['observed_at'] = '2026-10-02T12:05:03Z'; variants.append(future)
        for evidence in variants:
            with self.session() as session:
                self.assertBlocked(session.refresh_accounting(evidence))
            self.assertEqual(self.path.read_bytes(), before)

    def test_refresh_does_not_retire_or_double_count_pending_reservation(self):
        self.reserve()
        before = self.path.read_bytes()
        with self.session() as session:
            self.assertBlocked(session.refresh_accounting(self.refresh_evidence()), 'prior_window_pending')
        self.assertEqual(self.path.read_bytes(), before)

    def test_refresh_persist_failure_leaves_old_state_and_admission_blocked(self):
        self.now += timedelta(seconds=301)
        before = self.path.read_bytes()
        with self.session() as session, patch.object(module.os, 'replace', side_effect=OSError('SECRET-CANARY')):
            result = self.assertBlocked(session.refresh_accounting(self.refresh_evidence('2', '1')))
        self.assertNotIn('CANARY', json.dumps(result))
        self.assertEqual(self.path.read_bytes(), before)
        self.assertBlocked(self.reserve(), 'interrupted_persistence')

    def test_ledger_uses_exact_borrowed_fd_and_never_reopens_parent(self):
        original_open = os.open
        opened_directories = []
        def observed_open(path, flags, *args, **kwargs):
            fd = original_open(path, flags, *args, **kwargs)
            if flags & os.O_DIRECTORY:
                opened_directories.append(fd)
            return fd
        with patch.object(module.os, 'open', side_effect=observed_open):
            with self.session() as session:
                self.assertEqual(opened_directories, [session._fd])
                self.assertEqual(session.reserve('one', self.base.obs, self.base.window)['status'], 'pass')
                fd = session._fd
        with self.assertRaises(OSError): os.fstat(fd)
        self.assertBlocked(session.reserve('two', self.base.obs, self.base.window), 'session_closed')

    def test_replaced_directory_and_symlink_prevent_ledger_read_write_divergence(self):
        for symlink in (False, True):
            moved = self.directory.with_name(self.directory.name+'-moved')
            original = self.path.read_bytes()
            try:
                with self.session() as session:
                    self.directory.rename(moved)
                    if symlink:
                        self.directory.symlink_to(moved, target_is_directory=True)
                    else:
                        self.directory.mkdir(mode=0o700)
                        self.path.write_text('replacement-CANARY'); self.path.chmod(0o600)
                    result = self.assertBlocked(session.reserve('one', self.base.obs, self.base.window))
                    self.assertNotIn('CANARY', json.dumps(result))
                    self.assertEqual((moved/'setup.json').read_bytes(), original)
                    if not symlink: self.assertEqual(self.path.read_text(), 'replacement-CANARY')
                    if self.directory.is_symlink(): self.directory.unlink()
                    else: shutil.rmtree(self.directory)
                    moved.rename(self.directory)
            finally:
                if moved.exists():
                    if self.directory.is_symlink(): self.directory.unlink()
                    elif self.directory.exists(): shutil.rmtree(self.directory)
                    moved.rename(self.directory)

    def test_directory_replacement_during_write_keeps_intent_and_never_renames_new_state(self):
        moved = self.directory.with_name(self.directory.name+'-moved')
        original_fsync = os.fsync
        original = self.path.read_bytes()
        replaced = False
        def replace_directory_on_temp_fsync(fd):
            nonlocal replaced
            result = original_fsync(fd)
            if not replaced and any(name.startswith(module.TEMP_PREFIX) for name in os.listdir(self.directory)):
                self.directory.rename(moved)
                self.directory.mkdir(mode=0o700)
                self.path.write_text('replacement-CANARY'); self.path.chmod(0o600)
                replaced = True
            return result
        try:
            with self.session() as session:
                with patch.object(module.os, 'fsync', side_effect=replace_directory_on_temp_fsync):
                    self.assertBlocked(session.reserve('one', self.base.obs, self.base.window), 'directory_identity_changed')
                self.assertEqual((moved/'setup.json').read_bytes(), original)
                self.assertEqual(self.path.read_text(), 'replacement-CANARY')
                self.assertTrue((moved/module.INTENT_NAME).exists())
                shutil.rmtree(self.directory); moved.rename(self.directory)
            self.assertBlocked(self.reserve(), 'interrupted_persistence')
        finally:
            if moved.exists():
                if self.directory.exists(): shutil.rmtree(self.directory)
                moved.rename(self.directory)

    def test_directory_symlink_replacement_during_read_blocks_before_persistence(self):
        moved = self.directory.with_name(self.directory.name+'-moved')
        original_open = os.open
        original = self.path.read_bytes()
        replaced = False
        def race_open(path, flags, *args, **kwargs):
            nonlocal replaced
            fd = original_open(path, flags, *args, **kwargs)
            if path == module.STATE_NAME and not replaced:
                self.directory.rename(moved)
                self.directory.symlink_to(moved, target_is_directory=True)
                replaced = True
            return fd
        try:
            with self.session() as session:
                with patch.object(module.os, 'open', side_effect=race_open):
                    self.assertBlocked(session.reserve('one', self.base.obs, self.base.window))
                self.assertEqual((moved/'setup.json').read_bytes(), original)
                self.assertFalse((moved/module.INTENT_NAME).exists())
                self.directory.unlink(); moved.rename(self.directory)
        finally:
            if moved.exists():
                if self.directory.is_symlink(): self.directory.unlink()
                elif self.directory.exists(): shutil.rmtree(self.directory)
                moved.rename(self.directory)

    def test_lock_file_replacement_blocks_ledger_operation(self):
        lock = self.directory/r0.LOCK_NAME
        saved = self.directory/'saved-lock'
        with self.session() as session:
            lock.rename(saved)
            lock.write_text(''); lock.chmod(0o600)
            self.assertBlocked(session.reserve('one', self.base.obs, self.base.window), 'lock_identity_changed')
            lock.unlink(); saved.rename(lock)
        self.assertEqual(self.read(), self.state)

    def test_fresh_clock_checked_on_each_transition(self):
        self.reserve()
        with self.session() as session:
            evidence = self.cleanup()
            self.now += timedelta(seconds=301)
            self.assertBlocked(session.verify_cleanup('one', evidence), 'stale_cleanup_evidence')


if __name__ == '__main__':
    unittest.main()
