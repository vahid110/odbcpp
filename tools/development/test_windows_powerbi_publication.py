"""Actual Windows file-sharing regression; no app, driver, registry or network."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import uuid

import windows_powerbi_owner as owner
from windows_powerbi_contract import SID

if os.name == 'nt':
    import windows_live_backend as native


@unittest.skipUnless(os.name == 'nt', 'requires actual Windows sharing semantics')
class RecordPublication(unittest.TestCase):
    def setUp(self):
        native.SID = SID
        native.current_identity()
        self.ctx = owner.Context.__new__(owner.Context)
        self.ctx.native = native
        self.ctx.tree = native.PrivateTree()
        self.ctx.run = Path(tempfile.gettempdir()) / ('odbcpp-publication-' + uuid.uuid4().hex)
        self.ctx.tree.mkdir(self.ctx.run)
        self.name = 'offline.control.json'
        self.final = self.ctx.run / self.name
        self.staging = self.ctx.run / (self.name + '.publishing')

    def tearDown(self):
        try:
            for path in (self.final, self.staging):
                if path.exists():
                    path.unlink()
            self.ctx.tree.rmdir(self.ctx.run)
        finally:
            self.ctx.tree.close_handles()

    def test_reader_sees_only_closed_complete_record(self):
        original_write = native.write_file
        observations = []

        def during_exclusive_write(*args):
            observations.append(self.final.exists())
            self.assertTrue(self.staging.exists())
            self.assertFalse(self.final.exists())
            return original_write(*args)

        with patch.object(native, 'write_file', side_effect=during_exclusive_write):
            digest = self.ctx.record(self.name, {'complete': True})
        self.assertEqual(observations, [False])
        raw = self.ctx.read(self.final, private=True)
        self.assertEqual(json.loads(raw), {'complete': True})
        self.assertEqual(digest, owner.sha(raw))
        self.assertFalse(self.staging.exists())

    def test_duplicate_preserves_first_record_and_failed_publication(self):
        self.ctx.record(self.name, {'attempt': 1})
        with self.assertRaises(FileExistsError):
            self.ctx.record(self.name, {'attempt': 2})
        self.assertEqual(json.loads(self.ctx.read(self.final, private=True)), {'attempt': 1})
        self.assertEqual(json.loads(self.ctx.read(self.staging, private=True)), {'attempt': 2})

    def test_failed_writer_never_publishes_final_name(self):
        with patch.object(native, 'write_file', side_effect=OSError('offline injected write failure')):
            with self.assertRaisesRegex(OSError, 'offline injected write failure'):
                self.ctx.record(self.name, {'complete': False})
        self.assertFalse(self.final.exists())
        self.assertTrue(self.staging.exists())


if __name__ == '__main__':
    unittest.main()
