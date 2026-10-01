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
        host = ET.SubElement(self.xml, 'testcase', name='test_shared_crypto_cohabitation', status='run')
        ET.SubElement(host, 'system-out').text = '\n'.join(sorted(q.COHABITATION_MARKERS))

    def write(self, kind, data):
        (self.root / f'odbcpp-crypto-{kind}.json').write_text(json.dumps(data))

    def collect(self):
        self.write('identity-evidence', self.identity)
        self.write('artifact-evidence', self.artifact)
        ET.ElementTree(self.xml).write(self.report)
        return q.collect(self.root, self.driver, self.probe, self.report, 'Linux')

    def test_valid_record_has_bound_inputs_and_no_qualification_claim(self):
        result = self.collect()
        self.assertFalse(result['qualificationClaimed'])
        self.assertFalse(result['liveDriverPackageCoexistenceAcceptanceEvaluated'])
        self.assertEqual(result['inputs']['driver'], q.sha(self.driver))
        self.assertEqual(result['inputs']['unitReport'], q.sha(self.report))

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
