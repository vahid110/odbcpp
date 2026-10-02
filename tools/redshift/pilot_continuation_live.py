"""Explicit remaining-window launcher after separately initialized v6 accounting.

Only007/008, no automatic migration/retry; old v4 launcher remains isolated.
Uses existing reviewed identity/profile, IAM provenance/privacy/expiry and final
cleanup flow. Request binds exact canonical v6 bytes, current runner source and
executable. Reserve rechecks source binding inside its durable operation.
"""
import hashlib
from pathlib import Path

from tools.redshift import pilot_continuation as ledger
from tools.redshift import pilot_live as live
from tools.redshift import pilot_preflight as r0
from tools.redshift import pilot_recovery as recovery
from tools.redshift import pilot_recovery_live as recovery_live
from tools.redshift import pilot_window_live as windows

CODE_FILES=recovery_live.CODE_FILES+('pilot_continuation.py','pilot_continuation_live.py')


def code_digest():
    result=hashlib.sha256()
    for name in CODE_FILES:
        data=(Path(__file__).parent/name).read_bytes()
        result.update(name.encode()+b'\0'+str(len(data)).encode()+b'\0'+data)
    return result.hexdigest()


def request_record(request,original,now):
    keys={'sequence','reviewed','observed_at','hard_deadline','profile','manifest','admission',
          'state_digest','code_digest'}
    r0._record(request,keys,'invalid_continuation_request')
    r0._need(type(request['sequence']) is int and request['sequence'] in (7,8),
             'invalid_continuation_sequence')
    r0._need(request['code_digest']==code_digest(),'continuation_code_changed')
    r0._need(isinstance(request['state_digest'],str) and len(request['state_digest'])==64,
             'invalid_continuation_digest')
    return windows.request_record({k:v for k,v in request.items() if k not in ('state_digest','code_digest')},original,now)


def execute(sequence,directory=r0.CANONICAL_DIRECTORY):
    r0._need(type(sequence) is int and sequence in (7,8),'invalid_continuation_sequence')
    return windows._execute(sequence,directory,ledger.continuation_session,request_record,sequence_offset=6)


if __name__=='__main__':
    import json
    import sys
    try:
        r0._need(len(sys.argv)==2 and sys.argv[1] in ('7','8'),'invalid_continuation_sequence')
        report=execute(int(sys.argv[1]));print(json.dumps(report))
        raise SystemExit(0 if report['status']=='passed' else 1)
    except r0.Blocked as error:
        print(json.dumps(dict(status='blocked',reason=error.code)));raise SystemExit(1)
