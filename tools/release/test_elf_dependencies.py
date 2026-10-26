"""Synthetic parser/control fixtures: these do not qualify real Linux binaries."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from elf_dependencies import MANIFEST, numeric, parse_needs, parse_dynamic, scan

PROFILE = 'linux-ubuntu22.04-x86-64-unixodbc-ucs2-openssl-system-shared'


def needs(*versions):
    return ("Version definition section '.gnu.version_d' contains 1 entry:\n"
            "  0x001c: Name: GLIBCXX_99.99\n"
            "Version needs section '.gnu.version_r' contains 1 entry:\n"
            f"  0x0000: Version: 1 File: libc.so.6 Cnt: {len(versions)}\n" +
            ''.join(f'  0x0010: Name: {version} Flags: none Version: 2\n' for version in versions))


def elf(machine=62):
    data = bytearray(64)
    data[:7] = b'\x7fELF\x02\x01\x01'
    data[16:18] = (3).to_bytes(2, 'little')
    data[18:20] = machine.to_bytes(2, 'little')
    return data


DYNAMIC = 'Dynamic section at offset 0x10 contains 2 entries:\n 0x1 (NEEDED) Shared library: [libc.so.6]\n 0x0 (NULL) 0x0\n'


class ElfControls(unittest.TestCase):
    def test_numeric_boundaries_not_lexicographic(self):
        self.assertGreater(numeric('2.10'), numeric('2.9'))
        self.assertEqual(numeric('2.34.0'), numeric('2.34'))
        for invalid in ('PRIVATE', '2', '2.34x', '', None):
            with self.assertRaises(ValueError):
                numeric(invalid)

    def test_parse_requirements_not_exported_definitions(self):
        required, other = parse_needs(needs('GLIBC_2.34', 'GLIBCXX_3.4.29', 'CXXABI_1.3.13', 'OPENSSL_3.0.0'))
        self.assertEqual(required['GLIBCXX'], ['3.4.29'])
        self.assertEqual(other, ['OPENSSL_3.0.0'])
        self.assertEqual(parse_dynamic(DYNAMIC), ['libc.so.6'])
        self.assertEqual(parse_needs('No version information found in this file.')[0]['GLIBC'], [])

    def test_unknown_or_truncated_output_cannot_pass(self):
        for invalid in ('', 'translated tool output', "Version needs section '.gnu.version_r' contains 1 entry:\n", needs('GLIBC_2.34').replace('Cnt: 1', 'Cnt: 2'),
                        needs('GLIBC_PRIVATE'), needs('GLIBC_ABI_DT_RELR'), needs('CXXABI_TM_1')):
            with self.subTest(output=invalid), self.assertRaises(ValueError):
                parse_needs(invalid)
        with self.assertRaises(ValueError):
            parse_dynamic('')

    def test_dynamic_truncation_and_malformed_needed_fail(self):
        for text in ('Dynamic section at offset 0x10 contains 2 entries:\n',
                     DYNAMIC.replace('Shared library: [libc.so.6]', 'unrecognized value'),
                     DYNAMIC.replace('2 entries', '3 entries'),
                     DYNAMIC.replace(' 0x0 (NULL) 0x0\n', '')):
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_dynamic(text)
        self.assertEqual(parse_dynamic('Dynamic section at offset 0x10 contains 1 entry:\n 0x0 (NULL) 0x0\n'), [])
        self.assertEqual(parse_dynamic('There is no dynamic section in this file.'), [])

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stage = self.root / 'stage'
        self.stage.mkdir()
        (self.stage / 'driver.so').write_bytes(elf())
        (self.stage / 'bundled').mkdir()
        (self.stage / 'bundled/libhelper.so').write_bytes(elf())
        self.manifest = self.root / 'profiles.json'
        self.policy = json.loads(MANIFEST.read_text())
        self.profile = self.policy['profiles'][PROFILE]
        # Explicit synthetic baseline, never written to the shipping contract.
        self.profile['maxRequiredSymbolVersions'] = {'GLIBC': '2.35', 'GLIBCXX': '3.4.29', 'CXXABI': '1.3.13'}
        self.manifest.write_text(json.dumps(self.policy))

    def run_scan(self, helper='GLIBC_2.35'):
        def inspect(tool, option, path):
            if option == '--dynamic':
                return DYNAMIC
            return needs(helper if path.name == 'libhelper.so' else 'GLIBC_2.34', 'GLIBCXX_3.4.29', 'CXXABI_1.3.13')
        with mock.patch('elf_dependencies.readelf', side_effect=inspect):
            return scan(self.stage, PROFILE, self.manifest)

    def test_all_shipped_libraries_boundary_and_too_new(self):
        passing = self.run_scan()
        self.assertEqual(passing['status'], 'pass')
        self.assertFalse(passing['releaseQualified'])
        self.assertEqual(len(passing['artifacts']), 2)
        for too_new in ('GLIBC_2.36', 'GLIBCXX_3.4.30', 'CXXABI_1.3.14'):
            result = self.run_scan(too_new)
            self.assertEqual(result['status'], 'fail')
            self.assertIn('exceeds', ' '.join(result['errors']))

    def test_missing_baseline_is_pending_even_without_cpp_requirement(self):
        self.profile['maxRequiredSymbolVersions']['GLIBCXX'] = None
        self.manifest.write_text(json.dumps(self.policy))
        self.assertEqual(self.run_scan()['status'], 'pending-baseline')
        self.assertEqual(self.run_scan('GLIBC_2.36')['status'], 'fail')

    def test_wrong_architecture_no_elf_and_tool_failure(self):
        (self.stage / 'driver.so').write_bytes(elf(183))
        self.assertEqual(self.run_scan()['status'], 'fail')
        with mock.patch('elf_dependencies.readelf', side_effect=FileNotFoundError('missing readelf')):
            self.assertEqual(scan(self.stage, PROFILE, self.manifest)['status'], 'fail')
        for path in self.stage.rglob('*.so'):
            path.write_bytes(b'not ELF')
        self.assertEqual(self.run_scan()['status'], 'fail')

    def test_escape_rejected_and_in_stage_alias_inventoried(self):
        link = self.stage / 'alias.so'
        link.symlink_to('driver.so')
        self.assertEqual(len(self.run_scan()['artifacts']), 3)
        link.unlink()
        link.symlink_to(self.manifest)
        with self.assertRaises(ValueError):
            self.run_scan()

    def test_contract_does_not_invent_qualification(self):
        policy = json.loads(MANIFEST.read_text())
        self.assertFalse(policy['qualificationClaimed'])
        for profile in policy['profiles'].values():
            self.assertEqual(profile['status'], 'proposed-unqualified')
            self.assertIn(profile['driverSqlwcharBytes'], (2, 4))
            self.assertEqual(profile['driverSqlwcharBytes'], profile['driverManager']['sqlwcharBytes'])
            self.assertIsNone(profile['driverManager']['minimumVersion'])
            if profile['osFamily'] == 'Linux':
                self.assertIsNone(profile['maxRequiredSymbolVersions']['GLIBCXX'])
                self.assertIsNone(profile['maxRequiredSymbolVersions']['CXXABI'])
            else:
                self.assertIsNone(profile['minimumOsVersion'])


if __name__ == '__main__':
    unittest.main()
