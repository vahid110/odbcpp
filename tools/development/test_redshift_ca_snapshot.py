"""Synthetic CA custody tests; no operational module imports or resource calls."""
import ast
import copy
import hashlib
import math
import os
from pathlib import Path
import re
import stat
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
RESOURCE_SOURCE = ROOT / 'tools/development/redshift_resource.py'
OPS_SOURCE = ROOT / 'tools/development/redshift_ctest_ops.py'


class Refused(RuntimeError):
    pass


def need(condition, reason):
    if not condition:
        raise Refused(reason)


def selected_class(path, name, methods, namespace):
    """Compile only named lifecycle methods, without module imports/top-level code."""
    tree = ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
    constants = [node for node in tree.body if isinstance(node, ast.Assign) and any(
        isinstance(target, ast.Name) and target.id == 'MAX_CA_BYTES' for target in node.targets)]
    owner = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == name)
    owner.body = [node for node in owner.body if isinstance(node, ast.FunctionDef) and node.name in methods]
    exec(compile(ast.Module(body=constants + [owner], type_ignores=[]), str(path), 'exec'), namespace)
    return namespace[name]


class CaSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='odbcpp-ca-control-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.run = self.root / 'run'
        self.run.mkdir(mode=0o700)
        self.source = self.root / 'source-ca.pem'
        self.body = b'public synthetic admitted CA bytes\n'
        self.source.write_bytes(self.body)
        self.config = dict(admin_password='synthetic', admin_user='odbcpp_pilot_admin',
            ca=str(self.source), database='odbcpp_pilot', host='mock.invalid', port=5439,
            test_password='synthetic', test_user='odbcpp_pilot_test')
        self.snapshot_factory = mock.Mock(return_value=SimpleNamespace(
            config_path='/synthetic/config', credentials_path='/synthetic/credentials',
            passfile_path='/synthetic/admin.pgpass', close=lambda: True))
        self.namespace = dict(os=os, Path=Path, stat=stat, time=time, hashlib=hashlib,
            need=need, Refused=Refused, MAX_CA_BYTES=16 * 1024 * 1024,
            private_json=lambda path: copy.deepcopy(self.config),
            native_auth_inputs=SimpleNamespace(prepare_snapshot=self.snapshot_factory),
            bounded_process=mock.Mock(side_effect=AssertionError('unexpected process')))
        self.native_type = selected_class(RESOURCE_SOURCE, 'NativeOps',
            {'__init__', 'now', 'stamp', 'ordinary_held', 'owned_process', 'prepare',
             'prepare_ca_snapshot', 'ca_held', 'close_ca_snapshot'}, self.namespace)
        self.native = self.native_type({'resource': {'profile': 'synthetic'}, 'database_config': '/synthetic/config',
            'credential_source': '/synthetic/credentials'},
            {'ca_sha256': hashlib.sha256(self.body).hexdigest()}, self.run, time.clock_gettime(time.CLOCK_MONOTONIC) + 60)
        self.addCleanup(self.close_test_fd)
        self.ops_namespace = dict(os=os, Path=Path, math=math, re=re, need=need,
            ADMIN='odbcpp_pilot_admin', ORDINARY='odbcpp_pilot_test', DATABASE='odbcpp_pilot',
            BASE_ENV={}, record=lambda path, value: None, pin=lambda path: {'synthetic': True})
        self.ops_type = selected_class(OPS_SOURCE, 'Operations', {'_sql', 'close_inputs'}, self.ops_namespace)

    def close_test_fd(self):
        # Unknown/corrupt synthetic files are intentionally retained until this
        # test's own TemporaryDirectory cleanup; never unlink them via custody.
        row = getattr(self.native, 'ca_snapshot', None)
        if row is not None:
            try:
                os.close(row['fd'])
            except OSError:
                pass
            self.native.ca_snapshot = None

    def operations(self):
        ops = self.ops_type.__new__(self.ops_type)
        ops.native = self.native
        ops.directory = self.run
        ops.readers_known = True
        ops.reap = lambda end: True
        ops.sql_unknown = ops.sql_fenced = False
        ops.helpers = ops.statements = ops.logins = 0
        ops.now = lambda: time.clock_gettime(time.CLOCK_MONOTONIC)
        ops.preparation_manifest = {'psql': '/synthetic/psql'}
        ops.proofs = {}
        ops.generated = []
        ops.env_directory = ops.profile_inputs = ops.input_residue = None
        return ops

    def dispatch(self, ops):
        return ops._sql({'role': 'test', 'statements': 1, 'stage': 'synthetic', 'query': 'SELECT 1;'},
                        time.clock_gettime(time.CLOCK_MONOTONIC) + 60)

    def install_mock_process(self):
        calls = []
        def process(argv, **kwargs):
            calls.append((argv, kwargs))
            kwargs['output'].write_bytes(b'RSD1|odbcpp_pilot|odbcpp_pilot_test|123\n1\n')
            return SimpleNamespace(returncode=0, timed_out=False)
        self.native.process = process
        return calls

    def test_original_swap_cannot_change_dispatched_ca_bytes(self):
        self.native.prepare()
        self.source.write_bytes(b'unrelated replacement CA\n')
        calls = self.install_mock_process()
        self.dispatch(self.operations())
        self.assertEqual(len(calls), 1)
        consumed = Path(calls[0][1]['env']['PGSSLROOTCERT'])
        self.assertEqual(consumed.read_bytes(), self.body)
        self.assertNotEqual(consumed, self.source)
        self.assertEqual(self.source.read_bytes(), b'unrelated replacement CA\n')

    def test_deliberate_source_symlink_is_snapshot_not_a_live_alias(self):
        alias = self.root / 'ca-alias.pem'
        alias.symlink_to(self.source)
        self.config['ca'] = str(alias)
        self.native.prepare()
        snapshot = Path(self.native.config['ca'])
        self.assertEqual(snapshot.read_bytes(), self.body)
        self.assertFalse(snapshot.is_symlink())
        self.assertNotEqual(snapshot.stat().st_ino, self.source.stat().st_ino)
        alias.unlink()
        alias.symlink_to(self.root / 'absent')
        self.native.ca_held()
        self.assertEqual(snapshot.read_bytes(), self.body)

    def test_wrong_digest_refuses_before_credentials_or_output(self):
        self.native.m['ca_sha256'] = '0' * 64
        with self.assertRaises(Refused):
            self.native.prepare()
        self.snapshot_factory.assert_not_called()
        self.assertEqual(list(self.run.iterdir()), [])

    def test_oversized_source_refuses_before_reading(self):
        with self.source.open('r+b') as stream:
            stream.truncate(self.namespace['MAX_CA_BYTES'] + 1)
        with mock.patch.object(os, 'read', side_effect=AssertionError('oversized read attempted')):
            with self.assertRaisesRegex(Refused, 'CA_regular_bounded'):
                self.native.prepare()
        self.snapshot_factory.assert_not_called()
        self.assertEqual(list(self.run.iterdir()), [])

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'POSIX FIFO fixture unavailable')
    def test_fifo_refuses_without_blocking_or_reading(self):
        self.source.unlink()
        os.mkfifo(self.source)
        with mock.patch.object(os, 'read', side_effect=AssertionError('FIFO read attempted')):
            with self.assertRaisesRegex(Refused, 'CA_regular_bounded'):
                self.native.prepare()
        self.snapshot_factory.assert_not_called()

    def test_short_reads_are_accumulated_before_hash(self):
        original = os.read
        with mock.patch.object(os, 'read', side_effect=lambda fd, size: original(fd, min(size, 3))):
            self.native.prepare()
        self.assertEqual(Path(self.native.config['ca']).read_bytes(), self.body)

    def test_source_mutation_during_read_refuses_before_output(self):
        original = os.read
        changed = False
        def read(fd, size):
            nonlocal changed
            result = original(fd, size)
            if not changed:
                changed = True
                self.source.write_bytes(b'X' * len(self.body))
            return result
        with mock.patch.object(os, 'read', side_effect=read):
            with self.assertRaises(Refused):
                self.native.prepare()
        self.snapshot_factory.assert_not_called()
        self.assertEqual(list(self.run.iterdir()), [])

    def test_destination_collision_preserves_foreign_sentinel(self):
        sentinel = self.root / 'foreign-sentinel'
        sentinel.write_bytes(b'keep sentinel\n')
        destination = self.run / 'ca.pem'
        destination.symlink_to(sentinel)
        with self.assertRaises(FileExistsError):
            self.native.prepare()
        self.assertEqual(sentinel.read_bytes(), b'keep sentinel\n')
        self.assertTrue(destination.is_symlink())
        self.snapshot_factory.assert_not_called()

    def test_partial_write_failure_retires_only_owned_snapshot(self):
        original = os.write
        written = False
        def write(fd, body):
            nonlocal written
            if written:
                raise OSError('synthetic write failure')
            written = True
            return original(fd, body[:3])
        with mock.patch.object(os, 'write', side_effect=write):
            with self.assertRaises(OSError):
                self.native.prepare()
        self.assertEqual(list(self.run.iterdir()), [])
        self.assertIsNone(self.native.ca_snapshot)
        self.assertEqual(self.source.read_bytes(), self.body)
        self.snapshot_factory.assert_not_called()

    def test_changed_snapshot_refuses_dispatch_and_unknown_cleanup(self):
        self.native.prepare()
        snapshot = Path(self.native.config['ca'])
        snapshot.write_bytes(b'unknown snapshot mutation\n')
        calls = self.install_mock_process()
        with self.assertRaises(Refused):
            self.dispatch(self.operations())
        self.assertEqual(calls, [])
        with self.assertRaises(Refused):
            self.native.close_ca_snapshot()
        self.assertEqual(snapshot.read_bytes(), b'unknown snapshot mutation\n')

    def test_known_reader_cleanup_removes_only_owned_files(self):
        self.native.prepare()
        snapshot = Path(self.native.config['ca'])
        sentinel = self.root / 'foreign-sentinel'
        sentinel.write_bytes(b'keep sentinel\n')
        ops = self.operations()
        ops.readers_known = False
        with self.assertRaises(Refused):
            ops.close_inputs(time.clock_gettime(time.CLOCK_MONOTONIC) + 60)
        self.assertTrue(snapshot.exists())
        ops.readers_known = True
        self.assertTrue(ops.close_inputs(time.clock_gettime(time.CLOCK_MONOTONIC) + 60))
        self.assertFalse(snapshot.exists())
        self.assertEqual(list(self.run.iterdir()), [])
        self.assertEqual(self.source.read_bytes(), self.body)
        self.assertEqual(sentinel.read_bytes(), b'keep sentinel\n')
        self.assertIsNone(self.native.ca_snapshot)

    def test_expired_prepare_deadline_refuses_before_outputs(self):
        self.native.child_deadline = 0
        with self.assertRaisesRegex(Refused, 'CA_deadline'):
            self.native.prepare()
        self.snapshot_factory.assert_not_called()
        self.assertEqual(list(self.run.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
