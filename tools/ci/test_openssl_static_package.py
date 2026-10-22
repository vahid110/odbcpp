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
probe_spec = importlib.util.spec_from_file_location('static_load_probe', Path(__file__).with_name('static_load_probe.py'))
probe = importlib.util.module_from_spec(probe_spec)
probe_spec.loader.exec_module(probe)


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
        self.source = {
            'package': 'libssl-dev', 'packageVersion': 'fixture',
            'sourceArchives': {'libssl.a': {'sha256': 'ssl'}, 'libcrypto.a': {'sha256': 'crypto'}}}
        self.write_inputs()

    def write_inputs(self):
        (self.root / 'openssl-static-source-evidence.json').write_text(json.dumps(self.source))
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

    def add_auxiliary(self):
        notice = self.root / 'jitterentropy-copyright'
        notice.write_text('Jitterentropy distribution notice')
        entry = {'path': '/controlled/lib/libjitterentropy.a', 'sha256': 'fixture-aux'}
        self.manifest['auxiliaryStaticLinkInputs'] = [entry]
        self.artifact['auxiliaryStaticLinkInputs'] = [dict(entry, linkMapLoadVerified=True,
                                                          archiveMembersObserved=True)]
        self.source['auxiliaryArchives'] = ['libjitterentropy.a']
        self.source['sourceArchives']['libjitterentropy.a'] = dict(
            entry, controlledPath=entry['path'], licensePath=str(notice), licenseSha256=p.sha(notice),
            package='libjitterentropy3-dev:fixture', packageVersion='fixture')
        self.write_inputs()
        return notice

    def test_embedded_auxiliary_notice_inventory_and_tamper(self):
        self.add_auxiliary()
        manifest = self.stage()
        self.assertEqual(set(manifest['licenseFiles']), p.LICENSES | {'licenses/jitterentropy/copyright'})
        notice = self.package / 'licenses/jitterentropy/copyright'
        notice.write_bytes(b'changed notice')
        with self.assertRaisesRegex(RuntimeError, 'hash mismatch'):
            p.validate(self.package)

    def test_missing_changed_or_empty_auxiliary_notice_refused(self):
        notice = self.add_auxiliary()
        notice.write_bytes(b'changed notice')
        with self.assertRaisesRegex(RuntimeError, 'license changed'):
            self.stage()
        notice.unlink()
        with self.assertRaises(FileNotFoundError):
            self.stage()
        notice.write_bytes(b'')
        self.source['sourceArchives']['libjitterentropy.a']['licenseSha256'] = p.sha(notice)
        self.write_inputs()
        with self.assertRaisesRegex(RuntimeError, 'empty'):
            self.stage()

    def test_auxiliary_binding_or_omitted_inventory_refused(self):
        self.add_auxiliary()
        self.artifact['auxiliaryStaticLinkInputs'][0]['sha256'] = 'different'
        self.write_inputs()
        with self.assertRaisesRegex(RuntimeError, 'binding mismatch'):
            self.stage()
        self.source['auxiliaryArchives'] = []
        self.write_inputs()
        with self.assertRaisesRegex(RuntimeError, 'inventory mismatch'):
            self.stage()


class StaticLoadObservationTests(unittest.TestCase):
    driver = '/extracted/lib/libodbcpp.so'
    driver_identity = (8, 1, 123)

    @staticmethod
    def row(path, inode=12, start=0x1000, permissions='r-xp'):
        return dict(path=path, major=8, minor=1, inode=inode, start=start,
                    end=start + 0x1000, offset=0, permissions=permissions)

    def loaded_driver(self):
        return self.row(self.driver, inode=123, start=0x9000)

    def validate(self, before, after):
        return probe.validate(before, after, self.driver, self.driver_identity)

    def test_stable_host_zlib_is_recorded_without_global_absence_claim(self):
        baseline = [self.row('/usr/lib/libz.so.1.3.1')]
        evidence = self.validate(baseline, baseline + [self.loaded_driver()])
        self.assertEqual(evidence['baselineAuxiliaryMappings'], baseline)
        self.assertEqual(evidence['afterAuxiliaryMappings'], baseline)
        self.assertEqual(evidence['newSharedAuxiliaryMappings'], [])
        self.assertTrue(evidence['noNewSharedAuxiliaryMappings'])
        self.assertFalse(evidence['sharedCryptoMapped'])
        self.assertNotIn('sharedAuxiliaryMapped', evidence)

    def test_new_auxiliary_is_rejected_even_with_existing_host_zlib(self):
        before = [self.row('/usr/lib/libz.so.1')]
        for name in ('libz.so.2', 'libzstd.so.1', 'libjitterentropy.so.3'):
            with self.subTest(name=name), self.assertRaisesRegex(RuntimeError, 'auxiliary mappings'):
                self.validate(before, before + [self.loaded_driver(), self.row('/other/' + name, inode=42)])

    def test_same_basename_identity_range_path_or_permissions_changes_rejected(self):
        row = self.row('/usr/lib/libz.so.1')
        changes = dict(inode=13, major=9, minor=2, start=0x2000, end=0x3000,
                       offset=0x1000, path='/other/libz.so.1', permissions='r--p')
        for field, value in changes.items():
            changed = dict(row, **{field: value})
            with self.subTest(field=field), self.assertRaisesRegex(RuntimeError, 'auxiliary mappings'):
                self.validate([row], [changed, self.loaded_driver()])

    def test_disappearing_or_deleted_host_auxiliary_rejected(self):
        row = self.row('/usr/lib/libz.so.1')
        with self.assertRaisesRegex(RuntimeError, 'auxiliary mappings'):
            self.validate([row], [self.loaded_driver()])
        for name in ('libz.so.1', 'libz.so'):
            deleted = dict(row, path='/usr/lib/' + name + ' (deleted)')
            with self.subTest(name=name), self.assertRaisesRegex(RuntimeError, 'deleted file'):
                self.validate([deleted], [deleted, self.loaded_driver()])

    def test_shared_crypto_is_rejected_before_and_after_load(self):
        for name in ('libssl.so', 'libssl.so.3', 'libcrypto.so.3', 'libcrypto.so (deleted)'):
            row = self.row('/usr/lib/' + name)
            with self.subTest(name=name, phase='baseline'), self.assertRaisesRegex(RuntimeError, 'shared crypto'):
                self.validate([row], [row, self.loaded_driver()])
            with self.subTest(name=name, phase='load'), self.assertRaisesRegex(RuntimeError, 'shared crypto'):
                self.validate([], [row, self.loaded_driver()])

    def test_missing_preloaded_or_mismatched_driver_is_rejected(self):
        driver = self.loaded_driver()
        with self.assertRaisesRegex(RuntimeError, 'already mapped'):
            self.validate([driver], [driver])
        alias = dict(driver, path='/hardlink/libodbcpp.so')
        with self.assertRaisesRegex(RuntimeError, 'already mapped'):
            self.validate([alias], [driver])
        for rows in ([], [dict(driver, inode=999)], [alias], [dict(driver, permissions='r--p')],
                     [driver, dict(driver, path='/alias/libodbcpp.so', start=0xa000)]):
            with self.subTest(rows=rows), self.assertRaisesRegex(RuntimeError, 'file identity'):
                self.validate([], rows)

    def test_proc_parser_preserves_identity_ranges_and_escaped_paths(self):
        rows = probe.mappings('1000-2000 r-xp 00002000 08:01 123 /path\\040with\\134escape/libodbcpp.so\n'
                              '3000-4000 rw-p 00000000 00:00 0 [heap]\n')
        self.assertEqual(rows, [dict(path='/path with\\escape/libodbcpp.so', major=8, minor=1,
                                     inode=123, start=0x1000, end=0x2000, offset=0x2000,
                                     permissions='r-xp')])

    def test_malformed_empty_duplicate_or_invalid_range_snapshot_rejected(self):
        valid = '1000-2000 r-xp 0 08:01 123 /driver.so\n'
        for text in ('', 'garbage', valid + valid, valid.replace('1000-2000', '2000-1000'),
                     valid.replace('r-xp', 'bad!'), valid.replace('08:01', 'device'),
                     valid.replace('123 /', 'inode /')):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                probe.mappings(text)


if __name__ == '__main__':
    unittest.main()
