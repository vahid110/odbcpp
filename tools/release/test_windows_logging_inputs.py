"""Execute the real PowerShell package-input reader without installing anything."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

# Sibling import, like the other release tests: unittest discovery and direct
# script runs both put this directory on sys.path.
from test_logging_metadata import ROOT, INPUTS, populate, generate

# Hosts without PowerShell 7 skip; the Windows tooling job sets the requirement.
PWSH = shutil.which('pwsh')
REQUIRE_PWSH = os.environ.get('ODBCPP_REQUIRE_PWSH') == '1'


@unittest.skipUnless(PWSH or REQUIRE_PWSH, 'PowerShell 7 (pwsh) is not available')
class WindowsLoggingInputsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='windows logging inputs ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'
        self.source = self.root / 'override source'
        populate(self.source)
        self.configure()
        self.metadata = self.build / 'odbcpp-logging-package-inputs.json'
        self.reader = self.root / 'read.ps1'
        self.reader.write_text(
            "$ErrorActionPreference = 'Stop'\n"
            f". '{ROOT.as_posix()}/packaging/windows/logging-inputs.ps1'\n"
            f"Read-LoggingPackageInputs '{self.build.as_posix()}' | ConvertTo-Json -Depth 6\n")

    def configure(self):
        result = generate(self.source, self.build)
        self.assertEqual(result.returncode, 0, result.stderr)

    def read(self, error=None):
        result = subprocess.run([PWSH or 'pwsh', '-NoProfile', '-File', str(self.reader)],
                                capture_output=True, text=True, timeout=30)
        if error:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn(error, result.stderr)
            return None
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def change_metadata(self, change):
        data = json.loads(self.metadata.read_text())
        change(data)
        self.metadata.write_text(json.dumps(data))

    def test_override_beats_unrelated_default_copy(self):
        decoy = self.build / '_deps/spdlog-src'
        populate(decoy)
        (decoy / 'include/spdlog/version.h').write_text('#define SPDLOG_VER_MAJOR 99\n')
        (decoy / 'LICENSE').write_text('Unrelated license')
        result = self.read()
        self.assertEqual(Path(result['sourceDirectory']), self.source.resolve())
        self.assertEqual(result['versions'], {'spdlog': '1.17.0', 'fmt': '12.1.0'})
        self.assertEqual(set(result['evidence']['files']), set(INPUTS))
        self.assertNotIn(str(self.root), json.dumps(result['evidence']))
        self.assertNotIn(self.root.as_posix(), json.dumps(result['evidence']))

    def test_default_source(self):
        self.source = self.build / '_deps/spdlog-src'
        populate(self.source)
        self.configure()
        self.assertEqual(Path(self.read()['sourceDirectory']), self.source.resolve())

    def test_missing_metadata_does_not_fall_back(self):
        populate(self.build / '_deps/spdlog-src')
        self.metadata.unlink()
        self.read('Missing logging package metadata')

    def test_missing_source_does_not_fall_back(self):
        populate(self.build / '_deps/spdlog-src')
        self.change_metadata(lambda data: data.update(sourceDirectory=str(self.root / 'absent')))
        self.read('Missing or invalid configured logging source')

    def test_changed_each_input_refused(self):
        for member in INPUTS:
            with self.subTest(member=member):
                path = self.source / member
                path.write_bytes(path.read_bytes() + b'changed')
                self.read('Missing or changed logging package input')
                path.write_bytes(INPUTS[member].encode())

    def test_missing_each_input_refused(self):
        for member in INPUTS:
            with self.subTest(member=member):
                (self.source / member).unlink()
                self.read('Missing or changed logging package input')
                populate(self.source)

    def test_absent_at_configure_refused_even_if_later_supplied(self):
        (self.source / 'LICENSE').unlink()
        self.configure()
        populate(self.source)
        self.read('Missing or changed logging package input')

    def test_switched_source_with_different_headers_refused(self):
        other = self.root / 'other'
        populate(other)
        (other / 'include/spdlog/fmt/bundled/base.h').write_text('#define FMT_VERSION 99999\n')
        self.change_metadata(lambda data: data.update(sourceDirectory=str(other)))
        self.read('Missing or changed logging package input')

    def test_extra_member_refused(self):
        self.change_metadata(lambda data: data['files'].update({'../escape': 'a' * 64}))
        self.read('Invalid logging package input inventory')

    def test_bad_schema_refused(self):
        self.change_metadata(lambda data: data.update(schemaVersion=2))
        self.read('Invalid logging package metadata')

    def test_artifact_claim_refused(self):
        self.change_metadata(lambda data: data.update(configurationEvidenceOnly=False))
        self.read('Invalid logging package metadata')

    def test_recorded_invalid_or_duplicate_macro_refused(self):
        for text in ('// no version macro\n', '#define FMT_VERSION 120100\n#define FMT_VERSION 130000\n'):
            with self.subTest(header=text):
                (self.source / 'include/spdlog/fmt/bundled/base.h').write_text(text)
                self.configure()
                self.read('Missing or ambiguous logging version macro')


if __name__ == '__main__':
    unittest.main()
