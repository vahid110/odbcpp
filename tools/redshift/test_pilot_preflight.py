"""Offline lock and private-file checks: private temporary files only."""
import importlib.util
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest

MODULE = Path(__file__).with_name('pilot_preflight.py')
spec = importlib.util.spec_from_file_location('pilot_preflight', MODULE)
pilot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pilot)


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name).resolve()
        self.directory.chmod(0o700)
        self.config = self.directory / 'synthetic_config'
        self.config.write_text('not credentials')
        self.config.chmod(0o600)

    def blocked(self, path):
        with self.assertRaises(pilot.Blocked) as caught:
            pilot.validate_private_file(str(path))
        return caught.exception.code

    def test_private_config_permissions_symlink_missing(self):
        pilot.validate_private_file(str(self.config))
        self.config.chmod(0o644)
        self.assertEqual(self.blocked(self.config), 'unsafe_private_file')
        self.config.chmod(0o600)
        target = self.directory/'link'
        target.symlink_to(self.config)
        self.assertEqual(self.blocked(target), 'unsafe_private_file')
        absent = self.directory/'absent-CANARY'
        code = self.blocked(absent)
        self.assertEqual(code, 'private_config_unavailable')
        self.assertNotIn('CANARY', code)

    def test_lock_stable_inode_and_subprocess_contention(self):
        child = """import importlib.util,sys
s=importlib.util.spec_from_file_location('p',sys.argv[1]);p=importlib.util.module_from_spec(s);s.loader.exec_module(p)
try:
 with p.pilot_lock(sys.argv[2]): print('acquired')
except p.Blocked as e: print(e.code)
"""
        def run_child():
            return subprocess.run([sys.executable, '-c', child, str(MODULE), str(self.directory)],
                                   capture_output=True, text=True, timeout=5, check=True).stdout.strip()
        with pilot.pilot_lock(self.directory):
            lock = self.directory/pilot.LOCK_NAME
            inode = lock.stat().st_ino
            self.assertEqual(run_child(), 'lock_contended')
            self.assertEqual(lock.stat().st_mode & 0o777, 0o600)
        self.assertTrue(lock.exists())
        self.assertEqual(run_child(), 'acquired')
        self.assertEqual(lock.stat().st_ino, inode)

    def test_unsafe_lock_file_directory_symlinks_fifo_and_hardlinks(self):
        lock = self.directory/pilot.LOCK_NAME
        lock.write_text('CANARY-do-not-delete')
        lock.chmod(0o644)
        with self.assertRaises(pilot.Blocked):
            with pilot.pilot_lock(self.directory): pass
        self.assertEqual(lock.read_text(), 'CANARY-do-not-delete')
        lock.chmod(0o600)
        self.directory.chmod(0o755)
        with self.assertRaises(pilot.Blocked):
            with pilot.pilot_lock(self.directory): pass
        self.directory.chmod(0o700)
        link = self.directory/'directory-link'
        link.symlink_to(self.directory, target_is_directory=True)
        with self.assertRaises(pilot.Blocked):
            with pilot.pilot_lock(link): pass
        lock.unlink()
        lock.symlink_to(self.config)
        with self.assertRaises(pilot.Blocked):
            with pilot.pilot_lock(self.directory): pass
        lock.unlink()
        os.mkfifo(lock, 0o600)
        with self.assertRaises(pilot.Blocked):
            with pilot.pilot_lock(self.directory): pass
        lock.unlink()
        os.link(self.config, lock)
        with self.assertRaises(pilot.Blocked):
            with pilot.pilot_lock(self.directory): pass

    def test_borrowed_lock_directory_fd_lifetime(self):
        with pilot.pilot_lock(self.directory) as fd:
            self.assertIsInstance(fd, int)
            self.assertEqual(os.fstat(fd).st_ino, self.directory.stat().st_ino)
            pilot.assert_locked_directory(self.directory, fd)
        with self.assertRaises(OSError):
            os.fstat(fd)
        with self.assertRaises(pilot.Blocked):
            pilot.assert_locked_directory(self.directory, fd)

    def test_directory_replacement_and_symlink_block_lock_exit(self):
        for symlink in (False, True):
            moved = self.directory.with_name(self.directory.name+'-moved')
            try:
                with self.assertRaises(pilot.Blocked):
                    with pilot.pilot_lock(self.directory) as fd:
                        self.directory.rename(moved)
                        if symlink:
                            self.directory.symlink_to(moved, target_is_directory=True)
                        else:
                            self.directory.mkdir(mode=0o700)
                        with self.assertRaises(pilot.Blocked):
                            pilot.assert_locked_directory(self.directory, fd)
            finally:
                if self.directory.is_symlink(): self.directory.unlink()
                elif self.directory.exists(): shutil.rmtree(self.directory)
                moved.rename(self.directory)

    def test_directory_replaced_during_open_is_not_yielded(self):
        from unittest.mock import patch
        moved = self.directory.with_name(self.directory.name+'-moved')
        original_open = os.open
        def race_open(path, flags, *args, **kwargs):
            fd = original_open(path, flags, *args, **kwargs)
            if Path(path) == self.directory and flags & os.O_DIRECTORY:
                self.directory.rename(moved)
                self.directory.mkdir(mode=0o700)
            return fd
        try:
            with patch.object(pilot.os, 'open', side_effect=race_open), self.assertRaises(pilot.Blocked):
                with pilot.pilot_lock(self.directory): self.fail('divergent descriptor yielded')
        finally:
            shutil.rmtree(self.directory)
            moved.rename(self.directory)


if __name__ == '__main__':
    unittest.main()
