"""Offline one-window type qualification successor; no runtime entry point.

V8 freezes complete cleaned v7 bytes. The original period, resource identity,
unknown billing and all liability remain immutable; only one explicit type-case
reservation is allowed. Old v7 validators and launchers remain unchanged.
"""
from contextlib import contextmanager
from datetime import datetime, timezone
from decimal import Decimal, ROUND_CEILING
from pathlib import Path
import copy
import re

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_finite_amendment as prior
from tools.redshift import pilot_preflight as r0
from tools.redshift import pilot_recovery as r

INVENTORY = ['RedshiftRealTest.VersionQuery',
    'RedshiftRealTest.IntegerBoundariesAndNarrowing',
    'RedshiftRealTest.ExactDecimalAndNull', 'RedshiftRealTest.UnicodeRoundTrip',
    'RedshiftRealTest.TemporalExactAndNull',
    'RedshiftRealTest.TypedNullAndOutputPreservation']
REVIEW_KEYS = {'record_type', 'reviewed', 'observed_at', 'source_state_digest',
    'source_code_digest', 'evidence_digest', 'unknown_billing_bound_accepted',
    'controls_verified', 'no_other_resources_or_activity', 'network_verified',
    'price_verified', 'original_horizon_only', 'no_refunds', 'inventory',
    'max_new_windows', 'metering_ceiling_seconds', 'cleanup_ceiling_seconds'}


class TypeQualificationSession(prior.FiniteAmendmentSession):
    def _source_continuation(self, raw, now):
        state = r.decode(raw)
        validator = prior.FiniteAmendmentSession(self.fd, self.directory, self.anchor, self.clock)
        try:
            overlay = validator._v2(state, now)
        finally:
            validator.closed = True
        r0._need(len(overlay['attempts']) == 1 and all(
            x['phase'] == 'cleaned_pending_billing' for x in overlay['attempts']),
            'exhausted_clean_v7_required')
        r0._need(r0._number(overlay['metering_seconds'], 'invalid_headroom') == 12360
            and r0._number(overlay['cleanup_seconds'], 'invalid_headroom') == 720,
            'amendment_baseline_mismatch')
        return state, overlay

    def _review(self, record, digest, now, *, fresh):
        e = r0._record(record, REVIEW_KEYS, 'invalid_amendment_review')
        r0._need(e['record_type'] == 'independent_conservative_accounting_review'
            and all(e[k] is True for k in ('reviewed', 'unknown_billing_bound_accepted',
                'controls_verified', 'no_other_resources_or_activity', 'network_verified',
                'price_verified', 'original_horizon_only', 'no_refunds'))
            and e['source_state_digest'] == digest and e['inventory'] == INVENTORY,
            'amendment_review_mismatch')
        for key in ('source_state_digest', 'source_code_digest', 'evidence_digest'):
            r0._need(isinstance(e[key], str) and re.fullmatch('[0-9a-f]{64}', e[key]),
                'invalid_review_digest')
        for key, value in (('max_new_windows', 1), ('metering_ceiling_seconds', 13560),
                           ('cleanup_ceiling_seconds', 780)):
            r0._need(type(e[key]) is int and e[key] == value, 'invalid_amendment_scope')
        if fresh:
            r0._timestamp(e['observed_at'], now, Decimal(300), 'stale_amendment_review')
        else:
            b._time(e['observed_at'], now)
        return e

    def _prior_anchor(self, overlay):
        if overlay['attempts']:
            return b.BootstrapAnchor(**overlay['attempts'][-1]['anchor'])
        _, previous = self._source_continuation(overlay['source_continuation_raw'], self.clock())
        return b.BootstrapAnchor(**previous['attempts'][-1]['anchor'])

    def next_sequence(self, overlay):
        return 10

    def _attempt_limit(self):
        return 1

    def _amounts(self, deadline, metering, cleanup, floor_compute, floor_total, now):
        duration = b._seconds(b._time(deadline, now, True)
            - b._time(self.anchor.earliest_billable_start, now))
        r0._need(0 < duration <= 86400 and 12360 <= metering <= 13560
            and 720 <= cleanup <= 780, 'invalid_amended_bound')
        compute = max(floor_compute, (Decimal(4)*Decimal('.374')
            * (duration+metering+cleanup)/3600).quantize(Decimal('.01'), rounding=ROUND_CEILING))
        total = max(floor_total, compute+50)
        r0._need(compute <= 100 and total <= 150, 'budget_exhausted')
        return compute, total

    def _v2(self, state, now):
        o = r0._record(state.get(b.OVERLAY), {'schema_version', 'source_continuation_raw',
            'source_continuation_digest', 'legacy_digest', 'review', 'review_digest',
            'amended_at', 'migration_cleanup_at', 'attempts', 'metering_seconds',
            'cleanup_seconds', 'compute_upper_bound_usd', 'total_upper_bound_usd'},
            'invalid_finite_amendment')
        r0._need(type(o['schema_version']) is int and o['schema_version'] == 8
            and o['source_continuation_digest'] == r.digest(o['source_continuation_raw'])
            and o['legacy_digest'] == b._legacy_digest(state)
            and o['review_digest'] == r.digest(b._encode(o['review'])), 'amendment_history_mismatch')
        original, previous = self._source_continuation(o['source_continuation_raw'], now)
        r0._need(b._legacy_digest(original) == o['legacy_digest'], 'amendment_history_mismatch')
        review = self._review(o['review'], o['source_continuation_digest'], now, fresh=False)
        cleaned = previous['attempts'][-1]['cleanup_at']
        amended = b._time(o['amended_at'], now)
        r0._need(o['migration_cleanup_at'] == cleaned and amended >= b._time(cleaned, now)
            and amended >= b._time(review['observed_at'], now), 'amendment_time_mismatch')
        r0._need(isinstance(o['attempts'], list) and len(o['attempts']) <= 1, 'attempt_limit')
        metering = Decimal(12360); cleanup = Decimal(720)
        compute = r0._number(previous['compute_upper_bound_usd'], 'invalid_bound')
        total = r0._number(previous['total_upper_bound_usd'], 'invalid_bound')
        for item in o['attempts']:
            r0._record(item, {'sequence', 'anchor', 'reserved_at', 'phase', 'cleanup_at',
                'activation_cleanup_at', 'additional_metering_seconds', 'additional_cleanup_seconds',
                'compute_upper_bound_usd', 'total_upper_bound_usd'}, 'invalid_attempt')
            r0._need(type(item['sequence']) is int and item['sequence'] == 10, 'amendment_history_mismatch')
            anchor = b.BootstrapAnchor(**item['anchor']); self._fixed(anchor, now)
            reserved = b._time(item['reserved_at'], now)
            end = b._time(anchor.hard_deadline, now, True)
            r0._need(reserved >= amended and reserved >= b._time(cleaned, now)
                and end > reserved and end >= b._time(previous['attempts'][-1]['anchor']['hard_deadline'], now, True),
                'invalid_attempt_timestamp')
            r0._need(r0._number(item['additional_metering_seconds'], 'invalid_headroom') == 1200
                and r0._number(item['additional_cleanup_seconds'], 'invalid_headroom') == 60, 'invalid_headroom')
            metering += 1200; cleanup += 60
            compute, total = self._amounts(anchor.hard_deadline, metering, cleanup, compute, total, now)
            phase = item['phase']; activation = item['activation_cleanup_at']
            r0._need(phase in ('reserved', 'active', 'uncertain', 'cleaned_pending_billing'), 'invalid_phase')
            r0._need((phase != 'active' or activation is not None)
                and (phase != 'reserved' or activation is None), 'activation_cleanup_required')
            if activation is not None:
                r0._need(b._time(activation, now) >= reserved, 'cleanup_time_regression')
            if phase == 'cleaned_pending_billing':
                r0._need(b._time(item['cleanup_at'], now) >= reserved
                    and (activation is None or b._time(item['cleanup_at'], now) >= b._time(activation, now)),
                    'cleanup_time_regression')
            else:
                r0._need(item['cleanup_at'] is None, 'invalid_cleanup_state')
            r0._need(r0._number(item['compute_upper_bound_usd'], 'invalid_bound') == compute
                and r0._number(item['total_upper_bound_usd'], 'invalid_bound') == total, 'amendment_history_mismatch')
        for key, value in (('metering_seconds', metering), ('cleanup_seconds', cleanup),
                           ('compute_upper_bound_usd', compute), ('total_upper_bound_usd', total)):
            r0._need(r0._number(o[key], 'invalid_bound') == value, 'amendment_history_mismatch')
        return o

    def initialize(self, expected_source_digest, review):
        def change(state, now):
            raw = self._read_raw('setup.json')
            r0._need(r.digest(raw) == expected_source_digest and r.decode(raw) == state,
                'amendment_source_changed')
            _, previous = self._source_continuation(raw, now)
            self._review(review, expected_source_digest, now, fresh=True)
            state[b.OVERLAY] = dict(schema_version=8, source_continuation_raw=raw,
                source_continuation_digest=expected_source_digest, legacy_digest=b._legacy_digest(state),
                review=copy.deepcopy(review), review_digest=r.digest(b._encode(review)),
                amended_at=now.isoformat().replace('+00:00', 'Z'),
                migration_cleanup_at=previous['attempts'][-1]['cleanup_at'], attempts=[],
                **{k: previous[k] for k in ('metering_seconds', 'cleanup_seconds',
                    'compute_upper_bound_usd', 'total_upper_bound_usd')})
            self._v2(state, now)
        return self._operate(change)

    # Reuse reservation source-binding, fixed readback and post-reservation
    # cleanup gates. The v8 validator enforces ONE attempt before any persistence.
    # Shared lifecycle transitions also validate v8 before and after mutation.


@contextmanager
def type_qualification_session(path, original_anchor, *, clock=None):
    path = Path(path); r0._need(path.name == 'setup.json', 'invalid_state_path')
    with r0.pilot_lock(path.parent) as fd:
        session = TypeQualificationSession(fd, path.parent, original_anchor,
            clock or (lambda: datetime.now(timezone.utc)))
        try:
            yield session
        finally:
            session.closed = True
