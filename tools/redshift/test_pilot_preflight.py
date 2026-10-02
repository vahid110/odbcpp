"""Offline R0 checks: synthetic records, fake clock, private temporary files only."""
import copy
from datetime import datetime, timedelta, timezone
from decimal import Decimal, localcontext
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest

MODULE = Path(__file__).with_name('pilot_preflight.py')
spec = importlib.util.spec_from_file_location('pilot_preflight', MODULE)
pilot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pilot)


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name).resolve()
        self.directory.chmod(0o700)
        self.config = self.directory / 'synthetic_config'
        self.config.write_text('not credentials')
        self.config.chmod(0o600)
        self.now = datetime(2026, 10, 2, 12, tzinfo=timezone.utc)
        self.stamp = '2026-10-02T12:00:00Z'
        identity = {'account_id': '123456789012', 'workgroup_id': '11111111-1111-1111-1111-111111111111',
                    'namespace_id': '22222222-2222-2222-2222-222222222222', 'region': 'eu-north-1'}
        def verified(**fields):
            return {'verified': True, 'observed_at': self.stamp, **fields}
        self.ledger = {'schema_version': 1, 'period_id': 'initial-pilot', 'identity': identity,
            'total_limit_usd': '15', 'compute_limit_usd': '10', 'reconciled_total_usd': '0',
            'reconciled_compute_usd': '0', 'remaining_total_usd': '15', 'remaining_compute_usd': '10',
            'observed_at': self.stamp, 'reservations': [], 'unresolved_cleanup': False}
        self.obs = {'schema_version': 1, 'identity': verified(**identity),
            'capacity': verified(base_rpus=4, max_rpus=8, lowest_supported_max_rpus=8),
            'pricing': verified(usd_per_rpu_hour='0.5', plan_verified=True, credits_assumed=False),
            'billing': verified(total_spend_usd='0', compute_spend_usd='0', reporting_delay_seconds=300,
                other_charges_verified=True, delayed_total_upper_bound_usd='0', delayed_compute_upper_bound_usd='0'),
            'controls': verified(usage_limit_action='turn_off_user_queries', usage_limits_verified=True,
                budget_alerts_verified=True, statement_runtime_seconds=60, cleanup_bound_seconds=60),
            'fixture': verified(dedicated_user=True, tiny_schema_table=True, database_credentials_baseline=True, server_is_redshift=True),
            'network': verified(verified_tls=True, verified_hostname=True, restricted_to_test_machine=True),
            'private_config': verified(path=str(self.config), required_keys_verified=True, explicit_ssl=True)}
        self.window = {'execution_seconds': 60, 'metering_headroom_seconds': 60,
                       'cleanup_headroom_seconds': 60, 'other_charge_headroom_usd': '0.01'}

    def result(self):
        return pilot.preflight(self.ledger, self.obs, self.window, now=self.now)

    def blocked(self, code=None):
        result = self.result()
        self.assertEqual(result['status'], 'block')
        self.assertIs(result['live_enabled'], False)
        if code:
            self.assertEqual(result['reasons'], [code])
        return result

    def accounting(self, total, compute):
        self.ledger.update(reconciled_total_usd=total, reconciled_compute_usd=compute,
            remaining_total_usd=str(Decimal(15)-Decimal(total)), remaining_compute_usd=str(Decimal(10)-Decimal(compute)))
        self.obs['billing'].update(total_spend_usd=total, compute_spend_usd=compute)

    def test_happy_path_max_capacity_cent_rounding_and_no_mutation(self):
        before = copy.deepcopy((self.ledger, self.obs, self.window))
        result = self.result()
        self.assertEqual(result['status'], 'pass')
        self.assertEqual(result['reservation_compute_usd'], '0.20')
        self.assertEqual(result['reservation_total_usd'], '0.21')
        self.assertIs(result['live_enabled'], False)
        self.assertEqual(before, (self.ledger, self.obs, self.window))
        self.assertEqual(result, self.result())  # No reset/write/reservation side effect.

    def test_decimal_context_does_not_change_accounting(self):
        expected = self.result()
        with localcontext() as context:
            context.prec = 2
            self.assertEqual(expected, self.result())

    def test_ceiling_rounding_and_unknown_delayed_compute(self):
        self.obs['pricing']['usd_per_rpu_hour'] = '0.501'
        self.assertEqual(self.result()['reservation_compute_usd'], '0.21')
        self.obs['billing']['delayed_compute_upper_bound_usd'] = None
        self.blocked('unknown_delayed_charges')

    def test_exact_budget_then_one_cent_over(self):
        self.accounting('14.79', '9.80')
        self.assertEqual(self.result()['status'], 'pass')
        self.window['other_charge_headroom_usd'] = '0.02'
        self.blocked('budget_exhausted')

    def test_exhausted_and_unknown_spend_or_remaining(self):
        self.accounting('15', '10')
        self.blocked('budget_exhausted')
        for key in ('reconciled_total_usd', 'reconciled_compute_usd', 'remaining_total_usd', 'remaining_compute_usd'):
            with self.subTest(key=key):
                original = self.ledger[key]
                self.ledger[key] = None
                self.blocked('unknown_or_invalid_budget')
                self.ledger[key] = original

    def test_inconsistent_balances_and_limits(self):
        self.ledger['remaining_total_usd'] = '14'
        self.blocked('inconsistent_accounting')
        self.ledger['remaining_total_usd'] = '15'
        self.ledger['total_limit_usd'] = '150'
        self.blocked('invalid_limits')

    def test_outstanding_reservations_and_unknown_cleanup(self):
        self.ledger['reservations'] = [{'id': 'one', 'period_id': 'initial-pilot', 'total_usd': '14.8',
             'compute_usd': '9.8', 'status': 'outstanding', 'cleanup_verified': True}]
        self.blocked('budget_exhausted')
        self.ledger['reservations'][0]['total_usd'] = '14.79'
        self.assertEqual(self.result()['status'], 'pass')
        self.ledger['reservations'][0]['cleanup_verified'] = False
        self.blocked('unresolved_reservation')
        self.ledger['reservations'][0]['cleanup_verified'] = True
        self.ledger['unresolved_cleanup'] = None
        self.blocked('unresolved_cleanup')

    def test_reservations_duplicate_foreign_period_and_amounts(self):
        item = {'id': 'one', 'period_id': 'initial-pilot', 'total_usd': '1', 'compute_usd': '1',
                'status': 'outstanding', 'cleanup_verified': True}
        for mutation in ({'period_id': 'another'}, {'compute_usd': '2'}, {'id': 'password=CANARY'}):
            self.ledger['reservations'] = [{**item, **mutation}]
            self.blocked('invalid_reservations')
        self.ledger['reservations'] = [item, copy.deepcopy(item)]
        self.blocked('invalid_reservations')

    def test_reporting_delay_is_not_runtime_unknown_liability_blocks(self):
        expected = self.result()['reservation_compute_usd']
        self.obs['billing']['reporting_delay_seconds'] = 86400
        self.assertEqual(self.result()['reservation_compute_usd'], expected)
        self.obs['billing']['delayed_total_upper_bound_usd'] = None
        self.blocked('unknown_delayed_charges')
        self.obs['billing'].update(delayed_total_upper_bound_usd='14.80', delayed_compute_upper_bound_usd='9.81')
        self.blocked('budget_exhausted')

    def test_strict_numeric_inputs_and_absurd_exponents(self):
        bad = [True, False, None, -1, 'NaN', 'Infinity', Decimal('NaN'), Decimal('Infinity'),
               Decimal('1E1000000'), Decimal('1E-1000000'), '1e2', 0.5, '9'*65, 10**1000]
        for value in bad:
            with self.subTest(type=type(value).__name__):
                self.window['execution_seconds'] = value
                self.blocked('invalid_window')

    def test_bounded_durations_and_required_headroom(self):
        for field, value in [('execution_seconds', 3601), ('cleanup_headroom_seconds', 3601),
                             ('metering_headroom_seconds', 59), ('metering_headroom_seconds', 301),
                             ('other_charge_headroom_usd', 0)]:
            original = self.window[field]
            self.window[field] = value
            self.blocked()
            self.window[field] = original

    def test_stale_future_exact_freshness_and_invalid_clock(self):
        self.now += timedelta(seconds=300)
        self.assertEqual(self.result()['status'], 'pass')
        self.now += timedelta(microseconds=1)
        self.blocked('stale_accounting')
        self.now = datetime(2026, 10, 2, 11, 59, 59, tzinfo=timezone.utc)
        self.blocked('stale_accounting')
        self.now = datetime(2026, 10, 2, 12)
        self.blocked('invalid_clock')

    def test_each_verified_section_and_timestamp_required(self):
        for section in ('identity', 'capacity', 'pricing', 'controls', 'fixture', 'network', 'private_config', 'billing'):
            with self.subTest(section=section):
                self.obs[section]['verified'] = False
                self.blocked()
                self.obs[section]['verified'] = True
                original = self.obs[section]['observed_at']
                self.obs[section]['observed_at'] = '2026-10-02T12:00:01Z'
                self.blocked()
                self.obs[section]['observed_at'] = original

    def test_identity_capacity_controls_and_secret_sanitization(self):
        for section, field, value in [('identity', 'account_id', 'secret-PWD=CANARY'),
            ('capacity', 'base_rpus', 8), ('capacity', 'max_rpus', 16),
            ('controls', 'usage_limit_action', 'log'), ('network', 'verified_hostname', 1),
            ('private_config', 'explicit_ssl', False), ('pricing', 'credits_assumed', True)]:
            original = self.obs[section][field]
            self.obs[section][field] = value
            output = json.dumps(self.blocked())
            self.assertNotIn('CANARY', output)
            self.assertNotIn('123456789012', output)
            self.assertNotIn(str(self.config), output)
            self.obs[section][field] = original
        self.obs['secret-PWD=CANARY'] = 'token-CANARY'
        self.assertNotIn('CANARY', json.dumps(self.blocked('invalid_observations')))

    def test_private_config_permissions_symlink_missing(self):
        self.config.chmod(0o644)
        self.blocked('unsafe_private_file')
        self.config.chmod(0o600)
        target = self.directory/'link'
        target.symlink_to(self.config)
        self.obs['private_config']['path'] = str(target)
        self.blocked('unsafe_private_file')
        self.obs['private_config']['path'] = str(self.directory/'absent-CANARY')
        self.assertNotIn('CANARY', json.dumps(self.blocked('private_config_unavailable')))

    def test_lock_stable_inode_and_subprocess_contention(self):
        child = """import importlib.util,sys
s=importlib.util.spec_from_file_location('p',sys.argv[1]);p=importlib.util.module_from_spec(s);s.loader.exec_module(p)
try:
 with p.pilot_lock(sys.argv[2]): print('acquired')
except p.Blocked as e: print(e.code)
"""
        def run_child():
            return subprocess.run([sys.executable, '-c', child, str(MODULE), str(self.directory)],
                                   capture_output=True, text=True, timeout=5, check=True).stdout.strip()
        with pilot.pilot_lock(self.directory):
            lock = self.directory/pilot.LOCK_NAME
            inode = lock.stat().st_ino
            self.assertEqual(run_child(), 'lock_contended')
            self.assertEqual(lock.stat().st_mode & 0o777, 0o600)
        self.assertTrue(lock.exists())
        self.assertEqual(run_child(), 'acquired')
        self.assertEqual(lock.stat().st_ino, inode)

    def test_unsafe_lock_file_directory_symlinks_fifo_and_hardlinks(self):
        lock = self.directory/pilot.LOCK_NAME
        lock.write_text('CANARY-do-not-delete')
        lock.chmod(0o644)
        with self.assertRaises(pilot.Blocked):
            with pilot.pilot_lock(self.directory): pass
        self.assertEqual(lock.read_text(), 'CANARY-do-not-delete')
        lock.chmod(0o600)
        self.directory.chmod(0o755)
        with self.assertRaises(pilot.Blocked):
            with pilot.pilot_lock(self.directory): pass
        self.directory.chmod(0o700)
        link = self.directory/'directory-link'
        link.symlink_to(self.directory, target_is_directory=True)
        with self.assertRaises(pilot.Blocked):
            with pilot.pilot_lock(link): pass
        lock.unlink()
        lock.symlink_to(self.config)
        with self.assertRaises(pilot.Blocked):
            with pilot.pilot_lock(self.directory): pass
        lock.unlink()
        os.mkfifo(lock, 0o600)
        with self.assertRaises(pilot.Blocked):
            with pilot.pilot_lock(self.directory): pass
        lock.unlink()
        os.link(self.config, lock)
        with self.assertRaises(pilot.Blocked):
            with pilot.pilot_lock(self.directory): pass

    def test_borrowed_lock_directory_fd_lifetime(self):
        with pilot.pilot_lock(self.directory) as fd:
            self.assertIsInstance(fd, int)
            self.assertEqual(os.fstat(fd).st_ino, self.directory.stat().st_ino)
            pilot.assert_locked_directory(self.directory, fd)
        with self.assertRaises(OSError):
            os.fstat(fd)
        with self.assertRaises(pilot.Blocked):
            pilot.assert_locked_directory(self.directory, fd)

    def test_directory_replacement_and_symlink_block_lock_exit(self):
        for symlink in (False, True):
            moved = self.directory.with_name(self.directory.name+'-moved')
            try:
                with self.assertRaises(pilot.Blocked):
                    with pilot.pilot_lock(self.directory) as fd:
                        self.directory.rename(moved)
                        if symlink:
                            self.directory.symlink_to(moved, target_is_directory=True)
                        else:
                            self.directory.mkdir(mode=0o700)
                        with self.assertRaises(pilot.Blocked):
                            pilot.assert_locked_directory(self.directory, fd)
            finally:
                if self.directory.is_symlink(): self.directory.unlink()
                elif self.directory.exists(): shutil.rmtree(self.directory)
                moved.rename(self.directory)

    def test_directory_replaced_during_open_is_not_yielded(self):
        from unittest.mock import patch
        moved = self.directory.with_name(self.directory.name+'-moved')
        original_open = os.open
        def race_open(path, flags, *args, **kwargs):
            fd = original_open(path, flags, *args, **kwargs)
            if Path(path) == self.directory and flags & os.O_DIRECTORY:
                self.directory.rename(moved)
                self.directory.mkdir(mode=0o700)
            return fd
        try:
            with patch.object(pilot.os, 'open', side_effect=race_open), self.assertRaises(pilot.Blocked):
                with pilot.pilot_lock(self.directory): self.fail('divergent descriptor yielded')
        finally:
            shutil.rmtree(self.directory)
            moved.rename(self.directory)

    def test_cli_malformed_secret_input_is_sanitized(self):
        record = self.directory/'record.json'
        record.write_text('{"PWD":"secret-CANARY", invalid')
        child = subprocess.run([sys.executable, str(MODULE), str(record)],
                               capture_output=True, text=True, timeout=5)
        self.assertEqual(child.returncode, 2)
        self.assertEqual(json.loads(child.stdout)['status'], 'block')
        self.assertNotIn('CANARY', child.stdout+child.stderr)
        self.assertEqual(child.stderr, '')
        child = subprocess.run([sys.executable, str(MODULE), str(record), '--secret-CANARY'],
                               capture_output=True, text=True, timeout=5)
        self.assertEqual(child.returncode, 2)
        self.assertNotIn('CANARY', child.stdout+child.stderr)
        self.assertEqual(child.stderr, '')

    def test_cli_rejects_top_level_and_nested_duplicate_members(self):
        ledger, observations = copy.deepcopy((self.ledger, self.obs))
        stamp = datetime.now(timezone.utc).isoformat().replace('+00:00', 'Z')
        ledger['observed_at'] = stamp
        for section in observations.values():
            if isinstance(section, dict):
                section['observed_at'] = stamp
        payload = json.dumps({'ledger': ledger, 'observations': observations, 'window': self.window})
        record = self.directory / 'record.json'

        def invoke(text):
            record.write_text(text)
            return subprocess.run([sys.executable, str(MODULE), str(record)],
                                  capture_output=True, text=True, timeout=5)

        self.assertEqual(invoke(payload).returncode, 0)
        ambiguous = [payload[:-1] + ', "ledger":' + json.dumps(ledger) + '}',
                     payload.replace('"verified": true', '"verified": false, "verified": true', 1),
                     payload.replace('"verified": true', '"verified": true, "verified": true', 1)]
        for text in ambiguous:
            with self.subTest(text=text[:20]):
                child = invoke(text)
                self.assertEqual(child.returncode, 2)
                self.assertEqual(json.loads(child.stdout), {
                    'status': 'block', 'reasons': ['record_unavailable_or_invalid'], 'live_enabled': False})
                self.assertEqual(child.stderr, '')
                self.assertNotIn(str(self.config), child.stdout)


if __name__ == '__main__':
    unittest.main()
