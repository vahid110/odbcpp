"""Explicit single corrected operator qualification; no retry loop."""
import json
from tools.redshift.pilot_live import execute
from tools.redshift.pilot_preflight import Blocked

if __name__ == '__main__':
    try:
        result=execute(qualification=True)
        print(json.dumps(result))
        if result['status']!='passed':raise SystemExit(1)
    except Blocked as error:
        print(json.dumps(dict(status='blocked',reason=error.code)))
        raise SystemExit(1)
