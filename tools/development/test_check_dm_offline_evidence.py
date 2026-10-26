#!/usr/bin/env python3
"""Mutation tests for fixed offline checkpoint evidence; no binaries are run."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

_spec = importlib.util.spec_from_file_location(
    'check_dm_offline_evidence', Path(__file__).with_name('check_dm_offline_evidence.py'))
validator = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(validator)


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)
        self.directory = self.build / 'dm-offline'
        self.directory.mkdir()
        self.images = self.build / 'tests'
        self.images.mkdir()
        (self.build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Debug\n')
        self.provider = self.build / 'libodbc.so'
        self.provider.write_bytes(b'fixture provider image')
        for _, image in validator.OWNERS.values():
            (self.images / image).write_bytes(b'fixture image ' + image.encode())
        self.exit = self.directory / 'ctest.exit'
        self.exit.write_text('0\n')
        self.write_ctest()
        for key, expected in validator.CASES.items():
            root = ET.Element('testsuites', tests=str(len(expected)), failures='0', disabled='0')
            suite = ET.SubElement(root, 'testsuite', tests=str(len(expected)), failures='0', disabled='0')
            for classname, name in expected:
                row = ET.SubElement(suite, 'testcase', classname=classname, name=name,
                                    status='run', result='completed')
                if name == 'ActualProviderAndNullBufferRouteShareQualification':
                    properties = ET.SubElement(row, 'properties')
                    values = {'dm_version': '03.52.0002.0003',
                              'dm_loaded_library': str(self.provider),
                              'dm_pointer_bytes': '8', 'dm_application_sqlwchar_bytes': '2',
                              'dm_configured_driver_sqlwchar_bytes': '2', 'dm_odbc_environment': '3',
                              'dm_configuration_provenance':
                              'default_lookup=manager_defined_ini_locations;ODBCINI=unset;ODBCSYSINI=unset;ODBCINSTINI=unset'}
                    for prop, value in values.items():
                        ET.SubElement(properties, 'property', name=prop, value=value)
            ET.ElementTree(root).write(self.directory / (key + '.xml'), encoding='utf-8')
        self.source = {'head': 'fixture-head', 'tree': 'fixture-tree', 'status': '',
                       'tracked_diff_sha256': 'fixture-diff'}
        self.patch = mock.patch.object(validator, 'source_binding', return_value=self.source)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.normalize_times()

    def normalize_times(self):
        for path in self.build.rglob('*'):
            if path.is_file():
                stamp = 2000 if path.suffix == '.xml' else 1000
                os.utime(path, (stamp, stamp))
        os.utime(self.exit, (3000, 3000))

    def write_ctest(self):
        root = ET.Element('testsuite', tests='4', failures='0', disabled='0', skipped='0')
        for name, _ in validator.OWNERS.values():
            ET.SubElement(root, 'testcase', name=name, status='run')
        ET.ElementTree(root).write(self.directory / 'ctest.xml', encoding='utf-8')

    def mutate_xml(self, key, mutation):
        path = self.directory / (key + '.xml')
        root = ET.parse(path).getroot()
        mutation(root)
        ET.ElementTree(root).write(path, encoding='utf-8')
        os.utime(path, (2000, 2000))

    def validate(self, config=None):
        return validator.validate(self.build, self.exit, config)

    def rejected(self, text):
        report = self.validate()
        self.assertFalse(report['passed'])
        self.assertTrue(any(text in error for error in report['errors']), report['errors'])
        return report

    def test_complete_checkpoint_and_current_bindings(self):
        report = self.validate()
        self.assertTrue(report['passed'], report['errors'])
        self.assertEqual(8, sum(len(item['cases']) for item in report['gtest'].values()))
        self.assertEqual(4, len(report['ctest']['cases']))
        self.assertEqual(self.source, report['source'])
        self.assertEqual(4, len(report['images_now']))
        self.assertEqual(64, len(report['observed_provider_now']['sha256']))

    def test_release_executable_layout(self):
        release = self.images / 'Release'
        release.mkdir()
        for path in list(self.images.iterdir()):
            if path.is_file():
                path.rename(release / (path.name + '.exe'))
        report = self.validate('Release')
        self.assertTrue(report['passed'], report['errors'])

    def test_missing_document_does_not_discard_remaining_facts(self):
        (self.directory / 'identity.xml').unlink()
        report = self.rejected('cannot read XML')
        self.assertEqual(1, len(report['gtest']['manager-login-timeout']['cases']))

    def test_empty_and_malformed_xml(self):
        for content in ('', '<testsuites>', '<!DOCTYPE x [<!ENTITY y "z">]><testsuites/>'):
            with self.subTest(content=content):
                (self.directory / 'identity.xml').write_text(content)
                self.rejected('cannot read XML')

    def test_zero_cases_rejected_even_with_zero_aggregates(self):
        (self.directory / 'identity.xml').write_text('<testsuites tests="0" failures="0"/>')
        self.rejected('case inventory mismatch')

    def test_wrong_case_rejected(self):
        self.mutate_xml('identity', lambda root: next(root.iter('testcase')).set('name', 'Substitute'))
        self.rejected('case inventory mismatch')

    def test_duplicate_case_rejected_even_with_expected_count(self):
        def duplicate(root):
            rows = list(root.iter('testcase'))
            rows[1].set('name', rows[0].get('name'))
        self.mutate_xml('identity', duplicate)
        self.rejected('case inventory mismatch')

    def test_wrong_suite_rejected(self):
        self.mutate_xml('direct-login-timeout', lambda root: next(root.iter('testcase')).set('classname', 'Other'))
        self.rejected('case inventory mismatch')

    def test_notrun_and_suppressed_and_missing_status_rejected(self):
        path = self.directory / 'direct-login-timeout.xml'
        original = path.read_bytes()
        for attribute, value in (('status', 'notrun'), ('result', 'suppressed'), ('result', 'skipped')):
            with self.subTest(attribute=attribute, value=value):
                path.write_bytes(original)
                self.mutate_xml('direct-login-timeout', lambda root: next(root.iter('testcase')).set(attribute, value))
                self.rejected('not run/completed')
        self.mutate_xml('manager-login-timeout', lambda root: next(root.iter('testcase')).attrib.pop('status'))
        self.rejected('not run/completed')

    def test_failure_error_and_skip_nodes_rejected(self):
        path = self.directory / 'identity.xml'
        original = path.read_bytes()
        for tag in ('failure', 'error', 'skipped'):
            with self.subTest(tag=tag):
                path.write_bytes(original)
                self.mutate_xml('identity', lambda root: ET.SubElement(next(root.iter('testcase')), tag))
                self.rejected('failure/error/skipped')

    def test_aggregate_failure_and_wrong_counts_rejected(self):
        self.mutate_xml('identity', lambda root: root.set('disabled', '1'))
        self.rejected('suite disabled')
        self.mutate_xml('ctest', lambda root: root.set('tests', '0'))
        self.rejected('tests count disagrees')

    def test_ctest_substitution_and_duplicate_rejected(self):
        self.mutate_xml('ctest', lambda root: root[0].set('name', 'test_dm_identity'))
        self.rejected('ctest: case inventory mismatch')
        self.write_ctest()
        self.mutate_xml('ctest', lambda root: root[1].set('name', root[0].get('name')))
        self.rejected('ctest: case inventory mismatch')

    def test_ctest_notrun_rejected(self):
        self.mutate_xml('ctest', lambda root: root[0].set('status', 'notrun'))
        self.rejected('CTest case was not run/completed')

    def test_failed_exit_preserves_all_xml_facts(self):
        self.exit.write_text('8\n')
        report = self.rejected('actual CTest exit is not zero')
        self.assertEqual(8, report['ctest_exit'])
        self.assertEqual(5, len(report['gtest']['identity']['cases']))

    def test_missing_invalid_and_oversized_exit_rejected(self):
        for value in ('', '0 then 1', '0' * 65):
            with self.subTest(value=value):
                self.exit.write_text(value)
                self.rejected('actual CTest exit unavailable')
        self.exit.unlink()
        self.rejected('actual CTest exit unavailable')

    def test_missing_empty_and_ambiguous_image_rejected(self):
        image = self.images / 'test_dm_identity'
        image.unlink()
        self.rejected('missing or ambiguous owning image')
        image.touch()
        self.rejected('empty owning image')
        image.with_suffix('.exe').write_bytes(b'second')
        self.rejected('missing or ambiguous owning image')

    def test_stale_xml_and_xml_newer_than_exit_rejected(self):
        image = self.images / 'test_dm_identity'
        os.utime(image, (2500, 2500))
        self.rejected('XML predates')
        os.utime(image, (1000, 1000))
        os.utime(self.directory / 'identity.xml', (4000, 4000))
        self.rejected('XML postdates')

    def test_coarse_timestamp_tolerance(self):
        os.utime(self.images / 'test_dm_identity', (2001, 2001))
        self.assertTrue(self.validate()['passed'])

    def test_missing_provider_observation_rejected(self):
        self.mutate_xml('identity', lambda root: list(root.iter('properties'))[0].clear())
        self.rejected('provider binding failed')

    def test_provider_wrong_profile_rejected(self):
        def change(root):
            for prop in root.iter('property'):
                if prop.get('name') == 'dm_odbc_environment':
                    prop.set('value', '2')
        self.mutate_xml('identity', change)
        self.rejected('invalid observed/configured ABI')

    def test_missing_provider_file_rejected(self):
        self.provider.unlink()
        self.rejected('absolute regular file')

    def test_missing_configuration_preserves_test_facts(self):
        (self.build / 'CMakeCache.txt').unlink()
        report = self.rejected('configuration binding failed')
        self.assertEqual(5, len(report['gtest']['identity']['cases']))

    def test_source_binding_failure_preserves_test_facts(self):
        with mock.patch.object(validator, 'source_binding', side_effect=OSError('unavailable')):
            report = self.rejected('source binding failed')
        self.assertEqual(4, len(report['ctest']['cases']))

    def test_config_path_escape_rejected(self):
        report = self.validate('../outside')
        self.assertFalse(report['passed'])
        self.assertIn('config must be a single directory name', report['errors'])

    def test_cli_writes_failure_report_and_preserves_existing_report(self):
        (self.directory / 'identity.xml').unlink()
        argv = ['validator', '--build', str(self.build), '--ctest-exit-file', str(self.exit)]
        with mock.patch('sys.argv', argv), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(1, validator.main())
            saved = (self.directory / 'evidence.json').read_bytes()
            self.assertFalse(json.loads(saved)['passed'])
            with self.assertRaises(FileExistsError):
                validator.main()
            self.assertEqual(saved, (self.directory / 'evidence.json').read_bytes())


if __name__ == '__main__':
    unittest.main()
