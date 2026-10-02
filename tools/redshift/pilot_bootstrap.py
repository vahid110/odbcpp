"""Offline one-shot overlay for legacy setup.json; never a live launcher.

Explicit initialization requires a separately protected immutable operator anchor
assigning the existing initial pilot period. It preserves EVERY legacy field,
including null actual spend/remaining and history; it does not verify legacy
billing or invent a zero balance. The overlay in this SAME setup.json is the sole
bootstrap reservation authority, not another ledger. Future migration/reconciliation
must be explicit and preserve both legacy history and this outstanding bound.

Caller-trusted readback syntax is not AWS authentication. Reserve covers earliest
possible billable creation through an immutable hard deadline, plus minimum
metering and verified cleanup headroom and an explicit other/tax bound. A passed
result is offline accounting only, never execution permission. No retry, billing
refund, second window, initializer reset, CLI, cloud or credential access exists.

R0 lends the exact locked directory fd. Intent + exclusive 0600 temp + fsync +
replace + directory fsync preserve crash uncertainty; orphan intent/temp blocks.
Checkpoint inode guards require cooperative administrators not to rename paths or
unlink locks; they do not sandbox a malicious process of the same uid.
"""
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
from decimal import Decimal, ROUND_CEILING, localcontext
import hashlib
import json
import os
from pathlib import Path
import re
import secrets

from tools.redshift import pilot_preflight as r0

OVERLAY = 'bootstrap_overlay'
INTENT = '.pilot-bootstrap.intent'
TEMP = '.pilot-bootstrap.tmp-'
LIMIT = 1024 * 1024


def _encode(value):
    # Preserve JSON numeric Decimal values as numbers, never strings or floats.
    if isinstance(value, Decimal):
        r0._need(value.is_finite() and len(value.as_tuple().digits) <= LIMIT, 'invalid_state')
        return str(value)
    if isinstance(value, dict):
        return '{' + ','.join(json.dumps(k) + ':' + _encode(v) for k, v in sorted(value.items())) + '}'
    if isinstance(value, list):
        return '[' + ','.join(_encode(v) for v in value) + ']'
    return json.dumps(value, allow_nan=False)


def _seconds(delta):
    return Decimal(delta.days) * 86400 + Decimal(delta.seconds) + Decimal(delta.microseconds) / 1000000


def _time(value, now, future=False):
    if future:
        r0._need(isinstance(value, str) and len(value) <= 40 and value.endswith('Z'), 'invalid_timestamp')
        try:
            result = datetime.fromisoformat(value[:-1] + '+00:00')
        except (ValueError, TypeError):
            raise r0.Blocked('invalid_timestamp') from None
        r0._need(result.utcoffset().total_seconds() == 0, 'invalid_timestamp')
        return result
    return r0._timestamp(value, now, Decimal('999999999'), 'invalid_timestamp')


@dataclass(frozen=True)
class BootstrapAnchor:
    """Caller must load from a protected immutable operator-approved anchor."""
    pilot_id: str
    period_id: str
    account_id: str
    workgroup_id: str
    namespace_id: str
    earliest_billable_start: str
    hard_deadline: str
    operator_assigned_existing_period: bool
    region: str = 'eu-north-1'
    total_limit_usd: str = '15'
    compute_limit_usd: str = '10'

    def record(self, now):
        r0._need(self.pilot_id == 'initial-redshift-pilot' and self.period_id == 'initial-20261002'
                 and self.operator_assigned_existing_period is True, 'invalid_anchor')
        r0._need(isinstance(self.account_id, str) and re.fullmatch(r'\d{12}', self.account_id) is not None
                 and self.region == 'eu-north-1', 'invalid_anchor')
        for value in (self.workgroup_id, self.namespace_id):
            r0._need(isinstance(value, str) and re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', value) is not None,
                     'invalid_anchor')
        r0._need(r0._number(self.total_limit_usd, 'invalid_caps') == 15
                 and r0._number(self.compute_limit_usd, 'invalid_caps') == 10, 'invalid_caps')
        start, end = _time(self.earliest_billable_start, now), _time(self.hard_deadline, now, True)
        r0._need(start < end and _seconds(end-start) <= 86400, 'invalid_lifecycle')
        return dict(self.__dict__)


def _legacy(state, anchor):
    r0._need(isinstance(state, dict) and state.get('account_id') == anchor.account_id
             and state.get('region') == anchor.region, 'legacy_identity_mismatch')
    r0._need(r0._number(state.get('approved_total_usd'), 'invalid_caps') == 15
             and r0._number(state.get('compute_envelope_usd'), 'invalid_caps') == 10
             and r0._number(state.get('other_charges_and_tax_reserve_usd'), 'invalid_caps') == 5, 'invalid_caps')
    r0._need(r0._number(state.get('reserved_compute_usd'), 'existing_usage') == 0
             and state.get('usage_windows') == [] and state.get('live_testing_enabled') is False, 'existing_usage')
    # Known spend is not compatible with this bounded unknown-spend bootstrap.
    r0._need('actual_spend_usd_verified' in state and state['actual_spend_usd_verified'] is None
             and 'remaining_allowance_usd_verified' in state and state['remaining_allowance_usd_verified'] is None,
             'incompatible_accounting')


def _legacy_digest(state):
    return hashlib.sha256(_encode({k: v for k, v in state.items() if k != OVERLAY}).encode()).hexdigest()


class BootstrapSession:
    def __init__(self, fd, directory, anchor, clock):
        self.fd, self.directory, self.anchor, self.clock = fd, directory, anchor, clock
        self.closed = False
        self.lock_identity = self._lock()

    def _lock(self):
        info = os.stat(r0.LOCK_NAME, dir_fd=self.fd, follow_symlinks=False)
        r0._private_file(info)
        return info.st_dev, info.st_ino

    def _guard(self):
        r0._need(not self.closed, 'session_closed')
        r0.assert_locked_directory(self.directory, self.fd)
        r0._need(self._lock() == self.lock_identity, 'lock_identity_changed')

    def _load(self):
        self._guard()
        r0._need(not any(n in (INTENT, '.pilot-ledger.intent') or n.startswith((TEMP, '.pilot-ledger.tmp-'))
                         for n in os.listdir(self.fd)), 'interrupted_persistence')
        fd = os.open('setup.json', os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=self.fd)
        try:
            r0._private_file(os.fstat(fd))
            r0._need(os.fstat(fd).st_size <= LIMIT, 'state_too_large')
            with os.fdopen(fd, 'r', closefd=False) as stream:
                raw = stream.read(LIMIT+1)
            r0._need(len(raw) <= LIMIT, 'state_too_large')
            state = json.loads(raw, parse_float=Decimal, object_pairs_hook=r0._unique_object,
                parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
        finally:
            os.close(fd)
        self._guard()
        _legacy(state, self.anchor)
        return state

    def _overlay(self, state, now):
        o = r0._record(state.get(OVERLAY), {'schema_version', 'anchor', 'legacy_digest', 'window'}, 'overlay_missing_or_invalid')
        r0._need(type(o['schema_version']) is int and o['schema_version'] == 1
                 and o['anchor'] == self.anchor.record(now) and o['legacy_digest'] == _legacy_digest(state), 'overlay_mismatch')
        w = o['window']
        if w is not None:
            r0._record(w, {'phase', 'reserved_at', 'cleanup_at', 'compute_upper_bound_usd',
                'total_upper_bound_usd', 'metering_seconds', 'cleanup_seconds', 'other_tax_usd'}, 'invalid_window')
            r0._need(w['phase'] in ('reserved', 'active', 'uncertain', 'cleaned_pending_billing'), 'invalid_phase')
            reserved = _time(w['reserved_at'], now)
            cleaned = _time(w['cleanup_at'], now) if w['cleanup_at'] is not None else None
            r0._need((w['phase'] == 'cleaned_pending_billing' and cleaned is not None and cleaned >= reserved)
                     or (w['phase'] != 'cleaned_pending_billing' and cleaned is None), 'invalid_window')
            compute, total = self._bound(now, w['metering_seconds'], w['cleanup_seconds'], w['other_tax_usd'])
            r0._need(r0._number(w['compute_upper_bound_usd'], 'invalid_window') == compute
                     and r0._number(w['total_upper_bound_usd'], 'invalid_window') == total, 'invalid_window')
        return o

    def _bound(self, now, metering, cleanup, other):
        self.anchor.record(now)
        metering = r0._number(metering, 'invalid_headroom')
        cleanup = r0._number(cleanup, 'invalid_headroom', positive=True)
        other = r0._number(other, 'invalid_headroom', positive=True)
        # Unknown billing cannot justify releasing any of the approved USD5
        # non-compute/tax envelope on the strength of a boolean observation.
        r0._need(60 <= metering <= 300 and cleanup <= 3600 and other == 5, 'invalid_headroom')
        seconds = _seconds(_time(self.anchor.hard_deadline, now, True)
            - _time(self.anchor.earliest_billable_start, now))
        compute = (Decimal(4) * Decimal('.374') * (seconds+metering+cleanup) / 3600).quantize(Decimal('.01'), rounding=ROUND_CEILING)
        total = (compute+other).quantize(Decimal('.01'), rounding=ROUND_CEILING)
        r0._need(compute <= 10 and total <= 15, 'budget_exhausted')
        return compute, total

    def _persist(self, state):
        self._guard()
        payload = _encode(state).encode()
        r0._need(len(payload) <= LIMIT, 'state_too_large')
        fd = os.open(INTENT, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=self.fd)
        try:
            r0._private_file(os.fstat(fd)); os.write(fd, b'pending\n'); os.fsync(fd)
        finally:
            os.close(fd)
        os.fsync(self.fd); self._guard()
        temp = TEMP + secrets.token_hex(16)
        fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=self.fd)
        try:
            r0._private_file(os.fstat(fd))
            with os.fdopen(fd, 'wb', closefd=False) as stream:
                stream.write(payload); stream.flush(); os.fsync(fd)
        finally:
            os.close(fd)
        self._guard()
        os.replace(temp, 'setup.json', src_dir_fd=self.fd, dst_dir_fd=self.fd)
        os.fsync(self.fd); self._guard()
        os.unlink(INTENT, dir_fd=self.fd); os.fsync(self.fd); self._guard()

    def _operate(self, change):
        try:
            with localcontext() as ctx:
                ctx.prec = 64
                now = self.clock()
                r0._need(isinstance(now, datetime) and now.tzinfo is not None and now.utcoffset().total_seconds() == 0, 'invalid_clock')
                self.anchor.record(now)
                state = self._load()
                change(state, now)
                self._persist(state)
            return {'status': 'pass', 'reasons': [], 'live_enabled': False}
        except r0.Blocked as error:
            return {'status': 'block', 'reasons': [error.code], 'live_enabled': False}
        except Exception:
            return {'status': 'block', 'reasons': ['bootstrap_unavailable_or_invalid'], 'live_enabled': False}

    def initialize(self):
        """Explicit operator action only; no implicit call from reserve/session."""
        def change(state, now):
            r0._need(OVERLAY not in state, 'already_initialized')
            state[OVERLAY] = {'schema_version': 1, 'anchor': self.anchor.record(now),
                             'legacy_digest': _legacy_digest(state), 'window': None}
        return self._operate(change)

    def reserve(self, evidence, *, metering_seconds, cleanup_seconds, other_tax_usd):
        def change(state, now):
            o = self._overlay(state, now)
            r0._need(o['window'] is None, 'bootstrap_window_already_used')
            e = r0._record(evidence, {'verified', 'observed_at', 'anchor', 'base_rpus', 'max_rpus',
                'usd_per_rpu_hour', 'cleanup_bound_seconds', 'earliest_start_verified',
                'no_other_billable_activity', 'other_tax_bound_verified'}, 'invalid_evidence')
            r0._need(e['verified'] is True and e['anchor'] == self.anchor.record(now)
                     and e['earliest_start_verified'] is True and e['no_other_billable_activity'] is True
                     and e['other_tax_bound_verified'] is True, 'evidence_unverified')
            r0._timestamp(e['observed_at'], now, Decimal(300), 'stale_evidence')
            r0._need(r0._number(e['base_rpus'], 'invalid_capacity') == 4
                     and r0._number(e['max_rpus'], 'invalid_capacity') == 4
                     and r0._number(e['usd_per_rpu_hour'], 'invalid_rate') == Decimal('.374'), 'invalid_capacity_or_rate')
            r0._need(_time(self.anchor.hard_deadline, now, True) > now, 'deadline_expired')
            r0._need(r0._number(e['cleanup_bound_seconds'], 'invalid_headroom', positive=True)
                     <= r0._number(cleanup_seconds, 'invalid_headroom', positive=True), 'insufficient_cleanup_headroom')
            compute, total = self._bound(now, metering_seconds, cleanup_seconds, other_tax_usd)
            o['window'] = dict(phase='reserved', reserved_at=now.isoformat().replace('+00:00','Z'), cleanup_at=None,
                compute_upper_bound_usd=str(compute), total_upper_bound_usd=str(total),
                metering_seconds=str(r0._number(metering_seconds, 'invalid_headroom')),
                cleanup_seconds=str(r0._number(cleanup_seconds, 'invalid_headroom')),
                other_tax_usd=str(r0._number(other_tax_usd, 'invalid_headroom')))
        return self._operate(change)

    def transition(self, phase, evidence=None):
        def change(state, now):
            o = self._overlay(state, now); w = o['window']
            r0._need(w is not None and w['phase'] in ('reserved', 'active', 'uncertain'), 'invalid_transition')
            r0._need(phase in ('active', 'uncertain', 'cleaned_pending_billing')
                     and (phase != 'active' or w['phase'] == 'reserved'), 'invalid_transition')
            if phase == 'active':
                r0._need(_time(self.anchor.hard_deadline, now, True) > now, 'deadline_expired')
            if phase == 'cleaned_pending_billing':
                e = r0._record(evidence, {'verified', 'observed_at', 'anchor', 'no_active_queries',
                    'no_active_sessions', 'transactions_closed'}, 'invalid_cleanup_evidence')
                r0._need(e['verified'] is True and e['anchor'] == self.anchor.record(now)
                         and all(e[k] is True for k in ('no_active_queries','no_active_sessions','transactions_closed')), 'cleanup_unverified')
                when = r0._timestamp(e['observed_at'], now, Decimal(300), 'stale_cleanup')
                r0._need(when >= _time(w['reserved_at'], now), 'invalid_timestamp')
                w['cleanup_at'] = e['observed_at']
            w['phase'] = phase
        return self._operate(change)


@contextmanager
def bootstrap_session(path, anchor, *, clock=None):
    """Explicit path only; synthetic tests must use a private temporary directory."""
    path = Path(path)
    r0._need(path.name == 'setup.json', 'invalid_state_path')
    with r0.pilot_lock(path.parent) as fd:
        session = BootstrapSession(fd, path.parent, anchor, clock or (lambda: datetime.now(timezone.utc)))
        try:
            yield session
        finally:
            session.closed = True
