"""Operator-reviewed finite pilot windows. No provisioning, fixture DDL or retry.

Requires explicit v4 initialization and a private sequence-bound request. Every
SQL call, including preflight cleanup, follows durable cumulative reservation.
Raw diagnostics remain private; unknown billing never becomes a balance.
"""
from dataclasses import replace
from datetime import datetime, timezone
from decimal import Decimal
import ipaddress
import json
import os
from pathlib import Path
import time
import urllib.request

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_live as live
from tools.redshift import pilot_preflight as r0
from tools.redshift import pilot_windows as windows

PROFILES = {'scalar': tuple(x for x in live.BASELINE if not x.endswith('ConfiguredFixtureMetadata')),
            'catalog': ('RedshiftRealTest.ConfiguredFixtureMetadata',), 'baseline': live.BASELINE}
PRICE_URL = 'https://pricing.us-east-1.amazonaws.com/offers/v1.0/aws/AmazonRedshift/current/eu-north-1/index.json'


def utcnow():
    return datetime.now(timezone.utc)


def get_https(url, maximum):
    try:
        request = urllib.request.Request(url, headers={'Cache-Control': 'no-cache'})
        with urllib.request.urlopen(request, timeout=20) as response:
            if response.geturl() != url:
                raise r0.Blocked('evidence_redirect')
            data = response.read(maximum+1)
        r0._need(len(data) <= maximum, 'evidence_too_large')
        return data
    except (OSError, ValueError):
        raise r0.Blocked('fresh_evidence_unavailable') from None


def price_and_ip():
    try:
        price = json.loads(get_https(PRICE_URL, 32*1024*1024), object_pairs_hook=r0._unique_object)
        sku = 'ZUJGFS2VXV3ZX482'
        product = price['products'][sku]
        r0._need(product['attributes']['regionCode'] == live.REGION, 'price_region_mismatch')
        rates = [Decimal(dim['pricePerUnit']['USD'])
                 for term in price['terms']['OnDemand'][sku].values()
                 for dim in term['priceDimensions'].values() if dim['unit'] == 'RPU-Hr']
        r0._need(rates == [Decimal('.374')], 'price_changed')
        address = ipaddress.IPv4Address(get_https('https://checkip.amazonaws.com/', 128).decode().strip())
        r0._need(address.is_global, 'network_address_invalid')
        return str(address)+'/32'
    except (KeyError, TypeError, ValueError):
        raise r0.Blocked('fresh_evidence_invalid') from None


def request_record(request, anchor, now):
    r0._record(request, {'sequence','reviewed','observed_at','hard_deadline','profile','manifest','admission'},
               'invalid_window_request')
    r0._need(type(request['sequence']) is int and 1 <= request['sequence'] <= 8
             and request['reviewed'] is True and request['profile'] in PROFILES, 'invalid_window_request')
    r0._timestamp(request['observed_at'],now,Decimal(300),'stale_window_request')
    end = b._time(request['hard_deadline'],now,True)
    r0._need(60 <= (end-now).total_seconds() <= 180, 'invalid_window_deadline')
    manifest = r0._record(request['manifest'], {'path','sha256'}, 'invalid_manifest')
    live.verify_executable(Path(manifest['path']),manifest['sha256'])
    current = replace(anchor,hard_deadline=request['hard_deadline'])
    live.validate_admission(request['admission'],current,now,amended=True)
    return current


def fresh_controls(anchor, config, admission):
    aws = live.aws
    identity = aws(['sts','get-caller-identity'])
    workgroup = aws(['redshift-serverless','get-workgroup','--workgroup-name',live.NAME])
    namespace = aws(['redshift-serverless','get-namespace','--namespace-name',live.NAME])
    w = workgroup['workgroup']
    usage = aws(['redshift-serverless','list-usage-limits','--resource-arn',w['workgroupArn']])
    r0._need(not usage.get('nextToken'), 'usage_controls_mismatch')
    network = aws(['ec2','describe-security-groups','--group-ids']+w['securityGroupIds'])
    budget = aws(['budgets','describe-budget','--account-id',live.ACCOUNT,'--budget-name',live.NAME])
    live.validate_controls(identity,workgroup,namespace,usage,network,budget,anchor,config,admission,amended=True)
    inventory_w = aws(['redshift-serverless','list-workgroups'])
    inventory_n = aws(['redshift-serverless','list-namespaces'])
    r0._need(not inventory_w.get('nextToken') and not inventory_n.get('nextToken')
        and [x['workgroupId'] for x in inventory_w['workgroups']] == [anchor.workgroup_id]
        and [x['namespaceId'] for x in inventory_n['namespaces']] == [anchor.namespace_id],
        'additional_redshift_resources')
    controls = budget['Budget']
    r0._need(controls['BudgetType']=='COST' and controls['TimeUnit']=='MONTHLY'
        and controls['CostTypes']['IncludeTax'] is True, 'budget_controls_mismatch')
    notifications = aws(['budgets','describe-notifications-for-budget','--account-id',live.ACCOUNT,
                         '--budget-name',live.NAME])
    expected = {Decimal(100),Decimal(125),Decimal(150)}
    entries = notifications['Notifications']
    r0._need(not notifications.get('NextToken') and len(entries)==3
        and {Decimal(str(n['Threshold'])) for n in entries}==expected
        and all(n['NotificationType']=='ACTUAL' and n['ThresholdType']=='ABSOLUTE_VALUE'
                and n['ComparisonOperator']=='GREATER_THAN' for n in entries), 'budget_alerts_mismatch')
    for notification in entries:
        subscribers = aws(['budgets','describe-subscribers-for-notification','--account-id',live.ACCOUNT,
            '--budget-name',live.NAME,'--notification',json.dumps({k:notification[k] for k in
                ('NotificationType','ComparisonOperator','Threshold','ThresholdType')})])
        r0._need(bool(subscribers['Subscribers']) and not subscribers.get('NextToken')
            and all(s.get('SubscriptionType') in ('EMAIL','SNS') and isinstance(s.get('Address'),str)
                    and s['Address'] for s in subscribers['Subscribers']), 'budget_subscribers_missing')
    cidr = price_and_ip()
    r0._need(cidr==admission['allowed_ipv4_cidr'], 'network_address_changed')
    r0._need(Path(config['ca_file']).is_file(), 'ca_unavailable')


def passed(outcome):
    if outcome['status'] != 'pass':
        raise r0.Blocked(outcome['reasons'][0])


def cleanup_evidence(anchor):
    now = utcnow()
    return dict(verified=True,observed_at=now.isoformat().replace('+00:00','Z'),anchor=anchor.record(now),
                no_active_queries=True,no_active_sessions=True,transactions_closed=True)


def save_report(path, report, directory_fd):
    fd = os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    with os.fdopen(fd,'w') as stream:
        json.dump(report,stream,indent=2);stream.flush();os.fsync(stream.fileno())
    os.fsync(directory_fd)


def execute(sequence, directory=r0.CANONICAL_DIRECTORY):
    r0._need(type(sequence) is int and 1<=sequence<=8, 'invalid_window_sequence')
    directory = Path(directory)
    original = b.BootstrapAnchor(**live.private_json(directory/'bootstrap-anchor.json'))
    config = live.private_json(directory/'database-config.json');live.validate_config(config)
    request_path = directory/f'window-{sequence:03d}-request.json'
    request = live.private_json(request_path)
    report_path = directory/f'window-{sequence:03d}-result.json'
    r0._need(not report_path.exists() and not report_path.is_symlink(), 'window_already_reported')
    report = dict(status='blocked',reason=None,cleanup_verified=False,results=[],
                  actual_spend_usd=None,remaining_allowance_usd=None,sequence=sequence)
    with windows.window_session(directory/'setup.json',original) as ledger:
        now = utcnow(); current = request_record(request,original,now)
        r0._need(request['sequence']==sequence, 'window_sequence_mismatch')
        state = ledger._load(); overlay = ledger._v2(state,now)
        r0._need(sequence==len(overlay['attempts'])+1, 'window_already_consumed')
        r0._need(not overlay['attempts'] or overlay['attempts'][-1]['phase']=='cleaned_pending_billing',
                 'prior_attempt_unresolved')
        prior_cleanup = dict(verified=True,
            observed_at=(overlay['attempts'][-1]['cleanup_at'] if overlay['attempts']
                         else overlay['migration_cleanup_at']),
            anchor=ledger._prior_anchor(overlay).record(now),
            no_active_queries=True,no_active_sessions=True,transactions_closed=True)
        fresh_controls(current,config,request['admission'])
        # Revalidate after network calls: a preflight must not consume the SQL deadline.
        current = request_record(request,original,utcnow())
        evidence = dict(verified=True,observed_at=utcnow().isoformat().replace('+00:00','Z'),
            anchor=current.record(utcnow()),base_rpus=4,max_rpus=4,usd_per_rpu_hour='.374',
            no_additional_resources=True,no_other_billable_activity=True,controls_verified=True,
            network_verified=True,other_tax_usd='50',additional_metering_seconds=1200,
            additional_cleanup_seconds=60,cleanup_evidence=prior_cleanup)
        passed(ledger.reserve(evidence,current))
        remaining = (b._time(current.hard_deadline,utcnow(),True)-utcnow()).total_seconds()
        window = live.Window(directory,config,time.monotonic()+max(0,remaining));window.seq=sequence*1000
        try:
            window.cleanup()
            passed(ledger.transition('active',cleanup_evidence(current)))
            r0._need(window.sql('SHOW statement_timeout;',admin=False)=='15000', 'server_timeout_unverified')
            report['results'].append(window.driver(request['manifest'],live.IDENTITY))
            # Identity failure never reaches the profile cases; nothing retries them.
            window.last_driver_summary = None
            report['results'].append(window.driver(request['manifest'],PROFILES[request['profile']]))
            report['status']='passed'
        except BaseException as error:
            report['reason']=error.code if isinstance(error,r0.Blocked) else 'execution_interrupted'
            summary=getattr(window,'last_driver_summary',None)
            if summary is not None and summary not in report['results']:report['results'].append(summary)
            ledger.transition('uncertain')
        finally:
            window.deadline=time.monotonic()+60
            try:
                window.cleanup();passed(ledger.transition('cleaned_pending_billing',cleanup_evidence(current)))
                report['cleanup_verified']=True
            except BaseException:
                report.update(status='blocked',reason='remote_cleanup_unverified')
                ledger.transition('uncertain')
        try:
            save_report(report_path,report,ledger.fd)
        except BaseException:
            # Reservation remains consumed even if reporting fails after clean SQL.
            raise r0.Blocked('window_report_persistence_failed') from None
        return report


if __name__=='__main__':
    import sys
    try:
        r0._need(len(sys.argv)==2 and sys.argv[1].isdigit(), 'invalid_window_sequence')
        report=execute(int(sys.argv[1]));print(json.dumps(report))
        raise SystemExit(0 if report['status']=='passed' else 1)
    except r0.Blocked as error:
        print(json.dumps(dict(status='blocked',reason=error.code)));raise SystemExit(1)
    except Exception:
        print(json.dumps(dict(status='blocked',reason='window_unavailable_or_invalid')));raise SystemExit(1)
