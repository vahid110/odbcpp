"""Offline, one-shot cleanup-only accounting for unresolved IAM window006.

No cloud calls, SQL, credential exchange or launcher. V5 freezes exact v4 and
006 result bytes and retains every liability. Old qualification launchers reject
v5. Completion records recovery separately and never rewrites006 or authorizes007.
Caller observations require independent operator review; this module validates
and durably stores them, not their provenance.
"""
from contextlib import contextmanager
from datetime import datetime, timezone
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_preflight as r0
from tools.redshift import pilot_windows as w


def decode(raw):
    r0._need(isinstance(raw,str) and len(raw.encode())<=b.LIMIT,'invalid_recovery_history')
    return json.loads(raw,parse_float=Decimal,object_pairs_hook=r0._unique_object,
        parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))


def digest(raw):
    return hashlib.sha256(raw.encode()).hexdigest()


class RecoverySession(w.WindowSession):
    def _read_raw(self,name):
        self._guard()
        fd=os.open(name,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK,dir_fd=self.fd)
        try:
            r0._private_file(os.fstat(fd))
            r0._need(os.fstat(fd).st_size<=b.LIMIT,'state_too_large')
            with os.fdopen(fd,'rb',closefd=False) as stream:
                raw=stream.read(b.LIMIT+1).decode('utf-8')
        finally:os.close(fd)
        self._guard();decode(raw)
        return raw

    def _source(self,source,result,now):
        original=decode(source);overlay=w.WindowSession._v2(self,original,now)
        r0._need(len(overlay['attempts'])==6 and overlay['attempts'][-1]['phase']=='uncertain',
                 'recovery_target_mismatch')
        report=r0._record(decode(result),{'status','reason','cleanup_verified','results',
            'actual_spend_usd','remaining_allowance_usd','sequence'},'recovery_result_mismatch')
        r0._need(type(report['sequence']) is int and report['sequence']==6
            and report['status']=='blocked' and report['reason']=='remote_cleanup_unverified'
            and report['cleanup_verified'] is False and report['results']==[]
            and report['actual_spend_usd'] is None and report['remaining_allowance_usd'] is None,
            'recovery_result_mismatch')
        return original,overlay

    def _recovery(self,state,now):
        o=r0._record(state.get(b.OVERLAY),{'schema_version','source_state_raw','source_state_digest',
            'source_result_raw','source_result_digest','legacy_digest','recovery'},'invalid_recovery_overlay')
        r0._need(type(o['schema_version']) is int and o['schema_version']==5
            and o['source_state_digest']==digest(o['source_state_raw'])
            and o['source_result_digest']==digest(o['source_result_raw'])
            and o['legacy_digest']==b._legacy_digest(state),'recovery_history_mismatch')
        original,prior=self._source(o['source_state_raw'],o['source_result_raw'],now)
        r0._need(b._legacy_digest(original)==o['legacy_digest'],'recovery_history_mismatch')
        item=r0._record(o['recovery'],{'target_sequence','scope','anchor','reserved_at','phase',
            'started_at','cleanup_at','additional_metering_seconds','additional_cleanup_seconds',
            'metering_seconds','cleanup_seconds','compute_upper_bound_usd','total_upper_bound_usd'},
            'invalid_recovery_record')
        r0._need(type(item['target_sequence']) is int and item['target_sequence']==6
            and item['scope']=='cleanup_only' and item['additional_metering_seconds']=='1200'
            and item['additional_cleanup_seconds']=='60','invalid_recovery_scope')
        current=b.BootstrapAnchor(**item['anchor']);self._fixed(current,now)
        end=b._time(current.hard_deadline,now,True);reserved=b._time(item['reserved_at'],now)
        r0._need(0<(end-reserved).total_seconds()<=60
            and reserved>=b._time(prior['attempts'][-1]['reserved_at'],now)
            and end>=b._time(prior['attempts'][-1]['anchor']['hard_deadline'],now,True),
            'invalid_recovery_deadline')
        metering=r0._number(prior['metering_seconds'],'invalid_headroom')+1200
        cleanup=r0._number(prior['cleanup_seconds'],'invalid_headroom')+60
        compute,total=self._amounts(current.hard_deadline,metering,cleanup,
            r0._number(prior['compute_upper_bound_usd'],'invalid_cumulative_bound'),
            r0._number(prior['total_upper_bound_usd'],'invalid_cumulative_bound'),now)
        for key,value in [('metering_seconds',metering),('cleanup_seconds',cleanup),
            ('compute_upper_bound_usd',compute),('total_upper_bound_usd',total)]:
            r0._need(r0._number(item[key],'invalid_cumulative_bound')==value,'recovery_history_mismatch')
        r0._need(item['phase'] in ('reserved','active','uncertain','cleaned_pending_billing'),'invalid_phase')
        started=item['started_at'];cleaned=item['cleanup_at']
        if started is not None:
            r0._need(reserved<=b._time(started,now)<end,'invalid_recovery_timestamp')
        r0._need((item['phase']!='reserved' or started is None)
            and (item['phase'] not in ('active','cleaned_pending_billing') or started is not None),
            'invalid_recovery_timestamp')
        if item['phase']=='cleaned_pending_billing':
            r0._need(cleaned is not None and b._time(started,now)<=b._time(cleaned,now)<=end,
                     'invalid_recovery_timestamp')
        else:r0._need(cleaned is None,'invalid_recovery_timestamp')
        return item

    def reserve_recovery(self,evidence,current):
        def change(state,now):
            source=self._read_raw('setup.json');result=self._read_raw('window-006-result.json')
            original,prior=self._source(source,result,now)
            r0._need(original==state,'recovery_history_mismatch')
            anchor=self._fixed(current,now)
            e=r0._record(evidence,{'verified','observed_at','anchor','base_rpus','max_rpus',
                'usd_per_rpu_hour','no_additional_resources','no_other_billable_activity',
                'controls_verified','network_verified','other_tax_usd','scope'},'invalid_recovery_evidence')
            r0._need(e['verified'] is True and e['anchor']==anchor and e['scope']=='cleanup_only'
                and all(e[k] is True for k in ('no_additional_resources','no_other_billable_activity',
                    'controls_verified','network_verified')),'recovery_unverified')
            r0._timestamp(e['observed_at'],now,Decimal(300),'stale_recovery_evidence')
            r0._need(r0._number(e['base_rpus'],'invalid_capacity')==4
                and r0._number(e['max_rpus'],'invalid_capacity')==4
                and r0._number(e['usd_per_rpu_hour'],'invalid_price')==Decimal('.374')
                and r0._number(e['other_tax_usd'],'invalid_headroom')==50,'fixed_controls_mismatch')
            end=b._time(current.hard_deadline,now,True)
            r0._need(0<(end-now).total_seconds()<=60,'invalid_recovery_deadline')
            metering=r0._number(prior['metering_seconds'],'invalid_headroom')+1200
            cleanup=r0._number(prior['cleanup_seconds'],'invalid_headroom')+60
            compute,total=self._amounts(current.hard_deadline,metering,cleanup,
                r0._number(prior['compute_upper_bound_usd'],'invalid_cumulative_bound'),
                r0._number(prior['total_upper_bound_usd'],'invalid_cumulative_bound'),now)
            state[b.OVERLAY]=dict(schema_version=5,source_state_raw=source,source_state_digest=digest(source),
                source_result_raw=result,source_result_digest=digest(result),legacy_digest=b._legacy_digest(state),
                recovery=dict(target_sequence=6,scope='cleanup_only',anchor=anchor,
                    reserved_at=now.isoformat().replace('+00:00','Z'),phase='reserved',started_at=None,
                    cleanup_at=None,additional_metering_seconds='1200',additional_cleanup_seconds='60',
                    metering_seconds=str(metering),cleanup_seconds=str(cleanup),
                    compute_upper_bound_usd=str(compute),total_upper_bound_usd=str(total)))
            self._recovery(state,now)
        return self._operate(change)

    def start_recovery(self):
        def change(state,now):
            item=self._recovery(state,now)
            r0._need(item['phase']=='reserved','recovery_already_consumed')
            r0._need(now<b._time(item['anchor']['hard_deadline'],now,True),'deadline_expired')
            item.update(phase='active',started_at=now.isoformat().replace('+00:00','Z'))
            self._recovery(state,now)
        return self._operate(change)

    def finish_recovery(self,cleanup_evidence=None):
        def change(state,now):
            item=self._recovery(state,now)
            r0._need(item['phase']=='active','recovery_already_consumed')
            if cleanup_evidence is None:item['phase']='uncertain'
            else:
                anchor=b.BootstrapAnchor(**item['anchor'])
                cleaned=self._cleanup(cleanup_evidence,anchor,now,item['started_at'])
                r0._need(now<=b._time(anchor.hard_deadline,now,True),'deadline_expired')
                item.update(phase='cleaned_pending_billing',cleanup_at=cleaned)
            self._recovery(state,now)
        return self._operate(change)

    def initialize(self):return {'status':'block','reasons':['cleanup_only_scope'],'live_enabled':False}
    def reserve(self,*args,**kwargs):return self.initialize()
    def transition(self,*args,**kwargs):return self.initialize()
    def migrate(self,*args,**kwargs):return self.initialize()


@contextmanager
def recovery_session(path,original_anchor,*,clock=None):
    path=Path(path);r0._need(path.name=='setup.json','invalid_state_path')
    with r0.pilot_lock(path.parent) as fd:
        session=RecoverySession(fd,path.parent,original_anchor,clock or (lambda:datetime.now(timezone.utc)))
        try:yield session
        finally:session.closed=True
