#!/usr/bin/env python3
"""Pinned authentication and bounded SDK direct-query proof; no ODBC driver claim."""
import argparse
import importlib.util
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

# Load the reviewed sibling from this source directory, independent of cwd.
try:
    _receipt_spec = importlib.util.spec_from_file_location(
        'odbcpp_mysql_receipt_xml', Path(__file__).resolve().with_name('mysql_datetime_receipt_xml.py'))
    _receipt_module = importlib.util.module_from_spec(_receipt_spec)
    _receipt_spec.loader.exec_module(_receipt_module)
    validate_datetime_receipt_xml = _receipt_module.validate_datetime_receipt_xml
except Exception:
    raise RuntimeError('MySQL DATETIME receipt validator unavailable') from None

DATETIME_RECEIPT_SUITE = 'MySqlDatetimeReceiptObservationIntegrationTest'
DATETIME_RECEIPT_CASE = 'ActualCastParameterMetadataRefusalIsObserved'

def run_datetime_receipt(binary, junit, port, ca):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('GTEST_')
           and not (key.startswith('ODBCPP_MYSQL_') and key.endswith('_FIXTURE_ADMITTED'))}
    env.update(ODBCPP_MYSQL_TEST_USER='sdk', ODBCPP_MYSQL_TEST_PASSWORD=PASSWORD,
               ODBCPP_MYSQL_TEST_HOST='localhost', ODBCPP_MYSQL_TEST_PORT=port,
               ODBCPP_MYSQL_TEST_CA_FILE=str(ca.resolve()),
               ODBCPP_MYSQL_DATETIME_RECEIPT_FIXTURE_ADMITTED='pinned-8.4.11-temporary-datetime-receipt-observation')
    try:
        run([str(binary), '--gtest_filter=' + DATETIME_RECEIPT_SUITE + '.' + DATETIME_RECEIPT_CASE,
             '--gtest_repeat=1', '--gtest_output=xml:' + str(junit)], env=env, timeout=60)
        return validate_datetime_receipt_xml(junit)
    except Exception:
        raise RuntimeError('MySQL DATETIME receipt observation failed') from None

# Load the reviewed sibling from this source directory, independent of cwd.
try:
    _decimal_receipt_spec = importlib.util.spec_from_file_location(
        'odbcpp_mysql_decimal_receipt_xml', Path(__file__).resolve().with_name('mysql_decimal_receipt_xml.py'))
    _decimal_receipt_module = importlib.util.module_from_spec(_decimal_receipt_spec)
    _decimal_receipt_spec.loader.exec_module(_decimal_receipt_module)
    validate_decimal_receipt_xml = _decimal_receipt_module.validate_decimal_receipt_xml
except Exception:
    raise RuntimeError('MySQL DECIMAL receipt validator unavailable') from None

DECIMAL_RECEIPT_SUITE = 'MySqlDecimalReceiptObservationIntegrationTest'
DECIMAL_RECEIPT_CASE = 'ActualCastParameterMetadataRefusalIsObserved'

def run_decimal_receipt(binary, junit, port, ca):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('GTEST_')
           and not (key.startswith('ODBCPP_MYSQL_') and key.endswith('_FIXTURE_ADMITTED'))}
    env.update(ODBCPP_MYSQL_TEST_USER='sdk', ODBCPP_MYSQL_TEST_PASSWORD=PASSWORD,
               ODBCPP_MYSQL_TEST_HOST='localhost', ODBCPP_MYSQL_TEST_PORT=port,
               ODBCPP_MYSQL_TEST_CA_FILE=str(ca.resolve()),
               ODBCPP_MYSQL_DECIMAL_RECEIPT_FIXTURE_ADMITTED='pinned-8.4.11-temporary-decimal-receipt-observation')
    try:
        run([str(binary), '--gtest_filter=' + DECIMAL_RECEIPT_SUITE + '.' + DECIMAL_RECEIPT_CASE,
             '--gtest_repeat=1', '--gtest_output=xml:' + str(junit)], env=env, timeout=60)
        return validate_decimal_receipt_xml(junit)
    except Exception:
        raise RuntimeError('MySQL DECIMAL receipt observation failed') from None

def run(args, **kw):
    return subprocess.run(args, check=True, text=True, capture_output=True,
                          timeout=kw.pop('timeout', 30), **kw)

DECIMAL_SUITE = 'MySqlDecimalResultsIntegrationTest'
DECIMAL_CASE = 'DeclaredSignedBoundsNullAndOwnershipAgreeAcrossProtocols'
DATETIME_SUITE = 'MySqlDatetimeResultsIntegrationTest'
DATETIME_CASE = 'CalendarPrecisionNullAndOwnershipAgreeAcrossProtocols'
DATE_PARAMETERS_SUITE = 'MySqlDateParametersIntegrationTest'
DATE_PARAMETERS_CASE = 'PhysicalDateComparisonBoundsNullAndOwningReceipts'

def validate_datetime_xml(path):
    root = ET.parse(path).getroot()
    if (root.tag != 'testsuites' or root.get('tests') != '1'
            or any(root.get(k) != '0' for k in ('failures', 'errors', 'disabled'))
            or root.get('skipped', '0') != '0' or len(root) != 1):
        raise RuntimeError('Unexpected MySQL DATETIME inventory')
    suite = root[0]
    if (suite.tag != 'testsuite' or suite.get('name') != DATETIME_SUITE
            or suite.get('tests') != '1'
            or any(suite.get(k) != '0' for k in ('failures', 'errors', 'disabled', 'skipped'))
            or len(suite) != 1):
        raise RuntimeError('Unexpected MySQL DATETIME inventory')
    case = suite[0]
    if (case.tag != 'testcase' or case.get('name') != DATETIME_CASE
            or case.get('classname') != DATETIME_SUITE or case.get('status') != 'run'
            or case.get('result') != 'completed' or len(case) != 0):
        raise RuntimeError('Unexpected MySQL DATETIME inventory')

def run_datetime(binary, junit, port, ca):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('GTEST_')
           and not (key.startswith('ODBCPP_MYSQL_') and key.endswith('_FIXTURE_ADMITTED'))}
    env.update(ODBCPP_MYSQL_TEST_USER='sdk', ODBCPP_MYSQL_TEST_PASSWORD=PASSWORD,
               ODBCPP_MYSQL_TEST_HOST='localhost', ODBCPP_MYSQL_TEST_PORT=port,
               ODBCPP_MYSQL_TEST_CA_FILE=str(ca.resolve()),
               ODBCPP_MYSQL_DATETIME_FIXTURE_ADMITTED='pinned-8.4.11-temporary-datetime-valid')
    try:
        run([str(binary), '--gtest_filter=' + DATETIME_SUITE + '.' + DATETIME_CASE,
             '--gtest_repeat=1', '--gtest_output=xml:' + str(junit)], env=env, timeout=60)
        validate_datetime_xml(junit)
    except Exception:
        raise RuntimeError('MySQL DATETIME result proof failed') from None

def validate_date_parameters_xml(path):
    root = ET.parse(path).getroot()
    if (root.tag != 'testsuites' or root.get('tests') != '1'
            or any(root.get(k) != '0' for k in ('failures', 'errors', 'disabled'))
            or root.get('skipped', '0') != '0' or len(root) != 1):
        raise RuntimeError('Unexpected MySQL DATE parameter inventory')
    suite = root[0]
    if (suite.tag != 'testsuite' or suite.get('name') != DATE_PARAMETERS_SUITE
            or suite.get('tests') != '1'
            or any(suite.get(k) != '0' for k in ('failures', 'errors', 'disabled', 'skipped'))
            or len(suite) != 1):
        raise RuntimeError('Unexpected MySQL DATE parameter inventory')
    case = suite[0]
    if (case.tag != 'testcase' or case.get('name') != DATE_PARAMETERS_CASE
            or case.get('classname') != DATE_PARAMETERS_SUITE or case.get('status') != 'run'
            or case.get('result') != 'completed' or len(case) != 0):
        raise RuntimeError('Unexpected MySQL DATE parameter inventory')

def run_date_parameters(binary, junit, port, ca):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('GTEST_')
           and not (key.startswith('ODBCPP_MYSQL_') and key.endswith('_FIXTURE_ADMITTED'))}
    env.update(ODBCPP_MYSQL_TEST_USER='sdk', ODBCPP_MYSQL_TEST_PASSWORD=PASSWORD,
               ODBCPP_MYSQL_TEST_HOST='localhost', ODBCPP_MYSQL_TEST_PORT=port,
               ODBCPP_MYSQL_TEST_CA_FILE=str(ca.resolve()),
               ODBCPP_MYSQL_DATE_PARAMETER_FIXTURE_ADMITTED='pinned-8.4.11-temporary-date-parameters-valid')
    try:
        run([str(binary), '--gtest_filter=' + DATE_PARAMETERS_SUITE + '.' + DATE_PARAMETERS_CASE,
             '--gtest_repeat=1', '--gtest_output=xml:' + str(junit)], env=env, timeout=60)
        validate_date_parameters_xml(junit)
    except Exception:
        raise RuntimeError('MySQL DATE parameter proof failed') from None

def same_artifact(left, right):
    return left == right or (left.exists() and right.exists() and os.path.samefile(left, right))

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
    parser.add_argument('--datetime-test-binary')
    parser.add_argument('--datetime-junit')
    parser.add_argument('--date-parameters-test-binary')
    parser.add_argument('--date-parameters-junit')
    parser.add_argument('--datetime-receipt-test-binary')
    parser.add_argument('--datetime-receipt-junit')
    parser.add_argument('--decimal-receipt-test-binary')
    parser.add_argument('--decimal-receipt-junit')
    args = parser.parse_args()
    if bool(args.decimal_receipt_test_binary) != bool(args.decimal_receipt_junit):
        parser.error('DECIMAL receipt binary and JUnit must be supplied together')
    decimal_receipt_binary = Path(args.decimal_receipt_test_binary).resolve() if args.decimal_receipt_test_binary else None
    decimal_receipt_junit = Path(args.decimal_receipt_junit).resolve() if args.decimal_receipt_junit else None
    if bool(args.datetime_receipt_test_binary) != bool(args.datetime_receipt_junit):
        parser.error('DATETIME receipt binary and JUnit must be supplied together')
    receipt_binary = Path(args.datetime_receipt_test_binary).resolve() if args.datetime_receipt_test_binary else None
    receipt_junit = Path(args.datetime_receipt_junit).resolve() if args.datetime_receipt_junit else None
    if bool(args.decimal_test_binary) != bool(args.decimal_junit):
        parser.error('decimal binary and JUnit must be supplied together')
    if bool(args.datetime_test_binary) != bool(args.datetime_junit):
        parser.error('DATETIME binary and JUnit must be supplied together')
    if bool(args.date_parameters_test_binary) != bool(args.date_parameters_junit):
        parser.error('DATE parameter binary and JUnit must be supplied together')
    date_parameters_binary = Path(args.date_parameters_test_binary).resolve() if args.date_parameters_test_binary else None
    date_parameters_junit = Path(args.date_parameters_junit).resolve() if args.date_parameters_junit else None
    decimal_binary = Path(args.decimal_test_binary).resolve() if args.decimal_test_binary else None
    decimal_junit = Path(args.decimal_junit).resolve() if args.decimal_junit else None
    datetime_binary = Path(args.datetime_test_binary).resolve() if args.datetime_test_binary else None
    datetime_junit = Path(args.datetime_junit).resolve() if args.datetime_junit else None
    output = Path(args.output).resolve()
    if datetime_binary is not None or date_parameters_binary is not None or receipt_binary is not None or decimal_receipt_binary is not None:
        for binary, label in ((decimal_binary, 'decimal'), (datetime_binary, 'DATETIME'),
                              (date_parameters_binary, 'DATE parameter'), (receipt_binary, 'DATETIME receipt'), (decimal_receipt_binary, 'DECIMAL receipt')):
            if binary is not None and (not binary.is_file() or not os.access(binary, os.X_OK)):
                parser.error(label + ' binary must be executable')
        inputs = [Path(args.probe).resolve()] + [binary for binary in
            (decimal_binary, datetime_binary, date_parameters_binary, receipt_binary, decimal_receipt_binary) if binary is not None]
        outputs = [output] + [junit for junit in
            (decimal_junit, datetime_junit, date_parameters_junit, receipt_junit, decimal_receipt_junit) if junit is not None]
        if (any(same_artifact(out, source) for out in outputs for source in inputs)
                or any(same_artifact(left, right) for i, left in enumerate(outputs) for right in outputs[i+1:])):
            parser.error('result paths must be distinct from inputs and other results')
    if decimal_binary is not None:
        if not decimal_binary.is_file() or not os.access(decimal_binary, os.X_OK):
            parser.error('decimal binary must be executable')
        decimal_junit.parent.mkdir(parents=True, exist_ok=True)
        decimal_junit.unlink(missing_ok=True)
    if datetime_junit is not None:
        datetime_junit.parent.mkdir(parents=True, exist_ok=True)
        datetime_junit.unlink(missing_ok=True)
    if date_parameters_junit is not None:
        date_parameters_junit.parent.mkdir(parents=True, exist_ok=True)
        date_parameters_junit.unlink(missing_ok=True)
    if receipt_junit is not None:
        receipt_junit.parent.mkdir(parents=True, exist_ok=True)
        receipt_junit.unlink(missing_ok=True)
    if decimal_receipt_junit is not None:
        decimal_receipt_junit.parent.mkdir(parents=True, exist_ok=True)
        decimal_receipt_junit.unlink(missing_ok=True)
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
            if datetime_binary is not None:
                run_datetime(datetime_binary, datetime_junit, port, certs / 'ca.pem')
            if date_parameters_binary is not None:
                run_date_parameters(date_parameters_binary, date_parameters_junit, port, certs / 'ca.pem')
            receipt_observation = None
            if receipt_binary is not None:
                receipt_observation = run_datetime_receipt(receipt_binary, receipt_junit, port, certs / 'ca.pem')
            decimal_receipt_observation = None
            if decimal_receipt_binary is not None:
                decimal_receipt_observation = run_decimal_receipt(decimal_receipt_binary, decimal_receipt_junit, port, certs / 'ca.pem')
            output.parent.mkdir(parents=True, exist_ok=True)
            evidence = {'image': IMAGE, 'version': version, 'plugin': plugin,
                                          'cases': results, 'sdkDecimalResultsProven': decimal_binary is not None,
                                          'sdkDatetimeValidResultsProven': datetime_binary is not None,
                                          'sdkDateParametersProven': date_parameters_binary is not None,
                                          'sdkDirectSessionProven': True, 'odbcSessionClaimed': False}
            evidence['mysqlDatetimePrepareMetadataObserved'] = receipt_observation is not None
            if receipt_observation is not None:
                evidence['mysqlDatetimePrepareMetadataObservation'] = receipt_observation
            evidence['mysqlDecimalPrepareMetadataObserved'] = decimal_receipt_observation is not None
            if decimal_receipt_observation is not None:
                evidence['mysqlDecimalPrepareMetadataObservation'] = decimal_receipt_observation
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
