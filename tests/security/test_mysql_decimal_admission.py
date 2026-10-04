"""Offline fail-closed inventory and child admission checks; no database."""
import importlib.util
import json
import xml.etree.ElementTree as ET
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('mysql_live', Path(__file__).with_name('test_mysql_auth_live.py'))
live = importlib.util.module_from_spec(spec)
spec.loader.exec_module(live)

VALID = '<testsuites><testsuite name="MySqlDecimalResultsIntegrationTest" tests="1" failures="0" errors="0" disabled="0" skipped="0"><testcase name="DeclaredSignedBoundsNullAndOwnershipAgreeAcrossProtocols" classname="MySqlDecimalResultsIntegrationTest" status="run" result="completed"/></testsuite></testsuites>'

class DecimalAdmissionTest(unittest.TestCase):
    def test_inventory_rejects_missing_malformed_empty_duplicate_skipped_and_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'result.xml'
            with self.assertRaises(FileNotFoundError):
                live.validate_decimal_xml(path)
            for data in ('bad XML', '<testsuites/>', VALID.replace('tests="1"', 'tests="0"'),
                         VALID.replace('status="run"', 'status="notrun"'),
                         VALID.replace('result="completed"', 'result="skipped"'),
                         VALID.replace('/></testsuite>', '><failure/></testcase></testsuite>'),
                         VALID.replace('skipped="0"', 'skipped="1"'),
                         VALID.replace('</testsuites>', VALID + '</testsuites>')):
                path.write_text(data)
                with self.assertRaises(Exception):
                    live.validate_decimal_xml(path)
            path.write_text(VALID)
            live.validate_decimal_xml(path)

    def test_cleanup_requires_confirmed_absence_even_if_remove_fails(self):
        failed_remove = subprocess.CompletedProcess([], 1)
        for remaining in ('', 'owned-container-id'):
            with patch.object(live.subprocess, 'run', return_value=failed_remove):
                with patch.object(live, 'run', return_value=subprocess.CompletedProcess([], 0, stdout=remaining)) as observe:
                    if remaining:
                        with self.assertRaisesRegex(RuntimeError, 'cleanup failed'):
                            live.cleanup_fixture('owned-name')
                    else:
                        live.cleanup_fixture('owned-name')
                    self.assertEqual(observe.call_args[0][0][4], 'name=^/owned-name$')
        with patch.object(live.subprocess, 'run', return_value=failed_remove):
            with patch.object(live, 'run', side_effect=subprocess.CalledProcessError(1, 'docker')):
                with self.assertRaises(Exception):
                    live.cleanup_fixture('owned-name')

    def test_fixed_inventory_environment_timeout_and_no_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'result.xml'
            def child(args, **kwargs):
                self.assertEqual(kwargs['timeout'], 60)
                self.assertEqual(args[1], '--gtest_filter=' + live.DECIMAL_SUITE + '.' + live.DECIMAL_CASE)
                self.assertEqual(args[2], '--gtest_repeat=1')
                env = kwargs['env']
                self.assertFalse(any(k.startswith('GTEST_') for k in env))
                self.assertEqual(env['ODBCPP_MYSQL_TEST_HOST'], 'localhost')
                self.assertEqual(env['ODBCPP_MYSQL_TEST_PORT'], '12345')
                self.assertEqual(env['ODBCPP_MYSQL_DECIMAL_FIXTURE_ADMITTED'], 'pinned-8.4.11-temporary-decimal')
                path.write_text(VALID)
            with patch.dict(live.os.environ, {'GTEST_FILTER': '*', 'GTEST_REPEAT': '99', 'ODBCPP_MYSQL_TEST_HOST': 'wrong'}):
                with patch.object(live, 'run', side_effect=child) as run:
                    live.run_decimal(Path('/fake/binary'), path, '12345', Path('/fake/ca'))
                    self.assertEqual(run.call_count, 1)
            for error in (subprocess.TimeoutExpired('secret command', 60), subprocess.CalledProcessError(1, 'secret', stderr='private')):
                with patch.object(live, 'run', side_effect=error) as run:
                    with self.assertRaisesRegex(RuntimeError, '^MySQL decimal result proof failed$'):
                        live.run_decimal(Path('/fake/binary'), path, '12345', Path('/fake/ca'))
                    self.assertEqual(run.call_count, 1)


# Pending DATETIME runner contracts. Native operations below are mocked; this
# suite must run against the real reviewed API, not a permissive test stub.
DATETIME_SUITE = 'MySqlDatetimeResultsIntegrationTest'
DATETIME_CASE = 'CalendarPrecisionNullAndOwnershipAgreeAcrossProtocols'
DATETIME_VALID = ('<testsuites tests="1" failures="0" disabled="0" errors="0" name="AllTests">'
                  '<testsuite name="' + DATETIME_SUITE + '" tests="1" failures="0" disabled="0" skipped="0" errors="0">'
                  '<testcase name="' + DATETIME_CASE + '" classname="' + DATETIME_SUITE + '" status="run" result="completed"/>'
                  '</testsuite></testsuites>')

class DatetimeAdmissionTest(unittest.TestCase):
    def test_pair_and_path_refusal_preserves_inputs_before_dispatch(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory).resolve()
            inputs = [directory / name for name in ('probe', 'decimal', 'datetime')]
            for binary in inputs:
                binary.write_bytes(b'input artifact')
                binary.chmod(0o700)
            output, xml, decimal_xml = [directory / name for name in ('success.json', 'datetime.xml', 'decimal.xml')]
            output.write_bytes(b'prior evidence')
            base = ['mysql-live', '--probe', str(inputs[0]), '--output', str(output)]
            pairs = ['--datetime-test-binary', str(inputs[2]), '--datetime-junit', str(xml),
                     '--decimal-test-binary', str(inputs[1]), '--decimal-junit', str(decimal_xml)]
            bad_arguments = [base + pairs[:2], base + pairs[2:4]]
            for binary in inputs:
                for kind in ('same', 'symlink', 'hardlink'):
                    alias = directory / (binary.name + '-' + kind)
                    if kind == 'symlink':
                        alias.symlink_to(binary)
                    elif kind == 'hardlink':
                        live.os.link(binary, alias)
                    else:
                        alias = binary
                    for option in ('--datetime-junit', '--decimal-junit', '--output'):
                        argv = base + pairs
                        argv[argv.index(option) + 1] = str(alias)
                        bad_arguments.append(argv)
            for destination in (output, decimal_xml):
                argv = base + pairs
                argv[argv.index('--datetime-junit') + 1] = str(destination)
                bad_arguments.append(argv)
            for argv in bad_arguments:
                with self.subTest(argv=argv), patch.object(live.sys, 'argv', argv), \
                        patch.object(live, 'run') as dispatch:
                    with self.assertRaises(SystemExit) as failure:
                        live.main()
                    self.assertEqual(2, failure.exception.code)
                    dispatch.assert_not_called()
                    self.assertEqual(b'prior evidence', output.read_bytes())
                    for binary in inputs:
                        self.assertEqual(b'input artifact', binary.read_bytes())

    def test_real_emitter_inventory_requires_suite_skipped_but_not_root_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'datetime.xml'
            path.write_text(DATETIME_VALID)
            live.validate_datetime_xml(path)
            tree = ET.fromstring(DATETIME_VALID)
            tree.set('skipped', '0')
            path.write_text(ET.tostring(tree, encoding='unicode'))
            live.validate_datetime_xml(path)

    def test_inventory_rejects_missing_counts_names_status_errors_and_extra_cases(self):
        invalid = [('malformed', 'not XML'), ('empty', '<testsuites/>')]
        for scope in ('root', 'suite'):
            for key in ('tests', 'failures', 'errors', 'disabled') + (('skipped',) if scope == 'suite' else ()):
                for value in (None, '2' if key == 'tests' else '1'):
                    tree = ET.fromstring(DATETIME_VALID)
                    node = tree if scope == 'root' else tree[0]
                    if value is None:
                        del node.attrib[key]
                    else:
                        node.set(key, value)
                    invalid.append((scope + '-' + key + '-' + str(value), ET.tostring(tree, encoding='unicode')))
        for tag in ('failure', 'error', 'skipped'):
            tree = ET.fromstring(DATETIME_VALID)
            ET.SubElement(tree[0][0], tag)
            invalid.append((tag + '-child', ET.tostring(tree, encoding='unicode')))
        for scope, key, value in (('root', 'skipped', '1'), ('suite', 'name', 'WrongSuite'),
                                   ('case', 'name', 'WrongCase'), ('case', 'classname', 'WrongSuite'),
                                   ('case', 'status', 'notrun'), ('case', 'result', 'skipped')):
            for actual in (None, value):
                tree = ET.fromstring(DATETIME_VALID)
                node = tree if scope == 'root' else (tree[0] if scope == 'suite' else tree[0][0])
                if actual is None:
                    if scope == 'root':
                        continue  # Root skipped is intentionally optional.
                    del node.attrib[key]
                else:
                    node.set(key, actual)
                invalid.append((scope + '-' + key + '-' + str(actual), ET.tostring(tree, encoding='unicode')))
        tree = ET.fromstring(DATETIME_VALID)
        invalid.append(('bare-suite', ET.tostring(tree[0], encoding='unicode')))
        tree.append(ET.fromstring(ET.tostring(tree[0], encoding='unicode')))
        invalid.append(('extra-suite', ET.tostring(tree, encoding='unicode')))
        tree = ET.fromstring(DATETIME_VALID)
        tree[0].append(ET.fromstring(ET.tostring(tree[0][0], encoding='unicode')))
        invalid.append(('extra-case', ET.tostring(tree, encoding='unicode')))
        tree = ET.fromstring(DATETIME_VALID)
        ET.SubElement(tree[0][0], 'testcase', name=DATETIME_CASE)
        invalid.append(('nested-case', ET.tostring(tree, encoding='unicode')))
        # Listing XML has names/counts but no execution status or result.
        invalid.append(('listing', '<testsuites tests="1"><testsuite name="' + DATETIME_SUITE +
                        '" tests="1"><testcase name="' + DATETIME_CASE + '"/></testsuite></testsuites>'))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'datetime.xml'
            with self.assertRaises(FileNotFoundError):
                live.validate_datetime_xml(path)
            for label, data in invalid:
                with self.subTest(label=label):
                    path.write_text(data)
                    with self.assertRaises(Exception):
                        live.validate_datetime_xml(path)

    def test_child_overrides_private_endpoint_and_strips_all_filters_and_fixture_markers(self):
        inherited = {'GTEST_FILTER': '*', 'GTEST_REPEAT': '99', 'GTEST_SHARD_INDEX': '3',
                     'GTEST_TOTAL_SHARDS': '4', 'GTEST_OUTPUT': 'xml:/unowned/path', 'GTEST_RANDOM_SEED': '13',
                     'ODBCPP_MYSQL_TEST_HOST': 'wrong', 'ODBCPP_MYSQL_TEST_PORT': '9999',
                     'ODBCPP_MYSQL_TEST_CA_FILE': '/wrong/ca', 'ODBCPP_MYSQL_TEST_USER': 'wrong',
                     'ODBCPP_MYSQL_TEST_PASSWORD': 'untrusted',
                     'ODBCPP_MYSQL_DECIMAL_FIXTURE_ADMITTED': 'wrong',
                     'ODBCPP_MYSQL_DATE_FIXTURE_ADMITTED': 'wrong',
                     'ODBCPP_MYSQL_UNSIGNED_BIGINT_FIXTURE_ADMITTED': 'wrong',
                     'ODBCPP_MYSQL_DATETIME_FIXTURE_ADMITTED': 'wrong',
                     'ODBCPP_MYSQL_UNREVIEWED_FIXTURE_ADMITTED': 'wrong'}
        with tempfile.TemporaryDirectory() as directory:
            junit = Path(directory) / 'datetime.xml'
            binary = Path(directory) / 'child'
            ca = Path(directory) / 'ca.pem'
            def child(args, **kwargs):
                self.assertEqual(args, [str(binary), '--gtest_filter=' + DATETIME_SUITE + '.' + DATETIME_CASE,
                                        '--gtest_repeat=1', '--gtest_output=xml:' + str(junit)])
                self.assertEqual(kwargs['timeout'], 60)
                env = kwargs['env']
                self.assertFalse(any(key.startswith('GTEST_') for key in env))
                admitted = {key: value for key, value in env.items()
                            if key.startswith('ODBCPP_MYSQL_') and key.endswith('_FIXTURE_ADMITTED')}
                self.assertEqual(admitted, {'ODBCPP_MYSQL_DATETIME_FIXTURE_ADMITTED':
                                           'pinned-8.4.11-temporary-datetime-valid'})
                for key, expected in {'HOST': 'localhost', 'PORT': '12345', 'CA_FILE': str(ca.resolve()),
                                      'USER': 'sdk', 'PASSWORD': live.PASSWORD}.items():
                    self.assertEqual(env['ODBCPP_MYSQL_TEST_' + key], expected)
                junit.write_text(DATETIME_VALID)
                return subprocess.CompletedProcess(args, 0, stdout='', stderr='')
            with patch.dict(live.os.environ, inherited):
                with patch.object(live, 'run', side_effect=child) as invoke:
                    live.run_datetime(binary, junit, '12345', ca)
                    self.assertEqual(invoke.call_count, 1)

    def test_child_failures_have_safe_error_and_never_retry_or_accept_missing_inventory(self):
        with tempfile.TemporaryDirectory() as directory:
            junit = Path(directory) / 'datetime.xml'
            for failure in ('timeout', 'nonzero', 'missing', 'malformed', 'wrongcase'):
                with self.subTest(failure=failure):
                    junit.unlink(missing_ok=True)
                    def child(args, **kwargs):
                        if failure == 'timeout':
                            raise subprocess.TimeoutExpired('private command', 60, output='private output', stderr='private error')
                        if failure == 'nonzero':
                            raise subprocess.CalledProcessError(1, 'private command', output='private output', stderr='private error')
                        if failure == 'malformed':
                            junit.write_text('private malformed XML')
                        if failure == 'wrongcase':
                            junit.write_text(DATETIME_VALID.replace(DATETIME_CASE, 'WrongCase'))
                        return subprocess.CompletedProcess(args, 0, stdout='private output', stderr='private error')
                    with patch.object(live, 'run', side_effect=child) as invoke:
                        with self.assertRaisesRegex(RuntimeError, '^MySQL DATETIME result proof failed$') as caught:
                            live.run_datetime(Path('/fake/binary'), junit, '12345', Path('/fake/ca'))
                        self.assertTrue(caught.exception.__suppress_context__)
                        self.assertIsNone(caught.exception.__cause__)
                        self.assertEqual(invoke.call_count, 1)

    def _mock_main(self, *, failure=None, decimal=True, datetime=True, cleanup_failure=False):
        # Exercise real future main/dispatch/validators. Every process, SQL and
        # cleanup invocation is intercepted; filesystem outputs are disposable.
        events = []
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            output, dt_xml, decimal_xml = (directory / value for value in ('success.json', 'datetime.xml', 'decimal.xml'))
            output.write_text('{"stale":true}')
            dt_xml.write_text('stale XML')
            binaries = {}
            for label in ('probe', 'decimal', 'datetime'):
                binary = directory / label
                binary.write_text('mock-only executable')
                binary.chmod(0o700)
                binaries[label] = binary.resolve()
            argv = ['mysql-live', '--probe', str(binaries['probe']), '--output', str(output)]
            if decimal:
                argv += ['--decimal-test-binary', str(binaries['decimal']), '--decimal-junit', str(decimal_xml)]
            if datetime:
                argv += ['--datetime-test-binary', str(binaries['datetime']), '--datetime-junit', str(dt_xml)]
            owned_name = None
            def process(args, **kwargs):
                nonlocal owned_name
                if args[0] == 'openssl':
                    for option in ('-keyout', '-out'):
                        if option in args:
                            Path(args[args.index(option) + 1]).write_text('mock certificate')
                elif args[:2] == ['docker', 'pull']:
                    events.append('pull')
                elif args[:2] == ['docker', 'run']:
                    events.append('create')
                    self.assertFalse(output.exists())
                    if datetime:
                        self.assertFalse(dt_xml.exists())
                    owned_name = args[args.index('--name') + 1]
                    if failure == 'uncertain-create':
                        raise subprocess.TimeoutExpired('private create command', 60)
                elif args[:2] == ['docker', 'port']:
                    return subprocess.CompletedProcess(args, 0, stdout='127.0.0.1:12345\n')
                elif args[:2] == ['docker', 'exec']:
                    if args[-1].startswith('SELECT VERSION()'):
                        return subprocess.CompletedProcess(args, 0, stdout='8.4.11\t0\n')
                    if args[-1].startswith('SELECT plugin'):
                        return subprocess.CompletedProcess(args, 0, stdout='caching_sha2_password\n')
                    self.assertEqual(args[-1], 'FLUSH PRIVILEGES')
                elif args[0] == str(binaries['probe']):
                    events.append(('probe', args[-1], args[1], Path(args[3]).name))
                    return subprocess.CompletedProcess(args, 0, stdout='PASS ' + args[-1] + '\n')
                elif args[0] in (str(binaries['decimal']), str(binaries['datetime'])):
                    label = 'datetime' if args[0] == str(binaries['datetime']) else 'decimal'
                    events.append(label)
                    self.assertFalse(output.exists())
                    if label == 'datetime' and failure == 'child':
                        raise subprocess.CalledProcessError(1, 'private child', stderr='private')
                    data = DATETIME_VALID if label == 'datetime' else VALID
                    if label == 'datetime' and failure == 'xml':
                        data = '<testsuites/>'
                    (dt_xml if label == 'datetime' else decimal_xml).write_text(data)
                else:
                    self.fail('Unexpected mocked process kind')
                return subprocess.CompletedProcess(args, 0, stdout='', stderr='')
            def cleanup(name):
                self.assertEqual(name, owned_name)
                self.assertFalse(output.exists())
                events.append('cleanup')
                if cleanup_failure:
                    raise RuntimeError('mock cleanup failure')
                events.append('absence-confirmed')
            caught = None
            with patch.object(live.sys, 'argv', argv), patch.object(live, 'run', side_effect=process), \
                    patch.object(live, 'cleanup_fixture', side_effect=cleanup):
                try:
                    live.main()
                except Exception as error:
                    caught = error
            evidence = None
            if output.exists():
                events.append('success-written')
                evidence = json.loads(output.read_text())
            return events, evidence, caught

    def test_main_preserves_six_probes_decimal_then_datetime_and_cleanup_before_success(self):
        events, evidence, error = self._mock_main()
        self.assertIsNone(error)
        self.assertIsNotNone(evidence)
        expected_probes = [('probe', 'full', 'localhost', 'ca.pem'), ('probe', 'cached', 'localhost', 'ca.pem'),
                           ('probe', 'session', 'localhost', 'ca.pem'), ('probe', 'reject-auth', 'localhost', 'ca.pem'),
                           ('probe', 'reject-tls', 'localhost', 'wrong-ca.pem'), ('probe', 'reject-tls', '127.0.0.1', 'ca.pem')]
        self.assertEqual(events, ['pull', 'create'] + expected_probes + ['decimal', 'datetime', 'cleanup', 'absence-confirmed', 'success-written'])
        self.assertTrue(evidence['sdkDatetimeValidResultsProven'])
        self.assertTrue(evidence['sdkDecimalResultsProven'])
        self.assertTrue(evidence['sdkDirectSessionProven'])
        self.assertFalse(evidence['odbcSessionClaimed'])
        self.assertEqual([item['case'] for item in evidence['cases']], [item[1] for item in expected_probes])

    def test_main_optional_children_remain_independent_without_implicit_decimal_admission(self):
        for decimal, datetime in ((False, False), (False, True), (True, False)):
            with self.subTest(decimal=decimal, datetime=datetime):
                events, evidence, error = self._mock_main(decimal=decimal, datetime=datetime)
                self.assertIsNone(error)
                self.assertIsNotNone(evidence)
                self.assertEqual(decimal, evidence['sdkDecimalResultsProven'])
                self.assertEqual(datetime, evidence['sdkDatetimeValidResultsProven'])
                self.assertEqual(decimal, 'decimal' in events)
                self.assertEqual(datetime, 'datetime' in events)
                self.assertEqual(['cleanup', 'absence-confirmed', 'success-written'], events[-3:])

    def test_main_uncertain_create_child_xml_and_cleanup_failures_never_publish_success(self):
        for failure, cleanup_failure in (('uncertain-create', False), ('child', False), ('xml', False),
                                         ('child', True), (None, True)):
            with self.subTest(failure=failure, cleanup_failure=cleanup_failure):
                events, evidence, error = self._mock_main(failure=failure, cleanup_failure=cleanup_failure)
                self.assertIsNone(evidence)
                self.assertIsNotNone(error)
                self.assertIn('cleanup', events)
                self.assertNotIn('success-written', events)
                if failure == 'uncertain-create':
                    self.assertIsInstance(error, subprocess.TimeoutExpired)
                    self.assertNotIn('datetime', events)
                elif failure in ('child', 'xml'):
                    self.assertIsInstance(error, RuntimeError)
                    self.assertEqual('MySQL DATETIME result proof failed', str(error))
                else:
                    self.assertEqual('MySQL fixture cleanup failed', str(error))


# Future DATE-parameter contracts: run against the REAL reviewed runner APIs.
# They are intentionally pending while those APIs are absent; no stub pass or
# mocked XML below constitutes native qualification evidence.
DATE_PARAMETERS_SUITE = 'MySqlDateParametersIntegrationTest'
DATE_PARAMETERS_CASE = 'PhysicalDateComparisonBoundsNullAndOwningReceipts'
DATE_PARAMETERS_VALID = ('<testsuites tests="1" failures="0" disabled="0" errors="0" name="AllTests">'
                         '<testsuite name="' + DATE_PARAMETERS_SUITE + '" tests="1" failures="0" disabled="0" skipped="0" errors="0">'
                         '<testcase name="' + DATE_PARAMETERS_CASE + '" classname="' + DATE_PARAMETERS_SUITE + '" status="run" result="completed"/>'
                         '</testsuite></testsuites>')

class DateParametersAdmissionTest(unittest.TestCase):
    def test_pairs_executability_and_all_path_aliases_refuse_before_unlink_or_dispatch(self):
        for combined in (False, True):
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary).resolve()
                binaries = {key: directory / key for key in ('probe', 'decimal', 'datetime', 'date-parameters')}
                outputs = {key: directory / (key + '.xml') for key in ('decimal', 'datetime', 'date-parameters')}
                outputs['output'] = directory / 'success.json'
                for binary in binaries.values():
                    binary.write_bytes(b'private input artifact')
                    binary.chmod(0o700)
                for output in outputs.values():
                    output.write_bytes(b'prior evidence')
                base = ['mysql-live', '--probe', str(binaries['probe']), '--output', str(outputs['output'])]
                if combined:
                    for label in ('decimal', 'datetime'):
                        base += ['--' + label + '-test-binary', str(binaries[label]),
                                 '--' + label + '-junit', str(outputs[label])]
                pair = ['--date-parameters-test-binary', str(binaries['date-parameters']),
                        '--date-parameters-junit', str(outputs['date-parameters'])]
                argv = base + pair
                invalid = [base + pair[:2], base + pair[2:],
                           base + ['--date-parameters-test-binary', str(directory / 'missing'),
                                   '--date-parameters-junit', str(outputs['date-parameters'])]]
                nonexecutable = directory / 'nonexecutable'
                nonexecutable.write_bytes(b'nonexecutable input')
                nonexecutable.chmod(0o600)
                invalid.append(base + ['--date-parameters-test-binary', str(nonexecutable),
                                       '--date-parameters-junit', str(outputs['date-parameters'])])
                requested = ['date-parameters'] + (['decimal', 'datetime'] if combined else [])
                input_names = ['probe'] + requested
                output_options = ['--output'] + ['--' + label + '-junit' for label in requested]
                serial = 0
                for input_name in input_names:
                    for kind in ('same', 'resolved-relative', 'symlink', 'hardlink'):
                        serial += 1
                        original = binaries[input_name]
                        alias = directory / ('alias-' + str(serial))
                        if kind == 'same':
                            alias = original
                        elif kind == 'resolved-relative':
                            alias = directory / '..' / directory.name / input_name
                        elif kind == 'symlink':
                            alias.symlink_to(original)
                        else:
                            live.os.link(original, alias)
                        for option in output_options:
                            altered = argv.copy()
                            altered[altered.index(option) + 1] = str(alias)
                            invalid.append(altered)
                # Every requested output-output alias is checked, including
                # new XML vs JSON and existing XMLs in both directions.
                destinations = {'--output': outputs['output']}
                destinations.update({'--' + label + '-junit': outputs[label] for label in requested})
                for option, destination in destinations.items():
                    for other_option in destinations:
                        if option == other_option:
                            continue
                        for kind in ('same', 'symlink', 'hardlink'):
                            serial += 1
                            alias = directory / ('output-alias-' + str(serial))
                            if kind == 'same':
                                alias = destination
                            elif kind == 'symlink':
                                alias.symlink_to(destination)
                            else:
                                live.os.link(destination, alias)
                            altered = argv.copy()
                            altered[altered.index(other_option) + 1] = str(alias)
                            invalid.append(altered)
                for arguments in invalid:
                    with self.subTest(combined=combined, arguments=arguments), \
                            patch.object(live.sys, 'argv', arguments), patch.object(live, 'run') as dispatch:
                        with self.assertRaises(SystemExit) as failure:
                            live.main()
                        self.assertEqual(2, failure.exception.code)
                        dispatch.assert_not_called()
                        for binary in binaries.values():
                            self.assertEqual(b'private input artifact', binary.read_bytes())
                        for output in outputs.values():
                            self.assertEqual(b'prior evidence', output.read_bytes())
                        self.assertEqual(b'nonexecutable input', nonexecutable.read_bytes())

    def test_real_emitter_inventory_requires_suite_skipped_but_not_root_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'date-parameters.xml'
            path.write_text(DATE_PARAMETERS_VALID)
            live.validate_date_parameters_xml(path)
            tree = ET.fromstring(DATE_PARAMETERS_VALID)
            tree.set('skipped', '0')
            path.write_text(ET.tostring(tree, encoding='unicode'))
            live.validate_date_parameters_xml(path)

    def test_inventory_rejects_missing_counts_names_status_errors_and_extra_cases(self):
        invalid = [('malformed', 'not XML'), ('empty', '<testsuites/>')]
        for scope in ('root', 'suite'):
            for key in ('tests', 'failures', 'errors', 'disabled') + (('skipped',) if scope == 'suite' else ()):
                for value in (None, '2' if key == 'tests' else '1'):
                    tree = ET.fromstring(DATE_PARAMETERS_VALID)
                    node = tree if scope == 'root' else tree[0]
                    if value is None:
                        del node.attrib[key]
                    else:
                        node.set(key, value)
                    invalid.append((scope + '-' + key + '-' + str(value), ET.tostring(tree, encoding='unicode')))
        for tag in ('failure', 'error', 'skipped', 'properties', 'unreviewed'):
            tree = ET.fromstring(DATE_PARAMETERS_VALID)
            ET.SubElement(tree[0][0], tag)
            invalid.append((tag + '-child', ET.tostring(tree, encoding='unicode')))
        for scope, key, value in (('root', 'skipped', '1'), ('suite', 'name', 'WrongSuite'),
                                   ('case', 'name', 'WrongCase'), ('case', 'classname', 'WrongSuite'),
                                   ('case', 'status', 'notrun'), ('case', 'result', 'skipped')):
            for actual in (None, value):
                tree = ET.fromstring(DATE_PARAMETERS_VALID)
                node = tree if scope == 'root' else (tree[0] if scope == 'suite' else tree[0][0])
                if actual is None:
                    if scope == 'root':
                        continue  # Root skipped is intentionally optional.
                    del node.attrib[key]
                else:
                    node.set(key, actual)
                invalid.append((scope + '-' + key + '-' + str(actual), ET.tostring(tree, encoding='unicode')))
        tree = ET.fromstring(DATE_PARAMETERS_VALID)
        invalid.append(('bare-suite', ET.tostring(tree[0], encoding='unicode')))
        tree.append(ET.fromstring(ET.tostring(tree[0], encoding='unicode')))
        invalid.append(('extra-suite', ET.tostring(tree, encoding='unicode')))
        tree = ET.fromstring(DATE_PARAMETERS_VALID)
        tree[0].append(ET.fromstring(ET.tostring(tree[0][0], encoding='unicode')))
        invalid.append(('extra-case', ET.tostring(tree, encoding='unicode')))
        tree = ET.fromstring(DATE_PARAMETERS_VALID)
        ET.SubElement(tree[0][0], 'testcase', name=DATE_PARAMETERS_CASE)
        invalid.append(('nested-case', ET.tostring(tree, encoding='unicode')))
        # Listing XML has names/counts but no execution status or result.
        invalid.append(('listing', '<testsuites tests="1"><testsuite name="' + DATE_PARAMETERS_SUITE +
                        '" tests="1"><testcase name="' + DATE_PARAMETERS_CASE + '"/></testsuite></testsuites>'))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'date-parameters.xml'
            with self.assertRaises(FileNotFoundError):
                live.validate_date_parameters_xml(path)
            for label, data in invalid:
                with self.subTest(label=label):
                    path.write_text(data)
                    with self.assertRaises(Exception):
                        live.validate_date_parameters_xml(path)

    def test_child_overrides_private_endpoint_and_strips_all_filters_and_fixture_markers(self):
        inherited = {'GTEST_FILTER': '*', 'GTEST_REPEAT': '99', 'GTEST_SHARD_INDEX': '3',
                     'GTEST_TOTAL_SHARDS': '4', 'GTEST_OUTPUT': 'xml:/unowned/path', 'GTEST_RANDOM_SEED': '13', 'GTEST_UNREVIEWED_KEY': 'polluted',
                     'ODBCPP_MYSQL_TEST_HOST': 'wrong', 'ODBCPP_MYSQL_TEST_PORT': '9999',
                     'ODBCPP_MYSQL_TEST_CA_FILE': '/wrong/ca', 'ODBCPP_MYSQL_TEST_USER': 'wrong',
                     'ODBCPP_MYSQL_TEST_PASSWORD': 'untrusted',
                     'ODBCPP_MYSQL_DECIMAL_FIXTURE_ADMITTED': 'wrong',
                     'ODBCPP_MYSQL_DATE_FIXTURE_ADMITTED': 'wrong',
                     'ODBCPP_MYSQL_UNSIGNED_BIGINT_FIXTURE_ADMITTED': 'wrong',
                     'ODBCPP_MYSQL_DATETIME_FIXTURE_ADMITTED': 'wrong',
                     'ODBCPP_MYSQL_DATE_PARAMETER_FIXTURE_ADMITTED': 'wrong',
                     'ODBCPP_MYSQL_UNREVIEWED_FIXTURE_ADMITTED': 'wrong'}
        with tempfile.TemporaryDirectory() as directory:
            junit = Path(directory) / 'date-parameters.xml'
            binary = Path(directory) / 'child'
            ca = Path(directory) / 'ca.pem'
            def child(args, **kwargs):
                self.assertEqual(args, [str(binary), '--gtest_filter=' + DATE_PARAMETERS_SUITE + '.' + DATE_PARAMETERS_CASE,
                                        '--gtest_repeat=1', '--gtest_output=xml:' + str(junit)])
                self.assertEqual(kwargs['timeout'], 60)
                env = kwargs['env']
                self.assertFalse(any(key.startswith('GTEST_') for key in env))
                admitted = {key: value for key, value in env.items()
                            if key.startswith('ODBCPP_MYSQL_') and key.endswith('_FIXTURE_ADMITTED')}
                self.assertEqual(admitted, {'ODBCPP_MYSQL_DATE_PARAMETER_FIXTURE_ADMITTED':
                                           'pinned-8.4.11-temporary-date-parameters-valid'})
                for key, expected in {'HOST': 'localhost', 'PORT': '12345', 'CA_FILE': str(ca.resolve()),
                                      'USER': 'sdk', 'PASSWORD': live.PASSWORD}.items():
                    self.assertEqual(env['ODBCPP_MYSQL_TEST_' + key], expected)
                junit.write_text(DATE_PARAMETERS_VALID)
                return subprocess.CompletedProcess(args, 0, stdout='', stderr='')
            with patch.dict(live.os.environ, inherited):
                with patch.object(live, 'run', side_effect=child) as invoke:
                    live.run_date_parameters(binary, junit, '12345', ca)
                    self.assertEqual(invoke.call_count, 1)

    def test_child_failures_have_safe_error_and_never_retry_or_accept_missing_inventory(self):
        with tempfile.TemporaryDirectory() as directory:
            junit = Path(directory) / 'date-parameters.xml'
            for failure in ('timeout', 'nonzero', 'missing', 'malformed', 'wrongcase'):
                with self.subTest(failure=failure):
                    junit.unlink(missing_ok=True)
                    def child(args, **kwargs):
                        if failure == 'timeout':
                            raise subprocess.TimeoutExpired('private command', 60, output='private output', stderr='private error')
                        if failure == 'nonzero':
                            raise subprocess.CalledProcessError(1, 'private command', output='private output', stderr='private error')
                        if failure == 'malformed':
                            junit.write_text('private malformed XML')
                        if failure == 'wrongcase':
                            junit.write_text(DATE_PARAMETERS_VALID.replace(DATE_PARAMETERS_CASE, 'WrongCase'))
                        return subprocess.CompletedProcess(args, 0, stdout='private output', stderr='private error')
                    with patch.object(live, 'run', side_effect=child) as invoke:
                        with self.assertRaisesRegex(RuntimeError, '^MySQL DATE parameter proof failed$') as caught:
                            live.run_date_parameters(Path('/fake/binary'), junit, '12345', Path('/fake/ca'))
                        self.assertTrue(caught.exception.__suppress_context__)
                        self.assertIsNone(caught.exception.__cause__)
                        self.assertEqual(invoke.call_count, 1)


    def _mock_main(self, *, decimal=True, datetime=True, date_parameters=True,
                   failure=None, cleanup_failure=False):
        # Exercise real main/child helpers/validators. ALL processes and native
        # operations are intercepted. Mock XML is parser-test input only.
        events = []
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            output = directory / 'success.json'
            output.write_text('{"stale":true}')
            binaries = {}
            reports = {}
            for label in ('probe', 'decimal', 'datetime', 'date-parameters'):
                binary = directory / label
                binary.write_text('mock-only executable')
                binary.chmod(0o700)
                binaries[label] = binary.resolve()
                reports[label] = directory / (label + '.xml')
                reports[label].write_text('stale XML')
            enabled = {'decimal': decimal, 'datetime': datetime, 'date-parameters': date_parameters}
            argv = ['mysql-live', '--probe', str(binaries['probe']), '--output', str(output)]
            for label, requested in enabled.items():
                if requested:
                    argv += ['--' + label + '-test-binary', str(binaries[label]),
                             '--' + label + '-junit', str(reports[label])]
            owned_name = None
            def process(args, **kwargs):
                nonlocal owned_name
                if args[0] == 'openssl':
                    for option in ('-keyout', '-out'):
                        if option in args:
                            Path(args[args.index(option) + 1]).write_text('mock certificate')
                elif args[:2] == ['docker', 'pull']:
                    events.append('pull')
                    self.assertEqual(live.IMAGE, args[2])
                elif args[:2] == ['docker', 'run']:
                    events.append('create')
                    self.assertFalse(output.exists())
                    for label, requested in enabled.items():
                        if requested:
                            self.assertFalse(reports[label].exists())
                    self.assertIn(live.IMAGE, args)
                    owned_name = args[args.index('--name') + 1]
                    if failure == 'uncertain-create':
                        raise subprocess.TimeoutExpired('private create command', 60)
                elif args[:2] == ['docker', 'port']:
                    self.assertEqual(owned_name, args[2])
                    return subprocess.CompletedProcess(args, 0, stdout='127.0.0.1:12345\n')
                elif args[:2] == ['docker', 'exec']:
                    self.assertIn(owned_name, args)
                    if args[-1].startswith('SELECT VERSION()'):
                        return subprocess.CompletedProcess(args, 0, stdout='8.4.11\t0\n')
                    if args[-1].startswith('SELECT plugin'):
                        return subprocess.CompletedProcess(args, 0, stdout='caching_sha2_password\n')
                    self.assertEqual('FLUSH PRIVILEGES', args[-1])
                elif args[0] == str(binaries['probe']):
                    events.append(('probe', args[-1], args[1], Path(args[3]).name))
                    if failure == 'probe':
                        raise subprocess.CalledProcessError(1, 'private probe', stderr='private')
                    return subprocess.CompletedProcess(args, 0, stdout='PASS ' + args[-1] + '\n')
                else:
                    label = next((key for key in enabled if args[0] == str(binaries[key])), None)
                    if label is None:
                        self.fail('Unexpected mocked process kind')
                    self.assertTrue(enabled[label])
                    self.assertFalse(output.exists())
                    events.append(label)
                    if failure == label + '-child':
                        raise subprocess.CalledProcessError(1, 'private child', stderr='private')
                    if failure == label + '-timeout':
                        raise subprocess.TimeoutExpired('private child', 60, stderr='private')
                    data = {'decimal': VALID, 'datetime': DATETIME_VALID,
                            'date-parameters': DATE_PARAMETERS_VALID}[label]
                    if failure == label + '-xml':
                        data = '<testsuites/>'
                    reports[label].write_text(data)
                return subprocess.CompletedProcess(args, 0, stdout='', stderr='')
            def cleanup(name):
                self.assertEqual(owned_name, name)
                self.assertFalse(output.exists())
                events.append('cleanup')
                if cleanup_failure:
                    raise RuntimeError('private cleanup failure')
                events.append('absence-confirmed')
            caught = None
            with patch.object(live.sys, 'argv', argv), patch.object(live, 'run', side_effect=process), \
                    patch.object(live, 'cleanup_fixture', side_effect=cleanup):
                try:
                    live.main()
                except Exception as error:
                    caught = error
            evidence = None
            if output.exists():
                events.append('success-written')
                evidence = json.loads(output.read_text())
            return events, evidence, caught

    def test_main_preserves_six_probes_and_sequential_children_cleanup_before_json(self):
        events, evidence, error = self._mock_main()
        self.assertIsNone(error)
        expected_probes = [('probe', 'full', 'localhost', 'ca.pem'), ('probe', 'cached', 'localhost', 'ca.pem'),
                           ('probe', 'session', 'localhost', 'ca.pem'), ('probe', 'reject-auth', 'localhost', 'ca.pem'),
                           ('probe', 'reject-tls', 'localhost', 'wrong-ca.pem'), ('probe', 'reject-tls', '127.0.0.1', 'ca.pem')]
        self.assertEqual(['pull', 'create'] + expected_probes +
                         ['decimal', 'datetime', 'date-parameters', 'cleanup', 'absence-confirmed', 'success-written'], events)
        self.assertTrue(evidence['sdkDecimalResultsProven'])
        self.assertTrue(evidence['sdkDatetimeValidResultsProven'])
        self.assertTrue(evidence['sdkDateParametersProven'])
        self.assertTrue(evidence['sdkDirectSessionProven'])
        self.assertFalse(evidence['odbcSessionClaimed'])
        self.assertEqual(live.IMAGE, evidence['image'])
        self.assertEqual('8.4.11', evidence['version'])
        self.assertEqual('caching_sha2_password', evidence['plugin'])
        self.assertEqual([item[1] for item in expected_probes], [item['case'] for item in evidence['cases']])

    def test_optional_pairs_are_independent_date_only_and_all_omitted_keep_prior_proofs(self):
        for decimal in (False, True):
            for datetime in (False, True):
                for date_parameters in (False, True):
                    events, evidence, error = self._mock_main(decimal=decimal, datetime=datetime,
                                                              date_parameters=date_parameters)
                    self.assertIsNone(error)
                    self.assertEqual(decimal, evidence['sdkDecimalResultsProven'])
                    self.assertEqual(datetime, evidence['sdkDatetimeValidResultsProven'])
                    self.assertEqual(date_parameters, evidence['sdkDateParametersProven'])
                    self.assertEqual(decimal, 'decimal' in events)
                    self.assertEqual(datetime, 'datetime' in events)
                    self.assertEqual(date_parameters, 'date-parameters' in events)
                    self.assertEqual(['cleanup', 'absence-confirmed', 'success-written'], events[-3:])

    def test_main_failures_uncertain_create_and_cleanup_never_emit_success_preserve_original_error(self):
        expected_errors = {'uncertain-create': None, 'probe': 'MySQL probe rejected fixture',
                           'decimal-child': 'MySQL decimal result proof failed',
                           'datetime-child': 'MySQL DATETIME result proof failed',
                           'date-parameters-child': 'MySQL DATE parameter proof failed',
                           'date-parameters-timeout': 'MySQL DATE parameter proof failed',
                           'date-parameters-xml': 'MySQL DATE parameter proof failed',
                           None: 'MySQL fixture cleanup failed'}
        for failure, expected in expected_errors.items():
            for cleanup_failure in (False, True):
                if failure is None and not cleanup_failure:
                    continue
                with self.subTest(failure=failure, cleanup_failure=cleanup_failure):
                    events, evidence, error = self._mock_main(failure=failure, cleanup_failure=cleanup_failure)
                    self.assertIsNone(evidence)
                    self.assertIsNotNone(error)
                    self.assertIn('cleanup', events)
                    self.assertNotIn('success-written', events)
                    if failure == 'uncertain-create':
                        self.assertIsInstance(error, subprocess.TimeoutExpired)
                        self.assertNotIn('date-parameters', events)
                    else:
                        self.assertIsInstance(error, RuntimeError)
                        self.assertEqual(expected, str(error))
                    if failure in ('probe', 'decimal-child', 'datetime-child'):
                        self.assertNotIn('date-parameters', events)

    def test_real_cleanup_requires_confirmed_exact_owned_absence(self):
        # Test current real cleanup API separately from orchestration mocks.
        failed_remove = subprocess.CompletedProcess([], 1)
        for remaining in ('', 'surviving-owned-container'):
            with patch.object(live.subprocess, 'run', return_value=failed_remove) as remove, \
                    patch.object(live, 'run', return_value=subprocess.CompletedProcess([], 0, stdout=remaining)) as observe:
                if remaining:
                    with self.assertRaisesRegex(RuntimeError, 'cleanup failed'):
                        live.cleanup_fixture('owned-name')
                else:
                    live.cleanup_fixture('owned-name')
                self.assertEqual(['docker', 'rm', '--force', 'owned-name'], remove.call_args[0][0])
                self.assertEqual(30, remove.call_args.kwargs['timeout'])
                self.assertEqual(['docker', 'ps', '--all', '--filter', 'name=^/owned-name$', '--format', '{{.ID}}'],
                                 observe.call_args[0][0])
                self.assertEqual(10, observe.call_args.kwargs['timeout'])
        with patch.object(live.subprocess, 'run', return_value=failed_remove), \
                patch.object(live, 'run', side_effect=subprocess.CalledProcessError(1, 'private observation')):
            with self.assertRaises(Exception):
                live.cleanup_fixture('owned-name')


# Future runner specifications: synthetic process XML, never native evidence.
from mysql_datetime_receipt_xml import validate_datetime_receipt_xml

RECEIPT_SUITE = 'MySqlDatetimeReceiptObservationIntegrationTest'
RECEIPT_CASE = 'ActualCastParameterMetadataRefusalIsObserved'

def synthetic_receipt_xml():
    root = ET.Element('testsuites', tests='1', failures='0', errors='0', disabled='0', name='AllTests', time='0.')
    suite = ET.SubElement(root, 'testsuite', name=RECEIPT_SUITE, tests='1', failures='0', errors='0', disabled='0', skipped='0')
    case = ET.SubElement(suite, 'testcase', name=RECEIPT_CASE, classname=RECEIPT_SUITE, status='run', result='completed', file='synthetic.cpp', line='1')
    properties = ET.SubElement(case, 'properties')
    values = {'observation_schema': 1, 'prepare_parameter_count': 1, 'prepare_result_count': 2,
              'observed_parameter_count': 1, 'records_truncated': 0, 'exchange_complete': 0,
              'transport_complete_calls': 1, 'prepare_command_count': 1, 'execute_command_count': 0,
              'close_command_count': 0, 'raw_parameter_0_native_type': 12,
              'raw_parameter_0_charset': 65535, 'raw_parameter_0_byte_width': 4294967295,
              'raw_parameter_0_decimals': 255, 'runtime_retired': 1, 'physical_close_delta': 1,
              'refusal_policy_code': 2, 'parameter_support_claimed': 0}
    for name, value in values.items():
        ET.SubElement(properties, 'property', name=name, value=str(value))
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding='unicode')

RECEIPT_VALID = synthetic_receipt_xml()

class DatetimeReceiptAdmissionTest(unittest.TestCase):
    def setUp(self):
        # Missing APIs MUST fail specs, not make alias tests pass as unknown flags.
        self.assertTrue(callable(getattr(live, 'run_datetime_receipt', None)),
                        'Pending real runner API: run_datetime_receipt')

    def test_pairs_executability_and_all_path_aliases_refuse_before_unlink_or_dispatch(self):
        for combined in (False, True):
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary).resolve()
                binaries = {key: directory / key for key in ('probe', 'decimal', 'datetime', 'date-parameters', 'datetime-receipt')}
                outputs = {key: directory / (key + '.xml') for key in ('decimal', 'datetime', 'date-parameters', 'datetime-receipt')}
                outputs['output'] = directory / 'success.json'
                for binary in binaries.values():
                    binary.write_bytes(b'private input artifact')
                    binary.chmod(0o700)
                for output in outputs.values():
                    output.write_bytes(b'prior evidence')
                base = ['mysql-live', '--probe', str(binaries['probe']), '--output', str(outputs['output'])]
                if combined:
                    for label in ('decimal', 'datetime', 'date-parameters'):
                        base += ['--' + label + '-test-binary', str(binaries[label]),
                                 '--' + label + '-junit', str(outputs[label])]
                pair = ['--datetime-receipt-test-binary', str(binaries['datetime-receipt']),
                        '--datetime-receipt-junit', str(outputs['datetime-receipt'])]
                argv = base + pair
                invalid = [base + pair[:2], base + pair[2:],
                           base + ['--datetime-receipt-test-binary', str(directory / 'missing'),
                                   '--datetime-receipt-junit', str(outputs['datetime-receipt'])]]
                nonexecutable = directory / 'nonexecutable'
                nonexecutable.write_bytes(b'nonexecutable input')
                nonexecutable.chmod(0o600)
                invalid.append(base + ['--datetime-receipt-test-binary', str(nonexecutable),
                                       '--datetime-receipt-junit', str(outputs['datetime-receipt'])])
                requested = ['datetime-receipt'] + (['decimal', 'datetime', 'date-parameters'] if combined else [])
                input_names = ['probe'] + requested
                output_options = ['--output'] + ['--' + label + '-junit' for label in requested]
                serial = 0
                for input_name in input_names:
                    for kind in ('same', 'resolved-relative', 'symlink', 'hardlink'):
                        serial += 1
                        original = binaries[input_name]
                        alias = directory / ('alias-' + str(serial))
                        if kind == 'same':
                            alias = original
                        elif kind == 'resolved-relative':
                            alias = directory / '..' / directory.name / input_name
                        elif kind == 'symlink':
                            alias.symlink_to(original)
                        else:
                            live.os.link(original, alias)
                        for option in output_options:
                            altered = argv.copy()
                            altered[altered.index(option) + 1] = str(alias)
                            invalid.append(altered)
                # Every requested output-output alias is checked, including
                # new XML vs JSON and existing XMLs in both directions.
                destinations = {'--output': outputs['output']}
                destinations.update({'--' + label + '-junit': outputs[label] for label in requested})
                for option, destination in destinations.items():
                    for other_option in destinations:
                        if option == other_option:
                            continue
                        for kind in ('same', 'symlink', 'hardlink'):
                            serial += 1
                            alias = directory / ('output-alias-' + str(serial))
                            if kind == 'same':
                                alias = destination
                            elif kind == 'symlink':
                                alias.symlink_to(destination)
                            else:
                                live.os.link(destination, alias)
                            altered = argv.copy()
                            altered[altered.index(other_option) + 1] = str(alias)
                            invalid.append(altered)
                for arguments in invalid:
                    with self.subTest(combined=combined, arguments=arguments), \
                            patch.object(live.sys, 'argv', arguments), patch.object(live, 'run') as dispatch, \
                            patch.object(Path, 'unlink') as unlink:
                        with self.assertRaises(SystemExit) as failure:
                            live.main()
                        self.assertEqual(2, failure.exception.code)
                        dispatch.assert_not_called()
                        unlink.assert_not_called()
                        for binary in binaries.values():
                            self.assertEqual(b'private input artifact', binary.read_bytes())
                        for output in outputs.values():
                            self.assertEqual(b'prior evidence', output.read_bytes())
                        self.assertEqual(b'nonexecutable input', nonexecutable.read_bytes())

    def test_actual_validator_return_private_environment_exact_one_observation_child(self):
        inherited = {'GTEST_FILTER': '*', 'GTEST_REPEAT': '99', 'GTEST_SHARD_INDEX': '3',
                     'GTEST_TOTAL_SHARDS': '4', 'GTEST_OUTPUT': 'xml:/unowned',
                     'GTEST_RANDOM_SEED': '13', 'GTEST_UNKNOWN': 'polluted',
                     'ODBCPP_MYSQL_TEST_HOST': 'wrong', 'ODBCPP_MYSQL_TEST_PORT': '9999',
                     'ODBCPP_MYSQL_TEST_CA_FILE': '/wrong', 'ODBCPP_MYSQL_TEST_USER': 'wrong',
                     'ODBCPP_MYSQL_TEST_PASSWORD': 'wrong'}
        for label in ('DECIMAL', 'DATE', 'DATETIME', 'DATE_PARAMETER', 'UNSIGNED_BIGINT',
                      'DATETIME_RECEIPT', 'FUTURE_UNKNOWN'):
            inherited['ODBCPP_MYSQL_' + label + '_FIXTURE_ADMITTED'] = 'untrusted'
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            binary, junit, ca = (directory / name for name in ('child', 'receipt.xml', 'ca.pem'))
            def process(args, **kwargs):
                self.assertEqual(args, [str(binary), '--gtest_filter=' + RECEIPT_SUITE + '.' + RECEIPT_CASE,
                                        '--gtest_repeat=1', '--gtest_output=xml:' + str(junit)])
                self.assertEqual(kwargs['timeout'], 60)
                env = kwargs['env']
                self.assertFalse(any(key.startswith('GTEST_') for key in env))
                markers = {k: v for k, v in env.items() if k.startswith('ODBCPP_MYSQL_') and k.endswith('_FIXTURE_ADMITTED')}
                self.assertEqual({'ODBCPP_MYSQL_DATETIME_RECEIPT_FIXTURE_ADMITTED':
                                  'pinned-8.4.11-temporary-datetime-receipt-observation'}, markers)
                for key, value in {'HOST': 'localhost', 'PORT': '12345', 'CA_FILE': str(ca.resolve()),
                                   'USER': 'sdk', 'PASSWORD': live.PASSWORD}.items():
                    self.assertEqual(value, env['ODBCPP_MYSQL_TEST_' + key])
                junit.write_text(RECEIPT_VALID, encoding='utf-8')
                return subprocess.CompletedProcess(args, 0, stdout='NOT AUTHORITY', stderr='private')
            with patch.dict(live.os.environ, inherited), patch.object(live, 'run', side_effect=process) as invoke:
                observation = live.run_datetime_receipt(binary, junit, '12345', ca)
            self.assertEqual(1, invoke.call_count)
            self.assertEqual(validate_datetime_receipt_xml(junit), observation)
            self.assertFalse(observation['parameterSupportClaimed'])
            self.assertFalse(observation['exchangeComplete'])
            self.assertEqual(0, observation['properties']['execute_command_count'])
            junit.unlink()
            self.assertEqual(4294967295, observation['rawParameter']['byteWidth'])

    def test_child_timeout_failure_or_invalid_xml_never_retries_or_trusts_stdout(self):
        with tempfile.TemporaryDirectory() as temporary:
            junit = Path(temporary) / 'receipt.xml'
            for failure in ('timeout', 'nonzero', 'missing', 'malformed', 'wrongcase',
                            'bad-policy', 'support-claimed', 'execute-claimed', 'oversized'):
                with self.subTest(failure=failure):
                    junit.unlink(missing_ok=True)
                    def process(args, **kwargs):
                        if failure == 'timeout':
                            raise subprocess.TimeoutExpired('private', 60, stderr='secret')
                        if failure == 'nonzero':
                            raise subprocess.CalledProcessError(1, 'private', stderr='secret')
                        data = RECEIPT_VALID
                        if failure == 'malformed':
                            data = 'private malformed XML'
                        elif failure == 'wrongcase':
                            data = data.replace(RECEIPT_CASE, 'WrongCase')
                        elif failure in ('bad-policy', 'support-claimed', 'execute-claimed'):
                            root = ET.fromstring(data)
                            key = {'bad-policy': 'refusal_policy_code', 'support-claimed': 'parameter_support_claimed',
                                   'execute-claimed': 'execute_command_count'}[failure]
                            next(n for n in root[0][0][0] if n.get('name') == key).set('value', '1')
                            data = ET.tostring(root, encoding='unicode')
                        elif failure == 'oversized':
                            data += ' ' * 16385
                        if failure != 'missing':
                            junit.write_text(data, encoding='utf-8')
                        return subprocess.CompletedProcess(args, 0, stdout='PASS receipt native_type=12 width=104', stderr='private')
                    with patch.object(live, 'run', side_effect=process) as invoke:
                        with self.assertRaisesRegex(RuntimeError, '^MySQL DATETIME receipt observation failed$') as caught:
                            live.run_datetime_receipt(Path('/fake/child'), junit, '12345', Path('/fake/ca'))
                        self.assertTrue(caught.exception.__suppress_context__)
                        self.assertIsNone(caught.exception.__cause__)
                        self.assertEqual(1, invoke.call_count)

    def _mock_main(self, *, decimal=True, datetime=True, date_parameters=True,
                   receipt=True, failure=None, cleanup_failure=False):
        # Exercise real main/child helpers/validators. ALL processes and native
        # operations are intercepted. Mock XML is parser-test input only.
        events = []
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            output = directory / 'success.json'
            output.write_text('{"stale":true}')
            binaries = {}
            reports = {}
            for label in ('probe', 'decimal', 'datetime', 'date-parameters', 'datetime-receipt'):
                binary = directory / label
                binary.write_text('mock-only executable')
                binary.chmod(0o700)
                binaries[label] = binary.resolve()
                reports[label] = (directory / (label + '.xml')).resolve()
                reports[label].write_text('stale XML')
            enabled = {'decimal': decimal, 'datetime': datetime, 'date-parameters': date_parameters, 'datetime-receipt': receipt}
            argv = ['mysql-live', '--probe', str(binaries['probe']), '--output', str(output)]
            for label, requested in enabled.items():
                if requested:
                    argv += ['--' + label + '-test-binary', str(binaries[label]),
                             '--' + label + '-junit', str(reports[label])]
            owned_name = None
            def process(args, **kwargs):
                nonlocal owned_name
                if args[0] == 'openssl':
                    for option in ('-keyout', '-out'):
                        if option in args:
                            Path(args[args.index(option) + 1]).write_text('mock certificate')
                elif args[:2] == ['docker', 'pull']:
                    events.append('pull')
                    self.assertEqual(live.IMAGE, args[2])
                elif args[:2] == ['docker', 'run']:
                    events.append('create')
                    self.assertFalse(output.exists())
                    for label, requested in enabled.items():
                        if requested:
                            self.assertFalse(reports[label].exists())
                    self.assertIn(live.IMAGE, args)
                    owned_name = args[args.index('--name') + 1]
                    if failure == 'uncertain-create':
                        raise subprocess.TimeoutExpired('private create command', 60)
                elif args[:2] == ['docker', 'port']:
                    self.assertEqual(owned_name, args[2])
                    return subprocess.CompletedProcess(args, 0, stdout='127.0.0.1:12345\n')
                elif args[:2] == ['docker', 'exec']:
                    self.assertIn(owned_name, args)
                    if args[-1].startswith('SELECT VERSION()'):
                        return subprocess.CompletedProcess(args, 0, stdout='8.4.11\t0\n')
                    if args[-1].startswith('SELECT plugin'):
                        return subprocess.CompletedProcess(args, 0, stdout='caching_sha2_password\n')
                    self.assertEqual('FLUSH PRIVILEGES', args[-1])
                elif args[0] == str(binaries['probe']):
                    events.append(('probe', args[-1], args[1], Path(args[3]).name))
                    if failure == 'probe':
                        raise subprocess.CalledProcessError(1, 'private probe', stderr='private')
                    return subprocess.CompletedProcess(args, 0, stdout='PASS ' + args[-1] + '\n')
                else:
                    label = next((key for key in enabled if args[0] == str(binaries[key])), None)
                    if label is None:
                        self.fail('Unexpected mocked process kind')
                    self.assertTrue(enabled[label])
                    self.assertFalse(output.exists())
                    events.append(label)
                    if label == 'datetime-receipt':
                        self.assertEqual(args[1:], ['--gtest_filter=' + RECEIPT_SUITE + '.' + RECEIPT_CASE,
                                                   '--gtest_repeat=1', '--gtest_output=xml:' + str(reports[label])])
                        self.assertEqual(kwargs['timeout'], 60)
                    if failure == label + '-child':
                        raise subprocess.CalledProcessError(1, 'private child', stderr='private')
                    if failure == label + '-timeout':
                        raise subprocess.TimeoutExpired('private child', 60, stderr='private')
                    data = {'decimal': VALID, 'datetime': DATETIME_VALID,
                            'date-parameters': DATE_PARAMETERS_VALID, 'datetime-receipt': RECEIPT_VALID}[label]
                    if failure == label + '-xml':
                        data = '<testsuites/>'
                    reports[label].write_text(data)
                return subprocess.CompletedProcess(args, 0, stdout='', stderr='')
            def cleanup(name):
                self.assertEqual(owned_name, name)
                self.assertFalse(output.exists())
                events.append('cleanup')
                if cleanup_failure:
                    raise RuntimeError('private cleanup failure')
                events.append('absence-confirmed')
            caught = None
            with patch.object(live.sys, 'argv', argv), patch.object(live, 'run', side_effect=process), \
                    patch.object(live, 'cleanup_fixture', side_effect=cleanup):
                try:
                    live.main()
                except Exception as error:
                    caught = error
            evidence = None
            if output.exists():
                events.append('success-written')
                evidence = json.loads(output.read_text())
            return events, evidence, caught

    def test_main_keeps_existing_proofs_then_observation_after_cleanup_absence(self):
        for combined, receipt in ((True, True), (False, True), (True, False), (False, False)):
            with self.subTest(combined=combined, receipt=receipt):
                events, evidence, error = self._mock_main(decimal=combined, datetime=combined,
                                                          date_parameters=combined, receipt=receipt)
                self.assertIsNone(error)
                expected_probes = [('probe', 'full', 'localhost', 'ca.pem'), ('probe', 'cached', 'localhost', 'ca.pem'),
                                   ('probe', 'session', 'localhost', 'ca.pem'), ('probe', 'reject-auth', 'localhost', 'ca.pem'),
                                   ('probe', 'reject-tls', 'localhost', 'wrong-ca.pem'), ('probe', 'reject-tls', '127.0.0.1', 'ca.pem')]
                children = (['decimal', 'datetime', 'date-parameters'] if combined else []) + (['datetime-receipt'] if receipt else [])
                self.assertEqual(['pull', 'create'] + expected_probes + children +
                                 ['cleanup', 'absence-confirmed', 'success-written'], events)
                self.assertEqual(combined, evidence['sdkDecimalResultsProven'])
                self.assertEqual(combined, evidence['sdkDatetimeValidResultsProven'])
                self.assertEqual(combined, evidence['sdkDateParametersProven'])
                self.assertTrue(evidence['sdkDirectSessionProven'])
                self.assertFalse(evidence['odbcSessionClaimed'])
                self.assertNotIn('sdkDatetimeParametersProven', evidence)
                self.assertEqual(receipt, evidence['mysqlDatetimePrepareMetadataObserved'])
                if receipt:
                    observed = evidence['mysqlDatetimePrepareMetadataObservation']
                    self.assertFalse(observed['parameterSupportClaimed'])
                    self.assertFalse(observed['exchangeComplete'])
                    self.assertEqual({'nativeType': 12, 'charset': 65535, 'byteWidth': 4294967295, 'decimals': 255}, observed['rawParameter'])
                    import hashlib
                    self.assertEqual(hashlib.sha256(RECEIPT_VALID.encode('utf-8')).hexdigest(), observed['xmlSha256'])
                    self.assertEqual(0, observed['properties']['execute_command_count'])
                else:
                    self.assertNotIn('mysqlDatetimePrepareMetadataObservation', evidence)

    def test_original_failures_and_uncertain_create_never_publish_observation_json(self):
        expected = {'uncertain-create': None, 'probe': 'MySQL probe rejected fixture',
                    'decimal-child': 'MySQL decimal result proof failed',
                    'datetime-child': 'MySQL DATETIME result proof failed',
                    'date-parameters-child': 'MySQL DATE parameter proof failed',
                    'datetime-receipt-child': 'MySQL DATETIME receipt observation failed',
                    'datetime-receipt-timeout': 'MySQL DATETIME receipt observation failed',
                    'datetime-receipt-xml': 'MySQL DATETIME receipt observation failed',
                    None: 'MySQL fixture cleanup failed'}
        for failure, message in expected.items():
            for cleanup_failure in (False, True):
                if failure is None and not cleanup_failure:
                    continue
                with self.subTest(failure=failure, cleanup_failure=cleanup_failure):
                    events, evidence, error = self._mock_main(failure=failure, cleanup_failure=cleanup_failure)
                    self.assertIsNone(evidence)
                    self.assertIsNotNone(error)
                    self.assertIn('cleanup', events)
                    self.assertNotIn('success-written', events)
                    if failure == 'uncertain-create':
                        self.assertIsInstance(error, subprocess.TimeoutExpired)
                    else:
                        self.assertEqual(message, str(error))
                    if failure in ('uncertain-create', 'probe', 'decimal-child', 'datetime-child', 'date-parameters-child'):
                        self.assertNotIn('datetime-receipt', events)


class ReceiptSiblingLoadingTest(unittest.TestCase):
    def test_unrelated_cwd_and_missing_or_malformed_dependency_fail_closed(self):
        import shutil
        import sys
        code = """import importlib.util,sys
spec=importlib.util.spec_from_file_location('candidate_runner',sys.argv[1])
module=importlib.util.module_from_spec(spec)
try:
 spec.loader.exec_module(module)
 print('PASS' if callable(module.run_datetime_receipt) else 'INVALID')
except Exception as error:
 print(str(error))
"""
        with tempfile.TemporaryDirectory() as temporary:
            base=Path(temporary).resolve()
            owned=base/'owned'; owned.mkdir()
            other=base/'unrelated'; other.mkdir()
            runner=owned/'test_mysql_auth_live.py'
            shutil.copy2(Path(live.__file__).resolve(),runner)
            dependency=owned/'mysql_datetime_receipt_xml.py'
            for state in ('valid','missing','malformed'):
                with self.subTest(state=state):
                    if state=='valid':
                        shutil.copy2(Path(live.__file__).with_name(dependency.name),dependency)
                    elif state=='missing':
                        dependency.unlink()
                    else:
                        dependency.write_text('broken syntax @ private sentinel')
                    result=subprocess.run([sys.executable,'-B','-c',code,str(runner)],
                                          cwd=other,capture_output=True,text=True,timeout=10)
                    self.assertEqual(result.returncode,0)
                    self.assertEqual(result.stderr,'')
                    self.assertEqual(result.stdout.strip(),'PASS' if state=='valid' else
                                     'MySQL DATETIME receipt validator unavailable')

if __name__ == '__main__':
    unittest.main()
