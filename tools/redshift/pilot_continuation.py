"""Offline continuation for original windows007/008 after verified recovery.

Explicit v5→v6 freezes exact complete recovery history. No SQL, launcher,
provisioning, automatic initialization, period extension or billing refund.
"""
from contextlib import contextmanager
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_cumulative as c
from tools.redshift import pilot_preflight as r0
from tools.redshift import pilot_recovery as r


class ContinuationSession(r.RecoverySession):
    def _baseline(self,raw,now):
        original=r.decode(raw)
        item=r.RecoverySession._recovery(self,original,now)
        r0._need(item['phase']=='cleaned_pending_billing','recovery_cleanup_required')
        return original,item

    def _prior_anchor(self,overlay):
        if overlay['attempts']:return b.BootstrapAnchor(**overlay['attempts'][-1]['anchor'])
        _,item=self._baseline(overlay['source_recovery_raw'],self.clock())
        return b.BootstrapAnchor(**item['anchor'])

    def next_sequence(self,overlay):return 7+len(overlay['attempts'])

    def _v2(self,state,now):
        o=r0._record(state.get(b.OVERLAY),{'schema_version','source_recovery_raw','source_recovery_digest',
            'legacy_digest','migration_cleanup_at','attempts','metering_seconds','cleanup_seconds',
            'compute_upper_bound_usd','total_upper_bound_usd'},'invalid_continuation_overlay')
        r0._need(type(o['schema_version']) is int and o['schema_version']==6
            and o['source_recovery_digest']==r.digest(o['source_recovery_raw'])
            and o['legacy_digest']==b._legacy_digest(state),'continuation_history_mismatch')
        original,baseline=self._baseline(o['source_recovery_raw'],now)
        r0._need(b._legacy_digest(original)==o['legacy_digest']
            and o['migration_cleanup_at']==baseline['cleanup_at'],'continuation_history_mismatch')
        r0._need(isinstance(o['attempts'],list) and len(o['attempts'])<=2,'attempt_limit')
        metering=r0._number(baseline['metering_seconds'],'invalid_headroom')
        cleanup=r0._number(baseline['cleanup_seconds'],'invalid_headroom')
        compute=r0._number(baseline['compute_upper_bound_usd'],'invalid_cumulative_bound')
        total=r0._number(baseline['total_upper_bound_usd'],'invalid_cumulative_bound')
        deadline=baseline['anchor']['hard_deadline'];last_cleanup=baseline['cleanup_at']
        for index,item in enumerate(o['attempts']):
            r0._record(item,{'sequence','anchor','reserved_at','phase','cleanup_at','activation_cleanup_at',
                'additional_metering_seconds','additional_cleanup_seconds','compute_upper_bound_usd',
                'total_upper_bound_usd'},'invalid_attempt')
            r0._need(type(item['sequence']) is int and item['sequence']==7+index,'continuation_history_mismatch')
            anchor=b.BootstrapAnchor(**item['anchor']);self._fixed(anchor,now)
            end=b._time(anchor.hard_deadline,now,True);reserved=b._time(item['reserved_at'],now)
            r0._need(end>=b._time(deadline,now,True) and end>reserved
                and reserved>=b._time(last_cleanup,now),'invalid_attempt_timestamp')
            am=r0._number(item['additional_metering_seconds'],'invalid_headroom')
            ac=r0._number(item['additional_cleanup_seconds'],'invalid_headroom')
            r0._need(am==1200 and ac==60,'invalid_headroom')
            metering+=am;cleanup+=ac;deadline=anchor.hard_deadline
            compute,total=self._amounts(deadline,metering,cleanup,compute,total,now)
            r0._need(r0._number(item['compute_upper_bound_usd'],'invalid_cumulative_bound')==compute
                and r0._number(item['total_upper_bound_usd'],'invalid_cumulative_bound')==total,
                'continuation_history_mismatch')
            phase=item['phase'];activation=item['activation_cleanup_at']
            r0._need(phase in ('reserved','active','uncertain','cleaned_pending_billing'),'invalid_phase')
            r0._need((phase!='active' or activation is not None)
                and (phase!='reserved' or activation is None),'activation_cleanup_required')
            if activation is not None:r0._need(b._time(activation,now)>=reserved,'cleanup_time_regression')
            if phase=='cleaned_pending_billing':
                last_cleanup=item['cleanup_at']
                r0._need(b._time(last_cleanup,now)>=reserved
                    and (activation is None or b._time(last_cleanup,now)>=b._time(activation,now)),
                    'cleanup_time_regression')
            else:r0._need(item['cleanup_at'] is None and index==len(o['attempts'])-1,'prior_attempt_unresolved')
        for key,value in [('metering_seconds',metering),('cleanup_seconds',cleanup),
            ('compute_upper_bound_usd',compute),('total_upper_bound_usd',total)]:
            r0._need(r0._number(o[key],'invalid_cumulative_bound')==value,'continuation_history_mismatch')
        return o

    def initialize(self,expected_source_digest):
        def change(state,now):
            raw=self._read_raw('setup.json')
            r0._need(r.digest(raw)==expected_source_digest,'continuation_source_changed')
            original,baseline=self._baseline(raw,now)
            r0._need(original==state,'continuation_history_mismatch')
            state[b.OVERLAY]=dict(schema_version=6,source_recovery_raw=raw,
                source_recovery_digest=r.digest(raw),legacy_digest=b._legacy_digest(state),
                migration_cleanup_at=baseline['cleanup_at'],attempts=[],
                **{k:baseline[k] for k in ('metering_seconds','cleanup_seconds',
                    'compute_upper_bound_usd','total_upper_bound_usd')})
            self._v2(state,now)
        return self._operate(change)

    def reserve(self,evidence,current):
        def change(state,now):
            o=self._v2(state,now)
            r0._need(len(o['attempts'])<2,'attempt_limit')
            r0._need(not o['attempts'] or o['attempts'][-1]['phase']=='cleaned_pending_billing','prior_attempt_unresolved')
            anchor=self._fixed(current,now)
            e=r0._record(evidence,{'verified','observed_at','anchor','base_rpus','max_rpus','usd_per_rpu_hour',
                'no_additional_resources','no_other_billable_activity','controls_verified','network_verified',
                'other_tax_usd','additional_metering_seconds','additional_cleanup_seconds','cleanup_evidence'},
                'invalid_admission_evidence')
            r0._need(e['verified'] is True and e['anchor']==anchor
                and all(e[k] is True for k in ('no_additional_resources','no_other_billable_activity',
                    'controls_verified','network_verified')),'admission_unverified')
            r0._timestamp(e['observed_at'],now,Decimal(300),'stale_admission')
            r0._need(r0._number(e['base_rpus'],'invalid_capacity')==4
                and r0._number(e['max_rpus'],'invalid_capacity')==4
                and r0._number(e['usd_per_rpu_hour'],'invalid_price')==Decimal('.374')
                and r0._number(e['other_tax_usd'],'invalid_headroom')==50,'fixed_controls_mismatch')
            last=o['attempts'][-1] if o['attempts'] else None
            self._cleanup(e['cleanup_evidence'],self._prior_anchor(o),now,
                last['cleanup_at'] if last else o['migration_cleanup_at'],fresh=False)
            r0._need(b._time(current.hard_deadline,now,True)>now,'deadline_expired')
            r0._need(r0._number(e['additional_metering_seconds'],'invalid_headroom')==1200
                and r0._number(e['additional_cleanup_seconds'],'invalid_headroom')==60,'invalid_headroom')
            metering=r0._number(o['metering_seconds'],'invalid_headroom')+1200
            cleanup=r0._number(o['cleanup_seconds'],'invalid_headroom')+60
            compute,total=self._amounts(current.hard_deadline,metering,cleanup,
                r0._number(o['compute_upper_bound_usd'],'invalid_cumulative_bound'),
                r0._number(o['total_upper_bound_usd'],'invalid_cumulative_bound'),now)
            o['attempts'].append(dict(sequence=self.next_sequence(o),anchor=anchor,
                reserved_at=now.isoformat().replace('+00:00','Z'),phase='reserved',cleanup_at=None,
                activation_cleanup_at=None,additional_metering_seconds='1200',additional_cleanup_seconds='60',
                compute_upper_bound_usd=str(compute),total_upper_bound_usd=str(total)))
            o.update(metering_seconds=str(metering),cleanup_seconds=str(cleanup),
                compute_upper_bound_usd=str(compute),total_upper_bound_usd=str(total))
            self._v2(state,now)
        return self._operate(change)

    def transition(self,phase,cleanup_evidence=None):
        return c.CumulativeSession.transition(self,phase,cleanup_evidence)

    def reserve_recovery(self,*args,**kwargs):return self._blocked()
    def start_recovery(self,*args,**kwargs):return self._blocked()
    def finish_recovery(self,*args,**kwargs):return self._blocked()
    def migrate(self,*args,**kwargs):return self._blocked()
    def _blocked(self):return {'status':'block','reasons':['continuation_only_scope'],'live_enabled':False}


@contextmanager
def continuation_session(path,original_anchor,*,clock=None):
    path=Path(path);r0._need(path.name=='setup.json','invalid_state_path')
    with r0.pilot_lock(path.parent) as fd:
        session=ContinuationSession(fd,path.parent,original_anchor,clock or (lambda:datetime.now(timezone.utc)))
        try:yield session
        finally:session.closed=True
