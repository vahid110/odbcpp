#!/usr/bin/env python3
"""Pinned authentication and bounded SDK direct-query proof; no ODBC driver claim."""
import argparse
import json
import os
import re
import sys
from pathlib import Path
import subprocess
import tempfile
import time
import uuid
import xml.etree.ElementTree as ET

IMAGE = 'mysql:8.4.11@sha256:6ea90827b1100f8f2ae306a539f86d2c264a26ed435a2a9f75551dd5c3aeb242'
PASSWORD = 'odbcpp-public-mysql-fixture-password'

def run(args, **kw):
    return subprocess.run(args, check=True, text=True, capture_output=True,
                          timeout=kw.pop('timeout', 30), **kw)

DECIMAL_SUITE = 'MySqlDecimalResultsIntegrationTest'
DECIMAL_CASE = 'DeclaredSignedBoundsNullAndOwnershipAgreeAcrossProtocols'

def validate_decimal_xml(path):
    root = ET.parse(path).getroot()
    suites = list(root) if root.tag == 'testsuites' else [root]
    if len(suites) != 1 or suites[0].tag != 'testsuite':
        raise RuntimeError('Unexpected MySQL decimal inventory')
    suite = suites[0]
    cases = list(suite)
    if (suite.get('name') != DECIMAL_SUITE or suite.get('tests') != '1'
            or any(suite.get(k, '0') != '0' for k in ('failures', 'errors', 'disabled', 'skipped'))
            or len(cases) != 1 or cases[0].tag != 'testcase'
            or cases[0].get('name') != DECIMAL_CASE
            or cases[0].get('classname') != DECIMAL_SUITE
            or cases[0].get('status') != 'run'
            or cases[0].get('result') != 'completed'
            or any(c.tag in ('failure', 'error', 'skipped') for c in cases[0])):
        raise RuntimeError('Unexpected MySQL decimal inventory')

def run_decimal(binary, junit, port, ca):
    env = os.environ.copy()
    for key in list(env):
        if key.startswith('GTEST_'):
            del env[key]
    env.update(ODBCPP_MYSQL_TEST_USER='sdk', ODBCPP_MYSQL_TEST_PASSWORD=PASSWORD,
               ODBCPP_MYSQL_TEST_HOST='localhost', ODBCPP_MYSQL_TEST_PORT=port,
               ODBCPP_MYSQL_TEST_CA_FILE=str(ca.resolve()),
               ODBCPP_MYSQL_DECIMAL_FIXTURE_ADMITTED='pinned-8.4.11-temporary-decimal')
    try:
        run([str(binary), '--gtest_filter=' + DECIMAL_SUITE + '.' + DECIMAL_CASE,
             '--gtest_repeat=1', '--gtest_output=xml:' + str(junit)], env=env, timeout=60)
        validate_decimal_xml(junit)
    except Exception:
        raise RuntimeError('MySQL decimal result proof failed') from None

def cleanup_fixture(name):
    # A failed/timed-out create may still have created this exact owned name.
    subprocess.run(['docker', 'rm', '--force', name], capture_output=True, timeout=30)
    remaining = run(['docker', 'ps', '--all', '--filter', 'name=^/' + name + '$',
                     '--format', '{{.ID}}'], timeout=10)
    if remaining.stdout.strip():
        raise RuntimeError('MySQL fixture cleanup failed')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--decimal-test-binary')
    parser.add_argument('--decimal-junit')
    args = parser.parse_args()
    if bool(args.decimal_test_binary) != bool(args.decimal_junit):
        parser.error('decimal binary and JUnit must be supplied together')
    decimal_binary = Path(args.decimal_test_binary).resolve() if args.decimal_test_binary else None
    decimal_junit = Path(args.decimal_junit).resolve() if args.decimal_junit else None
    if decimal_binary is not None:
        if not decimal_binary.is_file() or not os.access(decimal_binary, os.X_OK):
            parser.error('decimal binary must be executable')
        decimal_junit.parent.mkdir(parents=True, exist_ok=True)
        decimal_junit.unlink(missing_ok=True)
    output = Path(args.output)
    output.unlink(missing_ok=True)
    name = 'odbcpp-mysql-' + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix='odbcpp-mysql-tls-') as temp:
        directory = Path(temp)
        certs = directory / 'certs'
        certs.mkdir(mode=0o755)
        for ca in ('ca', 'wrong-ca'):
            run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
                 '-subj', '/CN=ODBCPP test ' + ca, '-keyout', str(certs / (ca + '.key')),
                 '-out', str(certs / (ca + '.pem'))])
        run(['openssl', 'req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
             '-keyout', str(certs / 'server.key'), '-out', str(certs / 'server.csr')])
        (certs / 'server.ext').write_text('subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n')
        run(['openssl', 'x509', '-req', '-in', str(certs / 'server.csr'), '-CA', str(certs / 'ca.pem'),
             '-CAkey', str(certs / 'ca.key'), '-CAcreateserial', '-days', '2',
             '-extfile', str(certs / 'server.ext'), '-out', str(certs / 'server.pem')])
        # Only disposable fixture files. Host parent remains private; mount omits CA keys.
        served = directory / 'served'
        served.mkdir(mode=0o755)
        for file in ('ca.pem', 'server.pem', 'server.key'):
            (served / file).write_bytes((certs / file).read_bytes())
            (served / file).chmod(0o644)
        create_attempted = False
        try:
            run(['docker', 'pull', IMAGE], timeout=300)
            create_attempted = True
            run(['docker', 'run', '--detach', '--name', name, '--publish', '127.0.0.1::3306',
                 '--mount', 'type=bind,src=' + str(served) + ',dst=/odbcpp-tls,readonly',
                 '--env', 'MYSQL_ROOT_PASSWORD=' + PASSWORD,
                 '--env', 'MYSQL_DATABASE=odbcpp', '--env', 'MYSQL_USER=sdk',
                 '--env', 'MYSQL_PASSWORD=' + PASSWORD, IMAGE,
                 '--ssl-ca=/odbcpp-tls/ca.pem', '--ssl-cert=/odbcpp-tls/server.pem',
                 '--ssl-key=/odbcpp-tls/server.key', '--require-secure-transport=ON'], timeout=60)
            port = run(['docker', 'port', name, '3306/tcp']).stdout.strip().rsplit(':', 1)[1]
            sql_prefix = ['docker', 'exec', '--env', 'MYSQL_PWD=' + PASSWORD, name,
                          'mysql', '--batch', '--skip-column-names', '--user=root']
            deadline = time.monotonic() + 150
            while True:
                try:
                    ready = run(sql_prefix + ['--execute', 'SELECT VERSION(), @@global.skip_networking'], timeout=10).stdout.strip()
                    if ready.endswith('\t0'):
                        version = ready.split('\t')[0]
                        break
                except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError('MySQL fixture readiness failed')
                time.sleep(1)
            if version != '8.4.11':
                raise RuntimeError('Pinned MySQL version mismatch')
            plugin = run(sql_prefix + ['--execute', "SELECT plugin FROM mysql.user WHERE user='sdk'"]).stdout.strip()
            if plugin != 'caching_sha2_password':
                raise RuntimeError('Pinned authentication plugin mismatch')
            run(sql_prefix + ['--execute', 'FLUSH PRIVILEGES'])
            results = []
            for case, host, trust, password in (
                ('full', 'localhost', 'ca.pem', PASSWORD),
                ('cached', 'localhost', 'ca.pem', PASSWORD),
                ('session', 'localhost', 'ca.pem', PASSWORD),
                ('reject-auth', 'localhost', 'ca.pem', 'invalid-public-fixture-password'),
                ('reject-tls', 'localhost', 'wrong-ca.pem', PASSWORD),
                ('reject-tls', '127.0.0.1', 'ca.pem', PASSWORD),
            ):
                env = os.environ.copy()
                env.update(ODBCPP_MYSQL_TEST_USER='sdk', ODBCPP_MYSQL_TEST_PASSWORD=password)
                try:
                    result = run([str(Path(args.probe).resolve()), host, port, str(certs / trust), case], env=env)
                except subprocess.CalledProcessError as failure:
                    # Only fixed check IDs and numeric codes; never dump native stderr.
                    for line in (failure.stderr or '').splitlines():
                        if re.fullmatch(r'FAIL session-(?:check|query)-[0-9]+(?: code [0-9]+)?', line):
                            print(line, flush=True)
                    raise RuntimeError('MySQL probe rejected fixture') from None
                if result.stdout.strip() != 'PASS ' + case:
                    raise RuntimeError('Unexpected MySQL probe result')
                results.append({'case': case, 'host': host, 'trust': trust, 'passed': True})
                print('PASS MySQL ' + case + ' (' + host + ', ' + trust + ')', flush=True)
            if decimal_binary is not None:
                run_decimal(decimal_binary, decimal_junit, port, certs / 'ca.pem')
            output.parent.mkdir(parents=True, exist_ok=True)
            evidence = {'image': IMAGE, 'version': version, 'plugin': plugin,
                                          'cases': results, 'sdkDecimalResultsProven': decimal_binary is not None,
                                          'sdkDirectSessionProven': True, 'odbcSessionClaimed': False}
        finally:
            original_failure = sys.exc_info()[0] is not None
            if create_attempted:
                try:
                    cleanup_fixture(name)
                except Exception:
                    if not original_failure:
                        raise RuntimeError('MySQL fixture cleanup failed') from None
        output.write_text(json.dumps(evidence, indent=2) + '\n')

if __name__ == '__main__':
    try:
        main()
    except Exception:
        # Never echo SQL, environment, subprocess stderr or credential payloads.
        raise SystemExit('MySQL live authentication proof failed')
