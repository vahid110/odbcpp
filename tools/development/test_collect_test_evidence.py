"""Offline checks of mixed and incomplete native/CTest result reporting."""
import argparse
import json
from pathlib import Path
import tempfile
import unittest

from collect_test_evidence import collect, xml_summary


class EvidenceTests(unittest.TestCase):
    def test_mixed_native_outcomes_and_incomplete_xml(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            xml = root / 'native.xml'
            xml.write_text('''<testsuites><testsuite><testcase name="pass"/>
              <testcase name="fail"><failure/></testcase>
              <testcase name="crash"><error/></testcase>
              <testcase name="skip"><skipped/></testcase>
              <testcase name="disabled" status="notrun" result="suppressed"/>
              </testsuite></testsuites>''')
            self.assertEqual(xml_summary(xml), {'cases': 5, 'pass': 1, 'fail': 2,
                                               'skip': 1, 'disabled': 1})
            image = root / 'image';image.write_bytes(b'public synthetic image')
            inventory = root / 'inventory.json'
            inventory.write_text(json.dumps({'cases': [1, 2, 3]}))
            (root / 'partial.xml').write_text('<testsuites>')
            args = argparse.Namespace(source=Path(__file__).resolve().parents[2],
                build=None, image=[image], ctest_exit=8, ctest_junit=root/'missing.xml',
                inventory=inventory, gtest_dir=root, cleanup_receipt=None)
            result = collect(args)
            self.assertEqual(result['ctest_exit'], 8)
            self.assertIsNone(result['ctest'])
            self.assertEqual(result['inventory']['count'], 3)
            self.assertEqual(len(result['gtest']), 1)
            self.assertEqual(len(result['xml_errors']), 2)
            self.assertFalse(result['acceptance_claimed'])
            self.assertIsNone(result['cleanup_receipt'])
            self.assertEqual(result['images_now'][0]['bytes'], 22)


if __name__ == '__main__':
    unittest.main()
