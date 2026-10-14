"""Offline orchestration safety checks; these are not Windows product proof."""
from copy import deepcopy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('windows_product', Path(__file__).with_name('windows_product_qualification.py'))
w = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w)


class RequestSafety(unittest.TestCase):
    def setUp(self):
        self.env = {'WINDOWS_WORKFLOW_HEAD': 'a' * 40, 'WINDOWS_PRODUCT_HEAD': w.FIRST_PRODUCT,
                    'WINDOWS_PRODUCT_BASE': 'b' * 40, 'WINDOWS_PRODUCT_TARGETS': 'test_backend_contract',
                    'WINDOWS_CONFLICTING_MODE': 'false', 'WINDOWS_CACHE_REQUEST': '{}',
                    'WINDOWS_SUBMITTED_AT': '2026-01-01T00:00:00+00:00',
                    'GITHUB_REPOSITORY': 'vahid110/odbcpp', 'GITHUB_REPOSITORY_VISIBILITY': 'public',
                    'RUNNER_ENVIRONMENT': 'github-hosted', 'RUNNER_OS': 'Windows', 'RUNNER_ARCH': 'X64'}

    def test_exact_cold_request_and_owning_selection(self):
        self.assertIsNotNone(w.request_guard(self.env))
        self.assertEqual(w.selection('test_backend_contract,test_diagnostics_apis'),
                         ['test_backend_contract', 'test_diagnostics_apis'])

    def test_reject_mutable_refs_and_injected_or_duplicate_targets(self):
        for key in ('WINDOWS_WORKFLOW_HEAD', 'WINDOWS_PRODUCT_HEAD', 'WINDOWS_PRODUCT_BASE'):
            for bad in ('main', 'a' * 39, 'A' * 40, 'a' * 40 + '; echo bad'):
                env = dict(self.env, **{key: bad})
                with self.subTest(key=key, value=bad), self.assertRaises(ValueError):
                    w.request_guard(env)
        for bad in ('', 'test_backend_contract;whoami', '.*', 'all',
                    'test_backend_contract,test_backend_contract', ' test_backend_contract', ','.join(w.TARGETS)):
            with self.subTest(targets=bad), self.assertRaises(ValueError):
                w.selection(bad)

    def test_reject_conflicts_cache_private_paid_or_wrong_platform(self):
        for key, bad in (('WINDOWS_CONFLICTING_MODE', 'true'), ('WINDOWS_CACHE_REQUEST', '{"key":"x"}'),
                         ('GITHUB_REPOSITORY', 'some/private'), ('GITHUB_REPOSITORY_VISIBILITY', 'private'),
                         ('RUNNER_ENVIRONMENT', 'self-hosted'), ('RUNNER_ARCH', 'ARM64'),
                         ('RUNNER_OS', 'Linux'), ('WINDOWS_PRODUCT_TARGETS', 'test_diagnostics_apis'),
                         ('WINDOWS_SUBMITTED_AT', '2999-01-01T00:00:00Z'),
                         ('WINDOWS_SUBMITTED_AT', '2026-01-01T00:00:00')):
            with self.subTest(key=key, value=bad), self.assertRaises(ValueError):
                w.request_guard(dict(self.env, **{key: bad}))

    def test_source_identity_rejects_tracked_and_untracked_injection(self):
        responses = ['a' * 40, ' M core/x.cpp']
        with patch.object(w, 'output', side_effect=responses), self.assertRaises(ValueError):
            w.source('a' * 40)
        responses = ['a' * 40, '', 'core/injected.cpp']
        with patch.object(w, 'output', side_effect=responses), self.assertRaises(ValueError):
            w.source('a' * 40)

    def test_source_guard_checks_both_immutable_sources(self):
        before = {'product': {'head': 'a'}, 'workflow': {'head': 'b'}}
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            w.write('identity.json', {'sources': before})
            for role in ('product', 'workflow'):
                after = deepcopy(before)
                after[role]['head'] = 'changed'
                with patch.object(w, 'sources', return_value=after), self.assertRaises(ValueError):
                    w.source_guard()


class ResultAndFailureSafety(unittest.TestCase):
    def test_real_inventory_requires_cases_and_rejects_disabled(self):
        text = 'Running main() from gtest_main.cc\nBackendContractTest.\n  One\n  Two # TypeParam = x\n'
        self.assertEqual(w.gtest_names(text), ['BackendContractTest.One', 'BackendContractTest.Two'])
        for bad in ('', 'Empty.\n', 'Suite.\n  DISABLED_case\n', 'Suite.\n  One\n  One\n'):
            with self.subTest(value=bad), self.assertRaises(ValueError):
                w.gtest_names(bad)

    def test_xml_exact_inventory_failure_skip_and_missing_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'owning.xml'
            good = '<testsuites tests="1"><testsuite><testcase classname="Suite" name="One"/></testsuite></testsuites>'
            path.write_text(good)
            self.assertEqual(w.xml_result(path, ['Suite.One'])['count'], 1)
            for bad in (good.replace('/>', '><failure/></testcase>'),
                        good.replace('/>', '><skipped/></testcase>'),
                        good.replace('name="One"', 'name="Two"'),
                        good.replace('tests="1"', 'tests="2"'),
                        good.replace('name="One"', 'name="One" status="notrun"'),
                        good.replace('tests="1"', 'tests="1" failures="1"')):
                path.write_text(bad)
                with self.subTest(xml=bad), self.assertRaises(ValueError):
                    w.xml_result(path, ['Suite.One'])

    def test_failed_and_missing_phases_never_become_completion(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            complete, phases = w.completion()
            self.assertFalse(complete)
            self.assertTrue(all(v['status'] == 'missing' for v in phases.values()))
            for phase in w.PHASES:
                w.write(phase + '.json', {'status': 'passed'})
            self.assertFalse(w.completion()[0])  # Missing actual XML-derived results.
            w.write('results.json', {'owning': 'validated'})
            self.assertTrue(w.completion()[0])
            w.write('build.json', {'status': 'failed', 'error': 'original MSVC failure'})
            self.assertFalse(w.completion()[0])
            with patch.object(w, 'source_guard'), self.assertRaises(RuntimeError):
                w.finish()
            self.assertEqual(json.loads((Path(tmp) / 'build.json').read_text())['error'], 'original MSVC failure')

    def test_failed_command_preserves_exit_and_log(self):
        # Execute only a trivial local offline child, never a product/helper phase.
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            with self.assertRaises(RuntimeError):
                w.measure('failure', [os.sys.executable, '-c', 'print("original diagnostic"); raise SystemExit(7)'], 10)
            record = json.loads((Path(tmp) / 'failure-command.json').read_text())
            self.assertEqual(record['exit'], 7)
            self.assertTrue(record['direct_terminal'])
            self.assertIn('original diagnostic', (Path(tmp) / 'failure.log').read_text())

    def test_truncated_log_keeps_original_final_diagnostic(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)), patch.object(w, 'LOG_BYTES', 16):
            with self.assertRaises(RuntimeError):
                w.measure('large', [os.sys.executable, '-c', 'print("x" * 100); print("final original error"); raise SystemExit(9)'], 10)
            self.assertIn('final original error', (Path(tmp) / 'large.log').read_text())
            self.assertTrue(json.loads((Path(tmp) / 'large-command.json').read_text())['log_truncated'])

    def test_timeout_preserves_failure_and_terminal(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            with self.assertRaises(RuntimeError):
                w.measure('timeout', [os.sys.executable, '-c', 'import time; time.sleep(10)'], 0.05)
            record = json.loads((Path(tmp) / 'timeout-command.json').read_text())
            self.assertTrue(record['timed_out'])
            self.assertTrue(record['direct_terminal'])

    def test_artifact_includes_index_in_bound_and_rejects_binary_or_symlink(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            w.write('a.json', {'text': 'evidence'})
            w.package()
            self.assertIn('a.json', json.loads((Path(tmp) / 'evidence-index.json').read_text()))
            with patch.object(w, 'MAX_BYTES', 10), self.assertRaises(ValueError):
                w.package()
            binary = Path(tmp) / 'restored.exe'
            binary.write_bytes(b'MZ')
            with self.assertRaises(ValueError):
                w.package()
            binary.unlink()
            link = Path(tmp) / 'link.txt'
            try:
                link.symlink_to(Path(tmp) / 'a.json')
            except OSError:
                return  # Windows non-admin symlink permission is not a product result.
            with self.assertRaises(ValueError):
                w.package()


class WorkflowSafety(unittest.TestCase):
    def test_default_off_mutual_modes_and_full_body_preservation(self):
        # No YAML dependency: compare actual baseline full-job bodies byte for byte.
        ci = (ROOT / '.github/workflows/ci.yml').read_text()
        old = subprocess.check_output(['git', '-C', str(ROOT), 'show', w.FIRST_PRODUCT + ':.github/workflows/ci.yml'], text=True)
        jobs = ('build-and-test', 'redshift-build', 'windows-live', 'windows-package',
                'windows-linked-server', 'linux-sanitizers', 'linux-openssl-static',
                'awslc-adapter-proof', 'iodbc-unicode')
        import re
        def job(text, name):
            block = re.search(r'^  ' + name + r':\n(.*?)(?=^  [a-z][a-z-]*:|\Z)', text, re.M | re.S)[1]
            return '\n'.join(line for line in block.splitlines() if not line.startswith('    if:'))
        for name in jobs:
            self.assertEqual(job(ci, name), job(old, name), name)
        self.assertIn('run_windows_product_qualification:\n', ci)
        window_input = ci.split('      run_windows_product_qualification:', 1)[1].split('      windows_product_targets:', 1)[0]
        self.assertIn('default: false', window_input)
        for name in jobs:
            guard = ci.split('\n  ' + name + ':', 1)[1].splitlines()[1]
            self.assertIn('run_windows_product_qualification', guard)
        self.assertIn("'windows-product-qualification'", ci)
        self.assertIn("'ubuntu-development-pilot'", ci)
        self.assertIn('cancel-in-progress: false', ci)
        self.assertIn('inputs.run_development_pilot || inputs.run_product_qualification || inputs.run_linked_server', ci)
        self.assertIn('inputs.run_product_qualification && !inputs.run_windows_product_qualification', ci)
        self.assertIn('inputs.run_development_pilot && !inputs.run_product_qualification && !inputs.run_windows_product_qualification', ci)

    def test_windows_job_has_no_restores_services_native_or_secret_steps(self):
        workflow = (ROOT / '.github/workflows/windows-product-qualification.yml').read_text()
        self.assertIn('runs-on: windows-2025', workflow)
        self.assertIn('timeout-minutes: 30', workflow)
        self.assertIn('retention-days: 3', workflow)
        self.assertIn('if: always()', workflow)
        for forbidden in ('actions/cache', 'services:', 'secrets:', 'windows-postgres', 'windows-system-dsn',
                          'packaging/windows', 'Start-Service', 'self-hosted', 'larger-runner'):
            self.assertNotIn(forbidden, workflow)
        self.assertIn('persist-credentials: false', workflow)
        self.assertIn('github.event.repository.private == false', workflow)
        self.assertIn("'--parallel', '2'", (ROOT / 'tools/ci/windows_product_qualification.py').read_text())
        self.assertIn('-DODBCPP_CRYPTO_PROFILE_LIVE_TESTS=OFF', w.PROFILE)
        self.assertIn('-DODBCPP_CRYPTO_TLS_PROOF=OFF', w.PROFILE)
        self.assertEqual(len(w.CHECKS), 6)
        self.assertEqual(len(w.NUMERIC_CASES), 4)


if __name__ == '__main__':
    unittest.main()
