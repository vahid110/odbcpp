"""Offline distro-metadata, real CMake binding and inspector rejection controls."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('static_inputs', Path(__file__).with_name('prepare-openssl-static-inputs.py'))
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)


class StaticInputsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.prefix = self.root / 'controlled'
        (self.prefix / 'lib').mkdir(parents=True)
        self.ssl, self.crypto, self.aux = (self.prefix / 'lib' / name for name in
                                          ('libssl.a', 'libcrypto.a', 'libjitterentropy.a'))
        for path in (self.ssl, self.crypto, self.aux):
            path.write_bytes(b'synthetic archive')

    def test_closed_static_metadata(self):
        self.assertEqual(p.static_names('-lssl -lcrypto -l:libjitterentropy.a -lz -lzstd -ldl -pthread',
                                         Path('/usr/lib/x86_64-linux-gnu')),
                         ['ssl', 'crypto', ':libjitterentropy.a', 'z', 'zstd'])

    def test_unknown_dependency_or_flag_refused(self):
        for flag in ('-lunknown', '-Wl,--unresolved-symbols=ignore-all', '-L/unowned', '-lz;bad'):
            with self.subTest(flag=flag), self.assertRaisesRegex(RuntimeError, 'Unrecognized'):
                p.static_names('-lssl -lcrypto ' + flag, Path('/usr/lib/x86_64-linux-gnu'))

    def test_existing_prefix_or_evidence_refused_before_commands(self):
        with patch.object(p, 'command', side_effect=AssertionError('must not query existing state')):
            with self.assertRaisesRegex(RuntimeError, 'Preserve existing'):
                p.prepare(self.prefix, self.root / 'evidence.json')

    def test_absent_required_archive_refused_before_package_query(self):
        with patch.object(p, 'command', side_effect=AssertionError('must not query absent file')):
            with self.assertRaisesRegex(RuntimeError, 'Missing regular'):
                p.package_file(self.root / 'missing.a', 'libjitterentropy3-dev')

    def test_package_ownership_and_version(self):
        with patch.object(p, 'command', side_effect=[f'libjitterentropy3-dev:amd64: {self.aux}', '3.7.0']):
            self.assertEqual(p.package_file(self.aux, 'libjitterentropy3-dev'),
                             ('libjitterentropy3-dev:amd64', '3.7.0'))
        for owner in ('foreign-dev', 'libjitterentropy3-dev:amd64: /other'):
            row = owner if ': /' in owner else f'{owner}: {self.aux}'
            with patch.object(p, 'command', return_value=row), self.assertRaises(RuntimeError):
                p.package_file(self.aux, 'libjitterentropy3-dev')

    def configure(self, dependency=':libjitterentropy.a'):
        source = self.root / 'source'
        source.mkdir()
        source.joinpath('CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(static_controls NONE)
set(CMAKE_SYSTEM_NAME Linux)
set(ODBCPP_CRYPTO_LINKAGE BUNDLED_STATIC CACHE STRING "")
set(ODBCPP_CRYPTO_ROOT "{self.prefix.as_posix()}" CACHE PATH "")
include("{ROOT.as_posix()}/cmake/CryptoProfile.cmake")
set(CMAKE_DL_LIBS dl)
add_library(OpenSSL::Crypto INTERFACE IMPORTED)
add_library(OpenSSL::SSL INTERFACE IMPORTED)
set_property(TARGET OpenSSL::Crypto PROPERTY INTERFACE_LINK_LIBRARIES "{dependency};dl;Threads::Threads")
set_property(TARGET OpenSSL::SSL PROPERTY INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
set(OPENSSL_VERSION 3.5.5)
set(OPENSSL_SSL_LIBRARY "{self.ssl.as_posix()}")
set(OPENSSL_CRYPTO_LIBRARY "{self.crypto.as_posix()}")
set(OPENSSL_INCLUDE_DIR "{self.prefix.as_posix()}/include")
include("{ROOT.as_posix()}/cmake/GenerateCryptoManifest.cmake")
get_target_property(bound OpenSSL::Crypto INTERFACE_LINK_LIBRARIES)
file(WRITE "${{CMAKE_BINARY_DIR}}/bound.txt" "${{bound}}")
''')
        return subprocess.run([shutil.which('cmake'), '-S', str(source), '-B', str(self.root / 'build')],
                              capture_output=True, text=True, timeout=30)

    def test_real_configure_binds_absolute_auxiliary_and_manifest(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        manifest = json.loads((self.root / 'build/odbcpp-crypto-manifest.json').read_text())
        self.assertEqual(manifest['auxiliaryStaticLinkInputs'],
                         [{'path': self.aux.as_posix(), 'sha256': p.sha(self.aux)}])
        self.assertEqual((self.root / 'build/bound.txt').read_text(), self.aux.as_posix() + ';dl;Threads::Threads')

    def test_real_configure_missing_auxiliary_refused(self):
        self.aux.unlink()
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Missing regular controlled static dependency', result.stderr)

    def test_real_configure_unknown_interface_refused(self):
        result = self.configure('unrecognized')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Unrecognized static OpenSSL dependency', result.stderr)

    def test_real_configure_compression_inputs_are_controlled(self):
        for name in ('libz.a', 'libzstd.a'):
            (self.prefix / 'lib' / name).write_bytes(b'synthetic compression archive')
        result = self.configure('ZLIB::ZLIB;zstd')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        manifest = json.loads((self.root / 'build/odbcpp-crypto-manifest.json').read_text())
        self.assertEqual({Path(entry['path']).name for entry in manifest['auxiliaryStaticLinkInputs']},
                         {'libz.a', 'libzstd.a'})

    def inspect(self, mode='valid'):
        artifact = self.root / 'driver.so'
        artifact.write_bytes(b'synthetic driver')
        entry = {'path': self.aux.as_posix(), 'sha256': p.sha(self.aux)}
        if mode == 'outside':
            outside = self.root / 'libjitterentropy.a'
            outside.write_bytes(self.aux.read_bytes())
            entry['path'] = outside.as_posix()
        if mode == 'hash':
            entry['sha256'] = '0' * 64
        manifest = {'provider': 'OPENSSL', 'requestedLinkage': 'BUNDLED_STATIC',
                    'dependencyRoot': self.prefix.as_posix(), 'sslLinkInput': self.ssl.as_posix(),
                    'cryptoLinkInput': self.crypto.as_posix(), 'auxiliaryStaticLinkInputs': [entry]}
        if mode == 'duplicate':
            manifest['auxiliaryStaticLinkInputs'].append(entry)
        (self.root / 'manifest.json').write_text(json.dumps(manifest))
        paths = [self.ssl, self.crypto] + ([] if mode == 'missing-load' else [self.aux])
        link_map = f'OUTPUT({artifact.as_posix()} elf64-x86-64)\n'
        for path in paths:
            link_map += 'LOAD ' + path.as_posix() + '\n'
            if mode != 'unused-aux' or path != self.aux:
                link_map += path.as_posix() + '(synthetic.o)\n'
        (self.root / 'link.map').write_text(link_map)
        return subprocess.run([shutil.which('cmake'), '-DEXPECTED_PROVIDER=OPENSSL',
                               '-DEXPECTED_LINKAGE=BUNDLED_STATIC', '-DPLATFORM=Linux',
                               '-DINSPECTION_OUTPUT=NEEDED ' + ('libjitterentropy.so.3' if mode == 'dynamic' else 'libc.so.6'),
                               '-DSTATIC_LINKER_IDENTITY=GNU ld (synthetic)',
                               '-DARTIFACT=' + artifact.as_posix(),
                               '-DCONFIG_MANIFEST=' + (self.root / 'manifest.json').as_posix(),
                               '-DSTATIC_LINK_MAP=' + (self.root / 'link.map').as_posix(),
                               '-DEVIDENCE=' + (self.root / 'artifact.json').as_posix(),
                               '-P', str(ROOT / 'cmake/InspectCryptoArtifact.cmake')],
                              capture_output=True, text=True, timeout=30)

    def test_real_inspector_binds_hash_load_and_member(self):
        result = self.inspect()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        evidence = json.loads((self.root / 'artifact.json').read_text())
        self.assertTrue(evidence['staticArchiveMemberEvidenceVerified'])
        self.assertEqual(evidence['auxiliaryStaticLinkInputs'][0],
                         {'path': self.aux.as_posix(), 'sha256': p.sha(self.aux),
                          'linkMapLoadVerified': True, 'archiveMembersObserved': True})

    def test_real_inspector_distinguishes_declared_unused_archive(self):
        result = self.inspect('unused-aux')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(json.loads((self.root / 'artifact.json').read_text())['auxiliaryStaticLinkInputs'][0]['archiveMembersObserved'])

    def test_real_inspector_missing_load_tamper_outside_or_duplicate_refused(self):
        for mode in ('missing-load', 'hash', 'outside', 'duplicate', 'dynamic'):
            with self.subTest(mode=mode):
                result = self.inspect(mode)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
