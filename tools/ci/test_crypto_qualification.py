"""Consistency failures must not become usable qualification summaries."""
import importlib.util
import json
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

spec = importlib.util.spec_from_file_location('qualification', Path(__file__).with_name('record-crypto-qualification.py'))
q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(q)


class QualificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.driver, self.probe, self.report = (self.root / name for name in ('driver', 'probe', 'unit.xml'))
        self.driver.write_bytes(b'driver')
        self.probe.write_bytes(b'probe')
        self.manifest = {'provider': 'OPENSSL', 'compileVersion': '3.6.4',
                         'requestedLinkage': 'SYSTEM_SHARED', 'fipsClaimed': False}
        self.write('manifest', self.manifest)
        self.identity = {
            'provider': 'OPENSSL', 'requestedLinkage': 'SYSTEM_SHARED', 'qualificationClaimed': False,
            'compileVersion': 'OpenSSL 3.6.4 date', 'runtimeVersion': 'OpenSSL 3.6.4 date',
            'activeFips': False, 'minimumTlsVersion': '1.2', 'verifyPeer': True, 'verifyHostname': True,
            'artifactSha256': q.sha(self.driver), 'probeSha256': q.sha(self.probe),
            'manifestSha256': q.sha(self.root / 'odbcpp-crypto-manifest.json'),
        }
        self.artifact = {
            'provider': 'OPENSSL', 'requestedLinkage': 'SYSTEM_SHARED', 'qualificationClaimed': False,
            'artifactSha256': q.sha(self.driver), 'platform': 'Linux',
            'dependencyFormVerified': True, 'observedDependencyForm': 'shared',
        }
        self.xml = ET.Element('testsuite')
        for name in sorted(q.REQUIRED_TESTS):
            case = ET.SubElement(self.xml, 'testcase', name=name, status='run')
            if name == 'verified_crypto_tls_interop':
                ET.SubElement(case, 'system-out').text = '\n'.join(sorted(q.TLS_MARKERS))
        self.reload_case = ET.SubElement(self.xml, 'testcase', name=q.RELOAD_TEST, status='run')
        ET.SubElement(self.reload_case, 'system-out').text = self.reload_marker()
        self.reload_xml = ET.Element('testsuites', tests='2', failures='0', errors='0', disabled='0',
                                     odbcpp_trace_reload_run='123-456')
        self.reload_suite = ET.SubElement(self.reload_xml, 'testsuite', name='OdbcTraceReload',
                                          tests='2', failures='0', errors='0', disabled='0', skipped='0')
        for full_name in sorted(q.RELOAD_CASES):
            classname, name = full_name.split('.')
            ET.SubElement(self.reload_suite, 'testcase', classname=classname, name=name,
                          status='run', result='completed')
        host = ET.SubElement(self.xml, 'testcase', name='test_shared_crypto_cohabitation', status='run')
        ET.SubElement(host, 'system-out').text = '\n'.join(sorted(q.COHABITATION_MARKERS))

    def write(self, kind, data):
        (self.root / f'odbcpp-crypto-{kind}.json').write_text(json.dumps(data))

    @staticmethod
    def reload_marker(skipped=0):
        return (f'{q.RELOAD_PREFIX}run=123-456 selected=2 passed={2-skipped} skipped={skipped} '
                f'failed=0 disabled=0 result=0 exit={77 if skipped else 0}')

    def darwin_boundary(self):
        self.artifact['platform'] = 'Darwin'
        self.xml.set('skipped', '1')
        self.reload_case.set('status', 'notrun')
        ET.SubElement(self.reload_case, 'skipped', message='SKIP_RETURN_CODE')
        self.reload_case.find('system-out').text = self.reload_marker(2)
        self.reload_suite.set('skipped', '2')
        for case in self.reload_suite:
            case.set('result', 'skipped')
            full_name = case.get('classname') + '.' + case.get('name')
            reason = '/fixture/tests/unit/test_odbc_trace_reload.cpp:203\n' + q.RELOAD_CASES[full_name]
            if 'DisabledTracing' in full_name:
                reason += '\nGoogle Test trace:\n/fixture/tests/unit/test_odbc_trace_reload.cpp:191: 0'
            ET.SubElement(case, 'skipped', message=reason).text = reason
            properties = ET.SubElement(case, 'properties')
            ET.SubElement(properties, 'property', name='odbcpp_trace_reload_boundary',
                          value='driver_image_retained')

    def collect(self, live_report=None, platform='Linux', reload_report=None, write_reload=True):
        self.write('identity-evidence', self.identity)
        self.write('artifact-evidence', self.artifact)
        ET.ElementTree(self.xml).write(self.report)
        if write_reload:
            ET.ElementTree(self.reload_xml).write(self.root / q.RELOAD_REPORT)
        return q.collect(self.root, self.driver, self.probe, self.report, platform, live_report, reload_report)

    def test_valid_record_has_bound_inputs_and_no_qualification_claim(self):
        result = self.collect()
        self.assertFalse(result['qualificationClaimed'])
        self.assertFalse(result['liveDriverPackageCoexistenceAcceptanceEvaluated'])
        self.assertEqual(result['inputs']['driver'], q.sha(self.driver))
        self.assertEqual(result['inputs']['unitReport'], q.sha(self.report))
        self.assertEqual(result['inputs']['nativeReloadReport'], q.sha(self.root / q.RELOAD_REPORT))
        self.assertTrue(result['traceReload']['unloadClosureObserved'])
        self.assertEqual(result['traceReload']['passed'], 2)
        self.assertEqual(result['nonMandatoryLoaderBoundaries'], [])

    def test_exact_darwin_loader_boundary_is_not_an_unload_pass(self):
        self.darwin_boundary()
        result = self.collect(platform='Darwin')
        boundary, = result['nonMandatoryLoaderBoundaries']
        self.assertEqual(boundary['status'], 'UNAVAILABLE_LOADER_BOUNDARY')
        self.assertFalse(boundary['unloadClosureObserved'])
        self.assertEqual((boundary['passed'], boundary['skipped'], boundary['failed']), (0, 2, 0))
        self.assertEqual({case['name'] for case in boundary['nativeCases']}, set(q.RELOAD_CASES))
        self.assertEqual(result['unitReportCounts']['skipped'], 1)
        self.assertFalse(result['qualificationClaimed'])
        self.assertFalse(result['liveDriverTlsAcceptanceEvaluated'])

    def test_boundary_rejected_on_other_platform_or_historical_regex(self):
        self.darwin_boundary()
        self.artifact['platform'] = 'Linux'
        with self.assertRaisesRegex(RuntimeError, 'Unapproved platform'):
            self.collect()
        self.artifact['platform'] = 'Darwin'
        self.reload_case.find('skipped').set('message', 'SKIP_REGULAR_EXPRESSION_MATCHED')
        with self.assertRaisesRegex(RuntimeError, 'historical reload skip'):
            self.collect(platform='Darwin')

    def test_unknown_and_mandatory_skips_still_rejected_on_darwin(self):
        self.darwin_boundary()
        for name in sorted(q.REQUIRED_TESTS | {'test_shared_crypto_cohabitation', 'unknown_case'}):
            with self.subTest(name=name):
                case = next((item for item in self.xml if item.get('name') == name), None)
                added = case is None
                if added:
                    case = ET.SubElement(self.xml, 'testcase', name=name, status='run')
                case.set('status', 'notrun')
                skip = ET.SubElement(case, 'skipped', message='SKIP_RETURN_CODE')
                self.xml.set('skipped', '2')
                with self.assertRaises(RuntimeError):
                    self.collect(platform='Darwin')
                case.remove(skip)
                case.set('status', 'run')
                if added:
                    self.xml.remove(case)
                self.xml.set('skipped', '1')

    def test_boundary_failures_anywhere_are_not_hidden_by_skip(self):
        self.darwin_boundary()
        for owner in (self.xml, self.reload_xml, self.reload_suite[0]):
            for tag in ('failure', 'error'):
                with self.subTest(tag=tag, owner=owner.tag):
                    element = ET.SubElement(owner, tag)
                    with self.assertRaises(RuntimeError):
                        self.collect(platform='Darwin')
                    owner.remove(element)

    def test_reload_missing_duplicate_wrapper_and_native_inventory_rejected(self):
        self.xml.remove(self.reload_case)
        with self.assertRaisesRegex(RuntimeError, 'Reload unit case missing'):
            self.collect()
        self.xml.append(self.reload_case)
        self.xml.append(self.reload_case)
        with self.assertRaisesRegex(RuntimeError, 'Reload unit case missing or duplicate'):
            self.collect()
        self.xml.remove(self.reload_case)
        case = self.reload_suite[0]
        self.reload_suite.remove(case)
        with self.assertRaises(RuntimeError):
            self.collect()
        self.reload_suite.append(case)
        self.reload_suite.append(case)
        with self.assertRaises(RuntimeError):
            self.collect()

    def test_filtered_suppressed_disabled_and_inconsistent_native_reports_rejected(self):
        case = self.reload_suite[0]
        for field, value in (('status', 'notrun'), ('result', 'suppressed'), ('name', 'OtherCase')):
            original = case.get(field)
            case.set(field, value)
            with self.assertRaises(RuntimeError):
                self.collect()
            case.set(field, original)
        for node in (self.reload_xml, self.reload_suite):
            for field, value in (('tests', '1'), ('failures', '1'), ('disabled', '1'),
                                 ('errors', '1'), ('skipped', '1'), ('tests', '-2'), ('tests', '999999999999999')):
                original = node.get(field)
                node.set(field, value)
                with self.assertRaises(RuntimeError):
                    self.collect()
                if original is None:
                    del node.attrib[field]
                else:
                    node.set(field, original)

    def test_unknown_skip_reason_property_and_extra_context_rejected(self):
        self.darwin_boundary()
        case = self.reload_suite[0]
        skip = case.find('skipped')
        original = skip.get('message')
        for reason in ('unknown loader reason', original + '\nunapproved extra context'):
            skip.set('message', reason)
            skip.text = reason
            with self.assertRaises(RuntimeError):
                self.collect(platform='Darwin')
        skip.set('message', original)
        skip.text = original
        case.find('properties/property').set('value', 'unknown_boundary')
        with self.assertRaisesRegex(RuntimeError, 'Unknown native loader boundary'):
            self.collect(platform='Darwin')

    def test_missing_duplicate_stale_and_inconsistent_stdout_binding_rejected(self):
        output = self.reload_case.find('system-out')
        original = output.text
        for changed in ('', original + '\n' + original, original.replace('123-456', '123-457'),
                        original.replace('selected=2', 'selected=1'), original.replace('result=0', 'result=1'),
                        original.replace('failed=0', 'failed=1'), original.replace('disabled=0', 'disabled=1'),
                        original.replace('passed=2 skipped=0', 'passed=1 skipped=1')):
            output.text = changed
            with self.assertRaises(RuntimeError):
                self.collect()
        output.text = original
        self.reload_xml.set('odbcpp_trace_reload_run', 'another-invocation')
        with self.assertRaisesRegex(RuntimeError, 'Stale'):
            self.collect()

    def test_missing_malformed_oversized_or_alternate_native_xml(self):
        path = self.root / q.RELOAD_REPORT
        with self.assertRaises(FileNotFoundError):
            self.collect(write_reload=False)
        path.write_text('<malformed')
        with self.assertRaises(ET.ParseError):
            self.collect(write_reload=False)
        path.write_bytes(b'x' * (2 * 1024 * 1024 + 1))
        with self.assertRaisesRegex(RuntimeError, 'Oversized'):
            self.collect(write_reload=False)
        alternate = self.root / 'owned-external-gtest-output.xml'
        ET.ElementTree(self.reload_xml).write(alternate)
        result = self.collect(reload_report=alternate, write_reload=False)
        self.assertEqual(result['inputs']['nativeReloadReport'], q.sha(alternate))

    def test_wrapper_native_status_and_skip_counts_must_agree(self):
        self.darwin_boundary()
        self.xml.set('skipped', '0')
        with self.assertRaises(RuntimeError):
            self.collect(platform='Darwin')
        self.xml.set('skipped', '1')
        self.reload_case.set('status', 'run')
        with self.assertRaises(RuntimeError):
            self.collect(platform='Darwin')
        self.reload_case.set('status', 'notrun')
        self.reload_case.find('system-out').text = self.reload_marker(0)
        with self.assertRaises(RuntimeError):
            self.collect(platform='Darwin')

    def test_substituted_driver_probe_and_manifest_rejected(self):
        for path in (self.driver, self.probe, self.root / 'odbcpp-crypto-manifest.json'):
            original = path.read_bytes()
            path.write_bytes(original + b' ')
            with self.assertRaisesRegex(RuntimeError, 'hash mismatch'):
                self.collect()
            path.write_bytes(original)

    def test_shared_host_evidence_missing_duplicate_and_filtered_cases_rejected(self):
        host = self.xml[-1]
        self.xml.remove(host)
        with self.assertRaisesRegex(RuntimeError, 'Mandatory unit case'):
            self.collect()
        self.xml.append(host)
        output = host.find('system-out')
        valid = output.text
        for text in ('', valid + '\n' + sorted(q.COHABITATION_MARKERS)[0]):
            output.text = text
            with self.assertRaisesRegex(RuntimeError, 'cohabitation output'):
                self.collect()
        output.text = valid
        self.assertTrue(self.collect()['sharedProviderHostLifecycleEvaluated'])

    def test_weaker_policy_wrong_version_and_unsupported_claim_rejected(self):
        for field, value in (('activeFips', True), ('verifyPeer', False), ('verifyHostname', False),
                             ('minimumTlsVersion', '1.1'), ('runtimeVersion', 'OpenSSL 3.5.5 date'),
                             ('qualificationClaimed', True)):
            original = self.identity[field]
            self.identity[field] = value
            with self.assertRaises(RuntimeError):
                self.collect()
            self.identity[field] = original

    def test_wrong_linkage_platform_and_unverified_artifact_rejected(self):
        for field, value in (('requestedLinkage', 'BUNDLED_STATIC'), ('platform', 'Windows'),
                             ('dependencyFormVerified', False), ('observedDependencyForm', 'static')):
            original = self.artifact[field]
            self.artifact[field] = value
            with self.assertRaises(RuntimeError):
                self.collect()
            self.artifact[field] = original

    def test_missing_duplicate_and_not_run_unit_case_rejected(self):
        case = self.xml[0]
        self.xml.remove(case)
        with self.assertRaises(RuntimeError):
            self.collect()
        self.xml.append(case)
        self.xml.append(case)
        with self.assertRaises(RuntimeError):
            self.collect()
        self.xml.remove(case)
        case.set('status', 'notrun')
        with self.assertRaises(RuntimeError):
            self.collect()

    def test_failure_and_skip_rejected(self):
        for tag in ('failure', 'error', 'skipped'):
            element = ET.SubElement(self.xml[0], tag)
            with self.assertRaises(RuntimeError):
                self.collect()
            self.xml[0].remove(element)

    def test_inconsistent_report_totals_rejected(self):
        for field in ('failures', 'errors', 'disabled', 'skipped', 'tests'):
            self.xml.set(field, '1')
            with self.assertRaises(RuntimeError):
                self.collect()
            del self.xml.attrib[field]

    def test_missing_legacy_control_and_duplicate_peer_case_rejected(self):
        peer = next(case for case in self.xml if case.get('name') == 'verified_crypto_tls_interop')
        output = peer.find('system-out')
        original = output.text
        output.text = original.replace('TLSv1_1: control accepted; production client rejected downgrade', '')
        with self.assertRaises(RuntimeError):
            self.collect()
        output.text = original + '\n' + sorted(q.TLS_MARKERS)[0]
        with self.assertRaises(RuntimeError):
            self.collect()

    def live_fixture(self):
        root = ET.Element('testsuites', tests='3', failures='0', disabled='0')
        suite = ET.SubElement(root, 'testsuite', tests='3', failures='0', skipped='0')
        for full_name in sorted(q.LIVE_TESTS):
            classname, name = full_name.split('.')
            ET.SubElement(suite, 'testcase', classname=classname, name=name,
                          status='run', result='completed')
        path = self.root / 'live.xml'
        ET.ElementTree(root).write(path)
        return root, suite, path

    def test_live_report_is_bound_without_package_or_coexistence_claim(self):
        _, _, path = self.live_fixture()
        result = self.collect(path)
        self.assertTrue(result['liveDriverTlsAcceptanceEvaluated'])
        self.assertEqual(result['mandatoryLiveDriverTests'], sorted(q.LIVE_TESTS))
        self.assertEqual(result['inputs']['liveReport'], q.sha(path))
        self.assertFalse(result['qualificationClaimed'])
        self.assertFalse(result['liveDriverPackageCoexistenceAcceptanceEvaluated'])
        unit_only = self.collect()
        self.assertFalse(unit_only['liveDriverTlsAcceptanceEvaluated'])
        self.assertNotIn('liveReport', unit_only['inputs'])

    def test_live_missing_duplicate_unexpected_and_incomplete_cases_rejected(self):
        for mutation in ('missing', 'duplicate', 'unexpected', 'notrun', 'suppressed'):
            with self.subTest(mutation=mutation):
                root, suite, path = self.live_fixture()
                case = suite[0]
                if mutation == 'missing':
                    suite.remove(case)
                elif mutation == 'duplicate':
                    suite.append(case)
                elif mutation == 'unexpected':
                    case.set('name', 'Unknown')
                elif mutation == 'notrun':
                    case.set('status', 'notrun')
                else:
                    case.set('result', 'suppressed')
                ET.ElementTree(root).write(path)
                with self.assertRaisesRegex(RuntimeError, 'Mandatory live cases'):
                    self.collect(path)

    def test_live_failure_skip_and_inconsistent_totals_rejected(self):
        for tag in ('failure', 'error', 'skipped'):
            root, suite, path = self.live_fixture()
            ET.SubElement(suite[0], tag)
            ET.ElementTree(root).write(path)
            with self.assertRaisesRegex(RuntimeError, 'failed or skipped'):
                self.collect(path)
        for field in ('tests', 'failures', 'errors', 'disabled', 'skipped'):
            root, suite, path = self.live_fixture()
            suite.set(field, '1')
            ET.ElementTree(root).write(path)
            with self.assertRaisesRegex(RuntimeError, 'live report totals'):
                self.collect(path)

    def test_live_binding_rejects_unsupported_profile(self):
        _, _, path = self.live_fixture()
        self.manifest['requestedLinkage'] = 'BUNDLED_STATIC'
        self.write('manifest', self.manifest)
        self.identity['manifestSha256'] = q.sha(self.root / 'odbcpp-crypto-manifest.json')
        self.identity['requestedLinkage'] = 'BUNDLED_STATIC'
        self.artifact['requestedLinkage'] = 'BUNDLED_STATIC'
        self.artifact['observedDependencyForm'] = 'static'
        with self.assertRaisesRegex(RuntimeError, 'limited to Linux SYSTEM_SHARED'):
            self.collect(path)

    def test_cli_removes_stale_summary_for_missing_live_report(self):
        self.collect()
        output = self.root / 'live-summary.json'
        output.write_text('stale success')
        result = subprocess.run([sys.executable, str(Path(q.__file__)),
                                 '--build', str(self.root), '--driver', str(self.driver),
                                 '--probe', str(self.probe), '--unit-report', str(self.report),
                                 '--platform', 'Linux', '--live-report', str(self.root / 'missing.xml'),
                                 '--output', str(output)], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())

    def test_cli_removes_stale_summary_after_failed_validation(self):
        self.collect()
        output = self.root / 'summary.json'
        output.write_text('stale success')
        self.driver.write_bytes(b'substituted driver')
        result = subprocess.run([sys.executable, str(Path(q.__file__)),
                                 '--build', str(self.root), '--driver', str(self.driver),
                                 '--probe', str(self.probe), '--unit-report', str(self.report),
                                 '--platform', 'Linux', '--output', str(output)],
                                capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('hash mismatch', result.stderr)
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
