"""Real CMake registration, impossible private-input paths, and negative image/inventory cases."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

from live_preflight import preflight


class PreflightTests(unittest.TestCase):
    def test_registration_does_not_read_private_inputs_or_execute_images(self):
        os.umask(0o077)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # Not executable: any attempted case execution would fail.
            image = root / 'not-an-executable';image.write_bytes(b'public synthetic image')
            row = {'id': 'sample', 'image': 'owner', 'profile': 'approved',
                   'gtest_filter': 'Suite.Case', 'timeout_seconds': 40}
            index = {'schema_version': 1, 'images': {'owner': {'executable': str(image),
                'sha256': hashlib.sha256(image.read_bytes()).hexdigest()}},
                'profiles': {'approved': {'environment_file': str(root/'never-read-private-file')}},
                'cases': [row]}
            expected = {'schema_version': 1, 'cases': [{k: v for k, v in row.items()
                                                     if k != 'timeout_seconds'}]}
            idx = root/'index.json';want=root/'expected.json'
            idx.write_text(json.dumps(index));want.write_text(json.dumps(expected))
            result = preflight(idx, want, root/'valid')
            self.assertEqual(result['configure_exit'], 0)
            self.assertFalse(result['private_inputs_read'])
            self.assertFalse(result['test_images_executed'])
            self.assertFalse(result['live_admission_granted'])
            self.assertEqual(result['case_count'], 1)
            self.assertNotIn('never-read-private-file', (root/'valid/public-registration.json').read_text())
            with self.assertRaises(FileExistsError):
                preflight(idx, want, root/'valid')
            index['images']['owner']['sha256'] = '0'*64
            idx.write_text(json.dumps(index))
            self.assertNotEqual(preflight(idx, want, root/'bad-image')['configure_exit'], 0)
            index['images']['owner']['sha256'] = hashlib.sha256(image.read_bytes()).hexdigest()
            idx.write_text(json.dumps(index))
            expected['cases'][0]['id'] = 'wrong'
            want.write_text(json.dumps(expected))
            self.assertNotEqual(preflight(idx, want, root/'bad-inventory')['configure_exit'], 0)


if __name__ == '__main__':
    unittest.main()
