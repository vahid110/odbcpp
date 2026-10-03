"""One explicitly reviewed exact-type window using the existing bounded runner.

No implicit accounting migration, extra warehouses or automatic retries. The
original launcher limits remain unchanged. V8 must be initialized separately.
"""
import hashlib
from datetime import timedelta
from pathlib import Path

from tools.redshift import pilot_finite_live as previous
from tools.redshift import pilot_type_qualification as ledger
from tools.redshift import pilot_preflight as r0
from tools.redshift import pilot_window_live as windows

CODE_FILES = previous.CODE_FILES + ('pilot_type_qualification.py', 'pilot_type_live.py')


PROFILES = {'types': tuple(ledger.INVENTORY[1:])}

def code_digest():
    result = hashlib.sha256()
    for name in CODE_FILES:
        data = (Path(__file__).parent/name).read_bytes()
        result.update(name.encode()+b'\0'+str(len(data)).encode()+b'\0'+data)
    return result.hexdigest()


def request_record(request, original, now):
    r0._record(request, {'sequence', 'reviewed', 'observed_at', 'hard_deadline',
        'profile', 'manifest', 'admission', 'state_digest', 'code_digest'}, 'invalid_finite_request')
    r0._need(type(request['sequence']) is int and request['sequence'] == 10
        and request['profile'] == 'types', 'invalid_finite_inventory')
    r0._need(request['code_digest'] == code_digest(), 'finite_code_changed')
    r0._need(isinstance(request['state_digest'], str) and len(request['state_digest']) == 64,
        'invalid_finite_digest')
    current = windows.request_record({k:v for k,v in request.items()
        if k not in ('state_digest', 'code_digest')}, original, now, maximum_sequence=10, profiles=PROFILES)
    horizon = windows.b._time(original.earliest_billable_start, now) + timedelta(days=1)
    end = windows.b._time(request['hard_deadline'], now, True)
    r0._need(end + timedelta(seconds=60) <= horizon, 'type_cleanup_outside_original_horizon')
    return current


def validate_overlay(request, overlay):
    r0._need(overlay['review']['source_code_digest'] == request['code_digest'],
        'amendment_code_review_mismatch')


def execute(directory=r0.CANONICAL_DIRECTORY):
    return windows._execute(10, directory, ledger.type_qualification_session,
        request_record, sequence_offset=9, maximum_sequence=10,
        validate_overlay=validate_overlay, profiles=PROFILES)


if __name__ == '__main__':
    import json
    try:
        result = execute()
        print(json.dumps(result))
        raise SystemExit(0 if result['status'] == 'passed' else 1)
    except r0.Blocked as error:
        print(json.dumps({'status': 'blocked', 'reason': error.code}))
        raise SystemExit(1)
