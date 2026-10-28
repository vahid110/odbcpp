"""Actual temporary-filesystem tests for release metadata path boundaries."""
import os
from pathlib import Path
import tempfile
import unittest
from paths import member, write_generated
from version import generate, metadata


class ReleasePaths(unittest.TestCase):
    def test_operator_can_choose_external_source_and_build_roots(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, output = root / 'source export', root / 'other build'
            source.mkdir()
            (source / 'version.json').write_text('{"schemaVersion":1,"major":1,"minor":1,"patch":0,"prerelease":"beta","sequence":1,"abi":1}')
            generate(metadata(source), output)
            self.assertIn('export.unknown', (output / 'odbcpp-version.json').read_text())
            generate(metadata(source), output)  # ordinary reconfigure may replace our files.

    def test_member_traversal_drive_and_alias_escape(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for bad in ('../victim', '/tmp/victim', 'C:/victim', 'x\\victim', 'x:stream', './victim', 'a//b', 'a/../b', 'trailing.', 'NUL', 'dir/COM1.txt'):
                with self.subTest(bad=bad), self.assertRaises(ValueError):
                    member(root, bad)
            (root / 'escape').symlink_to(root.parent, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, 'escapes'):
                member(root, 'escape/victim')

    def test_generated_file_never_writes_through_symlink_or_hardlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / 'generated'
            output.mkdir()
            victim = root / 'victim'
            victim.write_text('original')
            target = output / 'odbcpp-version.json'
            target.symlink_to(victim)
            with self.assertRaises(ValueError):
                write_generated(target, 'new')
            self.assertEqual(victim.read_text(), 'original')
            target.unlink()
            os.link(victim, target)
            write_generated(target, 'new')
            self.assertEqual(victim.read_text(), 'original')
            self.assertEqual(target.read_text(), 'new')
            self.assertEqual(list(output.glob('.odbcpp-version-*')), [])

    def test_version_manifest_cannot_be_an_outside_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'source'
            source.mkdir()
            victim = root / 'version.json'
            victim.write_text('do not read')
            (source / 'version.json').symlink_to(victim)
            with self.assertRaisesRegex(ValueError, 'escapes'):
                metadata(source)


if __name__ == '__main__':
    unittest.main()
