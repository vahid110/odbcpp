"""R0 offline Redshift pilot preflight and canonical, nonblocking OS lock.

This partial proof validates caller-supplied *verified observation* records and
persistent accounting; it does not upgrade setup.json's reported fields to proof.
No AWS clients, credential reads, ledger writes, launchers, retries or provisioning
exist here. CLI only validates records and always returns live_enabled=False.
R1 must obtain trustworthy AWS/billing/control readback under this same lock,
atomically reserve the canonical ledger, bound reviewed process-tree execution,
verify server cancellation/cleanup and reconcile delayed charges before activation.
A passed offline record is not permission or qualification to run paid work.

Amounts accept Decimal, integer or decimal strings (not bool/float/NaN/Inf).
JSON CLI numbers are decoded as Decimal. Remaining balances are pre-reservation;
outstanding reservations are deducted exactly once and never reset per call.
All timestamps are UTC ISO strings ending Z, with no future grace. Headroom uses
maximum capacity, execution + >=60s metering + cleanup, rounds
up to cents, and adds explicit other-charge USD headroom to the total reservation.
Reporting lag is separate: explicit delayed-charge upper bounds are liabilities;
unknown delayed charges block. Billing delay is never modeled as active runtime.
The lock protects cooperating local callers only; it cannot constrain cloud use.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation, ROUND_CEILING, localcontext
import fcntl
import json
import os
from pathlib import Path
import re
import stat
from typing import Iterator

CANONICAL_DIRECTORY = Path('/Users/vahidsbr/.local/state/odbcpp/redshift-pilot')
LOCK_NAME = 'pilot.lock'
TOTAL_LIMIT = Decimal('15')
COMPUTE_LIMIT = Decimal('10')


class Blocked(Exception):
    """Only fixed public codes leave validation; no input or OS error text."""
    def __init__(self, code: str):
        self.code = code
        super().__init__(code)


def _need(condition: bool, code: str) -> None:
    if not condition:
        raise Blocked(code)


def _record(value, keys: set[str], code: str):
    _need(isinstance(value, dict) and set(value) == keys, code)
    return value


def _number(value, code: str, *, positive=False) -> Decimal:
    _need(not isinstance(value, bool) and isinstance(value, (str, int, Decimal)), code)
    if isinstance(value, str):
        _need(len(value) <= 64 and re.fullmatch(r'[+]?(?:\d+(?:\.\d*)?|\.\d+)', value) is not None, code)
    if isinstance(value, int):
        _need(value.bit_length() <= 44, code)
    try:
        number = Decimal(value)
        _need(number.is_finite(), code)
        parts = number.as_tuple()
        _need(len(parts.digits) <= 32 and -12 <= parts.exponent <= 12, code)
        _need(number >= 0 and (not positive or number > 0) and number.adjusted() <= 12, code)
        return number
    except (InvalidOperation, ValueError, TypeError):
        raise Blocked(code) from None


def _timestamp(value, now: datetime, max_age: Decimal, code: str):
    _need(isinstance(value, str) and len(value) <= 40 and value.endswith('Z'), code)
    try:
        observed = datetime.fromisoformat(value[:-1] + '+00:00')
    except (ValueError, TypeError):
        raise Blocked(code) from None
    _need(observed.utcoffset() == timezone.utc.utcoffset(observed), code)
    age = Decimal(str((now - observed).total_seconds()))
    _need(0 <= age <= max_age, code)
    return observed


def _verified(value, fields: set[str], now, max_age, code):
    record = _record(value, fields | {'verified', 'observed_at'}, code)
    _need(record['verified'] is True, code)
    _timestamp(record['observed_at'], now, max_age, code)
    return record


def _private_components(path: Path) -> None:
    """Reject symlink components; only the immediate private directory needs 0700."""
    _need(path.is_absolute(), 'unsafe_private_path')
    cursor = Path(path.anchor)
    for part in path.parts[1:]:
        _need(part not in ('.', '..'), 'unsafe_private_path')
        cursor = cursor / part
        info = os.lstat(cursor)
        _need(not stat.S_ISLNK(info.st_mode) and stat.S_ISDIR(info.st_mode), 'unsafe_private_path')
    info = os.lstat(path)
    _need(info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o700, 'unsafe_private_directory')


def _private_file(info) -> None:
    _need(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
          and stat.S_IMODE(info.st_mode) == 0o600 and info.st_nlink == 1, 'unsafe_private_file')


def validate_private_file(path) -> None:
    """Metadata inspection only: never open or read connection credentials."""
    try:
        _need(isinstance(path, str) and len(path) <= 4096, 'unsafe_private_path')
        value = Path(path)
        _private_components(value.parent)
        _private_file(os.lstat(value))
    except Blocked:
        raise
    except (OSError, ValueError, TypeError):
        raise Blocked('private_config_unavailable') from None


def assert_locked_directory(directory: Path, folder_fd: int) -> None:
    """Fail closed if the canonical path diverges from the borrowed directory."""
    try:
        directory = Path(directory)
        _private_components(directory)
        opened = os.fstat(folder_fd)
        current = os.stat(directory, follow_symlinks=False)
        _need(stat.S_ISDIR(opened.st_mode) and opened.st_uid == os.getuid()
              and stat.S_IMODE(opened.st_mode) == 0o700, 'unsafe_private_directory')
        _need((opened.st_dev, opened.st_ino) == (current.st_dev, current.st_ino), 'directory_identity_changed')
    except Blocked:
        raise
    except (OSError, ValueError, TypeError):
        raise Blocked('directory_identity_changed') from None


@contextmanager
def pilot_lock(directory: Path = CANONICAL_DIRECTORY) -> Iterator[int]:
    """Nonblocking stable lock inode; never delete, truncate or stale-reap it.

    Tests pass a private temporary directory. Production defaults to the single
    canonical path, independent of cwd/worktree. Directory must already exist.
    Yields the exact verified directory fd, borrowed only until context exit.
    Consumers must not close/retain it, reopen the pathname, or unlink the lock.
    Consumers ignoring the yielded value remain compatible. Use
    assert_locked_directory before/after directory-relative state operations.
    """
    folder_fd = lock_fd = None
    try:
        directory = Path(directory)
        _private_components(directory)
        folder_fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        folder_info = os.fstat(folder_fd)
        _need(folder_info.st_uid == os.getuid() and stat.S_IMODE(folder_info.st_mode) == 0o700,
              'unsafe_private_directory')
        assert_locked_directory(directory, folder_fd)
        flags = os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK
        try:
            lock_fd = os.open(LOCK_NAME, flags | os.O_CREAT | os.O_EXCL, 0o600, dir_fd=folder_fd)
        except FileExistsError:
            lock_fd = os.open(LOCK_NAME, flags, dir_fd=folder_fd)
        _private_file(os.fstat(lock_fd))
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise Blocked('lock_contended') from None
        current = os.stat(LOCK_NAME, dir_fd=folder_fd, follow_symlinks=False)
        _need((current.st_dev, current.st_ino) == (os.fstat(lock_fd).st_dev, os.fstat(lock_fd).st_ino),
              'lock_identity_changed')
        assert_locked_directory(directory, folder_fd)
        yield folder_fd
        assert_locked_directory(directory, folder_fd)
        _private_file(os.fstat(lock_fd))
        current = os.stat(LOCK_NAME, dir_fd=folder_fd, follow_symlinks=False)
        _need((current.st_dev, current.st_ino) == (os.fstat(lock_fd).st_dev, os.fstat(lock_fd).st_ino),
              'lock_identity_changed')
    except Blocked:
        raise
    except (OSError, ValueError, TypeError):
        raise Blocked('lock_unavailable') from None
    finally:
        # Closing descriptor releases OS lock; retain stable file for all chats.
        if lock_fd is not None:
            os.close(lock_fd)
        if folder_fd is not None:
            os.close(folder_fd)


def _validate(ledger, observations, window, now, max_age):
    _need(isinstance(now, datetime) and now.tzinfo is not None
          and now.utcoffset() == timezone.utc.utcoffset(now), 'invalid_clock')
    age_limit = _number(max_age, 'invalid_freshness', positive=True)
    _need(age_limit <= 300, 'invalid_freshness')
    ledger = _record(ledger, {'schema_version', 'period_id', 'identity', 'total_limit_usd',
        'compute_limit_usd', 'reconciled_total_usd', 'reconciled_compute_usd',
        'remaining_total_usd', 'remaining_compute_usd', 'observed_at',
        'reservations', 'unresolved_cleanup'}, 'invalid_ledger')
    _need(type(ledger['schema_version']) is int and ledger['schema_version'] == 1, 'invalid_ledger')
    _need(isinstance(ledger['period_id'], str) and re.fullmatch(r'[A-Za-z0-9_-]{1,64}', ledger['period_id']) is not None,
          'invalid_period')
    _timestamp(ledger['observed_at'], now, age_limit, 'stale_accounting')
    _need(ledger['unresolved_cleanup'] is False, 'unresolved_cleanup')
    _need(_number(ledger['total_limit_usd'], 'invalid_limits') == TOTAL_LIMIT
          and _number(ledger['compute_limit_usd'], 'invalid_limits') == COMPUTE_LIMIT, 'invalid_limits')
    amounts = {key: _number(ledger[key], 'unknown_or_invalid_budget') for key in
               ('reconciled_total_usd', 'reconciled_compute_usd', 'remaining_total_usd', 'remaining_compute_usd')}
    total_spent, compute_spent = amounts['reconciled_total_usd'], amounts['reconciled_compute_usd']
    _need(compute_spent <= total_spent <= TOTAL_LIMIT and compute_spent <= COMPUTE_LIMIT,
          'inconsistent_accounting')
    _need(amounts['remaining_total_usd'] == TOTAL_LIMIT - total_spent
          and amounts['remaining_compute_usd'] == COMPUTE_LIMIT - compute_spent, 'inconsistent_accounting')
    observations = _record(observations, {'schema_version', 'identity', 'capacity', 'pricing',
        'controls', 'fixture', 'network', 'private_config', 'billing'}, 'invalid_observations')
    _need(type(observations['schema_version']) is int and observations['schema_version'] == 1, 'invalid_observations')
    identity_fields = {'account_id', 'workgroup_id', 'namespace_id', 'region'}
    bound = _record(ledger['identity'], identity_fields, 'invalid_identity')
    identity = _verified(observations['identity'], identity_fields, now, age_limit, 'identity_unverified')
    _need(identity['region'] == 'eu-north-1', 'identity_mismatch')
    for key, pattern in [('account_id', r'\d{12}'), ('workgroup_id', r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}'),
                         ('namespace_id', r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}')]:
        _need(isinstance(identity[key], str) and re.fullmatch(pattern, identity[key]) is not None, 'invalid_identity')
    _need(all(identity[key] == bound[key] for key in identity_fields), 'identity_mismatch')
    capacity = _verified(observations['capacity'], {'base_rpus', 'max_rpus', 'lowest_supported_max_rpus'},
                         now, age_limit, 'capacity_unverified')
    base, maximum, lowest = [_number(capacity[k], 'invalid_capacity', positive=True) for k in
                             ('base_rpus', 'max_rpus', 'lowest_supported_max_rpus')]
    _need(base == 4 and maximum >= base and maximum == lowest
          and maximum == maximum.to_integral_value() and maximum <= 512, 'invalid_capacity')
    pricing = _verified(observations['pricing'], {'usd_per_rpu_hour', 'plan_verified', 'credits_assumed'},
                        now, age_limit, 'pricing_unverified')
    _need(pricing['plan_verified'] is True and pricing['credits_assumed'] is False, 'pricing_unverified')
    rate = _number(pricing['usd_per_rpu_hour'], 'invalid_rate', positive=True)
    _need(rate <= 10000, 'invalid_rate')
    billing = _verified(observations['billing'], {'total_spend_usd', 'compute_spend_usd',
        'reporting_delay_seconds', 'other_charges_verified', 'delayed_total_upper_bound_usd',
        'delayed_compute_upper_bound_usd'}, now, age_limit, 'billing_unverified')
    _need(billing['other_charges_verified'] is True, 'billing_unverified')
    _need(_number(billing['total_spend_usd'], 'unknown_or_invalid_budget') == total_spent
          and _number(billing['compute_spend_usd'], 'unknown_or_invalid_budget') == compute_spent,
          'inconsistent_accounting')
    _need(billing['observed_at'] == ledger['observed_at'], 'inconsistent_accounting')
    delay = _number(billing['reporting_delay_seconds'], 'invalid_reporting_delay')
    _need(delay <= 86400, 'invalid_reporting_delay')
    delayed_total = _number(billing['delayed_total_upper_bound_usd'], 'unknown_delayed_charges')
    delayed_compute = _number(billing['delayed_compute_upper_bound_usd'], 'unknown_delayed_charges')
    _need(delayed_compute <= delayed_total, 'inconsistent_accounting')
    window = _record(window, {'execution_seconds', 'metering_headroom_seconds', 'cleanup_headroom_seconds',
                              'other_charge_headroom_usd'}, 'invalid_window')
    execution = _number(window['execution_seconds'], 'invalid_window', positive=True)
    metering = _number(window['metering_headroom_seconds'], 'invalid_window')
    cleanup = _number(window['cleanup_headroom_seconds'], 'invalid_window', positive=True)
    other = _number(window['other_charge_headroom_usd'], 'invalid_window', positive=True)
    _need(60 <= metering <= 300, 'insufficient_metering_headroom')
    _need(execution <= 3600 and cleanup <= 3600 and other <= 5, 'invalid_window')
    controls = _verified(observations['controls'], {'usage_limit_action', 'usage_limits_verified',
        'budget_alerts_verified', 'statement_runtime_seconds', 'cleanup_bound_seconds'},
        now, age_limit, 'controls_unverified')
    _need(controls['usage_limit_action'] == 'turn_off_user_queries'
          and controls['usage_limits_verified'] is True and controls['budget_alerts_verified'] is True,
          'controls_unverified')
    _need(_number(controls['statement_runtime_seconds'], 'invalid_controls', positive=True) <= execution
          and _number(controls['cleanup_bound_seconds'], 'invalid_controls', positive=True) <= cleanup,
          'invalid_controls')
    for section, fields in [('fixture', {'dedicated_user', 'tiny_schema_table', 'database_credentials_baseline', 'server_is_redshift'}),
                            ('network', {'verified_tls', 'verified_hostname', 'restricted_to_test_machine'})]:
        record = _verified(observations[section], fields, now, age_limit, section + '_unverified')
        _need(all(record[field] is True for field in fields), section + '_unverified')
    config = _verified(observations['private_config'], {'path', 'required_keys_verified', 'explicit_ssl'},
                       now, age_limit, 'private_config_unverified')
    _need(config['required_keys_verified'] is True and config['explicit_ssl'] is True, 'private_config_unverified')
    validate_private_file(config['path'])
    _need(isinstance(ledger['reservations'], list) and len(ledger['reservations']) <= 10000, 'invalid_reservations')
    reserved_total = reserved_compute = Decimal(0)
    ids = set()
    for reservation in ledger['reservations']:
        reservation = _record(reservation, {'id', 'period_id', 'total_usd', 'compute_usd', 'status', 'cleanup_verified'},
                              'invalid_reservations')
        rid = reservation['id']
        _need(isinstance(rid, str) and re.fullmatch(r'[A-Za-z0-9_-]{1,64}', rid) is not None
              and rid not in ids and reservation['period_id'] == ledger['period_id'], 'invalid_reservations')
        ids.add(rid)
        _need(reservation['status'] in ('outstanding', 'reconciled') and reservation['cleanup_verified'] is True,
              'unresolved_reservation')
        rt = _number(reservation['total_usd'], 'invalid_reservations')
        rc = _number(reservation['compute_usd'], 'invalid_reservations')
        _need(rc <= rt, 'invalid_reservations')
        if reservation['status'] == 'outstanding':
            reserved_total += rt
            reserved_compute += rc
    available_total = TOTAL_LIMIT - total_spent - reserved_total - delayed_total
    available_compute = COMPUTE_LIMIT - compute_spent - reserved_compute - delayed_compute
    # ROUND_CEILING prevents decimal representation/cent rounding under-reservation.
    proposed_compute = (maximum * rate * (execution + metering + cleanup) / Decimal(3600)).quantize(
        Decimal('.01'), rounding=ROUND_CEILING)
    proposed_total = (proposed_compute + other).quantize(Decimal('.01'), rounding=ROUND_CEILING)
    _need(proposed_compute <= available_compute and proposed_total <= available_total, 'budget_exhausted')
    return {'reservation_compute_usd': str(proposed_compute), 'reservation_total_usd': str(proposed_total),
            'available_compute_usd': str(available_compute), 'available_total_usd': str(available_total)}


def preflight(ledger, observations, window, *, now=None, max_age_seconds=300):
    """Return sanitized arithmetic outcome; does not acquire lock or reserve funds.

    R1 must call under pilot_lock and persist reservation before any connection.
    A supplied verified=True record is evidence-input syntax, not AWS verification.
    """
    try:
        with localcontext() as context:
            context.prec = 64
            amounts = _validate(ledger, observations, window, now if now is not None else datetime.now(timezone.utc),
                                max_age_seconds)
        return {'status': 'pass', 'reasons': [], 'live_enabled': False, **amounts}
    except Blocked as error:
        return {'status': 'block', 'reasons': [error.code], 'live_enabled': False}
    except Exception:
        # Never serialize arbitrary exceptions, field names, paths or inputs.
        return {'status': 'block', 'reasons': ['invalid_record'], 'live_enabled': False}


class _SafeArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        raise Blocked('invalid_cli_arguments')


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        _need(key not in result, 'duplicate_record_member')
        result[key] = value
    return result


def main(argv=None):
    parser = _SafeArgumentParser(description='Offline pilot record validation; never launches live tests.')
    parser.add_argument('record', help='JSON containing ledger, observations, window (no credentials)')
    try:
        args = parser.parse_args(argv)
        with open(args.record, encoding='utf-8') as stream:
            contents = stream.read(1024 * 1024 + 1)
            _need(len(contents) <= 1024 * 1024, 'record_too_large')
            record = json.loads(contents, parse_float=Decimal, object_pairs_hook=_unique_object,
                parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
        _record(record, {'ledger', 'observations', 'window'}, 'invalid_record')
        result = preflight(record['ledger'], record['observations'], record['window'])
    except Exception:
        result = {'status': 'block', 'reasons': ['record_unavailable_or_invalid'], 'live_enabled': False}
    print(json.dumps(result, sort_keys=True))
    return 0 if result['status'] == 'pass' else 2


if __name__ == '__main__':
    raise SystemExit(main())
