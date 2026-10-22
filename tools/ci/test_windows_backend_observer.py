"""Offline Windows control tests: never launch a backend executable."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


SCRIPT = Path(__file__).with_name('windows-backend-direct-exit009.ps1')
POWERSHELL = Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
HARNESS = r'''
param([string]$Observer, [string]$InputXml, [switch]$ClassifyOnly)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if ($ClassifyOnly) {
  # Execute the actual checked-in classifier, without the child-launch body.
  $tokens=$null; $parseErrors=$null
  $ast=[Management.Automation.Language.Parser]::ParseFile($Observer,[ref]$tokens,[ref]$parseErrors)
  if ($parseErrors.Count -ne 0) { throw 'observer_parse_failed' }
  $functions=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Test-BackendDiagnosticEligibility'},$true))
  if ($functions.Count -ne 1) { throw 'classifier_definition_missing_or_duplicate' }
  Invoke-Expression $functions[0].Extent.Text
  $settings=[Xml.XmlReaderSettings]::new(); $settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit
  $settings.XmlResolver=$null; $settings.MaxCharactersInDocument=16777216
  $reader=[Xml.XmlReader]::Create($InputXml,$settings)
  try { $report=[Xml.XmlDocument]::new(); $report.XmlResolver=$null; $report.Load($reader) }
  finally { $reader.Dispose() }
  if (Test-BackendDiagnosticEligibility $report) { Write-Output 'eligible' }
  else { Write-Output 'not_applicable' }
  exit 0
}
# Windows PowerShell has no automatic IsWindows variable. Seed it from the
# actual OS for this no-child control path; pwsh already defines it read-only.
if ($null -eq (Get-Variable IsWindows -ErrorAction SilentlyContinue)) {
  $IsWindows=([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT)
}
& $Observer -ExpectedHead ('a' * 40)
exit $LASTEXITCODE
'''


def report(cases):
    """Small CTest-shaped original-suite fixture, including failed/skipped bodies."""
    root = ET.Element('testsuite', name='Win32-MSBuild', tests=str(len(cases)),
                      failures=str(sum(status == 'fail' for _, status in cases)))
    for name, status in cases:
        case = ET.SubElement(root, 'testcase', name=name, classname=name, status=status)
        if status == 'fail':
            ET.SubElement(case, 'failure', message='Failed')
        if status == 'notrun':
            ET.SubElement(case, 'skipped', message='Skipped')
    return ET.tostring(root, encoding='unicode')


@unittest.skipUnless(os.name == 'nt' and POWERSHELL.is_file(), 'requires actual Windows PowerShell')
class BackendObserverEligibility(unittest.TestCase):
    def invoke(self, xml, *, full_script=False):
        with tempfile.TemporaryDirectory(prefix='odbcpp-observer-') as tmp:
            workspace = Path(tmp)
            build = workspace / 'build-windows'
            binary = build / 'tests/Release/test_backend_contract.exe'
            binary.parent.mkdir(parents=True)
            # Deliberately not executable: a classifier regression must never
            # create its diagnostic directory or attempt to launch this input.
            binary.write_bytes(b'not-an-executable; must-not-start')
            original = build / 'windows-openssl-unit.xml'
            original.write_text(xml, encoding='utf-8')
            log = build / 'Testing/Temporary/LastTest.log'
            log.parent.mkdir(parents=True)
            log.write_bytes(b'original failed CTest log\n')
            paths = (binary, original, log)
            before = [hashlib.sha256(p.read_bytes()).hexdigest() for p in paths]
            harness = workspace / 'harness.ps1'
            harness.write_text(HARNESS, encoding='utf-8')
            env = {k: v for k, v in os.environ.items() if not k.startswith('GTEST_')}
            env.update(GITHUB_SHA='a' * 40, GITHUB_WORKSPACE=str(workspace))
            args = [str(POWERSHELL), '-NoProfile', '-NonInteractive', '-File', str(harness),
                    '-Observer', str(SCRIPT), '-InputXml', str(original)]
            if not full_script:
                args.append('-ClassifyOnly')
            result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(before, [hashlib.sha256(p.read_bytes()).hexdigest() for p in paths])
            self.assertFalse((build / 'windows-backend-direct-exit009').exists(), result.stdout + result.stderr)
            return result

    def test_backend_failure_is_eligible_even_with_another_failure(self):
        for cases in ([('test_backend_contract', 'fail')],
                      [('test_backend_contract', 'fail'), ('test_setup_dialog', 'fail')]):
            with self.subTest(cases=cases):
                result = self.invoke(report(cases))
                self.assertEqual(0, result.returncode, result.stderr)
                self.assertEqual('eligible', result.stdout.strip())

    def test_original_gui_failure_with_passing_backend_starts_no_child(self):
        xml = report([('test_backend_contract', 'run'), ('test_setup_dialog', 'fail'),
                      ('test_platform_boundary', 'notrun')])
        classified = self.invoke(xml)
        self.assertEqual(0, classified.returncode, classified.stderr)
        self.assertEqual('not_applicable', classified.stdout.strip())
        actual = self.invoke(xml, full_script=True)
        self.assertEqual(1, actual.returncode)
        self.assertIn('backend_diagnostic_not_applicable', actual.stdout)
        self.assertIn('original suite failed', actual.stdout)
        self.assertIn('no child started', actual.stdout)
        self.assertEqual('', actual.stderr)

    def test_ambiguous_or_malformed_originals_are_refused_before_launch(self):
        good = report([('test_backend_contract', 'run'), ('test_setup_dialog', 'fail')])
        invalid = {
            'missing_backend': report([('test_setup_dialog', 'fail')]),
            'duplicate_backend': report([('test_backend_contract', 'run'), ('test_backend_contract', 'fail')]),
            'duplicate_other': report([('test_backend_contract', 'run'), ('test_setup_dialog', 'fail'), ('test_setup_dialog', 'run')]),
            'skipped_backend': report([('test_backend_contract', 'notrun'), ('test_setup_dialog', 'fail')]),
            'unsafe_name': good.replace('test_setup_dialog', 'test_setup_dialog;whoami'),
            'no_original_failure': report([('test_backend_contract', 'run')]),
            'wrong_case_count': good.replace('tests="2"', 'tests="3"'),
            'wrong_failure_count': good.replace('failures="1"', 'failures="2"'),
            'failure_status_mismatch': good.replace('status="fail"', 'status="run"'),
            'missing_failure_body': good.replace('<failure message="Failed" />', ''),
            'ambiguous_failure_body': good.replace('<failure message="Failed" />', '<failure/><failure/>'),
            'error_body': good.replace('<failure message="Failed" />', '<error/>'),
            'nested_case': good.replace('<testcase name="test_setup_dialog"', '<group><testcase name="test_setup_dialog"').replace('</testsuite>', '</group></testsuite>'),
            'malformed_xml': '<testsuite>',
            'dtd': '<!DOCTYPE testsuite [<!ENTITY x "secret">]>' + good,
        }
        for name, xml in invalid.items():
            with self.subTest(name=name):
                result = self.invoke(xml, full_script=True)
                self.assertNotEqual(0, result.returncode)
                self.assertNotIn('backend_diagnostic_not_applicable', result.stdout)
                self.assertTrue(result.stderr, 'a refused input must report its error')


if __name__ == '__main__':
    unittest.main()
