"""Bind bounded OpenSSL unit/live evidence; never qualify a matrix row."""
import argparse
import importlib.util
import hashlib
import json
from pathlib import Path
import re

# Load only the parser in this source tree; never fall back to cwd/sys.path.
try:
    _xml_spec = importlib.util.spec_from_file_location(
        'odbcpp_xml_evidence', Path(__file__).resolve().parents[1] / 'odbcpp_xml_evidence.py')
    _xml_module = importlib.util.module_from_spec(_xml_spec)
    _xml_spec.loader.exec_module(_xml_module)
    parse_xml_file = _xml_module.parse_xml_file
    parse_xml_bytes = _xml_module.parse_xml_bytes
    XmlLimits = _xml_module.XmlLimits
except Exception:
    raise RuntimeError('Bounded XML evidence parser unavailable') from None

REQUIRED_TESTS = {
    'test_crypto_runtime_identity', 'test_crypto_identity_evidence_rules',
    'test_crypto_identity_evidence_recording', 'verified_crypto_tls_interop',
    'test_crypto_artifact_dependency_form', 'test_driver_export_surface',
}
TLS_MARKERS = {'TLSv1_1: control accepted; production client rejected downgrade'}
for version in ('TLSv1_2', 'TLSv1_3'):
    for host, outcome in (('localhost', 'accept'), ('127.0.0.1', 'accept'),
                          ('wrong.example.test', 'hostname'), ('127.0.0.2', 'hostname'),
                          ('localhost', 'trust')):
        TLS_MARKERS.add(f'{version}: {host}: {outcome} passed')
PROFILES = {('Linux', 'SYSTEM_SHARED'), ('Linux', 'BUNDLED_STATIC'),
            ('Darwin', 'SYSTEM_SHARED'), ('Windows', 'BUNDLED_SHARED')}
COHABITATION_MARKERS = {
    'COHABITATION: host operations across driver lifetime passed',
    'COHABITATION: repeated driver references passed',
}

LIVE_TESTS = {'CryptoProfileLiveTest.' + name for name in (
    'CompletesVerifiedTlsScramQuery', 'RejectsUntrustedCertificate', 'RejectsHostnameMismatch')}

RELOAD_TEST = 'test_odbc_trace_reload'
RELOAD_REPORT = 'odbcpp-trace-reload-gtest.xml'
RELOAD_CASES = {
    'OdbcTraceReload.UnloadClosesTheTraceFileAndReloadStartsFresh':
        'The platform loader kept the driver image resident after unload; '
        'unload-time sink closure was not observable.',
    'OdbcTraceReload.DisabledTracingAlsoRetiresStorageAcrossImageReloads':
        'The loader retained the image; disabled unload cleanup was not observable.',
}
RELOAD_PREFIX = 'ODBCPP_TRACE_RELOAD_RESULT_V1 '
RELOAD_RESULT = re.compile(
    re.escape(RELOAD_PREFIX) + r'run=([0-9]{1,20}-[0-9]{1,20}) '
    r'selected=([0-9]{1,9}) passed=([0-9]{1,9}) skipped=([0-9]{1,9}) '
    r'failed=([0-9]{1,9}) disabled=([0-9]{1,9}) result=([0-9]{1,9}) exit=([0-9]{1,9})')


def report_count(node, field, default=None):
    value = node.get(field, default)
    if value is None or re.fullmatch(r'[0-9]{1,9}', value) is None:
        raise RuntimeError('Malformed or missing report count: ' + field)
    return int(value)


def validate_reload_report(case, report, platform, linkage):
    """Bind native results to this CTest invocation, not a historical skip regex."""
    lines = [line for line in (case.findtext('system-out') or '').splitlines()
             if line.startswith(RELOAD_PREFIX)]
    match = RELOAD_RESULT.fullmatch(lines[0]) if len(lines) == 1 else None
    if match is None:
        raise RuntimeError('Missing, duplicate or malformed native reload result')
    run = match[1]
    selected, passed, skipped, failed, disabled, native, exit_code = map(int, match.groups()[1:])
    if (selected != 2 or passed + skipped != 2 or failed != 0 or disabled != 0
            or native != 0 or skipped not in (0, 2) or exit_code != (77 if skipped else 0)):
        raise RuntimeError('Failed, filtered or partial native reload result')
    with report.open('rb') as source:
        data = source.read(2 * 1024 * 1024 + 1)
    if len(data) > 2 * 1024 * 1024:
        raise RuntimeError('Oversized native reload report')
    root = parse_xml_bytes(data, limits=XmlLimits(max_bytes=2 * 1024 * 1024))
    suites = root.findall('testsuite')
    cases = list(root.iter('testcase'))
    names = [f'{item.get("classname")}.{item.get("name")}' for item in cases]
    if (root.tag != 'testsuites' or len(suites) != 1 or len(list(root.iter('testsuite'))) != 1
            or suites[0].get('name') != 'OdbcTraceReload' or len(suites[0].findall('testcase')) != 2
            or len(names) != 2 or set(names) != set(RELOAD_CASES)
            or root.get('odbcpp_trace_reload_run') != run):
        raise RuntimeError('Stale, missing, duplicate or unexpected native reload inventory')
    if any(list(root.iter(tag)) for tag in ('failure', 'error')):
        raise RuntimeError('Native reload report contains failures')
    for node in (root, suites[0]):
        if (report_count(node, 'tests') != 2
                or any(report_count(node, field) != 0 for field in ('failures', 'errors', 'disabled'))
                or report_count(node, 'skipped', str(skipped)) != skipped):
            raise RuntimeError('Inconsistent native reload report totals')
    # GoogleTest 1.14 has no root skipped attribute, but does emit it per suite.
    if report_count(suites[0], 'skipped') != skipped:
        raise RuntimeError('Missing native reload skip count')
    records = []
    for item, name in zip(cases, names):
        skip_nodes = item.findall('skipped')
        if (item.get('status') != 'run' or item.get('result') != ('skipped' if skipped else 'completed')
                or len(skip_nodes) != (1 if skipped else 0)):
            raise RuntimeError('Suppressed or inconsistent native reload case')
        record = {'name': name, 'status': 'SKIP' if skipped else 'PASS'}
        properties = item.findall('properties/property')
        if skipped:
            if (len(properties) != 1 or properties[0].attrib != {
                    'name': 'odbcpp_trace_reload_boundary', 'value': 'driver_image_retained'}):
                raise RuntimeError('Unknown native loader boundary')
            message = skip_nodes[0].get('message', '')
            parts = message.splitlines()
            if (len(message) > 4096 or len(parts) < 2
                    or re.fullmatch(r'.*test_odbc_trace_reload\.cpp:[1-9][0-9]{0,6}', parts[0]) is None
                    or parts[1] != RELOAD_CASES[name]
                    or (skip_nodes[0].text or '').rstrip() != message.rstrip()):
                raise RuntimeError('Unknown native loader reason')
            tail = parts[2:]
            if tail and (len(tail) != 2 or tail[0] != 'Google Test trace:'
                         or re.fullmatch(r'.*test_odbc_trace_reload\.cpp:[1-9][0-9]{0,6}: [01]', tail[1]) is None):
                raise RuntimeError('Unexpected native loader reason context')
            record.update(reasonCode='driver_image_retained', reason=RELOAD_CASES[name])
        elif properties:
            raise RuntimeError('Unexpected passing reload boundary properties')
        records.append(record)
    wrapper_skips = case.findall('skipped')
    if skipped:
        if ((platform, linkage) != ('Darwin', 'SYSTEM_SHARED') or case.get('status') != 'notrun'
                or len(wrapper_skips) != 1 or wrapper_skips[0].get('message') != 'SKIP_RETURN_CODE=77'):
            raise RuntimeError('Unapproved platform or historical reload skip producer')
    elif case.get('status') != 'run' or wrapper_skips:
        raise RuntimeError('Unexpected passing reload wrapper status')
    return {'ctestName': RELOAD_TEST, 'run': run, 'selected': 2, 'passed': passed,
            'skipped': skipped, 'failed': 0, 'disabled': 0, 'exitCode': exit_code,
            'reportSha256': hashlib.sha256(data).hexdigest(),
            'nativeCases': sorted(records, key=lambda value: value['name']),
            'unloadClosureObserved': skipped == 0,
            'status': 'UNAVAILABLE_LOADER_BOUNDARY' if skipped else 'PASS'}


def validate_live_report(report):
    root = parse_xml_file(report)
    cases = list(root.iter('testcase'))
    names = [f'{case.get("classname")}.{case.get("name")}' for case in cases]
    if (root.tag not in ('testsuites', 'testsuite') or len(names) != len(LIVE_TESTS)
            or set(names) != LIVE_TESTS or any(case.get('status') != 'run'
                                             or case.get('result') != 'completed' for case in cases)):
        raise RuntimeError('Mandatory live cases missing, duplicate, unexpected or not completed')
    for suite in (node for node in root.iter() if node.tag in ('testsuites', 'testsuite')):
        if (any(int(suite.get(field, '0')) != 0 for field in ('failures', 'errors', 'disabled', 'skipped'))
                or int(suite.get('tests', str(len(list(suite.iter('testcase')))))) != len(list(suite.iter('testcase')))):
            raise RuntimeError('Inconsistent or unsuccessful live report totals')
    if any(list(root.iter(tag)) for tag in ('failure', 'error', 'skipped')):
        raise RuntimeError('Live report contains failed or skipped tests')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def collect(build, driver, probe, report, platform, live_report=None, reload_report=None):
    manifest_path = build / 'odbcpp-crypto-manifest.json'
    identity_path = build / 'odbcpp-crypto-identity-evidence.json'
    artifact_path = build / 'odbcpp-crypto-artifact-evidence.json'
    manifest, identity, artifact = (json.loads(path.read_text()) for path in
                                   (manifest_path, identity_path, artifact_path))
    linkage = manifest['requestedLinkage']
    if (manifest['provider'] != 'OPENSSL' or (platform, linkage) not in PROFILES
            or manifest['fipsClaimed'] is not False):
        raise RuntimeError('Profile outside the bounded OpenSSL matrix')
    for record in (identity, artifact):
        if (record['provider'] != 'OPENSSL' or record['requestedLinkage'] != linkage
                or record['qualificationClaimed'] is not False):
            raise RuntimeError('Inconsistent identity/artifact profile or qualification claim')
    if (identity['artifactSha256'] != sha(driver) or artifact['artifactSha256'] != sha(driver)
            or identity['probeSha256'] != sha(probe)
            or identity['manifestSha256'] != sha(manifest_path)):
        raise RuntimeError('Evidence input hash mismatch')
    for field in ('compileVersion', 'runtimeVersion'):
        version = re.match(r'^OpenSSL (\d+\.\d+\.\d+[a-z]*)(?: |$)', identity[field])
        if version is None or version[1] != manifest['compileVersion']:
            raise RuntimeError('Identity version differs from configured manifest')
    if (identity['activeFips'] is not False or identity['minimumTlsVersion'] != '1.2'
            or identity['verifyPeer'] is not True or identity['verifyHostname'] is not True):
        raise RuntimeError('Unexpected FIPS state or weaker default TLS policy')
    if (artifact['platform'] != platform or artifact['dependencyFormVerified'] is not True
            or artifact['observedDependencyForm'] != ('static' if linkage == 'BUNDLED_STATIC' else 'shared')):
        raise RuntimeError('Artifact platform/linkage evidence mismatch')
    root = parse_xml_file(report)
    cases = list(root.iter('testcase'))
    wrapper_skips = [case for case in cases if case.find('skipped') is not None]
    if (root.tag != 'testsuite' or any(report_count(root, field, '0') != 0
                                    for field in ('failures', 'errors', 'disabled'))
            or report_count(root, 'tests', str(len(cases))) != len(cases)
            or report_count(root, 'skipped', '0') != len(wrapper_skips)):
        raise RuntimeError('Inconsistent or unsuccessful unit report totals')
    if any(list(root.iter(tag)) for tag in ('failure', 'error')):
        raise RuntimeError('Unit report contains failed tests')
    names = [case.get('name') for case in cases]
    reload_cases = [case for case in cases if case.get('name') == RELOAD_TEST]
    if len(reload_cases) != 1:
        raise RuntimeError('Reload unit case missing or duplicate')
    reload_report = Path(reload_report) if reload_report is not None else build / RELOAD_REPORT
    reload = validate_reload_report(reload_cases[0], reload_report, platform, linkage)
    if (not all(names) or len(names) != len(set(names)) or any(case.get('status') != 'run'
            or case.find('skipped') is not None for case in cases if case is not reload_cases[0])
            or list(root.iter('skipped')) != list(reload_cases[0].iter('skipped'))):
        raise RuntimeError('Unit report contains unknown, duplicate or not-run cases')
    required_tests = set(REQUIRED_TESTS)
    if linkage == 'SYSTEM_SHARED':
        required_tests.add('test_shared_crypto_cohabitation')
    for name in required_tests:
        matching = [case for case in cases if case.get('name') == name]
        if len(matching) != 1 or matching[0].get('status') != 'run':
            raise RuntimeError(f'Mandatory unit case missing, duplicate or not run: {name}')
    peer = next(case for case in cases if case.get('name') == 'verified_crypto_tls_interop')
    lines = (peer.findtext('system-out') or '').splitlines()
    if any(lines.count(marker) != 1 for marker in TLS_MARKERS):
        raise RuntimeError('Independent TLS case/control output missing or duplicated')
    if linkage == 'SYSTEM_SHARED':
        host = next(case for case in cases if case.get('name') == 'test_shared_crypto_cohabitation')
        host_lines = (host.findtext('system-out') or '').splitlines()
        if any(host_lines.count(marker) != 1 for marker in COHABITATION_MARKERS):
            raise RuntimeError('Shared-provider cohabitation output missing or duplicated')
    if live_report is not None:
        if (platform, linkage) != ('Linux', 'SYSTEM_SHARED'):
            raise RuntimeError('Live summary binding is limited to Linux SYSTEM_SHARED')
        validate_live_report(live_report)
    evidence = {
        'schemaVersion': 1, 'profile': f'OPENSSL/{linkage}/{platform}',
        'qualificationClaimed': False,
        'scope': 'Unit identity, default policy, artifact/export checks and independent TLS peer',
        'liveDriverPackageCoexistenceAcceptanceEvaluated': False,
        'liveDriverTlsAcceptanceEvaluated': live_report is not None,
        'mandatoryLiveDriverTests': sorted(LIVE_TESTS) if live_report is not None else [],
        'mandatoryUnitTests': sorted(required_tests),
        'sharedProviderHostLifecycleEvaluated': linkage == 'SYSTEM_SHARED',
        'mandatoryTlsPeerCases': sorted(TLS_MARKERS),
        'unitReportCounts': {'selected': len(cases), 'passed': len(cases) - len(wrapper_skips),
                             'skipped': len(wrapper_skips), 'failed': 0, 'disabled': 0},
        'traceReload': reload,
        'nonMandatoryLoaderBoundaries': [reload] if reload['skipped'] else [],
        'inputs': {name: sha(path) for name, path in (
            ('driver', driver), ('probe', probe), ('manifest', manifest_path),
            ('identity', identity_path), ('artifact', artifact_path), ('unitReport', report))},
    }
    evidence['inputs']['nativeReloadReport'] = reload['reportSha256']
    if live_report is not None:
        evidence['scope'] += '; live driver verified TLS/SCRAM query, trust and hostname rejection'
        evidence['inputs']['liveReport'] = sha(live_report)
    return evidence


def main():
    parser = argparse.ArgumentParser()
    for name in ('build', 'driver', 'probe', 'unit-report', 'platform', 'output'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--live-report')
    parser.add_argument('--reload-report', help='Actual standard GoogleTest XML if GTEST_OUTPUT overrides the build-root default')
    args = parser.parse_args()
    output = Path(args.output)
    output.unlink(missing_ok=True)
    evidence = collect(Path(args.build), Path(args.driver), Path(args.probe),
                       Path(args.unit_report), args.platform,
                       Path(args.live_report) if args.live_report else None,
                       Path(args.reload_report) if args.reload_report else None)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
