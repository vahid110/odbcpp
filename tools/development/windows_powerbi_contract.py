"""Closed qualification007 DATA contracts. No platform APIs or import effects.

External application qualification owns these controls; no SDK/query semantics.
Python never decides feature outcomes or derives measured connection demand.
"""
import datetime as dt
import hashlib
from pathlib import Path
import re

SID = 'S-1-5-21-1833777940-313916754-1955530366-1002'
APP = Path('C:/Program Files/WindowsApps/Microsoft.MicrosoftPowerBIDesktop_2.159.883.0_x64__8wekyb3d8bbwe/bin/PBIDesktop.exe')
APP_SHA = '91b7a44a01ad23eeba6ce8a6728cb6e01ea7dabf0e7f548e9c083545b2b6ee91'
BASE = Path('C:/Users/vahid/odbcpp-qualification/PowerBI007')
CA = Path('C:/Users/vahid/AppData/Local/odbcpp-migration/protected-live-inputs/tls/mac-trusted-ca-bundle.pem')
CA_SHA = '9dae8d76e55cb08991f2b672d58999ea15560d910759c16b544f843bdffbb994'
GIT = Path('C:/Users/vahid/.cache/codex-runtimes/codex-primary-runtime/dependencies/native/git/cmd/git.exe')
PS = Path('C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe')
PYTHON = Path('C:/Users/vahid/AppData/Local/Programs/Python/Python312/python.exe')
AWS_MSI_SHA = 'df6c4bf34c0907509ddbec05dfacda5b7ae563235a335e3fca020a9c1e084d39'
AWS_ROOT = Path('C:/Program Files/Amazon Redshift ODBC Driver x64')
OWN_ROOT = Path('C:/Program Files/ODBCPP/Redshift')
MODULES = ('windows_powerbi_contract.py', 'windows_powerbi_scope.py',
           'windows_powerbi_provider.py', 'windows_powerbi_owner.py')
CANONICAL = ('windows_live_backend.py', 'windows_native_job.py', 'redshift_fixture_factory.py')
SAMPLES = ('sample-connected', 'sample-navigator', 'sample-refresh')
SERVERLESS_HOST = 'odbcpp-redshift-pilot.385207570137.eu-north-1.redshift-serverless.amazonaws.com'


def need(value, code):
    if not value:
        raise RuntimeError(code)


def closed(value, keys, code):
    need(type(value) is dict and set(value) == set(keys), code)


def digest(value):
    need(type(value) is str and re.fullmatch('[0-9a-f]{64}', value) is not None, 'sha256')
    return value


def utc(value):
    need(type(value) is str, 'utc_string')
    parsed = dt.datetime.fromisoformat(value.replace('Z', '+00:00'))
    need(parsed.tzinfo is not None, 'utc_timezone')
    return parsed.astimezone(dt.timezone.utc)


def pin(value):
    closed(value, ('path', 'sha256'), 'evidence_pin')
    need(type(value['path']) is str and (Path(value['path']).is_absolute()
         or value['path'].startswith('/home/vahid/.local/state/odbcpp/'))
         and len(value['path']) <= 1024 and not any(c in value['path'] for c in '\0\r\n'), 'absolute_evidence_path')
    digest(value['sha256'])
    return value


def validate_plan(plan):
    closed(plan, ('schema_version', 'scope', 'run_id', 'source_head', 'checkout', 'workload_sha256',
                  'application', 'ca', 'driver', 'modules', 'canonical', 'tools',
                  'max_execution_seconds', 'max_cleanup_seconds', 'max_launches', 'max_capture_per_phase',
                  'trace', 'independent_review'), 'native_plan_schema')
    need(plan['schema_version'] == 1 and plan['scope'] == 'redshift-powerbi007'
         and plan['independent_review'] == 'ACCEPT', 'reviewed_native_plan')
    need(type(plan['source_head']) is str and re.fullmatch('[0-9a-f]{40}', plan['source_head']), 'source_head')
    checkout = Path(plan['checkout'])
    need(checkout.is_absolute() and checkout.parent.parent == Path('C:/Users/vahid/.codex/worktrees')
         and checkout.name == 'odbcpp', 'attached_checkout_only')
    digest(plan['workload_sha256'])
    need(plan['application'] == dict(path=str(APP), sha256=APP_SHA)
         and plan['ca'] == dict(path=str(CA), sha256=CA_SHA), 'fixed_application_ca')
    for names, key in ((MODULES, 'modules'), (CANONICAL, 'canonical')):
        closed(plan[key], names, 'closed_module_pins')
        for value in plan[key].values():
            digest(value)
    closed(plan['tools'], ('git', 'powershell', 'python'), 'closed_tools')
    for name, path in (('git', GIT), ('powershell', PS), ('python', PYTHON)):
        pin(plan['tools'][name])
        need(Path(plan['tools'][name]['path']) == path, 'fixed_tool')
    need(type(plan['max_execution_seconds']) is int and plan['max_execution_seconds'] == 1800
         and type(plan['max_cleanup_seconds']) is int and plan['max_cleanup_seconds'] == 300
         and plan['max_launches'] == 3 and plan['max_capture_per_phase'] == 12, 'unchanged_finite_bounds')
    driver = plan['driver']
    closed(driver, ('kind', 'description', 'path', 'sha256', 'install_receipt', 'inventory', 'payload', 'dsn'), 'driver_schema')
    need(driver['kind'] in ('own', 'aws2'), 'closed_driver_kind')
    root = OWN_ROOT if driver['kind'] == 'own' else AWS_ROOT
    expected = ('ODBCPP Redshift', 'ODBCPP_PBI_RS_20261021_003') if driver['kind'] == 'own' else (
        'Amazon Redshift ODBC Driver (x64)', 'ODBCPP_AWS2_COMPARE007')
    need((driver['description'], driver['dsn']) == expected, 'fixed_driver_dsn')
    need(re.fullmatch('powerbi007-' + driver['kind'] + '-[0-9]{3}', plan['run_id']) is not None, 'fixed_run_namespace')
    image = Path(driver['path'])
    need(image == OWN_ROOT/'odbcpp.dll' if driver['kind'] == 'own' else
         image.parent == AWS_ROOT/'Drivers' and image.suffix.lower() == '.dll', 'fixed_installed_driver_namespace')
    digest(driver['sha256'])
    pin(driver['install_receipt'])
    need(type(driver['payload']) is list and 1 <= len(driver['payload']) <= 64, 'payload_bound')
    paths = set()
    for item in driver['payload']:
        pin(item)
        path = Path(item['path'])
        need(path.is_relative_to(root) and path not in paths, 'unique_installed_payload_namespace')
        paths.add(path)
    need(dict(path=driver['path'], sha256=driver['sha256']) in driver['payload'], 'driver_in_payload')
    if driver['kind'] == 'own':
        pin(driver['inventory'])
        need(Path(driver['inventory']['path']) == OWN_ROOT/'inventory.json', 'own_inventory')
    else:
        need(driver['inventory'] is None, 'vendor_not_own_inventory')
    need(plan['trace'] == dict(kind='own-driver' if driver['kind'] == 'own' else 'vendor-log-only',
                               maximum_bytes=16*1024*1024, polling_seconds=1), 'closed_log_trace_policy')
    return plan


def validate_message(message, plan, plan_sha, now, check_clock=True):
    closed(message, ('authority', 'connection'), 'stage_message')
    a, c = message['authority'], message['connection']
    closed(a, ('schema_version', 'state', 'independent_review', 'scope', 'source_head', 'run_id', 'nonce',
               'plan_sha256', 'execute_before_utc', 'execution_end_utc', 'cleanup_end_utc',
               'original_cap', 'cap_observed_utc', 'cap_observation'), 'authority_schema')
    need(a['schema_version'] == 1 and a['state'] == 'ROOT_ADMITTED' and a['independent_review'] == 'ACCEPT'
         and a['scope'] == plan['scope'] and a['source_head'] == plan['source_head']
         and a['run_id'] == plan['run_id'] and a['plan_sha256'] == plan_sha, 'admission_binding')
    need(type(a['nonce']) is str and re.fullmatch('[A-Za-z0-9_-]{16,128}', a['nonce']), 'oneuse_nonce')
    need(type(a['original_cap']) is int and 1 <= a['original_cap'] <= 16, 'finite_baseline_cap_1_to_16')
    pin(a['cap_observation'])
    observed, before, end, cleanup = map(utc, (a['cap_observed_utc'], a['execute_before_utc'],
                                             a['execution_end_utc'], a['cleanup_end_utc']))
    need(before <= end and 0 < (cleanup-end).total_seconds() <= plan['max_cleanup_seconds'], 'original_cleanup_horizon')
    if check_clock:
        need(0 <= (now-observed).total_seconds() <= 300, 'fresh_actual_baseline_cap')
        need(now <= before <= now + dt.timedelta(seconds=300)
             and 240 <= (end-now).total_seconds() <= plan['max_execution_seconds'], 'original_execution_horizon')
    closed(c, ('SERVER', 'PORT', 'DATABASE', 'UID', 'SSLCAFILE'), 'test_connection_no_password_or_admin')
    need(all(type(v) is str and 0 < len(v) <= 2048 and not any(x in v for x in '\r\n\0') for v in c.values()), 'bounded_connection')
    need((re.fullmatch(r'[a-z0-9-]+\.[a-z0-9.-]+\.redshift\.amazonaws\.com', c['SERVER'])
          or c['SERVER'] == SERVERLESS_HOST)
         and c['PORT'] == '5439' and c['DATABASE'] == 'odbcpp_pilot'
         and c['UID'] == 'odbcpp_pilot_test' and Path(c['SSLCAFILE']) == CA, 'exact_test_identity_tls')
    return a, c


def bound_record(record, authority, plan, plan_sha, stage):
    need(record['schema_version'] == 1 and record['run_id'] == authority['run_id']
         and record['nonce'] == authority['nonce'] and record['source_head'] == plan['source_head']
         and record['plan_sha256'] == plan_sha and record['stage'] == stage, 'control_binding')
    need(type(record['phase']) is int and record['phase'] in (0, 1, 2), 'fixed_phase')


RECORD_KEYS = ('schema_version', 'run_id', 'nonce', 'source_head', 'plan_sha256', 'stage', 'phase')


def validate_ack(ack, authority, plan, plan_sha, request, request_sha, baseline_sha=None):
    stage = ack.get('stage') if type(ack) is dict else None
    extra = (('target_cap', 'baseline_closed_sha256', 'zero_sessions', 'measurement_review', 'cap_decision')
             if stage == 'cap-ready' else ('measurement_pin',) if stage == 'reviewed-measurement' else ())
    closed(ack, (*RECORD_KEYS, 'status', 'request_sha256', 'evidence', *extra), 'ack_schema')
    need(stage in (*SAMPLES, 'cap-ready', 'reviewed-measurement', 'abort'), 'fixed_ack_stage')
    bound_record(ack, authority, plan, plan_sha, stage)
    need(ack['status'] == 'ACK' and ack['request_sha256'] == request_sha
         and ack['phase'] == request['phase'], 'ack_exact_request')
    need(type(ack['evidence']) is list and 1 <= len(ack['evidence']) <= 8, 'bounded_ack_evidence')
    for evidence in ack['evidence']:
        pin(evidence)
    if stage == 'cap-ready':
        need(type(ack['target_cap']) is int and 1 <= ack['target_cap'] <= 16
             and ack['phase'] == 0 and ack['baseline_closed_sha256'] == baseline_sha, 'closed_cap_ready')
        for name in ('zero_sessions', 'measurement_review', 'cap_decision'):
            pin(ack[name])
    if stage == 'reviewed-measurement':
        need(ack['phase'] == 0, 'baseline_measurement_phase')
        pin(ack['measurement_pin'])
    return ack


def record_base(authority, plan, plan_sha, stage, phase):
    return dict(schema_version=1, run_id=authority['run_id'], nonce=authority['nonce'],
                source_head=plan['source_head'], plan_sha256=plan_sha, stage=stage, phase=phase)


def safe_readers(result):
    return (result.get('readers_known') is True and result.get('receipt', {}).get('treeEmpty') is True
            and result['receipt'].get('jobActiveProcesses') == 0
            and result['receipt'].get('heldProcessHandlesStopped') is True
            and all(p.get('stopped') is True for p in result.get('held_observed_processes', [])))
