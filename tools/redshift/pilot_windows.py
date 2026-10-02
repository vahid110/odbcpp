"""Finite USD150 window ledger; grants no live SQL permission by itself.

Explicit v3→v4 migration preserves allowance/control evidence and all v2 history.
Eight additional windows, each requiring durable reservation and fresh cleanup
before activation. Original resource identity and 24h bootstrap horizon remain
fixed. The cumulative continuous-compute bound retains unknown costs and all
minimum-metering/cleanup headroom; no idle-time refund or automatic retry exists.
The limited horizon is intentional: later periods need independently reviewed
billing/usage reconciliation, not silently extended bootstrap assumptions.
"""
import copy
from contextlib import contextmanager
from datetime import datetime, timezone
from decimal import Decimal, ROUND_CEILING
from pathlib import Path

from tools.redshift import pilot_allowance as a
from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_cumulative as c
from tools.redshift import pilot_preflight as r0


class WindowSession(c.CumulativeSession):
    def _reservation_other_tax(self):
        return Decimal(50)

    def _max_additional_metering(self):
        return Decimal(1200)

    def _allowance(self, state, allowance, now):
        previous = dict(state); previous[b.OVERLAY] = allowance
        validator = a.AllowanceSession(self.fd, self.directory, self.anchor, self.clock)
        try:
            o = validator._v3(previous, now)
        finally:
            validator.closed = True
        r0._need(o['controls_status'] == 'verified', 'allowance_controls_pending')
        return o

    def _prior_anchor(self, overlay):
        if overlay['attempts']:
            return b.BootstrapAnchor(**overlay['attempts'][-1]['anchor'])
        prior = overlay['allowance_overlay']['previous_overlay']
        return b.BootstrapAnchor(**prior['attempts'][-1]['anchor']) if prior['attempts'] else self.anchor

    def _amounts(self, deadline, metering, cleanup, floor_compute, floor_total, now):
        duration = b._seconds(b._time(deadline,now,True)-b._time(self.anchor.earliest_billable_start,now))
        r0._need(0 < duration <= 86400 and 300 <= metering <= 12000
                 and 60 <= cleanup <= 3000, 'invalid_cumulative_bound')
        compute = max(floor_compute,(Decimal(4)*Decimal('.374')*(duration+metering+cleanup)/3600
            ).quantize(Decimal('.01'),rounding=ROUND_CEILING))
        total = max(floor_total,compute+50)
        r0._need(compute<=100 and total<=150,'budget_exhausted')
        return compute,total

    def _v2(self, state, now):
        # Method name is the lifecycle base-class hook; only v4 is accepted.
        o = r0._record(state.get(b.OVERLAY), {'schema_version','allowance_overlay',
            'allowance_digest','legacy_digest','migration_cleanup_at','attempts',
            'compute_upper_bound_usd','total_upper_bound_usd','metering_seconds','cleanup_seconds'},
            'invalid_window_overlay')
        r0._need(type(o['schema_version']) is int and o['schema_version']==4
            and o['legacy_digest']==b._legacy_digest(state)
            and o['allowance_digest']==c._digest(o['allowance_overlay']), 'window_history_mismatch')
        allowance = self._allowance(state,o['allowance_overlay'],now)
        previous = allowance['previous_overlay']
        prior_attempts = previous['attempts']
        last_cleanup = prior_attempts[-1]['cleanup_at'] if prior_attempts else previous['migration_cleanup_at']
        r0._need(o['migration_cleanup_at']==last_cleanup,'window_history_mismatch')
        r0._need(isinstance(o['attempts'],list) and len(o['attempts'])<=c.MAX_ATTEMPTS,'invalid_attempts')
        metering = r0._number(previous['metering_seconds'],'invalid_cumulative_bound')
        cleanup = r0._number(previous['cleanup_seconds'],'invalid_cumulative_bound')
        compute = r0._number(allowance['compute_upper_bound_usd'],'invalid_cumulative_bound')
        total = r0._number(allowance['total_upper_bound_usd'],'invalid_cumulative_bound')
        deadline = prior_attempts[-1]['anchor']['hard_deadline'] if prior_attempts else self.anchor.hard_deadline
        for index,attempt in enumerate(o['attempts']):
            r0._record(attempt, {'sequence','anchor','reserved_at','phase','cleanup_at','activation_cleanup_at',
                'additional_metering_seconds','additional_cleanup_seconds','compute_upper_bound_usd',
                'total_upper_bound_usd'}, 'invalid_attempt')
            r0._need(type(attempt['sequence']) is int and attempt['sequence']==index+1,'window_history_mismatch')
            anchor = b.BootstrapAnchor(**attempt['anchor']); self._fixed(anchor,now)
            end = b._time(anchor.hard_deadline,now,True)
            reserved = b._time(attempt['reserved_at'],now)
            r0._need(end>=b._time(deadline,now,True) and end>reserved
                and reserved>=b._time(last_cleanup,now)
                and reserved>=b._time(allowance['amended_at'],now)
                and reserved>=b._time(allowance['control_observation']['observed_at'],now),
                'invalid_attempt_timestamp')
            am=r0._number(attempt['additional_metering_seconds'],'invalid_headroom')
            ac=r0._number(attempt['additional_cleanup_seconds'],'invalid_headroom')
            r0._need(60<=am<=1200 and 60<=ac<=300,'invalid_headroom')
            metering+=am;cleanup+=ac;deadline=anchor.hard_deadline
            compute,total=self._amounts(deadline,metering,cleanup,compute,total,now)
            r0._need(r0._number(attempt['compute_upper_bound_usd'],'invalid_cumulative_bound')==compute
                and r0._number(attempt['total_upper_bound_usd'],'invalid_cumulative_bound')==total,
                'window_history_mismatch')
            phase=attempt['phase']; activation=attempt['activation_cleanup_at']
            r0._need(phase in ('reserved','active','uncertain','cleaned_pending_billing'),'invalid_phase')
            r0._need((phase!='active' or activation is not None)
                and (phase!='reserved' or activation is None),'activation_cleanup_required')
            if activation is not None:
                r0._need(b._time(activation,now)>=reserved,'cleanup_time_regression')
            if phase=='cleaned_pending_billing':
                last_cleanup=attempt['cleanup_at']
                r0._need(b._time(last_cleanup,now)>=reserved
                    and (activation is None or b._time(last_cleanup,now)>=b._time(activation,now)),
                    'cleanup_time_regression')
            else:
                r0._need(attempt['cleanup_at'] is None and index==len(o['attempts'])-1,'prior_attempt_unresolved')
        for key,value in (('compute_upper_bound_usd',compute),('total_upper_bound_usd',total),
                          ('metering_seconds',metering),('cleanup_seconds',cleanup)):
            r0._need(r0._number(o[key],'invalid_cumulative_bound')==value,'window_history_mismatch')
        return o

    def initialize(self):
        def change(state,now):
            allowance=self._allowance(state,state.get(b.OVERLAY),now)
            previous=allowance['previous_overlay']
            last=previous['attempts'][-1]['cleanup_at'] if previous['attempts'] else previous['migration_cleanup_at']
            state[b.OVERLAY]=dict(schema_version=4,allowance_overlay=copy.deepcopy(allowance),
                allowance_digest=c._digest(allowance),legacy_digest=b._legacy_digest(state),
                migration_cleanup_at=last,attempts=[],
                compute_upper_bound_usd=allowance['compute_upper_bound_usd'],
                total_upper_bound_usd=allowance['total_upper_bound_usd'],
                metering_seconds=previous['metering_seconds'],cleanup_seconds=previous['cleanup_seconds'])
            self._v2(state,now)
        return self._operate(change)

    def migrate(self, *args, **kwargs):
        raise r0.Blocked('use_explicit_window_initialize')


@contextmanager
def window_session(path, original_anchor, *, clock=None):
    path=Path(path);r0._need(path.name=='setup.json','invalid_state_path')
    with r0.pilot_lock(path.parent) as fd:
        session=WindowSession(fd,path.parent,original_anchor,clock or (lambda:datetime.now(timezone.utc)))
        try:
            yield session
        finally:
            session.closed=True
