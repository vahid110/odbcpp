"""Synthetic offline XML adversaries; never native endpoint evidence."""
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

import mysql_datetime_receipt_xml as validator


def fixture(*, precision=6, width=104, charset=45):
    # Same emitter structure, synthetic identity/properties only.
    root = ET.Element('testsuites', tests='1', failures='0', errors='0', disabled='0', name='AllTests', time='0.', timestamp='2026-10-04T09:08:30.845')
    suite = ET.SubElement(root, 'testsuite', name=validator.SUITE, tests='1', failures='0', errors='0', disabled='0', skipped='0')
    case = ET.SubElement(suite, 'testcase', name=validator.CASE, classname=validator.SUITE, status='run', result='completed', file='synthetic_fixture.cpp', line='123')
    properties = ET.SubElement(case, 'properties')
    # Independent literal inventory so schema changes cannot silently alter fixtures.
    values = {
        'observation_schema': 2, 'prepare_parameter_count': 1,
        'prepare_result_count': 2, 'observed_parameter_count': 1,
        'records_truncated': 0, 'exchange_complete': 0,
        'transport_complete_calls': 1, 'prepare_command_count': 1,
        'execute_command_count': 0, 'close_command_count': 0,
        'raw_parameter_0_native_type': 12, 'runtime_retired': 1,
        'physical_close_delta': 1, 'parameter_support_claimed': 0,
    }
    values.update(raw_parameter_0_charset=charset, raw_parameter_0_byte_width=width,
                  raw_parameter_0_decimals=precision,
                  refusal_policy_code=3)
    for name, value in values.items():
        ET.SubElement(properties, 'property', name=name, value=str(value))
    return root


def serialized(root):
    return b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding='utf-8')


class DatetimeReceiptXmlTest(unittest.TestCase):
    def invalid(self, raw):
        with self.assertRaisesRegex(RuntimeError, '^' + validator.ERROR + '$') as result:
            validator.validate_datetime_receipt_xml_bytes(raw)
        self.assertTrue(result.exception.__suppress_context__)

    def test_revision2_accepts_only_qualified_actual_fields_without_old_policy_fallback(self):
        raw = serialized(fixture())
        result = validator.validate_datetime_receipt_xml_bytes(raw)
        self.assertEqual(2, result['schemaVersion']);self.assertEqual(3, result['refusalPolicyCode'])
        self.assertEqual(hashlib.sha256(raw).hexdigest(), result['xmlSha256'])
        self.assertFalse(result['exchangeComplete']);self.assertFalse(result['parameterSupportClaimed'])
        for precision in (0, 3, 6, 7, 255):
            for width in (0, 19, 23, 26, 76, 92, 104, 4294967295):
                for charset in (0, 45, 63, 65535):
                    if (precision, width, charset) == (6, 104, 45):
                        continue
                    with self.subTest(precision=precision, width=width, charset=charset):
                        self.invalid(serialized(fixture(precision=precision, width=width, charset=charset)))

    def test_root_skipped_optional_suite_skipped_mandatory_and_property_order_unimportant(self):
        root = fixture();root.set('skipped', '0')
        properties = root[0][0][0];properties[:] = list(reversed(properties))
        validator.validate_datetime_receipt_xml_bytes(serialized(root))
        del root[0].attrib['skipped'];self.invalid(serialized(root))
        self.invalid(serialized(fixture()).replace(b'<testsuites ', b'<testsuites skipped="1" '))

    def test_every_inventory_counter_identity_and_status_is_required_and_exact(self):
        for scope, keys in ((0, ('tests', 'failures', 'errors', 'disabled')),
                            (1, ('name', 'tests', 'failures', 'errors', 'disabled', 'skipped')),
                            (2, ('name', 'classname', 'status', 'result'))):
            for key in keys:
                for value in (None, 'wrong', '01', '-1'):
                    root = fixture();node = root if scope == 0 else (root[0] if scope == 1 else root[0][0])
                    if value is None:
                        del node.attrib[key]
                    else:
                        node.set(key, value)
                    with self.subTest(scope=scope, key=key, value=value):
                        self.invalid(serialized(root))

    def test_all_eighteen_properties_missing_duplicate_extra_or_wrong_exact_value_fail(self):
        for key in validator.EXACT.keys() | validator.RANGES.keys():
            for mode in ('missing', 'duplicate', 'renamed'):
                root = fixture();properties = root[0][0][0]
                node = next(node for node in properties if node.get('name') == key)
                if mode == 'missing':
                    properties.remove(node)
                elif mode == 'duplicate':
                    ET.SubElement(properties, 'property', **node.attrib)
                else:
                    node.set('name', 'unreviewed')
                with self.subTest(key=key, mode=mode):
                    self.invalid(serialized(root))
        for key, value in validator.EXACT.items():
            root = fixture();node = next(n for n in root[0][0][0] if n.get('name') == key)
            node.set('value', str(1 if value == 0 else 0));self.invalid(serialized(root))

    def test_numeric_canonicality_ranges_and_classifier_consistency(self):
        for bad in ('', '+1', '-1', ' 1', '1 ', '01', '1e0', '1.0', '١', '１', '9' * 1000):
            root = fixture();root[0][0][0][0].set('value', bad);self.invalid(serialized(root))
        for key, maximum in validator.RANGES.items():
            root = fixture();node = next(n for n in root[0][0][0] if n.get('name') == key)
            node.set('value', str(maximum + 1));self.invalid(serialized(root))
        for precision, width, wrong in ((6, 104, 1), (6, 104, 2), (3, 92, 2), (6, 26, 3)):
            root = fixture(precision=precision, width=width)
            node = next(n for n in root[0][0][0] if n.get('name') == 'refusal_policy_code')
            node.set('value', str(wrong));self.invalid(serialized(root))

    def test_property_placement_duplicate_attributes_and_non_emitter_children(self):
        for mode in range(10):
            root = fixture();case = root[0][0];props = case[0]
            if mode == 0:
                case.remove(props);root.append(props)
            elif mode == 1:
                case.remove(props);root[0].append(props)
            elif mode == 2:
                case.append(ET.fromstring(ET.tostring(props)))
            elif mode == 3:
                props.set('extra', '1')
            elif mode == 4:
                props[0].set('extra', '1')
            elif mode == 5:
                ET.SubElement(props[0], 'property', name='x', value='1')
            elif mode == 6:
                props[0].text = ' '
            elif mode == 7:
                case.text = 'private text'
            elif mode == 8:
                props[0].tail = 'private tail'
            else:
                case.set('observation_schema', '1')
            self.invalid(serialized(root))
        raw = serialized(fixture())
        self.invalid(raw.replace(b'name="observation_schema"', b'name="observation_schema" name="other"'))
        for tag in ('failure', 'error', 'skipped', 'system-out', 'testcase', 'unreviewed'):
            root = fixture();ET.SubElement(root[0][0], tag);self.invalid(serialized(root))
        for scope in (0, 1, 2):
            root = fixture();node = root if scope == 0 else (root[0] if scope == 1 else root[0][0])
            node.set('unreviewed', '0');self.invalid(serialized(root))

    def test_namespace_dtd_entity_comment_pi_encoding_and_escape_adversaries(self):
        raw = serialized(fixture())
        attacks = [raw.replace(b'<testsuites ', b'<testsuites xmlns="urn:other" '),
                   raw.replace(b'<testsuites ', b'<testsuites xmlns:x="urn:other" '),
                   raw.replace(b'<testsuites ', b'<x:testsuites ').replace(b'</testsuites>', b'</x:testsuites>'),
                   raw.replace(b'value="1"', b'value="&#49;"', 1),
                   raw.replace(b'value="1"', b'value="&#x31;"', 1),
                   raw.replace(b'value="1"', b'value="&custom;"', 1),
                   raw.replace(b'UTF-8', b'UTF-16'),raw.decode().encode('utf-16'),b'\xff',b'',b'<testsuites>\0</testsuites>',
                   raw.replace(b'?>', b'?><!-- hidden -->', 1),
                   raw.replace(b'?>', b'?><?reviewed nope?>', 1),
                   raw.replace(b'<properties>', b'<properties><![CDATA[ ]]>', 1)]
        for doctype in (b'<!DOCTYPE testsuites>',
                        b'<!DOCTYPE testsuites [<!ENTITY custom "1">]>',
                        b'<!DOCTYPE testsuites SYSTEM "file:///private">',
                        b'<!DOCTYPE testsuites [<!ENTITY ext SYSTEM "https://invalid.example/">]>'):
            attacks.append(raw.replace(b'?>', b'?>' + doctype, 1))
        for attack in attacks:
            self.invalid(attack)
        root = fixture();root[0][0].set('file', 'synthetic&file.cpp')
        validator.validate_datetime_receipt_xml_bytes(serialized(root))
        validator.validate_datetime_receipt_xml_bytes(b'\xef\xbb\xbf' + raw)

    def test_aggregate_tree_depth_count_listing_and_resource_boundaries(self):
        raw = serialized(fixture())
        validator.validate_datetime_receipt_xml_bytes(raw + b' ' * (validator.MAX_XML_BYTES - len(raw)))
        self.invalid(raw + b' ' * (validator.MAX_XML_BYTES - len(raw) + 1))
        self.invalid(raw + raw)
        self.invalid(ET.tostring(fixture()[0]))
        self.invalid(b'<testsuites tests="1"><testsuite name="' + validator.SUITE.encode() + b'" tests="1"><testcase name="' + validator.CASE.encode() + b'"/></testsuite></testsuites>')
        root = fixture();root.append(ET.fromstring(ET.tostring(root[0])));self.invalid(serialized(root))
        root = fixture();root[0].append(ET.fromstring(ET.tostring(root[0][0])));self.invalid(serialized(root))
        self.invalid(b'<testsuites>' * 6 + b'</testsuites>' * 6)
        self.invalid(b'<testsuites>' + b'<property/>' * 23 + b'</testsuites>')
        root = fixture();root[0][0].set('file', 'x' * 1025);self.invalid(serialized(root))
        for key, value in (('time', 'NaN'), ('time', '-1.'), ('line', '0'), ('line', '4294967296'), ('timestamp', 'arbitrary')):
            root = fixture();root[0][0].set(key, value);self.invalid(serialized(root))

    def test_path_read_is_bounded_hash_is_same_bytes_and_results_are_owning(self):
        raw = serialized(fixture())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'synthetic.xml';path.write_bytes(raw)
            result = validator.validate_datetime_receipt_xml(path)
            path.write_bytes(b'changed');self.assertEqual(hashlib.sha256(raw).hexdigest(), result['xmlSha256'])
            result['properties']['raw_parameter_0_charset'] = 0
            fresh = validator.validate_datetime_receipt_xml_bytes(raw);self.assertEqual(45, fresh['properties']['raw_parameter_0_charset'])
            for invalid in (Path(directory), Path(directory) / 'absent'):
                with self.assertRaisesRegex(RuntimeError, '^' + validator.ERROR + '$'):
                    validator.validate_datetime_receipt_xml(invalid)
            path.write_bytes(b' ' * (validator.MAX_XML_BYTES + 1))
            with self.assertRaisesRegex(RuntimeError, '^' + validator.ERROR + '$'):
                validator.validate_datetime_receipt_xml(path)
        self.invalid(bytearray(raw))

    def test_only_xml_ascii_whitespace_is_permitted_between_elements(self):
        raw = serialized(fixture())
        for whitespace in (' ', '\t', '\r', '\n', ' \t\r\n'):
            validator.validate_datetime_receipt_xml_bytes(raw.replace(
                b'<properties>', b'<properties>' + whitespace.encode('utf-8'), 1))
        for whitespace in ('\u00a0', '\u0085', '\u1680', '\u2000', '\u2003',
                           '\u2028', '\u2029', '\u202f', '\u205f', '\u3000'):
            for boundary in (b'<testsuites ', b'<properties>', b'</properties>'):
                with self.subTest(whitespace=repr(whitespace), boundary=boundary):
                    if boundary == b'<testsuites ':
                        attack = raw.replace(b'?>', b'?>' + whitespace.encode('utf-8'), 1)
                    else:
                        attack = raw.replace(boundary, boundary + whitespace.encode('utf-8'), 1)
                    self.invalid(attack)

    def test_path_reader_uses_single_capped_read_and_rejects_nonregular_files(self):
        raw = serialized(fixture())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'synthetic.xml'
            path.write_bytes(raw)
            actual_fdopen = validator.os.fdopen
            reads = []

            class ObservedFile:
                def __init__(self, stream):
                    self.stream = stream

                def __enter__(self):
                    return self

                def __exit__(self, *args):
                    return self.stream.__exit__(*args)

                def fileno(self):
                    return self.stream.fileno()

                def read(self, size):
                    reads.append(size)
                    return self.stream.read(size)

            def observed_fdopen(*args):
                return ObservedFile(actual_fdopen(*args))

            with patch.object(validator.os, 'fdopen', side_effect=observed_fdopen):
                result = validator.validate_datetime_receipt_xml(path)
            self.assertEqual([16385], reads)
            self.assertEqual(hashlib.sha256(raw).hexdigest(), result['xmlSha256'])
            if hasattr(validator.os, 'mkfifo'):
                fifo = Path(directory) / 'not-a-regular-file'
                validator.os.mkfifo(fifo)
                with self.assertRaisesRegex(RuntimeError, '^' + validator.ERROR + '$'):
                    validator.validate_datetime_receipt_xml(fifo)

    def test_expat_split_callbacks_preserve_complete_attributes_and_text_policy(self):
        actual_create = validator.expat.ParserCreate

        class ChunkedParser:
            def __init__(self, parser, chunk):
                object.__setattr__(self, 'parser', parser)
                object.__setattr__(self, 'chunk', chunk)

            def __setattr__(self, name, value):
                setattr(self.parser, name, value)

            def Parse(self, text, final):
                for offset in range(0, len(text), self.chunk):
                    self.parser.Parse(text[offset:offset + self.chunk], False)
                return self.parser.Parse('', final)

        root = fixture()
        root[0][0].set('file', "synthetic-é-&-<->-\"-'.cpp")
        raw = serialized(root).replace(b'<properties>', b'<properties> \t\r\n', 1)
        expected = validator.validate_datetime_receipt_xml_bytes(raw)
        invalid = (
            raw.replace(b'<properties>', b'<properties>\xc2\xa0', 1),
            raw.replace(b'<properties>', b'<properties>private', 1),
            raw.replace(b'value="1"', b'value="&#49;"', 1),
            raw.replace(b'value="1"', b'value="01"', 1),
            raw.replace(b'value="1"', b'value="&custom;"', 1),
        )
        for chunk, buffered in ((n, b) for n in (1, 2, 3, 7, 31) for b in (False, True)):
            with self.subTest(chunk=chunk, buffered=buffered):
                def create(*args, **kwargs):
                    parser = actual_create(*args, **kwargs)
                    parser.buffer_text = buffered
                    return ChunkedParser(parser, chunk)

                with patch.object(validator.expat, 'ParserCreate', side_effect=create):
                    self.assertEqual(expected, validator.validate_datetime_receipt_xml_bytes(raw))
                    for attack in invalid:
                        self.invalid(attack)

    def test_bundled_emitter_auxiliary_attributes_and_narrow_escape_limit(self):
        root = fixture()
        for node in (root, root[0], root[0][0]):
            node.set('time', '0.012')
            node.set('timestamp', '2026-10-04T09:08:30.845')
        root[0][0].set('file', "synthetic-é-&-<->-\"-'.cpp")
        raw = serialized(root)
        result = validator.validate_datetime_receipt_xml_bytes(raw)
        self.assertEqual(validator.SUITE, result['suite'])
        self.assertNotIn('file', result)
        # GoogleTest escapes tab/newline in file attributes as numeric references.
        # Accepted narrow-format policy refuses those uncommon paths unchanged.
        for reference in (b'&#x09;', b'&#x0A;', b'&#x0D;'):
            self.invalid(raw.replace(b'synthetic-', b'synthetic-' + reference, 1))

    def test_actual_synthetic_publisher_inventory_is_not_native_authority(self):
        # Two synthetic publisher cases cannot be substituted for exact native1.
        root = fixture();root[0].set('name', 'MySqlDatetimeReceiptPropertyShapeTest')
        root[0][0].set('name', 'SyntheticOwnedRecordEmitsExactCanonicalNumericProperties')
        self.invalid(serialized(root))

    def test_historical_v1_native_bytes_hash_and_old_case_are_not_reinterpreted(self):
        self.assertEqual('fdff65252b7c1ae959c9f7052c4a3c116e1fb368fa94ca7e0bfdd11ddf40e89f',
                         hashlib.sha256(HISTORICAL_V1_XML).hexdigest())
        self.invalid(HISTORICAL_V1_XML)
        root = fixture();root[0][0].set('name', 'ActualCastParameterMetadataRefusalIsObserved')
        self.invalid(serialized(root))
        root = fixture();root[0][0][0][0].set('value', '1');self.invalid(serialized(root))


# Immutable supplied dbce810 CI observation, NOT revision2/native execution proof.
HISTORICAL_V1_XML = b'<?xml version="1.0" encoding="UTF-8"?>\n<testsuites tests="1" failures="0" disabled="0" errors="0" time="0.008" timestamp="2026-10-04T10:28:15.810" name="AllTests">\n  <testsuite name="MySqlDatetimeReceiptObservationIntegrationTest" tests="1" failures="0" disabled="0" skipped="0" errors="0" time="0.008" timestamp="2026-10-04T10:28:15.810">\n    <testcase name="ActualCastParameterMetadataRefusalIsObserved" file="/home/runner/work/odbcpp/odbcpp/tests/integration/it_mysql_datetime_parameter_receipt.cpp" line="206" status="run" result="completed" time="0.008" timestamp="2026-10-04T10:28:15.810" classname="MySqlDatetimeReceiptObservationIntegrationTest">\n      <properties>\n        <property name="observation_schema" value="1"/>\n        <property name="prepare_parameter_count" value="1"/>\n        <property name="prepare_result_count" value="2"/>\n        <property name="observed_parameter_count" value="1"/>\n        <property name="records_truncated" value="0"/>\n        <property name="exchange_complete" value="0"/>\n        <property name="transport_complete_calls" value="1"/>\n        <property name="prepare_command_count" value="1"/>\n        <property name="execute_command_count" value="0"/>\n        <property name="close_command_count" value="0"/>\n        <property name="raw_parameter_0_native_type" value="12"/>\n        <property name="raw_parameter_0_charset" value="45"/>\n        <property name="raw_parameter_0_byte_width" value="104"/>\n        <property name="raw_parameter_0_decimals" value="6"/>\n        <property name="runtime_retired" value="1"/>\n        <property name="physical_close_delta" value="1"/>\n        <property name="refusal_policy_code" value="1"/>\n        <property name="parameter_support_claimed" value="0"/>\n      </properties>\n    </testcase>\n  </testsuite>\n</testsuites>\n'

if __name__ == '__main__':
    unittest.main()
