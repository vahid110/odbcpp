"""Offline XML budgets and consumer integration; no product/native/live execution."""
import ast
import hashlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import odbcpp_xml_evidence as xml


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BoundedXmlTests(unittest.TestCase):
    def test_ordinary_junit_preserves_text_escapes_attributes_and_tails(self):
        raw = (b'<?xml version="1.0"?><testsuites tests="3"><testsuite name="Unicode">'
               b'<testcase name="pass"/><testcase name="fail"><failure message="x &amp; y">'
               b'bad &lt;value&gt;</failure></testcase><testcase name="skip"><skipped/>'
               b'<system-out><![CDATA[ordinary <!DOCTYPE text> & output]]></system-out>'
               b'</testcase></testsuite> tail<!-- <!ENTITY text> --><?note ignored?></testsuites>')
        root = xml.parse_xml_bytes(raw)
        self.assertEqual(root.attrib, {'tests': '3'})
        self.assertEqual([row.get('name') for row in root.iter('testcase')], ['pass', 'fail', 'skip'])
        self.assertEqual(root.find('.//failure').attrib, {'message': 'x & y'})
        self.assertEqual(root.findtext('.//failure'), 'bad <value>')
        self.assertEqual(root.findtext('.//system-out'), 'ordinary <!DOCTYPE text> & output')
        self.assertEqual(root[0].tail, ' tail')
        self.assertEqual(len(root), 1)

    def test_namespace_and_unicode_match_elementtree_conventions(self):
        raw = '<r xmlns="urn:default" xmlns:x="urn:other"><x:c x:a="café">雪&#x1f30d;</x:c></r>'
        root = xml.parse_xml_bytes(raw.encode())
        self.assertEqual(root.tag, '{urn:default}r')
        self.assertEqual(root[0].tag, '{urn:other}c')
        self.assertEqual(root[0].attrib, {'{urn:other}a': 'café'})
        self.assertEqual(root[0].text, '雪🌍')

    def test_supported_encodings_preserve_ordinary_reports(self):
        for encoding in ('utf-8', 'utf-8-sig', 'utf-16', 'utf-16-le', 'utf-16-be', 'iso-8859-1', 'ascii'):
            declaration = 'UTF-16' if encoding.startswith('utf-16') else ('UTF-8' if encoding == 'utf-8-sig' else encoding)
            source = '<?xml version="1.0" encoding="' + declaration + '"?><testsuite tests="1"><testcase name="pass"/></testsuite>'
            with self.subTest(encoding=encoding):
                root = xml.parse_xml_bytes(source.encode(encoding))
                self.assertEqual(root[0].get('name'), 'pass')

    def test_dtd_entities_are_forbidden_encoding_aware_even_unused(self):
        documents = [
            '<!DOCTYPE r><r/>',
            '<!DOCTYPE r [<!ENTITY unused "never used">]><r/>',
            '<!DOCTYPE r [<!ENTITY x "EXPAND">]><r>&x;&x;</r>',
            '<!DOCTYPE r SYSTEM "https://not-contacted.invalid/external.dtd"><r/>',
            '<!DOCTYPE r [<!ENTITY x SYSTEM "file:///not-read">]><r>&x;</r>',
            '<!DOCTYPE r [<!ENTITY % x SYSTEM "https://not-contacted.invalid/parameter">%x;]><r/>',
        ]
        for encoding in ('utf-8', 'utf-8-sig', 'utf-16', 'utf-16-le', 'utf-16-be', 'iso-8859-1'):
            for source in documents:
                with self.subTest(encoding=encoding, source=source):
                    with self.assertRaises(xml.XmlSafetyError):
                        xml.parse_xml_bytes(source.encode(encoding))

    def test_declarations_across_every_small_chunk_boundary_are_rejected(self):
        for size in (1, 2, 3, 7, 13):
            for encoding in ('utf-8', 'utf-16-le', 'utf-16-be'):
                with self.subTest(size=size, encoding=encoding), patch.object(xml, 'CHUNK_BYTES', size):
                    with self.assertRaises(xml.XmlSafetyError):
                        xml.parse_xml_bytes('<!DOCTYPE r [<!ENTITY unused "x">]><r/>'.encode(encoding))

    def test_malformed_or_unsupported_input_is_parse_error_compatible(self):
        for raw in (b'', b'<r>', b'<r></different>', b'<r/><r/>', b'<r>&unknown;</r>',
                    b'<r a="1" a="2"/>', b'<r>\x00</r>', '<r/>'.encode('utf-32')):
            with self.subTest(raw=raw), self.assertRaises(ET.ParseError):
                xml.parse_xml_bytes(raw)
        with self.assertRaises(TypeError):
            xml.parse_xml_bytes('<r/>')

    def test_parse_error_preserves_expat_position_and_code(self):
        with self.assertRaises(ET.ParseError) as failure:
            xml.parse_xml_bytes(b'<r>\n</wrong>')
        self.assertEqual(failure.exception.position[0], 2)
        self.assertIsInstance(failure.exception.code, int)

    def test_bytes_limit_exact_fit_and_one_byte_overflow(self):
        raw = b'<r/>'
        self.assertEqual(xml.parse_xml_bytes(raw, limits=xml.XmlLimits(max_bytes=4)).tag, 'r')
        with self.assertRaises(xml.XmlSafetyError):
            xml.parse_xml_bytes(raw, limits=xml.XmlLimits(max_bytes=3))

    def test_file_reads_only_budget_plus_sentinel_without_read_bytes(self):
        stream = io.BytesIO(b'<r/>' + b' ' * 100000)
        with patch.object(Path, 'open', return_value=stream), \
                patch.object(Path, 'read_bytes', side_effect=AssertionError('whole file allocation')):
            with self.assertRaises(xml.XmlSafetyError):
                xml.parse_xml_file('unused', limits=xml.XmlLimits(max_bytes=100))
            # BytesIO closes at context exit; consumed position is captured below instead.
        self.assertTrue(stream.closed)

    def test_stream_overflow_stops_after_exact_sentinel(self):
        class Observed(io.BytesIO):
            reads = []
            consumed = 0
            def read(self, size=-1):
                self.reads.append(size)
                result = super().read(size)
                self.consumed += len(result)
                return result
        stream = Observed(b'<r/>' + b' ' * 100000)
        with patch.object(Path, 'open', return_value=stream):
            with self.assertRaises(xml.XmlSafetyError):
                xml.parse_xml_file('unused', limits=xml.XmlLimits(max_bytes=100))
        self.assertEqual(stream.consumed, 101)
        self.assertEqual(stream.reads, [101])

    def test_depth_and_nodes_stop_before_extra_element_allocation(self):
        factory = ET.TreeBuilder
        starts = []
        class Observed:
            def __init__(self): self.target = factory()
            def start(self, name, attributes):
                starts.append(name)
                return self.target.start(name, attributes)
            def end(self, name): return self.target.end(name)
            def data(self, data): return self.target.data(data)
            def close(self): return self.target.close()
        for raw, limits in [(b'<r><a><b/></a></r>', xml.XmlLimits(max_depth=2)),
                            (b'<r><a/><b/></r>', xml.XmlLimits(max_nodes=2))]:
            starts.clear()
            with patch.object(xml.ET, 'TreeBuilder', Observed), self.assertRaises(xml.XmlSafetyError):
                xml.parse_xml_bytes(raw, limits=limits)
            self.assertEqual(starts, ['r', 'a'])

    def test_nodes_depth_attributes_and_text_accept_exact_limits(self):
        limits = xml.XmlLimits(max_nodes=2, max_depth=2, max_attributes=1, max_token_bytes=8)
        self.assertEqual(xml.parse_xml_bytes(b'<r><c a="v">12345678</c></r>', limits=limits)[0].text, '12345678')

    def test_attribute_text_cdata_comment_and_pi_token_budgets(self):
        for raw in (b'<r a="123456789"/>', b'<r>123456789</r>',
                    b'<r><![CDATA[123456789]]></r>', b'<r><!--123456789--></r>',
                    b'<r><?p 123456789?></r>', b'<r a="1" b="2"/>'):
            with self.subTest(raw=raw), self.assertRaises(xml.XmlSafetyError):
                xml.parse_xml_bytes(raw, limits=xml.XmlLimits(max_token_bytes=8, max_attributes=1))

    def test_text_budget_cannot_be_bypassed_with_entities_or_cdata_chunks(self):
        for raw in (b'<r>1234<![CDATA[56789]]></r>', b'<r>1234<!-- gap -->56789</r>',
                    b'<r>&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;</r>'):
            with self.subTest(raw=raw), patch.object(xml, 'CHUNK_BYTES', 3), self.assertRaises(xml.XmlSafetyError):
                xml.parse_xml_bytes(raw, limits=xml.XmlLimits(max_token_bytes=8))

    def test_unfinished_large_token_rejected_before_document_end(self):
        for raw in (b'<r a="' + b'x' * 300, b'<r><!--' + b'x' * 300):
            with self.subTest(prefix=raw[:8]), patch.object(xml, 'CHUNK_BYTES', 7):
                with self.assertRaises(xml.XmlSafetyError):
                    xml.parse_xml_bytes(raw, limits=xml.XmlLimits(max_token_bytes=32))

    def test_large_ordinary_log_text_and_cdata_not_confused_with_markup(self):
        text = 'ordinary log line\n' * 20000
        for body in (text, '<![CDATA[' + text + ']]>'):
            for encoding in ('utf-8', 'utf-16'):
                with self.subTest(cdata=body.startswith('<!'), encoding=encoding):
                    root = xml.parse_xml_bytes(('<testsuite><system-out>' + body + '</system-out></testsuite>').encode(encoding))
                    self.assertEqual(root[0].text, text)

    def test_markup_budget_is_separate_from_large_report_text(self):
        for raw in (b'<r a="' + b'x' * 200000 + b'"/>', b'<r><!--' + b'x' * 200000 + b'--></r>'):
            with self.subTest(prefix=raw[:8]), self.assertRaises(xml.XmlSafetyError):
                xml.parse_xml_bytes(raw)

    def test_stricter_consumer_can_forbid_misc_markup_encoding_aware(self):
        for source in ('<r><!-- comment --></r>', '<r><?p value?></r>', '<r><![CDATA[text]]></r>'):
            for encoding in ('utf-8', 'utf-16-le', 'utf-16-be'):
                with self.subTest(source=source, encoding=encoding), self.assertRaises(xml.XmlDeclarationError):
                    xml.parse_xml_bytes(source.encode(encoding), forbid_misc=True)

    def test_limits_are_positive_integer_configuration(self):
        for field in vars(xml.DEFAULT_LIMITS):
            for value in (0, -1, True, 1.5):
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    xml.XmlLimits(**{field: value})

    def test_raw_file_is_unchanged_on_success_and_refusal(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'report.xml'
            for raw in (b'<testsuite tests="0"/>', b'<!DOCTYPE r><r/>', b'<r>'):
                path.write_bytes(raw)
                before = hashlib.sha256(raw).hexdigest()
                try:
                    xml.parse_xml_file(path)
                except ET.ParseError:
                    pass
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), before)


class ConsumerIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pilot = load('xml_test_pilot', 'tools/ci/ubuntu_development_pilot.py')
        cls.windows = load('xml_test_windows', 'tools/ci/windows_product_qualification.py')
        # Its resource module is Unix-only; the shared parser runs on every OS.
        cls.sanitizer = (load('xml_test_sanitizer', 'tools/ci/sanitizer_evidence.py')
                         if sys.platform != 'win32' else None)
        cls.crypto = load('xml_test_crypto', 'tools/ci/record-crypto-qualification.py')
        cls.collector = load('xml_test_collect', 'tools/development/collect_test_evidence.py')
        cls.mysql = load('xml_test_mysql', 'tests/security/test_mysql_auth_live.py')

    def test_actual_consumer_functions_refuse_declarations_before_inventory(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'report.xml'
            path.write_bytes('<!DOCTYPE testsuite [<!ENTITY unused "x">]><testsuite/>'.encode('utf-16'))
            functions = [lambda: self.pilot.xml_result(path, []), lambda: self.windows.xml_result(path, []),
                         lambda: self.crypto.validate_live_report(path), lambda: self.collector.xml_summary(path),
                         lambda: self.mysql.validate_decimal_xml(path), lambda: self.mysql.validate_datetime_xml(path),
                         lambda: self.mysql.validate_date_parameters_xml(path)]
            for function in functions:
                with self.subTest(function=function), self.assertRaises(ET.ParseError):
                    function()

    def test_sanitizer_unsafe_error_compatibility(self):
        if self.sanitizer is None:
            self.skipTest('sanitizer helper is Unix-only')
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'report.xml'
            path.write_bytes('<!DOCTYPE r><r/>'.encode('utf-16'))
            with self.assertRaisesRegex(ValueError, 'unsafe XML'):
                self.sanitizer.xml_root(path)

    def test_existing_case_count_failure_skip_disabled_assertions_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'report.xml'
            good = '<testsuites tests="1" failures="0" errors="0" disabled="0"><testsuite><testcase classname="Suite" name="One" status="run" result="completed"/></testsuite></testsuites>'
            for consumer in (self.pilot, self.windows, self.sanitizer):
                if consumer is None:
                    continue
                path.write_text(good)
                self.assertEqual(consumer.xml_result(path, ['Suite.One'])['count'], 1)
                for bad in (good.replace('tests="1"', 'tests="2"'), good.replace('status="run"', 'status="notrun"'),
                            good.replace('/></testsuite>', '><failure/></testcase></testsuite>'),
                            good.replace('/></testsuite>', '><skipped/></testcase></testsuite>')):
                    path.write_text(bad)
                    with self.subTest(consumer=consumer.__name__, bad=bad), self.assertRaises(ValueError):
                        consumer.xml_result(path, ['Suite.One'])

    def test_collector_binding_is_streamed_and_xml_capped(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'report.xml'
            raw = b'<testsuite/>'
            path.write_bytes(raw)
            with patch.object(Path, 'read_bytes', side_effect=AssertionError('whole file allocation')):
                record = self.collector.binding(path, max_bytes=len(raw))
                self.assertEqual(record['sha256'], hashlib.sha256(raw).hexdigest())
                with self.assertRaises(ET.ParseError):
                    self.collector.binding(path, max_bytes=len(raw) - 1)

    def test_shell_embedded_parsers_without_running_live_script(self):
        source = (ROOT / 'tools/ci/run-crypto-profile-live.sh').read_text()
        blocks = [source.split("<<'" + marker + "'\n", 1)[1].split('\n' + marker, 1)[0]
                  for marker in ('PY', 'PYRESULT')]
        suites = [('CryptoProfileLiveTest', self.crypto.LIVE_TESTS),
                  ('OrdinaryPgMaterialTls', {'OrdinaryPgMaterialTls.' + name for name in
                    ('VerifiedScramMaterialConnectQueryOwnsAfterDisconnect',
                     'NativeRefusalRetiresAndFreshMaterialRecovers',
                     'ProviderCreatedDriverPasswordRouteUsesVerifiedTls')})]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'report.xml'
            for code, (_, names) in zip(blocks, suites):
                root = ET.Element('testsuites')
                suite = ET.SubElement(root, 'testsuite')
                for name in sorted(names):
                    classname, case = name.split('.')
                    ET.SubElement(suite, 'testcase', classname=classname, name=case)
                path.write_bytes(ET.tostring(root))
                with patch.object(sys, 'argv', ['-', str(path), str(ROOT / 'tools')]):
                    exec(compile(code, 'embedded XML parser', 'exec'), {})
                    path.write_bytes(b'<!DOCTYPE r><r/>')
                    with self.assertRaises(ET.ParseError):
                        exec(compile(code, 'embedded XML parser', 'exec'), {})

    def test_all_migrated_file_parse_sites_use_shared_parser(self):
        counts = {'tools/ci/record-crypto-qualification.py': 2, 'tools/ci/ubuntu_development_pilot.py': 2,
                  'tools/ci/windows_product_qualification.py': 2, 'tools/ci/prove-openssl-static-package.py': 1,
                  'tools/ci/sanitizer_evidence.py': 1, 'tools/development/collect_test_evidence.py': 1,
                  'tools/development/test_windows_live_backend.py': 1,
                  'tests/security/test_mysql_auth_live.py': 3, 'tests/security/test_trace_reload_exit_policy.py': 3}
        for relative, count in counts.items():
            source = (ROOT / relative).read_text()
            tree = ast.parse(source)
            calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
                     and isinstance(node.func, ast.Name) and node.func.id == 'parse_xml_file']
            with self.subTest(file=relative):
                self.assertEqual(len(calls), count)
                self.assertNotRegex(source, r'ET\.(parse|fromstring)\(')


if __name__ == '__main__':
    unittest.main()
