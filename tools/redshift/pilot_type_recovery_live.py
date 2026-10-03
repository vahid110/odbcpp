"""One explicitly admitted cleanup-only recovery; no qualification or retry.

Fresh read-only controls precede a durable v9 reservation and consumed start.
One cleanup call shares a fixed <=60s deadline. No exchange, driver, fixture DDL,
renewal, second cleanup or automatic re-entry exists. Old010 evidence is frozen.
"""
from dataclasses import replace
from datetime import datetime, timezone, timedelta
from decimal import Decimal
import hashlib
from pathlib import Path
import time

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_live as live
from tools.redshift import pilot_preflight as r0
from tools.redshift import pilot_recovery as recovery
from tools.redshift import pilot_type_recovery as accounting
from tools.redshift import pilot_type_live as previous
from tools.redshift import pilot_window_live as windows

CODE_FILES = previous.CODE_FILES + ('pilot_type_recovery.py', 'pilot_type_recovery_live.py')


def code_digest():
    result=hashlib.sha256()
    for name in CODE_FILES:
        data=(Path(__file__).parent/name).read_bytes()
        result.update(name.encode()+b'\0'+str(len(data)).encode()+b'\0'+data)
    return result.hexdigest()


def utcnow():return datetime.now(timezone.utc)


def request_record(request,original,now):
    r0._record(request,{'target_sequence','scope','reviewed','observed_at','hard_deadline',
        'source_state_digest','source_result_digest','code_digest','admission'},'invalid_recovery_request')
    r0._need(type(request['target_sequence']) is int and request['target_sequence']==10
        and request['scope']=='cleanup_only' and request['reviewed'] is True,'invalid_recovery_scope')
    r0._timestamp(request['observed_at'],now,Decimal(300),'stale_recovery_request')
    r0._need(request['code_digest']==code_digest(),'recovery_code_changed')
    end=b._time(request['hard_deadline'],now,True)
    r0._need(0<(end-now).total_seconds()<=60,'invalid_recovery_deadline')
    r0._need(end <= b._time(original.earliest_billable_start, now)+timedelta(days=1), 'recovery_outside_original_horizon')
    current=replace(original,hard_deadline=request['hard_deadline'])
    live.validate_admission(request['admission'],current,now,amended=True)
    return current


def execute(directory=r0.CANONICAL_DIRECTORY):
    directory=Path(directory)
    original=b.BootstrapAnchor(**live.private_json(directory/'bootstrap-anchor.json'))
    config=live.private_json(directory/'database-config.json');live.validate_config(config)
    report_path=directory/'cleanup-recovery-010-result.json'
    report=dict(status='blocked',reason=None,cleanup_verified=False,target_sequence=10,
                scope='cleanup_only',actual_spend_usd=None,remaining_allowance_usd=None)
    with accounting.type_recovery_session(directory/'setup.json',original) as ledger:
        r0._need(not report_path.exists() and not report_path.is_symlink(),'recovery_already_reported')
        request=live.private_json(directory/'cleanup-recovery-010-request.json')
        current=request_record(request,original,utcnow())
        # Validate the unchanged v8 source and failed result before controls or SQL.
        state=ledger._load()
        source=ledger._read_raw('setup.json');result=ledger._read_raw('window-010-result.json')
        ledger._source(source,result,utcnow())
        r0._need(recovery.decode(source)==state
            and request['source_state_digest']==recovery.digest(source)
            and request['source_result_digest']==recovery.digest(result),'recovery_source_changed')
        windows.fresh_controls(current,config,request['admission'])
        current=request_record(request,original,utcnow())
        evidence=dict(verified=True,observed_at=utcnow().isoformat().replace('+00:00','Z'),
            anchor=current.record(utcnow()),base_rpus=4,max_rpus=4,usd_per_rpu_hour='.374',
            no_additional_resources=True,no_other_billable_activity=True,controls_verified=True,
            network_verified=True,other_tax_usd='50',scope='cleanup_only',
            source_state_digest=request['source_state_digest'],source_result_digest=request['source_result_digest'])
        windows.passed(ledger.reserve_recovery(evidence,current))
        try:
            windows.passed(ledger.start_recovery())
            remaining=(b._time(current.hard_deadline,utcnow(),True)-utcnow()).total_seconds()
            r0._need(remaining>0,'deadline_expired')
            window=live.Window(directory,config,time.monotonic()+min(60,remaining),
                               cleanup_principal=live.IAM_DB_USER)
            window.seq=20000
            window.log_prefix='cleanup-recovery-010'
            # Only this call can execute SQL. It never dispatches driver cases.
            window.cleanup()
            windows.passed(ledger.finish_recovery(windows.cleanup_evidence(current)))
            report.update(status='passed',cleanup_verified=True)
        except BaseException as error:
            report['reason']=error.code if isinstance(error,r0.Blocked) else 'execution_interrupted'
            # No second cleanup/retry. Failed accounting persistence stays fail-closed.
            ledger.finish_recovery()
        ledger._guard()
        try:windows.save_report(report_path,report,ledger.fd)
        except BaseException:raise r0.Blocked('recovery_report_persistence_failed') from None
        return report


if __name__=='__main__':
    import json
    import sys
    try:
        r0._need(len(sys.argv)==1,'invalid_recovery_scope')
        report=execute();print(json.dumps(report))
        raise SystemExit(0 if report['status']=='passed' else 1)
    except r0.Blocked as error:
        print(json.dumps(dict(status='blocked',reason=error.code)));raise SystemExit(1)
