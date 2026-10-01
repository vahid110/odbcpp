"""Positive and adversarial checks for the bounded static runtime archive."""
import importlib.util
import json
import sys
from pathlib import Path
import tempfile
import tarfile
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('static_package', Path(__file__).with_name('prove-openssl-static-package.py'))
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)


class StaticPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build, self.package = self.root / 'build', self.root / p.ROOT_NAME
        self.build.mkdir()
        (self.build / 'libodbcpp.so').write_bytes(b'driver')
        self.license = self.root / 'copyright'
        self.license.write_text('OpenSSL distribution license')
        logging = self.build / '_deps/spdlog-src'
        (logging / 'include/spdlog/fmt/bundled').mkdir(parents=True)
        (logging / 'LICENSE').write_text('spdlog license')
        (logging / 'include/spdlog/fmt/bundled/base.h').write_text('// See format.h for license\n#define FMT_VERSION 120100\n')
        (logging / 'include/spdlog/fmt/bundled/format.h').write_text('/* fmt license */\n')
        self.manifest = {'provider': 'OPENSSL', 'requestedLinkage': 'BUNDLED_STATIC',
                         'compileVersion': '3.6.4', 'fipsClaimed': False}
        self.artifact = {'provider': 'OPENSSL', 'requestedLinkage': 'BUNDLED_STATIC',
                         'qualificationClaimed': False, 'platform': 'Linux',
                         'observedDependencyForm': 'static', 'dependencyFormVerified': True,
                         'staticLinkMapVerified': True, 'staticArchiveMemberEvidenceVerified': True,
                         'sslLinkInputSha256': 'ssl', 'cryptoLinkInputSha256': 'crypto',
                         'artifactSha256': p.sha(self.build / 'libodbcpp.so')}
        (self.root / 'openssl-static-source-evidence.json').write_text(json.dumps({
            'package': 'libssl-dev', 'packageVersion': 'fixture',
            'sourceArchives': {'libssl.a': {'sha256': 'ssl'}, 'libcrypto.a': {'sha256': 'crypto'}}}))
        self.write_inputs()

    def write_inputs(self):
        for name, value in (('manifest', self.manifest), ('artifact-evidence', self.artifact)):
            (self.build / f'odbcpp-crypto-{name}.json').write_text(json.dumps(value))

    def stage(self):
        return p.stage(self.build, self.package, self.license)

    def test_driver_only_archive_roundtrip_and_licenses(self):
        manifest = self.stage()
        expected = p.archive_checks.inventory(self.package)
        self.assertEqual(set(manifest['licenseFiles']), p.LICENSES)
        self.assertFalse(manifest['qualificationClaimed'])
        archive = self.root / 'runtime.tar.gz'
        with tarfile.open(archive, 'w:gz') as stream:
            stream.add(self.package, arcname=p.ROOT_NAME)
        result = p.archive_checks.extract_verified(archive, self.root / 'extracted', expected, p.ROOT_NAME)
        self.assertEqual(p.validate(result), manifest)

    def test_missing_or_empty_dependency_license_rejected(self):
        self.license.write_text('')
        with self.assertRaisesRegex(RuntimeError, 'empty'):
            self.stage()
        self.license.unlink()
        with self.assertRaises(FileNotFoundError):
            self.stage()

    def test_nonstatic_and_unbound_driver_rejected(self):
        self.manifest['requestedLinkage'] = 'SYSTEM_SHARED'
        self.write_inputs()
        with self.assertRaises(RuntimeError):
            self.stage()
        self.manifest['requestedLinkage'] = 'BUNDLED_STATIC'
        self.write_inputs()
        (self.build / 'libodbcpp.so').write_bytes(b'changed driver')
        with self.assertRaises(RuntimeError):
            self.stage()

    def test_source_archive_binding_mismatch_rejected(self):
        self.artifact['sslLinkInputSha256'] = 'different archive'
        self.write_inputs()
        with self.assertRaises(RuntimeError):
            self.stage()

    def test_tampered_driver_license_or_missing_license_rejected(self):
        self.stage()
        for name in ('lib/libodbcpp.so', 'licenses/openssl/copyright'):
            file = self.package / name
            original = file.read_bytes()
            file.write_bytes(b'substituted')
            with self.assertRaises(RuntimeError):
                p.validate(self.package)
            file.write_bytes(original)
        (self.package / 'licenses/fmt/LICENSE').unlink()
        with self.assertRaises(RuntimeError):
            p.validate(self.package)

    def test_development_and_unlisted_runtime_files_rejected(self):
        self.stage()
        for name in ('provider.h', 'provider.a', 'extra.so'):
            file = self.package / name
            file.write_bytes(b'not runtime inventory')
            with self.assertRaises(RuntimeError):
                p.validate(self.package)
            file.unlink()

    def test_archive_root_cannot_escape(self):
        with self.assertRaisesRegex(RuntimeError, 'simple directory'):
            p.archive_checks.extract_verified(self.root / 'missing.tar.gz', self.root / 'extracted', {}, '../escape')


if __name__ == '__main__':
    unittest.main()
