"""Bind existing OpenSSL unit qualification evidence; never qualify a matrix row."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

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


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def collect(build, driver, probe, report, platform):
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
    root = ET.parse(report).getroot()
    cases = list(root.iter('testcase'))
    if (root.tag != 'testsuite' or any(int(root.get(field, '0')) != 0
                                    for field in ('failures', 'errors', 'disabled', 'skipped'))
            or int(root.get('tests', str(len(cases)))) != len(cases)):
        raise RuntimeError('Inconsistent or unsuccessful unit report totals')
    if any(list(root.iter(tag)) for tag in ('failure', 'error', 'skipped')):
        raise RuntimeError('Unit report contains failed or skipped tests')
    for name in REQUIRED_TESTS:
        matching = [case for case in cases if case.get('name') == name]
        if len(matching) != 1 or matching[0].get('status') != 'run':
            raise RuntimeError(f'Mandatory unit case missing, duplicate or not run: {name}')
    peer = next(case for case in cases if case.get('name') == 'verified_crypto_tls_interop')
    lines = (peer.findtext('system-out') or '').splitlines()
    if any(lines.count(marker) != 1 for marker in TLS_MARKERS):
        raise RuntimeError('Independent TLS case/control output missing or duplicated')
    return {
        'schemaVersion': 1, 'profile': f'OPENSSL/{linkage}/{platform}',
        'qualificationClaimed': False,
        'scope': 'Unit identity, default policy, artifact/export checks and independent TLS peer only',
        'liveDriverPackageCoexistenceAcceptanceEvaluated': False,
        'mandatoryUnitTests': sorted(REQUIRED_TESTS),
        'mandatoryTlsPeerCases': sorted(TLS_MARKERS),
        'inputs': {name: sha(path) for name, path in (
            ('driver', driver), ('probe', probe), ('manifest', manifest_path),
            ('identity', identity_path), ('artifact', artifact_path), ('unitReport', report))},
    }


def main():
    parser = argparse.ArgumentParser()
    for name in ('build', 'driver', 'probe', 'unit-report', 'platform', 'output'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    output = Path(args.output)
    output.unlink(missing_ok=True)
    evidence = collect(Path(args.build), Path(args.driver), Path(args.probe),
                       Path(args.unit_report), args.platform)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
