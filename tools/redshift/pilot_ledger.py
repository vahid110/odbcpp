"""R1a OFFLINE durable ledger transitions; no launcher, AWS or credential reader.

There is deliberately no initializer, migration or production default. Callers
must supply an existing private setup.json with this strict synthetic schema and
an immutable binding loaded from a separately protected, trusted anchor. The
current real setup.json is not assumed compatible: centralized migration must
preserve unknown balances/history and establish ONE budget authority before R2.
These APIs cannot authenticate supplied AWS/billing evidence; trusted readback is
still required. No result enables live execution.

Session lifetime holds R0's stable pilot.lock. A reservation is persisted before
return; reserved/active/uncertain phases block all further admission, including
crashes before a launcher starts. Verified cleanup retains upper bounds until
matching complete-through billing evidence reconciles known charges. Client time
or a caller phase flag never refunds a reservation. Independent console activity
and hostile trusted processes are outside the cooperative local-lock boundary.

Writes use exclusive private intent/temp files, file fsync, replace, directory
fsync, then intent removal. Interruptions retain an intent/orphan and block rather
than guessing recovery. An operator must reconcile interrupted state separately;
no stale-file deletion or automatic refund exists here.

Accounting refresh is explicit and durably committed separately from reserve.
It accepts complete-through fresh billing only when all windows are reconciled;
this avoids double-counting already-accounted pending-window charges. Increased
or over-budget spend persists even when a subsequent reservation is blocked.
Over-budget balances clamp to zero availability; actual spend is never clamped.
Reconciliation records trusted monotonic cumulative spend before reporting budget
or window-bound overruns. An explicit terminal overrun phase retains original
outstanding bounds and numeric overrun evidence, blocks restart/admission and has
no automatic refund/recovery transition. Missing/mismatched/incomplete evidence
still blocks without claiming its costs are verified.

The ledger consumes the exact borrowed directory fd yielded by R0; it never
reopens the directory pathname. Each load/persistence checkpoint verifies that
the canonical directory still names this inode, and the stable lock-file identity
is unchanged. Replacement/symlink divergence blocks; uncertain writes retain the
intent marker. This detects changes at checkpoints, not hostile same-UID actors
renaming paths between syscalls; exclusive administrator directory ownership and
cooperative no-rename/no-unlink rules remain deployment constraints.
"""
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from decimal import Decimal, ROUND_CEILING, localcontext
import json
import os
from pathlib import Path
import re
import secrets
from typing import Iterator

from tools.redshift import pilot_preflight as r0

STATE_NAME = 'setup.json'
INTENT_NAME = '.pilot-ledger.intent'
TEMP_PREFIX = '.pilot-ledger.tmp-'
LIMIT = 1024 * 1024
ACTIVE_PHASES = {'reserved', 'active', 'uncertain'}


@dataclass(frozen=True)
class PilotBinding:
    """Trusted caller-owned immutable anchor, never inferred from mutable ledger."""
    pilot_id: str
    period_id: str
    account_id: str
    workgroup_id: str
    namespace_id: str
    region: str = 'eu-north-1'

    def record(self):
        for value in (self.pilot_id, self.period_id):
            r0._need(isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9_-]{1,64}', value) is not None,
                     'invalid_binding')
        r0._need(isinstance(self.account_id, str) and re.fullmatch(r'\d{12}', self.account_id) is not None,
                 'invalid_binding')
        for value in (self.workgroup_id, self.namespace_id):
            r0._need(isinstance(value, str) and re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', value) is not None,
                     'invalid_binding')
        r0._need(self.region == 'eu-north-1', 'invalid_binding')
        return {key: getattr(self, key) for key in ('pilot_id', 'period_id', 'account_id', 'workgroup_id', 'namespace_id', 'region')}

    def identity(self):
        return {k: self.record()[k] for k in ('account_id', 'workgroup_id', 'namespace_id', 'region')}


def _stamp(now):
    r0._need(isinstance(now, datetime) and now.tzinfo is not None
             and now.utcoffset() == timedelta(0), 'invalid_clock')
    return now.isoformat().replace('+00:00', 'Z')


def _time(value, now):
    return r0._timestamp(value, now, Decimal('999999999999'), 'invalid_timestamp')


def _id(value):
    r0._need(isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9_-]{1,64}', value) is not None,
             'invalid_reservation_id')


def _state(record, binding, now):
    record = r0._record(record, {'schema_version', 'binding', 'revision', 'ledger', 'windows'}, 'invalid_state')
    r0._need(type(record['schema_version']) is int and record['schema_version'] == 1
             and type(record['revision']) is int and 0 <= record['revision'] <= 1000000, 'invalid_state')
    r0._need(record['binding'] == binding.record(), 'binding_mismatch')
    ledger = r0._record(record['ledger'], {'schema_version', 'period_id', 'identity', 'total_limit_usd',
        'compute_limit_usd', 'reconciled_total_usd', 'reconciled_compute_usd', 'remaining_total_usd',
        'remaining_compute_usd', 'observed_at', 'reservations', 'unresolved_cleanup'}, 'invalid_ledger')
    r0._need(type(ledger['schema_version']) is int and ledger['schema_version'] == 1
             and ledger['period_id'] == binding.period_id and ledger['identity'] == binding.identity(), 'binding_mismatch')
    r0._need(r0._number(ledger['total_limit_usd'], 'invalid_limits') == r0.TOTAL_LIMIT
             and r0._number(ledger['compute_limit_usd'], 'invalid_limits') == r0.COMPUTE_LIMIT, 'invalid_limits')
    _time(ledger['observed_at'], now)
    total, compute, remaining_total, remaining_compute = [r0._number(ledger[k], 'unknown_or_invalid_budget') for k in
        ('reconciled_total_usd', 'reconciled_compute_usd', 'remaining_total_usd', 'remaining_compute_usd')]
    r0._need(compute <= total and remaining_total == max(Decimal(0), Decimal(15)-total)
             and remaining_compute == max(Decimal(0), Decimal(10)-compute), 'inconsistent_accounting')
    r0._need(isinstance(record['windows'], dict) and len(record['windows']) <= 10000
             and isinstance(ledger['reservations'], list), 'invalid_windows')
    reservations = {}
    for item in ledger['reservations']:
        item = r0._record(item, {'id', 'period_id', 'total_usd', 'compute_usd', 'status', 'cleanup_verified'}, 'invalid_reservations')
        rid = item['id']; _id(rid)
        r0._need(rid not in reservations and item['period_id'] == binding.period_id
                 and item['status'] in ('outstanding', 'reconciled') and type(item['cleanup_verified']) is bool,
                 'invalid_reservations')
        rt = r0._number(item['total_usd'], 'invalid_reservations')
        rc = r0._number(item['compute_usd'], 'invalid_reservations')
        r0._need(0 <= rc <= rt, 'invalid_reservations')
        reservations[rid] = item
    r0._need(set(reservations) == set(record['windows']), 'inconsistent_history')
    uncertain = False
    for rid, window in record['windows'].items():
        _id(rid)
        r0._need(isinstance(window, dict), 'invalid_windows')
        fields = {'phase', 'reserved_at', 'cleanup_at', 'reconciled_at'}
        if window.get('phase') == 'overrun': fields.add('overrun')
        window = r0._record(window, fields, 'invalid_windows')
        r0._need(window['phase'] in ACTIVE_PHASES | {'cleaned', 'reconciled', 'overrun'}, 'invalid_phase')
        reserved = _time(window['reserved_at'], now)
        cleanup = _time(window['cleanup_at'], now) if window['cleanup_at'] is not None else None
        reconciled = _time(window['reconciled_at'], now) if window['reconciled_at'] is not None else None
        phase = window['phase']; reservation = reservations[rid]
        r0._need((phase in ACTIVE_PHASES and cleanup is None and reconciled is None
                  and reservation['cleanup_verified'] is False and reservation['status'] == 'outstanding')
                 or (phase in ('cleaned', 'overrun') and cleanup is not None and reconciled is None
                     and reservation['cleanup_verified'] is True and reservation['status'] == 'outstanding')
                 or (phase == 'reconciled' and cleanup is not None and reconciled is not None
                     and reservation['cleanup_verified'] is True and reservation['status'] == 'reconciled'),
                 'inconsistent_history')
        r0._need(cleanup is None or cleanup >= reserved, 'invalid_timestamp')
        r0._need(reconciled is None or reconciled >= cleanup, 'invalid_timestamp')
        if phase == 'overrun':
            overrun = r0._record(window['overrun'], {'actual_total_usd', 'actual_compute_usd', 'observed_at',
                'bound_exceeded', 'budget_exceeded'}, 'invalid_overrun')
            actual_total = r0._number(overrun['actual_total_usd'], 'invalid_overrun')
            actual_compute = r0._number(overrun['actual_compute_usd'], 'invalid_overrun')
            bound = (actual_total > r0._number(reservation['total_usd'], 'invalid_overrun')
                     or actual_compute > r0._number(reservation['compute_usd'], 'invalid_overrun'))
            budget = total > 15 or compute > 10
            r0._need(actual_compute <= actual_total <= total and actual_compute <= compute
                     and type(overrun['bound_exceeded']) is bool and type(overrun['budget_exceeded']) is bool
                     and overrun['bound_exceeded'] == bound and overrun['budget_exceeded'] == budget
                     and (bound or budget), 'invalid_overrun')
            observed = _time(overrun['observed_at'], now)
            r0._need(cleanup <= observed <= _time(ledger['observed_at'], now), 'invalid_overrun')
        uncertain |= phase in ACTIVE_PHASES
    r0._need(type(ledger['unresolved_cleanup']) is bool and ledger['unresolved_cleanup'] == uncertain,
             'inconsistent_cleanup')
    return record


class LedgerSession:
    def __init__(self, folder_fd, directory, binding, clock):
        self._fd, self._directory, self._binding, self._clock = folder_fd, directory, binding, clock
        self._lock_identity = self._lock_stat()
        self._now = clock()
        self._closed = False

    def _lock_stat(self):
        info = os.stat(r0.LOCK_NAME, dir_fd=self._fd, follow_symlinks=False)
        r0._private_file(info)
        return (info.st_dev, info.st_ino)

    def _ensure_directory(self):
        r0._need(not self._closed, 'session_closed')
        r0.assert_locked_directory(self._directory, self._fd)
        r0._need(self._lock_stat() == self._lock_identity, 'lock_identity_changed')

    def _load(self):
        self._ensure_directory()
        r0._need(not any(name == INTENT_NAME or name.startswith(TEMP_PREFIX) for name in os.listdir(self._fd)),
                 'interrupted_persistence')
        fd = os.open(STATE_NAME, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=self._fd)
        try:
            r0._private_file(os.fstat(fd))
            r0._need(os.fstat(fd).st_size <= LIMIT, 'state_too_large')
            with os.fdopen(fd, 'r', encoding='utf-8', closefd=False) as stream:
                value = stream.read(LIMIT + 1)
            r0._need(len(value) <= LIMIT, 'state_too_large')
            record = json.loads(value, parse_float=Decimal, object_pairs_hook=r0._unique_object,
                                parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
            self._ensure_directory()
            return _state(record, self._binding, self._now)
        finally:
            os.close(fd)

    def _persist(self, state):
        self._ensure_directory()
        state['revision'] += 1
        _state(state, self._binding, self._now)
        # Strict validated schema excludes secret fields/payloads before serialization.
        payload = json.dumps(state, sort_keys=True, separators=(',', ':'),
                             default=lambda value: str(value) if isinstance(value, Decimal) else None).encode()
        r0._need(len(payload) <= LIMIT, 'state_too_large')
        intent = os.open(INTENT_NAME, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=self._fd)
        try:
            r0._private_file(os.fstat(intent))
            os.write(intent, b'pending\n'); os.fsync(intent)
        finally:
            os.close(intent)
        os.fsync(self._fd)
        self._ensure_directory()
        temporary = TEMP_PREFIX + secrets.token_hex(16)
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=self._fd)
        try:
            r0._private_file(os.fstat(fd))
            with os.fdopen(fd, 'wb', closefd=False) as stream:
                stream.write(payload); stream.flush(); os.fsync(fd)
        finally:
            os.close(fd)
        self._ensure_directory()
        os.replace(temporary, STATE_NAME, src_dir_fd=self._fd, dst_dir_fd=self._fd)
        os.fsync(self._fd)
        self._ensure_directory()
        os.unlink(INTENT_NAME, dir_fd=self._fd)
        os.fsync(self._fd)
        self._ensure_directory()

    def _operation(self, change):
        try:
            with localcontext() as context:
                context.prec = 64
                self._now = self._clock()
                _stamp(self._now)
                state = self._load()
                blocked_reason = change(state)
                self._persist(state)
                return {'status': 'block' if blocked_reason else 'pass',
                        'reasons': [blocked_reason] if blocked_reason else [],
                        'live_enabled': False, 'revision': state['revision']}
        except r0.Blocked as error:
            return {'status': 'block', 'reasons': [error.code], 'live_enabled': False}
        except Exception:
            return {'status': 'block', 'reasons': ['ledger_unavailable_or_invalid'], 'live_enabled': False}

    def refresh_accounting(self, evidence):
        """Commit a fresh complete billing snapshot; never initialize/reset state.

        Caller supplies matching R0 observations separately before reserve. A
        denied later reserve cannot undo this refresh or the known spend.
        """
        def change(state):
            evidence_record = r0._record(evidence, {'binding', 'verified', 'observed_at',
                'charges_complete_through', 'reporting_delay_seconds', 'cumulative_total_usd',
                'cumulative_compute_usd'}, 'invalid_billing_evidence')
            r0._need(evidence_record['binding'] == self._binding.record() and evidence_record['verified'] is True,
                     'evidence_mismatch')
            r0._need(all(item['phase'] == 'reconciled' for item in state['windows'].values()), 'prior_window_pending')
            observed = r0._timestamp(evidence_record['observed_at'], self._now, Decimal(300), 'stale_billing_evidence')
            ledger = state['ledger']
            previous = _time(ledger['observed_at'], self._now)
            r0._need(observed >= previous, 'nonmonotonic_billing_evidence')
            delay = r0._number(evidence_record['reporting_delay_seconds'], 'unknown_billing')
            r0._need(delay <= 86400, 'unknown_billing')
            delay_us = int((delay * Decimal(1000000)).to_integral_value(rounding=ROUND_CEILING))
            through = _time(evidence_record['charges_complete_through'], self._now)
            r0._need(previous <= through <= observed - timedelta(microseconds=delay_us), 'billing_incomplete')
            total = r0._number(evidence_record['cumulative_total_usd'], 'unknown_billing')
            compute = r0._number(evidence_record['cumulative_compute_usd'], 'unknown_billing')
            old_total = r0._number(ledger['reconciled_total_usd'], 'unknown_billing')
            old_compute = r0._number(ledger['reconciled_compute_usd'], 'unknown_billing')
            r0._need(compute <= total and total >= old_total and compute >= old_compute
                     and total-old_total >= compute-old_compute, 'nonmonotonic_or_inconsistent_spend')
            ledger.update(reconciled_total_usd=str(total), reconciled_compute_usd=str(compute),
                remaining_total_usd=str(max(Decimal(0), Decimal(15)-total)),
                remaining_compute_usd=str(max(Decimal(0), Decimal(10)-compute)),
                observed_at=evidence_record['observed_at'])
        return self._operation(change)

    def reserve(self, reservation_id, observations, window):
        """Persist BEFORE future work; no execution capability returned by R1a."""
        def change(state):
            _id(reservation_id)
            r0._need(reservation_id not in state['windows'], 'duplicate_reservation')
            r0._need(all(item['phase'] == 'reconciled' for item in state['windows'].values()), 'prior_window_pending')
            r0._need(r0._number(state['ledger']['reconciled_total_usd'], 'unknown_or_invalid_budget') < 15
                     and r0._number(state['ledger']['reconciled_compute_usd'], 'unknown_or_invalid_budget') < 10,
                     'budget_exhausted')
            check = r0.preflight(state['ledger'], observations, window, now=self._now)
            if check['status'] != 'pass':
                raise r0.Blocked(check['reasons'][0])
            state['ledger']['reservations'].append({'id': reservation_id, 'period_id': self._binding.period_id,
                'total_usd': check['reservation_total_usd'], 'compute_usd': check['reservation_compute_usd'],
                'status': 'outstanding', 'cleanup_verified': False})
            state['ledger']['unresolved_cleanup'] = True
            state['windows'][reservation_id] = {'phase': 'reserved', 'reserved_at': _stamp(self._now),
                                                'cleanup_at': None, 'reconciled_at': None}
        return self._operation(change)

    def mark_active(self, reservation_id):
        def change(state):
            _id(reservation_id)
            r0._need(reservation_id in state['windows'] and state['windows'][reservation_id]['phase'] == 'reserved',
                     'invalid_transition')
            state['windows'][reservation_id]['phase'] = 'active'
        return self._operation(change)

    def mark_uncertain(self, reservation_id):
        def change(state):
            _id(reservation_id)
            r0._need(reservation_id in state['windows'] and state['windows'][reservation_id]['phase'] in ACTIVE_PHASES,
                     'invalid_transition')
            state['windows'][reservation_id]['phase'] = 'uncertain'
        return self._operation(change)

    def verify_cleanup(self, reservation_id, evidence):
        def change(state):
            _id(reservation_id)
            evidence_record = r0._record(evidence, {'binding', 'reservation_id', 'verified', 'observed_at',
                'no_active_queries', 'no_active_sessions', 'transactions_closed'}, 'invalid_cleanup_evidence')
            r0._need(evidence_record['binding'] == self._binding.record() and evidence_record['reservation_id'] == reservation_id,
                     'evidence_mismatch')
            r0._timestamp(evidence_record['observed_at'], self._now, Decimal(300), 'stale_cleanup_evidence')
            r0._need(all(evidence_record[k] is True for k in ('verified', 'no_active_queries', 'no_active_sessions',
                                                             'transactions_closed')), 'cleanup_unverified')
            r0._need(reservation_id in state['windows'] and state['windows'][reservation_id]['phase'] in ACTIVE_PHASES,
                     'invalid_transition')
            item = state['windows'][reservation_id]
            r0._need(_time(evidence_record['observed_at'], self._now) >= _time(item['reserved_at'], self._now), 'invalid_timestamp')
            item.update(phase='cleaned', cleanup_at=evidence_record['observed_at'])
            for reservation in state['ledger']['reservations']:
                if reservation['id'] == reservation_id:
                    reservation['cleanup_verified'] = True
            state['ledger']['unresolved_cleanup'] = any(w['phase'] in ACTIVE_PHASES for w in state['windows'].values())
        return self._operation(change)

    def reconcile(self, reservation_id, evidence):
        """Record trusted spend durably; overruns retain bounds and block admission."""
        def change(state):
            _id(reservation_id)
            evidence_record = r0._record(evidence, {'binding', 'reservation_id', 'verified', 'observed_at',
                'charges_complete_through', 'reporting_delay_seconds', 'actual_total_usd', 'actual_compute_usd',
                'cumulative_total_usd', 'cumulative_compute_usd'}, 'invalid_billing_evidence')
            r0._need(evidence_record['binding'] == self._binding.record() and evidence_record['reservation_id'] == reservation_id
                     and evidence_record['verified'] is True, 'evidence_mismatch')
            observed = r0._timestamp(evidence_record['observed_at'], self._now, Decimal(300), 'stale_billing_evidence')
            r0._need(reservation_id in state['windows'] and state['windows'][reservation_id]['phase'] == 'cleaned',
                     'cleanup_required')
            item = state['windows'][reservation_id]
            delay = r0._number(evidence_record['reporting_delay_seconds'], 'unknown_billing')
            r0._need(delay <= 86400, 'unknown_billing')
            through = _time(evidence_record['charges_complete_through'], self._now)
            delay_us = int((delay * Decimal(1000000)).to_integral_value(rounding=ROUND_CEILING))
            r0._need(_time(item['cleanup_at'], self._now) <= through <= observed - timedelta(microseconds=delay_us),
                     'billing_incomplete')
            actual_total, actual_compute, total, compute = [r0._number(evidence_record[k], 'unknown_billing') for k in
                ('actual_total_usd', 'actual_compute_usd', 'cumulative_total_usd', 'cumulative_compute_usd')]
            reservation = next(r for r in state['ledger']['reservations'] if r['id'] == reservation_id)
            r0._need(actual_compute <= actual_total, 'inconsistent_window_spend')
            bound_exceeded = (actual_total > r0._number(reservation['total_usd'], 'invalid_reservations')
                              or actual_compute > r0._number(reservation['compute_usd'], 'invalid_reservations'))
            ledger = state['ledger']
            r0._need(observed >= _time(ledger['observed_at'], self._now), 'nonmonotonic_billing_evidence')
            old_total = r0._number(ledger['reconciled_total_usd'], 'unknown_billing')
            old_compute = r0._number(ledger['reconciled_compute_usd'], 'unknown_billing')
            r0._need(compute <= total and total >= old_total and compute >= old_compute
                     and total-old_total >= actual_total and compute-old_compute >= actual_compute
                     and total-old_total >= compute-old_compute, 'nonmonotonic_or_inconsistent_spend')
            ledger.update(reconciled_total_usd=str(total), reconciled_compute_usd=str(compute),
                remaining_total_usd=str(max(Decimal(0), Decimal(15)-total)),
                remaining_compute_usd=str(max(Decimal(0), Decimal(10)-compute)),
                observed_at=evidence_record['observed_at'])
            budget_exceeded = total > 15 or compute > 10
            if bound_exceeded or budget_exceeded:
                item.update(phase='overrun', overrun={'actual_total_usd': str(actual_total),
                    'actual_compute_usd': str(actual_compute), 'observed_at': evidence_record['observed_at'],
                    'bound_exceeded': bound_exceeded, 'budget_exceeded': budget_exceeded})
                return 'reservation_overrun' if bound_exceeded else 'budget_overrun'
            reservation['status'] = 'reconciled'
            item.update(phase='reconciled', reconciled_at=evidence_record['observed_at'])
        return self._operation(change)


@contextmanager
def ledger_session(path: Path, binding: PilotBinding, *, clock=None) -> Iterator[LedgerSession]:
    """Explicit synthetic/existing state only. Hold the SAME stable R0 lock."""
    session = None
    try:
        clock = clock if clock is not None else lambda: datetime.now(timezone.utc)
        _stamp(clock())
        r0._need(isinstance(binding, PilotBinding), 'invalid_binding')
        binding.record()
        path = Path(path)
        r0._need(path.name == STATE_NAME, 'noncanonical_ledger_name')
        with r0.pilot_lock(path.parent) as borrowed_fd:
            session = LedgerSession(borrowed_fd, path.parent, binding, clock)
            try:
                yield session
            finally:
                session._closed = True
    except r0.Blocked:
        raise
    except Exception:
        raise r0.Blocked('ledger_unavailable_or_invalid') from None
    finally:
        if session is not None:
            session._closed = True
