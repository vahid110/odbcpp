"""Offline explicit same-file cumulative pilot admission, never a live launcher.

V2 preserves the entire v1 overlay and legacy subtree. Original protected anchor,
price .374, 4/4 capacity, creation, IDs, period and 15/10 caps remain fixed.
Migration requires preserved independently verified cleanup and carries four additional 60s
usage-start allowances (including three post-window cleanup calls). Every finite
new attempt adds >=60s metering AND >=60s cleanup, never returns them on cleanup.
One monotonically increasing lifecycle reservation covers overlapping time once;
unknown actual spend/remaining remain null. No billing refund, reset or auto retry.
Historical cleanup permits reservation only; active requires fresh matching cleanup
after durable reservation, so paid cleanup SQL is never an unreserved preflight.
Caller evidence syntax is not independent verification: the integration owner must
provide protected anchors and fresh AWS/control/cleanup observations. No result
allows execution. Borrowed R0 fd and bootstrap intent/fsync protocol are reused;
interrupted writes and directory/lock divergence fail closed. Migration/recovery
and corrected SQL execution are explicit operator actions, not scheduler retries.
"""
import copy
from contextlib import contextmanager
from datetime import datetime, timezone
from decimal import Decimal, ROUND_CEILING
import hashlib

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_preflight as r0

MAX_ATTEMPTS = 8


def _digest(record):
    return hashlib.sha256(b._encode(record).encode()).hexdigest()


class CumulativeSession(b.BootstrapSession):
    def _cleanup(self, evidence, anchor, now, after, *, fresh=True):
        e = r0._record(evidence, {'verified','observed_at','anchor','no_active_queries',
            'no_active_sessions','transactions_closed'}, 'invalid_cleanup_evidence')
        r0._need(e['verified'] is True and e['anchor'] == anchor.record(now)
                 and all(e[k] is True for k in ('no_active_queries','no_active_sessions','transactions_closed')),
                 'cleanup_unverified')
        observed = (r0._timestamp(e['observed_at'], now, Decimal(300), 'stale_cleanup')
                    if fresh else b._time(e['observed_at'],now))
        r0._need(observed >= b._time(after,now), 'cleanup_time_regression')
        return e['observed_at']

    def _fixed(self, anchor, now):
        record = anchor.record(now)
        original = self.anchor.record(now)
        r0._need({k:v for k,v in record.items() if k != 'hard_deadline'} ==
                 {k:v for k,v in original.items() if k != 'hard_deadline'}, 'binding_mismatch')
        return record

    def _amounts(self, deadline, metering, cleanup, floor_compute, floor_total, now):
        duration = b._seconds(b._time(deadline,now,True)-b._time(self.anchor.earliest_billable_start,now))
        r0._need(0 < duration <= 86400 and 300 <= metering <= 3000
                 and 60 <= cleanup <= 3000, 'invalid_cumulative_bound')
        compute = max(floor_compute, (Decimal(4)*Decimal('.374')*(duration+metering+cleanup)/3600
            ).quantize(Decimal('.01'),rounding=ROUND_CEILING))
        total = max(floor_total, compute+5)
        r0._need(compute <= 10 and total <= 15, 'budget_exhausted')
        return compute,total

    def _v2(self, state, now):
        o = r0._record(state.get(b.OVERLAY), {'schema_version','original_bootstrap','original_digest',
            'legacy_digest','migration_cleanup_at','attempts','compute_upper_bound_usd',
            'total_upper_bound_usd','metering_seconds','cleanup_seconds'}, 'invalid_cumulative_overlay')
        r0._need(type(o['schema_version']) is int and o['schema_version'] == 2
                 and o['legacy_digest'] == b._legacy_digest(state)
                 and o['original_digest'] == _digest(o['original_bootstrap']), 'cumulative_history_mismatch')
        v1state = dict(state); v1state[b.OVERLAY] = o['original_bootstrap']
        original = super()._overlay(v1state,now)
        w = original['window']
        r0._need(w is not None and w['phase'] == 'cleaned_pending_billing', 'original_cleanup_required')
        migration = b._time(o['migration_cleanup_at'],now)
        r0._need(migration >= b._time(w['cleanup_at'],now), 'cleanup_time_regression')
        r0._need(isinstance(o['attempts'],list) and len(o['attempts']) <= MAX_ATTEMPTS, 'invalid_attempts')
        metering = r0._number(w['metering_seconds'],'invalid_cumulative_bound')+240
        cleanup = r0._number(w['cleanup_seconds'],'invalid_cumulative_bound')
        deadline = self.anchor.hard_deadline
        compute,total = self._amounts(deadline,metering,cleanup,
            r0._number(w['compute_upper_bound_usd'],'invalid_cumulative_bound'),
            r0._number(w['total_upper_bound_usd'],'invalid_cumulative_bound'),now)
        last_cleanup = o['migration_cleanup_at']
        for i,a in enumerate(o['attempts']):
            r0._record(a, {'sequence','anchor','reserved_at','phase','cleanup_at','activation_cleanup_at',
                'additional_metering_seconds','additional_cleanup_seconds','compute_upper_bound_usd',
                'total_upper_bound_usd'}, 'invalid_attempt')
            r0._need(type(a['sequence']) is int and a['sequence']==i+1, 'cumulative_history_mismatch')
            anchor = b.BootstrapAnchor(**a['anchor']); self._fixed(anchor,now)
            end = b._time(anchor.hard_deadline,now,True)
            reserved = b._time(a['reserved_at'],now)
            r0._need(end >= b._time(deadline,now,True) and end > reserved
                     and reserved >= b._time(last_cleanup,now), 'invalid_attempt_timestamp')
            am = r0._number(a['additional_metering_seconds'],'invalid_headroom')
            ac = r0._number(a['additional_cleanup_seconds'],'invalid_headroom')
            r0._need(60 <= am <= 300 and 60 <= ac <= 300, 'invalid_headroom')
            metering += am; cleanup += ac; deadline=anchor.hard_deadline
            compute,total = self._amounts(deadline,metering,cleanup,compute,total,now)
            r0._need(r0._number(a['compute_upper_bound_usd'],'invalid_cumulative_bound') == compute
                     and r0._number(a['total_upper_bound_usd'],'invalid_cumulative_bound') == total,
                     'cumulative_history_mismatch')
            r0._need(a['phase'] in ('reserved','active','uncertain','cleaned_pending_billing'), 'invalid_phase')
            activation = a['activation_cleanup_at']
            r0._need((a['phase']!='active' or activation is not None)
                     and (a['phase']!='reserved' or activation is None), 'activation_cleanup_required')
            if activation is not None:
                r0._need(b._time(activation,now)>=reserved, 'cleanup_time_regression')
            if a['phase']=='cleaned_pending_billing':
                last_cleanup=a['cleanup_at']
                r0._need(b._time(last_cleanup,now)>=reserved
                         and (activation is None or b._time(last_cleanup,now)>=b._time(activation,now)),
                         'cleanup_time_regression')
            else:
                r0._need(a['cleanup_at'] is None and i==len(o['attempts'])-1, 'prior_attempt_unresolved')
        r0._need(r0._number(o['metering_seconds'],'invalid_cumulative_bound')==metering
                 and r0._number(o['cleanup_seconds'],'invalid_cumulative_bound')==cleanup
                 and r0._number(o['compute_upper_bound_usd'],'invalid_cumulative_bound')==compute
                 and r0._number(o['total_upper_bound_usd'],'invalid_cumulative_bound')==total,
                 'cumulative_history_mismatch')
        return o

    def migrate(self, cleanup_evidence):
        """Explicit one-time v1→v2; never called implicitly by reserve."""
        def change(state,now):
            original=super(CumulativeSession,self)._overlay(state,now)
            w=original['window']
            r0._need(w is not None and w['phase']=='cleaned_pending_billing', 'original_cleanup_required')
            observed=self._cleanup(cleanup_evidence,self.anchor,now,w['cleanup_at'],fresh=False)
            metering=r0._number(w['metering_seconds'],'invalid_headroom')+240
            cleanup=r0._number(w['cleanup_seconds'],'invalid_headroom')
            compute,total=self._amounts(self.anchor.hard_deadline,metering,cleanup,
                r0._number(w['compute_upper_bound_usd'],'invalid_cumulative_bound'),
                r0._number(w['total_upper_bound_usd'],'invalid_cumulative_bound'),now)
            state[b.OVERLAY]=dict(schema_version=2,original_bootstrap=copy.deepcopy(original),
                original_digest=_digest(original),legacy_digest=original['legacy_digest'],migration_cleanup_at=observed,
                attempts=[],compute_upper_bound_usd=str(compute),total_upper_bound_usd=str(total),
                metering_seconds=str(metering),cleanup_seconds=str(cleanup))
            self._v2(state,now)
        return self._operate(change)

    def reserve(self, evidence, current_anchor):
        def change(state,now):
            o=self._v2(state,now)
            r0._need(len(o['attempts'])<MAX_ATTEMPTS, 'attempt_limit')
            r0._need(not o['attempts'] or o['attempts'][-1]['phase']=='cleaned_pending_billing', 'prior_attempt_unresolved')
            anchor=self._fixed(current_anchor,now)
            e=r0._record(evidence, {'verified','observed_at','anchor','base_rpus','max_rpus',
                'usd_per_rpu_hour','no_additional_resources','no_other_billable_activity','controls_verified',
                'network_verified','other_tax_usd','additional_metering_seconds','additional_cleanup_seconds',
                'cleanup_evidence'}, 'invalid_admission_evidence')
            r0._need(e['verified'] is True and e['anchor']==anchor
                     and all(e[k] is True for k in ('no_additional_resources','no_other_billable_activity',
                         'controls_verified','network_verified')), 'admission_unverified')
            r0._timestamp(e['observed_at'],now,Decimal(300),'stale_admission')
            r0._need(r0._number(e['base_rpus'],'invalid_capacity')==4 and r0._number(e['max_rpus'],'invalid_capacity')==4
                     and r0._number(e['usd_per_rpu_hour'],'invalid_price')==Decimal('.374')
                     and r0._number(e['other_tax_usd'],'invalid_headroom')==5, 'fixed_controls_mismatch')
            last=o['attempts'][-1] if o['attempts'] else None
            previous_anchor=b.BootstrapAnchor(**last['anchor']) if last else self.anchor
            self._cleanup(e['cleanup_evidence'],previous_anchor,now,
                last['cleanup_at'] if last else o['migration_cleanup_at'],fresh=False)
            r0._need(b._time(current_anchor.hard_deadline,now,True)>now, 'deadline_expired')
            am=r0._number(e['additional_metering_seconds'],'invalid_headroom')
            ac=r0._number(e['additional_cleanup_seconds'],'invalid_headroom')
            r0._need(60<=am<=300 and 60<=ac<=300,'invalid_headroom')
            m=r0._number(o['metering_seconds'],'invalid_headroom')+am
            c=r0._number(o['cleanup_seconds'],'invalid_headroom')+ac
            compute,total=self._amounts(current_anchor.hard_deadline,m,c,
                r0._number(o['compute_upper_bound_usd'],'invalid_cumulative_bound'),
                r0._number(o['total_upper_bound_usd'],'invalid_cumulative_bound'),now)
            o['attempts'].append(dict(sequence=len(o['attempts'])+1,anchor=anchor,
                reserved_at=now.isoformat().replace('+00:00','Z'),phase='reserved',cleanup_at=None,activation_cleanup_at=None,
                additional_metering_seconds=str(am),additional_cleanup_seconds=str(ac),
                compute_upper_bound_usd=str(compute),total_upper_bound_usd=str(total)))
            o.update(compute_upper_bound_usd=str(compute),total_upper_bound_usd=str(total),
                metering_seconds=str(m),cleanup_seconds=str(c))
            self._v2(state,now)
        return self._operate(change)

    def transition(self, phase, cleanup_evidence=None):
        def change(state,now):
            o=self._v2(state,now)
            r0._need(bool(o['attempts']), 'attempt_missing')
            a=o['attempts'][-1]
            r0._need(a['phase'] in ('reserved','active','uncertain') and phase in
                ('active','uncertain','cleaned_pending_billing'), 'invalid_transition')
            anchor=b.BootstrapAnchor(**a['anchor'])
            if phase=='active':
                r0._need(a['phase']=='reserved' and b._time(anchor.hard_deadline,now,True)>now, 'active_not_admitted')
                a['activation_cleanup_at']=self._cleanup(cleanup_evidence,anchor,now,a['reserved_at'])
            if phase=='cleaned_pending_billing':
                a['cleanup_at']=self._cleanup(cleanup_evidence,anchor,now,a['activation_cleanup_at'] or a['reserved_at'])
            a['phase']=phase
            self._v2(state,now)
        return self._operate(change)


@contextmanager
def currentperiod_session(path, original_anchor, *, clock=None):
    """Exact same setup.json/lock; original protected anchor never replaced."""
    from pathlib import Path
    path=Path(path)
    r0._need(path.name=='setup.json','invalid_state_path')
    with r0.pilot_lock(path.parent) as fd:
        session=CumulativeSession(fd,path.parent,original_anchor,clock or (lambda:datetime.now(timezone.utc)))
        try:
            yield session
        finally:
            session.closed=True
