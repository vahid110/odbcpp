"""Offline configure-time package-input controls; no dependency download."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
INPUTS = {
    'LICENSE': 'Synthetic spdlog notice\n',
    'include/spdlog/version.h': '#define SPDLOG_VER_MAJOR 1\n#define SPDLOG_VER_MINOR 17\n#define SPDLOG_VER_PATCH 0\n',
    'include/spdlog/fmt/bundled/fmt.license.rst': 'Synthetic bundled fmt notice\n',
    'include/spdlog/fmt/bundled/base.h': '#define FMT_VERSION 120100\n',
}


def populate(source):
    for member, text in INPUTS.items():
        path = source / member
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode())


def generate(source, build):
    build.mkdir(parents=True, exist_ok=True)
    script = build / 'write.cmake'
    script.write_text(
        f'include("{ROOT.as_posix()}/cmake/LoggingPackageMetadata.cmake")\n'
        f'odbcpp_write_logging_package_metadata([==[{source.as_posix()}]==] '
        f'[==[{build.as_posix()}/odbcpp-logging-package-inputs.json]==])\n')
    return subprocess.run(['cmake', '-P', str(script)], capture_output=True,
                          text=True, timeout=30)


class LoggingMetadataTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='logging inputs ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_resolved_default_and_override_sources_with_spaces(self):
        for name in ('build/_deps/spdlog-src', 'explicit dependency source'):
            with self.subTest(source=name):
                source = self.root / name
                populate(source)
                build = self.root / 'metadata'
                result = generate(source, build)
                self.assertEqual(result.returncode, 0, result.stderr)
                data = json.loads((build / 'odbcpp-logging-package-inputs.json').read_text())
                self.assertEqual(Path(data['sourceDirectory']), source.resolve())
                self.assertEqual(data['schemaVersion'], 1)
                self.assertIs(data['configurationEvidenceOnly'], True)
                self.assertEqual(data['files'], {
                    name: hashlib.sha256(text.encode()).hexdigest()
                    for name, text in INPUTS.items()})

    def test_missing_inputs_recorded_without_blocking_nonpackaging_configure(self):
        for i, member in enumerate(INPUTS):
            with self.subTest(member=member):
                source = self.root / str(i)
                populate(source)
                (source / member).unlink()
                build = self.root / f'build-{i}'
                result = generate(source, build)
                self.assertEqual(result.returncode, 0, result.stderr)
                data = json.loads((build / 'odbcpp-logging-package-inputs.json').read_text())
                self.assertIsNone(data['files'][member])

    def test_directory_is_not_an_input_file(self):
        source = self.root / 'source'
        populate(source)
        (source / 'LICENSE').unlink()
        (source / 'LICENSE').mkdir()
        result = generate(source, self.root / 'build')
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads((self.root / 'build/odbcpp-logging-package-inputs.json').read_text())
        self.assertIsNone(data['files']['LICENSE'])


if __name__ == '__main__':
    unittest.main()
